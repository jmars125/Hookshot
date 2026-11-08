//! Floating point: SIMD&FP register loads and stores, and scalar FP data processing (S and D).
//!
//! The vector registers stay in the context (`vreg`, two u64 halves each). Results must be the Arm ARM's bit for bit,
//! FPSR included, as the C path's are (suyu's translator, its FPX1 scheme): FADD/FSUB/FMUL/FDIV/FSQRT/FMA compute
//! natively and keep the native result only when it provably equals the architectural one (the same gate and checks as
//! a64_suyu.h recomp_fpx_*); otherwise they call the exact C bodies `recomp_fpe_*` (a64_suyu.c). The rest is written
//! out here from the pseudocode. FPSR bits: IOC 0, DZC 1, OFC 2, UFC 3, IXC 4, IDC 7; FPCR.FZ is bit 24.

use super::super::ctx;
use super::{bits, Em, Unsupported, R};
use cranelift_codegen::ir::condcodes::{FloatCC, IntCC};
use cranelift_codegen::ir::{types, AbiParam, InstBuilder, MemFlagsData, Signature, Type, Value};

pub(super) const IOC: i64 = 1;
pub(super) const IXC: i64 = 16;
pub(super) const IDC: i64 = 128;
/// Every FPCR control bit but FZ, plus the host's inhibit bit 32 (a64_suyu.h RECOMP_FPX_FPCR_MASK)
const FPX_FPCR_MASK: i64 = 0x1_06C8_FF07;
pub(super) const FZ: i64 = 0x0100_0000;

/// (sign bit, exponent mask, smallest normal) of binary32 / binary64
pub(super) fn fmt(dbl: bool) -> (u64, u64, u64) {
    if dbl {
        (1 << 63, 0x7ff0_0000_0000_0000, 0x0010_0000_0000_0000)
    } else {
        (1 << 31, 0x7f80_0000, 0x0080_0000)
    }
}

impl Em<'_, '_> {
    pub(super) fn ity_of(dbl: bool) -> Type {
        if dbl {
            types::I64
        } else {
            types::I32
        }
    }
    pub(super) fn fty_of(dbl: bool) -> Type {
        if dbl {
            types::F64
        } else {
            types::F32
        }
    }

    // ---- vector registers in the context -------------------------------------------------------------------------
    pub(super) fn v_off(n: u32) -> i32 {
        ctx::VREG + 16 * n as i32
    }
    /// The low `ty` bits of Vn as an integer
    pub(super) fn vget(&mut self, n: u32, ty: Type) -> Value {
        self.b
            .ins()
            .load(ty, MemFlagsData::trusted(), self.ctx, Self::v_off(n))
    }
    pub(super) fn vget_hi(&mut self, n: u32) -> Value {
        self.b.ins().load(
            types::I64,
            MemFlagsData::trusted(),
            self.ctx,
            Self::v_off(n) + 8,
        )
    }
    /// Vn = the integer v zero-extended to 128 bits (a scalar write clears the rest of the register)
    pub(super) fn vset(&mut self, n: u32, v: Value) {
        let lo = self.widen(v);
        let z = self.b.ins().iconst(types::I64, 0);
        self.b
            .ins()
            .store(MemFlagsData::trusted(), lo, self.ctx, Self::v_off(n));
        self.b
            .ins()
            .store(MemFlagsData::trusted(), z, self.ctx, Self::v_off(n) + 8);
    }
    pub(super) fn vset128(&mut self, n: u32, lo: Value, hi: Value) {
        self.b
            .ins()
            .store(MemFlagsData::trusted(), lo, self.ctx, Self::v_off(n));
        self.b
            .ins()
            .store(MemFlagsData::trusted(), hi, self.ctx, Self::v_off(n) + 8);
    }

    pub(super) fn fpcr(&mut self) -> Value {
        self.b
            .ins()
            .load(types::I64, MemFlagsData::trusted(), self.ctx, ctx::FPCR)
    }
    /// FPSR |= bits where cond (i8) holds
    pub(super) fn fpsr_or(&mut self, cond: Value, bits: i64) {
        let s = self
            .b
            .ins()
            .load(types::I64, MemFlagsData::trusted(), self.ctx, ctx::FPSR);
        let k = self.b.ins().iconst(types::I64, bits);
        let z = self.b.ins().iconst(types::I64, 0);
        let add = self.b.ins().select(cond, k, z);
        let s = self.b.ins().bor(s, add);
        self.b
            .ins()
            .store(MemFlagsData::trusted(), s, self.ctx, ctx::FPSR);
    }
    /// FPCR.FZ, as an i8
    pub(super) fn fz(&mut self) -> Value {
        let c = self.fpcr();
        let f = self.b.ins().band_imm_s(c, FZ);
        self.b.ins().icmp_imm_s(IntCC::NotEqual, f, 0)
    }

