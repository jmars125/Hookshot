//! AArch64 instructions -> Cranelift IR, against the guest context of ctx.rs.
//!
//! Registers and flags live in Cranelift variables (the caller loads them from the context at the function's entry and
//! stores them back at its exits and around calls); guest memory is the 64-bit window at ctx::MEM_BASE.
//! The semantics are the Arm ARM's pseudocode; every instruction here is checked against Unicorn
//! (rs/tests/difftest.py). An instruction this file does not translate returns `Unsupported`, and the function holding it
//! is left to the C lifter.

use super::ctx;
use cranelift_codegen::ir::condcodes::IntCC;
use cranelift_codegen::ir::{
    types, FuncRef, Function, InstBuilder, MemFlagsData, Signature, Type, Value,
};

mod fp;
mod neon;
use cranelift_frontend::{FunctionBuilder, Variable};

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct Unsupported;

pub type R = Result<(), Unsupported>;

/// The variables holding the guest's integer registers and flags
#[derive(Clone)]
pub struct Regs {
    pub x: [Variable; 31],
    pub sp: Variable,
    pub n: Variable,
    pub z: Variable,
    pub c: Variable,
    pub v: Variable,
    /// Which variables the lifted code has written: bit n for Xn, 31 for SP, 32 for the flags (func.rs stores only
    /// these back to the context)
    pub dirty: std::cell::Cell<u64>,
}

impl Regs {
    pub fn declare(b: &mut FunctionBuilder) -> Regs {
        let x = std::array::from_fn(|_| b.declare_var(types::I64));
        let sp = b.declare_var(types::I64);
        let [n, z, c, v] = std::array::from_fn(|_| b.declare_var(types::I8));
        Regs {
            x,
            sp,
            n,
            z,
            c,
            v,
            dirty: Default::default(),
        }
    }
}

/// Resolves the runtime helpers lifted code calls (the exact FP bodies, for one): a FuncRef in the function being built
pub trait Imports {
    fn func(&mut self, name: &str, sig: &Signature, f: &mut Function) -> FuncRef;
}

impl<M: cranelift_module::Module> Imports for M {
    fn func(&mut self, name: &str, sig: &Signature, f: &mut Function) -> FuncRef {
        let id = self
            .declare_function(name, cranelift_module::Linkage::Import, sig)
            .expect("helper declaration");
        self.declare_func_in_func(id, f)
    }
}

/// For translating without a module (coverage): every helper is a placeholder external name
pub struct NoImports;

impl Imports for NoImports {
    fn func(&mut self, _name: &str, sig: &Signature, f: &mut Function) -> FuncRef {
        let sig = f.import_signature(sig.clone());
        let name =
            f.declare_imported_user_function(cranelift_codegen::ir::UserExternalName::new(0, 0));
        f.import_function(cranelift_codegen::ir::ExtFuncData {
            name: cranelift_codegen::ir::ExternalName::user(name),
            signature: sig,
            colocated: false,
            patchable: false,
        })
    }
}

pub struct Em<'a, 'b> {
    pub b: &'a mut FunctionBuilder<'b>,
    pub r: &'a Regs,
    pub ctx: Value,
    pub pc: u64,
    pub m: &'a mut dyn Imports,
    /// The host calling convention of the runtime helpers (the function being built may use another)
    pub cc: cranelift_codegen::isa::CallConv,
}

fn bits(w: u32, hi: u32, lo: u32) -> u32 {
    (w >> lo) & ((1u32 << (hi - lo + 1)) - 1)
}

fn sx(v: u64, nbits: u32) -> i64 {
    ((v << (64 - nbits)) as i64) >> (64 - nbits)
}

fn ones(n: u32) -> u64 {
    if n >= 64 {
        u64::MAX
    } else {
        (1u64 << n) - 1
    }
}

fn ror(v: u64, r: u32, size: u32) -> u64 {
    let v = v & ones(size);
    if r == 0 {
        v
    } else {
        ((v >> r) | (v << (size - r))) & ones(size)
    }
}

/// DecodeBitMasks of the Arm ARM: (wmask, tmask), None when the encoding is reserved
pub fn decode_bit_masks(
    n: u32,
    imms: u32,
    immr: u32,
    immediate: bool,
    datasize: u32,
) -> Option<(u64, u64)> {
    let combined = (n << 6) | (!imms & 0x3f);
    if combined == 0 {
        return None;
    }
    let len = 31 - combined.leading_zeros();
    if len < 1 {
        return None;
    }
    let levels = ones(len) as u32;
    if immediate && (imms & levels) == levels {
        return None;
    }
    let s = imms & levels;
    let r = immr & levels;
    let d = s.wrapping_sub(r) & levels;
    let esize = 1u32 << len;
    if esize > datasize {
        return None;
    }
    let welem = ror(ones(s + 1), r, esize);
    let telem = ones(d + 1);
    let rep = |e: u64| {
        let mut out = 0u64;
        let mut i = 0;
        while i < datasize {
            out |= e << i;
            i += esize;
        }
        out & ones(datasize)
    };
    Some((rep(welem), rep(telem)))
}

