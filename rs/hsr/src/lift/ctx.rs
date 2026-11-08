//! The guest context lifted code works on: a64-kit's GuestContext (third_party/a64-kit/rt/a64.h) with the `poll` field
//! nx2/rtgen.py adds for the standalone build. Field order and types must match the C struct exactly: the C runtime and
//! C-lifted functions read the same memory. `offsets_match_c` in tests checks the offsets against clang.

#[repr(C)]
pub struct Ctx {
    pub x: [u64; 32], // x[31] is SP
    pub pc: u64,
    pub n: u8,
    pub z: u8,
    pub c: u8,
    pub v: u8,
    pub halted: i32,
    pub vreg: [[u64; 2]; 32],
    pub tpidr_el0: u64,
    pub tpidrro_el0: u64,
    pub fpcr: u64,
    pub fpsr: u64,
    pub excl_addr: u64,
    pub excl_lo: u64,
    pub excl_hi: u64,
    pub excl_size: u32,
    pub excl_valid: i32,
    pub thread: *mut u8,
    pub poll: u32,
}

macro_rules! off {
    ($f:ident) => {
        std::mem::offset_of!(Ctx, $f) as i32
    };
}

pub const X: i32 = off!(x);
pub const PC: i32 = off!(pc);
pub const N: i32 = off!(n);
pub const Z: i32 = off!(z);
pub const C: i32 = off!(c);
pub const V: i32 = off!(v);
pub const VREG: i32 = off!(vreg);
pub const TPIDR_EL0: i32 = off!(tpidr_el0);
pub const TPIDRRO_EL0: i32 = off!(tpidrro_el0);
pub const FPCR: i32 = off!(fpcr);
pub const FPSR: i32 = off!(fpsr);
pub const POLL: i32 = off!(poll);

/// Host address of guest address 0: the 64-bit guest window (nx2/rtgen.py A64_ADDR64, a64.h A64_HOST_BASE)
pub const MEM_BASE: i64 = 0x1000_0000_0000;

impl Default for Ctx {
    fn default() -> Self {
        // SAFETY: every field is an integer or a raw pointer, for which all-zero bytes are valid
        unsafe { std::mem::zeroed() }
    }
}