    pub(super) fn abs_bits(&mut self, x: Value, dbl: bool) -> Value {
        let (sign, _, _) = fmt(dbl);
        let k = self.b.ins().iconst(Self::ity_of(dbl), !sign as i64);
        self.b.ins().band(x, k)
    }
    /// A nonzero subnormal (a64_suyu.h RECOMP_FPX_SUBN*)
    pub(super) fn subnormal(&mut self, x: Value, dbl: bool) -> Value {
        let (_, _, min) = fmt(dbl);
        let a = self.abs_bits(x, dbl);
        let a1 = self.b.ins().iadd_imm_s(a, -1);
        self.b
            .ins()
            .icmp_imm_u(IntCC::UnsignedLessThan, a1, (min - 1) as i64)
    }
    pub(super) fn is_zero(&mut self, x: Value, dbl: bool) -> Value {
        let a = self.abs_bits(x, dbl);
        self.b.ins().icmp_imm_s(IntCC::Equal, a, 0)
    }
    pub(super) fn finite(&mut self, x: Value, dbl: bool) -> Value {
        let (_, e, _) = fmt(dbl);
        let k = self.b.ins().iconst(Self::ity_of(dbl), e as i64);
        let m = self.b.ins().band(x, k);
        self.b.ins().icmp(IntCC::NotEqual, m, k)
    }
    /// Finite and larger in magnitude than the smallest normal (RECOMP_FPX_BIG*)
    pub(super) fn big(&mut self, x: Value, dbl: bool) -> Value {
        let (_, e, min) = fmt(dbl);
        let a = self.abs_bits(x, dbl);
        let d = self.b.ins().iadd_imm_s(a, -((min + 1) as i64));
        self.b
            .ins()
            .icmp_imm_u(IntCC::UnsignedLessThan, d, (e - min - 1) as i64)
    }
    /// Under FZ, a subnormal input is read as a zero of its sign and sets IDC
    pub(super) fn flush_in(&mut self, x: Value, dbl: bool, fz: Value) -> Value {
        let (sign, _, _) = fmt(dbl);
        let sub = self.subnormal(x, dbl);
        let flush = self.b.ins().band(sub, fz);
        self.fpsr_or(flush, IDC);
        let k = self.b.ins().iconst(Self::ity_of(dbl), sign as i64);
        let z = self.b.ins().band(x, k);
        self.b.ins().select(flush, z, x)
    }

    /// Calls a C helper by name: (ctx, args...) -> u64
    pub(super) fn call_c(&mut self, name: &str, args: &[Value]) -> Value {
        let mut sig = Signature::new(self.b.func.signature.call_conv);
        sig.params.push(AbiParam::new(types::I64));
        for _ in args {
            sig.params.push(AbiParam::new(types::I64));
        }
        sig.returns.push(AbiParam::new(types::I64));
        let f = self.m.func(name, &sig, self.b.func);
        let mut all = vec![self.ctx];
        for &a in args {
            all.push(self.widen(a));
        }
        let call = self.b.ins().call(f, &all);
        self.b.inst_results(call)[0]
    }

    /// `fast` when `keep` (i8) holds, else the exact C body `slow`(ctx, args)
    pub(super) fn fast_or_exact(
        &mut self,
        keep: Value,
        fast: Value,
        slow: &str,
        args: &[Value],
        dbl: bool,
    ) -> Value {
        let ty = Self::ity_of(dbl);
        let slow_b = self.b.create_block();
        let done = self.b.create_block();
        self.b.append_block_param(done, ty);
        self.b.ins().brif(keep, done, &[fast.into()], slow_b, &[]);
        self.b.switch_to_block(slow_b);
        self.b.seal_block(slow_b);
        let r = self.call_c(slow, args);
        let r = if dbl {
            r
        } else {
            self.b.ins().ireduce(types::I32, r)
        };
        self.b.ins().jump(done, &[r.into()]);
        self.b.switch_to_block(done);
        self.b.seal_block(done);
        self.b.block_params(done)[0]
    }

    /// RECOMP_FPX_OPEN: FPSR.IXC already set and FPCR at its default apart from FZ
    pub(super) fn fpx_open(&mut self) -> Value {
        let s = self
            .b
            .ins()
            .load(types::I64, MemFlagsData::trusted(), self.ctx, ctx::FPSR);
        let ixc = self.b.ins().band_imm_s(s, IXC);
        let ixc = self.b.ins().icmp_imm_s(IntCC::NotEqual, ixc, 0);
        let c = self.fpcr();
        let m = self.b.ins().band_imm_s(c, FPX_FPCR_MASK);
        let m = self.b.ins().icmp_imm_s(IntCC::Equal, m, 0);
        self.b.ins().band(ixc, m)
    }

