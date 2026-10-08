//! `fpacheck` — how much of the corpus's FPA code VFP can express.
//!
//!     fpacheck <path to RiscOS/Sources>
//!
//! Walks every source file, finds the FPA instructions, and runs each through
//! the same `fpa::convert` the assembler uses. Reports what translated, and
//! groups what did not by the reason it was refused.
//!
//! This reads the source text directly rather than expanding it, so a line
//! inside a false conditional is counted along with one that assembles. The
//! total may therefore be a little too high, but never too low. For a report
//! about what cannot be translated, that is the safe direction to err in.
//!
//! Two filters stop it counting far too much. First, only files under an
//! `s/` or `hdr/` directory are read. The tree also holds C, BASIC and
//! documentation, whose second word means nothing here. Second, a candidate
//! must name a register `f0`-`f7`, because many ordinary symbols decode as
//! FPA mnemonics if allowed to. For example, `LOGGED` parses as `LOG`,
//! conditional on `GE`, in double precision, yet it is a variable. The
//! status transfers are the exception, because they take an ARM register.
//! Those are matched by name instead.

use rosasm::fpa;
use rosasm::legalize::Legalized;
use rosasm::lex;
use rosasm::source::SourceFile;
use std::collections::BTreeMap;
use std::path::Path;

#[derive(Default)]
struct Tally {
    /// Register names declared with `FN`; for example, `FACC FN 0` names
    /// `f0`. The assembler learns these as it expands. This tool gathers
    /// them in a first pass instead. Without them, every
    /// `ADFD FACC,FACC,F1` would be wrongly counted as untranslatable.
    fn_aliases: BTreeMap<String, u32>,
    translated: BTreeMap<String, usize>,
    refused: BTreeMap<String, usize>,
    /// Reason text to (count, an example mnemonic).
    reasons: BTreeMap<String, (usize, String)>,
    files: BTreeMap<String, usize>,
}

/// The reason without the mnemonic in front of it, so that refusals for the
/// same reason are grouped together.
fn reason_of(why: &str) -> String {
    match why.split_once(": ") {
        Some((_, rest)) => rest.trim().to_string(),
        None => why.to_string(),
    }
}

fn walk(dir: &Path, t: &mut Tally) {
    let Ok(entries) = std::fs::read_dir(dir) else { return };
    for e in entries.flatten() {
        let p = e.path();
        if p.is_dir() {
            if p.file_name().is_some_and(|n| n == ".git") {
                continue;
            }
            walk(&p, t);
        } else if is_assembler(&p) {
            if let Ok(sf) = SourceFile::load(&p) {
                scan(&p, &sf.lines, t);
            }
        }
    }
}

/// Gather every `FN` declaration in the tree, so an aliased register name is
/// recognised as the FPA register it stands for.
fn collect_fn(dir: &Path, t: &mut Tally) {
    let Ok(entries) = std::fs::read_dir(dir) else { return };
    for e in entries.flatten() {
        let p = e.path();
        if p.is_dir() {
            if !p.file_name().is_some_and(|n| n == ".git") {
                collect_fn(&p, t);
            }
        } else if is_assembler(&p) {
            let Ok(sf) = SourceFile::load(&p) else { continue };
            for (n, raw) in sf.lines.iter().enumerate() {
                let l = lex::lex_line(n + 1, raw);
                if l.opcode_str().map(|o| o.eq_ignore_ascii_case("FN")) != Some(true) {
                    continue;
                }
                let (Some(name), Some(v)) = (l.label_str(), l.operands_str()) else { continue };
                if let Ok(v) = v.trim().parse::<u32>() {
                    if v <= 7 {
                        t.fn_aliases.insert(name.trim().to_string(), v);
                    }
                }
            }
        }
    }
}

/// Replace `FN`-declared names with the register they stand for.
fn resolve_aliases(operands: &str, aliases: &BTreeMap<String, u32>) -> String {
    let mut out = String::with_capacity(operands.len());
    let mut word = String::new();
    let flush = |w: &mut String, out: &mut String| {
        if !w.is_empty() {
            match aliases.get(w.as_str()) {
                Some(n) => out.push_str(&format!("f{n}")),
                None => out.push_str(w),
            }
            w.clear();
        }
    };
    for c in operands.chars() {
        if c.is_ascii_alphanumeric() || c == '_' || c == '$' {
            word.push(c);
        } else {
            flush(&mut word, &mut out);
            out.push(c);
        }
    }
    flush(&mut word, &mut out);
    out
}

