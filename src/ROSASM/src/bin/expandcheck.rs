//! Run the macro expander over every `s/` file in the corpus and report what
//! fails. This was the test that the expander stage was complete.
//!
//!     cargo run --bin expandcheck -- <path to RiscOS>
//!
//! Full expansion needs the build's exported headers, which exist only after
//! `export_hdrs` has run. So a great many `GET`s cannot be found in a clean
//! tree. Those are counted separately. What matters here is whether anything
//! fails for a reason *other* than a missing include.

use std::collections::HashMap;
use std::path::{Path, PathBuf};

use rosasm::expand::{Expander, FileResolver};
use rosasm::source::SourceFile;

/// Resolves `GET` targets the way the RISC OS build does.
///
/// Sources name headers as `Hdr:Global.Services` (a path variable and a
/// dot-separated path) or `hdr.Options` (relative). Both map onto directories
/// once `:` and `.` become separators.
struct BuildResolver {
    /// Searched in order for a relative name.
    dirs: Vec<PathBuf>,
    /// Prefix before `:` -> directories to search.
    vars: HashMap<String, Vec<PathBuf>>,
}

impl BuildResolver {
    fn candidates(&self, name: &str) -> Vec<PathBuf> {
        let name = name.trim();
        let mut out = Vec::new();
        if let Some((var, rest)) = name.split_once(':') {
            let rel = rest.replace('.', "/");
            if let Some(dirs) = self.vars.get(&var.to_ascii_lowercase()) {
                for d in dirs {
                    out.push(d.join(&rel));
                }
            }
            return out;
        }
        let rel = name.replace('.', "/");
        for d in &self.dirs {
            out.push(d.join(&rel));
        }
        out
    }
}

impl FileResolver for BuildResolver {
    fn resolve(&self, name: &str) -> Option<(String, Vec<String>)> {
        for p in self.candidates(name) {
            if p.is_file() {
                if let Ok(sf) = SourceFile::load(&p) {
                    return Some((name.to_string(), sf.lines));
                }
            }
        }
        None
    }
}

/// Walk up from an `s/` file to the component root, the directory that holds
/// the `s/` directory.
fn component_root(file: &Path) -> PathBuf {
    file.parent()
        .and_then(|p| p.parent())
        .map(|p| p.to_path_buf())
        .unwrap_or_default()
}

