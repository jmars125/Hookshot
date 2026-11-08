//! The guest image the lifter reads: an exefs's NSOs parsed (a64-kit tools/nso.py with nx2/nso2.py's DT_RELR), laid
//! out at Eden's addresses (nx2/image.py: rtld, main, subsdk*, sdk back to back from 0x80000000 + the ASLR offset),
//! relocated and bound as rtld would (hos/nx_image.py), with its function entry candidates. For the standalone build,
//! `hle_set` (nx2/standalone.py) names the SDK functions hooked to the native runtime, and `adr_entries`
//! (nx2/adr_entries.py) adds the code addresses the code builds with ADR / ADRP+ADD.

use anyhow::{bail, ensure, Context, Result};
use std::collections::{BTreeMap, BTreeSet, HashMap, HashSet};
use std::path::Path;

pub const MODULE_ORDER: [&str; 13] = crate::nso::NAMES;
pub const CODE_39BIT: u64 = 0x8000_0000;
pub const TRAMP_BASE: u64 = 0x0700_0000;
const PAGE: u64 = 0x1000;

const DT_PLTRELSZ: u64 = 2;
const DT_STRTAB: u64 = 5;
const DT_SYMTAB: u64 = 6;
const DT_RELA: u64 = 7;
const DT_RELASZ: u64 = 8;
const DT_SYMENT: u64 = 11;
const DT_INIT: u64 = 12;
const DT_FINI: u64 = 13;
const DT_JMPREL: u64 = 23;
const DT_INIT_ARRAY: u64 = 25;
const DT_INIT_ARRAYSZ: u64 = 27;
const DT_RELRSZ: u64 = 35;
const DT_RELR: u64 = 36;
const DT_RELRENT: u64 = 37;

pub const R_ABS64: u32 = 257;
pub const R_GLOB_DAT: u32 = 1025;
pub const R_JUMP_SLOT: u32 = 1026;
pub const R_RELATIVE: u32 = 1027;

pub const STT_FUNC: u8 = 2;
pub const STB_WEAK: u8 = 2;

#[derive(Debug, Clone)]
pub struct Sym {
    pub name: String,
    pub ty: u8,
    pub bind: u8,
    pub defined: bool,
    pub value: u64,
}

#[derive(Debug, Clone)]
pub struct Reloc {
    pub offset: u64,
    pub ty: u32,
    /// The symbol's name; None for symbol index 0
    pub sym: Option<String>,
    pub addend: i64,
}

pub struct Module {
    pub name: String,
    pub image: Vec<u8>,
    /// (memory offset, size) of .text, .rodata, .data
    pub segments: [(u64, u64); 3],
    pub bss_size: u64,
    pub eh_frame_hdr: (u64, u64),
    pub dynamic: HashMap<u64, Vec<u64>>,
    pub symbols: Vec<Sym>,
    pub relocs: Vec<Reloc>,
    pub eh_funcs: Vec<u64>,
    pub base: u64,
}

fn u32_at(b: &[u8], o: usize) -> u32 {
    u32::from_le_bytes(b[o..o + 4].try_into().unwrap())
}
fn u64_at(b: &[u8], o: usize) -> u64 {
    u64::from_le_bytes(b[o..o + 8].try_into().unwrap())
}
fn i32_at(b: &[u8], o: usize) -> i32 {
    u32_at(b, o) as i32
}

/// An LZ4 block (no frame), decompressed to exactly `size` bytes
fn lz4_block(src: &[u8], size: usize) -> Result<Vec<u8>> {
    let mut out = Vec::with_capacity(size);
    let mut i = 0;
    let len = |i: &mut usize, mut n: usize| -> Result<usize> {
        if n == 15 {
            loop {
                let b = *src.get(*i).context("lz4: truncated length")?;
                *i += 1;
                n += b as usize;
                if b != 255 {
                    break;
                }
            }
        }
        Ok(n)
    };
    while i < src.len() {
        let tok = src[i];
        i += 1;
        let lit = len(&mut i, (tok >> 4) as usize)?;
        out.extend_from_slice(src.get(i..i + lit).context("lz4: truncated literals")?);
        i += lit;
        if i >= src.len() {
            break;
        }
        let off = u16::from_le_bytes([src[i], src[i + 1]]) as usize;
        i += 2;
        let ml = len(&mut i, (tok & 15) as usize)? + 4;
        ensure!(off != 0 && off <= out.len(), "lz4: bad offset");
        let start = out.len() - off;
        for k in 0..ml {
            out.push(out[start + k]);
        }
    }
    ensure!(out.len() == size, "lz4: {} bytes, want {size}", out.len());
    Ok(out)
}

