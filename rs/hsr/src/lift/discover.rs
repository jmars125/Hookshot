//! Function discovery (a64-kit lift/a64_lift.py Lifter.discover, with nx2/lift_fixes.py's jump-table fixes): every
//! entry's instructions, the targets of the BRs that are jump tables, PLT stubs and the functions that call __setjmp.
//!
//! The jump-table search is a port, not a redesign: the same symbolic window before each BR, the same fallbacks
//! (constants reaching the BR through the CFG, the hoisted ADRP scan, the function's one constant, ADR candidates),
//! run in the same order, so the result matches the Python lifter's functions.json.

use super::func::{classify, Body, Kind};
use crate::image::Image;
use std::collections::{BTreeMap, BTreeSet, HashMap, HashSet};

fn sx(v: u64, n: u32) -> i64 {
    ((v << (64 - n)) as i64) >> (64 - n)
}

fn adr_imm(w: u32) -> i64 {
    sx((((w >> 3) & 0x1F_FFFC) | ((w >> 29) & 3)) as u64, 21)
}

fn add_imm(w: u32) -> u64 {
    (((w >> 10) & 0xFFF) as u64) << if w & (1 << 22) != 0 { 12 } else { 0 }
}

/// Bits 0-4 are not a destination: stores, compares, branches
fn reads_rt_only(w: u32) -> bool {
    (w & 0x3A00_0000 == 0x3800_0000 && (w >> 22) & 1 == 0)
        || w & 0x1C00_0000 == 0x1400_0000
        || matches!(w & 0x7E00_0000, 0x3400_0000 | 0x3600_0000)
        || ((w >> 27) & 7 == 5 && (w >> 22) & 1 == 0)
}

/// The instruction may write X`reg`
pub fn writes(w: u32, reg: u32) -> bool {
    if (w >> 25) & 7 == 7 {
        return w & 31 == reg && (w & 0x5F20_FC00 == 0x1E20_0000 || w & 0xBFE0_DC00 == 0x0E00_2C00);
    }
    if w & 0x0A00_0000 == 0x0800_0000 && (w >> 26) & 1 != 0 {
        return false;
    }
    if w & 31 == reg && !reads_rt_only(w) {
        return true;
    }
    (w >> 27) & 7 == 5 && (w >> 22) & 1 != 0 && (w >> 26) & 1 == 0 && (w >> 10) & 31 == reg
}

/// RETAA, RETAB, BRAAZ/BRABZ, BRAA/BRAB: the end of a block, as RET and BR are
fn auth_end(w: u32) -> bool {
    w == 0xD65F_0BFF
        || w == 0xD65F_0FFF
        || w & 0xFFFF_F81F == 0xD61F_081F
        || w & 0xFFFF_F800 == 0xD71F_0800
}

/// A symbolic value in the window before a BR
#[derive(Clone, Debug)]
enum Val {
    C(u64),
    /// the register's value at the window's start, plus an offset added in the window (Reg2)
    Reg(u32),
    Reg2(u32, u64),
    /// a load of `size` bytes from base + Xm << shift
    Ld {
        base: Box<Val>,
        m: u32,
        size: u64,
        signed: bool,
    },
    Add(Option<Box<Val>>, Option<Box<Val>>, u32),
}

#[derive(Clone, Copy)]
enum Loc {
    Reg(u32),
    Slot(i64),
}

pub struct Discovery<'a> {
    pub img: &'a Image,
    pub entries: HashSet<u64>,
    eh_starts: Vec<u64>,
    /// entry -> body, and the order they were first found in
    pub funcs: HashMap<u64, Body>,
    pub order: Vec<u64>,
    pub jt: HashMap<u64, Vec<u64>>,
    /// BRs with no table found, in first-seen order
    jt_missing: Vec<(u64, u32)>,
    jt_missing_at: HashMap<u64, usize>,
    preds: HashMap<u64, HashMap<u64, Vec<u64>>>,
    pub plt: HashMap<u64, (u64, u64)>,
    pub setjmp_owner: BTreeMap<u64, Vec<u64>>,
    pub stats: BTreeMap<&'static str, u64>,
    /// (br, table register, load pc): lift_fixes.py fix 2's redirect of the table register's resolution
    redirect: Option<(u64, u32, u64)>,
}

