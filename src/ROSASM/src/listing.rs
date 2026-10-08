//! Emit an assembler listing in ObjAsm's format.
//!
//! The listing is what we compare against ObjAsm's. For each line it gives
//! the source line number, the address, the bytes generated and the
//! *expanded* source text. Matching it proves expansion, layout and encoding
//! all at once. It could also be compared before there was an encoder.
//!
//! ObjAsm 4.08 lays a line out as:
//!
//! ```text
//!    NN AAAAAAAA BBBBBBBB <text>
//!    ^^ ^^^^^^^^ ^^^^^^^^
//!     |     |        `-- bytes: one word as 8 hex digits, or bytes separated
//!     |     |            by spaces for DCB. Further words go on lines of
//!     |     |            their own, with the number and address blank.
//!     |     `-- address within the AREA
//!     `-- source line number, width 5, right aligned
//! ```
//!
//! A line that generates nothing still appears, with the byte column blank.

use crate::expand::ExpandedLine;

/// Width of the line-number column, not counting the space after it.
const NUM_W: usize = 5;

/// Render one expanded line, which may occupy several listing lines when it
/// generates more than one word.
pub fn line(out: &mut String, l: &ExpandedLine) {
    let n = l.origin.line;
    // DCB-style output is bytes separated by spaces. Everything else is
    // words.
    let byte_wise = !l.bytes.len().is_multiple_of(4) || is_byte_directive(&l.text);

    if l.bytes.is_empty() {
        out.push_str(&format!("{:>w$} {:08X}          {}\n", n, l.addr, l.text, w = NUM_W));
        return;
    }

    if byte_wise {
        // `68 69 00`: up to three bytes to a row, as ObjAsm does.
        let groups: Vec<String> = l
            .bytes
            .chunks(3)
            .map(|c| c.iter().map(|b| format!("{b:02X}")).collect::<Vec<_>>().join(" "))
            .collect();
        for (i, g) in groups.iter().enumerate() {
            // As with words, the number and address go on the first row and
            // the source text on the last.
            let (num, addr) = if i == 0 {
                (format!("{n:>nw$}", nw = NUM_W), format!("{:08X}", l.addr))
            } else {
                (" ".repeat(NUM_W), " ".repeat(8))
            };
            let text = if i + 1 == groups.len() { l.text.as_str() } else { "" };
            out.push_str(&format!("{num} {addr} {g:<8} {text}\n"));
        }
        return;
    }

    let words: Vec<u32> = l
        .bytes
        .chunks(4)
        .map(|c| u32::from_le_bytes([c[0], c[1], c[2], c[3]]))
        .collect();
    for (i, w) in words.iter().enumerate() {
        // The first word carries the line number and address. ObjAsm leaves
        // both blank on the words that follow. The source text goes on the
        // last.
        let (num, addr) = if i == 0 {
            (format!("{n:>nw$}", nw = NUM_W), format!("{:08X}", l.addr))
        } else {
            (" ".repeat(NUM_W), " ".repeat(8))
        };
        let text = if i + 1 == words.len() { l.text.as_str() } else { "" };
        out.push_str(&format!("{num} {addr} {w:08X} {text}\n"));
    }
}

fn is_byte_directive(text: &str) -> bool {
    let up = text.to_ascii_uppercase();
    up.split_whitespace().any(|t| t == "DCB" || t == "=")
}

pub fn render(lines: &[ExpandedLine]) -> String {
    let mut s = String::new();
    for l in lines {
        line(&mut s, l);
    }
    s
}