impl Em<'_, '_> {
    fn ty(sf: bool) -> Type {
        if sf {
            types::I64
        } else {
            types::I32
        }
    }
    fn k(&mut self, ty: Type, v: u64) -> Value {
        let v = if ty == types::I32 {
            v as u32 as i32 as i64
        } else {
            v as i64
        };
        self.b.ins().iconst(ty, v)
    }
    /// a OP constant, the constant at a's type
    fn band_k(&mut self, a: Value, v: u64) -> Value {
        let t = self.b.func.dfg.value_type(a);
        let k = self.k(t, v);
        self.b.ins().band(a, k)
    }
    fn bor_k(&mut self, a: Value, v: u64) -> Value {
        let t = self.b.func.dfg.value_type(a);
        let k = self.k(t, v);
        self.b.ins().bor(a, k)
    }
    fn bxor_k(&mut self, a: Value, v: u64) -> Value {
        let t = self.b.func.dfg.value_type(a);
        let k = self.k(t, v);
        self.b.ins().bxor(a, k)
    }
    fn narrow(&mut self, v: Value, sf: bool) -> Value {
        if sf {
            v
        } else {
            self.b.ins().ireduce(types::I32, v)
        }
    }
    fn widen(&mut self, v: Value) -> Value {
        if self.b.func.dfg.value_type(v) == types::I64 {
            v
        } else {
            self.b.ins().uextend(types::I64, v)
        }
    }
    /// Xn / Wn, register 31 = zero
    pub fn x(&mut self, r: u32, sf: bool) -> Value {
        if r == 31 {
            return self.k(Self::ty(sf), 0);
        }
        let v = self.b.use_var(self.r.x[r as usize]);
        self.narrow(v, sf)
    }
    /// Xn / Wn, register 31 = SP
    pub fn xsp(&mut self, r: u32, sf: bool) -> Value {
        let v = if r == 31 {
            self.b.use_var(self.r.sp)
        } else {
            self.b.use_var(self.r.x[r as usize])
        };
        self.narrow(v, sf)
    }
    /// Writes Xd (a W value is zero-extended); register 31 = zero, the write is dropped
    pub fn set(&mut self, r: u32, v: Value) {
        if r == 31 {
            return;
        }
        let v = self.widen(v);
        self.mark(r);
        self.b.def_var(self.r.x[r as usize], v);
    }
    /// Writes Xd (a W value is zero-extended); register 31 = SP
    pub fn setsp(&mut self, r: u32, v: Value) {
        let v = self.widen(v);
        self.mark(r);
        if r == 31 {
            self.b.def_var(self.r.sp, v);
        } else {
            self.b.def_var(self.r.x[r as usize], v);
        }
    }
    /// Records a write of register variable `bit` (Regs::dirty)
    pub(crate) fn mark(&self, bit: u32) {
        self.r.dirty.set(self.r.dirty.get() | 1 << bit);
    }
    fn set_flags(&mut self, n: Value, z: Value, c: Value, v: Value) {
        self.mark(32);
        self.b.def_var(self.r.n, n);
        self.b.def_var(self.r.z, z);
        self.b.def_var(self.r.c, c);
        self.b.def_var(self.r.v, v);
    }
    fn set_nz00(&mut self, res: Value) {
        let n = self.b.ins().icmp_imm_s(IntCC::SignedLessThan, res, 0);
        let z = self.b.ins().icmp_imm_s(IntCC::Equal, res, 0);
        let zero = self.b.ins().iconst(types::I8, 0);
        self.set_flags(n, z, zero, zero);
    }

    /// AddWithCarry(a, b, carry_in): (result, N, Z, C, V)
    fn add_with_carry(&mut self, a: Value, b: Value, cin: Value, sf: bool) -> (Value, [Value; 4]) {
        let ty = Self::ty(sf);
        let wide = if sf { types::I128 } else { types::I64 };
        let cw = self.b.ins().uextend(wide, cin);
        let ua = self.b.ins().uextend(wide, a);
        let ub = self.b.ins().uextend(wide, b);
        let us = self.b.ins().iadd(ua, ub);
        let us = self.b.ins().iadd(us, cw);
        let res = self.b.ins().ireduce(ty, us);
        let ur = self.b.ins().uextend(wide, res);
        let c = self.b.ins().icmp(IntCC::NotEqual, ur, us);
        let sa = self.b.ins().sextend(wide, a);
        let sb = self.b.ins().sextend(wide, b);
        let ss = self.b.ins().iadd(sa, sb);
        let ss = self.b.ins().iadd(ss, cw);
        let sr = self.b.ins().sextend(wide, res);
        let v = self.b.ins().icmp(IntCC::NotEqual, sr, ss);
        let n = self.b.ins().icmp_imm_s(IntCC::SignedLessThan, res, 0);
        let z = self.b.ins().icmp_imm_s(IntCC::Equal, res, 0);
        (res, [n, z, c, v])
    }

    /// a + b or a - b, setting the flags when `s`
    fn addsub(&mut self, a: Value, b: Value, sub: bool, s: bool, sf: bool) -> Value {
        if !s {
            return if sub {
                self.b.ins().isub(a, b)
            } else {
                self.b.ins().iadd(a, b)
            };
        }
        let (b, cin) = if sub {
            (self.b.ins().bnot(b), 1)
        } else {
            (b, 0)
        };
        let cin = self.b.ins().iconst(types::I8, cin);
        let (res, [n, z, c, v]) = self.add_with_carry(a, b, cin, sf);
        self.set_flags(n, z, c, v);
        res
    }

    /// ConditionHolds(cond), as an i8 0/1
    pub fn cond(&mut self, cc: u32) -> Value {
        let n = self.b.use_var(self.r.n);
        let z = self.b.use_var(self.r.z);
        let c = self.b.use_var(self.r.c);
        let v = self.b.use_var(self.r.v);
        let base = match cc >> 1 {
            0 => z,
            1 => c,
            2 => n,
            3 => v,
            4 => {
                let nz = self.b.ins().bxor_imm_u(z, 1);
                self.b.ins().band(c, nz)
            }
            5 => {
                let e = self.b.ins().bxor(n, v);
                self.b.ins().bxor_imm_u(e, 1)
            }
            6 => {
                let e = self.b.ins().bxor(n, v);
                let ge = self.b.ins().bxor_imm_u(e, 1);
                let nz = self.b.ins().bxor_imm_u(z, 1);
                self.b.ins().band(ge, nz)
            }
            _ => return self.b.ins().iconst(types::I8, 1),
        };
        if cc & 1 != 0 {
            self.b.ins().bxor_imm_u(base, 1)
        } else {
            base
        }
    }