fn cstr(b: &[u8], o: usize) -> String {
    let end = b[o..]
        .iter()
        .position(|&c| c == 0)
        .map_or(b.len(), |p| o + p);
    String::from_utf8_lossy(&b[o..end]).into_owned()
}

impl Module {
    pub fn load(path: &Path) -> Result<Module> {
        let raw = std::fs::read(path).with_context(|| format!("{}", path.display()))?;
        ensure!(
            raw.len() >= 0x100 && &raw[..4] == b"NSO0",
            "{}: not an NSO",
            path.display()
        );
        let name = path.file_name().unwrap().to_string_lossy().into_owned();
        let flags = u32_at(&raw, 0xc);
        ensure!(
            flags & 0x80 == 0,
            "{name}: ZBIC-compressed; run `hsr unpack` on the exefs first"
        );
        let mut segments = [(0u64, 0u64); 3];
        let mut datas = Vec::new();
        let mut end = 0;
        for i in 0..3 {
            let (foff, moff, size) = (
                u32_at(&raw, 0x10 + 0x10 * i) as usize,
                u32_at(&raw, 0x14 + 0x10 * i) as u64,
                u32_at(&raw, 0x18 + 0x10 * i) as usize,
            );
            let cs = u32_at(&raw, 0x60 + 4 * i) as usize;
            let src = raw
                .get(foff..foff + cs)
                .context("segment outside the file")?;
            let data = if flags & (1 << i) != 0 {
                lz4_block(src, size)?
            } else {
                src.to_vec()
            };
            ensure!(
                data.len() == size,
                "{name} segment {i}: {} != {size}",
                data.len()
            );
            segments[i] = (moff, size as u64);
            end = end.max(moff as usize + size);
            datas.push(data);
        }
        let mut image = vec![0u8; end];
        for i in 0..3 {
            let o = segments[i].0 as usize;
            image[o..o + datas[i].len()].copy_from_slice(&datas[i]);
        }
        let mut m = Module {
            name,
            image,
            segments,
            bss_size: u32_at(&raw, 0x3c) as u64,
            eh_frame_hdr: (0, 0),
            dynamic: HashMap::new(),
            symbols: Vec::new(),
            relocs: Vec::new(),
            eh_funcs: Vec::new(),
            base: 0,
        };
        m.mod0()?;
        Ok(m)
    }

    fn d(&self, tag: u64) -> u64 {
        self.dynamic.get(&tag).map_or(0, |v| v[0])
    }

