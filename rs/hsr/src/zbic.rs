//! ZBIC, the zstd variant Switch 2 NSOs (and system 22.0.0 and later) store their segments in.
//!
//! A ZBIC frame is a zstd frame (RFC 8878) with two differences: the frame magic is "ZBIC" (0x4349425A), and every FSE
//! table description (the Huffman weight table of compressed literals, and the literal length, offset and match length
//! tables of the sequences section) is written with binary interpolative coding instead of the NCount format of RFC 8878
//! section 4.1.1. Everything else is unchanged, so each frame is rewritten into a standard zstd frame (only the table
//! descriptions and the size fields around them change) and decoded by ruzstd.
//!
//! The interpolative table: byte 0 holds a low-probability flag (bit 7) and the payload length n (bits 0-6); bytes 1..n
//! are a number read from the last byte towards the first through a 40-bit window refilled a byte at a time while it
//! holds less than 2^40. From it come, by successive division, the largest symbol (mod 52), the accuracy log minus 5
//! (mod 8), the cumulative total, then the cumulative counts at the midpoints of a binary subdivision of the symbol
//! range (right half before left half; a span whose ends are equal is filled with that value, one whose ends differ is
//! decoded mod (difference + 1)). Counts are the differences of the cumulative values, less one with the low flag set.
//!
//! Written from the format as worked out for oot-recomp's recomp/zbic.py (the same author's MIT code), not from
//! kinnay/zbic.

use anyhow::{anyhow, bail, Result};
use std::io::Read;

const ZBIC_MAGIC: u32 = 0x4349425A;
const ZSTD_MAGIC: u32 = 0xFD2FB528;
const MAX_SV_LL: usize = 35;
const MAX_SV_OF: usize = 31;
const MAX_SV_ML: usize = 52;
const MAX_SV_HUF: usize = 255;
const BLOCK_MAX: usize = 1 << 17;

/// (middle, first, last) of every span of [lo, hi] wider than one: node, then right half, then left half
fn subdivision(lo: usize, hi: usize, out: &mut Vec<(usize, usize, usize)>) {
    if hi - lo < 2 {
        return;
    }
    let mid = (lo + hi) / 2;
    out.push((mid, lo, hi));
    subdivision(mid, hi, out);
    subdivision(lo, mid, out);
}

/// Decodes one interpolative table at buf[pos..end]: (counts, accuracy log, bytes used)
fn read_ncount_bic(
    buf: &[u8],
    pos: usize,
    end: usize,
    max_sv: usize,
) -> Result<(Vec<i32>, u32, usize)> {
    if pos >= end {
        bail!("table description past the end of the block");
    }
    let head = buf[pos];
    let n = (head & 0x7f) as usize;
    let low = (head >> 7) as i64;
    if n + 1 >= end - pos {
        bail!("table description longer than its block");
    }
    let mut left = n;
    let mut refill = |mut v: u64| -> u64 {
        while left > 0 && (v >> 32) < 0x100 {
            v = buf[pos + left] as u64 | (v << 8);
            left -= 1;
        }
        v
    };
    let mut v = refill(0);
    let max_symbol = (v % 0x34) as usize;
    v = refill(v / 0x34);
    if max_symbol + 1 > max_sv {
        bail!("table has more symbols than its alphabet");
    }
    let log = (v & 7) as u32 + 5;
    v = refill(v >> 3);
    let total = 1u64 << log;
    let rest = v % total;
    v = refill(v / total);
    let last = rest as i64 + 1 + if low != 0 { max_symbol as i64 + 2 } else { 0 };
    let mut p = 1usize;
    while p <= max_symbol + 1 {
        p <<= 1;
    }
    if p > 0x80 {
        bail!("table too wide");
    }
    let mut plan = Vec::new();
    subdivision(0, p, &mut plan);
    plan.push((0, 0, 1)); // the decoder ends every table with this step; it can consume input
    let mut cum = [0i64; 257];
    cum[p] = last;
    for (mid, lo, hi) in plan {
        let (a, b) = (cum[lo], cum[hi]);
        if a == b {
            for k in lo + 1..hi {
                cum[k] = a;
            }
        } else {
            let span = (b - a + 1) as u64;
            let e = v % span;
            v = refill(v / span);
            cum[mid] = e as i64 + a;
        }
    }
    let mut counts = Vec::with_capacity(max_symbol + 2);
    let (mut acc, mut remaining) = (0i64, total as i64);
    for s in 0..max_symbol + 2 {
        let c = (cum[s + 1] & 0xffff) as u16 as i16 as i64;
        let d = c - acc;
        acc += d;
        let count = d - low;
        counts.push(count as i32);
        remaining -= count.abs();
    }
    if remaining != 0 || left != 0 {
        bail!("table counts do not add up");
    }
    Ok((counts, log, n + 1))
}

/// A little-endian bit writer whose last bit can be taken back (NCount's short codes)
struct Bits {
    out: Vec<u8>,
    acc: u64,
    n: u32,
    total: u64,
}