    fn shift(
        &mut self,
        v: Value,
        kind: u32,
        amount: u32,
        allow_ror: bool,
    ) -> Result<Value, Unsupported> {
        if amount == 0 {
            return Ok(v);
        }
        let a = amount as i64;
        Ok(match kind {
            0 => self.b.ins().ishl_imm_u(v, a),
            1 => self.b.ins().ushr_imm_u(v, a),
            2 => self.b.ins().sshr_imm_u(v, a),
            _ if allow_ror => self.b.ins().rotr_imm_u(v, a),
            _ => return Err(Unsupported),
        })
    }

    /// ExtendReg(m, option, shift), as a value of the operation's size
    fn extend(&mut self, m: u32, option: u32, sh: u32, sf: bool) -> Value {
        let x = self.x(m, true);
        let (bits, signed) = (8u32 << (option & 3), option & 4 != 0);
        let v = if bits == 64 {
            x
        } else {
            let t = match bits {
                8 => types::I8,
                16 => types::I16,
                _ => types::I32,
            };
            let n = self.b.ins().ireduce(t, x);
            if signed {
                self.b.ins().sextend(types::I64, n)
            } else {
                self.b.ins().uextend(types::I64, n)
            }
        };
        let v = if sh != 0 {
            self.b.ins().ishl_imm_u(v, sh as i64)
        } else {
            v
        };
        self.narrow(v, sf)
    }

    // ---- guest memory --------------------------------------------------------------------------------------------
    fn host(&mut self, addr: Value) -> Value {
        self.b.ins().iadd_imm_s(addr, ctx::MEM_BASE)
    }
    fn load(&mut self, ty: Type, addr: Value) -> Value {
        let h = self.host(addr);
        self.b.ins().load(ty, MemFlagsData::new(), h, 0)
    }
    fn store(&mut self, v: Value, addr: Value) {
        let h = self.host(addr);
        self.b.ins().store(MemFlagsData::new(), v, h, 0);
    }
    /// A load of `size` bytes into a register: zero-extended, or sign-extended to 32 / 64 bits
    fn load_ext(&mut self, size: u32, sign: Option<Type>, addr: Value) -> Value {
        let t = [types::I8, types::I16, types::I32, types::I64][size as usize];
        let v = self.load(t, addr);
        match sign {
            Some(to) if to != t => {
                let s = self.b.ins().sextend(to, v);
                self.widen(s)
            }
            _ => self.widen(v),
        }
    }
    fn store_n(&mut self, size: u32, rt: u32, addr: Value) {
        let v = self.x(rt, true);
        let v = if size == 3 {
            v
        } else {
            self.b
                .ins()
                .ireduce([types::I8, types::I16, types::I32][size as usize], v)
        };
        self.store(v, addr);
    }

    pub fn emit(&mut self, w: u32) -> R {
        match bits(w, 28, 25) {
            0b1000 | 0b1001 => self.dp_imm(w),
            0b0101 | 0b1101 => self.dp_reg(w),
            0b0100 | 0b0110 | 0b1100 | 0b1110 => self.ldst(w),
            0b1010 | 0b1011 => self.system(w),
            0b0111 | 0b1111 => self.fp_scalar(w),
            _ => Err(Unsupported),
        }
    }

    // ---- data processing, immediate ------------------------------------------------------------------------------
    fn dp_imm(&mut self, w: u32) -> R {
        let sf = w >> 31 != 0;
        let (rd, rn) = (bits(w, 4, 0), bits(w, 9, 5));
        match bits(w, 25, 23) {
            0b000 | 0b001 => {
                // ADR / ADRP
                let imm = sx(((bits(w, 23, 5) as u64) << 2) | bits(w, 30, 29) as u64, 21);
                let v = if w >> 31 != 0 {
                    (self.pc & !0xfff).wrapping_add((imm << 12) as u64)
                } else {
                    self.pc.wrapping_add(imm as u64)
                };
                let v = self.k(types::I64, v);
                self.set(rd, v);
                Ok(())
            }
            0b010 => {
                // ADD/SUB (immediate)
                let (sub, s) = (bits(w, 30, 30) != 0, bits(w, 29, 29) != 0);
                let imm = (bits(w, 21, 10) as u64) << (12 * bits(w, 22, 22));
                let a = self.xsp(rn, sf);
                let b = self.k(Self::ty(sf), imm);
                let r = self.addsub(a, b, sub, s, sf);
                if s {
                    self.set(rd, r);
                } else {
                    self.setsp(rd, r);
                }
                Ok(())
            }
            0b100 => {
                // logical (immediate)
                let n = bits(w, 22, 22);
                if !sf && n != 0 {
                    return Err(Unsupported);
                }
                let datasize = if sf { 64 } else { 32 };
                let (imm, _) =
                    decode_bit_masks(n, bits(w, 15, 10), bits(w, 21, 16), true, datasize)
                        .ok_or(Unsupported)?;
                let a = self.x(rn, sf);
                let r = match bits(w, 30, 29) {
                    0 | 3 => self.band_k(a, imm),
                    1 => self.bor_k(a, imm),
                    _ => self.bxor_k(a, imm),
                };
                if bits(w, 30, 29) == 3 {
                    self.set_nz00(r);
                    self.set(rd, r);
                } else {
                    self.setsp(rd, r);
                }
                Ok(())
            }
            0b101 => {
                // move wide
                let (opc, hw) = (bits(w, 30, 29), bits(w, 22, 21));
                if opc == 1 || (!sf && hw >= 2) {
                    return Err(Unsupported);
                }
                let imm = (bits(w, 20, 5) as u64) << (16 * hw);
                let ty = Self::ty(sf);
                let r = match opc {
                    0 => self.k(ty, !imm),
                    2 => self.k(ty, imm),
                    _ => {
                        let old = self.x(rd, sf);
                        let kept = self.band_k(old, !(0xffffu64 << (16 * hw)));
                        self.bor_k(kept, imm)
                    }
                };
                self.set(rd, r);
                Ok(())
            }
            0b110 => self.bitfield(w),
            0b111 => {
                // EXTR
                let (n, o0) = (bits(w, 22, 22), bits(w, 21, 21));
                let lsb = bits(w, 15, 10);
                if bits(w, 30, 29) != 0 || o0 != 0 || (n != 0) != sf || (!sf && lsb >= 32) {
                    return Err(Unsupported);
                }
                let rm = bits(w, 20, 16);
                let lo = self.x(rm, sf);
                let r = if lsb == 0 {
                    lo
                } else {
                    let hi = self.x(rn, sf);
                    let size = if sf { 64 } else { 32 };
                    let l = self.b.ins().ushr_imm_u(lo, lsb as i64);
                    let h = self.b.ins().ishl_imm_u(hi, (size - lsb) as i64);
                    self.b.ins().bor(l, h)
                };
                self.set(rd, r);
                Ok(())
            }
            _ => Err(Unsupported),
        }
    }