const SETJMP_NAMES: [&str; 3] = ["__setjmp", "setjmp", "_setjmp"];

impl<'a> Discovery<'a> {
    pub fn new(img: &'a Image, extra: &BTreeSet<u64>) -> Self {
        let mut entries: HashSet<u64> = img.entries.iter().copied().collect();
        entries.extend(extra.iter().copied().filter(|&e| img.in_code(e)));
        entries.extend(img.hooks.keys().copied());
        let eh_starts = if img.eh_starts.is_empty() {
            let mut v: Vec<u64> = entries.iter().copied().collect();
            v.sort_unstable();
            v
        } else {
            img.eh_starts.clone()
        };
        Discovery {
            img,
            entries,
            eh_starts,
            funcs: HashMap::new(),
            order: Vec::new(),
            jt: HashMap::new(),
            jt_missing: Vec::new(),
            jt_missing_at: HashMap::new(),
            preds: HashMap::new(),
            plt: HashMap::new(),
            setjmp_owner: BTreeMap::new(),
            stats: BTreeMap::new(),
            redirect: None,
        }
    }

    fn stat(&mut self, k: &'static str) {
        *self.stats.entry(k).or_default() += 1;
    }

    fn set_func(&mut self, e: u64, b: Body) {
        if self.funcs.insert(e, b).is_none() {
            self.order.push(e);
        }
    }

    fn missing(&mut self, pc: u64, reg: u32) {
        match self.jt_missing_at.get(&pc) {
            Some(&i) => self.jt_missing[i].1 = reg,
            None => {
                self.jt_missing_at.insert(pc, self.jt_missing.len());
                self.jt_missing.push((pc, reg));
            }
        }
    }

    pub fn jt_missing(&self) -> &[(u64, u32)] {
        &self.jt_missing
    }

    pub fn discover(&mut self) {
        let mut es: Vec<u64> = self.entries.iter().copied().collect();
        es.sort_unstable();
        for e in es {
            if self.img.hooks.contains_key(&e) {
                continue;
            }
            let b = self.walk(e);
            self.set_func(e, b);
        }
        // A table found through another switch's edges needs those resolved first: retry the misses once with
        // every function's tables known, and re-walk the functions that contain a newly resolved one.
        let missing = std::mem::take(&mut self.jt_missing);
        self.jt_missing_at.clear();
        self.preds.clear();
        let mut found = HashSet::new();
        for &(pc, reg) in &missing {
            if let Some(t) = self.jump_table(pc, reg) {
                self.jt.insert(pc, t);
                found.insert(pc);
            }
        }
        let mut redo: Vec<u64> = self
            .funcs
            .iter()
            .filter(|(_, b)| b.insns.keys().any(|pc| found.contains(pc)))
            .map(|(&e, _)| e)
            .collect();
        redo.sort_unstable();
        for e in redo {
            let b = self.walk(e);
            self.set_func(e, b);
        }
        *self.stats.entry("jt_second_pass").or_default() =
            (missing.len() - self.jt_missing.len()) as u64;
        *self.stats.entry("jt_no_table").or_default() = self.jt_missing.len() as u64;
        for i in 0..self.order.len() {
            let e = self.order[i];
            if let Some(t) = self.plt_target(e) {
                self.plt.insert(e, t);
            }
        }
        self.find_setjmp_owners();
    }

    pub fn callee(&self, mut t: u64) -> u64 {
        for _ in 0..4 {
            match self.plt.get(&t) {
                Some(&(n, _)) => t = n,
                None => break,
            }
        }
        t
    }

    fn plt_target(&self, e: u64) -> Option<(u64, u64)> {
        let img = self.img;
        let w: Vec<u32> = (0..4)
            .map(|i| {
                img.read(e + 4 * i, 4)
                    .map(|b| u32::from_le_bytes(b.try_into().unwrap()))
            })
            .collect::<Option<_>>()?;
        if w[0] & 0x9F00_001F != 0x9000_0010
            || w[1] & 0xFFC0_03FF != 0xF940_0211
            || w[2] & 0xFFC0_03FF != 0x9100_0210
            || w[3] != 0xD61F_0220
        {
            return None;
        }
        let slot = (e & !0xFFF)
            .wrapping_add((adr_imm(w[0]) << 12) as u64)
            .wrapping_add((((w[1] >> 10) & 0xFFF) * 8) as u64);
        let t = u64::from_le_bytes(img.read(slot, 8)?.try_into().unwrap());
        self.entries.contains(&t).then_some((t, slot))
    }

