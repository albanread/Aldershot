//! A BASIC program as BASIC holds it: numbered lines of tokenised text.
//!
//! A tokenised file (spec chapter 2) is a sequence of lines. Each line is
//! `13, number high, number low, length, text...`, and the length counts
//! those four bytes. The file ends `13, &FF`. This module reads that form
//! and lists a line as `LIST` would show it, with the keywords spelt out.
//! Reading programs written as text, which is what `TEXTLOAD`'s tokeniser
//! does, follows spec chapter 2.

use crate::tokens::*;
use std::fmt;

/// One line: its number and its tokenised text (without the 13 and the
/// four-byte header).
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Line {
    pub number: u16,
    pub text: Vec<u8>,
}

#[derive(Debug, Clone, Default, PartialEq, Eq)]
pub struct Program {
    pub lines: Vec<Line>,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub enum LoadError {
    /// Not a tokenised program. The value is the byte offset where the
    /// structure fails.
    NotTokenised(usize),
}

impl fmt::Display for LoadError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            LoadError::NotTokenised(at) => write!(f, "not a tokenised BASIC program (at byte {at})"),
        }
    }
}

impl std::error::Error for LoadError {}

impl Program {
    /// True if `bytes` is a whole tokenised program.
    pub fn is_tokenised(bytes: &[u8]) -> bool {
        Program::from_tokenised(bytes).is_ok()
    }

    /// Reads a tokenised program.  Bytes after the end marker are ignored,
    /// as BASIC ignores them.
    pub fn from_tokenised(bytes: &[u8]) -> Result<Program, LoadError> {
        let mut lines = Vec::new();
        let mut at = 0;
        loop {
            if bytes.get(at) != Some(&13) {
                return Err(LoadError::NotTokenised(at));
            }
            match bytes.get(at + 1) {
                Some(&0xFF) => return Ok(Program { lines }),
                None => return Err(LoadError::NotTokenised(at + 1)),
                _ => {}
            }
            let (hi, lo, len) = match (bytes.get(at + 1), bytes.get(at + 2), bytes.get(at + 3)) {
                (Some(&h), Some(&l), Some(&n)) => (h, l, n as usize),
                _ => return Err(LoadError::NotTokenised(at)),
            };
            if len < 4 || at + len > bytes.len() {
                return Err(LoadError::NotTokenised(at + 3));
            }
            lines.push(Line { number: u16::from(hi) << 8 | u16::from(lo), text: bytes[at + 4..at + len].to_vec() });
            at += len;
        }
    }

    /// The program as a tokenised file.
    pub fn to_tokenised(&self) -> Vec<u8> {
        let mut out = Vec::new();
        for l in &self.lines {
            out.extend_from_slice(&[13, (l.number >> 8) as u8, l.number as u8, (l.text.len() + 4) as u8]);
            out.extend_from_slice(&l.text);
        }
        out.extend_from_slice(&[13, 0xFF]);
        out
    }
}

/// The spelling of a one-byte token, if it is one.
pub fn one_byte(token: u8) -> Option<&'static str> {
    if token == TELSE2 {
        return Some("ELSE");
    }
    // The left-hand (statement) forms of the pseudo-variables PTR, PAGE,
    // TIME, LOMEM and HIMEM have tokens above their function tokens. The
    // difference is TPTR2 minus PTR's token.
    let first = |t: u8| KEYWORDS.iter().find(|k| k.job & 8 == 0 && k.token == t).map(|k| k.word);
    if (TPTR2..TPTR2 + 5).contains(&token) {
        return first(token - (TPTR2 - 0x8F));
    }
    first(token)
}

/// The spelling of a two-byte token. `escape` is TESCFN, TESCCOM or
/// TESCSTMT.
pub fn two_byte(escape: u8, token: u8) -> Option<&'static str> {
    KEYWORDS
        .iter()
        .find(|k| {
            k.job & 8 != 0
                && k.token == token
                && match escape {
                    TESCFN => k.job & 4 != 0,
                    TESCSTMT => k.job & 64 != 0,
                    TESCCOM => k.job & (4 | 64) == 0,
                    _ => false,
                }
        })
        .map(|k| k.word)
}

/// A line-number constant: the three bytes after TCONST.
pub fn line_constant(b: [u8; 3]) -> u16 {
    let r0 = u32::from(b[0]) << 2;
    let lo = (r0 & 0xC0) ^ u32::from(b[1]);
    let hi = (u32::from(b[2]) ^ (r0 << 2)) & 0xFF;
    (lo | hi << 8) as u16
}

