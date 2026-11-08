//! A .nup package: a decrypted Switch 2 dump holding titles, each with an exefs/ and a romfs/ tree.
//! The same reader as nx2/nup.py (its docstring has the layout), in Rust, with the extraction run on every core.
//!
//! Layout (little-endian):
//!   0x000  "NUP\x01" header, u64 file size at +0x40
//!   0x080  "TOC\x01", u32 titles, u64 dirs, u64 files, u64 strings offset (from 0x80), u64 strings size
//!   0x0b0  titles, 0x40 each: u64 title id, u32 root dir, u32 exefs dir, u32 romfs dir, u32 version
//!   then   dirs, 24 bytes each: u32 name offset, u32 name length, u32 first child, u32 children, u64 0
//!   then   files, 80 bytes each: u32 name offset, u32 name length, u64 size, u64 data offset, u64 stored size,
//!          32-byte hash, u8 codec (0 raw, 1 zstd), ...
//!   then   the child list, u32 each: below the dir count a dir, else (index - dirs) a file
//!   then   the name strings (not 0-terminated)
//! A file's data is stored as is, or as a zstd frame whose content is the file (codec 1, or, from older dumpers, a frame
//! that states the file's size). A romfs .zs file is zstd in the game too; it is packed only when its frame holds zstd again.

use anyhow::{bail, Context, Result};
use std::fs::{self, File};
use std::io::{self, Read, Seek, SeekFrom, Write};
use std::path::{Path, PathBuf};
use std::sync::atomic::{AtomicU64, Ordering};

const ZSTD: [u8; 4] = [0x28, 0xb5, 0x2f, 0xfd];
const CHUNK: usize = 8 << 20;

#[derive(Clone, Copy, Debug)]
pub struct Title {
    pub index: usize,
    pub id: u64,
    pub exefs: u32,
    pub romfs: u32,
    pub version: u32,
}

#[derive(Clone, Copy, Debug)]
struct Dir {
    name_off: u32,
    name_len: u32,
    first: u32,
    count: u32,
}

#[derive(Clone, Copy, Debug)]
pub struct Entry {
    name_off: u32,
    name_len: u32,
    pub size: u64,
    pub offset: u64,
    pub stored: u64,
    pub codec: u8,
}

pub struct Nup {
    pub path: PathBuf,
    pub file_size: u64,
    pub titles: Vec<Title>,
    dirs: Vec<Dir>,
    pub files: Vec<Entry>,
    kids: Vec<u32>,
    strings: Vec<u8>,
}

fn u32_at(b: &[u8], o: usize) -> u32 {
    u32::from_le_bytes(b[o..o + 4].try_into().unwrap())
}
fn u64_at(b: &[u8], o: usize) -> u64 {
    u64::from_le_bytes(b[o..o + 8].try_into().unwrap())
}

/// The content size a zstd frame header states, None when it states none
fn frame_content_size(head: &[u8]) -> Option<u64> {
    if head.len() < 18 || head[..4] != ZSTD {
        return None;
    }
    let fhd = head[4];
    let single = (fhd >> 5) & 1 != 0;
    let at = 5 + if single { 0 } else { 1 } + [0usize, 1, 2, 4][(fhd & 3) as usize];
    let n = [if single { 1 } else { 0 }, 2usize, 4, 8][(fhd >> 6) as usize];
    if n == 0 {
        return None;
    }
    let mut v = 0u64;
    for i in 0..n {
        v |= (head[at + i] as u64) << (8 * i);
    }
    Some(if n == 2 { v + 256 } else { v })
}

/// Positional reads of the package; one per thread
pub struct Reader {
    f: File,
}

impl Reader {
    fn read_at(&mut self, off: u64, buf: &mut [u8]) -> io::Result<usize> {
        self.f.seek(SeekFrom::Start(off))?;
        let mut got = 0;
        while got < buf.len() {
            let n = self.f.read(&mut buf[got..])?;
            if n == 0 {
                break;
            }
            got += n;
        }
        Ok(got)
    }
    fn section(&mut self, off: u64, len: u64) -> io::Result<io::Take<&mut File>> {
        self.f.seek(SeekFrom::Start(off))?;
        Ok((&mut self.f).take(len))
    }
}

