//! Whole functions: which instructions belong to a function (a64_lift.py `walk`), and their translation into one
//! Cranelift function with the control flow a64.rs leaves out: branches, calls, returns, tail calls, jump tables,
//! indirect calls and jumps, SVC, traps and the loop-header interrupt polls (nx2/poll.py).
//!
//! Every lifted function gets two symbols:
//!   `t_<addr>`  the body, in Cranelift's `tail` calling convention, so a guest tail call between two Rust-lifted
//!               functions is a real tail call (`return_call`) and a chain of them does not grow the host stack;
//!   `f_<addr>`  the host-convention entry the C runtime and C-lifted code call (a64_lookup's table, a64_lift.py's
//!               names), a shim that calls `t_<addr>`.
//! A call or tail call of a function this build does not lift in Rust goes to its `f_` symbol (the C fallback); the
//! tail call is then a call and a return, as a64.h's TAILCALL is on targets without musttail.
//!
//! Registers and flags are Cranelift variables, loaded from the context at entry. Before anything that may read the
//! context (a call, an SVC, a poll, a trap, a return) the function stores back the variables it ever writes (its
//! dirty mask); after a call or an SVC it loads every register again, since the callee may have changed any of them.
//! This is exact for any callee, AAPCS or not; a64_lift.py's AAPCS shortcuts (scratch registers not stored, x19-x29
//! not reloaded) are not taken.

use super::a64::{Em, Imports, NoImports, Regs, Unsupported};
use super::{ctx, load_all};
use cranelift_codegen::ir::condcodes::IntCC;
use cranelift_codegen::ir::{
    types, AbiParam, Block, Function, InstBuilder, MemFlagsData, Signature, Value,
};
use cranelift_codegen::isa::CallConv;
use cranelift_frontend::{FunctionBuilder, FunctionBuilderContext, Switch};
use cranelift_module::{DataDescription, DataId, FuncId, Linkage, Module, ModuleResult};
use std::collections::{BTreeMap, BTreeSet, HashMap, HashSet};

/// The guest image as the builder reads it
pub trait Code {
    /// The instruction word at pc, None when pc is not in a code segment
    fn word(&self, pc: u64) -> Option<u32>;
    /// Eight bytes of the image at a (a GOT slot), None outside it
    fn read_u64(&self, _a: u64) -> Option<u64> {
        None
    }
}

/// What an instruction does to control flow (a64_lift.py `classify`, with nx2/insn.py's pointer-authenticated forms)
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Kind {
    /// Anything a64.rs translates
    Op,
    B(u64),
    Bl(u64),
    Bcond(u64, u32),
    Cbz {
        t: u64,
        rt: u32,
        sf: bool,
        nz: bool,
    },
    Tbz {
        t: u64,
        rt: u32,
        bit: u32,
        nz: bool,
    },
    /// BR / BRAA* / BRAB* through Xn (the pointer authentication is not checked: lifted code has no signed pointers
    /// to protect)
    Br(u32),
    Blr(u32),
    Ret,
    Svc(u32),
    /// BRK, HLT, UDF and words that are not code
    Trap,
    Isb,
    /// IC IVAU: there is no self-modifying code to keep coherent
    Ic,
}

fn sx(v: u32, n: u32) -> i64 {
    ((v as i64) << (64 - n)) >> (64 - n)
}