/// Reduce a listing to what is worth comparing.
///
/// ObjAsm's own listing carries page headers, form feeds and trailing
/// spaces that say nothing about the assembly. Both sides go through this
/// before they are compared, so that a page break does not show as a
/// difference.
pub fn normalise(text: &str) -> Vec<String> {
    let mut out: Vec<String> = Vec::new();
    for raw in text.lines() {
        let l = raw.trim_end();
        if l.is_empty() {
            continue;
        }
        if l.contains("ARM Macro Assembler") && l.contains("Page") {
            continue;
        }
        if l.starts_with('\u{c}') {
            continue;
        }
        // ObjAsm wraps long source text onto the next line. Where the text
        // carries on depends on the line: a comment carries on at the text
        // column, a data directive at column 0. Neither has a line number or
        // an address. So any line that does not start a row, and is not a
        // byte continuation, belongs to the line above.
        if !out.is_empty() && !starts_row(l) && !is_byte_continuation(l) {
            out.last_mut().unwrap().push_str(l.trim_start());
            continue;
        }
        out.push(l.to_string());
    }
    out
}

/// `   12 0000002C ...` - a line number and an address begin a row.
fn starts_row(l: &str) -> bool {
    let c: Vec<char> = l.chars().collect();
    let num: String = c.iter().take(5).collect();
    if num.trim().is_empty() || !num.trim().chars().all(|d| d.is_ascii_digit()) {
        return false;
    }
    let addr: String = c.iter().skip(6).take(8).collect();
    addr.len() == 8 && addr.chars().all(|d| d.is_ascii_hexdigit())
}

/// `               00 00 01` - more words or bytes of the same directive.
/// They sit in the byte column, with the number and address blank.
fn is_byte_continuation(l: &str) -> bool {
    let c: Vec<char> = l.chars().collect();
    if c.len() < 16 || !c[..15].iter().all(|ch| *ch == ' ') {
        return false;
    }
    c[15].is_ascii_hexdigit()
}

/// A listing line split into its fields, so a diff can say *which* differs.
#[derive(Debug, Clone, PartialEq)]
pub struct Row {
    pub num: Option<u32>,
    pub addr: Option<u32>,
    pub bytes: String,
    pub text: String,
}

/// Parse a listing line back into its columns.
///
/// This goes by column position, not by spaces, because the text column can
/// be blank and the byte groups contain spaces.
pub fn parse_row(l: &str) -> Row {
    let chars: Vec<char> = l.chars().collect();
    let take = |a: usize, b: usize| -> String {
        chars
            .get(a..b.min(chars.len()))
            .map(|c| c.iter().collect::<String>())
            .unwrap_or_default()
            .trim()
            .to_string()
    };
    let num = take(0, 5).parse::<u32>().ok();
    let addr = u32::from_str_radix(&take(6, 14), 16).ok();
    let bytes = take(15, 23);
    let text = chars
        .get(24..)
        .map(|c| c.iter().collect::<String>())
        .unwrap_or_default()
        .trim_end()
        .to_string();
    Row { num, addr, bytes, text }
}


#[cfg(test)]
mod tests {
    use super::*;
    use crate::expand::Origin;

    fn el(line: usize, addr: u32, bytes: Vec<u8>, text: &str) -> ExpandedLine {
        ExpandedLine {
            operands: String::new(),
            text: text.to_string(),
            origin: Origin { file: "t".into(), line, macros: vec![] },
            addr,
            bytes,
            listing_only: false,
            area_index: 0,
            rout: None,
            literal: None,
        }
    }

    #[test]
    fn a_line_with_no_bytes_keeps_its_columns() {
        let s = render(&[el(1, 0, vec![], "        AREA    Test, CODE")]);
        let r = parse_row(s.lines().next().unwrap());
        assert_eq!(r.num, Some(1));
        assert_eq!(r.addr, Some(0));
        assert_eq!(r.bytes, "");
        assert!(r.text.contains("AREA"));
    }

    #[test]
    fn one_word_lines_up_with_its_text() {
        let s = render(&[el(3, 0, vec![1, 0, 0xA0, 0xE3], "Start   MOV     r0, #1")]);
        let r = parse_row(s.lines().next().unwrap());
        assert_eq!(r.num, Some(3));
        assert_eq!(r.bytes, "E3A00001");
        assert!(r.text.starts_with("Start"));
    }

