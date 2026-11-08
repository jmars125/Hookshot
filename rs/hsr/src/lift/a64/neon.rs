//! Advanced SIMD (NEON): vector and SIMD-scalar data processing, and the element and structure loads and stores.
//!
//! Vectors are Cranelift SIMD values, loaded from and stored to the context's V registers. A 64-bit (Q = 0) result
//! clears the register's upper half; a SIMD-scalar result clears everything above its element. Floating point is
//! exact as in fp.rs: each operation runs natively on the whole vector when every lane passes that operation's FPX
//! keep checks (and the gate is open), and otherwise lane by lane through the scalar paths, which fall back to the
//! exact bodies per lane.

use super::fp::{fmt, IXC};
use super::{bits, Em, Unsupported, R};
use crate::lift::ctx;
use cranelift_codegen::ir::condcodes::{FloatCC, IntCC};
use cranelift_codegen::ir::{
    types, ConstantData, Endianness, InstBuilder, MemFlagsData, Type, Value,
};

fn vflags() -> MemFlagsData {
    let mut f = MemFlagsData::new();
    f.set_endianness(Endianness::Little);
    f
}

/// The integer vector type of `es`-bit elements
fn vt(es: u32) -> Type {
    match es {
        8 => types::I8X16,
        16 => types::I16X8,
        32 => types::I32X4,
        _ => types::I64X2,
    }
}
fn lt(es: u32) -> Type {
    match es {
        8 => types::I8,
        16 => types::I16,
        32 => types::I32,
        _ => types::I64,
    }
}
fn fvt(dbl: bool) -> Type {
    if dbl {
        types::F64X2
    } else {
        types::F32X4
    }
}
/// Elements an operation produces: those of 64 or 128 bits, or the one of a SIMD-scalar form
fn count(q: bool, es: u32, scalar: bool) -> u32 {
    if scalar {
        1
    } else {
        (if q { 128 } else { 64 }) / es
    }
}
fn ones(n: u32) -> u64 {
    if n >= 64 {
        u64::MAX
    } else {
        (1 << n) - 1
    }
}

/// AdvSIMDExpandImm; None for the unallocated op = 1, cmode = 1111 form when not 2D (`q`)
fn expand_imm(op: u32, cmode: u32, imm8: u32, q: bool) -> Option<u64> {
    let i = imm8 as u64;
    let rep2 = |x: u64| x | x << 32;
    let rep4 = |x: u64| x | x << 16 | x << 32 | x << 48;
    Some(match cmode >> 1 {
        0 => rep2(i),
        1 => rep2(i << 8),
        2 => rep2(i << 16),
        3 => rep2(i << 24),
        4 => rep4(i),
        5 => rep4(i << 8),
        6 => {
            if cmode & 1 == 0 {
                rep2(i << 8 | 0xff)
            } else {
                rep2(i << 16 | 0xffff)
            }
        }
        _ => match (cmode & 1, op) {
            (0, 0) => i * 0x0101_0101_0101_0101,
            (0, _) => (0..8)
                .map(|b| {
                    if i >> b & 1 != 0 {
                        0xffu64 << (8 * b)
                    } else {
                        0
                    }
                })
                .sum(),
            (_, 0) => {
                let b6 = (i >> 6) & 1;
                let s = (i >> 7) << 31
                    | (b6 ^ 1) << 30
                    | (if b6 != 0 { 0x1f } else { 0 }) << 25
                    | (i & 0x3f) << 19;
                rep2(s)
            }
            _ => {
                if !q {
                    return None;
                }
                let b6 = (i >> 6) & 1;
                (i >> 7) << 63
                    | (b6 ^ 1) << 62
                    | (if b6 != 0 { 0xff } else { 0 }) << 54
                    | (i & 0x3f) << 48
            }
        },
    })
}

impl Em<'_, '_> {
    // ---- vector registers ----------------------------------------------------------------------------------------
    fn qget(&mut self, n: u32, ty: Type) -> Value {
        self.b.ins().load(ty, vflags(), self.ctx, Self::v_off(n))
    }
    fn cast(&mut self, ty: Type, v: Value) -> Value {
        if self.b.func.dfg.value_type(v) == ty {
            v
        } else {
            self.b.ins().bitcast(ty, vflags(), v)
        }
    }
    /// Vd = v: all 128 bits (q), the low 64 with the rest cleared, or (scalar) element 0 with the rest cleared
    fn put(&mut self, rd: u32, v: Value, q: bool, scalar: bool, es: u32) {
        if scalar {
            let v = self.cast(vt(es), v);
            let e = self.b.ins().extractlane(v, 0);
            self.vset(rd, e);
        } else if q {
            let v = self.cast(types::I8X16, v);
            self.b.ins().store(vflags(), v, self.ctx, Self::v_off(rd));
        } else {
            let v = self.cast(types::I64X2, v);
            let lo = self.b.ins().extractlane(v, 0);
            self.vset(rd, lo);
        }
    }
    fn elem(&mut self, n: u32, es: u32, idx: u32) -> Value {
        self.b.ins().load(
            lt(es),
            MemFlagsData::trusted(),
            self.ctx,
            Self::v_off(n) + (idx * es / 8) as i32,
        )
    }
    fn set_elem(&mut self, d: u32, es: u32, idx: u32, v: Value) {
        self.b.ins().store(
            MemFlagsData::trusted(),
            v,
            self.ctx,
            Self::v_off(d) + (idx * es / 8) as i32,
        );
    }
    fn vbytes(&mut self, ty: Type, bytes: [u8; 16]) -> Value {
        let c = self
            .b
            .func
            .dfg
            .constants
            .insert(ConstantData::from(&bytes[..]));
        let v = self.b.ins().vconst(types::I8X16, c);
        self.cast(ty, v)
    }
    /// A vector of `ty` with every lane `c`
    fn vk(&mut self, ty: Type, c: u64) -> Value {
        let es = ty.lane_bits();
        let mut b = [0u8; 16];
        for (i, x) in b.iter_mut().enumerate() {
            *x = (c >> (8 * (i as u32 % (es / 8)))) as u8;
        }
        self.vbytes(ty, b)
    }
    /// All ones in the lanes from `lanes` on: or-ed into a lane mask, the lanes an operation does not produce pass
    fn unused(&mut self, ty: Type, lanes: u32) -> Option<Value> {
        let es = ty.lane_bits();
        if lanes * es >= 128 {
            return None;
        }
        let mut b = [0u8; 16];
        for x in b.iter_mut().skip((lanes * es / 8) as usize) {
            *x = 0xff;
        }
        Some(self.vbytes(ty, b))
    }
    /// Every produced lane of the mask set, as an i8
    fn all_lanes(&mut self, m: Value, lanes: u32) -> Value {
        let ty = self.b.func.dfg.value_type(m);
        let m = match self.unused(ty, lanes) {
            Some(u) => self.b.ins().bor(m, u),
            None => m,
        };
        self.b.ins().vall_true(m)
    }
    /// Any produced lane of the mask set, as an i8
    fn any_lane(&mut self, m: Value, lanes: u32) -> Value {
        let ty = self.b.func.dfg.value_type(m);
        let m = match self.unused(ty, lanes) {
            Some(u) => self.b.ins().band_not(m, u),
            None => m,
        };
        self.b.ins().vany_true(m)
    }
    /// A lane mask of the scalar condition c (i8)
    fn vcond(&mut self, ty: Type, c: Value) -> Value {
        let m = self.b.ins().bmask(ty.lane_type(), c);
        self.b.ins().splat(ty, m)
    }
    fn shuf(&mut self, a: Value, b: Value, mask: [u8; 16]) -> Value {
        let a = self.cast(types::I8X16, a);
        let b = self.cast(types::I8X16, b);
        let imm = self
            .b
            .func
            .dfg
            .immediates
            .push(ConstantData::from(&mask[..]));
        self.b.ins().shuffle(a, b, imm)
    }
    /// Element `src[i]` of (a ++ b) in each lane i; a's elements are 0 .. 128 / es, b's follow
    fn gather(&mut self, a: Value, b: Value, es: u32, src: &[u32]) -> Value {
        let eb = es / 8;
        let n = 16 / eb;
        let mut m = [0u8; 16];
        for (i, &s) in src.iter().enumerate() {
            let base = if s < n { s * eb } else { 16 + (s - n) * eb };
            for j in 0..eb {
                m[(i as u32 * eb + j) as usize] = (base + j) as u8;
            }
        }
        self.shuf(a, b, m)
    }
    /// The pairwise operands of ADDP-style operations over Vn:Vm: even and odd elements of the concatenation of the
    /// produced halves
    fn pairs(&mut self, a: Value, b: Value, es: u32, q: bool) -> (Value, Value) {
        let l = count(q, es, false);
        let n = 128 / es;
        let idx = |j: u32| if j < l { j } else { n + j - l };
        let ev: Vec<u32> = (0..l).map(|i| idx(2 * i)).collect();
        let od: Vec<u32> = (0..l).map(|i| idx(2 * i + 1)).collect();
        (self.gather(a, b, es, &ev), self.gather(a, b, es, &od))
    }
    /// Applies f to each produced lane of a (and b), lane by lane, into a vector of `out` lanes
    fn map_lanes(
        &mut self,
        a: Value,
        b: Option<Value>,
        c: Option<Value>,
        out: Type,
        lanes: u32,
        f: &mut dyn FnMut(&mut Self, Value, Option<Value>, Option<Value>) -> Value,
    ) -> Value {
        let mut r = self.vk(out, 0);
        for i in 0..lanes {
            let ai = self.b.ins().extractlane(a, i as u8);
            let bi = b.map(|b| self.b.ins().extractlane(b, i as u8));
            let ci = c.map(|c| self.b.ins().extractlane(c, i as u8));
            let ri = f(self, ai, bi, ci);
            r = self.b.ins().insertlane(r, ri, i as u8);
        }
        r
    }
    /// `fast` when keep (i8), else the lane-by-lane `slow`
    fn vbranch(
        &mut self,
        keep: Value,
        fast: &mut dyn FnMut(&mut Self) -> Value,
        slow: &mut dyn FnMut(&mut Self) -> Value,
        ty: Type,
    ) -> Value {
        let fb = self.b.create_block();
        let sb = self.b.create_block();
        let done = self.b.create_block();
        self.b.append_block_param(done, ty);
        self.b.ins().brif(keep, fb, &[], sb, &[]);
        self.b.switch_to_block(fb);
        self.b.seal_block(fb);
        let r = fast(self);
        let r = self.cast(ty, r);
        self.b.ins().jump(done, &[r.into()]);
        self.b.switch_to_block(sb);
        self.b.seal_block(sb);
        let r = slow(self);
        let r = self.cast(ty, r);
        self.b.ins().jump(done, &[r.into()]);
        self.b.switch_to_block(done);
        self.b.seal_block(done);
        self.b.block_params(done)[0]
    }