    fn bitfield(&mut self, w: u32) -> R {
        let sf = w >> 31 != 0;
        let (opc, n, immr, imms) = (
            bits(w, 30, 29),
            bits(w, 22, 22),
            bits(w, 21, 16),
            bits(w, 15, 10),
        );
        let (rd, rn) = (bits(w, 4, 0), bits(w, 9, 5));
        if opc == 3 || (n != 0) != sf || (!sf && (immr >= 32 || imms >= 32)) {
            return Err(Unsupported);
        }
        let size = if sf { 64 } else { 32 };
        let (wmask, tmask) = decode_bit_masks(n, imms, immr, false, size).ok_or(Unsupported)?;
        let src = self.x(rn, sf);
        let rot = if immr == 0 {
            src
        } else {
            self.b.ins().rotr_imm_u(src, immr as i64)
        };
        let bot = self.band_k(rot, wmask);
        let r = match opc {
            0 => {
                // SBFM: top = replicate(src<S>)
                let t = self.b.ins().ishl_imm_u(src, (size - 1 - imms) as i64);
                let top = self.b.ins().sshr_imm_u(t, (size - 1) as i64);
                let top = self.band_k(top, !tmask & ones(size));
                let bot = self.band_k(bot, tmask);
                self.b.ins().bor(top, bot)
            }
            1 => {
                // BFM
                let dst = self.x(rd, sf);
                let keep = self.band_k(dst, !wmask & ones(size));
                let bot = self.b.ins().bor(keep, bot);
                let top = self.band_k(dst, !tmask & ones(size));
                let bot = self.band_k(bot, tmask);
                self.b.ins().bor(top, bot)
            }
            _ => self.band_k(bot, tmask),
        };
        self.set(rd, r);
        Ok(())
    }

