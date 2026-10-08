//! Compiling a program file to C. The stages are: load (textload.rs),
//! analyse (flow.rs), lower (lower.rs) and emit (cgen.rs).

use crate::ast::Stmt;
use crate::flow::Flow;
use crate::parse::LineParser;
use crate::program::Program;
use crate::tokens::TREM;
use crate::{cgen, lower, textload};
use std::path::{Path, PathBuf};

/// The names of the gated quirks (spec Appendix C), in the bit order that
/// rb.h uses.
pub const QUIRKS: [&str; 16] = [
    "int-add-wrap",
    "int-array-mul-wrap",
    "array-late-fp-error",
    "inf-leak",
    "vfp-pow",
    "vfp-tan",
    "instr-start-wrap",
    "substr-assign-range",
    "cond-truncate",
    "for-byte-word",
    "error-keeps-locals",
    "stale-local-handler",
    "restore-data-pop",
    "dollar-no-cr",
    "five-byte-zero",
    "sys-string-255",
];

/// The bit for a quirk's name, or the bits for `ALL` (§1.3.10).
pub fn quirk_bits(name: &str) -> Option<u32> {
    if name.eq_ignore_ascii_case("all") {
        return Some((1 << QUIRKS.len()) - 1);
    }
    QUIRKS.iter().position(|q| q.eq_ignore_ascii_case(name)).map(|k| 1 << k)
}

/// The quirks that `REM {QUIRKS name, ...}` selects anywhere in the
/// program (§1.3.10).
pub fn quirks_in(p: &Program) -> Result<u32, String> {
    let mut bits = 0;
    for l in &p.lines {
        let t = &l.text;
        let mut i = 0;
        while i < t.len() {
            if t[i] == b'"' {
                i += 1;
                while i < t.len() && t[i] != b'"' {
                    i += 1;
                }
            } else if t[i] == TREM {
                let rest = String::from_utf8_lossy(&t[i + 1..]).to_string();
                let r = rest.trim_start();
                if let Some(list) = r.strip_prefix("{QUIRKS") {
                    let list = list.split('}').next().unwrap_or("");
                    for name in list.split(',') {
                        let name = name.trim();
                        if name.is_empty() {
                            continue;
                        }
                        bits |= quirk_bits(name).ok_or_else(|| format!("line {}: unknown quirk {name}", l.number))?;
                    }
                }
                break;
            }
            i += 1;
        }
    }
    Ok(bits)
}

/// Compiles a program's bytes to C, or returns every diagnostic.
pub fn compile(bytes: &[u8], source: &str, quirks: u32) -> Result<String, Vec<String>> {
    compile_with_warnings(bytes, source, quirks).map(|(c, _)| c)
}

/// Compiles a program's bytes to C and returns the warnings (§9.8.3) too,
/// or returns every diagnostic.
pub fn compile_with_warnings(bytes: &[u8], source: &str, quirks: u32) -> Result<(String, Vec<String>), Vec<String>> {
    compile_full(bytes, source, quirks, &[])
}

/// The names of the libraries that a program's `LIBRARY` statements read,
/// each with the number of its line (§15.3.1).
fn libraries_in(p: &Program) -> Vec<(Vec<u8>, u16)> {
    let mut out = Vec::new();
    for l in &p.lines {
        for s in LineParser::new(&l.text).parse_line() {
            if let Stmt::Library(name) = s.stmt {
                out.push((name, l.number));
            }
        }
    }
    out
}

/// The file that a library's name refers to (§15.3.1). An absolute name is
/// used as it stands. Otherwise the name is looked for in the program's
/// directory, and then in each search directory. If the bare name is not
/// there, the RISC OS filetype suffixes for BASIC and text are tried, and
/// then `.bas`.
fn find_library(name: &str, dirs: &[PathBuf]) -> Option<PathBuf> {
    let candidates = |p: PathBuf| {
        let s = p.to_string_lossy().to_string();
        [s.clone(), format!("{s},ffb"), format!("{s},fff"), format!("{s}.bas")].into_iter().map(PathBuf::from)
    };
    if Path::new(name).is_absolute() {
        return candidates(PathBuf::from(name)).find(|p| p.is_file());
    }
    dirs.iter().flat_map(|d| candidates(d.join(name))).find(|p| p.is_file())
}

