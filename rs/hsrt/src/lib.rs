//! The helpers hsr's lifted code (rs/hsr, `hsr compile`) calls that the C runtime does not have, built as a static
//! library for the game's link: the FP helpers of hsr/src/lift/rt.rs (the same file the JIT tests run) and
//! `hsr_icall`, the indirect call through a call site's cache.
//!
//! no_std and panic=abort: the library adds no Rust runtime to the game, only these functions and what of core they use.

#![no_std]

// (`std` names core here: ctx.rs is shared with hsr, which is a std crate, and spells its paths std::mem)
extern crate core as std;

#[allow(dead_code)]
#[path = "../../hsr/src/lift/ctx.rs"]
mod ctx;
#[allow(dead_code)]
#[path = "../../hsr/src/lift/rt.rs"]
mod rt;

use core::sync::atomic::{AtomicU64, Ordering};
use ctx::Ctx;

type A64Fn = unsafe extern "C" fn(*mut Ctx);

extern "C" {
    /// The function at a guest address, or null (rt/a64_rt.c)
    fn a64_lookup(addr: u64) -> Option<A64Fn>;
    /// The C runtime's BLR: a guest address that is no lifted function (HLE, a setjmp return, a bad pointer)
    fn a64_icall(c: *mut Ctx, target: u64);
}

/// BLR / BR to `t` from a lifted function's call site, whose 16-byte cache `site` (target, function) the caller
/// reads first: a site whose cache holds `t` calls the function itself. The first target found fills it (the
/// function before the target, with release ordering, so a reader that sees the target sees its function); other
/// targets are looked up each time. Addresses that are no lifted function go to the C runtime's a64_icall.
///
/// # Safety
/// `c` is the calling thread's live context and `site` a cache of the calling function's data.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn hsr_icall(c: *mut Ctx, t: u64, site: *mut u64) {
    let Some(f) = (unsafe { a64_lookup(t) }) else {
        return unsafe { a64_icall(c, t) };
    };
    let tgt = unsafe { &*(site as *const AtomicU64) };
    if tgt.load(Ordering::Acquire) == 0 {
        let fp = unsafe { &*(site.add(1) as *const AtomicU64) };
        // (claimed with u64::MAX first, which is no guest address: a reader goes on taking this path until both words
        // are in, and of two threads that see it empty only one fills it)
        if tgt
            .compare_exchange(0, u64::MAX, Ordering::AcqRel, Ordering::Acquire)
            .is_ok()
        {
            fp.store(f as u64, Ordering::Relaxed);
            tgt.store(t, Ordering::Release);
        }
    }
    unsafe { f(c) }
}

#[panic_handler]
fn panic(_: &core::panic::PanicInfo) -> ! {
    // (nothing here panics in release: no bounds-checked indexing, no unwrap; abort if that ever changes)
    extern "C" {
        fn abort() -> !;
    }
    unsafe { abort() }
}