    // ---- floating-point lane predicates (a64_suyu.h RECOMP_FPX_*), on integer vectors ----------------------------
    fn v_abs(&mut self, x: Value, dbl: bool) -> Value {
        let (sign, _, _) = fmt(dbl);
        let ty = self.b.func.dfg.value_type(x);
        let k = self.vk(ty, !sign);
        self.b.ins().band(x, k)
    }
    fn v_ult(&mut self, x: Value, k: u64) -> Value {
        let ty = self.b.func.dfg.value_type(x);
        let k = self.vk(ty, k);
        self.b.ins().icmp(IntCC::UnsignedLessThan, x, k)
    }
    fn v_big(&mut self, t: Value, dbl: bool) -> Value {
        let (_, e, min) = fmt(dbl);
        let a = self.v_abs(t, dbl);
        let ty = self.b.func.dfg.value_type(t);
        let k = self.vk(ty, min + 1);
        let d = self.b.ins().isub(a, k);
        self.v_ult(d, e - min - 1)
    }
    fn v_fin(&mut self, t: Value, dbl: bool) -> Value {
        let (_, e, _) = fmt(dbl);
        let ty = self.b.func.dfg.value_type(t);
        let k = self.vk(ty, e);
        let m = self.b.ins().band(t, k);
        self.b.ins().icmp(IntCC::NotEqual, m, k)
    }
    fn v_zero(&mut self, x: Value, dbl: bool) -> Value {
        let a = self.v_abs(x, dbl);
        let ty = self.b.func.dfg.value_type(x);
        let z = self.vk(ty, 0);
        self.b.ins().icmp(IntCC::Equal, a, z)
    }
    fn v_subn(&mut self, x: Value, dbl: bool) -> Value {
        let (_, _, min) = fmt(dbl);
        let a = self.v_abs(x, dbl);
        let ty = self.b.func.dfg.value_type(x);
        let one = self.vk(ty, 1);
        let a1 = self.b.ins().isub(a, one);
        self.v_ult(a1, min - 1)
    }
    fn v_nan(&mut self, x: Value, dbl: bool) -> Value {
        let (_, e, _) = fmt(dbl);
        let a = self.v_abs(x, dbl);
        let ty = self.b.func.dfg.value_type(x);
        let k = self.vk(ty, e);
        self.b.ins().icmp(IntCC::UnsignedGreaterThan, a, k)
    }
    /// Lanes FZ leaves alone: all of them without FZ, else those of no subnormal operand
    fn v_fzok(&mut self, xs: &[Value], dbl: bool) -> Value {
        let ty = self.b.func.dfg.value_type(xs[0]);
        let fz = self.fz();
        let nofz = self.b.ins().bxor_imm_u(fz, 1);
        let mut m = self.vcond(ty, nofz);
        let mut sub = self.vk(ty, 0);
        for &x in xs {
            let s = self.v_subn(x, dbl);
            sub = self.b.ins().bor(sub, s);
        }
        let ok = self.b.ins().bnot(sub);
        m = self.b.ins().bor(m, ok);
        m
    }
    fn fsplat(&mut self, dbl: bool, v: f64) -> Value {
        let s = if dbl {
            self.b.ins().f64const(v)
        } else {
            self.b.ins().f32const(v as f32)
        };
        self.b.ins().splat(fvt(dbl), s)
    }

    /// Lane-wise FP arithmetic, exact. op: 0 FMUL, 1 FDIV, 2 FADD, 3 FSUB, 9 FMA a*b+c, 10 FRECPS, 11 FRSQRTS
    fn vfp(
        &mut self,
        op: u32,
        a: Value,
        b: Value,
        c: Option<Value>,
        dbl: bool,
        lanes: u32,
    ) -> Value {
        let es = if dbl { 64 } else { 32 };
        let ity = vt(es);
        let fty = fvt(dbl);
        let (a, b) = (self.cast(ity, a), self.cast(ity, b));
        let c = c.map(|c| self.cast(ity, c));
        let fa = self.cast(fty, a);
        let fb = self.cast(fty, b);
        let t = match op {
            0 => self.b.ins().fmul(fa, fb),
            1 => self.b.ins().fdiv(fa, fb),
            2 => self.b.ins().fadd(fa, fb),
            3 => self.b.ins().fsub(fa, fb),
            9 => {
                let fc = self.cast(fty, c.unwrap());
                self.b.ins().fma(fa, fb, fc)
            }
            _ => {
                let na = self.b.ins().fneg(fa);
                let k = self.fsplat(dbl, if op == 10 { 2.0 } else { 3.0 });
                let t = self.b.ins().fma(na, fb, k);
                if op == 10 {
                    t
                } else {
                    let h = self.fsplat(dbl, 0.5);
                    self.b.ins().fmul(t, h)
                }
            }
        };
        let t = self.cast(ity, t);
        let fin = self.v_fin(t, dbl);
        let big = self.v_big(t, dbl);
        let keep = match op {
            2 | 3 => {
                let fzok = self.v_fzok(&[a, b], dbl);
                let zt = self.v_zero(t, dbl);
                let bz = self.b.ins().bor(big, zt);
                // under FZ: no subnormal operand and a large or zero sum; v_fzok is all ones without FZ
                let fz = self.fz();
                let fzm = self.vcond(ity, fz);
                let need = self.b.ins().band(fzm, bz);
                let nfzm = self.b.ins().bnot(fzm);
                let ok = self.b.ins().bor(nfzm, need);
                let ok = self.b.ins().band(ok, fzok);
                self.b.ins().band(fin, ok)
            }
            0 | 1 | 9 => {
                let za = self.v_zero(a, dbl);
                let z = if op == 1 {
                    za
                } else {
                    let zb = self.v_zero(b, dbl);
                    self.b.ins().bor(za, zb)
                };
                let z = self.b.ins().band(z, fin);
                let k = self.b.ins().bor(big, z);
                let fzok = if op == 9 {
                    self.v_fzok(&[a, b, c.unwrap()], dbl)
                } else {
                    self.v_fzok(&[a, b], dbl)
                };
                self.b.ins().band(k, fzok)
            }
            _ => {
                let fzok = self.v_fzok(&[a, b], dbl);
                self.b.ins().band(big, fzok)
            }
        };
        let all = self.all_lanes(keep, lanes);
        let open = self.fpx_open();
        let go = self.b.ins().band(all, open);
        self.vbranch(
            go,
            &mut |_| t,
            &mut |s| {
                s.map_lanes(a, Some(b), c, ity, lanes, &mut |s, x, y, z| match op {
                    9 => s.fma_lane(x, y.unwrap(), z.unwrap(), dbl),
                    10 | 11 => s.recps_lane(op, x, y.unwrap(), dbl),
                    _ => s.arith2(op, x, y.unwrap(), dbl).unwrap(),
                })
            },
            ity,
        )
    }

    /// One lane of FMA (a*b + c), as a64_suyu.h recomp_fpx_fma*: native when kept, else recomp_fpe_fma
    pub(super) fn fma_lane(&mut self, a: Value, b: Value, c: Value, dbl: bool) -> Value {
        let ity = Self::ity_of(dbl);
        let fty = Self::fty_of(dbl);
        let fa = self.b.ins().bitcast(fty, MemFlagsData::new(), a);
        let fb = self.b.ins().bitcast(fty, MemFlagsData::new(), b);
        let fc = self.b.ins().bitcast(fty, MemFlagsData::new(), c);
        let t = self.b.ins().fma(fa, fb, fc);
        let t = self.b.ins().bitcast(ity, MemFlagsData::new(), t);
        let big = self.big(t, dbl);
        let fin = self.finite(t, dbl);
        let za = self.is_zero(a, dbl);
        let zb = self.is_zero(b, dbl);
        let z = self.b.ins().bor(za, zb);
        let z = self.b.ins().band(z, fin);
        let k = self.b.ins().bor(big, z);
        let fz = self.fz();
        let nofz = self.b.ins().bxor_imm_u(fz, 1);
        let sa = self.subnormal(a, dbl);
        let sb = self.subnormal(b, dbl);
        let sc = self.subnormal(c, dbl);
        let s = self.b.ins().bor(sa, sb);
        let s = self.b.ins().bor(s, sc);
        let ok = self.b.ins().bxor_imm_u(s, 1);
        let ok = self.b.ins().bor(nofz, ok);
        let k = self.b.ins().band(k, ok);
        let open = self.fpx_open();
        let keep = self.b.ins().band(k, open);
        self.fast_or_exact(
            keep,
            t,
            &format!("recomp_fpe_fma{}", if dbl { 64 } else { 32 }),
            &[a, b, c],
            dbl,
        )
    }

