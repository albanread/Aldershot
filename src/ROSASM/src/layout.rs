//! Areas, location counters and local-label scopes.
//!
//! From `docs/PRM/pdf/Asm.pdf` chapters 4 and 6:
//!
//! * `AREA name«,attr»...` starts a section. The defaults are `REL`,
//!   `READWRITE` and 4-byte alignment. `ALIGN=n` aligns the area to 2^n
//!   bytes, for n from 2 to 12.
//! * `MAP expr«,base-register»` (`^`) sets the storage-map counter `@`.
//!   `«symbol» FIELD expr` (`#`) gives the symbol the current value of `@`,
//!   then advances `@` by `expr`. Without a `MAP`, `@` starts at zero.
//! * A local label is a number, 0..=99 by the manual, optionally followed by
//!   the name of the routine it is in. `ROUT` opens a local-label area. The
//!   area ends at the next `ROUT` or the end of the program, and a search
//!   never crosses that boundary.

/// Attributes an `AREA` can carry.
#[derive(Debug, Clone, PartialEq, Default)]
pub struct AreaAttrs {
    pub code: bool,
    pub readonly: bool,
    pub noinit: bool,
    pub abs: bool,
    pub pic: bool,
    pub common: bool,
    pub comdef: bool,
    pub reentrant: bool,
    pub interwork: bool,
    /// Alignment of the area's start, as a power of two: the start is a
    /// multiple of 2^align bytes. The manual allows 2..=12, and the default
    /// is 2 (a word).
    pub align: u32,
    /// `BASED Rn`: labels in this area are relative to register Rn.
    pub based: Option<u32>,
}

impl AreaAttrs {
    fn new() -> Self {
        AreaAttrs {
            align: 2,
            ..Default::default()
        }
    }
}

#[derive(Debug, Clone)]
pub struct Area {
    pub name: String,
    pub attrs: AreaAttrs,
    /// Bytes emitted so far. This is the value of `.` within this area.
    pub offset: u32,
}

/// Parse an `AREA` operand list. An unknown attribute is reported, not
/// ignored, because dropping an attribute without a word changes what the
/// object means.
pub fn parse_area(operands: &str) -> Result<(String, AreaAttrs), String> {
    let mut parts = operands.split(',').map(|s| s.trim());
    // `|Demo$$Code|` names the area `Demo$$Code`. The bars only mark the
    // ends of the name. They let a name hold characters a symbol otherwise
    // could not.
    let raw = parts.next().unwrap_or("").trim();
    let name = raw
        .strip_prefix('|')
        .and_then(|r| r.strip_suffix('|'))
        .unwrap_or(raw)
        .to_string();
    if name.is_empty() {
        return Err("AREA needs a name".into());
    }
    let mut a = AreaAttrs::new();
    for p in parts {
        if p.is_empty() {
            continue;
        }
        let up = p.to_ascii_uppercase();
        // `ALIGN=n` and `BASED Rn` carry a value.
        if let Some(v) = up.strip_prefix("ALIGN") {
            let v = v.trim_start_matches([' ', '=']).trim();
            match v.parse::<u32>() {
                Ok(n) if (2..=12).contains(&n) => a.align = n,
                Ok(n) => return Err(format!("AREA ALIGN={n} outside 2..12")),
                Err(_) => return Err(format!("bad AREA ALIGN '{v}'")),
            }
            continue;
        }
        if let Some(v) = up.strip_prefix("BASED") {
            let v = v.trim();
            let n = v
                .trim_start_matches(['R', 'r'])
                .parse::<u32>()
                .map_err(|_| format!("bad BASED register '{v}'"))?;
            a.based = Some(n);
            continue;
        }
        match up.as_str() {
            "CODE" => a.code = true,
            "DATA" => a.code = false,
            "READONLY" => a.readonly = true,
            "READWRITE" => a.readonly = false,
            "NOINIT" => a.noinit = true,
            "ABS" => a.abs = true,
            "REL" => a.abs = false,
            "PIC" => a.pic = true,
            "COMMON" => a.common = true,
            "COMDEF" => a.comdef = true,
            "REENTRANT" => a.reentrant = true,
            "INTERWORK" => a.interwork = true,
            // Documented but with no effect on what we emit.
            "HALFWORD" | "NOSWSTACKCHECK" | "VFP" | "CODEALIGN" => {}
            _ => return Err(format!("unknown AREA attribute '{p}'")),
        }
    }
    Ok((name, a))
}

/// A parsed local-label reference: `%«x»«y»n«routinename»`.
#[derive(Debug, Clone, PartialEq)]
pub struct LocalRef {
    pub dir: Dir,
    pub level: Level,
    pub number: u32,
    pub routine: Option<String>,
}