    /// The functions that call __setjmp, with the calls' return addresses: entries too, walked here
    fn find_setjmp_owners(&mut self) {
        for i in 0..self.order.len() {
            let e = self.order[i];
            let rets: Vec<u64> = self.funcs[&e]
                .insns
                .iter()
                .filter_map(|(&pc, &(k, _))| match k {
                    Kind::Bl(t) => self
                        .img
                        .names
                        .get(&self.callee(t))
                        .filter(|n| SETJMP_NAMES.contains(&n.as_str()))
                        .map(|_| pc + 4),
                    _ => None,
                })
                .collect();
            if !rets.is_empty() {
                self.setjmp_owner.insert(e, rets);
            }
        }
        let owners: Vec<Vec<u64>> = self.setjmp_owner.values().cloned().collect();
        for rets in owners {
            for r in rets {
                if !self.funcs.contains_key(&r) {
                    self.entries.insert(r);
                    let b = self.walk(r);
                    self.set_func(r, b);
                }
            }
        }
        *self.stats.entry("setjmp_owners").or_default() = self.setjmp_owner.len() as u64;
    }

    pub fn walk(&mut self, entry: u64) -> Body {
        let mut insns = BTreeMap::new();
        let mut labels = BTreeSet::new();
        let mut todo = vec![entry];
        while let Some(mut pc) = todo.pop() {
            while !insns.contains_key(&pc) {
                if (pc != entry && self.entries.contains(&pc)) || !self.img.in_code(pc) {
                    break;
                }
                let w = self.img.word(pc);
                let k = classify(w, pc);
                insns.insert(pc, (k, w));
                let visit = |t: u64, labels: &mut BTreeSet<u64>, todo: &mut Vec<u64>, s: &Self| {
                    if t != entry && s.entries.contains(&t) {
                        return;
                    }
                    if s.img.in_code(t) {
                        labels.insert(t);
                        todo.push(t);
                    }
                };
                match k {
                    Kind::B(t) => {
                        visit(t, &mut labels, &mut todo, self);
                        break;
                    }
                    Kind::Bcond(t, _) | Kind::Cbz { t, .. } | Kind::Tbz { t, .. } => {
                        visit(t, &mut labels, &mut todo, self)
                    }
                    Kind::Br(reg) => {
                        if let Some(ts) = self.jump_table(pc, reg) {
                            self.add_jt_edges(pc, &ts);
                            for &t in &ts {
                                visit(t, &mut labels, &mut todo, self);
                            }
                            self.jt.insert(pc, ts);
                        }
                        break;
                    }
                    Kind::Ret | Kind::Trap => break,
                    _ => {}
                }
                pc += 4;
            }
        }
        Body { insns, labels }
    }

    // ---- jump tables ---------------------------------------------------------------------------------------------

    fn extent(&self, pc: u64) -> (u64, u128) {
        let s = &self.eh_starts;
        let i = s.partition_point(|&x| x <= pc);
        let lo = if i > 0 { s[i - 1] } else { 0 };
        let hi = if i < s.len() {
            s[i] as u128
        } else {
            1u128 << 64
        };
        (lo, hi)
    }

    fn preds_of(&mut self, lo: u64, hi: u128) -> &HashMap<u64, Vec<u64>> {
        if !self.preds.contains_key(&lo) {
            let mut p: HashMap<u64, Vec<u64>> = HashMap::new();
            let mut pc = lo;
            while (pc as u128) < hi {
                if self.img.in_code(pc) {
                    let k = classify(self.img.word(pc), pc);
                    let mut add = |s: u64| p.entry(s).or_default().push(pc);
                    match k {
                        Kind::B(t) => add(t),
                        Kind::Bcond(t, _) | Kind::Cbz { t, .. } | Kind::Tbz { t, .. } => {
                            add(t);
                            add(pc + 4);
                        }
                        Kind::Br(_) => {
                            for &t in self.jt.get(&pc).into_iter().flatten() {
                                add(t);
                            }
                        }
                        Kind::Ret | Kind::Trap => {}
                        _ => add(pc + 4),
                    }
                }
                pc += 4;
            }
            self.preds.insert(lo, p);
        }
        &self.preds[&lo]
    }