    // ---- data processing, register -------------------------------------------------------------------------------
    fn dp_reg(&mut self, w: u32) -> R {
        let sf = w >> 31 != 0;
        let (rd, rn, rm) = (bits(w, 4, 0), bits(w, 9, 5), bits(w, 20, 16));
        if w & 0x1F00_0000 == 0x0A00_0000 {
            // logical (shifted register)
            let imm6 = bits(w, 15, 10);
            if !sf && imm6 >= 32 {
                return Err(Unsupported);
            }
            let a = self.x(rn, sf);
            let b = self.x(rm, sf);
            let b = self.shift(b, bits(w, 23, 22), imm6, true)?;
            let b = if bits(w, 21, 21) != 0 {
                self.b.ins().bnot(b)
            } else {
                b
            };
            let opc = bits(w, 30, 29);
            let r = match opc {
                0 | 3 => self.b.ins().band(a, b),
                1 => self.b.ins().bor(a, b),
                _ => self.b.ins().bxor(a, b),
            };
            if opc == 3 {
                self.set_nz00(r);
            }
            self.set(rd, r);
            return Ok(());
        }
        if w & 0x1F20_0000 == 0x0B00_0000 {
            // add/sub (shifted register)
            let (imm6, sh) = (bits(w, 15, 10), bits(w, 23, 22));
            if sh == 3 || (!sf && imm6 >= 32) {
                return Err(Unsupported);
            }
            let a = self.x(rn, sf);
            let b = self.x(rm, sf);
            let b = self.shift(b, sh, imm6, false)?;
            let r = self.addsub(a, b, bits(w, 30, 30) != 0, bits(w, 29, 29) != 0, sf);
            self.set(rd, r);
            return Ok(());
        }
        if w & 0x1F20_0000 == 0x0B20_0000 {
            // add/sub (extended register)
            let (imm3, opt) = (bits(w, 12, 10), bits(w, 23, 22));
            if imm3 > 4 || opt != 0 {
                return Err(Unsupported);
            }
            let s = bits(w, 29, 29) != 0;
            let a = self.xsp(rn, sf);
            let b = self.extend(rm, bits(w, 15, 13), imm3, sf);
            let r = self.addsub(a, b, bits(w, 30, 30) != 0, s, sf);
            if s {
                self.set(rd, r);
            } else {
                self.setsp(rd, r);
            }
            return Ok(());
        }
        if w & 0x1FE0_FC00 == 0x1A00_0000 {
            // ADC / SBC
            let (sub, s) = (bits(w, 30, 30) != 0, bits(w, 29, 29) != 0);
            let a = self.x(rn, sf);
            let b = self.x(rm, sf);
            let b = if sub { self.b.ins().bnot(b) } else { b };
            let cin = self.b.use_var(self.r.c);
            let (res, f) = self.add_with_carry(a, b, cin, sf);
            if s {
                self.set_flags(f[0], f[1], f[2], f[3]);
            }
            self.set(rd, res);
            return Ok(());
        }
        if w & 0x1FE0_0000 == 0x1A40_0000 {
            // CCMN / CCMP
            if bits(w, 29, 29) == 0 || bits(w, 10, 10) != 0 || bits(w, 4, 4) != 0 {
                return Err(Unsupported);
            }
            let sub = bits(w, 30, 30) != 0;
            let a = self.x(rn, sf);
            let b = if bits(w, 11, 11) != 0 {
                self.k(Self::ty(sf), rm as u64)
            } else {
                self.x(rm, sf)
            };
            let (b, cin) = if sub {
                (self.b.ins().bnot(b), 1)
            } else {
                (b, 0)
            };
            let cin = self.b.ins().iconst(types::I8, cin);
            let (_, f) = self.add_with_carry(a, b, cin, sf);
            let holds = self.cond(bits(w, 15, 12));
            let nzcv = bits(w, 3, 0);
            let vars = [self.r.n, self.r.z, self.r.c, self.r.v];
            for i in 0..4 {
                let imm = self
                    .b
                    .ins()
                    .iconst(types::I8, ((nzcv >> (3 - i)) & 1) as i64);
                let v = self.b.ins().select(holds, f[i as usize], imm);
                self.mark(32);
                self.b.def_var(vars[i as usize], v);
            }
            return Ok(());
        }
        if w & 0x1FE0_0000 == 0x1A80_0000 {
            // CSEL / CSINC / CSINV / CSNEG
            let (op, op2) = (bits(w, 30, 30), bits(w, 11, 10));
            if bits(w, 29, 29) != 0 || op2 > 1 {
                return Err(Unsupported);
            }
            let a = self.x(rn, sf);
            let b = self.x(rm, sf);
            let b = match (op, op2) {
                (0, 0) => b,
                (0, _) => self.b.ins().iadd_imm_s(b, 1),
                (_, 0) => self.b.ins().bnot(b),
                _ => self.b.ins().ineg(b),
            };
            let holds = self.cond(bits(w, 15, 12));
            let r = self.b.ins().select(holds, a, b);
            self.set(rd, r);
            return Ok(());
        }
        // Pointer authentication is the identity, as in nx2/insn.py: PAC*/AUT* and XPAC* leave Xd unchanged, PACGA
        // writes 0 (no pointer the game uses is ever signed)
        if w & 0xFFFF_C000 == 0xDAC1_0000 || w & 0xFFFF_FBE0 == 0xDAC1_43E0 {
            return Ok(());
        }
        if w & 0xFFE0_FC00 == 0x9AC0_3000 {
            let z = self.k(types::I64, 0);
            self.set(rd, z);
            return Ok(());
        }
        if w & 0x5FE0_0000 == 0x1AC0_0000 {
            // data processing (2 source)
            if bits(w, 29, 29) != 0 {
                return Err(Unsupported);
            }
            let a = self.x(rn, sf);
            let b = self.x(rm, sf);
            let ty = Self::ty(sf);
            let r = match bits(w, 15, 10) {
                0b000010 => {
                    // UDIV: x / 0 = 0
                    let one = self.k(ty, 1);
                    let zero = self.k(ty, 0);
                    let is0 = self.b.ins().icmp_imm_s(IntCC::Equal, b, 0);
                    let d = self.b.ins().select(is0, one, b);
                    let q = self.b.ins().udiv(a, d);
                    self.b.ins().select(is0, zero, q)
                }
                0b000011 => {
                    // SDIV: x / 0 = 0, MIN / -1 = MIN
                    let one = self.k(ty, 1);
                    let zero = self.k(ty, 0);
                    let is0 = self.b.ins().icmp_imm_s(IntCC::Equal, b, 0);
                    let m1 = self.b.ins().icmp_imm_s(IntCC::Equal, b, -1);
                    let bad = self.b.ins().bor(is0, m1);
                    let d = self.b.ins().select(bad, one, b);
                    let q = self.b.ins().sdiv(a, d);
                    let neg = self.b.ins().ineg(a);
                    let q = self.b.ins().select(m1, neg, q);
                    self.b.ins().select(is0, zero, q)
                }
                0b001000 => self.b.ins().ishl(a, b),
                0b001001 => self.b.ins().ushr(a, b),
                0b001010 => self.b.ins().sshr(a, b),
                0b001011 => self.b.ins().rotr(a, b),
                op @ 0b010000..=0b010111 => {
                    // CRC32{B,H,W,X} / CRC32C*: the reflected CRC of the low 8 << sz bits of Rm into Wn, bit by bit
                    let sz = op & 3;
                    if (sz == 3) != sf {
                        return Err(Unsupported);
                    }
                    let poly: i64 = if op & 4 != 0 {
                        0x82F6_3B78
                    } else {
                        0xEDB8_8320
                    };
                    let acc = self.x(rn, false);
                    let acc = self.b.ins().uextend(types::I64, acc);
                    let data = self.x(rm, true);
                    let data = if sz == 3 {
                        data
                    } else {
                        self.band_k(data, ones(8 << sz))
                    };
                    let mut t = self.b.ins().bxor(acc, data);
                    let p = self.k(types::I64, poly as u64);
                    for _ in 0..(8 << sz) {
                        let lsb = self.b.ins().band_imm_u(t, 1);
                        let mask = self.b.ins().ineg(lsb);
                        let m = self.b.ins().band(mask, p);
                        let s = self.b.ins().ushr_imm_u(t, 1);
                        t = self.b.ins().bxor(s, m);
                    }
                    let r = self.b.ins().ireduce(types::I32, t);
                    self.set(rd, r);
                    return Ok(());
                }
                _ => return Err(Unsupported),
            };
            self.set(rd, r);
            return Ok(());
        }
        if w & 0x5FE0_0000 == 0x5AC0_0000 {
            // data processing (1 source)
            if bits(w, 29, 29) != 0 || bits(w, 20, 16) != 0 {
                return Err(Unsupported);
            }
            let a = self.x(rn, sf);
            let r = match (bits(w, 15, 10), sf) {
                (0, _) => self.b.ins().bitrev(a),
                (1, _) => {
                    // REV16: the bytes of each halfword swapped
                    let m = if sf {
                        0x00ff_00ff_00ff_00ffu64
                    } else {
                        0x00ff_00ff
                    };
                    let lo = self.band_k(a, m);
                    let lo = self.b.ins().ishl_imm_u(lo, 8);
                    let hi = self.b.ins().ushr_imm_u(a, 8);
                    let hi = self.band_k(hi, m);
                    self.b.ins().bor(lo, hi)
                }
                (2, false) | (3, true) => self.b.ins().bswap(a),
                (2, true) => {
                    // REV32: the bytes of each word reversed
                    let s = self.b.ins().bswap(a);
                    self.b.ins().rotr_imm_u(s, 32)
                }
                (4, _) => self.b.ins().clz(a),
                (5, _) => self.b.ins().cls(a),
                _ => return Err(Unsupported),
            };
            self.set(rd, r);
            return Ok(());
        }
        if w & 0x1F00_0000 == 0x1B00_0000 {
            // data processing (3 source)
            let (op54, op31, o0) = (bits(w, 30, 29), bits(w, 23, 21), bits(w, 15, 15));
            let ra = bits(w, 14, 10);
            if op54 != 0 {
                return Err(Unsupported);
            }
            let r = match (op31, sf) {
                (0, _) => {
                    let a = self.x(rn, sf);
                    let b = self.x(rm, sf);
                    let p = self.b.ins().imul(a, b);
                    let acc = self.x(ra, sf);
                    if o0 != 0 {
                        self.b.ins().isub(acc, p)
                    } else {
                        self.b.ins().iadd(acc, p)
                    }
                }
                (1, true) | (5, true) => {
                    // SMADDL / SMSUBL / UMADDL / UMSUBL
                    let a = self.x(rn, false);
                    let b = self.x(rm, false);
                    let (a, b) = if op31 == 1 {
                        (
                            self.b.ins().sextend(types::I64, a),
                            self.b.ins().sextend(types::I64, b),
                        )
                    } else {
                        (
                            self.b.ins().uextend(types::I64, a),
                            self.b.ins().uextend(types::I64, b),
                        )
                    };
                    let p = self.b.ins().imul(a, b);
                    let acc = self.x(ra, true);
                    if o0 != 0 {
                        self.b.ins().isub(acc, p)
                    } else {
                        self.b.ins().iadd(acc, p)
                    }
                }
                (2, true) | (6, true) if o0 == 0 => {
                    let a = self.x(rn, true);
                    let b = self.x(rm, true);
                    if op31 == 2 {
                        self.b.ins().smulhi(a, b)
                    } else {
                        self.b.ins().umulhi(a, b)
                    }
                }
                _ => return Err(Unsupported),
            };
            self.set(rd, r);
            return Ok(());
        }
        Err(Unsupported)
    }

