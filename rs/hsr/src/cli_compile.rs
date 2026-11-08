//! `hsr compile <exefs> [--standalone [--host <dir>]] [--aslr <hex>] [--out <dir>] [--chunks N] [--limit N]`:
//! discovery, then every function lifted by Cranelift into object files, in parallel.
//!
//! Pass 1 checks every function (does each instruction translate, which registers it writes); the ones that do form
//! the Rust set, whose t_ bodies the others call and tail-call directly. Pass 2 builds them in `chunks` object files
//! (chunk_<i>.o, consecutive functions by address), one Cranelift ObjectModule per chunk, on all cores. The functions
//! that do not translate are listed in fallback.txt for the C lifter. --limit compiles only the first N functions
//! (a quick timing sample).

use crate::image;
use crate::lift::func::{Builder, Code, Program};
use crate::lift::{discover::Discovery, isa};
use anyhow::{bail, Result};
use cranelift_module::default_libcall_names;
use cranelift_object::{ObjectBuilder, ObjectModule};
use rayon::prelude::*;
use std::collections::{BTreeSet, HashSet};
use std::path::PathBuf;
use std::sync::Arc;
use std::time::Instant;

struct ImgCode<'a>(&'a image::Image);

impl Code for ImgCode<'_> {
    fn word(&self, pc: u64) -> Option<u32> {
        self.0.in_code(pc).then(|| self.0.word(pc))
    }
    fn read_u64(&self, a: u64) -> Option<u64> {
        self.0
            .read(a, 8)
            .map(|b| u64::from_le_bytes(b.try_into().unwrap()))
    }
}

pub fn run(args: &[String]) -> Result<()> {
    let mut it = args.iter();
    let Some(exefs) = it.next().map(PathBuf::from) else {
        bail!("usage: hsr compile <exefs> [options]")
    };
    let (mut out, mut host, mut aslr, mut standalone) = (None, PathBuf::from("host"), 0u64, false);
    let (mut chunks, mut limit, mut check_only) = (256usize, usize::MAX, false);
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
            "--chunks" => chunks = val()?.parse()?,
            "--limit" => limit = val()?.parse()?,
            "--standalone" => standalone = true,
            "--check" => check_only = true,
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
    let adr: BTreeSet<u64> = img
        .adr_entries()
        .into_iter()
        .filter(|e| img.in_code(*e) && !img.entries.contains(e))
        .collect();
    let mut d = Discovery::new(&img, &adr);
    d.discover();
    let t_disc = t0.elapsed().as_secs_f64();
    println!("discovered {} functions ({t_disc:.1}s)", d.funcs.len());

    let code = ImgCode(&img);
    let p = Program {
        code: &code,
        entries: d.entries.clone(),
        jt: d.jt.clone(),
        plt: d.plt.clone(),
        setjmp_owners: d.setjmp_owner.keys().copied().collect(),
        polls: true,
    };
    let mut funcs: Vec<u64> = d.order.clone();
    funcs.sort_unstable();
    funcs.truncate(limit);
    let frontend = isa().frontend_config();

    // pass 1: what translates
    let t1 = Instant::now();
    let checked: Vec<(u64, Option<u64>)> = funcs
        .par_iter()
        .map_init(
            || Builder::new(&p, frontend),
            |b, &e| (e, b.check(e, &d.funcs[&e]).ok()),
        )
        .collect();
    let rust: HashSet<u64> = checked
        .iter()
        .filter(|c| c.1.is_some())
        .map(|c| c.0)
        .collect();
    let fallback: Vec<u64> = checked
        .iter()
        .filter(|c| c.1.is_none())
        .map(|c| c.0)
        .collect();
    let insns: usize = rust.iter().map(|e| d.funcs[e].insns.len()).sum();
    let all_insns: usize = funcs.iter().map(|e| d.funcs[e].insns.len()).sum();
    println!(
        "check: {} of {} functions translate ({:.2}%), {} of {} instructions; {} for the C fallback ({:.1}s)",
        rust.len(),
        funcs.len(),
        100.0 * rust.len() as f64 / funcs.len().max(1) as f64,
        insns,
        all_insns,
        fallback.len(),
        t1.elapsed().as_secs_f64()
    );
    // what sends functions to the fallback: the first untranslated word of each, by its top 11 bits
    {
        let mut b = Builder::new(&p, frontend);
        let mut by: std::collections::HashMap<u32, (usize, usize, u32)> = Default::default();
        for &e in &fallback {
            let body = &d.funcs[&e];
            let w = body
                .insns
                .values()
                .filter(|(k, _)| matches!(k, crate::lift::func::Kind::Op))
                .map(|&(_, w)| w)
                .find(|&w| !b.translates(w));
            let key = w.map_or(u32::MAX, |w| w >> 21);
            let x = by.entry(key).or_insert((0, 0, w.unwrap_or(0)));
            x.0 += 1;
            x.1 += body.insns.len();
        }
        let mut by: Vec<_> = by.into_iter().collect();
        by.sort_by_key(|x| std::cmp::Reverse(x.1 .1));
        for (k, (nf, ni, w)) in by.iter().take(25) {
            println!("  fallback group {k:03x}: {nf} functions, {ni} instructions (e.g. {w:08x})");
        }
    }
    if check_only {
        return Ok(());
    }
    let rust = Arc::new(rust);

    // pass 2: compile
    let t2 = Instant::now();
    let work: Vec<(u64, u64)> = checked
        .iter()
        .filter_map(|&(e, m)| m.map(|m| (e, m)))
        .collect();
    let per = work.len().div_ceil(chunks.max(1)).max(1);
    let results: Vec<Result<(usize, Vec<(u64, String)>, usize)>> = work
        .par_chunks(per)
        .enumerate()
        .map(|(i, part)| {
            let ob = ObjectBuilder::new(isa(), format!("chunk_{i}"), default_libcall_names())?;
            let mut m = ObjectModule::new(ob);
            let mut b = Builder::new(&p, frontend);
            b.rust = rust.clone();
            let mut errs = Vec::new();
            for &(e, dirty) in part {
                if let Err(err) = b.define(&mut m, e, &d.funcs[&e], dirty) {
                    errs.push((
                        e,
                        format!("{err:#}").lines().next().unwrap_or("").to_string(),
                    ));
                }
            }
            let bytes = m.finish().emit()?;
            let n = bytes.len();
            if let Some(out) = &out {
                std::fs::write(out.join(format!("chunk_{i}.o")), bytes)?;
            }
            Ok((part.len(), errs, n))
        })
        .collect();
    let (mut n, mut size, mut errs) = (0, 0, Vec::new());
    for r in results {
        let (k, e, s) = r?;
        n += k;
        size += s;
        errs.extend(e);
    }
    println!(
        "compiled {} functions into {} objects, {:.1} MB, {} errors ({:.1}s; total {:.1}s)",
        n,
        work.len().div_ceil(per),
        size as f64 / 1e6,
        errs.len(),
        t2.elapsed().as_secs_f64(),
        t0.elapsed().as_secs_f64()
    );
    for (e, msg) in errs.iter().take(10) {
        println!("  {e:#x}: {msg}");
    }
    if let Some(out) = &out {
        let mut fb: Vec<u64> = fallback
            .iter()
            .copied()
            .chain(errs.iter().map(|x| x.0))
            .collect();
        fb.sort_unstable();
        let text: String = fb.iter().map(|e| format!("{e:x}\n")).collect();
        std::fs::write(out.join("fallback.txt"), text)?;
    }
    Ok(())
}