    /// FADD, FSUB, FMUL (as FMA with a signed zero addend, like the C path), FDIV, FNMUL
    pub(super) fn arith2(
        &mut self,
        op: u32,
        a: Value,
        b: Value,
        dbl: bool,
    ) -> Result<Value, Unsupported> {
        let fty = Self::fty_of(dbl);
        let w = if dbl { "64" } else { "32" };
        let fa = self.b.ins().bitcast(fty, MemFlagsData::new(), a);
        let fb = self.b.ins().bitcast(fty, MemFlagsData::new(), b);
        let t = match op {
            0 | 8 => self.b.ins().fmul(fa, fb),
            1 => self.b.ins().fdiv(fa, fb),
            2 => self.b.ins().fadd(fa, fb),
            3 => self.b.ins().fsub(fa, fb),
            _ => return Err(Unsupported),
        };
        let t = self
            .b
            .ins()
            .bitcast(Self::ity_of(dbl), MemFlagsData::new(), t);
        let open = self.fpx_open();
        let fz = self.fz();
        let nofz = self.b.ins().bxor_imm_u(fz, 1);
        let sa = self.subnormal(a, dbl);
        let sb = self.subnormal(b, dbl);
        let any_sub = self.b.ins().bor(sa, sb);
        let fzok = self.b.ins().bxor_imm_u(any_sub, 1);
        let big = self.big(t, dbl);
        let fin = self.finite(t, dbl);
        let keep = match op {
            2 | 3 => {
                // a finite sum; under FZ only a large or exactly zero one
                let zt = self.is_zero(t, dbl);
                let bz = self.b.ins().bor(big, zt);
                let fzsum = self.b.ins().band(fzok, bz);
                let ok = self.b.ins().bor(nofz, fzsum);
                self.b.ins().band(fin, ok)
            }
            0 | 8 => {
                let za = self.is_zero(a, dbl);
                let zb = self.is_zero(b, dbl);
                let z = self.b.ins().bor(za, zb);
                let z = self.b.ins().band(z, fin);
                let k = self.b.ins().bor(big, z);
                let ok = self.b.ins().bor(nofz, fzok);
                self.b.ins().band(k, ok)
            }
            _ => {
                let za = self.is_zero(a, dbl);
                let z = self.b.ins().band(za, fin);
                let k = self.b.ins().bor(big, z);
                let ok = self.b.ins().bor(nofz, fzok);
                self.b.ins().band(k, ok)
            }
        };
        let keep = self.b.ins().band(open, keep);
        let (sign, _, _) = fmt(dbl);
        let r = match op {
            0 | 8 => {
                // the C path's FMUL is FMA(a, b, zero of the product's sign)
                let x = self.b.ins().bxor(a, b);
                let z = self.b.ins().band_imm_u(x, sign as i64);
                self.fast_or_exact(keep, t, &format!("recomp_fpe_fma{w}"), &[a, b, z], dbl)
            }
            1 => self.fast_or_exact(keep, t, &format!("recomp_fpe_div{w}"), &[a, b], dbl),
            2 => self.fast_or_exact(keep, t, &format!("recomp_fpe_add{w}"), &[a, b], dbl),
            _ => self.fast_or_exact(keep, t, &format!("recomp_fpe_sub{w}"), &[a, b], dbl),
        };
        Ok(if op == 8 {
            self.b.ins().bxor_imm_u(r, sign as i64)
        } else {
            r
        })
    }

    /// FCMP / FCMPE: NZCV from the comparison; IOC for a signalling NaN (any NaN for FCMPE)
    pub(super) fn fcmp(&mut self, a: Value, b: Value, dbl: bool, signal_all: bool) {
        let fz = self.fz();
        let a = self.flush_in(a, dbl, fz);
        let b = self.flush_in(b, dbl, fz);
        let fty = Self::fty_of(dbl);
        let fa = self.b.ins().bitcast(fty, MemFlagsData::new(), a);
        let fb = self.b.ins().bitcast(fty, MemFlagsData::new(), b);
        let uno = self.b.ins().fcmp(FloatCC::Unordered, fa, fb);
        let eq = self.b.ins().fcmp(FloatCC::Equal, fa, fb);
        let lt = self.b.ins().fcmp(FloatCC::LessThan, fa, fb);
        // N = lt, Z = eq, C = !lt (or unordered), V = unordered
        let nlt = self.b.ins().bxor_imm_u(lt, 1);
        self.b.def_var(self.r.n, lt);
        self.b.def_var(self.r.z, eq);
        self.b.def_var(self.r.c, nlt);
        self.b.def_var(self.r.v, uno);
        let ioc = if signal_all {
            uno
        } else {
            // a signalling NaN: exponent all ones, quiet bit clear, fraction nonzero
            let (_, e, _) = fmt(dbl);
            let q = if dbl { 1u64 << 51 } else { 1 << 22 };
            let mut any = self.b.ins().iconst(types::I8, 0);
            for x in [a, b] {
                let ab = self.abs_bits(x, dbl);
                let nan = self
                    .b
                    .ins()
                    .icmp_imm_u(IntCC::UnsignedGreaterThan, ab, e as i64);
                let qb = self.b.ins().band_imm_u(x, q as i64);
                let quiet = self.b.ins().icmp_imm_s(IntCC::NotEqual, qb, 0);
                let nq = self.b.ins().bxor_imm_u(quiet, 1);
                let s = self.b.ins().band(nan, nq);
                any = self.b.ins().bor(any, s);
            }
            any
        };
        self.fpsr_or(ioc, IOC);
    }

