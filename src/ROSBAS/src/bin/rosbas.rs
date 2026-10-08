//! rosbas: the command for the BBC BASIC V compiler.
//!
//!     rosbas list FILE           list a program, tokenised or text, as LIST would
//!     rosbas tokenise FILE OUT   write FILE as the tokenised program BASIC loads
//!     rosbas compile FILE [-o OUT.c] [-L DIR]... [--quirk NAME]... [--quirks all]
//!                                compile FILE to C (stdout without -o); LIBRARY
//!                                looks in FILE's directory, then each DIR
//!     rosbas build FILE [-o EXE] [-L DIR]... [--quirk NAME]... [--quirks all]
//!                                compile FILE and build it for this machine
//!                                with the runtime (the host layer), by cc
//!     rosbas --version
//!
//! `build` finds the runtime in ROSBAS_RUNTIME, or else in the crate's
//! runtime/ directory next to target/. By default EXE is FILE without its
//! suffix.
//!
//! A text FILE is loaded in the same way as BASIC's TEXTLOAD loads it (spec
//! chapter 2). Both `list` and `tokenise` print on stderr the rejections of
//! chapter 2, and the warnings that the interpreter prints while loading
//! ("Failed with ...", "Program renumbered"). After a rejection, `list`
//! still lists what was loaded. `tokenise` still writes it, unless a line
//! is too long for the tokenised form. It then exits with status 1 if the
//! compiler would reject the program.

use rosbas::program::list_text;
use rosbas::textload::{load_either, Diagnostic, TextLoad};
use std::io::Write;
use std::process::ExitCode;

fn usage() -> ExitCode {
    eprintln!("usage: rosbas list FILE | rosbas tokenise FILE OUT");
    eprintln!("       rosbas compile FILE [-o OUT.c] [-L DIR]... [--quirk NAME]... [--quirks all]");
    eprintln!("       rosbas build FILE [-o EXE] [-L DIR]... [--quirk NAME]... [--quirks all]");
    ExitCode::from(2)
}

/// The options of `compile` and `build`: FILE, -o, -L and the quirks
/// (§1.3.9, §15.3.1).
fn options(args: &[String]) -> Option<(String, Option<String>, u32, Vec<std::path::PathBuf>)> {
    let (mut file, mut out, mut quirks, mut dirs) = (None, None, 0, Vec::new());
    let mut i = 0;
    while i < args.len() {
        match args[i].as_str() {
            "-o" => {
                i += 1;
                out = Some(args.get(i)?.clone());
            }
            "-L" => {
                i += 1;
                dirs.push(args.get(i)?.into());
            }
            "--quirk" | "--quirks" => {
                i += 1;
                match rosbas::driver::quirk_bits(args.get(i)?) {
                    Some(b) => quirks |= b,
                    None => {
                        eprintln!("rosbas: no quirk called {}", args[i]);
                        return None;
                    }
                }
            }
            f if file.is_none() && !f.starts_with('-') => file = Some(f.to_string()),
            _ => return None,
        }
        i += 1;
    }
    Some((file?, out, quirks, dirs))
}

/// Compiles FILE to C and prints the warnings. Returns None if the program
/// is rejected.
fn compile(file: &str, quirks: u32, dirs: &[std::path::PathBuf]) -> Option<String> {
    let bytes = match std::fs::read(file) {
        Ok(b) => b,
        Err(e) => {
            eprintln!("rosbas: {file}: {e}");
            return None;
        }
    };
    match rosbas::driver::compile_full(&bytes, file, quirks, dirs) {
        Ok((c, warnings)) => {
            for w in warnings {
                eprintln!("rosbas: {file}: {w}");
            }
            Some(c)
        }
        Err(ds) => {
            for d in ds {
                eprintln!("rosbas: {file}: {d}");
            }
            None
        }
    }
}

/// The runtime's directory (design/compiler.md, "The runtime library").
fn runtime_dir() -> std::path::PathBuf {
    if let Ok(d) = std::env::var("ROSBAS_RUNTIME") {
        return d.into();
    }
    // From target/<profile>/rosbas, go up to the crate and use its runtime/
    // directory.
    let exe = std::env::current_exe().unwrap_or_default();
    exe.parent().and_then(|p| p.parent()).and_then(|p| p.parent()).map(|p| p.join("runtime")).unwrap_or_else(|| "runtime".into())
}

