//! Runtime helpers that lifted code calls, written in Rust. They take the guest context first, like the C runtime's
//! `recomp_fpe_*` bodies, and read and update FPCR / FPSR there.

use super::ctx::Ctx;

/// SCVTF / UCVTF of an integer or fixed-point value, in the guest's FPCR rounding mode (a64_suyu.h recomp_fixed_to_fp).
/// `packed`: bits 7:0 the float width (32 or 64), 15:8 the integer width, bit 16 signed, 31:24 the fraction bits.
/// Every S/D input converts to zero or a finite normal, so only IXC can be raised.
pub extern "C" fn hsr_fixed_to_fp(c: *mut Ctx, bits: u64, packed: u64) -> u64 {
    // SAFETY: lifted code passes its own live context
    let c = unsafe { &mut *c };
    let fp_bits = (packed & 0xff) as u32;
    let int_bits = ((packed >> 8) & 0xff) as u32;
    let is_signed = (packed >> 16) & 1 != 0;
    let fbits = ((packed >> 24) & 0xff) as i32;
    let fraction_bits: u32 = if fp_bits == 64 { 52 } else { 23 };
    let bias: i32 = if fp_bits == 64 { 1023 } else { 127 };
    let integer_mask = if int_bits == 64 {
        u64::MAX
    } else {
        0xffff_ffff
    };
    let fraction_mask = (1u64 << fraction_bits) - 1;
    let bits = bits & integer_mask;
    let negative = is_signed && (bits >> (int_bits - 1)) & 1 != 0;
    let magnitude = if negative {
        bits.wrapping_neg() & integer_mask
    } else {
        bits
    };
    if magnitude == 0 {
        return 0;
    }
    let top = 63 - magnitude.leading_zeros();
    let mut exponent = top as i32 - fbits;
    let mut mantissa;
    if top <= fraction_bits {
        mantissa = magnitude << (fraction_bits - top);
    } else {
        let discarded = top - fraction_bits;
        let remainder = magnitude & ((1u64 << discarded) - 1);
        let halfway = 1u64 << (discarded - 1);
        let mode = (c.fpcr >> 22) & 3;
        mantissa = magnitude >> discarded;
        if remainder != 0 {
            c.fpsr |= 16;
            if (mode == 0 && (remainder > halfway || (remainder == halfway && mantissa & 1 != 0)))
                || (mode == 1 && !negative)
                || (mode == 2 && negative)
            {
                mantissa += 1;
            }
        }
        if mantissa >= 1u64 << (fraction_bits + 1) {
            mantissa >>= 1;
            exponent += 1;
        }
    }
    ((negative as u64) << (fp_bits - 1))
        | (((exponent + bias) as u64) << fraction_bits)
        | (mantissa & fraction_mask)
}

/// FCVT Sd, Dn by the Arm ARM (FPConvert / FPRoundBase): FPCR.RMode, FZ (input flushed with IDC; a result below the
/// normal range, judged before rounding, flushed with UFC), DN, tininess before rounding, IOC for a signalling NaN
pub extern "C" fn hsr_fcvt_ds(c: *mut Ctx, a: u64) -> u64 {
    // SAFETY: lifted code passes its own live context
    let c = unsafe { &mut *c };
    let sign = a >> 63;
    let exp = ((a >> 52) & 0x7ff) as i32;
    let frac = a & ((1u64 << 52) - 1);
    let fz = c.fpcr & (1 << 24) != 0;
    let s = (sign as u32) << 31;
    if exp == 0x7ff {
        if frac == 0 {
            return (s | 0x7f80_0000) as u64;
        }
        if frac >> 51 == 0 {
            c.fpsr |= 1; // signalling NaN
        }
        if c.fpcr & (1 << 25) != 0 {
            return 0x7fc0_0000; // DN: the default NaN
        }
        return (s | 0x7fc0_0000 | (frac >> 29) as u32) as u64;
    }
    if exp == 0 && (frac == 0 || fz) {
        if frac != 0 {
            c.fpsr |= 128;
        }
        return s as u64;
    }
    // value = mant * 2^e
    let (mant, e) = if exp == 0 {
        (frac, -1074)
    } else {
        (frac | 1 << 52, exp - 1075)
    };
    let top = 63 - mant.leading_zeros() as i32;
    let big_e = e + top; // the unrounded exponent
    if fz && big_e < -126 {
        c.fpsr |= 8;
        return s as u64;
    }
    let lsb = (big_e - 23).max(-149);
    let shift = lsb - e;
    let (mut m, rem, half) = if shift <= 0 {
        (mant << (-shift) as u32, 0u64, 1u64)
    } else if shift >= 64 {
        (0, mant, 0)
    } else {
        (
            mant >> shift,
            mant & ((1u64 << shift) - 1),
            1u64 << (shift - 1),
        )
    };
    let inexact = rem != 0;
    let rmode = (c.fpcr >> 22) & 3;
    if inexact {
        let up = match rmode {
            0 => half != 0 && (rem > half || (rem == half && m & 1 != 0)),
            1 => sign == 0,
            2 => sign != 0,
            _ => false,
        };
        if up {
            m += 1;
        }
    }
    let bits = (((lsb + 149) as u64) << 23) + m;
    if bits >= 0x7f80_0000 {
        c.fpsr |= 4 | 16;
        let inf = rmode == 0 || (rmode == 1 && sign == 0) || (rmode == 2 && sign != 0);
        return (s | if inf { 0x7f80_0000 } else { 0x7f7f_ffff }) as u64;
    }
    if inexact {
        if big_e < -126 {
            c.fpsr |= 8;
        }
        c.fpsr |= 16;
    }
    s as u64 | bits
}