    /// One lane of FRECPS (op 10, 2 - a*b) or FRSQRTS (11, (3 - a*b) / 2): native when a large normal, else exact
    fn recps_lane(&mut self, op: u32, a: Value, b: Value, dbl: bool) -> Value {
        let ity = Self::ity_of(dbl);
        let fty = Self::fty_of(dbl);
        let fa = self.b.ins().bitcast(fty, MemFlagsData::new(), a);
        let fb = self.b.ins().bitcast(fty, MemFlagsData::new(), b);
        let na = self.b.ins().fneg(fa);
        let kv = if op == 10 { 2.0 } else { 3.0 };
        let k = if dbl {
            self.b.ins().f64const(kv)
        } else {
            self.b.ins().f32const(kv as f32)
        };
        let mut t = self.b.ins().fma(na, fb, k);
        if op == 11 {
            let h = if dbl {
                self.b.ins().f64const(0.5)
            } else {
                self.b.ins().f32const(0.5)
            };
            t = self.b.ins().fmul(t, h);
        }
        let t = self.b.ins().bitcast(ity, MemFlagsData::new(), t);
        let big = self.big(t, dbl);
        let fz = self.fz();
        let nofz = self.b.ins().bxor_imm_u(fz, 1);
        let sa = self.subnormal(a, dbl);
        let sb = self.subnormal(b, dbl);
        let s = self.b.ins().bor(sa, sb);
        let ok = self.b.ins().bxor_imm_u(s, 1);
        let ok = self.b.ins().bor(nofz, ok);
        let k = self.b.ins().band(big, ok);
        let open = self.fpx_open();
        let keep = self.b.ins().band(k, open);
        let w = if dbl { 64 } else { 32 };
        let name = if op == 10 {
            format!("recomp_fpe_recps{w}")
        } else {
            format!("recomp_fpe_rsqrts{w}")
        };
        self.fast_or_exact(keep, t, &name, &[a, b], dbl)
    }

    /// FMAX / FMIN (nm: FMAXNM / FMINNM): native when no lane holds a NaN or (under FZ) a subnormal, which raise no
    /// exception and where the host's IEEE maximum and minimum agree with the Arm ones; else hsr_fminmax per lane
    fn vminmax(&mut self, max: bool, nm: bool, a: Value, b: Value, dbl: bool, lanes: u32) -> Value {
        let ity = vt(if dbl { 64 } else { 32 });
        let fty = fvt(dbl);
        let (a, b) = (self.cast(ity, a), self.cast(ity, b));
        let na = self.v_nan(a, dbl);
        let nb = self.v_nan(b, dbl);
        let n = self.b.ins().bor(na, nb);
        let ok = self.v_fzok(&[a, b], dbl);
        let ok = self.b.ins().band_not(ok, n);
        let go = self.all_lanes(ok, lanes);
        let packed = dbl as i64 | (max as i64) << 1 | (nm as i64) << 2;
        self.vbranch(
            go,
            &mut |s| {
                let fa = s.cast(fty, a);
                let fb = s.cast(fty, b);
                if max {
                    s.b.ins().fmax(fa, fb)
                } else {
                    s.b.ins().fmin(fa, fb)
                }
            },
            &mut |s| {
                s.map_lanes(a, Some(b), None, ity, lanes, &mut |s, x, y, _| {
                    let p = s.b.ins().iconst(types::I64, packed);
                    let r = s.call_c("hsr_fminmax", &[x, y.unwrap(), p]);
                    if dbl {
                        r
                    } else {
                        s.b.ins().ireduce(types::I32, r)
                    }
                })
            },
            ity,
        )
    }

    /// FCMEQ (kind 0) / FCMGE (1) / FCMGT (2), of absolute values when `abs`: lane masks. Native when no lane holds
    /// a NaN or (under FZ) a subnormal, else hsr_fcmp per lane
    fn vfcmp(&mut self, kind: u32, abs: bool, a: Value, b: Value, dbl: bool, lanes: u32) -> Value {
        let ity = vt(if dbl { 64 } else { 32 });
        let fty = fvt(dbl);
        let (mut a, mut b) = (self.cast(ity, a), self.cast(ity, b));
        if abs {
            a = self.v_abs(a, dbl);
            b = self.v_abs(b, dbl);
        }
        let na = self.v_nan(a, dbl);
        let nb = self.v_nan(b, dbl);
        let n = self.b.ins().bor(na, nb);
        let ok = self.v_fzok(&[a, b], dbl);
        let ok = self.b.ins().band_not(ok, n);
        let go = self.all_lanes(ok, lanes);
        let packed = kind as i64 | (dbl as i64) << 8;
        self.vbranch(
            go,
            &mut |s| {
                let fa = s.cast(fty, a);
                let fb = s.cast(fty, b);
                let cc = [
                    FloatCC::Equal,
                    FloatCC::GreaterThanOrEqual,
                    FloatCC::GreaterThan,
                ][kind as usize];
                s.b.ins().fcmp(cc, fa, fb)
            },
            &mut |s| {
                s.map_lanes(a, Some(b), None, ity, lanes, &mut |s, x, y, _| {
                    let p = s.b.ins().iconst(types::I64, packed);
                    let r = s.call_c("hsr_fcmp", &[x, y.unwrap(), p]);
                    if dbl {
                        r
                    } else {
                        s.b.ins().ireduce(types::I32, r)
                    }
                })
            },
            ity,
        )
    }

    /// FRINT{N,P,M,Z,A,I,X}: mode 0 nearest even, 1 up, 2 down, 3 toward zero, 4 nearest away, 5 FPCR.RMode; `exact`
    /// raises IXC on a change (FRINTX). Native when no lane holds a NaN or (under FZ) a subnormal and, for mode 5,
    /// the FPCR mode is to nearest; else hsr_frint per lane
    fn vfrint(&mut self, mode: u32, exact: bool, a: Value, dbl: bool, lanes: u32) -> Value {
        let ity = vt(if dbl { 64 } else { 32 });
        let fty = fvt(dbl);
        let a = self.cast(ity, a);
        let n = self.v_nan(a, dbl);
        let ok = self.v_fzok(&[a], dbl);
        let ok = self.b.ins().band_not(ok, n);
        let mut go = self.all_lanes(ok, lanes);
        if mode == 5 {
            let c = self.fpcr();
            let rm = self.b.ins().band_imm_s(c, 3 << 22);
            let rn = self.b.ins().icmp_imm_s(IntCC::Equal, rm, 0);
            go = self.b.ins().band(go, rn);
        }
        let packed = dbl as i64 | (mode as i64) << 1 | (exact as i64) << 4;
        self.vbranch(
            go,
            &mut |s| {
                let f = s.cast(fty, a);
                let r = match mode {
                    0 | 5 => s.b.ins().nearest(f),
                    1 => s.b.ins().ceil(f),
                    2 => s.b.ins().floor(f),
                    3 => s.b.ins().trunc(f),
                    _ => {
                        // to nearest, ties away: truncate, then step away from zero when the dropped part is >= 1/2
                        let t = s.b.ins().trunc(f);
                        let d = s.b.ins().fsub(f, t);
                        let d = s.b.ins().fabs(d);
                        let h = s.fsplat(dbl, 0.5);
                        let m = s.b.ins().fcmp(FloatCC::GreaterThanOrEqual, d, h);
                        // +-1.0 with x's sign, built bitwise (the host has no vector copysign)
                        let (sign, _, _) = fmt(dbl);
                        let sk = s.vk(ity, sign);
                        let xs = s.b.ins().band(a, sk);
                        let one = s.vk(
                            ity,
                            if dbl {
                                1f64.to_bits()
                            } else {
                                1f32.to_bits() as u64
                            },
                        );
                        let step = s.b.ins().bor(xs, one);
                        let step = s.cast(fty, step);
                        let up = s.b.ins().fadd(t, step);
                        let m = s.cast(fty, m);
                        s.b.ins().bitselect(m, up, t)
                    }
                };
                if exact {
                    let ne = s.b.ins().fcmp(FloatCC::NotEqual, r, f);
                    let any = s.any_lane(ne, lanes);
                    s.fpsr_or(any, IXC);
                }
                r
            },
            &mut |s| {
                s.map_lanes(a, None, None, ity, lanes, &mut |s, x, _, _| {
                    let p = s.b.ins().iconst(types::I64, packed);
                    let r = s.call_c("hsr_frint", &[x, p]);
                    if dbl {
                        r
                    } else {
                        s.b.ins().ireduce(types::I32, r)
                    }
                })
            },
            ity,
        )
    }