#[derive(Debug, Clone, Copy, PartialEq)]
pub enum Dir {
    Forward,
    Backward,
    /// No direction given: search both ways within the area.
    Both,
}

#[derive(Debug, Clone, Copy, PartialEq)]
pub enum Level {
    /// `A`: all macro levels.
    All,
    /// `T`: this macro level only.
    This,
    /// Neither letter: from the current macro level up to the top level.
    Default,
}

/// Parse `%FT01name`. Returns None if this is not a local-label reference.
pub fn parse_local_ref(s: &str) -> Option<LocalRef> {
    let body = s.strip_prefix('%')?;
    let mut chars = body.chars().peekable();

    let dir = match chars.peek() {
        Some('F' | 'f') => {
            chars.next();
            Dir::Forward
        }
        Some('B' | 'b') => {
            chars.next();
            Dir::Backward
        }
        _ => Dir::Both,
    };
    let level = match chars.peek() {
        Some('A' | 'a') => {
            chars.next();
            Level::All
        }
        Some('T' | 't') => {
            chars.next();
            Level::This
        }
        _ => Level::Default,
    };

    let mut digits = String::new();
    while chars.peek().is_some_and(|c| c.is_ascii_digit()) {
        digits.push(chars.next().unwrap());
    }
    if digits.is_empty() {
        return None;
    }
    let number: u32 = digits.parse().ok()?;
    // The manual gives the range as 0..=99, but the sources go further.
    // `%100`, `%110` and `%111` all appear, and ObjAsm assembles them. So
    // the number is taken as written, with no limit.
    let routine: String = chars.collect();
    Some(LocalRef {
        dir,
        level,
        number,
        routine: if routine.is_empty() {
            None
        } else {
            Some(routine)
        },
    })
}

/// Is this label field a local-label *definition*? `10`, or `10routine`.
pub fn parse_local_def(label: &str) -> Option<(u32, Option<String>)> {
    let mut chars = label.chars().peekable();
    let mut digits = String::new();
    while chars.peek().is_some_and(|c| c.is_ascii_digit()) {
        digits.push(chars.next().unwrap());
    }
    if digits.is_empty() {
        return None;
    }
    let n: u32 = digits.parse().ok()?;
    let rest: String = chars.collect();
    Some((n, if rest.is_empty() { None } else { Some(rest) }))
}

/// Bytes reserved by a data directive, given its operand text.
///
/// This gives only the size. The values themselves are worked out elsewhere
/// in the assembler. `DCB` counts the characters of a string, so
/// `DCB "abc",0` is four bytes.
pub fn data_size(directive: &str, operands: &str) -> Option<u32> {
    let unit = match directive.to_ascii_uppercase().as_str() {
        "DCB" | "=" => 1,
        "DCW" => 2,
        // `DCI` is a word, like `DCD`. The difference is that it declares
        // the word to be an instruction. That matters to a disassembler, not
        // to us.
        "DCD" | "&" | "DCFS" | "DCI" => 4,
        "DCQ" | "DCFD" => 8,
        _ => return None,
    };
    let mut total = 0u32;
    for item in split_top_level(operands) {
        let item = item.trim();
        if item.is_empty() {
            continue;
        }
        if unit == 1 && item.starts_with('"') {
            // A string gives one byte per character. `""` inside it is one
            // quote character, not two.
            // Take one quote off each end, not every quote. `DCB """", 0`
            // holds one quote character, and stripping every quote would
            // leave nothing.
            let inner = item
                .strip_prefix('"')
                .and_then(|s| s.strip_suffix('"'))
                .unwrap_or(item);
            total += crate::lex::string_chars(inner).chars().count() as u32;
        } else {
            total += unit;
        }
    }
    Some(total)
}

/// Split on commas outside strings and brackets.
pub fn split_top_level(s: &str) -> Vec<String> {
    let mut out = Vec::new();
    let mut cur = String::new();
    let mut depth = 0i32;
    let mut in_str = false;
    let mut it = s.chars().peekable();
    while let Some(c) = it.next() {
        match c {
            '"' => {
                if in_str && it.peek() == Some(&'"') {
                    cur.push('"');
                    cur.push(it.next().unwrap());
                    continue;
                }
                in_str = !in_str;
                cur.push(c);
            }
            '(' | '{' if !in_str => {
                depth += 1;
                cur.push(c);
            }
            ')' | '}' if !in_str => {
                depth -= 1;
                cur.push(c);
            }
            ',' if !in_str && depth == 0 => out.push(std::mem::take(&mut cur)),
            _ => cur.push(c),
        }
    }
    out.push(cur);
    out
}

