//! hsr: Hookshot's build tool in Rust. This first part replaces nx2/nup.py; the lift and code generation follow.
//!
//!   hsr nup <file.nup>                                   list the titles and their trees
//!   hsr nup <file.nup> --extract <dir> [--title <t>] [--only exefs|romfs]
//!   hsr unpack <exefs dir>                               ZBIC modules -> plain NSOs
//!   hsr nup <file.nup> --cat <title>:<exefs|romfs>/<path> <out>   one file, as itself

mod lift;
mod nso;
mod nup;
mod zbic;

use anyhow::{bail, Context, Result};
use std::path::{Path, PathBuf};
use std::time::Instant;

fn usage() -> ! {
    eprintln!("usage: hsr nup <file.nup> [--extract <dir>] [--title <index|version|id>] [--only exefs|romfs]");
    eprintln!("       hsr nup <file.nup> --cat <exefs|romfs>/<path> <out> [--title <t>]");
    std::process::exit(2)
}

fn cmd_nup(args: &[String]) -> Result<()> {
    let mut it = args.iter();
    let path = PathBuf::from(it.next().unwrap_or_else(|| usage()));
    let (mut extract, mut title, mut only, mut cat) = (None, None, None, None);
    while let Some(a) = it.next() {
        match a.as_str() {
            "--extract" => extract = Some(PathBuf::from(it.next().unwrap_or_else(|| usage()))),
            "--title" => title = Some(it.next().unwrap_or_else(|| usage()).clone()),
            "--only" => only = Some(it.next().unwrap_or_else(|| usage()).clone()),
            "--cat" => {
                let what = it.next().unwrap_or_else(|| usage()).clone();
                let out = PathBuf::from(it.next().unwrap_or_else(|| usage()));
                cat = Some((what, out));
            }
            _ => usage(),
        }
    }
    let t0 = Instant::now();
    let nup = nup::Nup::open(&path)?;
    let name = path.file_name().map(|s| s.to_string_lossy().into_owned()).unwrap_or_default();
    println!("{}: {:.2} GB, {} titles, {} files", name, nup.file_size as f64 / 1e9, nup.titles.len(), nup.files.len());
    if let Some((what, out)) = cat {
        let t = nup.pick_title(title.as_deref())?;
        let (part, rel) = what.split_once('/').context("--cat wants exefs/<path> or romfs/<path>")?;
        let dir = match part {
            "exefs" => t.exefs,
            "romfs" => t.romfs,
            _ => bail!("--cat wants exefs/<path> or romfs/<path>"),
        };
        let (_, e) = nup.walk(dir)?.into_iter().find(|(p, _)| p == rel).with_context(|| format!("no {what} in title {}", t.index))?;
        let mut r = nup.reader()?;
        std::fs::write(&out, nup.read(&mut r, &e, rel)?)?;
        return Ok(());
    }
    if extract.is_none() {
        for t in &nup.titles {
            let ex = nup.walk(t.exefs)?;
            let ro = nup.walk(t.romfs)?;
            let exs: Vec<String> = ex.iter().map(|(p, e)| format!("{p} {}", e.size)).collect();
            println!(
                "title {}: {:016x} v{}  exefs {} files ({})  romfs {} files, {:.2} GB",
                t.index,
                t.id,
                t.version,
                ex.len(),
                exs.join(", "),
                ro.len(),
                ro.iter().map(|(_, e)| e.size).sum::<u64>() as f64 / 1e9
            );
        }
        return Ok(());
    }
    let out = extract.unwrap();
    let t = nup.pick_title(title.as_deref())?;
    println!("extracting title {} (v{}) to {}", t.index, t.version, out.display());
    let r = nup::extract(&nup, &t, Path::new(&out), only.as_deref(), |n, b| {
        println!("  {n} files, {:.1} GB", b as f64 / 1e9);
    })?;
    let secs = t0.elapsed().as_secs_f64();
    if only.as_deref() != Some("romfs") {
        cmd_unpack(&out.join("exefs").to_string_lossy())?;
    }
    println!(
        "done: {} files written ({} already there), {:.2} GB in {:.1} s ({:.0} MB/s)",
        r.files,
        r.skipped,
        r.bytes as f64 / 1e9,
        secs,
        r.bytes as f64 / 1e6 / secs.max(0.001)
    );
    Ok(())
}

/// The exefs's ZBIC modules as plain NSOs (nx2/unpack.py)
fn cmd_unpack(exefs: &str) -> Result<()> {
    for n in nso::NAMES {
        let p = Path::new(exefs).join(n);
        if p.exists() {
            let r = nso::unpack_nso(&p)?;
            println!("{n:<8} {}", match r {
                nso::Unpacked::NotNso => "not an NSO",
                nso::Unpacked::Kept => "not ZBIC, kept",
                nso::Unpacked::Plain => "ZBIC -> plain",
            });
        }
    }
    Ok(())
}

/// Runs rs/tests/difftest.py's cases through the Cranelift translation
fn cmd_difftest(args: &[String]) -> Result<()> {
    let [input, output] = args else { usage() };
    let t0 = Instant::now();
    let (out, run, skipped) = lift::difftest(&std::fs::read(input)?)?;
    std::fs::write(output, out)?;
    println!("{run} cases run, {skipped} not translated, {:.2} s", t0.elapsed().as_secs_f64());
    Ok(())
}

/// How much of a code dump (raw little-endian words) the Rust translation covers
fn cmd_cover(args: &[String]) -> Result<()> {
    let [path] = args else { usage() };
    let raw = std::fs::read(path)?;
    let words: Vec<u32> = raw.chunks_exact(4).map(|c| u32::from_le_bytes(c.try_into().unwrap())).collect();
    let t0 = Instant::now();
    let (ok, bad, by) = lift::coverage(&words);
    let control = words.iter().filter(|&&w| lift::control_kind(w).is_some()).count();
    println!("{} words: {control} control flow, {ok} translated, {bad} not ({:.1}% of the rest), {:.1} s",
             words.len(), 100.0 * ok as f64 / (ok + bad).max(1) as f64, t0.elapsed().as_secs_f64());
    for (top, n) in by.iter().take(25) {
        println!("  {:03x}xxxxx  {n}", top << 1);
    }
    Ok(())
}

fn main() {
    let args: Vec<String> = std::env::args().skip(1).collect();
    let r = match args.first().map(String::as_str) {
        Some("nup") => cmd_nup(&args[1..]),
        Some("cover") => cmd_cover(&args[1..]),
        Some("difftest") => cmd_difftest(&args[1..]),
        Some("unpack") => cmd_unpack(args.get(1).map(String::as_str).unwrap_or_else(|| usage())),
        _ => usage(),
    };
    if let Err(e) = r {
        eprintln!("error: {e:#}");
        std::process::exit(1);
    }
}
