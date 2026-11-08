//! `hsr difftest-fn`: whole programs (several functions with branches, loops, calls, tail calls, jump tables, SVCs)
//! lifted by func.rs and run, for rs/tests/difftest_fn.py to compare against Unicorn.
//!
//! Input, a sequence of records: u32 length of the rest, the register block and the page of `difftest` (pc = the
//! entry), u32 n + n u64 entries, u32 n + n u32 words at CODE, u32 n + n jump tables (u64 BR address, u32 n, n u64
//! targets). Output per record: u32 status (0 ran, 1 not translated, 2 trapped), the register block, the page.
//!
//! The runtime the lifted code calls is the test's own (test_symbols): a64_svc applies x0 = 3 * x0 + x1 + imm, which
//! the Python side applies in Unicorn's interrupt hook; a64_poll does nothing (every case runs with poll set, so each
//! loop header takes the store / call / reload path); hsr_icall resolves guest addresses through the case's entries.

use super::func::{fname, walk, Builder, Code, Program};
use super::{ctx, helper_lookup, isa, map_test_page, REGS_SIZE, TEST_PAGE_SIZE};
use cranelift_jit::{JITBuilder, JITModule};
use cranelift_module::{default_libcall_names, Module};
use std::cell::RefCell;
use std::collections::{HashMap, HashSet};

/// Where the test programs' code is (rs/tests/difftest_fn.py CODE)
pub const CODE: u64 = 0x20000;

struct Words(Vec<u32>);

impl Code for Words {
    fn word(&self, pc: u64) -> Option<u32> {
        if pc < CODE || pc & 3 != 0 {
            return None;
        }
        self.0.get(((pc - CODE) / 4) as usize).copied()
    }
}

type HostFn = extern "C" fn(*mut ctx::Ctx);

thread_local! {
    static LOOKUP: RefCell<HashMap<u64, usize>> = RefCell::new(HashMap::new());
}

extern "C" fn a64_svc(c: *mut ctx::Ctx, imm: u32) {
    // SAFETY: lifted code passes its live context
    let c = unsafe { &mut *c };
    c.x[0] = c.x[0]
        .wrapping_mul(3)
        .wrapping_add(c.x[1])
        .wrapping_add(imm as u64);
}

extern "C" fn a64_poll(_c: *mut ctx::Ctx) {}

extern "C" fn a64_trap(c: *mut ctx::Ctx, pc: u64, _w: u32) {
    // SAFETY: as above
    let c = unsafe { &mut *c };
    if c.halted == 0 {
        c.halted = 1;
        c.pc = pc;
    }
}

extern "C" fn hsr_icall(c: *mut ctx::Ctx, t: u64, site: *mut u64) {
    let Some(f) = LOOKUP.with(|l| l.borrow().get(&t).copied()) else {
        return a64_trap(c, t, 0);
    };
    // SAFETY: site is a 16-byte slot pair of the calling function's cache; filled once, function before target
    unsafe {
        let tgt = &*(site as *const std::sync::atomic::AtomicU64);
        if tgt.load(std::sync::atomic::Ordering::Acquire) == 0 {
            *site.add(1) = f as u64;
            tgt.store(t, std::sync::atomic::Ordering::Release);
        }
        let f: HostFn = std::mem::transmute(f);
        f(c);
    }
}

pub fn test_symbols() -> Vec<(&'static str, *const u8)> {
    vec![
        ("a64_svc", a64_svc as *const u8),
        ("a64_poll", a64_poll as *const u8),
        ("a64_trap", a64_trap as *const u8),
        ("hsr_icall", hsr_icall as *const u8),
    ]
}

struct Rd<'a>(&'a [u8]);
impl Rd<'_> {
    fn u32(&mut self) -> u32 {
        let v = u32::from_le_bytes(self.0[..4].try_into().unwrap());
        self.0 = &self.0[4..];
        v
    }
    fn u64(&mut self) -> u64 {
        let v = u64::from_le_bytes(self.0[..8].try_into().unwrap());
        self.0 = &self.0[8..];
        v
    }
}