/// Round `offset` up to a multiple of `boundary` (a power of two), then add
/// `plus`.
pub fn align_to(offset: u32, boundary: u32, plus: u32) -> u32 {
    if boundary == 0 {
        return offset;
    }
    let rem = offset % boundary;
    let base = if rem == 0 { offset } else { offset + (boundary - rem) };
    base + plus
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn area_defaults_match_the_manual() {
        let (n, a) = parse_area("MyCode").unwrap();
        assert_eq!(n, "MyCode");
        // REL, READWRITE, word aligned.
        assert!(!a.abs);
        assert!(!a.readonly);
        assert_eq!(a.align, 2);
        assert!(!a.code);
    }

    #[test]
    fn bars_around_an_area_name_are_delimiters() {
        assert_eq!(parse_area("|Demo$$Code|, CODE").unwrap().0, "Demo$$Code");
        // An unbarred name is unchanged, and a lone bar is not a delimiter.
        assert_eq!(parse_area("Plain, CODE").unwrap().0, "Plain");
    }

    #[test]
    fn area_attributes_parse() {
        let (n, a) = parse_area("Kernel, CODE, READONLY, PIC").unwrap();
        assert_eq!(n, "Kernel");
        assert!(a.code && a.readonly && a.pic);
    }

    #[test]
    fn area_align_must_be_in_range() {
        assert_eq!(parse_area("A, ALIGN=4").unwrap().1.align, 4);
        assert!(parse_area("A, ALIGN=13").is_err());
        assert!(parse_area("A, ALIGN=1").is_err());
    }

    #[test]
    fn area_based_register() {
        assert_eq!(parse_area("D, DATA, BASED r9").unwrap().1.based, Some(9));
    }

    #[test]
    fn unknown_area_attribute_is_reported_not_ignored() {
        assert!(parse_area("A, WIBBLE").is_err());
    }

    #[test]
    fn local_reference_forms() {
        assert_eq!(
            parse_local_ref("%FT01"),
            Some(LocalRef { dir: Dir::Forward, level: Level::This, number: 1, routine: None })
        );
        assert_eq!(
            parse_local_ref("%BA20"),
            Some(LocalRef { dir: Dir::Backward, level: Level::All, number: 20, routine: None })
        );
        // No direction: search both ways.
        assert_eq!(parse_local_ref("%01").unwrap().dir, Dir::Both);
        assert_eq!(parse_local_ref("%01").unwrap().level, Level::Default);
        // With a routine name.
        assert_eq!(
            parse_local_ref("%FT05loop").unwrap().routine,
            Some("loop".into())
        );
    }

    #[test]
    fn local_labels_may_run_past_the_documented_99() {
        // The manual says 0..=99; the corpus uses %100, %110 and %111.
        assert_eq!(parse_local_ref("%99").unwrap().number, 99);
        assert_eq!(parse_local_ref("%111").unwrap().number, 111);
        // A leading zero is still just a number.
        assert_eq!(parse_local_ref("%016").unwrap().number, 16);
        assert_eq!(parse_local_def("100").unwrap().0, 100);
    }

    #[test]
    fn local_definitions_may_carry_the_routine_name() {
        assert_eq!(parse_local_def("10"), Some((10, None)));
        assert_eq!(parse_local_def("10loop"), Some((10, Some("loop".into()))));
        assert_eq!(parse_local_def("Label"), None);
    }

    #[test]
    fn data_sizes() {
        assert_eq!(data_size("DCI", "0xE1A00000"), Some(4));
        assert_eq!(data_size("DCD", "1, 2, 3"), Some(12));
        assert_eq!(data_size("DCW", "1, 2"), Some(4));
        assert_eq!(data_size("DCQ", "1"), Some(8));
        assert_eq!(data_size("MOV", "r0, #1"), None);
    }

    #[test]
    fn dcb_counts_string_characters() {
        assert_eq!(data_size("DCB", "\"abc\", 0"), Some(4));
        assert_eq!(data_size("DCB", "\"\""), Some(0));
        // A doubled quote is one character.
        assert_eq!(data_size("DCB", "\"a\"\"b\""), Some(3));
        // A comma inside the string is not a separator.
        assert_eq!(data_size("DCB", "\"a,b\""), Some(3));
    }

    #[test]
    fn alignment_rounds_up() {
        assert_eq!(align_to(0, 4, 0), 0);
        assert_eq!(align_to(1, 4, 0), 4);
        assert_eq!(align_to(4, 4, 0), 4);
        assert_eq!(align_to(5, 4, 0), 8);
        assert_eq!(align_to(1, 4, 2), 6);
    }
}