/// Is this an assembler file? By the tree's own convention, assembler lives
/// in `s/`, and `hdr/` holds the macro definitions it pulls in.
fn is_assembler(p: &Path) -> bool {
    p.parent()
        .and_then(Path::file_name)
        .and_then(|n| n.to_str())
        .is_some_and(|n| n.eq_ignore_ascii_case("s") || n.eq_ignore_ascii_case("hdr"))
}

/// Does this operand list name an FPA register?
fn names_an_fpa_register(operands: &str) -> bool {
    let cs: Vec<char> = operands.chars().collect();
    for (i, c) in cs.iter().enumerate() {
        if *c != 'f' && *c != 'F' {
            continue;
        }
        // Whole word only: `fp` and `offset` are not `f0`.
        if i > 0 && (cs[i - 1].is_ascii_alphanumeric() || cs[i - 1] == '_') {
            continue;
        }
        match cs.get(i + 1) {
            Some(d) if d.is_ascii_digit() && *d <= '7'
                && !cs.get(i + 2).is_some_and(|n| n.is_ascii_alphanumeric() || *n == '_') => {
                    return true;
                }
            _ => {}
        }
    }
    false
}

fn scan(path: &Path, lines: &[String], t: &mut Tally) {
    let mut here = 0usize;
    for (n, raw) in lines.iter().enumerate() {
        let l = lex::lex_line(n + 1, raw);
        let Some(op) = l.opcode_str() else { continue };
        if !fpa::is_fpa(op) {
            continue;
        }
        let operands = &resolve_aliases(l.operands_str().unwrap_or(""), &t.fn_aliases);
        let operands: &str = operands;
        let up = op.to_ascii_uppercase();
        // Skip macro bodies. They are templates, not real instructions:
        // `ADFD $a, $b, $c` becomes one only when the macro is invoked, and
        // this tool does not expand macros.
        if operands.contains('$') {
            continue;
        }
        // The status transfers take an ARM register. Everything else must
        // name an FPA register. If it does not, it is an ordinary symbol
        // that happens to decode as an FPA mnemonic.
        let status = matches!(up.as_str(), "RFS" | "WFS" | "RFC" | "WFC")
            || up.len() == 5 && matches!(&up[..3], "RFS" | "WFS" | "RFC" | "WFC");
        if !status && !names_an_fpa_register(operands) {
            continue;
        }
        match fpa::convert(&up, operands) {
            Some(Legalized::Unsupported(why)) => {
                *t.refused.entry(up.clone()).or_default() += 1;
                let r = reason_of(&why);
                let e = t.reasons.entry(r).or_insert((0, up));
                e.0 += 1;
            }
            Some(_) => *t.translated.entry(up).or_default() += 1,
            None => continue,
        }
        here += 1;
    }
    if here > 0 {
        *t.files.entry(path.display().to_string()).or_default() += here;
    }
}

fn main() {
    let Some(root) = std::env::args().nth(1) else {
        eprintln!("usage: fpacheck <path to RiscOS/Sources>");
        std::process::exit(2);
    };
    let mut t = Tally::default();
    collect_fn(Path::new(&root), &mut t);
    walk(Path::new(&root), &mut t);

    let translated: usize = t.translated.values().sum();
    let refused: usize = t.refused.values().sum();
    let total = translated + refused;
    if total == 0 {
        println!("no FPA instructions found under {root}");
        return;
    }
    let pct = |n: usize| 100.0 * n as f64 / total as f64;
    println!("FPA instructions      {total}");
    println!("  translated to VFP   {translated} ({:.1}%)", pct(translated));
    println!("  refused             {refused} ({:.1}%)", pct(refused));

    println!("\ntranslated:");
    let mut v: Vec<_> = t.translated.iter().collect();
    v.sort_by(|a, b| b.1.cmp(a.1).then(a.0.cmp(b.0)));
    for (m, n) in v.iter().take(24) {
        println!("  {n:5}  {m}");
    }

    println!("\nrefused, by reason:");
    let mut r: Vec<_> = t.reasons.iter().collect();
    r.sort_by(|a, b| b.1 .0.cmp(&a.1 .0).then_with(|| a.0.cmp(b.0)));
    for (why, (n, example)) in r {
        println!("  {n:5}  {example}: {why}");
    }

    println!("\nfiles with FPA code:");
    let mut f: Vec<_> = t.files.iter().collect();
    f.sort_by(|a, b| b.1.cmp(a.1));
    for (p, n) in f.iter().take(20) {
        println!("  {n:5}  {p}");
    }
}
