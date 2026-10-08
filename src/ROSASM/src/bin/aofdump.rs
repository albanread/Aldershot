//! `aofdump` — show what is inside an AOF object, or compare two.
//!
//!     aofdump <object> [-x]
//!     aofdump <object> --hash
//!     aofdump <ours> --against <theirs> [-n <count>]
//!
//! The output is roughly what the DDE's `decaof` prints, in the same order,
//! so the two can be read side by side. `-x` does not disassemble. It dumps
//! each area's words with their offsets, which is enough to compare
//! encodings by eye.
//!
//! `--against` gives the final answer. Everything else this project
//! measures is only an indirect sign. A unit that assembles may still have
//! assembled wrongly. Comparing our object with the one ObjAsm makes from
//! the same source is the only test that answers the question directly.
//!
//! `--hash` prints the content that `--against` compares as a single number.
//! A corpus can then be checked against a reference by comparing one number
//! per unit rather than diffing each unit, and the full diff run only where
//! the numbers disagree. The hash covers the areas, their attributes, their
//! bytes, the relocations and the symbols. It deliberately leaves out the
//! identification string, which names the tool that produced the file and
//! so differs between any two assemblers.

use rosasm::aof::{self, area_attr, sym_attr, FieldType, RelocBy};

/// Name the attribute bits of an area. Code and data areas reuse two bit
/// positions, so which names apply depends on the CODE bit.
fn describe_area(attr: u32) -> String {
    let mut v: Vec<&str> = Vec::new();
    let code = attr & area_attr::CODE != 0;
    for (bit, name) in [
        (area_attr::ABSOLUTE, "ABS"),
        (area_attr::CODE, "CODE"),
        (area_attr::COMMON_DEF, "COMDEF"),
        (area_attr::COMMON_REF, "COMMON"),
        (area_attr::ZERO_INIT, "NOINIT"),
        (area_attr::READ_ONLY, "READONLY"),
        (area_attr::POSITION_INDEPENDENT, "PIC"),
        (area_attr::DEBUG_TABLES, "DEBUG"),
    ] {
        if attr & bit != 0 {
            v.push(name);
        }
    }
    if code {
        for (bit, name) in [
            (area_attr::APCS_32, "APCS-32"),
            (area_attr::REENTRANT, "REENTRANT"),
            (area_attr::EXTENDED_FP, "EXTFP"),
            (area_attr::NO_STACK_CHECK, "NOSTACKCHECK"),
            (area_attr::THUMB, "THUMB"),
            (area_attr::HALFWORD_INSTRS, "HALFWORD"),
            (area_attr::INTERWORKING, "INTERWORK"),
        ] {
            if attr & bit != 0 {
                v.push(name);
            }
        }
    } else {
        for (bit, name) in [
            (area_attr::BASED, "BASED"),
            (area_attr::SHARED_LIB_STUB, "SLSTUB"),
        ] {
            if attr & bit != 0 {
                v.push(name);
            }
        }
    }
    if v.is_empty() {
        v.push("READWRITE");
    }
    v.join(", ")
}

fn describe_symbol(attr: u32) -> String {
    let mut v: Vec<&str> = Vec::new();
    // A symbol that is defined and global is a definition. One that is
    // global but not defined is a reference, which the linker must resolve
    // from another file.
    v.push(if attr & sym_attr::DEFINED != 0 {
        "defined"
    } else {
        "reference"
    });
    if attr & sym_attr::GLOBAL != 0 {
        v.push("global");
    } else {
        v.push("local");
    }
    for (bit, name) in [
        (sym_attr::ABSOLUTE, "absolute"),
        (sym_attr::CASE_INSENSITIVE, "case-insensitive"),
        (sym_attr::WEAK, "weak"),
        (sym_attr::STRONG, "strong"),
        (sym_attr::COMMON, "common"),
        (sym_attr::CODE_DATUM, "code-datum"),
    ] {
        if attr & bit != 0 {
            v.push(name);
        }
    }
    // Print the raw word as well. The named bits are only the ones we
    // understand. Reading ObjAsm's objects is how we learn what the rest
    // mean.
    format!("{} [{attr:02X}]", v.join(", "))
}

// ------------------------------------------------------------- comparison

