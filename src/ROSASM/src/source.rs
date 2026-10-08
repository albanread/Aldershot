//! Source loading and corpus discovery.
//!
//! RISC OS text is Latin-1, not UTF-8. Line endings in the ROOL tree are a
//! mix of LF, CRLF and bare CR, because the tarballs keep whatever the
//! component's author last used. Both are normalised here, so nothing
//! downstream has to deal with them.

use std::fs;
use std::path::{Path, PathBuf};

/// One loaded source file, split into lines. Line n is at index n-1.
pub struct SourceFile {
    pub path: PathBuf,
    pub lines: Vec<String>,
}

/// Decode Latin-1. Every byte is a valid code point, so this cannot fail.
/// That is what we want: a stray &A0 in a comment must not stop a build.
fn latin1(bytes: &[u8]) -> String {
    bytes.iter().map(|&b| b as char).collect()
}

/// Split on LF, CRLF or bare CR, dropping the terminator.
fn split_lines(text: &str) -> Vec<String> {
    let mut out = Vec::new();
    let mut cur = String::new();
    let mut chars = text.chars().peekable();
    while let Some(c) = chars.next() {
        match c {
            '\n' => {
                out.push(std::mem::take(&mut cur));
            }
            '\r' => {
                // CRLF collapses to one break; bare CR is also a break.
                if chars.peek() == Some(&'\n') {
                    chars.next();
                }
                out.push(std::mem::take(&mut cur));
            }
            _ => cur.push(c),
        }
    }
    // A trailing line without a terminator still counts.
    if !cur.is_empty() {
        out.push(cur);
    }
    out
}

impl SourceFile {
    pub fn load(path: &Path) -> std::io::Result<Self> {
        let bytes = fs::read(path)?;
        Ok(SourceFile {
            path: path.to_path_buf(),
            lines: split_lines(&latin1(&bytes)),
        })
    }
}

/// Directories that hold ObjAsm input in the RISC OS source layout.
/// `s` is assembler; `hdr`/`Hdr` are its headers, pulled in with GET/INCLUDE.
const ASM_DIRS: [&str; 3] = ["s", "hdr", "Hdr"];

/// Walk a RISC OS source tree and collect every ObjAsm file.
///
/// The layout is `<Component>/s/<Name>`: files with no extension, inside a
/// directory named for the language. This is the reverse of the Unix
/// convention.
pub fn discover(root: &Path) -> Vec<PathBuf> {
    let mut out = Vec::new();
    walk(root, &mut out);
    out.sort();
    out
}

fn walk(dir: &Path, out: &mut Vec<PathBuf>) {
    let entries = match fs::read_dir(dir) {
        Ok(e) => e,
        Err(_) => return,
    };
    for entry in entries.flatten() {
        let path = entry.path();
        let Ok(ft) = entry.file_type() else { continue };
        if ft.is_dir() {
            let name = entry.file_name();
            let name = name.to_string_lossy();
            if name == ".git" {
                continue;
            }
            if ASM_DIRS.contains(&name.as_ref()) {
                // Everything directly inside is assembler, whatever it's called.
                if let Ok(files) = fs::read_dir(&path) {
                    for f in files.flatten() {
                        if f.file_type().map(|t| t.is_file()).unwrap_or(false) {
                            out.push(f.path());
                        }
                    }
                }
            } else {
                walk(&path, out);
            }
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn line_endings_all_normalise() {
        assert_eq!(split_lines("a\nb\nc"), vec!["a", "b", "c"]);
        assert_eq!(split_lines("a\r\nb\r\nc"), vec!["a", "b", "c"]);
        assert_eq!(split_lines("a\rb\rc"), vec!["a", "b", "c"]);
        assert_eq!(split_lines("a\r\nb\nc\rd"), vec!["a", "b", "c", "d"]);
    }

    #[test]
    fn trailing_newline_does_not_add_empty_line() {
        assert_eq!(split_lines("a\nb\n"), vec!["a", "b"]);
    }

    #[test]
    fn latin1_high_bytes_survive() {
        // &A9 is the copyright sign in Latin-1 and appears in ROOL headers.
        assert_eq!(latin1(&[0x41, 0xA9, 0x42]), "A\u{A9}B");
    }
}
