//! The Rust lifter: AArch64 -> Cranelift -> native code. a64.rs translates instructions; this file builds functions
//! around them, and `difftest` runs single instructions for rs/tests/difftest.py to compare against Unicorn.

pub mod a64;
pub mod ctx;

use a64::{Em, Regs, Unsupported};
use cranelift_codegen::ir::{types, AbiParam, InstBuilder, MemFlagsData, Value};
use cranelift_codegen::settings::{self, Configurable};
use cranelift_frontend::{FunctionBuilder, FunctionBuilderContext};
use cranelift_jit::{JITBuilder, JITModule};
use cranelift_module::{default_libcall_names, Linkage, Module};

pub fn isa() -> cranelift_codegen::isa::OwnedTargetIsa {
    let mut f = settings::builder();
    f.set("opt_level", "speed").unwrap();
    f.set("is_pic", "false").unwrap();
    f.set("use_colocated_libcalls", "false").unwrap();
    cranelift_native::builder().unwrap().finish(settings::Flags::new(f)).unwrap()
}

/// Loads every register and flag from the context into the variables
pub fn load_all(b: &mut FunctionBuilder, r: &Regs, c: Value) {
    for i in 0..31 {
        let v = b.ins().load(types::I64, MemFlagsData::trusted(), c, ctx::X + 8 * i as i32);
        b.def_var(r.x[i], v);
    }
    let v = b.ins().load(types::I64, MemFlagsData::trusted(), c, ctx::X + 8 * 31);
    b.def_var(r.sp, v);
    for (var, off) in [(r.n, ctx::N), (r.z, ctx::Z), (r.c, ctx::C), (r.v, ctx::V)] {
        let v = b.ins().load(types::I8, MemFlagsData::trusted(), c, off);
        b.def_var(var, v);
    }
}

/// Stores every register and flag variable back into the context
pub fn store_all(b: &mut FunctionBuilder, r: &Regs, c: Value) {
    for i in 0..31 {
        let v = b.use_var(r.x[i]);
        b.ins().store(MemFlagsData::trusted(), v, c, ctx::X + 8 * i as i32);
    }
    let v = b.use_var(r.sp);
    b.ins().store(MemFlagsData::trusted(), v, c, ctx::X + 8 * 31);
    for (var, off) in [(r.n, ctx::N), (r.z, ctx::Z), (r.c, ctx::C), (r.v, ctx::V)] {
        let v = b.use_var(var);
        b.ins().store(MemFlagsData::trusted(), v, c, off);
    }
}

// ---- difftest: one instruction per function, run on a context and a 4 KB guest page ------------------------------

/// The guest page the test cases address (rs/tests/difftest.py PAGE)
pub const TEST_PAGE: u64 = 0x10000;
const TEST_PAGE_SIZE: usize = 0x1000;
/// One case: u32 word, u32 nzcv (bits 31:28), u64 pc, 32 x u64 (x31 = SP), the page
const CASE_SIZE: usize = 4 + 4 + 8 + 32 * 8 + TEST_PAGE_SIZE;

fn map_test_page() -> *mut u8 {
    use windows_sys::Win32::System::Memory::{VirtualAlloc, MEM_COMMIT, MEM_RESERVE, PAGE_READWRITE};
    let want = (ctx::MEM_BASE as u64 + TEST_PAGE) as *const core::ffi::c_void;
    // SAFETY: a fresh reservation at a fixed address nothing else uses; checked below
    let p = unsafe { VirtualAlloc(want, TEST_PAGE_SIZE, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE) } as *mut u8;
    assert_eq!(p as usize, want as usize, "could not map the test page at the guest window");
    p
}