    /// FCVTZS / FCVTZU: round toward zero, saturating; NaN -> 0. IOC when NaN or out of range, IXC when inexact
    pub(super) fn fcvtz(&mut self, x: Value, dbl: bool, to64: bool, signed: bool) -> Value {
        let fz = self.fz();
        let x = self.flush_in(x, dbl, fz);
        self.fcvtz_noflush(x, dbl, to64, signed)
    }
    /// fcvtz on an input FZ has already been applied to
    pub(super) fn fcvtz_noflush(&mut self, x: Value, dbl: bool, to64: bool, signed: bool) -> Value {
        let fty = Self::fty_of(dbl);
        let ity = if to64 { types::I64 } else { types::I32 };
        let f = self.b.ins().bitcast(fty, MemFlagsData::new(), x);
        let r = if signed {
            self.b.ins().fcvt_to_sint_sat(ity, f)
        } else {
            self.b.ins().fcvt_to_uint_sat(ity, f)
        };
        let t = self.b.ins().trunc(f);
        let n = if to64 { 64 } else { 32 };
        let (lo, hi) = if signed {
            (-(2f64.powi(n - 1)), 2f64.powi(n - 1))
        } else {
            (0.0, 2f64.powi(n))
        };
        let k = |s: &mut Self, v: f64| {
            if dbl {
                s.b.ins().f64const(v)
            } else {
                s.b.ins().f32const(v as f32)
            }
        };
        let klo = k(self, lo);
        let khi = k(self, hi);
        let nan = self.b.ins().fcmp(FloatCC::Unordered, f, f);
        let under = self.b.ins().fcmp(FloatCC::LessThan, t, klo);
        let over = self.b.ins().fcmp(FloatCC::GreaterThanOrEqual, t, khi);
        let ioc = self.b.ins().bor(nan, under);
        let ioc = self.b.ins().bor(ioc, over);
        let ne = self.b.ins().fcmp(FloatCC::NotEqual, t, f);
        let nioc = self.b.ins().bxor_imm_u(ioc, 1);
        let ixc = self.b.ins().band(ne, nioc);
        self.fpsr_or(ioc, IOC);
        self.fpsr_or(ixc, IXC);
        r
    }

    /// SCVTF / UCVTF (integer) from a 32- or 64-bit register. Natively under round-to-nearest (IXC when the result is
    /// not the integer: it does not convert back to itself, or it is one of the largest integers, which round up to
    /// 2^n and saturate back); any other FPCR.RMode goes to the exact body hsr_fixed_to_fp (rt.rs)
    pub(super) fn cvtf(&mut self, x: Value, dbl: bool, signed: bool) -> Value {
        let fty = Self::fty_of(dbl);
        let ity = Self::ity_of(dbl);
        let xt = self.b.func.dfg.value_type(x);
        let c = self.fpcr();
        let rm = self.b.ins().band_imm_s(c, 3 << 22);
        let rn = self.b.ins().icmp_imm_s(IntCC::Equal, rm, 0);
        let fast = self.b.create_block();
        let slow = self.b.create_block();
        let done = self.b.create_block();
        self.b.append_block_param(done, ity);
        self.b.ins().brif(rn, fast, &[], slow, &[]);
        self.b.switch_to_block(fast);
        self.b.seal_block(fast);
        let f = if signed {
            self.b.ins().fcvt_from_sint(fty, x)
        } else {
            self.b.ins().fcvt_from_uint(fty, x)
        };
        if !(xt == types::I32 && dbl) {
            let back = if signed {
                self.b.ins().fcvt_to_sint_sat(xt, f)
            } else {
                self.b.ins().fcvt_to_uint_sat(xt, f)
            };
            let ne = self.b.ins().icmp(IntCC::NotEqual, back, x);
            let max = if signed {
                (ones_of(xt) >> 1) as i64
            } else {
                ones_of(xt) as i64
            };
            let maxk = self.b.ins().iconst(
                xt,
                if xt == types::I32 {
                    max as u32 as i32 as i64
                } else {
                    max
                },
            );
            let at_max = self.b.ins().icmp(IntCC::Equal, x, maxk);
            let ixc = self.b.ins().bor(ne, at_max);
            self.fpsr_or(ixc, IXC);
        }
        let r = self.b.ins().bitcast(ity, MemFlagsData::new(), f);
        self.b.ins().jump(done, &[r.into()]);
        self.b.switch_to_block(slow);
        self.b.seal_block(slow);
        let xb = if xt == types::I32 { 32 } else { 64 };
        let packed = (if dbl { 64 } else { 32 }) | xb << 8 | (signed as i64) << 16;
        let p = self.b.ins().iconst(types::I64, packed);
        let r = self.call_c("hsr_fixed_to_fp", &[x, p]);
        let r = if dbl {
            r
        } else {
            self.b.ins().ireduce(types::I32, r)
        };
        self.b.ins().jump(done, &[r.into()]);
        self.b.switch_to_block(done);
        self.b.seal_block(done);
        self.b.block_params(done)[0]
    }