pub fn classify(w: u32, pc: u64) -> Kind {
    let rel = |imm: u32, n: u32| pc.wrapping_add((4 * sx(imm, n)) as u64);
    let rn = (w >> 5) & 31;
    if w & 0x7C00_0000 == 0x1400_0000 {
        let t = rel(w & 0x3FF_FFFF, 26);
        return if w >> 31 != 0 {
            Kind::Bl(t)
        } else {
            Kind::B(t)
        };
    }
    if w & 0xFF00_0010 == 0x5400_0000 {
        return Kind::Bcond(rel((w >> 5) & 0x7FFFF, 19), w & 15);
    }
    if w & 0x7E00_0000 == 0x3400_0000 {
        return Kind::Cbz {
            t: rel((w >> 5) & 0x7FFFF, 19),
            rt: w & 31,
            sf: w >> 31 != 0,
            nz: (w >> 24) & 1 != 0,
        };
    }
    if w & 0x7E00_0000 == 0x3600_0000 {
        return Kind::Tbz {
            t: rel((w >> 5) & 0x3FFF, 14),
            rt: w & 31,
            bit: ((w >> 31) << 5) | ((w >> 19) & 31),
            nz: (w >> 24) & 1 != 0,
        };
    }
    if w & 0xFFFF_FC1F == 0xD61F_0000 || w & 0xFEFF_F800 == 0xD61F_0800 {
        return Kind::Br(rn);
    }
    if w & 0xFFFF_FC1F == 0xD63F_0000 || w & 0xFEFF_F800 == 0xD63F_0800 {
        return Kind::Blr(rn);
    }
    if w & 0xFFFF_FC1F == 0xD65F_0000 || w == 0xD65F_0BFF || w == 0xD65F_0FFF {
        return Kind::Ret;
    }
    if w & 0xFFE0_001F == 0xD400_0001 {
        return Kind::Svc((w >> 5) & 0xFFFF);
    }
    if w & 0xFFE0_001F == 0xD420_0000 || w & 0xFFE0_001F == 0xD440_0000 {
        return Kind::Trap;
    }
    if w & 0xFFFF_F0FF == 0xD503_30DF {
        return Kind::Isb;
    }
    if w & 0xFFFF_FFE0 == 0xD50B_7520 {
        return Kind::Ic;
    }
    Kind::Op
}

/// One function's instructions: (kind, word) by address, and the addresses branched to inside it
#[derive(Default, Debug)]
pub struct Body {
    pub insns: BTreeMap<u64, (Kind, u32)>,
    pub labels: BTreeSet<u64>,
}