/// Builds C for this machine. It uses the runtime's layer for the host
/// operating system and leaves out the RISC OS layer.
fn build(c: &str, exe: &str) -> bool {
    let rt = runtime_dir();
    let mut sources: Vec<std::path::PathBuf> = match std::fs::read_dir(&rt) {
        Ok(d) => d
            .filter_map(|e| e.ok().map(|e| e.path()))
            .filter(|p| p.extension().is_some_and(|x| x == "c") && !p.file_name().is_some_and(|n| n.to_string_lossy().starts_with("rb_os_riscos")))
            .collect(),
        Err(e) => {
            eprintln!("rosbas: the runtime at {}: {e}", rt.display());
            return false;
        }
    };
    sources.sort();
    let src = std::env::temp_dir().join(format!("rosbas-{}.c", std::process::id()));
    if let Err(e) = std::fs::write(&src, c) {
        eprintln!("rosbas: {}: {e}", src.display());
        return false;
    }
    let status = std::process::Command::new(std::env::var("CC").unwrap_or_else(|_| "cc".into()))
        .args(["-std=c99", "-O2", "-ffp-contract=off", "-w"])
        .arg(format!("-I{}", rt.display()))
        .arg(&src)
        .args(&sources)
        .args(["-lm", "-lpthread", "-o", exe])
        .status();
    let _ = std::fs::remove_file(&src);
    matches!(status, Ok(s) if s.success())
}

/// Reads FILE. It is read as tokenised if it is a whole tokenised program,
/// and as text otherwise (spec §2.1.2). Prints the rejections and the
/// loader's warnings, then returns the program and the rejections.
fn read(path: &str) -> Option<(TextLoad, Vec<Diagnostic>)> {
    let bytes = match std::fs::read(path) {
        Ok(b) => b,
        Err(e) => {
            eprintln!("rosbas: {path}: {e}");
            return None;
        }
    };
    let (loaded, rejections) = load_either(&bytes);
    for w in &loaded.warnings {
        eprintln!("rosbas: {path}:{w}");
    }
    if loaded.renumbered {
        eprintln!("rosbas: {path}: Program renumbered");
    }
    for r in &rejections {
        eprintln!("rosbas: {path}:{}{r}", if r.text_line.is_some() { "" } else { " " });
    }
    Some((loaded, rejections))
}

fn main() -> ExitCode {
    let args: Vec<String> = std::env::args().skip(1).collect();
    match args.first().map(String::as_str) {
        Some("--version") => {
            println!("rosbas {}", env!("CARGO_PKG_VERSION"));
            ExitCode::SUCCESS
        }
        Some("list") if args.len() == 2 => {
            let Some((loaded, _)) = read(&args[1]) else {
                return ExitCode::FAILURE;
            };
            let mut out = std::io::stdout().lock();
            for line in &loaded.program.lines {
                let _ = write!(out, "{:5}", line.number);
                let _ = out.write_all(&list_text(&line.text));
                let _ = out.write_all(b"\n");
            }
            ExitCode::SUCCESS
        }
        Some("tokenise") if args.len() == 3 => {
            let Some((loaded, rejections)) = read(&args[1]) else {
                return ExitCode::FAILURE;
            };
            // A line body of more than 251 bytes cannot be given a length
            // byte, so the file is not written (spec §2.3.7).
            if loaded.program.lines.iter().all(|l| l.text.len() <= 251) {
                if let Err(e) = std::fs::write(&args[2], loaded.program.to_tokenised()) {
                    eprintln!("rosbas: {}: {e}", args[2]);
                    return ExitCode::FAILURE;
                }
            }
            if rejections.is_empty() {
                ExitCode::SUCCESS
            } else {
                ExitCode::FAILURE
            }
        }
        Some("compile") => {
            let Some((file, out, quirks, dirs)) = options(&args[1..]) else {
                return usage();
            };
            let Some(c) = compile(&file, quirks, &dirs) else {
                return ExitCode::FAILURE;
            };
            match out {
                Some(o) => {
                    if let Err(e) = std::fs::write(&o, c) {
                        eprintln!("rosbas: {o}: {e}");
                        return ExitCode::FAILURE;
                    }
                }
                None => print!("{c}"),
            }
            ExitCode::SUCCESS
        }
        Some("build") => {
            let Some((file, out, quirks, dirs)) = options(&args[1..]) else {
                return usage();
            };
            let exe = out.unwrap_or_else(|| {
                let p = std::path::Path::new(&file);
                let stem = p.file_stem().map(|s| s.to_string_lossy().to_string()).unwrap_or_else(|| "a.out".into());
                // Remove a RISC OS filetype suffix too, so that prog,ffb
                // becomes prog.
                let stem = stem.split(',').next().unwrap_or("a.out").to_string();
                p.with_file_name(stem).to_string_lossy().to_string()
            });
            let Some(c) = compile(&file, quirks, &dirs) else {
                return ExitCode::FAILURE;
            };
            if build(&c, &exe) {
                ExitCode::SUCCESS
            } else {
                ExitCode::FAILURE
            }
        }
        _ => usage(),
    }
}