    /// FCVT Dd, Sn: exact in every rounding mode. A NaN keeps its payload and is quietened (IOC if it was signalling;
    /// the default NaN under DN); under FZ a subnormal input becomes a zero of its sign, with IDC
    pub(super) fn fcvt_sd(&mut self, a: Value) -> Value {
        let c = self.fpcr();
        let fz = self.fz();
        let f = self.b.ins().bitcast(types::F32, MemFlagsData::new(), a);
        let d = self.b.ins().fpromote(types::F64, f);
        let d = self.b.ins().bitcast(types::I64, MemFlagsData::new(), d);
        let a64 = self.b.ins().uextend(types::I64, a);
        // NaN: sign, all-ones exponent, quiet bit, the 22 low fraction bits moved up
        let abs = self.abs_bits(a, false);
        let nan = self
            .b
            .ins()
            .icmp_imm_u(IntCC::UnsignedGreaterThan, abs, 0x7f80_0000);
        let q = self.b.ins().band_imm_u(a, 1 << 22);
        let quiet = self.b.ins().icmp_imm_s(IntCC::NotEqual, q, 0);
        let nq = self.b.ins().bxor_imm_u(quiet, 1);
        let snan = self.b.ins().band(nan, nq);
        self.fpsr_or(snan, IOC);
        let sgn = self.b.ins().band_imm_u(a64, 1 << 31);
        let sgn = self.b.ins().ishl_imm_u(sgn, 32);
        let pay = self.b.ins().band_imm_u(a64, 0x3f_ffff);
        let pay = self.b.ins().ishl_imm_u(pay, 29);
        let qn = self.b.ins().bor(sgn, pay);
        let qn = self.b.ins().bor_imm_u(qn, 0x7ff8_0000_0000_0000);
        let dn = self.b.ins().band_imm_s(c, 1 << 25);
        let dn = self.b.ins().icmp_imm_s(IntCC::NotEqual, dn, 0);
        let def = self.b.ins().iconst(types::I64, 0x7ff8_0000_0000_0000);
        let qn = self.b.ins().select(dn, def, qn);
        let r = self.b.ins().select(nan, qn, d);
        let sub = self.subnormal(a, false);
        let flush = self.b.ins().band(sub, fz);
        self.fpsr_or(flush, IDC);
        self.b.ins().select(flush, sgn, r)
    }

    /// FCVT Sd, Dn: native when the FPX gate is open and the result is a large normal (rounding to nearest, IXC
    /// already set) or the input is a zero or an infinity (exact either way); otherwise hsr_fcvt_ds (rt.rs)
    pub(super) fn fcvt_ds(&mut self, a: Value) -> Value {
        let open = self.fpx_open();
        let f = self.b.ins().bitcast(types::F64, MemFlagsData::new(), a);
        let s = self.b.ins().fdemote(types::F32, f);
        let t = self.b.ins().bitcast(types::I32, MemFlagsData::new(), s);
        let big = self.big(t, false);
        let abs = self.abs_bits(a, true);
        let zero = self.b.ins().icmp_imm_s(IntCC::Equal, abs, 0);
        let inf = self
            .b
            .ins()
            .icmp_imm_s(IntCC::Equal, abs, 0x7ff0_0000_0000_0000);
        let exact = self.b.ins().bor(zero, inf);
        let keep = self.b.ins().band(open, big);
        let keep = self.b.ins().bor(keep, exact);
        let slow = self.b.create_block();
        let done = self.b.create_block();
        self.b.append_block_param(done, types::I32);
        self.b.ins().brif(keep, done, &[t.into()], slow, &[]);
        self.b.switch_to_block(slow);
        self.b.seal_block(slow);
        let r = self.call_c("hsr_fcvt_ds", &[a]);
        let r = self.b.ins().ireduce(types::I32, r);
        self.b.ins().jump(done, &[r.into()]);
        self.b.switch_to_block(done);
        self.b.seal_block(done);
        self.b.block_params(done)[0]
    }

