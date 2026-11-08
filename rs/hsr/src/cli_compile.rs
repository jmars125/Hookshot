//! `hsr compile <exefs> [--standalone [--host <dir>]] [--aslr <hex>] [--out <dir>] [--chunks N] [--limit N] [--gen <dir>]`:
//! discovery, then every function lifted by Cranelift into object files, in parallel.
//!
//! Pass 1 checks every function (does each instruction translate, which registers it writes); the ones that do form
//! the Rust set, whose t_ bodies the others call and tail-call directly. Pass 2 builds them in `chunks` object files
//! (chunk_<i>.o, consecutive functions by address), one Cranelift ObjectModule per chunk, on all cores. The functions
//! that do not translate are listed in fallback.txt for the C lifter. --limit compiles only the first N functions
//! (a quick timing sample).
//!
//! --gen <dir>: the C lifter's output for the same image (work/sa/gen). The functions the game patches edit in C (they
//! use the host's hos_* state) stay C, and the C of every fallback function is written beside the objects as
//! fallback_<i>.c (cfallback.rs), one file per core, for the hybrid link. Each object carries Windows unwind tables
//! (winunwind.rs) and its guest stores mark a64_dirty, as the C build's do.

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
    let mut gen: Option<PathBuf> = None;
    // (bisection of a wrong-but-running build: only functions at [lo, hi) and without a word matching a denied
    // (mask, value) are lifted to Rust; the rest stay with the C build's objects, linked after with /force:multiple)
    let mut range: Vec<(u64, u64)> = vec![(0, u64::MAX)];
    let mut deny: Vec<(u32, u32)> = Vec::new();
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
            "--gen" => gen = Some(PathBuf::from(val()?)),
            "--range" => {
                let v = val()?;
                range = v
                    .split(',')
                    .map(|r| {
                        let (lo, hi) = r.split_once('-').ok_or_else(|| anyhow::anyhow!("--range lo-hi[,lo-hi...]"))?;
                        Ok((u64::from_str_radix(lo, 16)?, u64::from_str_radix(hi, 16)?))
                    })
                    .collect::<Result<_>>()?;
            }
            "--deny" => {
                let v = val()?;
                let (m, w) = v.split_once('=').ok_or_else(|| anyhow::anyhow!("--deny mask=value"))?;
                deny.push((u32::from_str_radix(m, 16)?, u32::from_str_radix(w, 16)?));
            }
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
    // the C lifter's output: the patched functions stay C, and the fallback's C comes from it
    let cgen = match &gen {
        Some(g) => {
            let t = Instant::now();
            let cg = crate::cfallback::Gen::scan(g)?;
            println!("read {} C functions from {} ({:.1}s)", cg.len(), g.display(), t.elapsed().as_secs_f64());
            Some(cg)
        }
        None => None,
    };
    let forced: HashSet<u64> = cgen.as_ref().map(|g| g.patched()).unwrap_or_default();
    if !forced.is_empty() {
        println!("{} patched functions stay C", forced.len());
    }

    // pass 1: what translates
    let t1 = Instant::now();
    let checked: Vec<(u64, Option<u64>)> = funcs
        .par_iter()
        .map_init(
            || Builder::new(&p, frontend),
            |b, &e| {
                let body = &d.funcs[&e];
                let ok = !forced.contains(&e)
                    && range.iter().any(|r| (r.0..r.1).contains(&e))
                    && !body.insns.values().any(|&(_, w)| deny.iter().any(|&(m, v)| w & m == v));
                (e, if ok { b.check(e, &d.funcs[&e]).ok() } else { None })
            },
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
    crate::lift::a64::COLOCATED.store(true, std::sync::atomic::Ordering::Relaxed);
    crate::lift::a64::WRITE_TRACK.store(true, std::sync::atomic::Ordering::Relaxed);

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
            b.unwind = Some(Vec::new());
            let mut errs = Vec::new();
            for &(e, dirty) in part {
                if let Err(err) = b.define(&mut m, e, &d.funcs[&e], dirty) {
                    errs.push((
                        e,
                        format!("{err:#}").lines().next().unwrap_or("").to_string(),
                    ));
                }
            }
            let mut prod = m.finish();
            crate::lift::winunwind::add(&mut prod, b.unwind.as_deref().unwrap_or_default())?;
            let bytes = prod.emit()?;
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
        if let Some(cg) = &cgen {
            let want: BTreeSet<u64> = fb.iter().copied().collect();
            let parts = std::thread::available_parallelism().map_or(8, |n| n.get());
            let missing = cg.write(&want, out, parts)?;
            println!("wrote {parts} fallback_<i>.c: {} functions", want.len() - missing.len());
            if !missing.is_empty() {
                bail!("the C output has no definition of {} fallback functions (e.g. {:#x})", missing.len(), missing[0]);
            }
        }
    }
    Ok(())
}
