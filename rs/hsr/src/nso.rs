//! Rewrites an exefs's ZBIC-compressed NSOs (flags bit 7, firmware 22.0.0+) as plain uncompressed NSOs, in place, the
//! same as nx2/unpack.py: the header is kept (segment offsets and sizes rewritten, compression bits cleared, hash bits
//! kept), the bytes between the header and the first segment (the module name) are kept, then the segments follow.
//! The segment hashes at 0xa0 are of the decompressed data, so they stay valid; they are checked here.

use crate::zbic;
use anyhow::{bail, Result};
use sha2::{Digest, Sha256};
use std::fs;
use std::path::Path;

const ZBIC_FLAG: u32 = 1 << 7;
pub const NAMES: [&str; 13] = [
    "rtld", "main", "subsdk0", "subsdk1", "subsdk2", "subsdk3", "subsdk4", "subsdk5", "subsdk6",
    "subsdk7", "subsdk8", "subsdk9", "sdk",
];

pub enum Unpacked {
    NotNso,
    Kept,
    Plain,
}

fn u32_at(b: &[u8], o: usize) -> u32 {
    u32::from_le_bytes(b[o..o + 4].try_into().unwrap())
}

pub fn unpack_nso(path: &Path) -> Result<Unpacked> {
    let raw = fs::read(path)?;
    if raw.len() < 0x100 || &raw[..4] != b"NSO0" {
        return Ok(Unpacked::NotNso);
    }
    let flags = u32_at(&raw, 0xc);
    if flags & ZBIC_FLAG == 0 {
        return Ok(Unpacked::Kept);
    }
    let segs: Vec<(usize, usize)> = (0..3)
        .map(|i| {
            (
                u32_at(&raw, 0x10 + 0x10 * i) as usize,
                u32_at(&raw, 0x18 + 0x10 * i) as usize,
            )
        })
        .collect();
    // the three segments decompress in parallel: main's text is most of the time
    let datas: Vec<Result<Vec<u8>>> = std::thread::scope(|s| {
        let hs: Vec<_> = (0..3)
            .map(|i| {
                let raw = &raw;
                let (foff, size) = segs[i];
                s.spawn(move || -> Result<Vec<u8>> {
                    let cs = u32_at(raw, 0x60 + 4 * i) as usize;
                    let d = &raw[foff..foff + cs];
                    let d = if flags & (1 << i) != 0 {
                        zbic::decompress(d, size)?
                    } else {
                        d.to_vec()
                    };
                    if d.len() != size {
                        bail!("segment {i}: {} bytes, {size} expected", d.len());
                    }
                    if flags & (8 << i) != 0
                        && Sha256::digest(&d)[..] != raw[0xa0 + 0x20 * i..0xc0 + 0x20 * i]
                    {
                        bail!("segment {i}: hash mismatch after ZBIC");
                    }
                    Ok(d)
                })
            })
            .collect();
        hs.into_iter().map(|h| h.join().unwrap()).collect()
    });
    let mut hdr = raw[..0x100].to_vec();
    let first = segs.iter().map(|s| s.0).min().unwrap();
    let mut body = raw[0x100..first].to_vec();
    for (i, d) in datas.into_iter().enumerate() {
        let d = d.map_err(|e| anyhow::anyhow!("{}: {e}", path.display()))?;
        hdr[0x10 + 0x10 * i..0x14 + 0x10 * i]
            .copy_from_slice(&(0x100 + body.len() as u32).to_le_bytes());
        hdr[0x60 + 4 * i..0x64 + 4 * i].copy_from_slice(&(d.len() as u32).to_le_bytes());
        body.extend_from_slice(&d);
    }
    hdr[0xc..0x10].copy_from_slice(&(flags & !(ZBIC_FLAG | 7)).to_le_bytes());
    let mut tmp = path.as_os_str().to_owned();
    tmp.push(".plain");
    hdr.extend_from_slice(&body);
    fs::write(&tmp, &hdr)?;
    fs::rename(&tmp, path)?;
    Ok(Unpacked::Plain)
}
