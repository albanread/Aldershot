//! TEXTLOAD: a program written as text, tokenised in the same way as BASIC
//! loads it.
//!
//! A text program (spec §2.3) is a sequence of lines. Each line is ended by
//! a linefeed or a carriage return. BASIC's loader (`LOADFILE0`, s/Command)
//! passes each line to its tokeniser (`MATCH`, s/Lexical) as a program line,
//! and then stores it. A line with a number is stored by its number
//! (`INSRT`, s/Basic). A line with no number is stored at the end with the
//! *current number* (`INSERT`). If any line had no number, the loader then
//! renumbers the whole program 10, 11, 12 ... (`RENUM1`, s/Command) and
//! prints "Program renumbered". This module does the same, byte for byte
//! and line number for line number. The numbers are the ones that `ERL`
//! reports, so they must be exact.
//!
//! [`load`] reads a file of either form (§2.1.2). [`textload`] reads text.
//! [`check_tokenised`] checks a tokenised program against the rules that
//! can reject one. The rejections of chapter 2 are returned as
//! [`Diagnostic`]s with the texts that the specification gives.

use crate::program::{line_constant, Line, Program};
use crate::tokens::*;
use std::collections::HashMap;
use std::fmt;

/// A rejection or a warning. Rejections carry the texts that chapter 2
/// gives.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Diagnostic {
    /// The line of the text that this concerns, counting from 1, when the
    /// program is text. Every linefeed and every carriage return ends a line
    /// (§2.3.1). So this count includes blank lines, and CR LF counts as two
    /// lines.
    pub text_line: Option<usize>,
    pub message: String,
}

impl fmt::Display for Diagnostic {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self.text_line {
            Some(l) => write!(f, "{l}: {}", self.message),
            None => f.write_str(&self.message),
        }
    }
}

/// A loaded program.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct TextLoad {
    /// The program as the interpreter holds it after loading.
    pub program: Program,
    /// For each line of `program`, the line of the text that it came from,
    /// counting from 1. This is empty when the file was tokenised.
    pub text_lines: Vec<usize>,
    /// True when the file was already tokenised.
    pub tokenised: bool,
    /// True when the loader renumbered the program because a line had no
    /// number (§2.3.8). The interpreter then prints "Program renumbered".
    pub renumbered: bool,
    /// What the interpreter prints while renumbering. This is "Failed with
    /// *r* on line *l*" for each reference that matches no line (§2.3.9).
    pub warnings: Vec<Diagnostic>,
}

/// Reads a program file of either form (§2.1.2). The file is read as
/// tokenised when it is a whole tokenised program, and as text otherwise.
/// A tokenised program is checked by [`check_tokenised`], and a text
/// program by [`textload`].
pub fn load(bytes: &[u8]) -> Result<TextLoad, Vec<Diagnostic>> {
    let (loaded, rejections) = load_either(bytes);
    if rejections.is_empty() {
        Ok(loaded)
    } else {
        Err(rejections)
    }
}

/// The same as [`load`], but returns the program together with the
/// rejections (see [`load_text`]).
pub fn load_either(bytes: &[u8]) -> (TextLoad, Vec<Diagnostic>) {
    match Program::from_tokenised(bytes) {
        Ok(program) => {
            let rejections = check_tokenised(&program);
            let loaded =
                TextLoad { program, text_lines: Vec::new(), tokenised: true, renumbered: false, warnings: Vec::new() };
            (loaded, rejections)
        }
        Err(_) => load_text(bytes),
    }
}

/// Loads a text program as TEXTLOAD does. Rejects it in the cases where
/// chapter 2 says that the compiler does: "Missing line end at end of file"
/// (§2.3.2), "Line too long" (§2.3.7) and "Control character in line *n*"
/// (§2.3.14). Every rejection is reported, including those after the
/// first.
pub fn textload(src: &[u8]) -> Result<TextLoad, Vec<Diagnostic>> {
    let (loaded, rejections) = load_text(src);
    if rejections.is_empty() {
        Ok(loaded)
    } else {
        Err(rejections)
    }
}

