//! Windows x64 unwind tables for an object file of lifted functions.
//!
//! cranelift-object writes no `.pdata` / `.xdata` for COFF, and without them Windows cannot unwind through a lifted
//! frame: an exception raised in lifted code (a fault the host's handlers report, a guest longjmp's host unwind) found
//! no handler and the process ended with no report. Each function's UNWIND_INFO (from Cranelift's
//! `create_unwind_info`) goes to `.xdata`, and a RUNTIME_FUNCTION (begin, end, unwind info: image-relative 32-bit
//! addresses) per function to `.pdata`, which the linker gathers into the image's exception directory.

use cranelift_codegen::isa::unwind::UnwindInfo;
use cranelift_codegen::Context;
use cranelift_module::{FuncId, Module};
use cranelift_object::object::write::{Relocation, SectionKind};
use cranelift_object::object::{pe, RelocationFlags};
use cranelift_object::ObjectProduct;

/// One function's code size and its encoded UNWIND_INFO
pub struct Entry {
    func: FuncId,
    size: u32,
    xdata: Vec<u8>,
}

/// The unwind entry of the function just compiled in `cx` (after define_function), or None off Windows
pub fn entry<M: Module>(m: &M, cx: &Context, func: FuncId) -> Option<Entry> {
    let code = cx.compiled_code()?;
    let Ok(Some(UnwindInfo::WindowsX64(u))) = code.create_unwind_info(m.isa()) else {
        return None;
    };
    let mut xdata = vec![0; u.emit_size()];
    u.emit(&mut xdata);
    Some(Entry {
        func,
        size: code.code_buffer().len() as u32,
        xdata,
    })
}

/// Adds .xdata and .pdata for `entries` to the finished object
pub fn add(p: &mut ObjectProduct, entries: &[Entry]) -> anyhow::Result<()> {
    if entries.is_empty() {
        return Ok(());
    }
    let obj = &mut p.object;
    let xdata = obj.add_section(Vec::new(), b".xdata".to_vec(), SectionKind::ReadOnlyData);
    let pdata = obj.add_section(Vec::new(), b".pdata".to_vec(), SectionKind::ReadOnlyData);
    let xsym = obj.section_symbol(xdata);
    let addr32nb = RelocationFlags::Coff {
        typ: pe::IMAGE_REL_AMD64_ADDR32NB,
    };
    let mut rows = Vec::with_capacity(entries.len() * 12);
    let mut relocs = Vec::with_capacity(entries.len() * 3);
    for e in entries {
        // (UNWIND_INFO is 4-byte aligned)
        let off = obj.append_section_data(xdata, &e.xdata, 4);
        let sym = p.functions[e.func]
            .ok_or_else(|| anyhow::anyhow!("an unwind entry for a function the object does not have"))?
            .0;
        let row = rows.len() as u64;
        rows.extend_from_slice(&[0u8; 12]);
        for (at, symbol, addend) in [(0, sym, 0), (4, sym, e.size as i64), (8, xsym, off as i64)] {
            relocs.push(Relocation {
                offset: row + at,
                symbol,
                addend,
                flags: addr32nb,
            });
        }
    }
    obj.append_section_data(pdata, &rows, 4);
    for r in relocs {
        obj.add_relocation(pdata, r)?;
    }
    Ok(())
}