    fn add_jt_edges(&mut self, br: u64, targets: &[u64]) {
        let (lo, _) = self.extent(br);
        if let Some(p) = self.preds.get_mut(&lo) {
            for &t in targets {
                p.entry(t).or_default().push(br);
            }
        }
    }

    fn at(&self, at: u64, r: u32) -> u64 {
        match self.redirect {
            Some((br, treg, load)) if (at, r) == (br, treg) => load,
            _ => at,
        }
    }

    /// lift_fixes.py jump_table: the window cut at a pointer-authenticated return, the table register resolved at
    /// its load when the window reuses it, then a64_lift.py's search
    fn jump_table(&mut self, br: u64, reg: u32) -> Option<Vec<u64>> {
        let mut window = 32u64;
        for k in 1..=32u64 {
            let pc = br.wrapping_sub(4 * k);
            if self.img.in_code(pc) && auth_end(self.img.word(pc)) {
                window = k - 1;
                self.stat("jt_window_cut_at_auth_ret");
                break;
            }
        }
        let hit = self.table_load(br, window);
        self.redirect = hit.map(|(load, treg)| (br, treg, load));
        let out = self.jump_table_base(br, reg, window);
        self.redirect = None;
        if hit.is_some() && out.is_some() {
            self.stat("jt_table_reg_reused");
        }
        out
    }

    fn table_load(&self, br: u64, window: u64) -> Option<(u64, u32)> {
        for k in 1..window {
            let pc = br - 4 * k;
            if !self.img.in_code(pc) {
                return None;
            }
            let w = self.img.word(pc);
            if w & 0xFC00_0000 == 0x1400_0000
                || matches!(w & 0xFFFF_FC1F, 0xD65F_0000 | 0xD61F_0000)
            {
                return None;
            }
            if w & 0x3B20_0C00 == 0x3820_0800 && (w >> 26) & 1 == 0 && (w >> 22) & 3 != 0 {
                let n = (w >> 5) & 31;
                let mut q = pc + 4;
                while q < br {
                    if writes(self.img.word(q), n) {
                        return Some((pc, n));
                    }
                    q += 4;
                }
                return None;
            }
        }
        None
    }