    fn mod0(&mut self) -> Result<()> {
        let m = u32_at(&self.image, 4) as usize;
        if self.image.get(m..m + 4) != Some(b"MOD0") {
            bail!("{}: no MOD0", self.name);
        }
        let f = |k: usize| (m as i64 + i32_at(&self.image, m + 4 + 4 * k) as i64) as u64;
        let (dyn_, bss_e, eh_s, eh_e) = (f(0), f(2), f(3), f(4));
        self.eh_frame_hdr = (eh_s, eh_e);
        if (self.image.len() as u64) < bss_e {
            self.image.resize(bss_e as usize, 0);
        }
        let mut off = dyn_ as usize;
        loop {
            let (tag, val) = (
                u64_at(&self.image, off) as i64,
                u64_at(&self.image, off + 8),
            );
            off += 16;
            if tag == 0 {
                break;
            }
            self.dynamic.entry(tag as u64).or_default().push(val);
        }
        let (strtab, symtab) = (self.d(DT_STRTAB), self.d(DT_SYMTAB));
        let syment = match self.d(DT_SYMENT) {
            0 => 24,
            v => v,
        };
        let nsyms = if strtab > symtab {
            (strtab - symtab) / syment
        } else {
            0
        };
        for i in 0..nsyms {
            let o = (symtab + i * syment) as usize;
            let img = &self.image;
            let info = img[o + 4];
            self.symbols.push(Sym {
                name: cstr(img, (strtab + u32_at(img, o) as u64) as usize),
                ty: info & 0xf,
                bind: info >> 4,
                defined: u16::from_le_bytes([img[o + 6], img[o + 7]]) != 0,
                value: u64_at(img, o + 8),
            });
        }
        for (sk, zk) in [(DT_RELA, DT_RELASZ), (DT_JMPREL, DT_PLTRELSZ)] {
            let (start, size) = (self.d(sk), self.d(zk));
            let mut o = start;
            while o < start + size {
                let (r_off, info, addend) = (
                    u64_at(&self.image, o as usize),
                    u64_at(&self.image, o as usize + 8),
                    u64_at(&self.image, o as usize + 16) as i64,
                );
                let sym = (info >> 32) as usize;
                self.relocs.push(Reloc {
                    offset: r_off,
                    ty: info as u32,
                    sym: (sym != 0).then(|| self.symbols[sym].name.clone()),
                    addend,
                });
                o += 24;
            }
        }
        // DT_RELR (nx2/nso2.py): even entries are addresses, odd ones bitmaps of the next 63 words
        let (start, size) = (self.d(DT_RELR), self.d(DT_RELRSZ));
        if start != 0 && size != 0 {
            ensure!(
                matches!(self.d(DT_RELRENT), 0 | 8),
                "{}: RELRENT",
                self.name
            );
            let mut offs = Vec::new();
            let mut at = 0u64;
            for k in 0..size / 8 {
                let e = u64_at(&self.image, (start + 8 * k) as usize);
                if e & 1 == 0 {
                    offs.push(e);
                    at = e + 8;
                } else {
                    let mut bits = e >> 1;
                    let mut i = 0;
                    while bits != 0 {
                        if bits & 1 != 0 {
                            offs.push(at + 8 * i);
                        }
                        bits >>= 1;
                        i += 1;
                    }
                    at += 63 * 8;
                }
            }
            for off in offs {
                let addend = u64_at(&self.image, off as usize) as i64;
                self.relocs.push(Reloc {
                    offset: off,
                    ty: R_RELATIVE,
                    sym: None,
                    addend,
                });
            }
        }
        self.eh_funcs = self.eh_table();
        Ok(())
    }

    /// Function starts from .eh_frame_hdr's binary-search table (datarel sdata4)
    fn eh_table(&self) -> Vec<u64> {
        let (s, e) = self.eh_frame_hdr;
        if e <= s {
            return Vec::new();
        }
        let s = s as usize;
        let h = &self.image[s..s + 4];
        if h[0] != 1 || h[3] != 0x3b || h[2] != 0x03 || !matches!(h[1] & 0xf, 0x0b | 0x03) {
            return Vec::new();
        }
        let count = u32_at(&self.image, s + 8) as usize;
        (0..count)
            .map(|i| (s as i64 + i32_at(&self.image, s + 12 + 8 * i) as i64) as u64)
            .collect()
    }

    pub fn code_range(&self) -> (u64, u64) {
        let (o, n) = self.segments[0];
        (o, o + n)
    }

    /// (start, end) of every function an FDE of .eh_frame_hdr's table covers, absolute (adr_entries.py fde_spans)
    fn fde_spans(&self, out: &mut Vec<(u64, u64)>) {
        let (s, e) = self.eh_frame_hdr;
        if e <= s {
            return;
        }
        let (s, img) = (s as usize, &self.image);
        if img[s] != 1 || img[s + 3] != 0x3b || img[s + 2] != 0x03 {
            return;
        }
        for i in 0..u32_at(img, s + 8) as usize {
            let loc = s as i64 + i32_at(img, s + 12 + 8 * i) as i64;
            let fde = s as i64 + i32_at(img, s + 16 + 8 * i) as i64;
            if fde < 0 || fde as usize + 16 > img.len() {
                continue;
            }
            let f = fde as usize;
            let (length, cie, begin, rng) = (
                u32_at(img, f),
                u32_at(img, f + 4),
                i32_at(img, f + 8),
                u32_at(img, f + 12),
            );
            if length < 12 || cie == 0 || fde + 8 + begin as i64 != loc {
                continue;
            }
            out.push((self.base + loc as u64, self.base + loc as u64 + rng as u64));
        }
    }
}