impl Bits {
    fn put(&mut self, value: u64, width: u32) {
        self.acc |= value << self.n;
        self.n += width;
        self.total += width as u64;
        while self.n >= 8 {
            self.out.push(self.acc as u8);
            self.acc >>= 8;
            self.n -= 8;
        }
    }
    /// Drops the last written bit (always a 0 bit by the NCount rule that calls this)
    fn unput1(&mut self) {
        if self.n == 0 {
            let b = self.out.pop().unwrap();
            self.acc = b as u64;
            self.n = 8;
        }
        self.n -= 1;
        self.total -= 1;
        self.acc &= (1u64 << self.n) - 1;
    }
    fn finish(mut self) -> Vec<u8> {
        if self.n > 0 {
            self.out.push(self.acc as u8);
        }
        self.out.truncate(((self.total + 7) / 8) as usize);
        self.out
    }
}

/// Encodes a normalised distribution in the NCount format of RFC 8878 section 4.1.1
fn write_ncount(counts: &[i32], log: u32) -> Result<Vec<u8>> {
    let mut b = Bits {
        out: Vec::new(),
        acc: 0,
        n: 0,
        total: 0,
    };
    b.put((log - 5) as u64, 4);
    let size = counts.len();
    let mut remaining: i64 = (1 << log) + 1;
    let mut threshold: i64 = 1 << log;
    let mut nb = log + 1;
    let mut s = 0usize;
    let mut prev0 = false;
    while s < size && remaining > 1 {
        if prev0 {
            let mut start = s;
            while s < size && counts[s] == 0 {
                s += 1;
            }
            if s == size {
                bail!("distribution ends in zeros");
            }
            while s >= start + 24 {
                start += 24;
                b.put(0xffff, 16);
            }
            while s >= start + 3 {
                start += 3;
                b.put(3, 2);
            }
            b.put((s - start) as u64, 2);
        }
        let mut count = counts[s] as i64;
        s += 1;
        let mx = (2 * threshold - 1) - remaining;
        remaining -= count.abs();
        count += 1;
        if count >= threshold {
            count += mx;
        }
        b.put(count as u64, nb);
        if count < mx {
            b.unput1();
        }
        prev0 = count == 1;
        if remaining < 1 {
            bail!("distribution over its total");
        }
        while remaining < threshold {
            nb -= 1;
            threshold >>= 1;
        }
    }
    if remaining != 1 {
        bail!("distribution under its total");
    }
    Ok(b.finish())
}

fn table(buf: &[u8], pos: usize, end: usize, max_sv: usize) -> Result<(Vec<u8>, usize)> {
    let (counts, log, used) = read_ncount_bic(buf, pos, end, max_sv)?;
    Ok((write_ncount(&counts, log)?, used))
}

fn le(b: &[u8]) -> u64 {
    b.iter().rev().fold(0u64, |v, &x| (v << 8) | x as u64)
}

/// Rewrites the literals section at the start of a compressed block: (new bytes, bytes read)
fn literals(blk: &[u8]) -> Result<(Vec<u8>, usize)> {
    let b0 = blk[0];
    let (typ, mut fmt) = (b0 & 3, (b0 >> 2) & 3);
    if typ <= 1 {
        let (size, hl) = match fmt {
            0 | 2 => ((b0 >> 3) as usize, 1),
            1 => (((b0 >> 4) as usize) | ((blk[1] as usize) << 4), 2),
            _ => (
                ((b0 >> 4) as usize) | ((blk[1] as usize) << 4) | ((blk[2] as usize) << 12),
                3,
            ),
        };
        let n = hl + if typ == 0 { size } else { 1 };
        return Ok((blk[..n].to_vec(), n));
    }
    let hw = |f: u8| match f {
        0 | 1 => (3usize, 10u32),
        2 => (4, 14),
        _ => (5, 18),
    };
    let (mut hl, mut width) = hw(fmt);
    let h = le(&blk[..hl]) >> 4;
    let regen = h & ((1 << width) - 1);
    let comp = ((h >> width) & ((1 << width) - 1)) as usize;
    if hl + comp > blk.len() {
        bail!("literals past the end of the block");
    }
    let body = &blk[hl..hl + comp];
    let read = hl + comp;
    if typ == 3 {
        return Ok((blk[..read].to_vec(), read)); // treeless: no table to rewrite
    }
    let hb = body[0] as usize;
    let new_body = if hb >= 128 {
        body.to_vec() // weights stored directly
    } else {
        let (t, used) = table(body, 1, 1 + hb, MAX_SV_HUF)?;
        let mut weights = t;
        weights.extend_from_slice(&body[1 + used..1 + hb]);
        if weights.len() >= 128 {
            bail!("Huffman weight table too large for a standard header");
        }
        let mut nb = vec![weights.len() as u8];
        nb.extend_from_slice(&weights);
        nb.extend_from_slice(&body[1 + hb..]);
        nb
    };
    let comp = new_body.len() as u64;
    if fmt == 0 {
        if comp >= 1 << 10 {
            bail!("single-stream literals grew past their size field");
        }
    } else {
        while comp >= 1 << width || regen >= 1 << width {
            fmt += 1;
            if fmt > 3 {
                bail!("literals too large");
            }
            (hl, width) = hw(fmt);
        }
    }
    let head = typ as u64 | (fmt as u64) << 2 | regen << 4 | comp << (4 + width);
    let mut out = head.to_le_bytes()[..hl].to_vec();
    out.extend_from_slice(&new_body);
    Ok((out, read))
}