    fn jump_table_base(&mut self, br: u64, reg: u32, window: u64) -> Option<Vec<u64>> {
        let img = self.img;
        let mut r: HashMap<u32, Option<Val>> = HashMap::new();
        let mut bound: Option<(u64, Option<u64>)> = None; // (imm, count)
        let mut cmp: Option<(u32, u64)> = None;
        let mut bounds: HashMap<u32, u64> = HashMap::new();
        let mut pc = br - 4 * window;
        while pc < br {
            let cur = pc;
            pc += 4;
            if !img.in_code(cur) {
                r.clear();
                bounds.clear();
                cmp = None;
                continue;
            }
            let w = img.word(cur);
            if w & 0xFC00_0000 == 0x1400_0000
                || matches!(w & 0xFFFF_FC1F, 0xD65F_0000 | 0xD61F_0000)
            {
                r.clear();
                bounds.clear();
                cmp = None;
                continue;
            }
            let (d, n, m) = (w & 31, (w >> 5) & 31, (w >> 16) & 31);
            let is_cmp = w & 0x7F80_001F == 0x7100_001F;
            let is_ccmp = w & 0x7FE0_0C10 == 0x7A40_0800;
            let is_bhi = matches!(w & 0xFF00_001F, 0x5400_0008 | 0x5400_0002);
            // index bounds, beside the values
            if is_cmp || is_ccmp {
                cmp = Some((
                    n,
                    if is_cmp {
                        ((w >> 10) & 0xFFF) as u64
                    } else {
                        ((w >> 16) & 31) as u64
                    },
                ));
            } else if is_bhi && cmp.is_some() {
                let (rr, imm) = cmp.unwrap();
                bounds.insert(rr, if w & 15 == 8 { imm + 1 } else { imm });
            } else if w & 0x7F80_0000 == 0x1200_0000 && (w >> 16) & 63 == 0 {
                let (imms, nbit) = ((w >> 10) & 63, (w >> 22) & 1);
                if (nbit != 0 || w >> 31 == 0) && imms < 31 {
                    bounds.insert(d, 1 << (imms + 1));
                } else {
                    bounds.remove(&d);
                }
            } else if w & 0x7FE0_FFE0 == 0x2A00_03E0 {
                match bounds.get(&m).copied() {
                    Some(b) => {
                        bounds.insert(d, b);
                    }
                    None => {
                        bounds.remove(&d);
                    }
                }
            } else if d != 31 && writes(w, d) {
                bounds.remove(&d);
            }
            let get_def = |r: &HashMap<u32, Option<Val>>, x: u32| match r.get(&x) {
                Some(v) => v.clone(),
                None => Some(Val::Reg(x)),
            };
            if w & 0x9F00_0000 == 0x9000_0000 {
                r.insert(
                    d,
                    Some(Val::C(
                        (cur & !0xFFF).wrapping_add((adr_imm(w) << 12) as u64),
                    )),
                );
            } else if w & 0x9F00_0000 == 0x1000_0000 {
                r.insert(d, Some(Val::C(cur.wrapping_add(adr_imm(w) as u64))));
            } else if w & 0xFF80_0000 == 0x9100_0000 {
                let imm = add_imm(w);
                match get_def(&r, n) {
                    Some(Val::C(c)) => {
                        r.insert(d, Some(Val::C(c.wrapping_add(imm))));
                    }
                    Some(Val::Reg(_) | Val::Reg2(..)) if d == n => {
                        r.remove(&d);
                    }
                    Some(Val::Reg(_)) => {
                        r.insert(d, Some(Val::Reg2(n, imm)));
                    }
                    _ => {
                        r.insert(d, None);
                    }
                }
            } else if w & 0x3B20_0C00 == 0x3820_0800 && (w >> 26) & 1 == 0 {
                let (size, opc) = (w >> 30, (w >> 22) & 3);
                let v = get_def(&r, n);
                let val = match v {
                    Some(v) if opc != 0 => Some(Val::Ld {
                        base: Box::new(v),
                        m,
                        size: 1 << size,
                        signed: opc >= 2,
                    }),
                    _ => None,
                };
                r.insert(d, val);
            } else if w & 0xFF20_0000 == 0x8B00_0000 && (w >> 22) & 3 == 0 {
                let a = r.get(&n).cloned().flatten().map(Box::new);
                let b = r.get(&m).cloned().flatten().map(Box::new);
                r.insert(d, Some(Val::Add(a, b, (w >> 10) & 63)));
            } else if w & 0xFFE0_0000 == 0x8B20_0000 {
                let a = r.get(&n).cloned().flatten().map(Box::new);
                let b = r.get(&m).cloned().flatten().map(Box::new);
                r.insert(d, Some(Val::Add(a, b, (w >> 10) & 7)));
            } else if is_cmp {
                bound = Some((((w >> 10) & 0xFFF) as u64, None));
            } else if is_ccmp {
                bound = Some((((w >> 16) & 31) as u64, None));
            } else if is_bhi && matches!(bound, Some((_, None))) {
                let imm = bound.unwrap().0;
                bound = Some((imm, Some(if w & 15 == 8 { imm + 1 } else { imm })));
            } else if writes(w, d) {
                r.insert(d, None);
            }
            let rt2 = (w >> 10) & 31;
            if (w >> 27) & 7 == 5 && (w >> 22) & 1 != 0 && (w >> 26) & 1 == 0 {
                r.insert(rt2, None);
                bounds.remove(&rt2);
            }
        }
        let v = r.get(&reg).cloned().flatten();
        if let Some(Val::C(c)) = v {
            let (lo, hi) = self.extent(br);
            if lo <= c && (c as u128) < hi && img.in_code(c) {
                self.stat("br_constant");
                return Some(vec![c]);
            }
            return None;
        }
        let Some(Val::Add(a, b, k)) = v else {
            return self.adr_targets(br, reg);
        };
        let (a, b) = if matches!(a.as_deref(), Some(Val::Ld { .. })) {
            (b, a)
        } else {
            (a, b)
        };
        let (
            Some(Val::C(base)),
            Some(Val::Ld {
                base: table,
                m: idx,
                size,
                signed,
            }),
        ) = (a.as_deref(), b.as_deref())
        else {
            self.stat("jt_unrecognized");
            self.missing(br, reg);
            return None;
        };
        let (base, size, signed, idx) = (*base, *size, *signed, *idx);
        let mut count = bound.and_then(|b| b.1);
        if let Some(&b) = bounds.get(&idx) {
            count = Some(b); // a bound on the very register that indexes the table wins
        }
        let tables: BTreeSet<u64> = match **table {
            Val::Reg(_) | Val::Reg2(..) => {
                let (reg_t, off) = match **table {
                    Val::Reg(x) => (x, 0),
                    Val::Reg2(x, o) => (x, o),
                    _ => unreachable!(),
                };
                let mut tables: BTreeSet<u64> =
                    self.reaching_constants(br, reg_t).into_iter().collect();
                let guess = match self.hoisted_constant(br, reg_t, 1024) {
                    Some(g) => Some(g),
                    None => self.function_constant(br, reg_t),
                };
                tables.extend(guess);
                let tables: BTreeSet<u64> =
                    tables.into_iter().map(|t| t.wrapping_add(off)).collect();
                if tables.is_empty() {
                    self.stat("jt_no_table");
                    self.missing(br, reg);
                    return None;
                }
                if tables.len() > 1 {
                    self.stat("jt_multi_table");
                }
                tables
            }
            Val::C(c) => [c].into(),
            // (a table address that is itself loaded or summed: a64_lift.py cannot use it either)
            _ => {
                self.stat("jt_unrecognized");
                self.missing(br, reg);
                return None;
            }
        };
        let (lo, hi) = self.extent(br);
        let count = count.filter(|&c| c > 0 && c <= 4096);
        let read = |table: u64, limit: u64, strict: bool| -> Option<Vec<u64>> {
            let mut out = Vec::new();
            for i in 0..limit {
                let Some(b) = img.read(table.wrapping_add(i * size), size) else {
                    break;
                };
                let e: i64 = match (size, signed) {
                    (1, false) => b[0] as i64,
                    (1, true) => b[0] as i8 as i64,
                    (2, false) => u16::from_le_bytes([b[0], b[1]]) as i64,
                    (2, true) => i16::from_le_bytes([b[0], b[1]]) as i64,
                    (4, false) => u32::from_le_bytes(b.try_into().unwrap()) as i64,
                    (4, true) => i32::from_le_bytes(b.try_into().unwrap()) as i64,
                    _ => u64::from_le_bytes(b.try_into().unwrap()) as i64,
                };
                let t = base.wrapping_add((e as u64).wrapping_shl(k));
                let t = if k >= 64 { base } else { t };
                if !(lo <= t && (t as u128) < hi && img.in_code(t)) {
                    if strict {
                        return None;
                    }
                    break;
                }
                out.push(t);
            }
            Some(out)
        };
        let mut targets: Vec<u64> = Vec::new();
        let mut bounded = true;
        for &t in &tables {
            let mut got = count.and_then(|c| read(t, c, true));
            if got.is_none() {
                if count.is_some() {
                    self.stat("jt_out_of_function");
                }
                got = read(t, if size == 1 { 256 } else { 1024 }, false);
                bounded = false;
            }
            for x in got.unwrap() {
                if !targets.contains(&x) {
                    targets.push(x);
                }
            }
        }
        if targets.is_empty() {
            return None;
        }
        self.stat(if bounded && count.is_some() {
            "jt_bounded"
        } else {
            "jt_unbounded"
        });
        Some(targets)
    }

