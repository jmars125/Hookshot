//! The C half of a hybrid build: the functions the Rust lifter leaves out, taken from the C lifter's output
//! (nx2/lift.py's gen/code_*.c and code_hot_*.c).
//!
//! Each of those files is `#include "a64.h"`, a prelude of prototypes and extern declarations, then function
//! definitions, each opened by `void f_<addr>(Ctx *__restrict A64_NOESC c) {` and closed by a `}` alone on its line.
//! [`Gen::scan`] reads them all; [`Gen::patched`] names the functions the game patches edit (their bodies read or write
//! the host's `hos_*` settings, which the Rust lifter does not reproduce), so they stay C; [`Gen::write`] makes one C file
//! of the chosen functions with the prelude lines of the files they came from.

use anyhow::{Context, Result};
use std::collections::{BTreeMap, BTreeSet, HashSet};
use std::path::Path;

pub struct Gen {
    /// file index -> its prelude lines (everything outside a definition but the #include)
    preludes: Vec<Vec<String>>,
    /// guest address -> (file index, full definition text)
    funcs: BTreeMap<u64, (usize, String)>,
}

const OPEN: &str = "(Ctx *__restrict A64_NOESC c) {";

impl Gen {
    pub fn scan(gen: &Path) -> Result<Gen> {
        let mut files: Vec<_> = std::fs::read_dir(gen)
            .with_context(|| format!("reading {}", gen.display()))?
            .filter_map(|e| e.ok().map(|e| e.path()))
            .filter(|p| {
                let n = p.file_name().and_then(|n| n.to_str()).unwrap_or("");
                n.starts_with("code_") && n.ends_with(".c")
            })
            .collect();
        files.sort();
        anyhow::ensure!(!files.is_empty(), "no code_*.c in {}", gen.display());
        let (mut preludes, mut funcs) = (Vec::new(), BTreeMap::new());
        for (i, path) in files.iter().enumerate() {
            let text = std::fs::read_to_string(path).with_context(|| format!("reading {}", path.display()))?;
            let mut prelude = Vec::new();
            let mut cur: Option<(u64, String)> = None;
            for line in text.lines() {
                if let Some((_, body)) = cur.as_mut() {
                    body.push_str(line);
                    body.push('\n');
                    if line == "}" {
                        let (a, body) = cur.take().unwrap();
                        funcs.insert(a, (i, body));
                    }
                } else if let Some(name) = line.strip_prefix("void f_").and_then(|r| r.strip_suffix(OPEN)) {
                    let a = u64::from_str_radix(name, 16)
                        .with_context(|| format!("{}: bad function name in {line:?}", path.display()))?;
                    cur = Some((a, format!("{line}\n")));
                } else if !line.is_empty() && line != "#include \"a64.h\"" {
                    prelude.push(line.to_string());
                }
            }
            anyhow::ensure!(cur.is_none(), "{}: a definition is not closed", path.display());
            preludes.push(prelude);
        }
        Ok(Gen { preludes, funcs })
    }

    pub fn len(&self) -> usize {
        self.funcs.len()
    }

    /// Functions whose C reads or writes the host's hos_* state (the game patches of nx2/oot_patches.py and oot_wide.py)
    pub fn patched(&self) -> HashSet<u64> {
        self.funcs
            .iter()
            .filter(|(_, (_, body))| body.contains("hos_"))
            .map(|(&a, _)| a)
            .collect()
    }

    /// Writes `want`'s definitions to `parts` files fallback_<i>.c in `dir` (balanced by size, so a compiler each
    /// takes about as long: the 1,389 functions as one file took clang 7 minutes on one core), each after the
    /// deduplicated prelude lines of the files its functions are in. Returns the addresses of `want` that the C output
    /// has no definition of.
    pub fn write(&self, want: &BTreeSet<u64>, dir: &Path, parts: usize) -> Result<Vec<u64>> {
        let parts = parts.max(1);
        let mut by_size: Vec<u64> = want.iter().copied().filter(|a| self.funcs.contains_key(a)).collect();
        by_size.sort_by_key(|a| std::cmp::Reverse(self.funcs[a].1.len()));
        let mut bins: Vec<(usize, BTreeSet<u64>)> = vec![(0, BTreeSet::new()); parts];
        for a in by_size {
            let bin = bins.iter_mut().min_by_key(|b| b.0).unwrap();
            bin.0 += self.funcs[&a].1.len();
            bin.1.insert(a);
        }
        for (i, (_, set)) in bins.iter().enumerate() {
            self.write_one(set, &dir.join(format!("fallback_{i}.c")))?;
        }
        Ok(want.iter().copied().filter(|a| !self.funcs.contains_key(a)).collect())
    }

    fn write_one(&self, want: &BTreeSet<u64>, out: &Path) -> Result<Vec<u64>> {
        let mut missing = Vec::new();
        let mut used = BTreeSet::new();
        for a in want {
            match self.funcs.get(a) {
                Some((i, _)) => {
                    used.insert(*i);
                }
                None => missing.push(*a),
            }
        }
        let mut s = String::from("#include \"a64.h\"\n");
        let mut seen = HashSet::new();
        for i in used {
            for l in &self.preludes[i] {
                if seen.insert(l.as_str()) {
                    s.push_str(l);
                    s.push('\n');
                }
            }
        }
        for a in want {
            if let Some((_, body)) = self.funcs.get(a) {
                s.push_str(body);
            }
        }
        std::fs::write(out, s).with_context(|| format!("writing {}", out.display()))?;
        Ok(missing)
    }
}