    // ---- decoding ------------------------------------------------------------------------------------------------
    /// 0 Q U 0111x ...: the vector forms
    pub(super) fn simd_vector(&mut self, w: u32) -> R {
        if bits(w, 31, 31) != 0 {
            return Err(Unsupported);
        }
        if bits(w, 24, 24) == 0 {
            if bits(w, 21, 21) == 1 {
                return match bits(w, 11, 10) {
                    1 | 3 => self.simd_3same(w, false),
                    0 => self.simd_3diff(w),
                    _ => match bits(w, 20, 17) {
                        0 => self.simd_2misc(w, false),
                        8 => self.simd_across(w),
                        _ => Err(Unsupported),
                    },
                };
            }
            if bits(w, 29, 29) == 0
                && bits(w, 23, 22) == 0
                && bits(w, 15, 15) == 0
                && bits(w, 10, 10) == 1
            {
                return self.simd_copy(w);
            }
            if bits(w, 29, 29) == 1
                && bits(w, 23, 22) == 0
                && bits(w, 15, 15) == 0
                && bits(w, 10, 10) == 1
            {
                return self.simd_copy(w); // INS (element)
            }
            if bits(w, 29, 29) == 0 && bits(w, 15, 15) == 0 && bits(w, 11, 10) == 2 {
                return self.simd_permute(w);
            }
            if bits(w, 29, 29) == 1
                && bits(w, 23, 22) == 0
                && bits(w, 15, 15) == 0
                && bits(w, 10, 10) == 0
            {
                return self.simd_ext(w);
            }
            return Err(Unsupported);
        }
        if bits(w, 10, 10) == 1 {
            if bits(w, 23, 23) != 0 {
                return Err(Unsupported);
            }
            if bits(w, 22, 19) == 0 {
                return self.simd_modimm(w);
            }
            return self.simd_shift_imm(w, false);
        }
        self.simd_elem(w, false)
    }

    /// 01 U 1111x ...: the SIMD-scalar forms
    pub(super) fn simd_scalar(&mut self, w: u32) -> R {
        if bits(w, 24, 24) == 0 {
            if bits(w, 21, 21) == 1 {
                return match bits(w, 11, 10) {
                    1 | 3 => self.simd_3same(w, true),
                    2 => match bits(w, 20, 17) {
                        0 => self.simd_2misc(w, true),
                        8 => self.simd_pairwise_scalar(w),
                        _ => Err(Unsupported),
                    },
                    _ => Err(Unsupported),
                };
            }
            // DUP (element), scalar: 01 0 11110000 imm5 0 0000 1
            if bits(w, 29, 29) == 0 && bits(w, 23, 22) == 0 && bits(w, 15, 10) == 1 {
                let imm5 = bits(w, 20, 16);
                let size = imm5.trailing_zeros();
                if size > 3 {
                    return Err(Unsupported);
                }
                let es = 8 << size;
                let e = self.elem(bits(w, 9, 5), es, imm5 >> (size + 1));
                self.vset(bits(w, 4, 0), e);
                return Ok(());
            }
            return Err(Unsupported);
        }
        if bits(w, 10, 10) == 1 {
            if bits(w, 23, 23) != 0 || bits(w, 22, 19) == 0 {
                return Err(Unsupported);
            }
            return self.simd_shift_imm(w, true);
        }
        self.simd_elem(w, true)
    }

    /// DUP (element, general), SMOV, UMOV, INS (general, element)
    fn simd_copy(&mut self, w: u32) -> R {
        let (q, op, imm5, imm4) = (
            bits(w, 30, 30) != 0,
            bits(w, 29, 29),
            bits(w, 20, 16),
            bits(w, 14, 11),
        );
        let (rd, rn) = (bits(w, 4, 0), bits(w, 9, 5));
        let size = imm5.trailing_zeros();
        if size > 3 {
            return Err(Unsupported);
        }
        let es = 8 << size;
        let idx = imm5 >> (size + 1);
        match (op, imm4) {
            (0, 0) | (0, 1) => {
                if size == 3 && !q {
                    return Err(Unsupported);
                }
                let e = if imm4 == 0 {
                    self.elem(rn, es, idx)
                } else {
                    let x = self.x(rn, size == 3);
                    if es < 32 {
                        self.b.ins().ireduce(lt(es), x)
                    } else {
                        x
                    }
                };
                let v = self.b.ins().splat(vt(es), e);
                self.put(rd, v, q, false, es);
            }
            (0, 5) | (0, 7) => {
                // SMOV / UMOV: Q selects the X form
                let signed = imm4 == 5;
                let ok = if signed {
                    size < 2 || (size == 2 && q)
                } else {
                    (size == 3) == q
                };
                if !ok || (signed && size == 3) {
                    return Err(Unsupported);
                }
                let e = self.elem(rn, es, idx);
                let v = if signed {
                    let t = if q { types::I64 } else { types::I32 };
                    self.b.ins().sextend(t, e)
                } else {
                    e
                };
                self.set(rd, v);
            }
            (0, 3) if q => {
                let x = self.x(rn, true);
                let e = if es < 64 {
                    self.b.ins().ireduce(lt(es), x)
                } else {
                    x
                };
                self.set_elem(rd, es, idx, e);
            }
            (1, _) if q => {
                let e = self.elem(rn, es, imm4 >> size);
                self.set_elem(rd, es, idx, e);
            }
            _ => return Err(Unsupported),
        }
        Ok(())
    }

    /// MOVI, MVNI, ORR, BIC (vector, immediate), FMOV (vector, immediate)
    fn simd_modimm(&mut self, w: u32) -> R {
        let (q, op, cmode, o2) = (
            bits(w, 30, 30) != 0,
            bits(w, 29, 29),
            bits(w, 15, 12),
            bits(w, 11, 11),
        );
        let rd = bits(w, 4, 0);
        if o2 != 0 {
            return Err(Unsupported);
        }
        let imm8 = bits(w, 18, 16) << 5 | bits(w, 9, 5);
        let imm = expand_imm(op, cmode, imm8, q).ok_or(Unsupported)?;
        let logic = cmode < 12 && cmode & 1 == 1; // ORR / BIC
        let k = if op == 1 && cmode < 14 { !imm } else { imm };
        let k = self.vk(types::I64X2, k);
        let r = if logic {
            let d = self.qget(rd, types::I64X2);
            if op == 0 {
                self.b.ins().bor(d, k)
            } else {
                self.b.ins().band(d, k)
            }
        } else {
            k
        };
        self.put(rd, r, q, false, 64);
        Ok(())
    }

    /// Three registers of the same type, integer and floating point
    fn simd_3same(&mut self, w: u32, scalar: bool) -> R {
        let (q, u, size, opcode) = (
            bits(w, 30, 30) != 0 || scalar,
            bits(w, 29, 29),
            bits(w, 23, 22),
            bits(w, 15, 11),
        );
        let (rd, rn, rm) = (bits(w, 4, 0), bits(w, 9, 5), bits(w, 20, 16));
        if bits(w, 10, 10) == 0 {
            return Err(Unsupported);
        }
        if opcode >= 0b11000 {
            let (a_bit, dbl) = (bits(w, 23, 23), bits(w, 22, 22) != 0);
            let qq = bits(w, 30, 30) != 0;
            if dbl && !qq && !scalar {
                return Err(Unsupported);
            }
            let es = if dbl { 64 } else { 32 };
            let lanes = count(qq, es, scalar);
            let ity = vt(es);
            let a = self.qget(rn, ity);
            let b = self.qget(rm, ity);
            let r = match (u, a_bit, opcode) {
                (0, 0, 0b11010) if !scalar => self.vfp(2, a, b, None, dbl, lanes),
                (0, 1, 0b11010) if !scalar => self.vfp(3, a, b, None, dbl, lanes),
                (1, 0, 0b11011) if !scalar => self.vfp(0, a, b, None, dbl, lanes),
                (1, 0, 0b11111) if !scalar => self.vfp(1, a, b, None, dbl, lanes),
                (0, _, 0b11001) if !scalar => {
                    let d = self.qget(rd, ity);
                    let a = if a_bit == 1 {
                        let (sign, _, _) = fmt(dbl);
                        let k = self.vk(ity, sign);
                        self.b.ins().bxor(a, k)
                    } else {
                        a
                    };
                    self.vfp(9, a, b, Some(d), dbl, lanes)
                }
                (1, 0, 0b11010) if !scalar => {
                    let (x, y) = self.pairs(a, b, es, qq);
                    self.vfp(2, x, y, None, dbl, lanes)
                }
                (1, 1, 0b11010) => {
                    // FABD: |a - b|, the sign of whatever FSUB gives (a NaN included) cleared
                    let r = self.vfp(3, a, b, None, dbl, lanes);
                    self.v_abs(r, dbl)
                }
                (0, _, 0b11110) | (0, _, 0b11000) if !scalar => {
                    self.vminmax(a_bit == 0, opcode == 0b11000, a, b, dbl, lanes)
                }
                (1, _, 0b11110) | (1, _, 0b11000) if !scalar => {
                    let (x, y) = self.pairs(a, b, es, qq);
                    self.vminmax(a_bit == 0, opcode == 0b11000, x, y, dbl, lanes)
                }
                (0, 0, 0b11100) => self.vfcmp(0, false, a, b, dbl, lanes),
                (1, 0, 0b11100) => self.vfcmp(1, false, a, b, dbl, lanes),
                (1, 1, 0b11100) => self.vfcmp(2, false, a, b, dbl, lanes),
                (1, 0, 0b11101) => self.vfcmp(1, true, a, b, dbl, lanes),
                (1, 1, 0b11101) => self.vfcmp(2, true, a, b, dbl, lanes),
                (0, 0, 0b11111) => self.vfp(10, a, b, None, dbl, lanes),
                (0, 1, 0b11111) => self.vfp(11, a, b, None, dbl, lanes),
                _ => return Err(Unsupported),
            };
            self.put(rd, r, qq, scalar, es);
            return Ok(());
        }
        let es = 8 << size;
        let qq = bits(w, 30, 30) != 0;
        if scalar && size != 3 && opcode != 0b00011 {
            return Err(Unsupported);
        }
        if scalar && opcode == 0b00011 {
            return Err(Unsupported);
        }
        if size == 3 && !q {
            return Err(Unsupported);
        }
        let ty = vt(es);
        let a = self.qget(rn, ty);
        let b = self.qget(rm, ty);
        let r = match (u, opcode) {
            (_, 0b00011) => {
                let (a, b) = (self.cast(types::I64X2, a), self.cast(types::I64X2, b));
                match (u, size) {
                    (0, 0) => self.b.ins().band(a, b),
                    (0, 1) => self.b.ins().band_not(a, b),
                    (0, 2) => self.b.ins().bor(a, b),
                    (0, _) => self.b.ins().bor_not(a, b),
                    (_, 0) => self.b.ins().bxor(a, b),
                    (_, s) => {
                        let d = self.qget(rd, types::I64X2);
                        match s {
                            1 => self.b.ins().bitselect(d, a, b),
                            2 => self.b.ins().bitselect(b, a, d),
                            _ => self.b.ins().bitselect(b, d, a),
                        }
                    }
                }
            }
            (0, 0b10000) => self.b.ins().iadd(a, b),
            (1, 0b10000) => self.b.ins().isub(a, b),
            (_, 0b00110) => self.b.ins().icmp(
                if u == 0 {
                    IntCC::SignedGreaterThan
                } else {
                    IntCC::UnsignedGreaterThan
                },
                a,
                b,
            ),
            (_, 0b00111) => self.b.ins().icmp(
                if u == 0 {
                    IntCC::SignedGreaterThanOrEqual
                } else {
                    IntCC::UnsignedGreaterThanOrEqual
                },
                a,
                b,
            ),
            (0, 0b10001) => {
                let x = self.b.ins().band(a, b);
                let z = self.vk(ty, 0);
                self.b.ins().icmp(IntCC::NotEqual, x, z)
            }
            (1, 0b10001) => self.b.ins().icmp(IntCC::Equal, a, b),
            _ if scalar => return Err(Unsupported),
            (0, 0b10011) if size < 3 => self.vmul(a, b, es),
            (_, 0b10010) if size < 3 => {
                let p = self.vmul(a, b, es);
                let d = self.qget(rd, ty);
                if u == 0 {
                    self.b.ins().iadd(d, p)
                } else {
                    self.b.ins().isub(d, p)
                }
            }
            (_, 0b01100) | (_, 0b01101) | (_, 0b10100) | (_, 0b10101) if size < 3 => {
                let (x, y) = if opcode >= 0b10100 {
                    self.pairs(a, b, es, qq)
                } else {
                    (a, b)
                };
                let (x, y) = (self.cast(ty, x), self.cast(ty, y));
                match (u, opcode & 1) {
                    (0, 0) => self.b.ins().smax(x, y),
                    (1, 0) => self.b.ins().umax(x, y),
                    (0, _) => self.b.ins().smin(x, y),
                    _ => self.b.ins().umin(x, y),
                }
            }
            (0, 0b10111) => {
                let (x, y) = self.pairs(a, b, es, qq);
                let (x, y) = (self.cast(ty, x), self.cast(ty, y));
                self.b.ins().iadd(x, y)
            }
            (_, 0b01110) | (_, 0b01111) if size < 3 => {
                let d = self.vabd(a, b, u == 0);
                if opcode == 0b01111 {
                    let acc = self.qget(rd, ty);
                    self.b.ins().iadd(acc, d)
                } else {
                    d
                }
            }
            (1, 0b00010) if size < 2 => self.b.ins().avg_round(a, b),
            _ => return Err(Unsupported),
        };
        self.put(rd, r, qq, scalar, es);
        Ok(())
    }