    /// FSQRT: a64_suyu.h recomp_fpx_sqrt*'s fast cases (a zero is its own root; a positive finite input, not flushed
    /// by FZ, has a normal root, inexact or not, which IXC already covers), else the exact body
    pub(super) fn fsqrt(&mut self, a: Value, dbl: bool) -> Value {
        let ity = Self::ity_of(dbl);
        let open = self.fpx_open();
        let fz = self.fz();
        let f = self
            .b
            .ins()
            .bitcast(Self::fty_of(dbl), MemFlagsData::new(), a);
        let s = self.b.ins().sqrt(f);
        let t = self.b.ins().bitcast(ity, MemFlagsData::new(), s);
        let za = self.is_zero(a, dbl);
        let (_, e, _) = fmt(dbl);
        // positive, finite, nonzero: (a - 1) < largest finite
        let a1 = self.b.ins().iadd_imm_s(a, -1);
        let pos = self
            .b
            .ins()
            .icmp_imm_u(IntCC::UnsignedLessThan, a1, (e - 1) as i64);
        let sub = self.subnormal(a, dbl);
        let bad = self.b.ins().band(fz, sub);
        let nbad = self.b.ins().bxor_imm_u(bad, 1);
        let ok = self.b.ins().band(pos, nbad);
        let ok = self.b.ins().band(ok, open);
        let zk = self.b.ins().band(za, open);
        let t = self.b.ins().select(za, a, t);
        let keep = self.b.ins().bor(ok, zk);
        self.fast_or_exact(
            keep,
            t,
            &format!("recomp_fpe_sqrt{}", if dbl { 64 } else { 32 }),
            &[a],
            dbl,
        )
    }

    /// VFPExpandImm
    pub(super) fn expand_imm(imm8: u32, dbl: bool) -> u64 {
        let sign = (imm8 >> 7) as u64;
        let b6 = ((imm8 >> 6) & 1) as u64;
        let frac = (imm8 & 0xf) as u64;
        let e_low = ((imm8 >> 4) & 3) as u64;
        if dbl {
            let exp = ((1 - b6) << 10) | (if b6 != 0 { 0xff << 2 } else { 0 }) | e_low;
            (sign << 63) | (exp << 52) | (frac << 48)
        } else {
            let exp = ((1 - b6) << 7) | (if b6 != 0 { 0x1f << 2 } else { 0 }) | e_low;
            (sign << 31) | (exp << 23) | (frac << 19)
        }
    }

    // ---- loads and stores of SIMD&FP registers -------------------------------------------------------------------
    /// Size in bytes as a power of two (0 B .. 4 Q) of a single-register access
    pub(super) fn vsize(w: u32) -> Option<u32> {
        let (size, opc1) = (bits(w, 31, 30), bits(w, 23, 23));
        match (opc1, size) {
            (0, s) => Some(s),
            (1, 0) => Some(4),
            _ => None,
        }
    }
    pub(super) fn vload(&mut self, rt: u32, scale: u32, addr: Value) {
        let h = self.b.ins().iadd_imm_s(addr, ctx::MEM_BASE);
        if scale == 4 {
            let lo = self.b.ins().load(types::I64, MemFlagsData::new(), h, 0);
            let hi = self.b.ins().load(types::I64, MemFlagsData::new(), h, 8);
            self.vset128(rt, lo, hi);
        } else {
            let t = [types::I8, types::I16, types::I32, types::I64][scale as usize];
            let v = self.b.ins().load(t, MemFlagsData::new(), h, 0);
            let v = if t == types::I64 {
                v
            } else {
                self.b.ins().uextend(types::I64, v)
            };
            self.vset(rt, v);
        }
    }
    pub(super) fn vstore(&mut self, rt: u32, scale: u32, addr: Value) {
        let h = self.b.ins().iadd_imm_s(addr, ctx::MEM_BASE);
        if scale == 4 {
            let lo = self.vget(rt, types::I64);
            let hi = self.vget_hi(rt);
            self.b.ins().store(MemFlagsData::new(), lo, h, 0);
            self.b.ins().store(MemFlagsData::new(), hi, h, 8);
        } else {
            let t = [types::I8, types::I16, types::I32, types::I64][scale as usize];
            let v = self.vget(rt, t);
            self.b.ins().store(MemFlagsData::new(), v, h, 0);
        }
    }