/// Writes the frame's content into `out`, at most `limit` bytes; the count written
fn unzstd(src: impl Read, out: &mut impl Write, limit: u64) -> Result<u64> {
    let mut d = ruzstd::decoding::StreamingDecoder::new(src).map_err(|e| anyhow::anyhow!("zstd: {e}"))?;
    let mut buf = vec![0u8; CHUNK];
    let mut got = 0u64;
    loop {
        let n = match d.read(&mut buf) {
            Ok(0) => break,
            Ok(n) => n,
            Err(e) => bail!("zstd: {e}"),
        };
        got += n as u64;
        if got > limit {
            bail!("a compressed file unpacks to more than its listed {limit} bytes");
        }
        out.write_all(&buf[..n])?;
    }
    Ok(got)
}

/// How many bytes a frame unpacks to (stops past `limit`), or None when it is not a valid frame
fn unzstd_count(src: impl Read, limit: u64) -> Option<u64> {
    struct Count(u64);
    impl Write for Count {
        fn write(&mut self, b: &[u8]) -> io::Result<usize> {
            self.0 += b.len() as u64;
            Ok(b.len())
        }
        fn flush(&mut self) -> io::Result<()> {
            Ok(())
        }
    }
    let mut c = Count(0);
    unzstd(src, &mut c, limit).ok()
}

impl Nup {
    pub fn open(path: &Path) -> Result<Nup> {
        let mut f = File::open(path).with_context(|| format!("{}", path.display()))?;
        let mut h = [0u8; 0xb0];
        f.read_exact(&mut h).context("reading the package header")?;
        if &h[..4] != b"NUP\x01" {
            bail!("{}: not a NUP package", path.display());
        }
        let file_size = u64_at(&h, 0x40);
        let have = f.metadata()?.len();
        if file_size != 0 && have < file_size {
            bail!(
                "the .nup file is incomplete: it is {} MB, its header says {} MB. Download or copy it again",
                have >> 20,
                file_size >> 20
            );
        }
        if &h[0x80..0x84] != b"TOC\x01" {
            bail!("{}: no TOC at 0x80", path.display());
        }
        let nt = u32_at(&h, 0x84) as usize;
        let nd = u64_at(&h, 0x88) as usize;
        let nf = u64_at(&h, 0x90) as usize;
        let soff = u64_at(&h, 0x98) as usize;
        let ssz = u64_at(&h, 0xa0) as usize;
        let dirs_at = 0x80 + 0x30 + 0x40 * nt;
        let files_at = dirs_at + 24 * nd;
        let kids_at = files_at + 80 * nf;
        let strings_at = 0x80 + soff;
        let mut meta = vec![0u8; strings_at + ssz];
        f.seek(SeekFrom::Start(0))?;
        f.read_exact(&mut meta).context("reading the package's table of contents")?;
        let titles = (0..nt)
            .map(|i| {
                let o = 0xb0 + 0x40 * i;
                Title {
                    index: i,
                    id: u64_at(&meta, o),
                    exefs: u32_at(&meta, o + 12),
                    romfs: u32_at(&meta, o + 16),
                    version: u32_at(&meta, o + 20),
                }
            })
            .collect();
        let dirs = (0..nd)
            .map(|i| {
                let o = dirs_at + 24 * i;
                Dir { name_off: u32_at(&meta, o), name_len: u32_at(&meta, o + 4), first: u32_at(&meta, o + 8), count: u32_at(&meta, o + 12) }
            })
            .collect();
        let files = (0..nf)
            .map(|i| {
                let o = files_at + 80 * i;
                Entry {
                    name_off: u32_at(&meta, o),
                    name_len: u32_at(&meta, o + 4),
                    size: u64_at(&meta, o + 8),
                    offset: u64_at(&meta, o + 16),
                    stored: u64_at(&meta, o + 24),
                    codec: meta[o + 64],
                }
            })
            .collect();
        let kids = (0..(strings_at - kids_at) / 4).map(|i| u32_at(&meta, kids_at + 4 * i)).collect();
        Ok(Nup { path: path.to_path_buf(), file_size, titles, dirs, files, kids, strings: meta[strings_at..].to_vec() })
    }

    pub fn reader(&self) -> Result<Reader> {
        Ok(Reader { f: File::open(&self.path)? })
    }