    fn vabd(&mut self, a: Value, b: Value, signed: bool) -> Value {
        let (hi, lo) = if signed {
            (self.b.ins().smax(a, b), self.b.ins().smin(a, b))
        } else {
            (self.b.ins().umax(a, b), self.b.ins().umin(a, b))
        };
        self.b.ins().isub(hi, lo)
    }

    /// Lane-wise multiply, low half kept; bytes through 16-bit lanes
    fn vmul(&mut self, a: Value, b: Value, es: u32) -> Value {
        if es != 8 {
            return self.b.ins().imul(a, b);
        }
        let al = self.b.ins().uwiden_low(a);
        let ah = self.b.ins().uwiden_high(a);
        let bl = self.b.ins().uwiden_low(b);
        let bh = self.b.ins().uwiden_high(b);
        let pl = self.b.ins().imul(al, bl);
        let ph = self.b.ins().imul(ah, bh);
        let even: Vec<u32> = (0..16).map(|i| 2 * i).collect();
        self.gather(pl, ph, 8, &even)
    }

    /// Three registers of different types: the long forms of add, subtract, absolute difference, multiply
    fn simd_3diff(&mut self, w: u32) -> R {
        let (q, u, size, opcode) = (
            bits(w, 30, 30) != 0,
            bits(w, 29, 29),
            bits(w, 23, 22),
            bits(w, 15, 12),
        );
        let (rd, rn, rm) = (bits(w, 4, 0), bits(w, 9, 5), bits(w, 20, 16));
        if size == 3 {
            return Err(Unsupported);
        }
        let es = 8 << size;
        let ty = vt(es);
        let wide = vt(2 * es);
        let widen = |s: &mut Self, v: Value| match (u == 0, q) {
            (true, false) => s.b.ins().swiden_low(v),
            (true, true) => s.b.ins().swiden_high(v),
            (false, false) => s.b.ins().uwiden_low(v),
            (false, true) => s.b.ins().uwiden_high(v),
        };
        let a = self.qget(rn, ty);
        let b = self.qget(rm, ty);
        let r = match opcode {
            0b0000 | 0b0010 => {
                let (x, y) = (widen(self, a), widen(self, b));
                if opcode == 0 {
                    self.b.ins().iadd(x, y)
                } else {
                    self.b.ins().isub(x, y)
                }
            }
            0b0001 | 0b0011 => {
                let x = self.qget(rn, wide);
                let y = widen(self, b);
                if opcode == 1 {
                    self.b.ins().iadd(x, y)
                } else {
                    self.b.ins().isub(x, y)
                }
            }
            0b0111 | 0b0101 => {
                let (x, y) = (widen(self, a), widen(self, b));
                let d = self.vabd(x, y, u == 0);
                if opcode == 0b0101 {
                    let acc = self.qget(rd, wide);
                    self.b.ins().iadd(acc, d)
                } else {
                    d
                }
            }
            0b1100 | 0b1000 | 0b1010 => {
                let (x, y) = (widen(self, a), widen(self, b));
                let p = self.b.ins().imul(x, y);
                match opcode {
                    0b1100 => p,
                    0b1000 => {
                        let acc = self.qget(rd, wide);
                        self.b.ins().iadd(acc, p)
                    }
                    _ => {
                        let acc = self.qget(rd, wide);
                        self.b.ins().isub(acc, p)
                    }
                }
            }
            _ => return Err(Unsupported),
        };
        self.put(rd, r, true, false, 2 * es);
        Ok(())
    }

    /// Two-register miscellaneous: reversals, counts, compares with zero, negation, narrowing, FP unary and conversions
    fn simd_2misc(&mut self, w: u32, scalar: bool) -> R {
        let (qq, u, size, opcode) = (
            bits(w, 30, 30) != 0,
            bits(w, 29, 29),
            bits(w, 23, 22),
            bits(w, 16, 12),
        );
        let (rd, rn) = (bits(w, 4, 0), bits(w, 9, 5));
        let fp = opcode >= 0b11000
            || (opcode >= 0b01100 && opcode <= 0b01111 && size >= 2)
            || opcode == 0b10110
            || opcode == 0b10111;
        if fp {
            return self.simd_2misc_fp(w, scalar);
        }
        let es = 8 << size;
        let ty = vt(es);
        if scalar && size != 3 {
            return Err(Unsupported);
        }
        if size == 3 && !qq && !scalar && !matches!(opcode, 0b10010) {
            return Err(Unsupported);
        }
        let a = self.qget(rn, ty);
        let z = self.vk(ty, 0);
        let r = match (u, opcode) {
            (_, 0b00000) | (0, 0b00001) => {
                // REV64 / REV32 / REV16: reverse the elements within each 64-, 32- or 16-bit container
                let cont = match (u, opcode) {
                    (0, 0) => 64,
                    (1, 0) => 32,
                    _ => 16,
                };
                if es >= cont {
                    return Err(Unsupported);
                }
                let per = cont / es;
                let src: Vec<u32> = (0..128 / es)
                    .map(|i| i / per * per + (per - 1 - i % per))
                    .collect();
                self.gather(a, a, es, &src)
            }
            (0, 0b00101) if size == 0 => self.b.ins().popcnt(a),
            (1, 0b00101) if size == 0 => self.b.ins().bnot(a),
            (1, 0b00101) if size == 1 => {
                let a = self.cast(types::I8X16, a);
                self.vrbit(a)
            }
            (0, 0b01000) => self.b.ins().icmp(IntCC::SignedGreaterThan, a, z),
            (0, 0b01001) => self.b.ins().icmp(IntCC::Equal, a, z),
            (0, 0b01010) => self.b.ins().icmp(IntCC::SignedLessThan, a, z),
            (1, 0b01000) => self.b.ins().icmp(IntCC::SignedGreaterThanOrEqual, a, z),
            (1, 0b01001) => self.b.ins().icmp(IntCC::SignedLessThanOrEqual, a, z),
            (0, 0b01011) => {
                let k = self.b.ins().iconst(types::I32, es as i64 - 1);
                let m = self.b.ins().sshr(a, k);
                let x = self.b.ins().bxor(a, m);
                self.b.ins().isub(x, m)
            }
            (1, 0b01011) => self.b.ins().ineg(a),
            (0, 0b10010) if !scalar && size < 3 => {
                // XTN / XTN2: the low half of each element
                let wide = self.qget(rn, vt(2 * es));
                let n = 64 / es;
                let src: Vec<u32> = (0..n).map(|i| 2 * i).collect();
                let lo = self.gather(wide, wide, es, &src);
                if qq {
                    let d = self.qget(rd, ty);
                    let keep: Vec<u32> = (0..n).chain((0..n).map(|i| 128 / es + i)).collect();
                    let r = self.gather(d, lo, es, &keep);
                    self.put(rd, r, true, false, es);
                } else {
                    self.put(rd, lo, false, false, es);
                }
                return Ok(());
            }
            _ => return Err(Unsupported),
        };
        self.put(rd, r, qq, scalar, es);
        Ok(())
    }

