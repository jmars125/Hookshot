//! `hsr discover <exefs> [--standalone [--host <host dir>]] [--aslr <hex>] [--out <dir>]`: the image and the function
//! discovery of nx2/lift.py (without --write); --out writes image.bin and functions.json, byte-comparable with the
//! Python lift's <work>/image (or <work>/sa/image with --standalone).

use crate::{image, lift};
use anyhow::{bail, Result};
use std::collections::BTreeSet;
use std::path::PathBuf;
use std::time::Instant;

pub fn run(args: &[String]) -> Result<()> {
    let mut it = args.iter();
    let Some(exefs) = it.next().map(PathBuf::from) else {
        bail!("usage: hsr discover <exefs> [options]")
    };
    let (mut out, mut host, mut aslr, mut standalone) = (None, PathBuf::from("host"), 0u64, false);
    while let Some(a) = it.next() {
        let mut val = || {
            it.next()
                .cloned()
                .ok_or_else(|| anyhow::anyhow!("{a} wants a value"))
        };
        match a.as_str() {
            "--out" => out = Some(PathBuf::from(val()?)),
            "--host" => host = PathBuf::from(val()?),
            "--aslr" => aslr = u64::from_str_radix(val()?.trim_start_matches("0x"), 16)?,
            "--standalone" => standalone = true,
            _ => bail!("unknown option {a}"),
        }
    }
    let t0 = Instant::now();
    let modules = image::load_exefs(&exefs)?;
    let hle = if standalone {
        image::hle_set(&modules, &host)?
    } else {
        BTreeSet::new()
    };
    let img = image::Image::eden(modules, aslr, &hle)?;
    let mods: Vec<String> = img
        .modules
        .iter()
        .map(|m| format!("{}@{:#x}+{:#x}", m.name, m.base, m.image.len()))
        .collect();
    println!(
        "image: {}; {} entries, {} trampolines, {} hooks ({:.1}s)",
        mods.join(", "),
        img.entries.len(),
        img.tramp.len(),
        img.hooks.len(),
        t0.elapsed().as_secs_f64()
    );
    let adr: BTreeSet<u64> = img
        .adr_entries()
        .into_iter()
        .filter(|e| img.in_code(*e) && !img.entries.contains(e))
        .collect();
    println!("ADR / ADRP+ADD code addresses: {} new entries", adr.len());
    let mut d = lift::discover::Discovery::new(&img, &adr);
    d.discover();
    let insns: usize = d.funcs.values().map(|b| b.insns.len()).sum();
    println!(
        "discovered {} functions, {} instructions, {} jump tables, {} BRs without a table, {} PLT stubs, {} setjmp \
         owners ({:.1}s)",
        d.funcs.len(),
        insns,
        d.jt.len(),
        d.jt_missing().len(),
        d.plt.len(),
        d.setjmp_owner.len(),
        t0.elapsed().as_secs_f64()
    );
    println!("{:?}", d.stats);
    if let Some(out) = out {
        std::fs::create_dir_all(&out)?;
        std::fs::write(out.join("image.bin"), &img.mem)?;
        std::fs::write(out.join("functions.json"), d.functions_json())?;
        println!("wrote {}", out.display());
    }
    Ok(())
}