/// A line's text with its keywords spelt out, as LIST shows it. The line
/// number is not included. Bytes are Latin-1.
pub fn list_text(text: &[u8]) -> Vec<u8> {
    let mut out = Vec::new();
    let mut i = 0;
    let mut quoted = false;
    let mut statement_start = true;
    while i < text.len() {
        let c = text[i];
        i += 1;
        if quoted {
            out.push(c);
            quoted = c != b'"';
            continue;
        }
        match c {
            b'"' => {
                out.push(c);
                quoted = true;
                statement_start = false;
            }
            b'*' if statement_start => {
                // A star command. The rest of the line is copied as it is.
                out.extend_from_slice(&text[i - 1..]);
                break;
            }
            b' ' => out.push(c),
            b':' => {
                out.push(c);
                statement_start = true;
            }
            TCONST if i + 3 <= text.len() => {
                out.extend_from_slice(line_constant([text[i], text[i + 1], text[i + 2]]).to_string().as_bytes());
                i += 3;
                statement_start = false;
            }
            TESCFN | TESCCOM | TESCSTMT if i < text.len() => {
                match two_byte(c, text[i]) {
                    Some(w) => out.extend_from_slice(w.as_bytes()),
                    None => out.extend_from_slice(&[c, text[i]]),
                }
                i += 1;
                statement_start = false;
            }
            0x7F..=0xFF => {
                match one_byte(c) {
                    Some(w) => out.extend_from_slice(w.as_bytes()),
                    None => out.push(c),
                }
                if c == TREM || c == TDATA {
                    out.extend_from_slice(&text[i..]);
                    break;
                }
                statement_start = c == TTHEN || c == TELSE || c == TELSE2;
            }
            _ => {
                out.push(c);
                statement_start = false;
            }
        }
    }
    out
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn line_constants_round_trip() {
        // CONSTI, which is how BASIC encodes a line number after GOTO and
        // similar keywords.
        fn consti(n: u32) -> [u8; 3] {
            [(((n >> 8) >> 4) & 0x0C | (n & 0xC0) >> 2) as u8 ^ 0x54, ((n & 0x3F) | 0x40) as u8, (((n >> 8) & 0x3F) | 0x40) as u8]
        }
        for n in [0u32, 1, 10, 63, 64, 255, 256, 1000, 32767, 65279] {
            assert_eq!(u32::from(line_constant(consti(n))), n);
        }
    }

    #[test]
    fn lists_a_small_program() {
        // 10 PRINT "A":GOTO 10  /  20 REM PRINT stays
        let mut bytes = vec![13, 0, 10, 0];
        let body = [TPRINT, b' ', b'"', b'A', b'"', b':', TGOTO, b' ', TCONST, 0x54, 0x4A, 0x40];
        bytes[3] = (body.len() + 4) as u8;
        bytes.extend_from_slice(&body);
        bytes.extend_from_slice(&[13, 0, 20, 0]);
        let rem = [TREM, b' ', b'P', b'R', b'I', b'N', b'T'];
        let n = bytes.len() - 1;
        bytes[n] = (rem.len() + 4) as u8;
        bytes.extend_from_slice(&rem);
        bytes.extend_from_slice(&[13, 0xFF]);
        let p = Program::from_tokenised(&bytes).unwrap();
        assert_eq!(p.lines.len(), 2);
        assert_eq!(list_text(&p.lines[0].text), b"PRINT \"A\":GOTO 10".to_vec());
        assert_eq!(list_text(&p.lines[1].text), b"REM PRINT".to_vec());
        assert_eq!(p.to_tokenised(), bytes);
    }

    #[test]
    fn two_byte_and_left_hand_tokens() {
        assert_eq!(two_byte(TESCSTMT, 0x8E), Some("CASE"));
        assert_eq!(two_byte(TESCCOM, 0x8E), Some("APPEND"));
        assert_eq!(two_byte(TESCFN, 0x8E), Some("SUM"));
        assert_eq!(one_byte(TPTR2), Some("PTR"));
        assert_eq!(one_byte(TPTR2 + 4), Some("HIMEM"));
        assert_eq!(one_byte(0x93), Some("HIMEM"));
        assert_eq!(one_byte(0xFB), Some("COLOUR"));
    }
}