    /// RBIT of every byte
    fn vrbit(&mut self, a: Value) -> Value {
        let mut x = a;
        for (sh, m) in [(1u32, 0x55u64), (2, 0x33), (4, 0x0f)] {
            let k = self.vk(types::I8X16, m);
            let s = self.b.ins().iconst(types::I32, sh as i64);
            let lo = self.b.ins().band(x, k);
            let lo = self.b.ins().ishl(lo, s);
            let hi = self.b.ins().ushr(x, s);
            let hi = self.b.ins().band(hi, k);
            x = self.b.ins().bor(lo, hi);
        }
        x
    }

    fn simd_2misc_fp(&mut self, w: u32, scalar: bool) -> R {
        let (qq, u, opcode) = (bits(w, 30, 30) != 0, bits(w, 29, 29), bits(w, 16, 12));
        let (a_bit, dbl) = (bits(w, 23, 23), bits(w, 22, 22) != 0);
        let (rd, rn) = (bits(w, 4, 0), bits(w, 9, 5));
        if opcode == 0b10110 || opcode == 0b10111 {
            // FCVTN(2) / FCVTL(2) between S and D (sz = 1); FCVTXN and the half forms not here
            if scalar || a_bit != 0 || !dbl || u != 0 {
                return Err(Unsupported);
            }
            if opcode == 0b10111 {
                let lanes: Vec<Value> = (0..2)
                    .map(|i| self.elem(rn, 32, i + if qq { 2 } else { 0 }))
                    .collect();
                let mut r = self.vk(types::I64X2, 0);
                for (i, &s) in lanes.iter().enumerate() {
                    let d = self.fcvt_sd(s);
                    r = self.b.ins().insertlane(r, d, i as u8);
                }
                self.put(rd, r, true, false, 64);
            } else {
                let src: Vec<Value> = (0..2).map(|i| self.elem(rn, 64, i)).collect();
                let out: Vec<Value> = src.iter().map(|&s| self.fcvt_ds(s)).collect();
                if qq {
                    for (i, &v) in out.iter().enumerate() {
                        self.set_elem(rd, 32, 2 + i as u32, v);
                    }
                } else {
                    let mut r = self.vk(types::I32X4, 0);
                    for (i, &v) in out.iter().enumerate() {
                        r = self.b.ins().insertlane(r, v, i as u8);
                    }
                    self.put(rd, r, false, false, 32);
                }
            }
            return Ok(());
        }
        if dbl && !qq && !scalar {
            return Err(Unsupported);
        }
        let es = if dbl { 64 } else { 32 };
        let ity = vt(es);
        let lanes = count(qq, es, scalar);
        let a = self.qget(rn, ity);
        let zero = self.vk(ity, 0);
        let (sign, _, _) = fmt(dbl);
        let r = match (u, a_bit, opcode) {
            (0, 1, 0b01111) => self.v_abs(a, dbl),
            (1, 1, 0b01111) => {
                let k = self.vk(ity, sign);
                self.b.ins().bxor(a, k)
            }
            (1, 1, 0b11111) => {
                self.map_lanes(a, None, None, ity, lanes, &mut |s, x, _, _| s.fsqrt(x, dbl))
            }
            (0, 1, 0b01100) => self.vfcmp(2, false, a, zero, dbl, lanes),
            (0, 1, 0b01101) => self.vfcmp(0, false, a, zero, dbl, lanes),
            (0, 1, 0b01110) => self.vfcmp(2, false, zero, a, dbl, lanes),
            (1, 1, 0b01100) => self.vfcmp(1, false, a, zero, dbl, lanes),
            (1, 1, 0b01101) => self.vfcmp(1, false, zero, a, dbl, lanes),
            (_, 1, 0b11011) => self.map_lanes(a, None, None, ity, lanes, &mut |s, x, _, _| {
                s.fcvtz(x, dbl, dbl, u == 0)
            }),
            (_, 0, 0b11101) => self.map_lanes(a, None, None, ity, lanes, &mut |s, x, _, _| {
                s.cvtf(x, dbl, u == 0)
            }),
            (_, 1, 0b11101) => self.map_lanes(a, None, None, ity, lanes, &mut |s, x, _, _| {
                let width = s.b.ins().iconst(types::I64, es as i64);
                let rs = s.b.ins().iconst(types::I64, u as i64);
                let r = s.call_c("recomp_fp_estimate", &[x, width, rs]);
                if dbl {
                    r
                } else {
                    s.b.ins().ireduce(types::I32, r)
                }
            }),
            (0, 0, 0b11000) if !scalar => self.vfrint(0, false, a, dbl, lanes),
            (0, 0, 0b11001) if !scalar => self.vfrint(2, false, a, dbl, lanes),
            (0, 1, 0b11000) if !scalar => self.vfrint(1, false, a, dbl, lanes),
            (0, 1, 0b11001) if !scalar => self.vfrint(3, false, a, dbl, lanes),
            (1, 0, 0b11000) if !scalar => self.vfrint(4, false, a, dbl, lanes),
            (1, 0, 0b11001) if !scalar => self.vfrint(5, true, a, dbl, lanes),
            (1, 1, 0b11001) if !scalar => self.vfrint(5, false, a, dbl, lanes),
            _ => return Err(Unsupported),
        };
        self.put(rd, r, qq, scalar, es);
        Ok(())
    }

    /// ADDV, SMAXV, UMAXV, SMINV, UMINV, SADDLV, UADDLV, and FMAXV / FMINV / FMAXNMV / FMINNMV (4S)
    fn simd_across(&mut self, w: u32) -> R {
        let (qq, u, size, opcode) = (
            bits(w, 30, 30) != 0,
            bits(w, 29, 29),
            bits(w, 23, 22),
            bits(w, 16, 12),
        );
        let (rd, rn) = (bits(w, 4, 0), bits(w, 9, 5));
        if opcode == 0b01100 || opcode == 0b01111 {
            if u != 1 || !qq || size & 1 != 0 {
                return Err(Unsupported);
            }
            // Reduce: pairwise halves, (e0 op e1) op (e2 op e3)
            let max = size >> 1 == 0;
            let nm = opcode == 0b01100;
            let e: Vec<Value> = (0..4).map(|i| self.elem(rn, 32, i)).collect();
            let x = self.minmax_lane(max, nm, e[0], e[1]);
            let y = self.minmax_lane(max, nm, e[2], e[3]);
            let r = self.minmax_lane(max, nm, x, y);
            self.vset(rd, r);
            return Ok(());
        }
        if size == 3 || (size == 2 && !qq) {
            return Err(Unsupported);
        }
        let es = 8 << size;
        let n = count(qq, es, false);
        let es_out = if opcode == 0b00011 { 2 * es } else { es };
        let ty = lt(es_out);
        let mut acc: Option<Value> = None;
        for i in 0..n {
            let e = self.elem(rn, es, i);
            let e = if es_out > es {
                if u == 0 {
                    self.b.ins().sextend(ty, e)
                } else {
                    self.b.ins().uextend(ty, e)
                }
            } else {
                e
            };
            acc = Some(match acc {
                None => e,
                Some(a) => match (u, opcode) {
                    (_, 0b00011) | (0, 0b11011) => self.b.ins().iadd(a, e),
                    (0, 0b01010) => self.b.ins().smax(a, e),
                    (1, 0b01010) => self.b.ins().umax(a, e),
                    (0, 0b11010) => self.b.ins().smin(a, e),
                    (1, 0b11010) => self.b.ins().umin(a, e),
                    _ => return Err(Unsupported),
                },
            });
        }
        let r = acc.unwrap();
        self.vset(rd, r);
        Ok(())
    }

    /// One FMAX / FMIN (NM) of two S lanes: native without NaNs or flushed subnormals, else hsr_fminmax
    fn minmax_lane(&mut self, max: bool, nm: bool, a: Value, b: Value) -> Value {
        let va = self.b.ins().scalar_to_vector(types::I32X4, a);
        let vb = self.b.ins().scalar_to_vector(types::I32X4, b);
        let r = self.vminmax(max, nm, va, vb, false, 1);
        self.b.ins().extractlane(r, 0)
    }