    fn name(&self, off: u32, len: u32) -> Result<&str> {
        std::str::from_utf8(&self.strings[off as usize..(off + len) as usize]).context("a name in the package is not UTF-8")
    }

    /// (relative path, entry) for every file under dir `d`
    pub fn walk(&self, d: u32) -> Result<Vec<(String, Entry)>> {
        let mut out = Vec::new();
        self.walk_into(d, String::new(), &mut out)?;
        Ok(out)
    }

    fn walk_into(&self, d: u32, prefix: String, out: &mut Vec<(String, Entry)>) -> Result<()> {
        let dir = self.dirs[d as usize];
        for &k in &self.kids[dir.first as usize..(dir.first + dir.count) as usize] {
            let k = k as usize;
            if k < self.dirs.len() {
                let c = self.dirs[k];
                self.walk_into(k as u32, format!("{prefix}{}/", self.name(c.name_off, c.name_len)?), out)?;
            } else {
                let e = self.files[k - self.dirs.len()];
                out.push((format!("{prefix}{}", self.name(e.name_off, e.name_len)?), e));
            }
        }
        Ok(())
    }

    /// The newest title (the update over the base), or the one named by index, version or title id
    pub fn pick_title(&self, want: Option<&str>) -> Result<Title> {
        match want {
            None => Ok(*self.titles.iter().max_by_key(|t| t.version).context("the package has no titles")?),
            Some(w) => self
                .titles
                .iter()
                .find(|t| t.index.to_string() == w || t.version.to_string() == w || format!("{:016x}", t.id) == w.to_lowercase())
                .copied()
                .with_context(|| format!("no title {w}")),
        }
    }

    /// Whether the package keeps this file compressed (nx2/nup.py Nup.is_packed)
    pub fn is_packed(&self, r: &mut Reader, e: &Entry, name: &str) -> Result<bool> {
        if e.codec == 1 {
            return Ok(true);
        }
        let mut head = [0u8; 18];
        let n = r.read_at(e.offset, &mut head)?;
        if n < 4 || head[..4] != ZSTD {
            return Ok(false);
        }
        let size = frame_content_size(&head[..n]);
        if name.ends_with(".zs") && (size.is_none() || size == Some(e.size)) {
            if size != Some(e.size) {
                return Ok(false);
            }
            // packed only if what the frame holds is zstd again
            let mut first = Vec::with_capacity(4);
            struct Head<'a>(&'a mut Vec<u8>);
            impl Write for Head<'_> {
                fn write(&mut self, b: &[u8]) -> io::Result<usize> {
                    let take = (4 - self.0.len().min(4)).min(b.len());
                    self.0.extend_from_slice(&b[..take]);
                    Ok(b.len())
                }
                fn flush(&mut self) -> io::Result<()> {
                    Ok(())
                }
            }
            let src = r.section(e.offset, e.size.min(1 << 20))?;
            let _ = unzstd(src, &mut Head(&mut first), u64::MAX);
            return Ok(first[..] == ZSTD);
        }
        if let Some(s) = size {
            return Ok(s == e.size);
        }
        let src = r.section(e.offset, e.size)?;
        Ok(unzstd_count(src, e.size) == Some(e.size))
    }

    /// The file, as itself, into `out` (nx2/nup.py Nup.copy_to)
    pub fn copy_to(&self, r: &mut Reader, e: &Entry, name: &str, out: &mut impl Write) -> Result<()> {
        if self.is_packed(r, e, name)? {
            let span = if e.codec == 1 { e.stored } else { e.size + (1 << 16) };
            let got = unzstd(r.section(e.offset, span)?, out, e.size)?;
            if got != e.size {
                bail!("{name}: a compressed file came out as {got} bytes, {} expected", e.size);
            }
            return Ok(());
        }
        let mut left = e.size;
        let mut at = e.offset;
        let mut buf = vec![0u8; CHUNK.min(e.size as usize).max(1)];
        while left > 0 {
            let want = (left as usize).min(buf.len());
            let n = r.read_at(at, &mut buf[..want])?;
            if n == 0 {
                bail!("the .nup file ends early (short read at {} MB): it is incomplete. Download or copy it again", at >> 20);
            }
            out.write_all(&buf[..n])?;
            left -= n as u64;
            at += n as u64;
        }
        Ok(())
    }

    pub fn read(&self, r: &mut Reader, e: &Entry, name: &str) -> Result<Vec<u8>> {
        let mut v = Vec::with_capacity(e.size as usize);
        self.copy_to(r, e, name, &mut v)?;
        Ok(v)
    }
}

