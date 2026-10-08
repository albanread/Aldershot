//! Corpus harness.
//!
//! Runs the front end over the whole RISC OS assembler corpus and reports what
//! it understood. This is how the project measures progress. Each later stage
//! adds coverage here, and a regression shows as the numbers going down.
//!
//!     cargo run --bin corpus -- <path to RiscOS/Sources>
//!
//! It checks the lexer and the opcode vocabulary, which was measured from
//! the corpus. Opcodes it cannot classify are printed, most frequent first.
//! That list is the to-do list, taken from the sources rather than guessed.

use std::collections::HashMap;
use std::path::PathBuf;

use rosasm::lex::{self, Kind};
use rosasm::source::{discover, SourceFile};
use rosasm::vocab;

fn main() {
    let root = match std::env::args().nth(1) {
        Some(p) => PathBuf::from(p),
        None => {
            eprintln!("usage: corpus <path to RiscOS/Sources>");
            std::process::exit(2);
        }
    };

    let files = discover(&root);
    if files.is_empty() {
        eprintln!("corpus: no assembler files found under {}", root.display());
        std::process::exit(1);
    }

    let mut n_lines = 0usize;
    let mut n_blank = 0usize;
    let mut n_comment = 0usize;
    let mut n_stmt = 0usize;
    let mut n_label_only = 0usize;
    let mut unreadable = Vec::new();

    // opcode token -> occurrences
    let mut opcodes: HashMap<String, usize> = HashMap::new();
    // Names defined by `MACRO` in the corpus. These occupy the opcode field but
    // are neither directives nor instructions, and there are a lot of them.
    let mut macro_names: std::collections::HashSet<String> = std::collections::HashSet::new();

    for path in &files {
        let sf = match SourceFile::load(path) {
            Ok(s) => s,
            Err(e) => {
                unreadable.push((path.clone(), e.to_string()));
                continue;
            }
        };
        // The line after MACRO is the prototype; its opcode field is the name.
        let mut expect_prototype = false;
        for line in lex::lex(&sf.lines) {
            n_lines += 1;
            match line.kind {
                Kind::Blank => n_blank += 1,
                Kind::Comment => n_comment += 1,
                Kind::Statement => {
                    n_stmt += 1;
                    match line.opcode_str() {
                        Some(op) => {
                            if expect_prototype {
                                macro_names.insert(op.to_string());
                                expect_prototype = false;
                            } else if op.eq_ignore_ascii_case("MACRO") {
                                expect_prototype = true;
                            }
                            *opcodes.entry(op.to_string()).or_insert(0) += 1;
                        }
                        None => n_label_only += 1,
                    }
                }
            }
        }
    }

    // Classify every opcode token we saw.
    let mut known_directive = 0usize;
    let mut known_symbolic = 0usize;
    let mut known_macro = 0usize;
    let mut unclassified: Vec<(String, usize)> = Vec::new();
    let mut macro_uses: Vec<(String, usize)> = Vec::new();
    for (op, n) in &opcodes {
        if vocab::is_directive(op) {
            known_directive += n;
        } else if vocab::is_symbolic(op) {
            known_symbolic += n;
        } else if macro_names.contains(op) {
            known_macro += n;
            macro_uses.push((op.clone(), *n));
        } else {
            unclassified.push((op.clone(), *n));
        }
    }
    macro_uses.sort_by(|a, b| b.1.cmp(&a.1).then(a.0.cmp(&b.0)));
    unclassified.sort_by(|a, b| b.1.cmp(&a.1).then(a.0.cmp(&b.0)));
    let n_unclassified: usize = unclassified.iter().map(|(_, n)| n).sum();

    println!("== corpus ==");
    println!("  files                {:>9}", files.len());
    println!("  lines                {:>9}", n_lines);
    println!("    blank              {:>9}", n_blank);
    println!("    comment            {:>9}", n_comment);
    println!("    statement          {:>9}", n_stmt);
    println!("      label only       {:>9}", n_label_only);
    if !unreadable.is_empty() {
        println!("  UNREADABLE           {:>9}", unreadable.len());
        for (p, e) in unreadable.iter().take(5) {
            println!("    {} — {e}", p.display());
        }
    }

    let with_opcode = n_stmt - n_label_only;
    println!();
    println!("== opcode field ==");
    println!("  distinct tokens      {:>9}", opcodes.len());
    println!("  occurrences          {:>9}", with_opcode);
    let pct = |x: usize| {
        if with_opcode == 0 { 0.0 } else { 100.0 * x as f64 / with_opcode as f64 }
    };
    println!("    directives         {:>9}  {:5.1}%", known_directive, pct(known_directive));
    println!("    symbolic forms     {:>9}  {:5.1}%", known_symbolic, pct(known_symbolic));
    println!(
        "    corpus macros      {:>9}  {:5.1}%   ({} distinct, defined by MACRO)",
        known_macro,
        pct(known_macro),
        macro_names.len()
    );
    println!(
        "    unclassified       {:>9}  {:5.1}%   (expected: ARM instructions)",
        n_unclassified,
        pct(n_unclassified)
    );

    println!();
    println!("== top 15 corpus-defined macros ==");
    println!("   (these must be expanded before any encoder sees them)");
    for (op, n) in macro_uses.iter().take(15) {
        println!("  {:>8}  {}", n, op);
    }

    println!();
    println!("== top 40 unclassified opcode tokens ==");
    println!("   (these should all be ARM/NEON mnemonics; a directive here is a bug)");
    for (op, n) in unclassified.iter().take(40) {
        println!("  {:>8}  {}", n, op);
    }

    // A directive hiding in the unclassified list is the failure we care about.
    let suspicious: Vec<&(String, usize)> = unclassified
        .iter()
        .filter(|(op, _)| {
            op.len() > 1
                && op.chars().all(|c| c.is_ascii_uppercase())
                && matches!(
                    op.as_str(),
                    "COMMON" | "REQUIRE" | "RELOC" | "ATTR" | "FRAME" | "STRONG"
                        | "WEAK" | "EXPORTAS" | "THUMB" | "CODE16" | "FILL" | "GLOBAL"
                        | "INFO" | "NOFP" | "ORG"
                )
        })
        .collect();
    if suspicious.is_empty() {
        println!();
        println!("no unimplemented directives found outside the measured vocabulary");
    } else {
        println!();
        println!("!! directives present that the vocabulary does not list:");
        for (op, n) in suspicious {
            println!("  {:>8}  {}", n, op);
        }
    }
}