    /// ADDP (D), FADDP / FMAXP / FMINP / FMAXNMP / FMINNMP (scalar): the two elements of Vn
    fn simd_pairwise_scalar(&mut self, w: u32) -> R {
        let (u, size, opcode) = (bits(w, 29, 29), bits(w, 23, 22), bits(w, 16, 12));
        let (rd, rn) = (bits(w, 4, 0), bits(w, 9, 5));
        if u == 0 {
            if opcode != 0b11011 || size != 3 {
                return Err(Unsupported);
            }
            let a = self.elem(rn, 64, 0);
            let b = self.elem(rn, 64, 1);
            let r = self.b.ins().iadd(a, b);
            self.vset(rd, r);
            return Ok(());
        }
        let dbl = size & 1 != 0;
        let es = if dbl { 64 } else { 32 };
        let a = self.elem(rn, es, 0);
        let b = self.elem(rn, es, 1);
        let ity = vt(es);
        let r = match (size >> 1, opcode) {
            (0, 0b01101) => self.arith2(2, a, b, dbl)?,
            (_, 0b01100) | (_, 0b01111) => {
                let va = self.b.ins().scalar_to_vector(ity, a);
                let vb = self.b.ins().scalar_to_vector(ity, b);
                let r = self.vminmax(size >> 1 == 0, opcode == 0b01100, va, vb, dbl, 1);
                self.b.ins().extractlane(r, 0)
            }
            _ => return Err(Unsupported),
        };
        self.vset(rd, r);
        Ok(())
    }

    /// UZP1/2, TRN1/2, ZIP1/2
    fn simd_permute(&mut self, w: u32) -> R {
        let (qq, size, opcode) = (bits(w, 30, 30) != 0, bits(w, 23, 22), bits(w, 14, 12));
        let (rd, rn, rm) = (bits(w, 4, 0), bits(w, 9, 5), bits(w, 20, 16));
        if size == 3 && !qq {
            return Err(Unsupported);
        }
        let es = 8 << size;
        let l = count(qq, es, false);
        let n = 128 / es;
        let a = self.qget(rn, types::I8X16);
        let b = self.qget(rm, types::I8X16);
        let m = |j: u32| n + j; // element j of Vm
        let src: Vec<u32> = match opcode {
            1 | 5 => {
                let p = (opcode == 5) as u32;
                (0..l)
                    .map(|i| {
                        if 2 * i + p < l {
                            2 * i + p
                        } else {
                            m(2 * i + p - l)
                        }
                    })
                    .collect()
            }
            2 | 6 => {
                let p = (opcode == 6) as u32;
                (0..l)
                    .map(|i| if i % 2 == 0 { i + p } else { m(i - 1 + p) })
                    .collect()
            }
            3 | 7 => {
                let base = if opcode == 7 { l / 2 } else { 0 };
                (0..l)
                    .map(|i| {
                        if i % 2 == 0 {
                            base + i / 2
                        } else {
                            m(base + i / 2)
                        }
                    })
                    .collect()
            }
            _ => return Err(Unsupported),
        };
        let r = self.gather(a, b, es, &src);
        self.put(rd, r, qq, false, es);
        Ok(())
    }

    /// EXT: bytes imm4 on of Vn:Vm (the low halves when Q = 0)
    fn simd_ext(&mut self, w: u32) -> R {
        let (qq, imm4) = (bits(w, 30, 30) != 0, bits(w, 14, 11));
        let (rd, rn, rm) = (bits(w, 4, 0), bits(w, 9, 5), bits(w, 20, 16));
        if !qq && imm4 >= 8 {
            return Err(Unsupported);
        }
        let a = self.qget(rn, types::I8X16);
        let b = self.qget(rm, types::I8X16);
        let l = if qq { 16 } else { 8 };
        let src: Vec<u32> = (0..l)
            .map(|i| {
                if imm4 + i < l {
                    imm4 + i
                } else {
                    16 + imm4 + i - l
                }
            })
            .collect();
        let r = self.gather(a, b, 8, &src);
        self.put(rd, r, qq, false, 8);
        Ok(())
    }

    /// Right shift by s (1 ..= es) without the host's modulo: s = es gives zero, or the sign for an arithmetic shift
    fn shr(&mut self, v: Value, s: u32, es: u32, signed: bool) -> Value {
        let ty = self.b.func.dfg.value_type(v);
        if s >= es {
            if !signed {
                return self.vk(ty, 0);
            }
            let k = self.b.ins().iconst(types::I32, es as i64 - 1);
            return self.b.ins().sshr(v, k);
        }
        let k = self.b.ins().iconst(types::I32, s as i64);
        if signed {
            self.b.ins().sshr(v, k)
        } else {
            self.b.ins().ushr(v, k)
        }
    }

    /// Shifts by immediate: SSHR USHR SSRA USRA SRSHR URSHR SRSRA URSRA SHL SLI SRI SHRN SSHLL USHLL, and the
    /// fixed-point SCVTF UCVTF FCVTZS FCVTZU
    fn simd_shift_imm(&mut self, w: u32, scalar: bool) -> R {
        let (qq, u, immh, opcode) = (
            bits(w, 30, 30) != 0,
            bits(w, 29, 29),
            bits(w, 22, 19),
            bits(w, 15, 11),
        );
        let (rd, rn) = (bits(w, 4, 0), bits(w, 9, 5));
        let imm7 = bits(w, 22, 16);
        let es = 8 << (31 - immh.leading_zeros());
        if es == 64 && !qq && !scalar {
            return Err(Unsupported);
        }
        let shr = 2 * es - imm7;
        let shl = imm7 - es;
        let ty = vt(es);
        let lanes = count(qq, es, scalar);
        if scalar && es != 64 && opcode != 0b11100 && opcode != 0b11111 {
            return Err(Unsupported);
        }
        let r = match (u, opcode) {
            (_, 0b00000) | (_, 0b00010) | (_, 0b00100) | (_, 0b00110) => {
                let a = self.qget(rn, ty);
                let signed = u == 0;
                let mut r = self.shr(a, shr, es, signed);
                if opcode & 4 != 0 {
                    // rounding: add the last bit shifted out
                    let b = self.shr(a, shr - 1, es, signed);
                    let one = self.vk(ty, 1);
                    let b = self.b.ins().band(b, one);
                    r = self.b.ins().iadd(r, b);
                }
                if opcode & 2 != 0 {
                    let d = self.qget(rd, ty);
                    r = self.b.ins().iadd(d, r);
                }
                r
            }
            (0, 0b01010) => {
                let a = self.qget(rn, ty);
                let k = self.b.ins().iconst(types::I32, shl as i64);
                self.b.ins().ishl(a, k)
            }
            (1, 0b01010) => {
                // SLI: insert a << shl, keeping the low shl bits of Vd
                let a = self.qget(rn, ty);
                let d = self.qget(rd, ty);
                let k = self.b.ins().iconst(types::I32, shl as i64);
                let s = self.b.ins().ishl(a, k);
                let keep = self.vk(ty, ones(shl));
                let d = self.b.ins().band(d, keep);
                self.b.ins().bor(s, d)
            }
            (1, 0b01000) => {
                // SRI: insert a >> shr, keeping the high shr bits of Vd
                let a = self.qget(rn, ty);
                let d = self.qget(rd, ty);
                let s = self.shr(a, shr, es, false);
                let keep = if shr >= es {
                    ones(es)
                } else {
                    ones(es) & !(ones(es) >> shr)
                };
                let keep = self.vk(ty, keep);
                let d = self.b.ins().band(d, keep);
                self.b.ins().bor(s, d)
            }
            (0, 0b10000) if !scalar && es < 64 => {
                // SHRN / SHRN2: the narrowed (wide >> shr)
                let wide = vt(2 * es);
                let a = self.qget(rn, wide);
                let s = self.shr(a, shr, 2 * es, false);
                let n = 64 / es;
                let src: Vec<u32> = (0..n).map(|i| 2 * i).collect();
                let lo = self.gather(s, s, es, &src);
                if qq {
                    let d = self.qget(rd, ty);
                    let keep: Vec<u32> = (0..n).chain((0..n).map(|i| 128 / es + i)).collect();
                    let r = self.gather(d, lo, es, &keep);
                    self.put(rd, r, true, false, es);
                } else {
                    self.put(rd, lo, false, false, es);
                }
                return Ok(());
            }
            (_, 0b10100) if !scalar && es < 64 => {
                let a = self.qget(rn, ty);
                let wv = match (u == 0, qq) {
                    (true, false) => self.b.ins().swiden_low(a),
                    (true, true) => self.b.ins().swiden_high(a),
                    (false, false) => self.b.ins().uwiden_low(a),
                    (false, true) => self.b.ins().uwiden_high(a),
                };
                let k = self.b.ins().iconst(types::I32, shl as i64);
                let r = self.b.ins().ishl(wv, k);
                self.put(rd, r, true, false, 2 * es);
                return Ok(());
            }
            (_, 0b11100) | (_, 0b11111) if es >= 32 => {
                let dbl = es == 64;
                let a = self.qget(rn, ty);
                let fbits = shr;
                if opcode == 0b11100 {
                    self.map_lanes(a, None, None, ty, lanes, &mut |s, x, _, _| {
                        s.cvtf_fixed(x, dbl, u == 0, fbits)
                    })
                } else {
                    self.map_lanes(a, None, None, ty, lanes, &mut |s, x, _, _| {
                        s.fcvtz_fixed(x, dbl, u == 0, fbits)
                    })
                }
            }
            _ => return Err(Unsupported),
        };
        self.put(rd, r, qq, scalar, es);
        Ok(())
    }

    /// SCVTF / UCVTF (fixed point, fbits fraction bits): the exact body hsr_fixed_to_fp (FPCR's rounding mode)
    fn cvtf_fixed(&mut self, x: Value, dbl: bool, signed: bool, fbits: u32) -> Value {
        let w = if dbl { 64 } else { 32 };
        let packed = w | w << 8 | (signed as i64) << 16 | (fbits as i64) << 24;
        let p = self.b.ins().iconst(types::I64, packed);
        let r = self.call_c("hsr_fixed_to_fp", &[x, p]);
        if dbl {
            r
        } else {
            self.b.ins().ireduce(types::I32, r)
        }
    }