/// Loads a text program as TEXTLOAD does, and returns it together with the
/// rejections. [`textload`] turns the rejections into an error. This
/// function lets a tool still see what the interpreter would have run. A
/// line that is too long for the interpreter (§2.3.7) is kept whole, so the
/// lines after it are numbered as if it fitted. Such a program cannot be
/// written as a tokenised file.
pub fn load_text(src: &[u8]) -> (TextLoad, Vec<Diagnostic>) {
    let mut rejections = Vec::new();

    // §2.3.1: each linefeed and each carriage return ends exactly one line.
    // MATCH stops at the first of them (it turns 10 into 13), and the next
    // line begins after it. So CR LF ends a line and then an empty line.
    let mut lines: Vec<&[u8]> = src.split(|&b| b == 10 || b == 13).collect();
    let last = lines.pop().unwrap_or_default();
    if src.is_empty() || !last.is_empty() {
        // §2.3.2: the loader would read on past the end of the file. An
        // empty file is treated the same way, because LOADFILE0 tokenises
        // at least one line.
        let at = if src.is_empty() { None } else { Some(lines.len() + 1) };
        rejections.push(Diagnostic { text_line: at, message: "Missing line end at end of file".into() });
        if !last.is_empty() {
            lines.push(last);
        }
    }

    // §2.3.4: the program is built one text line at a time. `current` is
    // R4 in LOADFILE0. It starts at 9 and is set to the number of each
    // numbered line. `sorted` records that the numbers are still in
    // ascending order, so that a search can use binary chop. Only
    // unnumbered lines can spoil the order.
    let mut store: Vec<Stored> = Vec::new();
    let mut current: u16 = 9;
    let mut renumber = false;
    let mut sorted = true;
    for (k, text) in lines.iter().enumerate() {
        let text_line = k + 1;
        let (bytes, control) = crunch(text);
        match line_number(&bytes) {
            Some((n, body)) => {
                current = n;
                // REMOVE in INSRT. This removes from the first line
                // numbered n or more, up to the first line after it that is
                // numbered n+1 or more.
                let from = first_at_least(&store, 0, u32::from(n), sorted);
                let to = first_at_least(&store, from, u32::from(n) + 1, sorted);
                store.drain(from..to);
                if body.is_empty() {
                    // Nothing followed the number, so the line is deleted.
                    continue;
                }
                let at = first_at_least(&store, 0, u32::from(n), sorted);
                let body = stored_body(body);
                too_long(&body, text_line, &mut rejections);
                store.insert(at, Stored { number: n, body, text_line, control });
            }
            None => {
                // INSERT puts the line at the end, with the current number
                // (§2.3.4 step 2).
                renumber = true;
                if store.last().is_some_and(|l| l.number > current) {
                    sorted = false;
                }
                let body = stored_body(&bytes);
                too_long(&body, text_line, &mut rejections);
                store.push(Stored { number: current, body, text_line, control });
            }
        }
    }

    let mut warnings = Vec::new();
    if renumber {
        renumber_program(&mut store, &mut warnings, &mut rejections);
    }

    // §2.3.14 is checked on the program as loaded. A line that a later
    // line replaced or deleted is not part of it.
    for l in &store {
        if l.control {
            rejections.push(Diagnostic {
                text_line: Some(l.text_line),
                message: format!("Control character in line {}", l.number),
            });
        }
    }
    rejections.sort_by_key(|d| d.text_line);

    let text_lines = store.iter().map(|l| l.text_line).collect();
    let program = Program { lines: store.into_iter().map(|l| Line { number: l.number, text: l.body }).collect() };
    (TextLoad { program, text_lines, tokenised: false, renumbered: renumber, warnings }, rejections)
}

/// The rejections that apply to a tokenised program. "Line numbers out of
/// order at line *n*" (§2.2.8) is given for each line whose number is not
/// greater than the one before it. "Control character in line *n*"
/// (§2.3.14) is given for each line with a control character outside a
/// string literal, a `REM` or `DATA` remainder and a `*` command.
pub fn check_tokenised(program: &Program) -> Vec<Diagnostic> {
    let mut rejections = Vec::new();
    for (i, l) in program.lines.iter().enumerate() {
        if i > 0 && l.number <= program.lines[i - 1].number {
            rejections.push(Diagnostic {
                text_line: None,
                message: format!("Line numbers out of order at line {}", l.number),
            });
        }
        if control_outside(&l.text) {
            rejections.push(Diagnostic { text_line: None, message: format!("Control character in line {}", l.number) });
        }
    }
    rejections
}

/// One line as the loader holds it while loading.
struct Stored {
    number: u16,
    body: Vec<u8>,
    text_line: usize,
    /// True if a control character stood outside a string, a REM or DATA
    /// remainder and a * command when the line was tokenised (§2.3.14).
    control: bool,
}

/// FNDLNO. Searching from `from`, returns the first line in program order
/// whose number is `n` or more, or else the end.
fn first_at_least(store: &[Stored], from: usize, n: u32, sorted: bool) -> usize {
    if sorted {
        from + store[from..].partition_point(|l| u32::from(l.number) < n)
    } else {
        from + store[from..].iter().position(|l| u32::from(l.number) >= n).unwrap_or(store.len() - from)
    }
}

/// SPTSTN. If the tokenised line begins with a line-number reference after
/// any spaces, returns its number and the body after it (§2.3.3).
///
/// The reference is normally one that the tokeniser made from digits. But a
/// raw &8D in the text is also a reference, whatever three bytes follow it.
/// Fewer than three bytes may follow, or they may give a number of 65280 or
/// more, whose high byte would read as the end of the program. In those
/// cases the interpreter's result depends on what is in memory. This
/// function treats such a line as unnumbered.
fn line_number(bytes: &[u8]) -> Option<(u16, &[u8])> {
    let p = bytes.iter().position(|&b| b != b' ')?;
    if bytes[p] != TCONST || bytes.len() < p + 4 {
        return None;
    }
    let n = line_constant([bytes[p + 1], bytes[p + 2], bytes[p + 3]]);
    (n <= 65279).then_some((n, &bytes[p + 4..]))
}

/// INSRTS. Returns the body as it is stored (§2.3.6). TRALSP removes the
/// spaces at the end but never looks at the first byte, so a body made of
/// spaces keeps one. INSLP1 turns an ELSE that is the first byte after any
/// spaces into the ELSE of a multi-line IF. (LISTO is 0 when a program is
/// loaded, so the spaces at the start stay.)
fn stored_body(body: &[u8]) -> Vec<u8> {
    let mut end = body.len();
    while end > 1 && body[end - 1] == b' ' {
        end -= 1;
    }
    let mut body = body[..end].to_vec();
    if let Some(p) = body.iter().position(|&b| b != b' ') {
        if body[p] == TELSE {
            body[p] = TELSE2;
        }
    }
    body
}

