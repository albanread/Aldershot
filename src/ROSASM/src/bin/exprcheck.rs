//! Run the expression evaluator over every expression in the corpus and
//! report the ones it could not *parse*. This was the test that the
//! evaluator stage was complete.
//!
//!     cargo run --bin exprcheck -- <path to RiscOS/Sources>
//!
//! Most expressions cannot be *evaluated* here. They refer to symbols that
//! exist only after `GET` files are read in and macros are expanded, and
//! this tool does neither. That is expected and is not a failure. What
//! matters here is syntax. An expression that cannot be parsed is a gap in
//! the evaluator, and those are listed.

use std::collections::HashMap;
use std::path::PathBuf;

use rosasm::expr;
use rosasm::lex::{self, Kind};
use rosasm::source::{discover, SourceFile};
use rosasm::symtab::SymTab;

/// Directives whose operand field is (or begins with) an expression.
fn expression_operand(opcode: &str, operands: &str) -> Option<String> {
    let up = opcode.to_ascii_uppercase();
    match up.as_str() {
        // `SETA x` / `SETL x` / `SETS x` — the whole operand is the expression.
        "SETA" | "SETL" | "SETS" => Some(operands.to_string()),
        // `*` and EQU define an absolute from an expression.
        "*" | "EQU" => Some(operands.to_string()),
        // Conditional and loop guards.
        "[" | "IF" | "WHILE" => Some(operands.to_string()),
        // ASSERT takes a logical expression.
        "ASSERT" => Some(operands.to_string()),
        _ => None,
    }
}

/// Errors that mean "not enough context yet", rather than "cannot parse".
fn is_expected(msg: &str) -> bool {
    msg.contains("undefined symbol")
        || msg.contains("not available yet")
        || msg.contains("not implemented")
        || msg.contains("has not been declared")
        || msg.contains("is undefined")
}

fn main() {
    let Some(root) = std::env::args().nth(1).map(PathBuf::from) else {
        eprintln!("usage: exprcheck <path to RiscOS/Sources>");
        std::process::exit(2);
    };

    let files = discover(&root);
    let syms = SymTab::new();

    let mut total = 0usize;
    let mut evaluated = 0usize;
    let mut expected = 0usize;
    let mut unparsed: Vec<(String, String, PathBuf, usize)> = Vec::new();
    let mut by_directive: HashMap<String, usize> = HashMap::new();

    for path in &files {
        let Ok(sf) = SourceFile::load(path) else { continue };
        for line in lex::lex(&sf.lines) {
            if line.kind != Kind::Statement {
                continue;
            }
            let (Some(op), Some(args)) = (line.opcode_str(), line.operands_str()) else {
                continue;
            };
            let Some(src) = expression_operand(op, args) else { continue };
            if src.trim().is_empty() {
                continue;
            }
            total += 1;
            *by_directive.entry(op.to_ascii_uppercase()).or_insert(0) += 1;

            match expr::eval(&src, &syms) {
                Ok(_) => evaluated += 1,
                Err(e) if is_expected(&e.msg) => expected += 1,
                Err(e) => unparsed.push((src.clone(), e.msg, path.clone(), line.num)),
            }
        }
    }

    println!("== expressions in the corpus ==");
    let mut dirs: Vec<_> = by_directive.into_iter().collect();
    dirs.sort_by(|a, b| b.1.cmp(&a.1).then_with(|| a.0.cmp(&b.0)));
    for (d, n) in &dirs {
        println!("  {:>8}  {}", n, d);
    }

    let pct = |x: usize| if total == 0 { 0.0 } else { 100.0 * x as f64 / total as f64 };
    println!();
    println!("  total                {:>8}", total);
    println!("  evaluated now        {:>8}  {:5.1}%", evaluated, pct(evaluated));
    println!(
        "  need later context   {:>8}  {:5.1}%  (symbols from GET/macros — sprint 2)",
        expected,
        pct(expected)
    );
    println!(
        "  UNPARSED             {:>8}  {:5.1}%  <- evaluator gaps",
        unparsed.len(),
        pct(unparsed.len())
    );

    if unparsed.is_empty() {
        println!();
        println!("every expression in the corpus parses");
        return;
    }

    // Group the failures by message so the gaps show up as classes, not
    // thousands of individual lines.
    let mut classes: HashMap<String, (usize, String, PathBuf, usize)> = HashMap::new();
    for (src, msg, path, line) in &unparsed {
        // Key on the message alone. It does not include the column, so
        // similar errors group together.
        let key = msg.clone();
        let e = classes
            .entry(key)
            .or_insert((0, src.clone(), path.clone(), *line));
        e.0 += 1;
    }
    let mut classes: Vec<_> = classes.into_iter().collect();
    classes.sort_by(|a, b| b.1 .0.cmp(&a.1 .0).then_with(|| a.0.cmp(&b.0)));

    println!();
    println!("== failure classes ==");
    for (msg, (n, example, path, line)) in classes.iter().take(20) {
        let name = path.file_name().unwrap_or_default().to_string_lossy();
        println!("  {:>6}  {}", n, msg);
        println!("          e.g. {name}:{line}   {}", example.trim());
    }
}