/// The suffix added to an error in a library's code (§9.2.8). It is the
/// text on the library's first line after `REM` and an optional `>`,
/// starting at the first character that is not a space.
fn library_suffix(p: &Program) -> Option<Vec<u8>> {
    let t = &p.lines.first()?.text;
    let mut i = t.iter().position(|&b| b != b' ')?;
    if t[i] != TREM {
        return None;
    }
    i += 1;
    while i < t.len() && t[i] == b' ' {
        i += 1;
    }
    if i < t.len() && t[i] == b'>' {
        i += 1;
    }
    while i < t.len() && t[i] == b' ' {
        i += 1;
    }
    Some(t[i..].to_vec())
}

/// Compiles a program and the libraries it names to C, and returns the
/// warnings too, or returns every diagnostic. `search` holds the
/// directories given by `rosbas -L`.
pub fn compile_full(bytes: &[u8], source: &str, quirks: u32, search: &[PathBuf]) -> Result<(String, Vec<String>), Vec<String>> {
    let loaded = textload::load(bytes).map_err(|d| d.iter().map(|x| x.message.clone()).collect::<Vec<_>>())?;
    let main = loaded.program;
    let q = quirks | quirks_in(&main).map_err(|e| vec![e])?;
    // Load each library once, in the order of the LIBRARY statements. The
    // libraries that a library names come after it.
    let mut dirs = vec![Path::new(source).parent().map(Path::to_path_buf).unwrap_or_default()];
    dirs.extend(search.iter().cloned());
    let mut libs: Vec<(Vec<u8>, Program)> = Vec::new();
    let mut todo = libraries_in(&main);
    let mut errors = Vec::new();
    while !todo.is_empty() {
        let (name, line) = todo.remove(0);
        if libs.iter().any(|l| l.0 == name) {
            continue;
        }
        let shown = String::from_utf8_lossy(&name).to_string();
        let Some(path) = find_library(&shown, &dirs) else {
            errors.push(format!("line {line}: Cannot read library {shown}"));
            continue;
        };
        match std::fs::read(&path).map_err(|e| e.to_string()).and_then(|b| textload::load(&b).map_err(|d| d.iter().map(|x| x.message.clone()).collect::<Vec<_>>().join("; "))) {
            Ok(l) => {
                todo.extend(libraries_in(&l.program));
                libs.push((name, l.program));
            }
            Err(e) => errors.push(format!("line {line}: Cannot read library {shown}: {e}")),
        }
    }
    if !errors.is_empty() {
        return Err(errors);
    }
    let mut program = main.clone();
    let mut starts = vec![0];
    for (_, l) in &libs {
        starts.push(program.lines.len());
        program.lines.extend(l.lines.iter().cloned());
    }
    let flow = Flow::with_segments(&program, &starts);
    if !flow.errors.is_empty() {
        return Err(flow.errors.iter().map(|d| format!("line {}: {}", d.line, d.message)).collect());
    }
    let warnings = flow.warnings.iter().map(|d| format!("line {}: {}", d.line, d.message)).collect();
    let info: Vec<(Vec<u8>, Option<Vec<u8>>)> = libs.iter().map(|(n, l)| (n.clone(), library_suffix(l))).collect();
    let mut ir = lower::lower_with_libs(&flow, q, &info);
    // PAGE holds only the main program. The libraries are not part of it
    // (§12.3.3).
    ir.tokenised = main.to_tokenised();
    Ok((cgen::emit(&ir, source), warnings))
}