pub fn load_exefs(dir: &Path) -> Result<Vec<Module>> {
    MODULE_ORDER
        .iter()
        .map(|n| dir.join(n))
        .filter(|p| p.exists())
        .map(|p| Module::load(&p))
        .collect()
}

pub fn c_ident(sym: &str) -> String {
    let mut s = String::from("imp_");
    s.extend(sym.chars().map(|c| {
        if c.is_alphanumeric() || c == '_' {
            c
        } else {
            '_'
        }
    }));
    s
}

/// The relocated, bound image at Eden's layout
pub struct Image {
    pub modules: Vec<Module>,
    pub base: u64,
    pub end: u64,
    pub mem: Vec<u8>,
    /// guest address -> C name of the hook
    pub hooks: BTreeMap<u64, String>,
    pub hook_syms: HashMap<String, String>,
    pub tramp: HashMap<Option<String>, u64>,
    pub unresolved: HashMap<Option<String>, u64>,
    /// (lo, hi, module index) of every module's .text
    pub code: Vec<(u64, u64, usize)>,
    pub entries: HashSet<u64>,
    pub eh_starts: Vec<u64>,
    /// guest address -> first function symbol defined there
    pub names: HashMap<u64, String>,
}

fn page_align(v: u64) -> u64 {
    (v + PAGE - 1) & !(PAGE - 1)
}

impl Image {
    /// nx2/image.py EdenImage (program_args off)
    pub fn eden(mut modules: Vec<Module>, aslr: u64, hle: &BTreeSet<String>) -> Result<Image> {
        let start = CODE_39BIT + aslr;
        let mut base = start;
        for m in &mut modules {
            m.base = base;
            let (doff, dsize) = m.segments[2];
            let size = page_align(doff + dsize + m.bss_size);
            if (m.image.len() as u64) < size {
                m.image.resize(size as usize, 0);
            }
            base += size;
        }
        let mut mem = vec![0u8; (base - start) as usize];
        for m in &modules {
            let o = (m.base - start) as usize;
            mem[o..o + m.image.len()].copy_from_slice(&m.image);
        }
        let mut img = Image {
            modules,
            base: start,
            end: base,
            mem,
            hooks: BTreeMap::new(),
            hook_syms: HashMap::new(),
            tramp: HashMap::new(),
            unresolved: HashMap::new(),
            code: Vec::new(),
            entries: HashSet::new(),
            eh_starts: Vec::new(),
            names: HashMap::new(),
        };
        let mut exports: HashMap<&str, u64> = HashMap::new();
        for m in &img.modules {
            for s in &m.symbols {
                if s.defined && !s.name.is_empty() {
                    exports.entry(&s.name).or_insert(m.base + s.value);
                }
            }
        }
        let exports: HashMap<String, u64> = exports
            .into_iter()
            .map(|(k, v)| (k.to_string(), v))
            .collect();
        for sym in hle {
            if let Some(&a) = exports.get(sym) {
                let name = c_ident(sym);
                img.hooks.insert(a, name.clone());
                img.hook_syms.insert(name, sym.clone());
            }
        }
        img.relocate(&exports, hle)?;
        img.code = img
            .modules
            .iter()
            .enumerate()
            .map(|(i, m)| {
                let (lo, hi) = m.code_range();
                (m.base + lo, m.base + hi, i)
            })
            .collect();
        img.find_entries();
        for m in &img.modules {
            for s in &m.symbols {
                if s.defined && s.ty == STT_FUNC && !s.name.is_empty() {
                    img.names
                        .entry(m.base + s.value)
                        .or_insert_with(|| s.name.clone());
                }
            }
        }
        Ok(img)
    }

    fn trampoline(&mut self, sym: &Option<String>) -> u64 {
        if let Some(&a) = self.tramp.get(sym) {
            return a;
        }
        let a = TRAMP_BASE + 16 * self.tramp.len() as u64;
        self.tramp.insert(sym.clone(), a);
        let s = sym.clone().unwrap_or_default();
        let name = c_ident(&s);
        self.hooks.insert(a, name.clone());
        self.hook_syms.insert(name, s);
        a
    }