    /// FCVTZS / FCVTZU (fixed point): x * 2^fbits converted toward zero. The input is flushed first under FZ; the
    /// scaling is exact (a power of two, at worst overflowing to an infinity, which saturates and signals as the
    /// out-of-range value would)
    fn fcvtz_fixed(&mut self, x: Value, dbl: bool, signed: bool, fbits: u32) -> Value {
        if fbits == 0 {
            return self.fcvtz(x, dbl, dbl, signed);
        }
        let fz = self.fz();
        let x = self.flush_in(x, dbl, fz);
        let fty = Self::fty_of(dbl);
        let f = self.b.ins().bitcast(fty, MemFlagsData::new(), x);
        let k = if dbl {
            self.b.ins().f64const(2f64.powi(fbits as i32))
        } else {
            self.b.ins().f32const(2f32.powi(fbits as i32))
        };
        let s = self.b.ins().fmul(f, k);
        let s = self
            .b
            .ins()
            .bitcast(Self::ity_of(dbl), MemFlagsData::new(), s);
        // already flushed: a scaled subnormal must not be flushed again, so FZ is out of the picture from here
        self.fcvtz_noflush(s, dbl, dbl, signed)
    }

    /// FMUL / FMLA / FMLS / MUL / MLA / MLS by element
    fn simd_elem(&mut self, w: u32, scalar: bool) -> R {
        let (qq, u, size, opcode) = (
            bits(w, 30, 30) != 0,
            bits(w, 29, 29),
            bits(w, 23, 22),
            bits(w, 15, 12),
        );
        let (rd, rn) = (bits(w, 4, 0), bits(w, 9, 5));
        let (l, m, h) = (bits(w, 21, 21), bits(w, 20, 20), bits(w, 11, 11));
        match (u, opcode) {
            (0, 0b0001) | (0, 0b0101) | (0, 0b1001) => {
                let dbl = size & 1 != 0;
                if size >> 1 != 1 {
                    return Err(Unsupported); // the half-precision forms
                }
                if dbl && (l != 0 || (!qq && !scalar)) {
                    return Err(Unsupported);
                }
                let es = if dbl { 64 } else { 32 };
                let idx = if dbl { h } else { h << 1 | l };
                let rm = bits(w, 20, 16);
                let lanes = count(qq, es, scalar);
                let ity = vt(es);
                let e = self.elem(rm, es, idx);
                let b = self.b.ins().splat(ity, e);
                let a = self.qget(rn, ity);
                let r = if opcode == 0b1001 {
                    self.vfp(0, a, b, None, dbl, lanes)
                } else {
                    let d = self.qget(rd, ity);
                    let a = if opcode == 0b0101 {
                        let (sign, _, _) = fmt(dbl);
                        let k = self.vk(ity, sign);
                        self.b.ins().bxor(a, k)
                    } else {
                        a
                    };
                    self.vfp(9, a, b, Some(d), dbl, lanes)
                };
                self.put(rd, r, qq, scalar, es);
                Ok(())
            }
            (0, 0b1000) | (1, 0b0000) | (1, 0b0100) if !scalar => {
                let (es, idx, rm) = match size {
                    1 => (16, h << 2 | l << 1 | m, bits(w, 19, 16)),
                    2 => (32, h << 1 | l, bits(w, 20, 16)),
                    _ => return Err(Unsupported),
                };
                let ty = vt(es);
                let e = self.elem(rm, es, idx);
                let b = self.b.ins().splat(ty, e);
                let a = self.qget(rn, ty);
                let p = self.b.ins().imul(a, b);
                let r = match (u, opcode) {
                    (0, _) => p,
                    (_, 0) => {
                        let d = self.qget(rd, ty);
                        self.b.ins().iadd(d, p)
                    }
                    _ => {
                        let d = self.qget(rd, ty);
                        self.b.ins().isub(d, p)
                    }
                };
                self.put(rd, r, qq, false, es);
                Ok(())
            }
            _ => Err(Unsupported),
        }
    }

    // ---- loads and stores of single and multiple structures ------------------------------------------------------
    /// LD1-LD4 / ST1-ST4 (multiple structures), LD1-LD4 / ST1-ST4 (single structure), LD1R-LD4R, with post-index
    pub(super) fn ldst_struct(&mut self, w: u32) -> R {
        let (qq, l) = (bits(w, 30, 30) != 0, bits(w, 22, 22) != 0);
        let (rt, rn, rm) = (bits(w, 4, 0), bits(w, 9, 5), bits(w, 20, 16));
        let post = bits(w, 23, 23) != 0;
        let single = bits(w, 24, 24) != 0;
        if !post && rm != 0 {
            return Err(Unsupported);
        }
        let base = self.xsp(rn, true);
        let host = self.b.ins().iadd_imm_s(base, ctx::MEM_BASE);
        let bytes;
        if !single {
            let (opcode, size) = (bits(w, 15, 12), bits(w, 11, 10));
            let (rpt, selem) = match opcode {
                0b0000 => (1, 4),
                0b0010 => (4, 1),
                0b0100 => (1, 3),
                0b0110 => (3, 1),
                0b0111 => (1, 1),
                0b1000 => (1, 2),
                0b1010 => (2, 1),
                _ => return Err(Unsupported),
            };
            if size == 3 && !qq && selem != 1 {
                return Err(Unsupported);
            }
            let regbytes = if qq { 16 } else { 8 };
            bytes = rpt * selem * regbytes;
            if selem == 1 {
                for r in 0..rpt {
                    let t = (rt + r) % 32;
                    let off = (r * regbytes) as i32;
                    if l {
                        let lo = self
                            .b
                            .ins()
                            .load(types::I64, MemFlagsData::new(), host, off);
                        let hi = if qq {
                            self.b
                                .ins()
                                .load(types::I64, MemFlagsData::new(), host, off + 8)
                        } else {
                            self.b.ins().iconst(types::I64, 0)
                        };
                        self.vset128(t, lo, hi);
                    } else {
                        let lo = self.vget(t, types::I64);
                        self.b.ins().store(MemFlagsData::new(), lo, host, off);
                        if qq {
                            let hi = self.vget_hi(t);
                            self.b.ins().store(MemFlagsData::new(), hi, host, off + 8);
                        }
                    }
                }
            } else {
                let es = 8 << size;
                let eb = es / 8;
                let elems = regbytes / eb;
                // read every element first: the registers may be the ones being loaded
                let mut loaded = Vec::new();
                for e in 0..elems {
                    for s in 0..selem {
                        let t = (rt + s) % 32;
                        let off = ((e * selem + s) * eb) as i32;
                        if l {
                            let v = self.b.ins().load(lt(es), MemFlagsData::new(), host, off);
                            loaded.push((t, e, v));
                        } else {
                            let v = self.elem(t, es, e);
                            self.b.ins().store(MemFlagsData::new(), v, host, off);
                        }
                    }
                }
                if l {
                    if !qq {
                        for s in 0..selem {
                            let z = self.b.ins().iconst(types::I64, 0);
                            self.b.ins().store(
                                MemFlagsData::trusted(),
                                z,
                                self.ctx,
                                Self::v_off((rt + s) % 32) + 8,
                            );
                        }
                    }
                    for (t, e, v) in loaded {
                        self.set_elem(t, es, e, v);
                    }
                }
            }
        } else {
            let (opcode, s, size, r) = (
                bits(w, 15, 13),
                bits(w, 12, 12),
                bits(w, 11, 10),
                bits(w, 21, 21),
            );
            let selem = ((opcode & 1) << 1 | r) + 1;
            let (scale, idx, replicate) = match opcode >> 1 {
                0 => (0, (qq as u32) << 3 | s << 2 | size, false),
                1 => {
                    if size & 1 != 0 {
                        return Err(Unsupported);
                    }
                    (1, (qq as u32) << 2 | s << 1 | size >> 1, false)
                }
                2 => match (size, s) {
                    (0, _) => (2, (qq as u32) << 1 | s, false),
                    (1, 0) => (3, qq as u32, false),
                    _ => return Err(Unsupported),
                },
                _ => {
                    if !l || s != 0 {
                        return Err(Unsupported);
                    }
                    (size, 0, true)
                }
            };
            let es = 8 << scale;
            let eb = es / 8;
            bytes = selem * eb;
            let vals: Vec<Value> = (0..selem)
                .map(|k| {
                    if l {
                        self.b
                            .ins()
                            .load(lt(es), MemFlagsData::new(), host, (k * eb) as i32)
                    } else {
                        let v = self.elem((rt + k) % 32, es, idx);
                        self.b
                            .ins()
                            .store(MemFlagsData::new(), v, host, (k * eb) as i32);
                        v
                    }
                })
                .collect();
            if l {
                for (k, v) in vals.into_iter().enumerate() {
                    let t = (rt + k as u32) % 32;
                    if replicate {
                        let sp = self.b.ins().splat(vt(es), v);
                        self.put(t, sp, qq, false, es);
                    } else {
                        self.set_elem(t, es, idx, v);
                    }
                }
            }
        }
        if post {
            let nb = if rm == 31 {
                self.b.ins().iadd_imm_s(base, bytes as i64)
            } else {
                let off = self.x(rm, true);
                self.b.ins().iadd(base, off)
            };
            self.setsp(rn, nb);
        }
        Ok(())
    }
}