// ---- binary32 / binary64 by bits, for the helpers below ------------------------------------------------------------

#[derive(Clone, Copy)]
struct Fmt {
    sign: u64,
    exp: u64,
    quiet: u64,
}

impl Fmt {
    fn of(dbl: bool) -> Fmt {
        if dbl {
            Fmt {
                sign: 1 << 63,
                exp: 0x7ff0_0000_0000_0000,
                quiet: 1 << 51,
            }
        } else {
            Fmt {
                sign: 1 << 31,
                exp: 0x7f80_0000,
                quiet: 1 << 22,
            }
        }
    }
    fn nan(self, x: u64) -> bool {
        x & !self.sign > self.exp
    }
    fn snan(self, x: u64) -> bool {
        self.nan(x) && x & self.quiet == 0
    }
    fn qnan(self, x: u64) -> bool {
        self.nan(x) && x & self.quiet != 0
    }
    fn zero(self, x: u64) -> bool {
        x & !self.sign == 0
    }
    fn inf(self, x: u64) -> bool {
        x & !self.sign == self.exp
    }
    fn value(self, x: u64) -> f64 {
        if self.sign == 1 << 63 {
            f64::from_bits(x)
        } else {
            f32::from_bits(x as u32) as f64
        }
    }
    fn bits(self, v: f64) -> u64 {
        if self.sign == 1 << 63 {
            v.to_bits()
        } else {
            (v as f32).to_bits() as u64
        }
    }
}

/// FPUnpack's FZ handling: a subnormal input reads as a zero of its sign and sets IDC
fn flush(c: &mut Ctx, f: Fmt, x: u64) -> u64 {
    if c.fpcr & (1 << 24) != 0 && x & f.exp == 0 && !f.zero(x) {
        c.fpsr |= 128;
        x & f.sign
    } else {
        x
    }
}

/// FPProcessNaN: quieten (IOC if signalling); the default NaN under FPCR.DN
fn process_nan(c: &mut Ctx, f: Fmt, x: u64) -> u64 {
    if f.snan(x) {
        c.fpsr |= 1;
    }
    if c.fpcr & (1 << 25) != 0 {
        f.exp | f.quiet
    } else {
        x | f.quiet
    }
}

/// FPProcessNaNs: the first signalling NaN, else the first quiet one
fn process_nans(c: &mut Ctx, f: Fmt, a: u64, b: u64) -> Option<u64> {
    let pick = if f.snan(a) {
        a
    } else if f.snan(b) {
        b
    } else if f.nan(a) {
        a
    } else if f.nan(b) {
        b
    } else {
        return None;
    };
    Some(process_nan(c, f, pick))
}