    fn reaching_constants(&mut self, at: u64, reg: u32) -> Vec<u64> {
        let at = self.at(at, reg);
        let mut v: Vec<u64> = self
            .reaching(at, Loc::Reg(reg), 50000, 0)
            .into_iter()
            .collect();
        v.sort_unstable();
        v
    }

    fn reaching(&mut self, at: u64, loc: Loc, limit: usize, depth: u32) -> HashSet<u64> {
        let (lo, hi) = self.extent(at);
        if hi - lo as u128 > 0x40000 || depth > 4 {
            return HashSet::new();
        }
        self.preds_of(lo, hi);
        let mut found = HashSet::new();
        let mut seen = HashSet::new();
        let mut todo = vec![at];
        while !todo.is_empty() && seen.len() < limit {
            let node = todo.pop().unwrap();
            let ps = self.preds[&lo].get(&node).cloned().unwrap_or_default();
            for p in ps {
                if !seen.insert(p) {
                    continue;
                }
                let w = self.img.word(p);
                match loc {
                    Loc::Reg(x) => {
                        if matches!(classify(w, p), Kind::Bl(_) | Kind::Blr(_))
                            && (x <= 18 || x == 30)
                        {
                            continue;
                        }
                        if !writes(w, x) {
                            todo.push(p);
                            continue;
                        }
                        if w & 0x9F00_0000 == 0x9000_0000 {
                            found.insert((p & !0xFFF).wrapping_add((adr_imm(w) << 12) as u64));
                        } else if w & 0xFF80_0000 == 0x9100_0000 {
                            if let Some(v) = self.hoisted_constant(p, (w >> 5) & 31, 64) {
                                found.insert(v.wrapping_add(add_imm(w)));
                            }
                        } else if w & 0xFFE0_FFE0 == 0xAA00_03E0 {
                            found.extend(self.reaching(
                                p,
                                Loc::Reg((w >> 16) & 31),
                                limit,
                                depth + 1,
                            ));
                        } else if w & 0xFFC0_03E0 == 0xF940_03E0 {
                            found.extend(self.reaching(
                                p,
                                Loc::Slot(((w >> 10) & 0xFFF) as i64 * 8),
                                limit,
                                depth + 1,
                            ));
                        } else if w & 0xFFC0_03E0 == 0xA940_03E0 {
                            let off = sx(((w >> 15) & 0x7F) as u64, 7) * 8
                                + if w & 31 != x { 8 } else { 0 };
                            found.extend(self.reaching(p, Loc::Slot(off), limit, depth + 1));
                        }
                    }
                    Loc::Slot(x) => {
                        if w & 0xFFC0_03E0 == 0xF900_03E0 && ((w >> 10) & 0xFFF) as i64 * 8 == x {
                            found.extend(self.reaching(p, Loc::Reg(w & 31), limit, depth + 1));
                            continue;
                        }
                        if w & 0xFFC0_03E0 == 0xA900_03E0 {
                            let off = sx(((w >> 15) & 0x7F) as u64, 7) * 8;
                            if x == off || x == off + 8 {
                                let r = if x == off { w & 31 } else { (w >> 10) & 31 };
                                found.extend(self.reaching(p, Loc::Reg(r), limit, depth + 1));
                                continue;
                            }
                        }
                        if (w & 31 == 31 && matches!(w & 0x7F80_0000, 0x5100_0000 | 0x1100_0000))
                            || ((w >> 5) & 31 == 31
                                && (matches!(w & 0x3B20_0C00, 0x3800_0C00 | 0x3800_0400)
                                    || w & 0x3A80_0000 == 0x2880_0000))
                        {
                            continue; // SP moved: the slots differ
                        }
                        todo.push(p);
                    }
                }
            }
        }
        found
    }