/// What differs between two objects, in the order a reader wants it.
///
/// The bytes decide whether the objects match. The other differences (area
/// names, attributes, symbols, relocations) are reported because, when the
/// bytes differ, one of them usually explains why.
fn compare(ours: &aof::Object, theirs: &aof::Object, limit: usize) -> Vec<String> {
    let mut out = Vec::new();
    let mut say = |s: String| out.push(s);

    // --- areas, matched by name rather than position ---------------------
    let names_a: Vec<&str> = ours.areas.iter().map(|a| a.name.as_str()).collect();
    let names_b: Vec<&str> = theirs.areas.iter().map(|a| a.name.as_str()).collect();
    if names_a != names_b {
        say(format!("areas: ours {names_a:?}"));
        say(format!("       theirs {names_b:?}"));
    }
    for a in &ours.areas {
        let Some(b) = theirs.areas.iter().find(|b| b.name == a.name) else {
            say(format!("area {}: only we produced it", a.name));
            continue;
        };
        if a.attributes != b.attributes {
            say(format!(
                "area {}: attributes {:06X} against {:06X}",
                a.name, a.attributes, b.attributes
            ));
        }
        if a.alignment != b.alignment {
            say(format!(
                "area {}: alignment 2^{} against 2^{}",
                a.name, a.alignment, b.alignment
            ));
        }
        if a.data.len() != b.data.len() {
            say(format!(
                "area {}: {} bytes against {}",
                a.name,
                a.data.len(),
                b.data.len()
            ));
        }
        // Compare the bytes a word at a time, because that is how they were
        // made.
        let n = a.data.len().min(b.data.len());
        let mut differing = 0usize;
        for off in (0..n & !3).step_by(4) {
            let w = |d: &[u8]| u32::from_le_bytes([d[off], d[off + 1], d[off + 2], d[off + 3]]);
            let (x, y) = (w(&a.data), w(&b.data));
            if x != y {
                differing += 1;
                if differing <= limit {
                    say(format!("  {}+{off:04X}: {x:08X} against {y:08X}", a.name));
                }
            }
        }
        if differing > limit {
            say(format!("  ... and {} more words", differing - limit));
        }
        if differing == 0 && a.data.len() == b.data.len() {
            say(format!("area {}: {} bytes, identical", a.name, a.data.len()));
        } else {
            say(format!(
                "area {}: {differing} of {} words differ",
                a.name,
                n / 4
            ));
        }
        // --- relocations, by what they name rather than by index ----------
        //
        // A relocation holds the index of a symbol in its own file's symbol
        // table. Two files that agree completely still hold different
        // indexes whenever their tables are in a different order. So what
        // has to match is the name, not the number.
        let named = |o: &aof::Object, r: &aof::Reloc| match r.by {
            aof::RelocBy::Symbol(i) => o
                .symbols
                .get(i as usize)
                .map(|s| format!("symbol {}", s.name))
                .unwrap_or_else(|| format!("symbol #{i}")),
            aof::RelocBy::Area(i) => o
                .areas
                .get(i as usize)
                .map(|x| format!("area {}", x.name))
                .unwrap_or_else(|| format!("area #{i}")),
        };
        let mut ra: Vec<_> = a
            .relocs
            .iter()
            .map(|r| (r.offset, named(ours, r), format!("{:?}", r.field)))
            .collect();
        let mut rb: Vec<_> = b
            .relocs
            .iter()
            .map(|r| (r.offset, named(theirs, r), format!("{:?}", r.field)))
            .collect();
        ra.sort();
        rb.sort();
        if ra != rb {
            say(format!(
                "area {}: {} relocations against {}",
                a.name,
                ra.len(),
                rb.len()
            ));
            for r in ra.iter().filter(|r| !rb.contains(r)).take(limit) {
                say(format!("  only ours:   &{:04X} {} {}", r.0, r.1, r.2));
            }
            for r in rb.iter().filter(|r| !ra.contains(r)).take(limit) {
                say(format!("  only theirs: &{:04X} {} {}", r.0, r.1, r.2));
            }
        }
    }
    for b in &theirs.areas {
        if !ours.areas.iter().any(|a| a.name == b.name) {
            say(format!("area {}: only they produced it", b.name));
        }
    }

    // --- symbols, by name and by which one of that name ------------------
    //
    // `$a` and `$d` repeat. An object has one at every point where its
    // content changes between code and data. Matching on the name alone
    // would compare every one of ours with the first of theirs. That shows
    // a difference at every boundary after the first, when nothing is
    // wrong. So the k-th symbol of a name in ours is compared with the k-th
    // of that name in theirs.
    let nth = |o: &aof::Object, n: &str, k: usize| {
        o.symbols.iter().filter(|s| s.name == n).nth(k).cloned()
    };
    let mut seen: std::collections::HashMap<&str, usize> = std::collections::HashMap::new();
    for s in &ours.symbols {
        let k = seen.entry(s.name.as_str()).or_insert(0);
        let which = *k;
        *k += 1;
        match nth(theirs, &s.name, which) {
            None => say(format!("symbol {}: only ours", s.name)),
            Some(t) => {
                if s.value != t.value {
                    say(format!(
                        "symbol {}: value &{:X} against &{:X}",
                        s.name, s.value, t.value
                    ));
                }
                if s.attributes != t.attributes {
                    say(format!(
                        "symbol {}: attributes {:02X} against {:02X}",
                        s.name, s.attributes, t.attributes
                    ));
                }
            }
        }
    }
    for t in &theirs.symbols {
        if !ours.symbols.iter().any(|s| s.name == t.name) {
            say(format!("symbol {}: only theirs", t.name));
        }
    }
    out
}