/// Runs the records; returns (output, ran, untranslated, trapped)
pub fn difftest_fn(input: &[u8]) -> anyhow::Result<(Vec<u8>, usize, usize, usize)> {
    let page = map_test_page();
    let mut out = Vec::new();
    let (mut ran, mut untr, mut trapped) = (0, 0, 0);
    let mut rest = input;
    while !rest.is_empty() {
        let len = u32::from_le_bytes(rest[..4].try_into().unwrap()) as usize;
        let rec = &rest[4..4 + len];
        rest = &rest[4 + len..];
        let (regs, tail) = rec.split_at(REGS_SIZE);
        let (mem0, tail) = tail.split_at(TEST_PAGE_SIZE);
        let mut r = Rd(tail);
        let entries: Vec<u64> = (0..r.u32()).map(|_| r.u64()).collect();
        let words: Vec<u32> = (0..r.u32()).map(|_| r.u32()).collect();
        let mut jt = HashMap::new();
        for _ in 0..r.u32() {
            let br = r.u64();
            let ts: Vec<u64> = (0..r.u32()).map(|_| r.u64()).collect();
            jt.insert(br, ts);
        }
        let code = Words(words);
        let p = Program {
            code: &code,
            entries: entries.iter().copied().collect(),
            jt,
            plt: HashMap::new(),
            setjmp_owners: HashSet::new(),
            polls: true,
        };
        let status = run_case(&p, &entries, regs, mem0, page, &mut out)?;
        match status {
            0 => ran += 1,
            1 => untr += 1,
            _ => trapped += 1,
        }
    }
    Ok((out, ran, untr, trapped))
}

fn run_case(
    p: &Program,
    entries: &[u64],
    regs: &[u8],
    mem0: &[u8],
    page: *mut u8,
    out: &mut Vec<u8>,
) -> anyhow::Result<u32> {
    let mut jb = JITBuilder::with_isa(isa(), default_libcall_names());
    jb.symbol_lookup_fn(helper_lookup());
    let mut m = JITModule::new(jb);
    let mut b = Builder::new(p, m.isa().frontend_config());
    let mut bodies = Vec::new();
    for &e in entries {
        let body = walk(p.code, e, &p.entries, &p.jt);
        match b.check(e, &body) {
            Ok(d) => bodies.push((e, body, d)),
            Err(_) => {
                out.extend_from_slice(&1u32.to_le_bytes());
                out.extend_from_slice(regs);
                out.extend_from_slice(mem0);
                return Ok(1);
            }
        }
    }
    b.rust = std::sync::Arc::new(entries.iter().copied().collect());
    for (e, body, d) in &bodies {
        b.define(&mut m, *e, body, *d)?;
    }
    m.finalize_definitions()?;
    let mut lookup = HashMap::new();
    for &e in entries {
        let id = m.get_name(&fname(e)).and_then(|n| match n {
            cranelift_module::FuncOrDataId::Func(f) => Some(f),
            _ => None,
        });
        lookup.insert(e, m.get_finalized_function(id.unwrap()) as usize);
    }
    LOOKUP.with(|l| *l.borrow_mut() = lookup);

    let mut c = ctx::Ctx::default();
    let nzcv = u32::from_le_bytes(regs[4..8].try_into().unwrap());
    (c.n, c.z, c.c, c.v) = (
        (nzcv >> 31) as u8 & 1,
        (nzcv >> 30) as u8 & 1,
        (nzcv >> 29) as u8 & 1,
        (nzcv >> 28) as u8 & 1,
    );
    for i in 0..32 {
        c.x[i] = u64::from_le_bytes(regs[16 + 8 * i..24 + 8 * i].try_into().unwrap());
    }
    c.poll = 1;
    // SAFETY: the page is mapped read-write and TEST_PAGE_SIZE long
    let mem = unsafe { std::slice::from_raw_parts_mut(page, TEST_PAGE_SIZE) };
    mem.copy_from_slice(mem0);
    let entry = u64::from_le_bytes(regs[8..16].try_into().unwrap());
    let f: HostFn = unsafe { std::mem::transmute(LOOKUP.with(|l| l.borrow()[&entry])) };
    f(&mut c);
    let status: u32 = if c.halted != 0 { 2 } else { 0 };
    out.extend_from_slice(&status.to_le_bytes());
    out.extend_from_slice(&regs[0..4]);
    let nzcv = (c.n as u32) << 31 | (c.z as u32) << 30 | (c.c as u32) << 29 | (c.v as u32) << 28;
    out.extend_from_slice(&nzcv.to_le_bytes());
    out.extend_from_slice(&c.pc.to_le_bytes());
    for x in c.x {
        out.extend_from_slice(&x.to_le_bytes());
    }
    out.extend_from_slice(&regs[16 + 256..REGS_SIZE]);
    out.extend_from_slice(mem);
    // SAFETY: nothing of this module runs after this point
    unsafe { m.free_memory() };
    Ok(status)
}
