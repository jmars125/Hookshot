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
    let integer_mask = if int_bits == 64 { u64::MAX } else { 0xffff_ffff };
    let fraction_mask = (1u64 << fraction_bits) - 1;
    let bits = bits & integer_mask;
    let negative = is_signed && (bits >> (int_bits - 1)) & 1 != 0;
    let magnitude = if negative { bits.wrapping_neg() & integer_mask } else { bits };
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
    ((negative as u64) << (fp_bits - 1)) | (((exponent + bias) as u64) << fraction_bits) | (mantissa & fraction_mask)
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
    let (mant, e) = if exp == 0 { (frac, -1074) } else { (frac | 1 << 52, exp - 1075) };
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
        (mant >> shift, mant & ((1u64 << shift) - 1), 1u64 << (shift - 1))
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

/// Every helper by name, for a JIT to resolve them
pub fn symbols() -> Vec<(&'static str, *const u8)> {
    vec![("hsr_fixed_to_fp", hsr_fixed_to_fp as *const u8), ("hsr_fcvt_ds", hsr_fcvt_ds as *const u8)]
}