    /// LDR/STR (immediate, unscaled, pre/post-indexed, register offset), LDP/STP and LDR (literal) of B/H/S/D/Q
    pub(super) fn ldst_simd(&mut self, w: u32) -> R {
        if w & 0xBE00_0000 == 0x0C00_0000 {
            return self.ldst_struct(w); // 0 Q 00110 x x L ...: single and multiple structures
        }
        let (rt, rn) = (bits(w, 4, 0), bits(w, 9, 5));
        if w & 0x3B00_0000 == 0x1800_0000 {
            let scale = match bits(w, 31, 30) {
                0 => 2,
                1 => 3,
                2 => 4,
                _ => return Err(Unsupported),
            };
            let a = self
                .pc
                .wrapping_add((super::sx(bits(w, 23, 5) as u64, 19) * 4) as u64);
            let a = self.b.ins().iconst(types::I64, a as i64);
            self.vload(rt, scale, a);
            return Ok(());
        }
        if w & 0x3A00_0000 == 0x2800_0000 {
            let (opc, mode, l, rt2) = (
                bits(w, 31, 30),
                bits(w, 24, 23),
                bits(w, 22, 22) != 0,
                bits(w, 14, 10),
            );
            if opc == 3 || (l && rt == rt2) {
                return Err(Unsupported);
            }
            let scale = 2 + opc;
            let imm = super::sx(bits(w, 21, 15) as u64, 7) << scale;
            let base = self.xsp(rn, true);
            let nb = self.b.ins().iadd_imm_s(base, imm);
            let addr = if mode == 1 { base } else { nb };
            let a2 = self.b.ins().iadd_imm_s(addr, 1 << scale);
            if l {
                self.vload(rt, scale, addr);
                self.vload(rt2, scale, a2);
            } else {
                self.vstore(rt, scale, addr);
                self.vstore(rt2, scale, a2);
            }
            if mode == 1 || mode == 3 {
                self.setsp(rn, nb);
            }
            return Ok(());
        }
        let scale = Self::vsize(w).ok_or(Unsupported)?;
        let load = bits(w, 22, 22) != 0;
        let base = self.xsp(rn, true);
        let (addr, wb) = if w & 0x3B00_0000 == 0x3900_0000 {
            (
                self.b
                    .ins()
                    .iadd_imm_s(base, ((bits(w, 21, 10) as u64) << scale) as i64),
                None,
            )
        } else if w & 0x3B20_0000 == 0x3800_0000 {
            let imm = super::sx(bits(w, 20, 12) as u64, 9);
            let nb = self.b.ins().iadd_imm_s(base, imm);
            match bits(w, 11, 10) {
                0 => (nb, None),
                1 => (base, Some(nb)),
                3 => (nb, Some(nb)),
                _ => return Err(Unsupported),
            }
        } else if w & 0x3B20_0C00 == 0x3820_0800 {
            let option = bits(w, 15, 13);
            if option & 2 == 0 {
                return Err(Unsupported);
            }
            let sh = if bits(w, 12, 12) != 0 { scale } else { 0 };
            let off = self.extend(bits(w, 20, 16), option, sh, true);
            (self.b.ins().iadd(base, off), None)
        } else {
            return Err(Unsupported);
        };
        if load {
            self.vload(rt, scale, addr);
        } else {
            self.vstore(rt, scale, addr);
        }
        if let Some(nb) = wb {
            self.setsp(rn, nb);
        }
        Ok(())
    }