    /// The one ADRP(+ADD) constant, or else the one ADR, the function ever assigns to `reg` (a64_lift.py and
    /// lift_fixes.py fix 1)
    fn function_constant(&mut self, at: u64, reg: u32) -> Option<u64> {
        let at = self.at(at, reg);
        let (lo, hi) = self.extent(at);
        if hi - lo as u128 > 0x40000 {
            return None;
        }
        let one = |found: HashSet<u64>| {
            if found.len() == 1 {
                found.into_iter().next()
            } else {
                None
            }
        };
        let img = self.img;
        let (mut page, mut found) = (HashMap::new(), HashSet::new());
        let mut pc = lo;
        while (pc as u128) < hi {
            let w = img.word(pc);
            let d = w & 31;
            if w & 0x9F00_0000 == 0x9000_0000 {
                page.insert(d, (pc & !0xFFF).wrapping_add((adr_imm(w) << 12) as u64));
            } else if w & 0xFF80_0000 == 0x9100_0000
                && page.contains_key(&((w >> 5) & 31))
                && d == reg
            {
                found.insert(page[&((w >> 5) & 31)].wrapping_add(add_imm(w)));
            }
            pc += 4;
        }
        if let Some(v) = one(found) {
            return Some(v);
        }
        let (mut page, mut found) = (HashMap::new(), HashSet::new());
        let mut pc = lo;
        while (pc as u128) < hi {
            if img.in_code(pc) {
                let w = img.word(pc);
                let d = w & 31;
                if w & 0x9F00_0000 == 0x9000_0000 {
                    page.insert(d, (pc & !0xFFF).wrapping_add((adr_imm(w) << 12) as u64));
                } else if w & 0xFF80_0000 == 0x9100_0000 && page.contains_key(&((w >> 5) & 31)) {
                    if d == reg {
                        found.insert(page[&((w >> 5) & 31)].wrapping_add(add_imm(w)));
                    }
                } else if w & 0x9F00_0000 == 0x1000_0000 && d == reg {
                    found.insert(pc.wrapping_add(adr_imm(w) as u64));
                }
            }
            pc += 4;
        }
        let v = one(found);
        if v.is_some() {
            self.stat("jt_adr_table");
        }
        v
    }