/// §2.3.7: the length byte counts the four bytes before the body, so a body
/// may have up to 251 bytes. INSRTS gives ERLINELONG when the length
/// reaches 256.
fn too_long(body: &[u8], text_line: usize, rejections: &mut Vec<Diagnostic>) {
    if body.len() + 4 >= 256 {
        rejections.push(Diagnostic { text_line: Some(text_line), message: "Line too long".into() });
    }
}

/// RENUM1 with start 10 and step 1 (§2.3.8).
fn renumber_program(store: &mut [Stored], warnings: &mut Vec<Diagnostic>, rejections: &mut Vec<Diagnostic>) {
    // NUMBC: a new number of 65280 or more fails, and RENUM puts the old
    // numbers back. This happens only with more than 65270 lines.
    if store.len() > 65279 - 10 + 1 {
        rejections.push(Diagnostic {
            text_line: None,
            message: "Line numbers larger than 65279 would be generated by this renumber".into(),
        });
        return;
    }
    // NUMBA and NUMBH: a reference goes to the first line, in program
    // order, whose old number matches it.
    let mut new_of_old: HashMap<u16, u16> = HashMap::new();
    for (i, l) in store.iter_mut().enumerate() {
        let new = 10 + i as u16;
        new_of_old.entry(l.number).or_insert(new);
        l.number = new;
    }
    // NUMBF: update every &8D outside a string literal and a REM or DATA
    // remainder, in the tokenised form as it stands. It does not skip *
    // commands.
    for l in store.iter_mut() {
        let body = &mut l.body;
        let mut quoted = false;
        let mut remainder = false;
        let mut i = 0;
        while i < body.len() {
            let c = body[i];
            i += 1;
            if c == b'"' {
                quoted = !quoted;
            }
            if quoted || remainder {
                continue;
            }
            if c == TREM || c == TDATA {
                remainder = true;
            } else if c == TCONST {
                // A raw &8D with fewer than three bytes after it would make
                // the interpreter read the next line's header. The result
                // is Unspecified, so the &8D is left alone.
                if i + 3 > body.len() {
                    break;
                }
                let r = line_constant([body[i], body[i + 1], body[i + 2]]);
                match new_of_old.get(&r) {
                    Some(&new) => body[i..i + 3].copy_from_slice(&consti(new)),
                    None => warnings.push(Diagnostic {
                        text_line: Some(l.text_line),
                        message: format!("Failed with {r} on line {}", l.number),
                    }),
                }
                i += 3;
            }
        }
    }
}

/// CONSTI. Encodes a line number as the three bytes that follow TCONST
/// (§2.2.5). This is the inverse of [`line_constant`].
pub fn consti(n: u16) -> [u8; 3] {
    let (lo, hi) = (n & 0xFF, n >> 8);
    [((lo & 0xC0) >> 2 | (hi & 0xC0) >> 4) as u8 ^ 0x54, (lo & 0x3F) as u8 | 0x40, (hi & 0x3F) as u8 | 0x40]
}

/// Tokenises one line of text as a program line. This is MATCH, starting at
/// the start of a statement with line-number conversion on (§2.4.3). The
/// text must not contain the line's end (a linefeed or carriage return).
/// The result is the tokenised line without the end.
pub fn tokenise_line(text: &[u8]) -> Vec<u8> {
    crunch(text).0
}

/// WORDCQ. True for a character that can be part of a name (§2.5.1).
fn wordc(c: u8) -> bool {
    c.is_ascii_digit() || c.is_ascii_uppercase() || (b'_'..=b'z').contains(&c)
}

#[derive(Clone, Copy, PartialEq, Eq)]
enum Mode {
    /// The left mode of MATCH: at the start of a statement.
    Statement,
    /// Right mode: in an expression.
    Expression,
}

/// Where MATCH has got to. Each value is one of its labels.
#[derive(Clone, Copy)]
enum At {
    /// %99: copy the next byte, assuming it is not a token.
    Copy,
    /// %00: look at the byte just copied.
    Examine,
    /// %10: the byte, which is known not to be in a string and not to be
    /// copied verbatim.
    Punctuation,
    /// MATCHZ: the rest of a number, copied.
    Number,
    /// %30: a byte that may begin a keyword.
    Word,
    /// MATCHH: the rest of a name, copied.
    Name,
}