    // ---- scalar floating point -----------------------------------------------------------------------------------
    pub(super) fn fp_scalar(&mut self, w: u32) -> R {
        if bits(w, 28, 28) == 0 {
            return self.simd_vector(w);
        }
        if bits(w, 31, 30) == 1 {
            return self.simd_scalar(w);
        }
        let (rd, rn, rm) = (bits(w, 4, 0), bits(w, 9, 5), bits(w, 20, 16));
        let ftype = bits(w, 23, 22);
        if w & 0x5F00_0000 != 0x1E00_0000 && w & 0x5F00_0000 != 0x1F00_0000 {
            return Err(Unsupported);
        }
        let dbl = match ftype {
            0 => false,
            1 => true,
            _ => {
                // FMOV Xd, Vn.D[1] / Vd.D[1], Xn
                if w & 0xFFFE_FC00 == 0x9EAE_0000 {
                    if bits(w, 16, 16) == 0 {
                        let v = self.vget_hi(rn);
                        self.set(rd, v);
                    } else {
                        let v = self.x(rn, true);
                        self.b.ins().store(
                            MemFlagsData::trusted(),
                            v,
                            self.ctx,
                            Self::v_off(rd) + 8,
                        );
                    }
                    return Ok(());
                }
                return Err(Unsupported);
            }
        };
        let ity = Self::ity_of(dbl);
        if w & 0x1F00_0000 == 0x1F00_0000 {
            // FMADD, FMSUB, FNMADD, FNMSUB: exact body (recomp_fpe_fma), operands negated as the pseudocode does
            if bits(w, 31, 29) != 0 {
                return Err(Unsupported);
            }
            let (o1, o0, ra) = (bits(w, 21, 21), bits(w, 15, 15), bits(w, 14, 10));
            let (sign, _, _) = fmt(dbl);
            let a = self.vget(ra, ity);
            let n = self.vget(rn, ity);
            let m = self.vget(rm, ity);
            let n = if o1 != o0 {
                self.b.ins().bxor_imm_u(n, sign as i64)
            } else {
                n
            };
            let a = if o1 != 0 {
                self.b.ins().bxor_imm_u(a, sign as i64)
            } else {
                a
            };
            let r = self.call_c(
                &format!("recomp_fpe_fma{}", if dbl { 64 } else { 32 }),
                &[n, m, a],
            );
            self.vset(rd, r);
            return Ok(());
        }
        if bits(w, 31, 31) == 0 && bits(w, 29, 29) == 0 && bits(w, 21, 21) == 1 {
            match bits(w, 11, 10) {
                0b10 if bits(w, 15, 15) == 0 || bits(w, 15, 12) == 8 => {
                    // 2-source: FMUL FDIV FADD FSUB FNMUL (min/max not yet)
                    let a = self.vget(rn, ity);
                    let b = self.vget(rm, ity);
                    let r = self.arith2(bits(w, 15, 12), a, b, dbl)?;
                    self.vset(rd, r);
                    return Ok(());
                }
                0b11 => {
                    // FCSEL
                    let a = self.vget(rn, ity);
                    let b = self.vget(rm, ity);
                    let c = self.cond(bits(w, 15, 12));
                    let r = self.b.ins().select(c, a, b);
                    self.vset(rd, r);
                    return Ok(());
                }
                0b00 if bits(w, 13, 10) == 0b1000 && bits(w, 2, 0) == 0 => {
                    // FCMP / FCMPE (bit 4), against zero (bit 3)
                    let a = self.vget(rn, ity);
                    let b = if bits(w, 3, 3) != 0 {
                        self.b.ins().iconst(ity, 0)
                    } else {
                        self.vget(rm, ity)
                    };
                    self.fcmp(a, b, dbl, bits(w, 4, 4) != 0);
                    return Ok(());
                }
                0b00 if bits(w, 12, 10) == 0b100 && bits(w, 9, 5) == 0 => {
                    // FMOV (scalar, immediate)
                    let v = Self::expand_imm(bits(w, 20, 13), dbl);
                    let v = self.b.ins().iconst(
                        ity,
                        if dbl {
                            v as i64
                        } else {
                            v as u32 as i32 as i64
                        },
                    );
                    self.vset(rd, v);
                    return Ok(());
                }
                0b00 if bits(w, 14, 10) == 0b10000 => {
                    // 1-source
                    let (sign, _, _) = fmt(dbl);
                    let a = self.vget(rn, ity);
                    match (bits(w, 20, 15), dbl) {
                        (5, false) => {
                            let r = self.fcvt_sd(a);
                            self.vset(rd, r);
                            return Ok(());
                        }
                        (4, true) => {
                            let r = self.fcvt_ds(a);
                            self.vset(rd, r);
                            return Ok(());
                        }
                        _ => {}
                    }
                    let r = match bits(w, 20, 15) {
                        0 => a,
                        1 => self.abs_bits(a, dbl),
                        2 => self.b.ins().bxor_imm_u(a, sign as i64),
                        3 => self.fsqrt(a, dbl),
                        _ => return Err(Unsupported),
                    };
                    self.vset(rd, r);
                    return Ok(());
                }
                _ => {}
            }
        }
        if w & 0x5F20_FC00 == 0x1E20_0000 {
            // conversions between floating point and integer
            let sf = bits(w, 31, 31) != 0;
            let (rmode, opc) = (bits(w, 20, 19), bits(w, 18, 16));
            match (rmode, opc) {
                (0, 6) if sf == dbl => {
                    let v = self.vget(rn, Self::ity_of(sf));
                    self.set(rd, v);
                }
                (0, 7) if sf == dbl => {
                    let v = self.x(rn, sf);
                    self.vset(rd, v);
                }
                (0, 2) | (0, 3) => {
                    let x = self.x(rn, sf);
                    let r = self.cvtf(x, dbl, opc == 2);
                    self.vset(rd, r);
                }
                (3, 0) | (3, 1) => {
                    let a = self.vget(rn, ity);
                    let r = self.fcvtz(a, dbl, sf, opc == 0);
                    self.set(rd, r);
                }
                _ => return Err(Unsupported),
            }
            return Ok(());
        }
        Err(Unsupported)
    }
}

fn ones_of(t: Type) -> u64 {
    if t == types::I64 {
        u64::MAX
    } else {
        u32::MAX as u64
    }
}