const WINDOWS_DEVICES: [&str; 6] = ["CON", "CONIN$", "CONOUT$", "PRN", "AUX", "NUL"];

/// A package path kept under `out/part`: no empty, dot or trailing-dot parts, no characters Windows refuses, no
/// device names (nx2/nup.py extraction_path)
pub fn extraction_path(out: &Path, part: &str, rel: &str) -> Result<PathBuf> {
    let bad_part = |p: &str| {
        let stem = p.split('.').next().unwrap_or("").trim_end_matches(' ').to_uppercase();
        p.is_empty()
            || p == "."
            || p == ".."
            || p.ends_with(' ')
            || p.ends_with('.')
            || p.chars().any(|c| "<>:\"|?*".contains(c) || (c as u32) < 32)
            || WINDOWS_DEVICES.contains(&stem.as_str())
            || ((stem.starts_with("COM") || stem.starts_with("LPT"))
                && stem.len() == 4
                && "123456789¹²³".contains(stem.chars().nth(3).unwrap_or('x')))
    };
    if rel.is_empty() || rel.contains('\\') || rel.split('/').any(bad_part) {
        bail!("unsafe path in the package: {rel:?}");
    }
    let mut p = out.join(part);
    for s in rel.split('/') {
        p.push(s);
    }
    Ok(p)
}

pub struct Extracted {
    pub files: u64,
    pub bytes: u64,
    pub skipped: u64,
}

/// Extracts the title's exefs and romfs (or one of them) under `out`, in parallel. A file already there at its size is
/// kept (a re-run picks up where it stopped); each is written to <name>.part and renamed when complete.
pub fn extract(nup: &Nup, t: &Title, out: &Path, only: Option<&str>, progress: impl Fn(u64, u64) + Sync) -> Result<Extracted> {
    use rayon::prelude::*;
    let mut jobs = Vec::new();
    for (part, dir) in [("exefs", t.exefs), ("romfs", t.romfs)] {
        if only.is_some_and(|o| o != part) {
            continue;
        }
        for (rel, e) in nup.walk(dir)? {
            jobs.push((extraction_path(out, part, &rel)?, rel, e));
        }
    }
    // the directories first, once each
    let mut dirs: Vec<&Path> = jobs.iter().filter_map(|(p, _, _)| p.parent()).collect();
    dirs.sort();
    dirs.dedup();
    for d in dirs {
        fs::create_dir_all(d).with_context(|| format!("creating {}", d.display()))?;
    }
    // biggest first, so one large file does not finish alone at the end
    jobs.sort_by_key(|(_, _, e)| std::cmp::Reverse(e.size));
    let (files, bytes, skipped) = (AtomicU64::new(0), AtomicU64::new(0), AtomicU64::new(0));
    jobs.par_iter().try_for_each_init(
        || nup.reader(),
        |r, (dst, rel, e)| -> Result<()> {
            let r = r.as_mut().map_err(|e| anyhow::anyhow!("{e}"))?;
            if fs::metadata(dst).map(|m| m.len() == e.size).unwrap_or(false) {
                skipped.fetch_add(1, Ordering::Relaxed);
                return Ok(());
            }
            let mut tmp = dst.clone().into_os_string();
            tmp.push(".part");
            let tmp = PathBuf::from(tmp);
            {
                let mut g = io::BufWriter::with_capacity(1 << 20, File::create(&tmp).with_context(|| format!("{}", tmp.display()))?);
                nup.copy_to(r, e, rel, &mut g).with_context(|| rel.clone())?;
                g.flush()?;
            }
            fs::rename(&tmp, dst).with_context(|| format!("{}", dst.display()))?;
            let n = files.fetch_add(1, Ordering::Relaxed) + 1;
            let b = bytes.fetch_add(e.size, Ordering::Relaxed) + e.size;
            if n % 5000 == 0 {
                progress(n, b);
            }
            Ok(())
        },
    )?;
    Ok(Extracted { files: files.into_inner(), bytes: bytes.into_inner(), skipped: skipped.into_inner() })
}