    fn relocate(&mut self, exports: &HashMap<String, u64>, hle: &BTreeSet<String>) -> Result<()> {
        for mi in 0..self.modules.len() {
            let bind: HashMap<&str, u8> = self.modules[mi]
                .symbols
                .iter()
                .map(|s| (s.name.as_str(), s.bind))
                .collect();
            let weak: Vec<bool> = self.modules[mi]
                .relocs
                .iter()
                .map(|r| r.sym.as_deref().and_then(|s| bind.get(s)) == Some(&STB_WEAK))
                .collect();
            let relocs = std::mem::take(&mut self.modules[mi].relocs);
            let mbase = self.modules[mi].base;
            for (r, weak) in relocs.iter().zip(weak) {
                let at = (mbase + r.offset - self.base) as usize;
                let v = match r.ty {
                    R_RELATIVE => mbase.wrapping_add(r.addend as u64),
                    R_ABS64 | R_GLOB_DAT | R_JUMP_SLOT => {
                        let hit = r.sym.as_ref().and_then(|s| exports.get(s)).copied();
                        let in_hle = r.sym.as_ref().is_some_and(|s| hle.contains(s));
                        let t = match hit {
                            Some(a) => a,
                            None if weak && !in_hle => 0,
                            None => {
                                if !in_hle {
                                    *self.unresolved.entry(r.sym.clone()).or_default() += 1;
                                }
                                self.trampoline(&r.sym)
                            }
                        };
                        t.wrapping_add(if r.ty == R_ABS64 { r.addend as u64 } else { 0 })
                    }
                    t => bail!(
                        "{}: relocation type {t} at {:#x} not supported",
                        self.modules[mi].name,
                        r.offset
                    ),
                };
                self.mem[at..at + 8].copy_from_slice(&v.to_le_bytes());
            }
            self.modules[mi].relocs = relocs;
        }
        Ok(())
    }

    pub fn in_code(&self, a: u64) -> bool {
        a & 3 == 0 && self.code.iter().any(|&(lo, hi, _)| lo <= a && a < hi)
    }

    pub fn module_of(&self, a: u64) -> &str {
        self.code
            .iter()
            .find(|&&(lo, hi, _)| lo <= a && a < hi)
            .map_or("?", |&(_, _, i)| &self.modules[i].name)
    }

    /// The word at a guest address of the image (0 outside it)
    pub fn word(&self, a: u64) -> u32 {
        match a.checked_sub(self.base) {
            Some(o) if a + 4 <= self.end => u32_at(&self.mem, o as usize),
            _ => 0,
        }
    }

    /// n bytes at a, None unless inside [base, end)
    pub fn read(&self, a: u64, n: u64) -> Option<&[u8]> {
        (a >= self.base && a + n <= self.end)
            .then(|| &self.mem[(a - self.base) as usize..(a - self.base + n) as usize])
    }

    fn find_entries(&mut self) {
        let mut e = HashSet::new();
        for m in &self.modules {
            let (lo, hi) = m.code_range();
            e.insert(m.base + lo);
            for k in [DT_INIT, DT_FINI] {
                if let Some(v) = m.dynamic.get(&k) {
                    e.insert(m.base + v[0]);
                }
            }
            e.extend(m.eh_funcs.iter().map(|&f| m.base + f));
            e.extend(
                m.symbols
                    .iter()
                    .filter(|s| s.defined && s.ty == STT_FUNC && s.value != 0)
                    .map(|s| m.base + s.value),
            );
            for r in &m.relocs {
                if r.ty == R_RELATIVE || r.ty == R_ABS64 {
                    let v = u64_at(&self.mem, (m.base + r.offset - self.base) as usize);
                    if self.in_code(v) {
                        e.insert(v);
                    }
                }
            }
            for pc in (m.base + lo..m.base + hi).step_by(4) {
                let w = self.word(pc);
                if w & 0xFC00_0000 == 0x9400_0000 {
                    let off = ((w << 6) as i32 >> 6) as i64;
                    e.insert(pc.wrapping_add((4 * off) as u64));
                }
            }
            let (start, size) = (m.d(DT_INIT_ARRAY), m.d(DT_INIT_ARRAYSZ));
            for i in 0..size / 8 {
                let v = u64_at(&self.mem, (m.base + start + 8 * i - self.base) as usize);
                if self.in_code(v) {
                    e.insert(v);
                }
            }
        }
        self.entries = e.into_iter().filter(|&a| self.in_code(a)).collect();
        let mut eh: Vec<u64> = self
            .modules
            .iter()
            .flat_map(|m| m.eh_funcs.iter().map(move |&f| m.base + f))
            .collect();
        eh.sort_unstable();
        self.eh_starts = eh;
    }