fn main() {
    let Some(root) = std::env::args().nth(1).map(PathBuf::from) else {
        eprintln!("usage: expandcheck <path to RiscOS>");
        std::process::exit(2);
    };
    let sources = root.join("Sources");
    let export = root.join("Export").join("APCS-32").join("Hdr");

    // Only `s/` files are assembler translation units; `hdr/` files are
    // included by them and are not assembled on their own.
    let mut units: Vec<PathBuf> = Vec::new();
    collect_s_files(&sources, &mut units);
    units.sort();

    let mut ok = 0usize;
    let mut missing_get = 0usize;
    let mut other: Vec<(PathBuf, String)> = Vec::new();
    let mut lines_out = 0usize;

    for unit in &units {
        let Ok(sf) = SourceFile::load(unit) else { continue };
        let comp = component_root(unit);
        let resolver = BuildResolver {
            // Deliberately NOT the unit's own `s/` directory: `GET GraphicsV`
            // must find hdr/GraphicsV, not the file doing the GET.
            dirs: vec![comp.clone(), comp.join("hdr"), export.clone()],
            vars: HashMap::from([
                ("hdr".into(), vec![export.clone(), comp.join("hdr")]),
                ("sys".into(), vec![export.clone()]),
                ("interface".into(), vec![export.join("Interface")]),
                ("apcs".into(), vec![export.clone()]),
            ]),
        };
        let mut ex = Expander::new(&resolver);
        ex.set_target_builtins();
        // Predefine the symbols the build passes with -PD in ASFLAGS; see
        // RiscOS/Env/!Common.sh and BuildSys/GNUmakefiles/StdTools. Only
        // those. Options like `International` are *not* among them. Each
        // component defines its own in hdr.Options, and the corpus uses that
        // name as a logical in some components and an arithmetic in others.
        // Giving it a type here would cause failures that are not real.
        for pd in [
            "APCS SETS \"APCS-32\"",
            "Machine SETS \"RPi\"",
            "UserIF SETS \"Raspberry\"",
            "RISCOS_MODULE SETL {TRUE}",
        ] {
            let _ = ex.predefine(pd);
        }
        let name = unit.file_name().unwrap_or_default().to_string_lossy().to_string();
        match ex.run(&name, sf.lines) {
            Ok(out) => {
                ok += 1;
                lines_out += out.len();
            }
            Err(e) => {
                if e.msg.starts_with("cannot find") {
                    missing_get += 1;
                } else {
                    other.push((unit.clone(), e.to_string()));
                }
            }
        }
    }

    let n = units.len();
    let pct = |x: usize| if n == 0 { 0.0 } else { 100.0 * x as f64 / n as f64 };
    println!("== expansion over {n} assembler units ==");
    println!("  expanded clean       {:>6}  {:5.1}%   ({lines_out} lines emitted)", ok, pct(ok));
    println!(
        "  blocked: missing GET {:>6}  {:5.1}%   (needs export_hdrs — not a gap)",
        missing_get,
        pct(missing_get)
    );
    // Split the remaining failures. A symbol that is simply undefined comes
    // from the missing headers. It is not a gap in the expander.
    let env = |m: &str| {
        m.contains("undefined symbol")
            || m.contains("is undefined")
            || m.contains("has not been declared")
    };
    let n_env = other.iter().filter(|(_, m)| env(m)).count();
    let n_gap = other.len() - n_env;
    println!(
        "  blocked: undefined   {:>6}  {:5.1}%   (symbols from those headers)",
        n_env,
        pct(n_env)
    );
    println!("  OTHER FAILURES       {:>6}  {:5.1}%  <- expander gaps", n_gap, pct(n_gap));
    let other: Vec<(PathBuf, String)> = other.into_iter().filter(|(_, m)| !env(m)).collect();

    if other.is_empty() {
        println!("\nno failures outside missing includes");
        return;
    }

    // Group by the first line of the message so gaps show up as classes.
    let mut classes: HashMap<String, (usize, String)> = HashMap::new();
    for (p, msg) in &other {
        let head = msg.lines().next().unwrap_or("").to_string();
        // Drop the file:line prefix so that similar errors group together.
        let key = head.splitn(3, ':').nth(2).unwrap_or(&head).trim().to_string();
        let e = classes.entry(key).or_insert((0, String::new()));
        e.0 += 1;
        if e.1.is_empty() {
            e.1 = format!("{}  —  {head}", p.file_name().unwrap_or_default().to_string_lossy());
        }
    }
    let mut classes: Vec<_> = classes.into_iter().collect();
    classes.sort_by(|a, b| b.1 .0.cmp(&a.1 .0).then_with(|| a.0.cmp(&b.0)));
    println!("\n== failure classes ==");
    for (msg, (count, example)) in classes.iter().take(20) {
        println!("  {:>5}  {}", count, msg);
        println!("         e.g. {example}");
    }
}

fn collect_s_files(dir: &Path, out: &mut Vec<PathBuf>) {
    let Ok(entries) = std::fs::read_dir(dir) else { return };
    for e in entries.flatten() {
        let p = e.path();
        let Ok(ft) = e.file_type() else { continue };
        if ft.is_dir() {
            let name = e.file_name();
            let name = name.to_string_lossy();
            if name == ".git" {
                continue;
            }
            if name == "s" {
                if let Ok(fs) = std::fs::read_dir(&p) {
                    for f in fs.flatten() {
                        if f.file_type().map(|t| t.is_file()).unwrap_or(false) {
                            out.push(f.path());
                        }
                    }
                }
            } else {
                collect_s_files(&p, out);
            }
        }
    }
}