/// MATCH (s/Lexical). Returns one line tokenised. Also returns whether a
/// control character stood outside a string literal and outside the
/// verbatim remainder of a line (REM, DATA, a * command). §2.3.14 rejects
/// such a line.
fn crunch(line: &[u8]) -> (Vec<u8>, bool) {
    // MATCH reads up to the carriage return that ends its line, and never
    // past it. Every loop that reads ahead stops at a byte that is not a
    // letter, a digit or a '.'.
    let mut src = Vec::with_capacity(line.len() + 1);
    src.extend_from_slice(line);
    src.push(13);
    let mut out: Vec<u8> = Vec::with_capacity(line.len() + 8);
    let mut i = 0;
    let mut c = 0u8;
    let mut mode = Mode::Statement;
    let mut numbers = true; // CONSTA: line-number conversion (§2.4.9)
    let mut quoted = false; // SMODE bit 0
    let mut verbatim = false; // SMODE bit 2: REM, DATA, a * command
    let mut control = false;
    let mut at = At::Copy;
    loop {
        match at {
            At::Copy => {
                c = src[i];
                i += 1;
                out.push(c);
                at = At::Examine;
            }
            At::Examine => {
                if c == b' ' {
                    at = At::Copy;
                    continue;
                }
                if c == 13 {
                    out.pop();
                    return (out, control);
                }
                // A quotation mark flips the string state even when the
                // rest of the line is verbatim. There it makes no
                // difference.
                if c == b'"' {
                    quoted = !quoted;
                }
                if quoted || verbatim {
                    at = At::Copy;
                    continue;
                }
                if c < 32 {
                    control = true;
                }
                if c == b'&' {
                    // Hexadecimal digits are copied before any keyword is
                    // looked for (§2.4.10). The mode does not change.
                    loop {
                        c = src[i];
                        i += 1;
                        out.push(c);
                        if !c.is_ascii_hexdigit() {
                            break;
                        }
                    }
                    at = if c < b'A' { At::Examine } else { At::Punctuation };
                    continue;
                }
                at = At::Punctuation;
            }
            At::Punctuation => {
                match c {
                    b':' => {
                        numbers = false;
                        mode = Mode::Statement;
                        at = At::Copy;
                    }
                    b',' => at = At::Copy,
                    b'*' => {
                        // A * at the start of a statement is a * command.
                        // Anywhere else it is a multiplication.
                        if mode == Mode::Statement {
                            verbatim = true;
                        } else {
                            numbers = false;
                            mode = Mode::Expression;
                        }
                        at = At::Copy;
                    }
                    b'.' => at = At::Number,
                    b'0'..=b'9' if !numbers => at = At::Number,
                    b'0'..=b'9' => {
                        // A line number. It is the value of the digits, if
                        // that stays below 65280 (§2.4.3 rule 8).
                        let mut n = u32::from(c & 15);
                        let mut j = i;
                        let mut d;
                        loop {
                            d = src[j];
                            j += 1;
                            if !d.is_ascii_digit() {
                                break;
                            }
                            n = n * 10 + u32::from(d & 15);
                            if n >= 65280 {
                                break;
                            }
                        }
                        if d.is_ascii_digit() {
                            // Too large, so this is an ordinary number. It
                            // is copied from the digit after the first.
                            at = At::Number;
                        } else {
                            // The reference replaces the digits. The byte
                            // after them is copied and examined. The mode
                            // and conversion stay as they were.
                            out.pop();
                            out.push(TCONST);
                            out.extend_from_slice(&consti(n as u16));
                            out.push(d);
                            i = j;
                            c = d;
                            at = At::Examine;
                        }
                    }
                    _ => at = At::Word,
                }
            }
            At::Number => {
                loop {
                    c = src[i];
                    i += 1;
                    out.push(c);
                    if !(c.is_ascii_digit() || c == b'.') {
                        break;
                    }
                }
                numbers = false;
                mode = Mode::Expression;
                at = At::Examine;
            }
            At::Name => {
                loop {
                    c = src[i];
                    i += 1;
                    out.push(c);
                    if !wordc(c) {
                        break;
                    }
                }
                numbers = false;
                mode = Mode::Expression;
                at = At::Examine;
            }
            At::Word => {
                if c < b'A' || (c > b'W' && !wordc(c)) {
                    // Punctuation, a control character, or a byte from &7F
                    // upwards. It is copied as it stands (§2.3.13).
                    numbers = false;
                    mode = Mode::Expression;
                    at = At::Copy;
                    continue;
                }
                if c > b'W' {
                    at = At::Name;
                    continue;
                }
                let Some((k, j)) = keyword(&src, i, c) else {
                    at = At::Name;
                    continue;
                };
                let kw = &KEYWORDS[k];
                let mut job = kw.job;
                let mut token = kw.token;
                // TRACE on the right of an expression takes no line
                // number.
                if mode == Mode::Expression && token == TTRACE {
                    job &= !16;
                }
                if mode == Mode::Expression
                    && token == TPRINT
                    && src[j - 1] == b'.'
                    && src[j - 2] == b'P'
                    && j >= 3
                    && src[j - 3] == b'U'
                    && out.len() >= 2
                    && out[out.len() - 2] == TVDU
                {
                    // §2.4.12: VDU in full followed by P. is the VFP
                    // assembler's VDUP. The P stays a character, followed by
                    // '.', and the tokeniser returns to the start of a
                    // statement.
                    out.push(b'.');
                    token = b'.';
                    job = 4;
                } else {
                    // §2.4.5: a conditional keyword followed by a name
                    // character is the start of a name.
                    if job & 1 != 0 && wordc(src[j]) {
                        at = At::Name;
                        continue;
                    }
                    let last = out.len() - 1;
                    if job & 8 != 0 {
                        // A two-byte token (§2.2.4).
                        out[last] = if job & 4 != 0 {
                            job &= !4;
                            TESCFN
                        } else if job & 64 != 0 {
                            TESCSTMT
                        } else {
                            TESCCOM
                        };
                        out.push(token);
                    } else {
                        // At the start of a statement, a pseudo-variable
                        // takes its statement form (§2.4.11).
                        if job & 64 != 0 && mode == Mode::Statement {
                            token += TPTR2 - TPTR;
                        }
                        out[last] = token;
                    }
                }
                if job & 2 != 0 {
                    mode = Mode::Expression;
                    numbers = false;
                }
                if job & 4 != 0 {
                    mode = Mode::Statement;
                    numbers = false;
                }
                i = j;
                if token == TFN || token == TPROC {
                    // Copy the routine's name as it stands (§2.4.8).
                    while wordc(src[i]) {
                        out.push(src[i]);
                        i += 1;
                    }
                }
                if job & 16 != 0 {
                    numbers = true;
                }
                if job & 32 != 0 {
                    verbatim = true;
                }
                at = At::Copy;
            }
        }
    }
}

