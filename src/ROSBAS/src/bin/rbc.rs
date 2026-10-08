//! rbc FILE [OUT.c] [--quirk NAME]...
//!
//! Compiles a BASIC program to C. It stands in for `rosbas compile` while
//! the command-line tool is being changed.
use std::process::ExitCode;

fn main() -> ExitCode {
    let args: Vec<String> = std::env::args().skip(1).collect();
    let mut quirks = 0;
    let mut files = Vec::new();
    let mut i = 0;
    while i < args.len() {
        if args[i] == "--quirk" || args[i] == "--quirks" {
            i += 1;
            match args.get(i).and_then(|n| rosbas::driver::quirk_bits(n)) {
                Some(b) => quirks |= b,
                None => {
                    eprintln!("rbc: unknown quirk");
                    return ExitCode::from(2);
                }
            }
        } else {
            files.push(args[i].clone());
        }
        i += 1;
    }
    let Some(src) = files.first() else {
        eprintln!("usage: rbc FILE [OUT.c] [--quirk NAME]...");
        return ExitCode::from(2);
    };
    let bytes = match std::fs::read(src) {
        Ok(b) => b,
        Err(e) => {
            eprintln!("rbc: {src}: {e}");
            return ExitCode::FAILURE;
        }
    };
    match rosbas::driver::compile_with_warnings(&bytes, src, quirks) {
        Ok((c, warnings)) => {
            for w in warnings {
                eprintln!("{src}: {w}");
            }
            match files.get(1) {
                Some(out) => {
                    if let Err(e) = std::fs::write(out, c) {
                        eprintln!("rbc: {out}: {e}");
                        return ExitCode::FAILURE;
                    }
                }
                None => print!("{c}"),
            }
            ExitCode::SUCCESS
        }
        Err(ds) => {
            for d in ds {
                eprintln!("{src}: {d}");
            }
            ExitCode::FAILURE
        }
    }
}