    /// Every `adr xreg, label` into the same function: candidate targets of a BR the window could not pin down
    fn adr_targets(&mut self, br: u64, reg: u32) -> Option<Vec<u64>> {
        let (lo, hi) = self.extent(br);
        if hi - lo as u128 > 0x40000 {
            return None;
        }
        let mut t = Vec::new();
        let mut pc = lo;
        while (pc as u128) < hi {
            let w = self.img.word(pc);
            if w & 0x9F00_001F == 0x1000_0000 | reg {
                let x = pc.wrapping_add(adr_imm(w) as u64);
                if lo <= x && (x as u128) < hi && self.img.in_code(x) {
                    t.push(x);
                }
            }
            pc += 4;
        }
        if t.is_empty() {
            return None;
        }
        self.stat("br_adr_candidates");
        Some(t)
    }

    /// The constant ADRP (+ ADD) last wrote to `reg` before `before`, scanning back in address order
    fn hoisted_constant(&mut self, before: u64, reg: u32, scan: u32) -> Option<u64> {
        let before = self.at(before, reg);
        let mut pc = before.wrapping_sub(4);
        for _ in 0..scan {
            if !self.img.in_code(pc) {
                return None;
            }
            let w = self.img.word(pc);
            if w & 31 == reg && !reads_rt_only(w) {
                if w & 0x9F00_0000 == 0x9000_0000 {
                    return Some((pc & !0xFFF).wrapping_add((adr_imm(w) << 12) as u64));
                }
                if w & 0xFF80_0000 == 0x9100_0000 {
                    return self
                        .hoisted_constant(pc, (w >> 5) & 31, 64)
                        .map(|v| v.wrapping_add(add_imm(w)));
                }
                return None;
            }
            pc = pc.wrapping_sub(4);
        }
        None
    }

    /// functions.json as nx2/lift.py writes it: {"<entry hex>": [instructions, module], ...} in discovery order
    pub fn functions_json(&self) -> String {
        let mut s = String::with_capacity(self.order.len() * 28);
        s.push('{');
        for (i, e) in self.order.iter().enumerate() {
            if i > 0 {
                s.push_str(", ");
            }
            s.push_str(&format!(
                "\"{:x}\": [{}, \"{}\"]",
                e,
                self.funcs[e].insns.len(),
                self.img.module_of(*e)
            ));
        }
        s.push('}');
        s
    }
}