/// The search of the keyword table made by MATCH (§2.4.4). The entries that
/// begin with `c` are tried in table order against the text from `i`, which
/// is just after `c`. Returns the entry and where the text after it begins.
/// The search ends at WIDTH.
fn keyword(src: &[u8], i: usize, c: u8) -> Option<(usize, usize)> {
    let first = KEYWORDS.iter().position(|k| k.word.as_bytes()[0] == c)?;
    for (k, kw) in KEYWORDS.iter().enumerate().skip(first) {
        let word = kw.word.as_bytes();
        if word[0] != c {
            return None;
        }
        let mut j = i;
        let mut matched = true;
        for &w in &word[1..] {
            let s = src[j];
            j += 1;
            if s == w {
                continue;
            }
            if s != b'.' {
                matched = false;
            }
            // '.' abbreviates the rest of the keyword, and is part of the
            // match.
            break;
        }
        if matched {
            return Some((k, j));
        }
        if kw.token == TWIDTH {
            return None;
        }
    }
    None
}

/// The job byte of a one-byte token, for [`control_outside`].
fn job_of(token: u8) -> u8 {
    match token {
        TELSE2 => 0x14,
        t if (TPTR2..TPTR2 + 5).contains(&t) => 0x43,
        t => KEYWORDS.iter().find(|k| k.job & 8 == 0 && k.token == t).map_or(2, |k| k.job),
    }
}

/// The job byte of a two-byte token.
fn job_of_two(escape: u8, token: u8) -> u8 {
    KEYWORDS
        .iter()
        .find(|k| {
            k.job & 8 != 0
                && k.token == token
                && match escape {
                    TESCFN => k.job & 4 != 0,
                    TESCSTMT => k.job & 64 != 0,
                    _ => k.job & (4 | 64) == 0,
                }
        })
        .map_or(2, |k| k.job)
}

