//! Assemble a source and print a listing in ObjAsm's format.
//!
//!     roslist <source> [-I dir]... [-PD "Sym SETA 1"]...
//!
//! The listing is what the differential harness compares against ObjAsm's own.

use std::collections::HashMap;
use std::path::PathBuf;

use rosasm::expand::{Expander, FileResolver};
use rosasm::listing;
use rosasm::source::SourceFile;

/// The paths a RISC OS name might mean on a host filesystem.
///
/// RISC OS writes `dir.file`, so `hdr.Options` is `hdr/Options`. But the
/// sources also write `Options.hdr`, which the DDE resolves by moving the
/// last element to the front. That convention lets one tree be read from both
/// a RISC OS host and a Unix-style host. ObjAsm accepts both spellings, and
/// 109 corpus units depend on `GET h_regs.s` finding `s/h_regs`.
fn relative_forms(name: &str) -> Vec<String> {
    let name = name.trim();
    let direct = name.replace('.', "/");
    let mut out = vec![direct];
    if let Some((head, tail)) = name.rsplit_once('.') {
        let swapped = format!("{}/{}", tail, head.replace('.', "/"));
        if !out.contains(&swapped) {
            out.push(swapped);
        }
    }
    out
}

struct Dirs {
    dirs: Vec<PathBuf>,
    vars: HashMap<String, Vec<PathBuf>>,
}

impl FileResolver for Dirs {
    fn resolve(&self, name: &str) -> Option<(String, Vec<String>)> {
        let name = name.trim();
        let mut cands: Vec<PathBuf> = Vec::new();
        if let Some((var, rest)) = name.split_once(':') {
            if let Some(ds) = self.vars.get(&var.to_ascii_lowercase()) {
                for rel in relative_forms(rest) {
                    cands.extend(ds.iter().map(|d| d.join(&rel)));
                }
            }
        } else {
            for rel in relative_forms(name) {
                cands.extend(self.dirs.iter().map(|d| d.join(&rel)));
            }
        }
        for p in cands {
            if p.is_file() {
                if let Ok(sf) = SourceFile::load(&p) {
                    return Some((name.to_string(), sf.lines));
                }
            }
        }
        None
    }
}

fn main() {
    let args: Vec<String> = std::env::args().skip(1).collect();
    let mut source: Option<PathBuf> = None;
    let mut dirs: Vec<PathBuf> = Vec::new();
    let mut pds: Vec<String> = Vec::new();
    let mut i = 0;
    while i < args.len() {
        match args[i].as_str() {
            "-I" | "-i" => {
                i += 1;
                if let Some(d) = args.get(i) {
                    dirs.push(PathBuf::from(d));
                }
            }
            "-PD" | "-pd" => {
                i += 1;
                if let Some(d) = args.get(i) {
                    pds.push(d.clone());
                }
            }
            s => source = Some(PathBuf::from(s)),
        }
        i += 1;
    }
    let Some(source) = source else {
        eprintln!("usage: roslist <source> [-I dir]... [-PD assignment]...");
        std::process::exit(2);
    };

    let sf = match SourceFile::load(&source) {
        Ok(s) => s,
        Err(e) => {
            eprintln!("roslist: {}: {e}", source.display());
            std::process::exit(1);
        }
    };
    // The component root (the parent of the source's own directory) is always
    // searched, after any -I directories. The source's own directory is not.
    let own = source.parent().map(|p| p.to_path_buf()).unwrap_or_default();
    let comp = own.parent().map(|p| p.to_path_buf()).unwrap_or_default();
    let mut all = dirs.clone();
    all.push(comp.clone());
    // Search `Hdr:` in the same order as the build's Hdr$Path. The
    // component's own hdr comes first, then the -I directories and their
    // Global, Interface and Interface2 subdirectories. A component's private
    // header must win over one of the same name elsewhere.
    let vars = HashMap::from([("hdr".to_string(), {
        let mut v = vec![comp.join("hdr")];
        v.extend(dirs.iter().cloned());
        v.extend(dirs.iter().map(|d| d.join("Global")));
        v.extend(dirs.iter().map(|d| d.join("Interface")));
        v.extend(dirs.iter().map(|d| d.join("Interface2")));
        v
    })]);

    let r = Dirs { dirs: all, vars };
    let mut e = Expander::new(&r);
    e.set_target_builtins();
    // A listing is made to be compared, so a failed assertion is reported
    // as a warning and the listing is still produced.
    e.set_assert_warnings(true);
    for pd in &pds {
        if let Err(err) = e.predefine(pd) {
            eprintln!("roslist: bad -PD {pd:?}: {err}");
            std::process::exit(1);
        }
    }
    let name = source
        .file_name()
        .unwrap_or_default()
        .to_string_lossy()
        .to_string();
    match e.run(&name, sf.lines) {
        Ok(out) => print!("{}", listing::render(&out)),
        Err(err) => {
            eprintln!("{err}");
            std::process::exit(1);
        }
    }
}