/// The instructions reachable from `entry` without entering another entry (a64_lift.py `walk`); `jt` gives the targets
/// of the BRs that are jump tables
pub fn walk(
    code: &dyn Code,
    entry: u64,
    entries: &HashSet<u64>,
    jt: &HashMap<u64, Vec<u64>>,
) -> Body {
    let mut insns = BTreeMap::new();
    let mut labels = BTreeSet::new();
    let mut todo = vec![entry];
    while let Some(mut pc) = todo.pop() {
        while !insns.contains_key(&pc) {
            if pc != entry && entries.contains(&pc) {
                break;
            }
            let Some(w) = code.word(pc) else { break };
            let k = classify(w, pc);
            insns.insert(pc, (k, w));
            let mut visit = |t: u64| {
                if (t == entry || !entries.contains(&t)) && code.word(t).is_some() {
                    labels.insert(t);
                    todo.push(t);
                }
            };
            match k {
                Kind::B(t) => {
                    visit(t);
                    break;
                }
                Kind::Bcond(t, _) | Kind::Cbz { t, .. } | Kind::Tbz { t, .. } => visit(t),
                Kind::Br(_) => {
                    for &t in jt.get(&pc).into_iter().flatten() {
                        visit(t);
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

/// A PLT stub at e (ADRP x16; LDR x17, [x16, #o]; ADD x16, x16, #o; BR x17): (its GOT slot's target, the slot), when the
/// target is an entry (a64_lift.py `plt_target`)
pub fn plt_target(code: &dyn Code, e: u64, entries: &HashSet<u64>) -> Option<(u64, u64)> {
    let w: Vec<u32> = (0..4)
        .map(|i| code.word(e + 4 * i))
        .collect::<Option<_>>()?;
    if w[0] & 0x9F00_001F != 0x9000_0010
        || w[1] & 0xFFC0_03FF != 0xF940_0211
        || w[2] & 0xFFC0_03FF != 0x9100_0210
        || w[3] != 0xD61F_0220
    {
        return None;
    }
    let imm = ((w[0] >> 3) & 0x1F_FFFC) | ((w[0] >> 29) & 3);
    let slot = (e & !0xFFF)
        .wrapping_add((sx(imm, 21) << 12) as u64)
        .wrapping_add((((w[1] >> 10) & 0xFFF) * 8) as u64);
    let t = code.read_u64(slot)?;
    entries.contains(&t).then_some((t, slot))
}

/// The program being lifted: the function entries and what discovery found about them
pub struct Program<'p> {
    pub code: &'p dyn Code,
    pub entries: HashSet<u64>,
    pub jt: HashMap<u64, Vec<u64>>,
    /// PLT stub entry -> (final target, GOT slot)
    pub plt: HashMap<u64, (u64, u64)>,
    /// Functions that call __setjmp: a call of one needs the host setjmp wrapper of a64_lift.py, which this builder
    /// does not emit yet, so their callers stay with the C lifter
    pub setjmp_owners: HashSet<u64>,
    /// Loop-header interrupt polls (nx2/poll.py, A64_POLLING)
    pub polls: bool,
}

pub fn fname(t: u64) -> String {
    format!("f_{t:08x}")
}
pub fn tname(t: u64) -> String {
    format!("t_{t:08x}")
}

impl Program<'_> {
    /// t through PLT stubs to the function that runs (a64_lift.py `callee`)
    pub fn callee(&self, mut t: u64) -> u64 {
        for _ in 0..4 {
            match self.plt.get(&t) {
                Some(&(n, _)) => t = n,
                None => break,
            }
        }
        t
    }
}

/// Builds functions of one Program into one Module
pub struct Builder<'p, 'q> {
    pub p: &'q Program<'p>,
    /// Functions with a t_ body in this module (a direct call or tail call of one goes to it)
    pub rust: HashSet<u64>,
    written: HashMap<u32, Result<u64, Unsupported>>,
    scratch: FunctionBuilderContext,
    pub fbc: FunctionBuilderContext,
    frontend: cranelift_codegen::isa::TargetFrontendConfig,
}

fn body_sig() -> Signature {
    let mut s = Signature::new(CallConv::Tail);
    s.params.push(AbiParam::new(types::I64));
    s
}

/// The ISA questions the builder asks of a module (object-safe)
pub trait ModuleIsa {
    fn host_cc(&self) -> CallConv;
}
impl<M: Module> ModuleIsa for M {
    fn host_cc(&self) -> CallConv {
        self.isa().default_call_conv()
    }
}

fn host_sig(cc: CallConv, extra: &[types::Type], ret: bool) -> Signature {
    let mut s = Signature::new(cc);
    s.params.push(AbiParam::new(types::I64));
    for &t in extra {
        s.params.push(AbiParam::new(t));
    }
    if ret {
        s.returns.push(AbiParam::new(types::I64));
    }
    s
}

const FLAGS: u32 = 32;

impl<'p, 'q> Builder<'p, 'q> {
    pub fn new(p: &'q Program<'p>, frontend: cranelift_codegen::isa::TargetFrontendConfig) -> Self {
        Builder {
            p,
            rust: HashSet::new(),
            written: HashMap::new(),
            scratch: FunctionBuilderContext::new(),
            fbc: FunctionBuilderContext::new(),
            frontend,
        }
    }

    /// The register variables an instruction word writes (Regs::dirty), or Unsupported: the word lifted alone into a
    /// throwaway function
    fn written(&mut self, w: u32) -> Result<u64, Unsupported> {
        if let Some(&r) = self.written.get(&w) {
            return r;
        }
        let mut func = Function::new();
        func.signature.params.push(AbiParam::new(types::I64));
        let r = {
            let mut b = FunctionBuilder::new(&mut func, &mut self.scratch);
            let entry = b.create_block();
            b.append_block_params_for_function_params(entry);
            b.switch_to_block(entry);
            b.seal_block(entry);
            let c = b.block_params(entry)[0];
            let regs = Regs::declare(&mut b);
            load_all(&mut b, &regs, c);
            let r = Em {
                b: &mut b,
                r: &regs,
                ctx: c,
                pc: 0x8000_0000,
                m: &mut NoImports,
                cc: CallConv::SystemV,
            }
            .emit(w)
            .map(|()| regs.dirty.get());
            b.ins().return_(&[]);
            b.finalize(self.frontend);
            r
        };
        self.written.insert(w, r);
        r
    }

    /// The function's dirty mask, or Unsupported when an instruction does not translate or a call needs setjmp
    pub fn check(&mut self, e: u64, body: &Body) -> Result<u64, Unsupported> {
        if self.p.plt.contains_key(&e) {
            return Ok(1 << 16 | 1 << 17);
        }
        let mut dirty = 0;
        for (_, &(k, w)) in &body.insns {
            match k {
                Kind::Op => dirty |= self.written(w)?,
                Kind::Bl(t) => {
                    if self.p.setjmp_owners.contains(&self.p.callee(t)) {
                        return Err(Unsupported);
                    }
                    dirty |= 1 << 30;
                }
                Kind::Blr(_) => dirty |= 1 << 30,
                _ => {}
            }
        }
        Ok(dirty)
    }

    /// Declares f_ and t_ of e; returns (f_, t_)
    pub fn declare<M: Module>(&self, m: &mut M, e: u64) -> ModuleResult<(FuncId, FuncId)> {
        let cc = m.host_cc();
        let f = m.declare_function(&fname(e), Linkage::Export, &host_sig(cc, &[], false))?;
        let t = m.declare_function(&tname(e), Linkage::Local, &body_sig())?;
        Ok((f, t))
    }

    /// Defines f_<e> and t_<e> in m (declare first). Unsupported leaves m without them.
    pub fn define<M: Module>(
        &mut self,
        m: &mut M,
        e: u64,
        body: &Body,
        dirty: u64,
    ) -> anyhow::Result<()> {
        let (fid, tid) = self.declare(m, e)?;
        let mut cx = m.make_context();
        cx.func.signature = body_sig();
        let sites = self.emit(m, &mut cx.func, e, body, dirty);
        m.define_function(tid, &mut cx)
            .map_err(|err| anyhow::anyhow!("t_{e:08x}: {err:?}\n{}", cx.func.display()))?;
        if let Some((id, n)) = sites {
            let mut d = DataDescription::new();
            d.define_zeroinit(16 * n);
            m.define_data(id, &d)?;
        }
        m.clear_context(&mut cx);
        // f_<e>: the host-convention entry
        cx.func.signature = host_sig(m.host_cc(), &[], false);
        {
            let mut b = FunctionBuilder::new(&mut cx.func, &mut self.fbc);
            let blk = b.create_block();
            b.append_block_params_for_function_params(blk);
            b.switch_to_block(blk);
            b.seal_block(blk);
            let c = b.block_params(blk)[0];
            let t = m.declare_func_in_func(tid, b.func);
            b.ins().call(t, &[c]);
            b.ins().return_(&[]);
            b.finalize(m.isa().frontend_config());
        }
        m.define_function(fid, &mut cx)
            .map_err(|err| anyhow::anyhow!("f_{e:08x}: {err:?}"))?;
        m.clear_context(&mut cx);
        Ok(())
    }

    /// Builds t_<e>'s IR into func; returns its call-site cache data (id, slot count) when it has indirect sites
    fn emit<M: Module>(
        &mut self,
        m: &mut M,
        func: &mut Function,
        e: u64,
        body: &Body,
        dirty: u64,
    ) -> Option<(DataId, usize)> {
        let frontend = m.isa().frontend_config();
        let mut b = FunctionBuilder::new(func, &mut self.fbc);
        let mut f = Fn {
            p: self.p,
            rust: &self.rust,
            m,
            regs: Regs::declare(&mut b),
            b,
            c: Value::from_u32(0),
            e,
            body,
            dirty,
            blocks: HashMap::new(),
            sites: None,
            nsites: 0,
        };
        f.run();
        let sites = f.sites.map(|id| (id, f.nsites));
        f.b.seal_all_blocks();
        f.b.finalize(frontend);
        sites
    }
}

/// The state of one function being built
struct Fn<'a, 'p, 'q, M: Module> {
    p: &'q Program<'p>,
    rust: &'a HashSet<u64>,
    m: &'a mut M,
    b: FunctionBuilder<'a>,
    regs: Regs,
    c: Value,
    e: u64,
    body: &'a Body,
    dirty: u64,
    blocks: HashMap<u64, Block>,
    sites: Option<DataId>,
    nsites: usize,
}

impl<M: Module> Fn<'_, '_, '_, M> {
    fn k64(&mut self, v: u64) -> Value {
        self.b.ins().iconst(types::I64, v as i64)
    }

    fn host_call(&mut self, name: &str, args: &[Value]) {
        let tys: Vec<types::Type> = args
            .iter()
            .map(|&a| self.b.func.dfg.value_type(a))
            .collect();
        let sig = host_sig(self.m.host_cc(), &tys, false);
        let fr = Imports::func(self.m, name, &sig, self.b.func);
        let mut all = vec![self.c];
        all.extend_from_slice(args);
        self.b.ins().call(fr, &all);
    }

    /// Stores the variables the function writes back into the context
    fn sync_out(&mut self) {
        let c = self.c;
        for i in 0..31 {
            if self.dirty >> i & 1 != 0 {
                let v = self.b.use_var(self.regs.x[i]);
                self.b
                    .ins()
                    .store(MemFlagsData::trusted(), v, c, ctx::X + 8 * i as i32);
            }
        }
        if self.dirty >> 31 & 1 != 0 {
            let v = self.b.use_var(self.regs.sp);
            self.b
                .ins()
                .store(MemFlagsData::trusted(), v, c, ctx::X + 8 * 31);
        }
        if self.dirty >> FLAGS & 1 != 0 {
            let r = &self.regs;
            for (var, off) in [(r.n, ctx::N), (r.z, ctx::Z), (r.c, ctx::C), (r.v, ctx::V)] {
                let v = self.b.use_var(var);
                self.b.ins().store(MemFlagsData::trusted(), v, c, off);
            }
        }
    }

    fn reload(&mut self) {
        load_all(&mut self.b, &self.regs, self.c);
    }

    fn block(&mut self, pc: u64) -> Block {
        if let Some(&b) = self.blocks.get(&pc) {
            return b;
        }
        let b = self.b.create_block();
        self.blocks.insert(pc, b);
        b
    }

    /// Leaves the current block: the rest of the function's code is reached through labels only
    fn dead(&mut self) {
        let b = self.b.create_block();
        self.b.switch_to_block(b);
    }

    /// A call of the function at t (an entry), host or tail convention by whether this module lifts it
    fn call_entry(&mut self, t: u64) {
        let c = self.c;
        if self.rust.contains(&t) {
            let sig = body_sig();
            let fr = Imports::func(self.m, &tname(t), &sig, self.b.func);
            self.b.ins().call(fr, &[c]);
        } else {
            let sig = host_sig(self.m.host_cc(), &[], false);
            let fr = Imports::func(self.m, &fname(t), &sig, self.b.func);
            self.b.ins().call(fr, &[c]);
        }
    }

    fn trap(&mut self, pc: u64, w: u32) {
        self.sync_out();
        let pc = self.k64(pc);
        let w = self.b.ins().iconst(types::I32, w as i64);
        self.host_call("a64_trap", &[pc, w]);
        self.b.ins().return_(&[]);
    }

    /// Control reaching t (a64_lift.py `transfer`): a jump inside the function, a tail call of another, or a trap
    fn transfer(&mut self, t: u64) {
        if self.body.insns.contains_key(&t) && (!self.p.entries.contains(&t) || t == self.e) {
            let blk = self.block(t);
            self.b.ins().jump(blk, &[]);
        } else if self.p.entries.contains(&t) {
            let t = self.p.callee(t);
            self.sync_out();
            let c = self.c;
            if self.rust.contains(&t) {
                let sig = body_sig();
                let fr = Imports::func(self.m, &tname(t), &sig, self.b.func);
                self.b.ins().return_call(fr, &[c]);
            } else {
                self.call_entry(t);
                self.b.ins().return_(&[]);
            }
        } else {
            self.trap(t, 0);
        }
    }

    /// An indirect call of the guest address in `target`, through a call-site cache: one slot pair {target, host
    /// function} filled once by the runtime's hsr_icall on the site's first resolvable target, never changed after
    /// (so a reader that sees the target also sees its function), a miss for every other target
    fn icall(&mut self, target: Value) {
        let id = match self.sites {
            Some(id) => id,
            None => {
                let id = self
                    .m
                    .declare_data(&format!("s_{:08x}", self.e), Linkage::Local, true, false)
                    .expect("site cache declaration");
                self.sites = Some(id);
                id
            }
        };
        let gv = self.m.declare_data_in_func(id, self.b.func);
        let base = self.b.ins().symbol_value(types::I64, gv);
        let site = self.b.ins().iadd_imm_s(base, 16 * self.nsites as i64);
        self.nsites += 1;
        let cached = self
            .b
            .ins()
            .atomic_load(types::I64, MemFlagsData::trusted(), site);
        let same = self.b.ins().icmp(IntCC::Equal, cached, target);
        let set = self.b.ins().icmp_imm_s(IntCC::NotEqual, cached, 0);
        let hit = self.b.ins().band(same, set);
        let (fast, slow, done) = (
            self.b.create_block(),
            self.b.create_block(),
            self.b.create_block(),
        );
        self.b.ins().brif(hit, fast, &[], slow, &[]);
        self.b.switch_to_block(fast);
        let fp = self
            .b
            .ins()
            .load(types::I64, MemFlagsData::trusted(), site, 8);
        let sig = host_sig(self.m.host_cc(), &[], false);
        let sr = self.b.import_signature(sig);
        let c = self.c;
        self.b.ins().call_indirect(sr, fp, &[c]);
        self.b.ins().jump(done, &[]);
        self.b.switch_to_block(slow);
        self.host_call("hsr_icall", &[target, site]);
        self.b.ins().jump(done, &[]);
        self.b.switch_to_block(done);
    }

    fn poll(&mut self) {
        let p = self
            .b
            .ins()
            .load(types::I32, MemFlagsData::trusted(), self.c, ctx::POLL);
        let (slow, go) = (self.b.create_block(), self.b.create_block());
        self.b.ins().brif(p, slow, &[], go, &[]);
        self.b.switch_to_block(slow);
        self.b.set_cold_block(slow);
        self.sync_out();
        self.host_call("a64_poll", &[]);
        self.reload();
        self.b.ins().jump(go, &[]);
        self.b.switch_to_block(go);
    }

    /// Loop headers: labels a branch at or after them goes back to (nx2/poll.py)
    fn loop_headers(&self) -> HashSet<u64> {
        let mut h = HashSet::new();
        for (&pc, &(k, _)) in &self.body.insns {
            let mut back = |t: u64| {
                if t <= pc
                    && self.body.insns.contains_key(&t)
                    && (t == self.e || !self.p.entries.contains(&t))
                {
                    h.insert(t);
                }
            };
            match k {
                Kind::B(t) | Kind::Bcond(t, _) | Kind::Cbz { t, .. } | Kind::Tbz { t, .. } => {
                    back(t)
                }
                Kind::Br(_) => {
                    for &t in self.p.jt.get(&pc).into_iter().flatten() {
                        back(t)
                    }
                }
                _ => {}
            }
        }
        h
    }

    fn run(&mut self) {
        let entry = self.b.create_block();
        self.b.append_block_params_for_function_params(entry);
        self.b.switch_to_block(entry);
        self.c = self.b.block_params(entry)[0];
        self.reload();
        if let Some(&(t, slot)) = self.p.plt.get(&self.e) {
            // a PLT stub reached indirectly: x16 = GOT slot, x17 = target, as the stub leaves them
            let (s, tv) = (self.k64(slot), self.k64(t));
            self.b.def_var(self.regs.x[16], s);
            self.b.def_var(self.regs.x[17], tv);
            self.transfer(t);
            return;
        }
        let first = self.block(self.e);
        self.b.ins().jump(first, &[]);
        self.dead();
        let headers = if self.p.polls {
            self.loop_headers()
        } else {
            HashSet::new()
        };
        let pcs: Vec<u64> = self.body.insns.keys().copied().collect();
        for (i, &pc) in pcs.iter().enumerate() {
            let (k, w) = self.body.insns[&pc];
            if pc == self.e || self.body.labels.contains(&pc) {
                let blk = self.block(pc);
                self.b.ins().jump(blk, &[]);
                self.b.switch_to_block(blk);
                if headers.contains(&pc) {
                    self.poll();
                }
            }
            let nxt = pc + 4;
            let mut falls = true;
            match k {
                Kind::Op => {
                    let cc = self.m.host_cc();
                    let r = Em {
                        b: &mut self.b,
                        r: &self.regs,
                        ctx: self.c,
                        pc,
                        m: &mut *self.m,
                        cc,
                    }
                    .emit(w);
                    if r.is_err() {
                        // (check() refuses such functions; a caller that skipped it gets the trap a64_lift.py emits)
                        self.trap(pc, w);
                        self.dead();
                        falls = false;
                    }
                }
                Kind::B(t) => {
                    self.transfer(t);
                    self.dead();
                    falls = false;
                }
                Kind::Bcond(_, _) | Kind::Cbz { .. } | Kind::Tbz { .. } => {
                    let (t, cond) = match k {
                        Kind::Bcond(t, cc) => {
                            let r = Em {
                                b: &mut self.b,
                                r: &self.regs,
                                ctx: self.c,
                                pc,
                                m: &mut *self.m,
                                cc: CallConv::SystemV,
                            }
                            .cond(cc);
                            (t, r)
                        }
                        Kind::Cbz { t, rt, sf, nz } => {
                            let v = if rt == 31 {
                                self.k64(0)
                            } else {
                                let v = self.b.use_var(self.regs.x[rt as usize]);
                                if sf {
                                    v
                                } else {
                                    self.b.ins().ireduce(types::I32, v)
                                }
                            };
                            let cc = if nz { IntCC::NotEqual } else { IntCC::Equal };
                            (t, self.b.ins().icmp_imm_s(cc, v, 0))
                        }
                        Kind::Tbz { t, rt, bit, nz } => {
                            let v = if rt == 31 {
                                self.k64(0)
                            } else {
                                self.b.use_var(self.regs.x[rt as usize])
                            };
                            let v = self.b.ins().band_imm_u(v, (1u64 << bit) as i64);
                            let cc = if nz { IntCC::NotEqual } else { IntCC::Equal };
                            (t, self.b.ins().icmp_imm_s(cc, v, 0))
                        }
                        _ => unreachable!(),
                    };
                    let (taken, not) = (self.b.create_block(), self.b.create_block());
                    self.b.ins().brif(cond, taken, &[], not, &[]);
                    self.b.switch_to_block(taken);
                    self.transfer(t);
                    self.b.switch_to_block(not);
                }
                Kind::Bl(t) => {
                    let lr = self.k64(nxt);
                    self.b.def_var(self.regs.x[30], lr);
                    if self.p.entries.contains(&t) {
                        self.sync_out();
                        let t = self.p.callee(t);
                        self.call_entry(t);
                        self.reload();
                    } else {
                        self.trap(pc, w);
                        self.dead();
                        falls = false;
                    }
                }
                Kind::Blr(rn) => {
                    let t = self.b.use_var(self.regs.x[rn as usize]);
                    let lr = self.k64(nxt);
                    self.b.def_var(self.regs.x[30], lr);
                    self.sync_out();
                    self.icall(t);
                    self.reload();
                }
                Kind::Ret => {
                    self.sync_out();
                    self.b.ins().return_(&[]);
                    self.dead();
                    falls = false;
                }
                Kind::Br(rn) => {
                    let t = self.b.use_var(self.regs.x[rn as usize]);
                    let other = self.b.create_block();
                    if let Some(ts) = self.p.jt.get(&pc) {
                        let ts: BTreeSet<u64> = ts.iter().copied().collect();
                        let mut sw = Switch::new();
                        let mut cases = Vec::new();
                        for &tt in &ts {
                            let blk = self.b.create_block();
                            sw.set_entry(tt as u128, blk);
                            cases.push((tt, blk));
                        }
                        sw.emit(&mut self.b, t, other);
                        for (tt, blk) in cases {
                            self.b.switch_to_block(blk);
                            self.transfer(tt);
                        }
                    } else {
                        self.b.ins().jump(other, &[]);
                    }
                    // BR without a table, or a value outside it: an indirect tail call (a call and a return)
                    self.b.switch_to_block(other);
                    self.sync_out();
                    self.icall(t);
                    self.b.ins().return_(&[]);
                    self.dead();
                    falls = false;
                }
                Kind::Svc(imm) => {
                    self.sync_out();
                    let imm = self.b.ins().iconst(types::I32, imm as i64);
                    self.host_call("a64_svc", &[imm]);
                    self.reload();
                }
                Kind::Trap => {
                    self.trap(pc, w);
                    self.dead();
                    falls = false;
                }
                Kind::Isb => {
                    self.b.ins().fence();
                }
                Kind::Ic => {}
            }
            if falls && pcs.get(i + 1) != Some(&nxt) {
                self.transfer(nxt);
                self.dead();
            }
        }
        // the block left open after the last instruction is unreachable
        self.b
            .ins()
            .trap(cranelift_codegen::ir::TrapCode::unwrap_user(1));
    }
}