/// Runs every case of `input`; writes, per case, a u32 status (0 run, 1 not translated) and the state after it in the
/// case's layout
pub fn difftest(input: &[u8]) -> anyhow::Result<(Vec<u8>, usize, usize)> {
    let cases: Vec<&[u8]> = input.chunks_exact(CASE_SIZE).collect();
    let mut module = JITModule::new(JITBuilder::with_isa(isa(), default_libcall_names()));
    let mut cx = module.make_context();
    let mut fbc = FunctionBuilderContext::new();
    let mut ids = Vec::with_capacity(cases.len());
    for (i, case) in cases.iter().enumerate() {
        let word = u32::from_le_bytes(case[0..4].try_into().unwrap());
        let pc = u64::from_le_bytes(case[8..16].try_into().unwrap());
        cx.func.signature.params.push(AbiParam::new(types::I64));
        let ok = {
            let mut b = FunctionBuilder::new(&mut cx.func, &mut fbc);
            let entry = b.create_block();
            b.append_block_params_for_function_params(entry);
            b.switch_to_block(entry);
            b.seal_block(entry);
            let c = b.block_params(entry)[0];
            let regs = Regs::declare(&mut b);
            load_all(&mut b, &regs, c);
            let r = Em { b: &mut b, r: &regs, ctx: c, pc }.emit(word);
            store_all(&mut b, &regs, c);
            b.ins().return_(&[]);
            b.finalize(module.isa().frontend_config());
            r
        };
        if ok == Err(Unsupported) {
            ids.push(None);
            module.clear_context(&mut cx);
            continue;
        }
        let id = module.declare_function(&format!("t{i}"), Linkage::Local, &cx.func.signature)?;
        module
            .define_function(id, &mut cx)
            .map_err(|e| anyhow::anyhow!("case {i} ({word:08x}): {e:?}\n{}", cx.func.display()))?;
        module.clear_context(&mut cx);
        ids.push(Some(id));
    }
    module.finalize_definitions()?;
    let page = map_test_page();
    let mut out = Vec::with_capacity(cases.len() * (4 + CASE_SIZE));
    let (mut run, mut skipped) = (0, 0);
    for (case, id) in cases.iter().zip(&ids) {
        let Some(id) = id else {
            out.extend_from_slice(&1u32.to_le_bytes());
            out.extend_from_slice(case);
            skipped += 1;
            continue;
        };
        let mut c = ctx::Ctx::default();
        let nzcv = u32::from_le_bytes(case[4..8].try_into().unwrap());
        (c.n, c.z, c.c, c.v) = ((nzcv >> 31) as u8 & 1, (nzcv >> 30) as u8 & 1, (nzcv >> 29) as u8 & 1, (nzcv >> 28) as u8 & 1);
        for i in 0..32 {
            c.x[i] = u64::from_le_bytes(case[16 + 8 * i..24 + 8 * i].try_into().unwrap());
        }
        // SAFETY: the page is mapped read-write above and is TEST_PAGE_SIZE long
        let mem = unsafe { std::slice::from_raw_parts_mut(page, TEST_PAGE_SIZE) };
        mem.copy_from_slice(&case[16 + 256..]);
        let f: extern "C" fn(*mut ctx::Ctx) = unsafe { std::mem::transmute(module.get_finalized_function(*id)) };
        f(&mut c);
        run += 1;
        out.extend_from_slice(&0u32.to_le_bytes());
        out.extend_from_slice(&case[0..4]);
        let nzcv = (c.n as u32) << 31 | (c.z as u32) << 30 | (c.c as u32) << 29 | (c.v as u32) << 28;
        out.extend_from_slice(&nzcv.to_le_bytes());
        out.extend_from_slice(&case[8..16]);
        for x in c.x {
            out.extend_from_slice(&x.to_le_bytes());
        }
        out.extend_from_slice(mem);
    }
    Ok((out, run, skipped))
}

// ---- coverage: which of the image's instruction words translate ---------------------------------------------------

/// Control flow and the instructions the function builder handles itself (a64_lift.py classify), None for the rest
pub fn control_kind(w: u32) -> Option<&'static str> {
    Some(if w & 0x7C00_0000 == 0x1400_0000 {
        "b/bl"
    } else if w & 0xFF00_0010 == 0x5400_0000 {
        "bcond"
    } else if w & 0x7E00_0000 == 0x3400_0000 {
        "cbz"
    } else if w & 0x7E00_0000 == 0x3600_0000 {
        "tbz"
    } else if w & 0xFFFF_FC1F == 0xD61F_0000 || w & 0xFFFF_FC1F == 0xD63F_0000 || w & 0xFFFF_FC1F == 0xD65F_0000 {
        "br/blr/ret"
    } else if w & 0xFFE0_001F == 0xD400_0001 {
        "svc"
    } else if w & 0xFFE0_001F == 0xD420_0000 || w & 0xFFE0_001F == 0xD440_0000 {
        "trap"
    } else if w & 0xFFFF_F0FF == 0xD503_30DF {
        "isb"
    } else {
        return None;
    })
}

/// Distinct words and whether each translates: (translated, untranslated) counts weighted by occurrences, and the
/// untranslated ones by encoding group (bits 28:25 and the top byte)
pub fn coverage(words: &[u32]) -> (u64, u64, Vec<(u32, u64)>) {
    use std::collections::HashMap;
    let mut count: HashMap<u32, u64> = HashMap::new();
    for &w in words {
        if control_kind(w).is_none() {
            *count.entry(w).or_default() += 1;
        }
    }
    let isa = isa();
    let mut fbc = FunctionBuilderContext::new();
    let (mut ok, mut bad) = (0u64, 0u64);
    let mut by: HashMap<u32, u64> = HashMap::new();
    for (&w, &n) in &count {
        let mut func = cranelift_codegen::ir::Function::new();
        func.signature.params.push(AbiParam::new(types::I64));
        let mut b = FunctionBuilder::new(&mut func, &mut fbc);
        let entry = b.create_block();
        b.append_block_params_for_function_params(entry);
        b.switch_to_block(entry);
        b.seal_block(entry);
        let c = b.block_params(entry)[0];
        let regs = Regs::declare(&mut b);
        load_all(&mut b, &regs, c);
        let r = Em { b: &mut b, r: &regs, ctx: c, pc: 0x8000_0000 }.emit(w);
        b.ins().return_(&[]);
        b.finalize(isa.frontend_config());
        if r.is_ok() {
            ok += n;
        } else {
            bad += n;
            *by.entry(w >> 21).or_default() += n;
        }
    }
    let mut by: Vec<(u32, u64)> = by.into_iter().collect();
    by.sort_by_key(|&(_, n)| std::cmp::Reverse(n));
    (ok, bad, by)
}