/// Applies §2.3.14 to a tokenised line. Returns whether a control character
/// stands outside a string literal, a REM or DATA remainder and a * command.
/// A text program is checked while it is tokenised. A tokenised program has
/// only its tokens, so this function follows the tokeniser's modes over
/// them. A statement starts at the start of the line, after ':', and after
/// a keyword that starts one (THEN, ELSE, LET ...). A keyword with neither
/// mode flag keeps the current mode.
fn control_outside(body: &[u8]) -> bool {
    let mut statement = true;
    let mut quoted = false;
    let mut i = 0;
    while i < body.len() {
        let c = body[i];
        i += 1;
        if quoted {
            if c == b'"' {
                quoted = false;
                statement = false;
            }
            continue;
        }
        let job = match c {
            b'"' => {
                quoted = true;
                continue;
            }
            b' ' | b',' => continue,
            b':' => {
                statement = true;
                continue;
            }
            b'*' if statement => return false,
            0..=31 => return true,
            b'&' => {
                while i < body.len() && body[i].is_ascii_hexdigit() {
                    i += 1;
                }
                continue;
            }
            TCONST => {
                i += 3;
                continue;
            }
            TESCFN | TESCCOM | TESCSTMT if i < body.len() => {
                i += 1;
                job_of_two(c, body[i - 1])
            }
            0x7F..=0xFF => job_of(c),
            _ => 2,
        };
        if job & 32 != 0 {
            return false;
        }
        if job & 2 != 0 {
            statement = false;
        }
        if job & 4 != 0 {
            statement = true;
        }
    }
    false
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::program::list_text;

    fn tok(text: &str) -> Vec<u8> {
        tokenise_line(text.as_bytes())
    }

    fn reference(n: u16) -> Vec<u8> {
        let mut v = vec![TCONST];
        v.extend_from_slice(&consti(n));
        v
    }

    fn cat(parts: &[&[u8]]) -> Vec<u8> {
        parts.concat()
    }

    /// The pair (number, listed text) for each line.
    fn listing(text: &str) -> Vec<(u16, String)> {
        let t = textload(text.as_bytes()).unwrap_or_else(|e| panic!("rejected: {e:?}"));
        t.program.lines.iter().map(|l| (l.number, String::from_utf8_lossy(&list_text(&l.text)).into_owned())).collect()
    }

    fn numbers(text: &str) -> Vec<u16> {
        listing(text).into_iter().map(|(n, _)| n).collect()
    }

    fn messages(text: &[u8]) -> Vec<String> {
        textload(text).err().unwrap_or_default().into_iter().map(|d| d.message).collect()
    }

    #[test]
    fn consti_is_basics_encoding() {
        // §2.2.5. This is the reverse of the decoder in program.rs.
        for n in [0u16, 1, 9, 10, 63, 64, 192, 255, 256, 1000, 32767, 49152, 65279] {
            assert_eq!(line_constant(consti(n)), n);
            assert!(consti(n).iter().all(|b| (0x40..0x80).contains(b)));
        }
        assert_eq!(consti(60), [0x54, 0x7C, 0x40]);
    }

    #[test]
    fn keywords_and_names() {
        assert_eq!(tok("PRINT \"A\""), cat(&[&[TPRINT], b" \"A\""]));
        // §2.4.1: keywords are upper case
        assert_eq!(tok("print"), b"print".to_vec());
        // §2.4.6: a name that begins with a keyword is split
        assert_eq!(tok("PRINTER"), cat(&[&[TPRINT], b"ER"]));
        assert_eq!(tok("TOTAL=5"), cat(&[&[TTO], b"TAL=5"]));
        assert_eq!(tok("TABLE"), b"TABLE".to_vec());
        // §2.4.7: a name swallows a keyword that follows it
        assert_eq!(tok("XPRINT"), b"XPRINT".to_vec());
        assert_eq!(tok("IFX=YTHENPRINT\"x\""), cat(&[&[TIF], b"X=YTHENPRINT\"x\""]));
        assert_eq!(tok("FORI=1TO3STEP2"), cat(&[&[TFOR], b"I=1", &[TTO], b"3", &[TSTEP], b"2"]));
        assert_eq!(tok("IFA%THENPRINT"), cat(&[&[TIF], b"A%", &[TTHEN, TPRINT]]));
    }

    #[test]
    fn conditional_keywords() {
        // §2.4.5
        for name in ["ENDING", "TIMER", "PIE", "COUNTER", "ERRNO", "STOPPED"] {
            assert_eq!(tok(name), name.as_bytes().to_vec(), "{name}");
        }
        assert_eq!(tok("COUNT%=1"), cat(&[&[TCOUNT], b"%=1"]));
        assert_eq!(tok("END"), vec![TEND]);
    }

    #[test]
    fn abbreviations() {
        // §2.4.4
        assert_eq!(tok("P.1"), cat(&[&[TPRINT], b"1"]));
        assert_eq!(tok("A=T.0"), cat(&[b"A=", &[TTAN], b"0"]));
        assert_eq!(tok("A=TI."), cat(&[b"A=", &[TTIME]]));
        assert_eq!(tok("END."), vec![TENDPR]);
        assert_eq!(tok("A$=INKEY.0"), cat(&[b"A$=", &[TINKED], b"0"]));
        assert_eq!(tok("OR."), cat(&[&[TESCSTMT, TORGIN]]));
        assert_eq!(tok("P.LE.\"abcd\",2)"), cat(&[&[TPRINT, TLEFTD], b"\"abcd\",2)"]));
    }

    #[test]
    fn line_number_references() {
        // §2.4.9
        assert_eq!(tok("GOTO 10"), cat(&[&[TGOTO], b" ", &reference(10)]));
        assert_eq!(
            tok("ON x GOTO 10,20,30"),
            cat(&[&[TON], b" x ", &[TGOTO], b" ", &reference(10), b",", &reference(20), b",", &reference(30)])
        );
        assert_eq!(tok("GOTO (400)"), cat(&[&[TGOTO], b" (400)"]));
        assert_eq!(tok("GOTO 40+20"), cat(&[&[TGOTO], b" ", &reference(40), b"+20"]));
        assert_eq!(tok("GOTO 70000"), cat(&[&[TGOTO], b" 70000"]));
        assert_eq!(tok("GOTO 65279"), cat(&[&[TGOTO], b" ", &reference(65279)]));
        assert_eq!(tok("RESTORE +1"), cat(&[&[TRESTORE], b" +1"]));
        assert_eq!(
            tok("IF A THEN 20 ELSE 30"),
            cat(&[&[TIF], b" A ", &[TTHEN], b" ", &reference(20), b" ", &[TELSE], b" ", &reference(30)])
        );
        // TRACE takes a line number at the start of a statement. In an
        // expression it does not.
        assert_eq!(tok("TRACE 100"), cat(&[&[TTRACE], b" ", &reference(100)]));
        assert_eq!(tok("PRINT TRACE,961"), cat(&[&[TPRINT], b" ", &[TTRACE], b",961"]));
        // A keyword with neither mode flag leaves conversion on.
        assert_eq!(tok("GOTO ABS 10"), cat(&[&[TGOTO], b" ", &[TABS], b" ", &reference(10)]));
        // A number at the start of any line is a line number
        assert_eq!(tok("  007 X"), cat(&[b"  ", &reference(7), b" X"]));
        assert_eq!(tok("65280 X"), b"65280 X".to_vec());
    }

    #[test]
    fn pseudo_variables() {
        // §2.4.11
        assert_eq!(tok("TIME=TIME"), cat(&[&[TTIME2], b"=", &[TTIME]]));
        assert_eq!(
            tok("IF TRUE THEN TIME=TIME"),
            cat(&[&[TIF], b" ", &[TTRUE], b" ", &[TTHEN], b" ", &[TTIME2], b"=", &[TTIME]])
        );
        assert_eq!(tok("REPEAT TIME=TIME"), cat(&[&[TREPEAT], b" ", &[TTIME2], b"=", &[TTIME]]));
        assert_eq!(tok("ERROR TIME,\"x\""), cat(&[&[TERROR], b" ", &[TTIME2], b",\"x\""]));
        assert_eq!(tok("A=1:PAGE=PAGE"), cat(&[b"A=1:", &[TPAGE2], b"=", &[TPAGE]]));
    }

    #[test]
    fn verbatim_text() {
        // §2.4.10
        assert_eq!(tok("REM PRINT: GOTO 10"), cat(&[&[TREM], b" PRINT: GOTO 10"]));
        assert_eq!(tok("REMARK"), cat(&[&[TREM], b"ARK"]));
        assert_eq!(tok("DATA PI, 10"), cat(&[&[TDATA], b" PI, 10"]));
        assert_eq!(tok("*CAT PRINT"), b"*CAT PRINT".to_vec());
        assert_eq!(tok("IF A THEN *CAT"), cat(&[&[TIF], b" A ", &[TTHEN], b" *CAT"]));
        assert_eq!(tok("A=2*COS 0"), cat(&[b"A=2*", &[TCOS], b" 0"]));
        assert_eq!(tok("PRINT \"GOTO 10\""), cat(&[&[TPRINT], b" \"GOTO 10\""]));
        // Hex digits are copied before keywords are looked for
        assert_eq!(tok("A=&FAND 3"), b"A=&FAND 3".to_vec());
        assert_eq!(tok("A=&FEOR 1"), cat(&[b"A=&FE", &[TOR], b" 1"]));
    }

    #[test]
    fn procedure_names() {
        // §2.4.8, and §2.4.6's DEF PROCESS
        assert_eq!(tok("PROCEND"), cat(&[&[TPROC], b"END"]));
        assert_eq!(tok("A=FNTIME"), cat(&[b"A=", &[TFN], b"TIME"]));
        assert_eq!(tok("DEF PROCESS"), cat(&[&[TDEF], b" ", &[TPROC], b"ESS"]));
    }

    #[test]
    fn two_byte_tokens() {
        assert_eq!(tok("CASE A OF"), cat(&[&[TESCSTMT, TCASE], b" A ", &[TOF]]));
        assert_eq!(tok("A=SUM(a())"), cat(&[b"A=", &[TESCFN, TSUM], b"(a())"]));
        assert_eq!(tok("INSTALL \"x\""), cat(&[&[TESCCOM, TINSTALL], b" \"x\""]));
        assert_eq!(tok("SWAP a,b"), cat(&[&[TESCSTMT, TSWAP], b" a,b"]));
    }

    #[test]
    fn vdu_p() {
        // §2.4.12
        assert_eq!(tok("VDUP."), cat(&[&[TVDU], b"P."]));
        assert_eq!(tok("VDU P."), cat(&[&[TVDU], b" ", &[TPRINT]]));
        assert_eq!(tok("V.P."), cat(&[&[TVDU, TPRINT]]));
        assert_eq!(tok("VDUP.:VDUPR."), cat(&[&[TVDU], b"P.:", &[TVDU, TPRINT]]));
        // Back at the start of a statement, a pseudo-variable takes its
        // statement form.
        assert_eq!(tok("VDUP.TIME"), cat(&[&[TVDU], b"P.", &[TTIME2]]));
    }

    #[test]
    fn numbered_programs() {
        // §2.3.4: lines are sorted. A later line replaces an earlier one. A
        // bare number deletes a line. A number and a space gives an empty
        // line.
        assert_eq!(numbers("30 A\n10 B\n20 C\n"), [10, 20, 30]);
        assert_eq!(listing("20 A\n30 B\n20 C\n"), [(20, " C".into()), (30, " B".into())]);
        assert_eq!(numbers("10 A\n20 B\n30 C\n20\n"), [10, 30]);
        assert_eq!(listing("10 A\n20 B\n20 \n"), [(10, " A".into()), (20, " ".into())]);
        // §2.3.3: spaces before and after the number.
        assert_eq!(listing("   10 A\n  20B\n30    C\n"), [(10, " A".into()), (20, "B".into()), (30, "    C".into())]);
        // §2.3.11: leading zeros are allowed, and numbers run from 0 to
        // 65279.
        assert_eq!(numbers("0 A\n0000000000000005 B\n65279 C\n"), [0, 5, 65279]);
        let t = textload(b"10 A\n20 B\n").unwrap();
        assert!(!t.renumbered);
        assert_eq!(t.text_lines, [1, 2]);
    }

    #[test]
    fn unnumbered_programs() {
        // §2.3.8: the kth line of the text is numbered 9+k. Blank lines
        // count.
        let t = textload(b"A\n\n   \nB\n").unwrap();
        assert!(t.renumbered);
        assert_eq!(t.program.lines.iter().map(|l| l.number).collect::<Vec<_>>(), [10, 11, 12, 13]);
        assert_eq!(t.program.lines[1].text, b"");
        assert_eq!(t.program.lines[2].text, b" ");
        // §2.3.1, §2.3.5: CR LF ends a line and then an empty line. A blank
        // line makes a numbered program be renumbered.
        assert_eq!(numbers("100 A\r\n200 B\r\n"), [10, 11, 12, 13]);
        assert_eq!(numbers("100 A\n\n300 B\n"), [10, 11, 12]);
        assert_eq!(numbers("100 A\r200 B\r"), [100, 200]);
        // 65280 is not a line number (§2.3.11)
        assert_eq!(listing("A\n65280 B\n"), [(10, "A".into()), (11, "65280 B".into())]);
    }

    #[test]
    fn mixed_programs() {
        // §2.3.4: an unnumbered line goes at the end with the current
        // number. A numbered line goes before the first line whose number
        // is at least its own (text-mixed-order).
        let l = listing("REM\nPRINT \"a\"\nPRINT \"b\"\n20 PRINT \"c\"\nPRINT \"d\"\n5 PRINT \"e\"\nPRINT \"f\"\n");
        let texts: Vec<&str> = l.iter().map(|(_, t)| t.as_str()).collect();
        assert_eq!(
            texts,
            [" PRINT \"e\"", "REM", "PRINT \"a\"", "PRINT \"b\"", " PRINT \"c\"", "PRINT \"d\"", "PRINT \"f\""]
        );
        assert_eq!(l.iter().map(|(n, _)| *n).collect::<Vec<_>>(), [10, 11, 12, 13, 14, 15, 16]);
        // Lines before the first numbered line carry the number 9, so a
        // line 9 removes them all (text-mixed-nine).
        let l = listing("REM\nPRINT \"a\"\n9 PRINT \"b\"\nPRINT \"c\"\n");
        assert_eq!(l, [(10, " PRINT \"b\"".into()), (11, "PRINT \"c\"".into())]);
        // REMOVE runs from the first line numbered n or more to the first
        // line after it numbered n+1 or more, whatever lies between.
        let l = listing("A\n5 B\nC\n9 D\n");
        assert_eq!(l, [(10, " B".into()), (11, " D".into())]);
    }

    #[test]
    fn references_follow_renumbering() {
        // §2.3.8 (text-mixed-refs, text-goto-nine, text-failed-ref)
        let t =
            textload(b"GOTO 200\n100 A\n200 B:GOTO 9\nGOTO (200)\nREM GOTO 100\nPRINT \"GOTO 100\":GOTO 77\n").unwrap();
        let l: Vec<String> =
            t.program.lines.iter().map(|l| String::from_utf8_lossy(&list_text(&l.text)).into_owned()).collect();
        assert_eq!(l, ["GOTO 12", " A", " B:GOTO 10", "GOTO (200)", "REM GOTO 100", "PRINT \"GOTO 100\":GOTO 77"]);
        assert_eq!(t.warnings, [Diagnostic { text_line: Some(6), message: "Failed with 77 on line 15".into() }]);
        // A reference goes to the first line with that old number.
        let t = textload(b"A\nB\nGOTO 9\n").unwrap();
        assert_eq!(list_text(&t.program.lines[2].text), b"GOTO 10");
    }

    #[test]
    fn stored_lines() {
        // §2.3.6: trailing spaces are removed. ELSE at the start of a line
        // becomes &CC.
        let t = textload(b"DATA  x  ,  y   \n  ELSE PRINT\nA ELSE B\n").unwrap();
        assert_eq!(t.program.lines[0].text, cat(&[&[TDATA], b"  x  ,  y"]));
        assert_eq!(t.program.lines[1].text, cat(&[b"  ", &[TELSE2], b" ", &[TPRINT]]));
        assert_eq!(t.program.lines[2].text, cat(&[b"A ", &[TELSE], b" B"]));
    }

    #[test]
    fn line_length() {
        // §2.3.7: a tokenised body of 251 bytes fits, and one of 252 does
        // not. The limit applies to the tokenised body after trailing
        // spaces are removed.
        let ok = format!("REM{}\n", "x".repeat(250));
        assert!(textload(ok.as_bytes()).is_ok());
        let long = format!("A\nREM{}\nB\n", "x".repeat(251));
        let e = textload(long.as_bytes()).unwrap_err();
        assert_eq!(e, [Diagnostic { text_line: Some(2), message: "Line too long".into() }]);
        let shrinks = format!("A={}\n", "COS(0)+".repeat(49) + "COS(0)");
        assert!(shrinks.len() > 256 && textload(shrinks.as_bytes()).is_ok());
        let spaces = format!("PRINT{}\n", " ".repeat(300));
        assert!(textload(spaces.as_bytes()).is_ok());
    }

    #[test]
    fn missing_line_end() {
        // §2.3.2
        assert_eq!(messages(b"PRINT"), ["Missing line end at end of file"]);
        assert_eq!(messages(b""), ["Missing line end at end of file"]);
        assert_eq!(messages(b"PRINT\r"), Vec::<String>::new());
    }

    #[test]
    fn control_characters() {
        // §2.3.14, with the line numbers that loading gives.
        assert_eq!(messages(b"REM\n\tPRINT\n"), ["Control character in line 11"]);
        assert_eq!(messages(b"PRINT\t1\n"), ["Control character in line 10"]);
        assert_eq!(messages(b"A=1\x01\n"), ["Control character in line 10"]);
        for fine in [&b"A$=\"a\tb\"\n"[..], b"REM\tx\n", b"DATA a\tb\n", b"*ECHO \t\n", b"A=1:REM \x01\n"] {
            assert!(textload(fine).is_ok(), "{fine:?}");
        }
        // A line that a later one replaces is not checked.
        assert!(textload(b"10 \tX\n10 Y\n").is_ok());
    }

    #[test]
    fn tokenised_checks() {
        // §2.2.8 and §2.3.14 applied to a tokenised program.
        let line = |number, text: &[u8]| Line { number, text: text.to_vec() };
        let p = Program { lines: vec![line(10, b"A"), line(30, b"B"), line(20, b"C"), line(20, b"D")] };
        let m: Vec<String> = check_tokenised(&p).into_iter().map(|d| d.message).collect();
        assert_eq!(m, ["Line numbers out of order at line 20", "Line numbers out of order at line 20"]);
        let p = Program {
            lines: vec![
                line(10, b"A=1\t"),
                line(20, &[TREM, 9]),
                line(30, b"*\t"),
                line(40, &[TTHEN, b'*', 9]),
                line(50, b"\"\t\""),
                line(60, &[TESCSTMT, TSWAP, 9]),
            ],
        };
        let m: Vec<String> = check_tokenised(&p).into_iter().map(|d| d.message).collect();
        assert_eq!(m, ["Control character in line 10", "Control character in line 60"]);
    }

    #[test]
    fn loads_either_form() {
        let t = load(b"PRINT\n").unwrap();
        assert!(!t.tokenised);
        let t = load(&t.program.to_tokenised()).unwrap();
        assert!(t.tokenised);
        assert_eq!(t.program.lines[0].number, 10);
    }
}