/// FMAX / FMIN / FMAXNM / FMINNM (FPMax, FPMin, FPMaxNum, FPMinNum). `packed`: bit 0 double, bit 1 max, bit 2 NM
pub extern "C" fn hsr_fminmax(c: *mut Ctx, a: u64, b: u64, packed: u64) -> u64 {
    // SAFETY: lifted code passes its own live context
    let c = unsafe { &mut *c };
    let f = Fmt::of(packed & 1 != 0);
    let max = packed & 2 != 0;
    let (mut a, mut b) = (a, b);
    if packed & 4 != 0 {
        // a lone quiet NaN loses: it becomes the infinity that never wins
        let inf = f.exp | if max { f.sign } else { 0 };
        let a_q = f.qnan(a);
        let b_q = f.qnan(b);
        if a_q && !b_q {
            a = inf;
        } else if !a_q && b_q {
            b = inf;
        }
    }
    let a = flush(c, f, a);
    let b = flush(c, f, b);
    if let Some(n) = process_nans(c, f, a, b) {
        return n;
    }
    if f.zero(a) && f.zero(b) {
        let s = if max { a & b } else { a | b };
        return s & f.sign;
    }
    let (va, vb) = (f.value(a), f.value(b));
    if (va > vb) == max {
        a
    } else {
        b
    }
}

/// FCMEQ / FCMGE / FCMGT / FACGE / FACGT: all ones when true. `packed`: bits 1:0 = 0 EQ, 1 GE, 2 GT; bit 3 compares
/// absolute values; bit 8 double. EQ signals (IOC) only for a signalling NaN, GE and GT for any NaN
pub extern "C" fn hsr_fcmp(c: *mut Ctx, a: u64, b: u64, packed: u64) -> u64 {
    // SAFETY: lifted code passes its own live context
    let c = unsafe { &mut *c };
    let f = Fmt::of(packed & 256 != 0);
    let (mut a, mut b) = (a, b);
    if packed & 8 != 0 {
        a &= !f.sign;
        b &= !f.sign;
    }
    let a = flush(c, f, a);
    let b = flush(c, f, b);
    let kind = packed & 3;
    if f.nan(a) || f.nan(b) {
        if kind != 0 || f.snan(a) || f.snan(b) {
            c.fpsr |= 1;
        }
        return 0;
    }
    let (va, vb) = (f.value(a), f.value(b));
    let r = match kind {
        0 => va == vb,
        1 => va >= vb,
        _ => va > vb,
    };
    if r {
        u64::MAX
    } else {
        0
    }
}

/// FRINT{N,P,M,Z,A,I,X} (FPRoundInt). `packed`: bit 0 double; bits 3:1 the mode (0 to nearest even, 1 up, 2 down,
/// 3 toward zero, 4 to nearest away, 5 FPCR.RMode); bit 4 raises IXC when the result differs (FRINTX)
pub extern "C" fn hsr_frint(c: *mut Ctx, a: u64, packed: u64) -> u64 {
    // SAFETY: lifted code passes its own live context
    let c = unsafe { &mut *c };
    let f = Fmt::of(packed & 1 != 0);
    let a = flush(c, f, a);
    if f.nan(a) {
        return process_nan(c, f, a);
    }
    if f.inf(a) || f.zero(a) {
        return a;
    }
    let mut mode = (packed >> 1) & 7;
    if mode == 5 {
        mode = (c.fpcr >> 22) & 3;
    }
    let v = f.value(a);
    let r = match mode {
        0 => v.round_ties_even(),
        1 => v.ceil(),
        2 => v.floor(),
        3 => v.trunc(),
        _ => v.round(),
    };
    if packed & 16 != 0 && r != v {
        c.fpsr |= 16;
    }
    let bits = f.bits(r);
    if f.zero(bits) {
        a & f.sign // a zero result keeps the input's sign
    } else {
        bits
    }
}

/// Every helper by name, for a JIT to resolve them
pub fn symbols() -> Vec<(&'static str, *const u8)> {
    vec![
        ("hsr_fixed_to_fp", hsr_fixed_to_fp as *const u8),
        ("hsr_fcvt_ds", hsr_fcvt_ds as *const u8),
        ("hsr_fminmax", hsr_fminmax as *const u8),
        ("hsr_fcmp", hsr_fcmp as *const u8),
        ("hsr_frint", hsr_frint as *const u8),
    ]
}