    // ---- loads and stores ----------------------------------------------------------------------------------------
    fn ldst(&mut self, w: u32) -> R {
        if w & 0x3F00_0000 == 0x0800_0000 {
            return self.exclusive(w);
        }
        if w & 0x3F20_0C00 == 0x3820_0000 {
            return self.atomic(w);
        }
        if bits(w, 26, 26) != 0 {
            return self.ldst_simd(w);
        }
        let (rt, rn) = (bits(w, 4, 0), bits(w, 9, 5));
        if w & 0x3B00_0000 == 0x1800_0000 {
            // LDR (literal)
            let addr = self
                .pc
                .wrapping_add((sx(bits(w, 23, 5) as u64, 19) * 4) as u64);
            let a = self.k(types::I64, addr);
            let v = match bits(w, 31, 30) {
                0 => self.load_ext(2, None, a),
                1 => self.load_ext(3, None, a),
                2 => self.load_ext(2, Some(types::I64), a),
                _ => return Ok(()), // PRFM
            };
            self.set(rt, v);
            return Ok(());
        }
        if w & 0x3A00_0000 == 0x2800_0000 {
            return self.ldst_pair(w);
        }
        let size = bits(w, 31, 30);
        let opc = bits(w, 23, 22);
        // (address, written-back address or None)
        let (addr, wb): (Value, Option<Value>) = if w & 0x3B00_0000 == 0x3900_0000 {
            let base = self.xsp(rn, true);
            (
                self.b
                    .ins()
                    .iadd_imm_s(base, ((bits(w, 21, 10) as u64) << size) as i64),
                None,
            )
        } else if w & 0x3B20_0000 == 0x3800_0000 {
            let imm = sx(bits(w, 20, 12) as u64, 9);
            let base = self.xsp(rn, true);
            match bits(w, 11, 10) {
                // 0 unscaled, 2 unprivileged (LDTR/STTR: the same at EL0)
                0 | 2 => (self.b.ins().iadd_imm_s(base, imm), None),
                1 => {
                    let nb = self.b.ins().iadd_imm_s(base, imm);
                    (base, Some(nb))
                }
                _ => {
                    let nb = self.b.ins().iadd_imm_s(base, imm);
                    (nb, Some(nb))
                }
            }
        } else if w & 0x3B20_0C00 == 0x3820_0800 {
            let option = bits(w, 15, 13);
            if option & 2 == 0 {
                return Err(Unsupported);
            }
            let sh = if bits(w, 12, 12) != 0 { size } else { 0 };
            let base = self.xsp(rn, true);
            let off = self.extend(bits(w, 20, 16), option, sh, true);
            (self.b.ins().iadd(base, off), None)
        } else {
            return Err(Unsupported);
        };
        match opc {
            0 => self.store_n(size, rt, addr),
            1 => {
                let v = self.load_ext(size, None, addr);
                self.set(rt, v);
            }
            2 if size == 3 => {} // PRFM
            2 => {
                let v = self.load_ext(size, Some(types::I64), addr);
                self.set(rt, v);
            }
            _ if size >= 2 => return Err(Unsupported),
            _ => {
                let v = self.load_ext(size, Some(types::I32), addr);
                self.set(rt, v);
            }
        }
        if let Some(nb) = wb {
            self.setsp(rn, nb);
        }
        Ok(())
    }