/// Read an object, or say why not and stop.
fn load(path: &str) -> aof::Object {
    let bytes = match std::fs::read(path) {
        Ok(b) => b,
        Err(e) => {
            eprintln!("aofdump: {path}: {e}");
            std::process::exit(1);
        }
    };
    match aof::read(&bytes) {
        Ok(o) => o,
        Err(e) => {
            eprintln!("aofdump: {path}: {e}");
            std::process::exit(1);
        }
    }
}

fn main() {
    let args: Vec<String> = std::env::args().skip(1).collect();
    let hex = args.iter().any(|a| a == "-x");
    let flag = |name: &str| {
        args.iter()
            .position(|a| a == name)
            .and_then(|i| args.get(i + 1))
            .cloned()
    };
    let against = flag("--against");
    let limit: usize = flag("-n").and_then(|v| v.parse().ok()).unwrap_or(12);
    let skip: Vec<&str> = vec!["--against", "-n"];
    let positional: Vec<&String> = args
        .iter()
        .enumerate()
        .filter(|(i, a)| {
            !a.starts_with('-')
                && !(*i > 0 && skip.contains(&args[i - 1].as_str()))
        })
        .map(|(_, a)| a)
        .collect();
    let Some(path) = positional.first() else {
        eprintln!(
            "usage: aofdump <object> [-x]\n\
             \x20      aofdump <object> --hash\n\
             \x20      aofdump <ours> --against <theirs> [-n count]"
        );
        std::process::exit(2);
    };
    let obj = load(path);

    if args.iter().any(|a| a == "--hash") {
        println!("{:016x}  {path}", aof::content_hash(&obj));
        return;
    }

    if let Some(other) = against {
        let theirs = load(&other);
        let (h1, h2) = (aof::content_hash(&obj), aof::content_hash(&theirs));
        if h1 == h2 {
            println!("identical: {h1:016x}");
            println!("  {path}");
            println!("  {other}");
            return;
        }
        println!("different: {h1:016x} against {h2:016x}");
        println!("  ours   {path}");
        println!("  theirs {other}");
        println!();
        for line in compare(&obj, &theirs, limit) {
            println!("{line}");
        }
        std::process::exit(1);
    }

    println!("** Object file {path}");
    println!("   produced by: {}", obj.identification);
    match obj.entry {
        Some((a, off)) => println!("   entry point: area {a}, offset &{off:X}"),
        None => println!("   entry point: none"),
    }
    println!("   {} area(s), {} symbol(s)", obj.areas.len(), obj.symbols.len());

    for (i, a) in obj.areas.iter().enumerate() {
        let size = (a.data.len() as u32).max(a.reserved);
        println!();
        println!("** Area {i}: {}", a.name);
        println!(
            "   size &{size:X} ({size}), alignment 2^{}, attributes: {}",
            a.alignment,
            describe_area(a.attributes)
        );
        if a.attributes & area_attr::ABSOLUTE != 0 {
            println!("   base &{:08X}", a.base);
        }
        if hex && !a.data.is_empty() {
            for (n, w) in a.data.chunks(4).enumerate() {
                let mut b = [0u8; 4];
                b[..w.len()].copy_from_slice(w);
                let word = u32::from_le_bytes(b);
                let ascii: String = w
                    .iter()
                    .map(|c| if (32..127).contains(c) { *c as char } else { '.' })
                    .collect();
                println!("   &{:04X}: {word:08X}  {ascii}", n * 4);
            }
        }
        for r in &a.relocs {
            let by = match r.by {
                RelocBy::Area(n) => format!("area {n}"),
                RelocBy::Symbol(n) => format!("symbol {n}"),
            };
            let kind = match r.field {
                FieldType::Byte => "byte",
                FieldType::HalfWord => "halfword",
                FieldType::Word => "word",
                FieldType::Instruction => "instruction",
            };
            let mut how = vec![kind.to_string(), format!("by {by}")];
            if r.pc_relative {
                how.push("pc-relative".into());
            }
            if r.based {
                how.push("based".into());
            }
            if r.field == FieldType::Instruction && r.max_instructions != 0 {
                how.push(format!("{} instruction(s)", r.max_instructions));
            }
            println!("   reloc &{:04X}: {}", r.offset, how.join(", "));
        }
    }

    if !obj.symbols.is_empty() {
        println!();
        println!("** Symbol table");
        for s in &obj.symbols {
            let where_ = match &s.area {
                Some(a) => format!("{a} + &{:X}", s.value),
                None => format!("&{:X}", s.value),
            };
            println!("   {:<24} {where_:<24} {}", s.name, describe_symbol(s.attributes));
        }
    }
}