    /// The code addresses ADR / ADRP+ADD (up to 4 words later, same register) form in .text, not strictly inside a
    /// function the unwind info covers (nx2/adr_entries.py)
    pub fn adr_entries(&self) -> BTreeSet<u64> {
        let mut spans = Vec::new();
        for m in &self.modules {
            m.fde_spans(&mut spans);
        }
        spans.sort_unstable();
        let texts: Vec<(u64, u64)> = self.code.iter().map(|&(lo, hi, _)| (lo, hi)).collect();
        let mut t = BTreeSet::new();
        let imm = |w: u32| -> i64 {
            let v = (((w >> 5) & 0x7FFFF) << 2 | (w >> 29) & 3) as i64;
            if v & (1 << 20) != 0 {
                v - (1 << 21)
            } else {
                v
            }
        };
        for &(lo, hi) in &texts {
            let n = ((hi - lo) / 4) as usize;
            for i in 0..n {
                let pc = lo + 4 * i as u64;
                let w = self.word(pc);
                if w & 0x9F00_0000 == 0x1000_0000 {
                    t.insert(pc.wrapping_add(imm(w) as u64));
                } else if w & 0x9F00_0000 == 0x9000_0000 {
                    let page = (pc & !0xFFF).wrapping_add((imm(w) << 12) as u64);
                    for d in 1..=4 {
                        if i + d >= n {
                            break;
                        }
                        let y = self.word(pc + 4 * d as u64);
                        if y & 0xFFC0_0000 == 0x9100_0000 && (y >> 5) & 31 == w & 31 {
                            t.insert(page + ((y >> 10) & 0xFFF) as u64);
                        }
                    }
                }
            }
        }
        let inside = |a: u64| texts.iter().any(|&(lo, hi)| lo <= a && a < hi);
        let starts: HashSet<u64> = spans.iter().map(|s| s.0).collect();
        let mut best = Vec::with_capacity(spans.len());
        let mut mx = 0u64;
        for &(_, h) in &spans {
            mx = mx.max(h);
            best.push(mx);
        }
        t.into_iter()
            .filter(|&a| inside(a) && a % 4 == 0)
            .filter(|&a| {
                let k = spans.partition_point(|s| s.0 <= a);
                k == 0 || !(a < best[k - 1] && !starts.contains(&a))
            })
            .collect()
    }
}

/// C names of the runtime's HLE functions in host/*.c: `void imp_x(Ctx` definitions and `HLE(x)` macros
pub fn implemented(host: &Path) -> Result<BTreeSet<String>> {
    let mut names = BTreeSet::new();
    for e in std::fs::read_dir(host)? {
        let p = e?.path();
        if p.extension().is_some_and(|x| x == "c") {
            let src = String::from_utf8_lossy(&std::fs::read(&p)?).into_owned();
            for line in src.lines() {
                let ident = |s: &str| {
                    s.chars()
                        .take_while(|c| c.is_alphanumeric() || *c == '_')
                        .collect::<String>()
                };
                if let Some(r) = line.strip_prefix("void imp_") {
                    let id = ident(r);
                    if r[id.len()..].starts_with("(Ctx") {
                        names.insert(format!("imp_{id}"));
                    }
                } else if let Some(r) = line.strip_prefix("HLE(") {
                    let id = ident(r);
                    if !id.is_empty() && r[id.len()..].starts_with(')') {
                        names.insert(format!("imp_{id}"));
                    }
                }
            }
        }
    }
    Ok(names)
}