    fn ity(size: u32) -> Type {
        [types::I8, types::I16, types::I32, types::I64][size as usize]
    }
    fn host_addr(&mut self, rn: u32) -> Value {
        let a = self.xsp(rn, true);
        self.host(a)
    }
    fn ctx_store(&mut self, v: Value, off: i32) {
        self.b
            .ins()
            .store(MemFlagsData::trusted(), v, self.ctx, off);
    }
    fn ctx_load(&mut self, ty: Type, off: i32) -> Value {
        self.b
            .ins()
            .load(ty, MemFlagsData::trusted(), self.ctx, off)
    }
    /// Xt narrowed to an access of 1 << size bytes
    fn x_sized(&mut self, r: u32, size: u32) -> Value {
        let v = self.x(r, true);
        if size == 3 {
            v
        } else {
            self.b.ins().ireduce(Self::ity(size), v)
        }
    }

    /// Exclusives, load-acquire / store-release and CAS. The monitor is a64-kit's (rt/a64_rt.c recomp_ldxr,
    /// recomp_stxr): LDXR records the address, size and value it saw; STXR compare-and-swaps against that value.
    fn exclusive(&mut self, w: u32) -> R {
        let (size, o2, l, o1, rs, rt2) = (
            bits(w, 31, 30),
            bits(w, 23, 23),
            bits(w, 22, 22) != 0,
            bits(w, 21, 21),
            bits(w, 20, 16),
            bits(w, 14, 10),
        );
        let (rt, rn) = (bits(w, 4, 0), bits(w, 9, 5));
        let ty = Self::ity(size);
        match (o2, o1) {
            (0, 0) => {
                let a = self.xsp(rn, true);
                let h = self.host(a);
                if l {
                    // LDXR / LDAXR
                    let v = self.b.ins().atomic_load(ty, MemFlagsData::new(), h);
                    let v = self.widen(v);
                    self.ctx_store(a, ctx::EXCL_ADDR);
                    let sz = self.b.ins().iconst(types::I32, 1 << size);
                    self.ctx_store(sz, ctx::EXCL_SIZE);
                    self.ctx_store(v, ctx::EXCL_LO);
                    let one = self.b.ins().iconst(types::I32, 1);
                    self.ctx_store(one, ctx::EXCL_VALID);
                    self.set(rt, v);
                    return Ok(());
                }
                // STXR / STLXR: Ws = 0 when stored, 1 when not
                if rs == rt || (rs == rn && rn != 31) {
                    return Err(Unsupported);
                }
                let valid = self.ctx_load(types::I32, ctx::EXCL_VALID);
                let ea = self.ctx_load(types::I64, ctx::EXCL_ADDR);
                let es = self.ctx_load(types::I32, ctx::EXCL_SIZE);
                let same_a = self.b.ins().icmp(IntCC::Equal, ea, a);
                let same_s = self.b.ins().icmp_imm_s(IntCC::Equal, es, 1 << size);
                let v_ok = self.b.ins().icmp_imm_s(IntCC::NotEqual, valid, 0);
                let ok = self.b.ins().band(same_a, same_s);
                let ok = self.b.ins().band(ok, v_ok);
                let zero = self.b.ins().iconst(types::I32, 0);
                self.ctx_store(zero, ctx::EXCL_VALID);
                let try_b = self.b.create_block();
                let done = self.b.create_block();
                self.b.append_block_param(done, types::I64);
                let fail = self.b.ins().iconst(types::I64, 1);
                self.b.ins().brif(ok, try_b, &[], done, &[fail.into()]);
                self.b.switch_to_block(try_b);
                self.b.seal_block(try_b);
                let expect = self.ctx_load(types::I64, ctx::EXCL_LO);
                let expect = if size == 3 {
                    expect
                } else {
                    self.b.ins().ireduce(ty, expect)
                };
                let val = self.x_sized(rt, size);
                let old = self.b.ins().atomic_cas(MemFlagsData::new(), h, expect, val);
                let neq = self.b.ins().icmp(IntCC::NotEqual, old, expect);
                let st = self.b.ins().uextend(types::I64, neq);
                self.b.ins().jump(done, &[st.into()]);
                self.b.switch_to_block(done);
                self.b.seal_block(done);
                let st = self.b.block_params(done)[0];
                self.set(rs, st);
                Ok(())
            }
            (1, 0) => {
                // LDAR / STLR (o0 = 1) and LDLAR / STLLR (o0 = 0): sequentially consistent here
                let h = self.host_addr(rn);
                if l {
                    let v = self.b.ins().atomic_load(ty, MemFlagsData::new(), h);
                    let v = self.widen(v);
                    self.set(rt, v);
                } else {
                    let v = self.x_sized(rt, size);
                    self.b.ins().atomic_store(MemFlagsData::new(), v, h);
                }
                Ok(())
            }
            (1, 1) if rt2 == 31 => {
                // CAS / CASA / CASL / CASAL: Xs = the old value; stored when it equals Xs
                let h = self.host_addr(rn);
                let e = self.x_sized(rs, size);
                let v = self.x_sized(rt, size);
                let old = self.b.ins().atomic_cas(MemFlagsData::new(), h, e, v);
                let old = self.widen(old);
                self.set(rs, old);
                Ok(())
            }
            _ => Err(Unsupported),
        }
    }