fn sequences(blk: &[u8], pos: usize, out: &mut Vec<u8>) -> Result<()> {
    let b0 = blk[pos];
    if b0 == 0 {
        out.extend_from_slice(&blk[pos..]);
        return Ok(());
    }
    let hl = if b0 < 128 {
        1
    } else if b0 < 255 {
        2
    } else {
        3
    };
    out.extend_from_slice(&blk[pos..pos + hl + 1]);
    let modes = blk[pos + hl];
    let mut p = pos + hl + 1;
    for (shift, max_sv) in [(6, MAX_SV_LL), (4, MAX_SV_OF), (2, MAX_SV_ML)] {
        match (modes >> shift) & 3 {
            1 => {
                out.push(blk[p]);
                p += 1;
            }
            2 => {
                let (t, used) = table(blk, p, blk.len(), max_sv)?;
                out.extend_from_slice(&t);
                p += used;
            }
            _ => {}
        }
    }
    out.extend_from_slice(&blk[p..]);
    Ok(())
}

/// Rewrites every ZBIC frame of `data` as a standard zstd frame; other frames are copied
pub fn to_zstd(data: &[u8]) -> Result<Vec<u8>> {
    let mut out = Vec::with_capacity(data.len() + 1024);
    let mut pos = 0;
    let u32_at = |p: usize| u32::from_le_bytes(data[p..p + 4].try_into().unwrap());
    while pos < data.len() {
        let magic = u32_at(pos);
        if magic & 0xFFFF_FFF0 == 0x184D_2A50 {
            let size = u32_at(pos + 4) as usize;
            out.extend_from_slice(&data[pos..pos + 8 + size]);
            pos += 8 + size;
            continue;
        }
        if magic != ZBIC_MAGIC && magic != ZSTD_MAGIC {
            bail!("unknown frame magic {magic:08X} at {pos:#x}");
        }
        let zbic = magic == ZBIC_MAGIC;
        let fhd = data[pos + 4];
        let single = (fhd >> 5) & 1 != 0;
        let hsize = 5
            + usize::from(!single)
            + [0, 1, 2, 4][(fhd & 3) as usize]
            + [usize::from(single), 2, 4, 8][(fhd >> 6) as usize];
        out.extend_from_slice(&ZSTD_MAGIC.to_le_bytes());
        out.extend_from_slice(&data[pos + 4..pos + hsize]);
        pos += hsize;
        loop {
            let bh = le(&data[pos..pos + 3]);
            let (last, btype, bsize) = (bh & 1, (bh >> 1) & 3, (bh >> 3) as usize);
            pos += 3;
            let payload: Vec<u8> = match btype {
                0 => {
                    pos += bsize;
                    data[pos - bsize..pos].to_vec()
                }
                1 => {
                    pos += 1;
                    data[pos - 1..pos].to_vec()
                }
                2 => {
                    let blk = &data[pos..pos + bsize];
                    pos += bsize;
                    if zbic {
                        let (mut p, n) = literals(blk)?;
                        sequences(blk, n, &mut p)?;
                        if p.len() > BLOCK_MAX {
                            bail!("a block grew past the zstd block limit");
                        }
                        p
                    } else {
                        blk.to_vec()
                    }
                }
                _ => bail!("reserved block type"),
            };
            // a type-1 (RLE) block keeps its regenerated size; the others take their payload's
            let size = if btype == 1 {
                bsize as u64
            } else {
                payload.len() as u64
            };
            out.extend_from_slice(&(last | btype << 1 | size << 3).to_le_bytes()[..3]);
            out.extend_from_slice(&payload);
            if last != 0 {
                break;
            }
        }
        if (fhd >> 2) & 1 != 0 {
            out.extend_from_slice(&data[pos..pos + 4]);
            pos += 4;
        }
    }
    Ok(out)
}

/// Decompresses ZBIC (or plain zstd) `data` to exactly `size` bytes
pub fn decompress(data: &[u8], size: usize) -> Result<Vec<u8>> {
    let std = to_zstd(data)?;
    let mut out = Vec::with_capacity(size);
    let mut src = &std[..];
    while !src.is_empty() {
        let mut d =
            ruzstd::decoding::StreamingDecoder::new(&mut src).map_err(|e| anyhow!("zstd: {e}"))?;
        d.read_to_end(&mut out).map_err(|e| anyhow!("zstd: {e}"))?;
    }
    if out.len() != size {
        bail!("decompressed {} bytes, expected {size}", out.len());
    }
    Ok(out)
}