    #[test]
    fn several_words_continue_on_their_own_lines() {
        // `DCD 0, 1, &FF` lists three words with the text on the last.
        let bytes = vec![0, 0, 0, 0, 1, 0, 0, 0, 0xFF, 0, 0, 0];
        let s = render(&[el(7, 0x10, bytes, "Tbl     DCD     0, 1, &FF")]);
        let rows: Vec<Row> = s.lines().map(parse_row).collect();
        assert_eq!(rows.len(), 3);
        assert_eq!(rows[0].bytes, "00000000");
        assert_eq!(rows[1].bytes, "00000001");
        assert_eq!(rows[2].bytes, "000000FF");
        assert!(rows[2].text.contains("DCD"));
        assert!(rows[0].text.is_empty());
    }

    #[test]
    fn dcb_lists_bytes_not_words() {
        let s = render(&[el(8, 0x1C, vec![0x68, 0x69, 0x00], "Msg     DCB     \"hi\", 0")]);
        let r = parse_row(s.lines().next().unwrap());
        assert_eq!(r.bytes, "68 69 00");
    }

    #[test]
    fn normalise_drops_page_furniture() {
        let raw = "\n\nARM Macro Assembler    Page 1\n\n    1 00000000     AREA x\n";
        assert_eq!(normalise(raw), vec!["    1 00000000     AREA x"]);
    }

    #[test]
    fn rows_round_trip_through_the_parser() {
        let s = render(&[el(5, 8, vec![1, 0, 0x50, 0xE2], "Loop    SUBS    r0, r0, #1")]);
        let r = parse_row(s.lines().next().unwrap());
        assert_eq!(r.num, Some(5));
        assert_eq!(r.addr, Some(8));
        assert_eq!(r.bytes, "E2500001");
    }
}


#[cfg(test)]
mod wrap_tests {
    use super::*;

    #[test]
    fn a_comment_wrap_resuming_at_the_text_column_is_rejoined() {
        let raw = concat!(
            "    3 00000000          ; Licensed under the Apache License (the \"\n",
            "                        License\");\n",
        );
        let n = normalise(raw);
        assert_eq!(n.len(), 1);
        assert!(n[0].ends_with("License\");"));
    }

    #[test]
    fn a_data_wrap_resuming_at_column_zero_is_rejoined() {
        // A DCB wraps back to column 0, unlike a comment.
        let raw = concat!(
            "               4D 00 FF LEV1    =       &15, &10, \"SR\n",
            "AM\", &00, &ff\n",
        );
        let n = normalise(raw);
        assert_eq!(n.len(), 1, "the wrap must join, not start a row");
        assert!(n[0].ends_with("AM\", &00, &ff"));
    }

    #[test]
    fn a_byte_continuation_stays_its_own_row() {
        let raw = concat!(
            "    7 00000010 00000000 \n",
            "               00000001 \n",
            "               000000FF Tbl     DCD     0, 1, &FF\n",
        );
        assert_eq!(normalise(raw).len(), 3);
    }

    #[test]
    fn ordinary_rows_are_untouched() {
        let raw = "    1 00000000          AREA x\n    2 00000004          MOV r0, #1\n";
        assert_eq!(normalise(raw).len(), 2);
    }
}

#[cfg(test)]
mod byte_layout_tests {
    use super::*;
    use crate::expand::Origin;

    #[test]
    fn multi_byte_data_puts_the_text_on_the_last_row() {
        // ObjAsm's layout: the number and address on the first row, the
        // source text on the last, and byte groups on every row.
        let l = ExpandedLine {
            operands: String::new(),
            text: "DEV_ID  = &01, &03, &61, &00, &ff".into(),
            origin: Origin { file: "t".into(), line: 32, macros: vec![] },
            addr: 0,
            bytes: vec![1, 3, 0x61, 0, 0xFF],
            listing_only: false,
            area_index: 0,
            rout: None,
            literal: None,
        };
        let s = render(&[l]);
        let rows: Vec<Row> = s.lines().map(parse_row).collect();
        assert_eq!(rows.len(), 2);
        assert_eq!(rows[0].num, Some(32));
        assert!(rows[0].text.is_empty(), "text belongs on the last row");
        assert_eq!(rows[1].num, None);
        assert!(rows[1].text.starts_with("DEV_ID"));
    }
}