    /// LSE atomic memory operations: LDADD, LDCLR, LDEOR, LDSET, LDSMAX, LDSMIN, LDUMAX, LDUMIN, SWP
    fn atomic(&mut self, w: u32) -> R {
        use cranelift_codegen::ir::AtomicRmwOp as Op;
        if bits(w, 26, 26) != 0 {
            return Err(Unsupported);
        }
        let (size, rs, o3, opc) = (
            bits(w, 31, 30),
            bits(w, 20, 16),
            bits(w, 15, 15),
            bits(w, 14, 12),
        );
        let (rt, rn) = (bits(w, 4, 0), bits(w, 9, 5));
        if (o3, opc, rs, bits(w, 23, 22)) == (1, 4, 31, 2) {
            // LDAPR{B,H,}: a load-acquire (RCpc ordering is weaker than LDAR's, so LDAR's lowering is correct for it)
            let h = self.host_addr(rn);
            let v = self
                .b
                .ins()
                .atomic_load(Self::ity(size), MemFlagsData::new(), h);
            let v = self.widen(v);
            self.set(rt, v);
            return Ok(());
        }
        let op = match (o3, opc) {
            (0, 0) => Op::Add,
            (0, 1) => Op::And, // LDCLR: AND with the complement
            (0, 2) => Op::Xor,
            (0, 3) => Op::Or,
            (0, 4) => Op::Smax,
            (0, 5) => Op::Smin,
            (0, 6) => Op::Umax,
            (0, 7) => Op::Umin,
            (1, 0) => Op::Xchg,
            _ => return Err(Unsupported), // LDAPR and the rest
        };
        let h = self.host_addr(rn);
        let v = self.x_sized(rs, size);
        let v = if (o3, opc) == (0, 1) {
            self.b.ins().bnot(v)
        } else {
            v
        };
        let old = self
            .b
            .ins()
            .atomic_rmw(Self::ity(size), MemFlagsData::new(), op, h, v);
        let old = self.widen(old);
        self.set(rt, old);
        Ok(())
    }

    fn ldst_pair(&mut self, w: u32) -> R {
        let (opc, mode, l) = (bits(w, 31, 30), bits(w, 24, 23), bits(w, 22, 22) != 0);
        let (rt, rn, rt2) = (bits(w, 4, 0), bits(w, 9, 5), bits(w, 14, 10));
        if opc == 3 || (opc == 1 && !l) {
            return Err(Unsupported); // STGP and reserved
        }
        let scale = if opc == 2 { 3 } else { 2 };
        let imm = sx(bits(w, 21, 15) as u64, 7) << scale;
        let base = self.xsp(rn, true);
        let (addr, wb) = match mode {
            0 | 2 => (self.b.ins().iadd_imm_s(base, imm), None),
            1 => {
                let nb = self.b.ins().iadd_imm_s(base, imm);
                (base, Some(nb))
            }
            _ => {
                let nb = self.b.ins().iadd_imm_s(base, imm);
                (nb, Some(nb))
            }
        };
        let step = 1i64 << scale;
        let addr2 = self.b.ins().iadd_imm_s(addr, step);
        if l {
            let sign = if opc == 1 { Some(types::I64) } else { None };
            let a = self.load_ext(scale, sign, addr);
            let b = self.load_ext(scale, sign, addr2);
            self.set(rt, a);
            self.set(rt2, b);
        } else {
            self.store_n(scale, rt, addr);
            self.store_n(scale, rt2, addr2);
        }
        if let Some(nb) = wb {
            self.setsp(rn, nb);
        }
        Ok(())
    }

    // ---- system --------------------------------------------------------------------------------------------------
    fn system(&mut self, w: u32) -> R {
        if w & 0xFFFF_F01F == 0xD503_201F {
            return Ok(()); // hints: NOP, YIELD, BTI, PAC*SP and the rest
        }
        if w & 0xFFFF_F0FF == 0xD503_305F {
            // CLREX
            let zero = self.b.ins().iconst(types::I32, 0);
            self.ctx_store(zero, ctx::EXCL_VALID);
            return Ok(());
        }
        if w & 0xFFFF_F0DF == 0xD503_309F {
            // DSB / DMB
            self.b.ins().fence();
            return Ok(());
        }
        let key = bits(w, 19, 5);
        let rt = bits(w, 4, 0);
        if w & 0xFFF0_0000 == 0xD530_0000 {
            // MRS
            let v = match key {
                0x5E82 => self.b.ins().load(
                    types::I64,
                    MemFlagsData::trusted(),
                    self.ctx,
                    ctx::TPIDR_EL0,
                ),
                0x5E83 => self.b.ins().load(
                    types::I64,
                    MemFlagsData::trusted(),
                    self.ctx,
                    ctx::TPIDRRO_EL0,
                ),
                0x5A10 => {
                    let mut acc = self.b.ins().iconst(types::I64, 0);
                    for (i, var) in [self.r.n, self.r.z, self.r.c, self.r.v]
                        .into_iter()
                        .enumerate()
                    {
                        let f = self.b.use_var(var);
                        let f = self.b.ins().uextend(types::I64, f);
                        let f = self.b.ins().ishl_imm_u(f, 31 - i as i64);
                        acc = self.b.ins().bor(acc, f);
                    }
                    acc
                }
                _ => return Err(Unsupported),
            };
            self.set(rt, v);
            return Ok(());
        }
        if w & 0xFFF0_0000 == 0xD510_0000 {
            // MSR (register)
            match key {
                0x5E82 => {
                    let v = self.x(rt, true);
                    self.b
                        .ins()
                        .store(MemFlagsData::trusted(), v, self.ctx, ctx::TPIDR_EL0);
                }
                0x5A10 => {
                    let v = self.x(rt, true);
                    for (i, var) in [self.r.n, self.r.z, self.r.c, self.r.v]
                        .into_iter()
                        .enumerate()
                    {
                        let f = self.b.ins().ushr_imm_u(v, 31 - i as i64);
                        let f = self.b.ins().band_imm_u(f, 1);
                        let f = self.b.ins().ireduce(types::I8, f);
                        self.mark(32);
                        self.b.def_var(var, f);
                    }
                }
                _ => return Err(Unsupported),
            }
            return Ok(());
        }
        Err(Unsupported)
    }
}