const OS_NAMESPACES: [&str; 42] = [
    "audio", "hid", "oe", "prepo", "nfp", "time", "account", "vi", "swkbd", "pl", "err", "codec",
    "diag", "detail", "settings", "ns", "applet", "album", "friends", "nifm", "ssl", "socket",
    "ec", "bcat", "web", "ldn", "irsensor", "aoc", "olsc", "pctl", "apm", "ro", "chat", "camera",
    "gs", "movie", "npns", "news", "fatal", "lbl", "capture", "syncpt",
];

/// The components of an Itanium nested name (_ZN[K]<len><id>...E), None for any other shape
fn nested_name(sym: &str) -> Option<Vec<&str>> {
    let b = sym.as_bytes();
    let mut i = if sym.starts_with("_ZNK") {
        4
    } else if sym.starts_with("_ZN") {
        3
    } else {
        return None;
    };
    let mut parts = Vec::new();
    while i < b.len() && b[i].is_ascii_digit() {
        let mut j = i;
        while j < b.len() && b[j].is_ascii_digit() {
            j += 1;
        }
        let n: usize = sym[i..j].parse().ok()?;
        let e = (j + n).min(b.len());
        parts.push(&sym[j..e]);
        i = j + n;
    }
    (i < b.len() && b[i] == b'E').then_some(parts)
}

fn os_facing(sym: &str) -> bool {
    let Some(p) = nested_name(sym) else {
        return false;
    };
    p.len() >= 2
        && p[0] == "nn"
        && OS_NAMESPACES.contains(&p[1])
        && (p.len() == 3 || (p.len() == 4 && p[2] == "sdk"))
}

/// The NVN proc a symbol is: nvnFoo, or the C++ _Z<len>nvnFoo<args>
pub fn nvn_proc_name(sym: &str) -> Option<&str> {
    if sym.starts_with("nvn") {
        return Some(sym);
    }
    let r = sym.strip_prefix("_Z")?;
    let d = r.bytes().take_while(u8::is_ascii_digit).count();
    let n: usize = r[..d].parse().ok()?;
    let rest = &r[d..];
    let w = rest
        .bytes()
        .take_while(|c| c.is_ascii_alphanumeric() || *c == b'_')
        .count();
    if !rest.starts_with("nvn") || w < 4 {
        return None;
    }
    Some(&rest[..n.min(w)])
}

/// The standalone build's hooked symbols (nx2/standalone.py hle_set): the runtime's imp_ functions some module has,
/// the sdk's NVN functions, and the OS-facing functions the game modules import without defining
pub fn hle_set(modules: &[Module], host: &Path) -> Result<BTreeSet<String>> {
    let imp = implemented(host)?;
    let known: HashSet<&str> = modules
        .iter()
        .flat_map(|m| m.symbols.iter().map(|s| s.name.as_str()))
        .collect();
    let hle: BTreeSet<String> = imp
        .iter()
        .map(|n| n[4..].to_string())
        .filter(|n| known.contains(n.as_str()))
        .collect();
    let mut kind: HashMap<&str, u8> = HashMap::new();
    for m in modules {
        for s in &m.symbols {
            if s.defined && !s.name.is_empty() {
                kind.insert(&s.name, s.ty);
            }
        }
    }
    let mut out = hle.clone();
    for m in modules {
        if m.name != "sdk" && m.name != "rtld" {
            for s in &m.symbols {
                if !s.defined
                    && !s.name.is_empty()
                    && os_facing(&s.name)
                    && kind.get(s.name.as_str()).copied().unwrap_or(STT_FUNC) == STT_FUNC
                {
                    out.insert(s.name.clone());
                }
            }
        }
    }
    let have: HashSet<String> = imp.iter().map(|n| n[4..].to_string()).collect();
    if let Some(sdk) = modules.iter().find(|m| m.name == "sdk") {
        for s in &sdk.symbols {
            if s.defined
                && !s.name.is_empty()
                && nvn_proc_name(&s.name).is_some()
                && s.ty == STT_FUNC
                && !have.contains(&s.name)
            {
                out.insert(s.name.clone());
            }
        }
    }
    Ok(out)
}
