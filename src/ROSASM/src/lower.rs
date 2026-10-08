//! ObjAsm instruction text to UAL, ready for an encoder.
//!
//! The corpus is written in pre-UAL ARM syntax throughout. 496 sites put the
//! condition before the `S`, and not one puts it after. So every instruction
//! must be normalised before LLVM's assembler will accept it. The transforms
//! are all mechanical, and each is counted in `docs/ROSASM-DESIGN.md`:
//!
//! | Transform                              | Sites |
//! |----------------------------------------|------:|
//! | `SWI` -> `SVC`                         | 9,023 |
//! | `ADR` / `ADRL`                         | 4,615 / 2,173 |
//! | register aliases from `RN`/`CN`/`FN`   | 960   |
//! | pre-UAL suffix order (`SUBNES`)        | 496   |
//! | local labels within a `ROUT`           | 3,493 |
//!
//! Anything the encoder cannot take is emitted as a raw word instead, so one
//! unknown instruction costs one instruction rather than the whole file.

use std::collections::HashMap;

/// The ARM condition codes. All are two characters, so match order does not
/// matter.
pub const CONDS: [&str; 16] = [
    "EQ", "NE", "CS", "CC", "MI", "PL", "VS", "VC", "HI", "LS", "GE", "LT", "GT", "LE", "AL", "NV",
];
/// Conditions with an alternative spelling; both appear in the corpus.
const COND_ALIASES: [(&str, &str); 2] = [("HS", "CS"), ("LO", "CC")];

/// Mnemonics that take an `S` suffix, so `SUBNES` can be told from a mnemonic
/// that merely ends in S.
const S_FORMS: [&str; 17] = [
    "ADD", "ADC", "AND", "ASR", "BIC", "EOR", "LSL", "LSR", "MLA", "MOV", "MUL", "MVN", "ORR",
    "ROR", "RSB", "RSC", "SUB",
];

/// The comparisons. They always set the flags, so UAL forbids the `S` that
/// pre-UAL code wrote anyway. The encoder answers `CMPS` with "instruction
/// 'cmp' can not set flags, but 's' suffix specified". The suffix is dropped,
/// not translated, because it carried no information.
const COMPARE_FORMS: [&str; 4] = ["CMP", "CMN", "TST", "TEQ"];

/// Split a mnemonic into (stem, condition, sets-flags).
///
/// Pre-UAL writes `<op>{cond}{S}`, and UAL writes `<op>{S}{cond}`. Both
/// spellings are accepted here, so running again over text that is already
/// normalised does no harm.
fn split_mnemonic(m: &str) -> Option<(String, Option<String>, bool)> {
    let up = m.to_ascii_uppercase();
    let canon = |c: &str| -> String {
        COND_ALIASES
            .iter()
            .find(|(a, _)| *a == c)
            .map(|(_, real)| real.to_string())
            .unwrap_or_else(|| c.to_string())
    };
    let is_cond = |c: &str| CONDS.contains(&c) || COND_ALIASES.iter().any(|(a, _)| *a == c);

    for stem in S_FORMS {
        if !up.starts_with(stem) {
            continue;
        }
        let rest = &up[stem.len()..];
        return Some(match rest {
            "" => (stem.to_string(), None, false),
            "S" => (stem.to_string(), None, true),
            r if r.len() == 2 && is_cond(r) => (stem.to_string(), Some(canon(r)), false),
            // Pre-UAL: condition then S.
            r if r.len() == 3 && r.ends_with('S') && is_cond(&r[..2]) => {
                (stem.to_string(), Some(canon(&r[..2])), true)
            }
            // UAL: S then condition.
            r if r.len() == 3 && r.starts_with('S') && is_cond(&r[1..]) => {
                (stem.to_string(), Some(canon(&r[1..])), true)
            }
            _ => continue,
        });
    }
    None
}

/// The size and sign suffixes a single data transfer may carry.
fn is_access_size(s: &str) -> bool {
    matches!(s, "B" | "H" | "D" | "SB" | "SH" | "T" | "BT")
}

/// The addressing modes a block transfer may carry, including the stack names.
fn is_block_mode(m: &str) -> bool {
    matches!(m, "IA" | "IB" | "DA" | "DB" | "FD" | "FA" | "ED" | "EA")
}

fn is_condition(c: &str) -> bool {
    CONDS.contains(&c) || COND_ALIASES.iter().any(|(a, _)| *a == c)
}

fn canonical_condition(c: &str) -> String {
    COND_ALIASES
        .iter()
        .find(|(a, _)| *a == c)
        .map(|(_, real)| real.to_string())
        .unwrap_or_else(|| c.to_string())
}

/// The condition on a 26-bit PSR-writing form, if that is what this is.
///
/// `TEQP`, `TSTP`, `CMPP` and `CMNP` set the flags directly on a 26-bit ARM.
/// The sources write the condition on either side of the `P`: `TEQNEP` and
/// `TEQPLS` both occur.
///
/// This makes the spelling ambiguous, and reading the `P` first gets it
/// wrong. `CMPPL` is a comparison on PL, and `TEQPLS` is one on PL with the
/// redundant `S`. So if what follows the stem is a condition in its own
/// right, it is read that way. Only what is left over is a PSR form.
pub fn psr_condition(m: &str) -> Option<String> {
    let up = m.to_ascii_uppercase();
    let stem = COMPARE_FORMS.iter().find(|s| up.starts_with(*s))?;
    let rest = &up[stem.len()..];
    let body = rest.strip_suffix('S').unwrap_or(rest);
    if body.is_empty() || is_condition(body) {
        return None;
    }
    let cond = rest.strip_prefix('P').or_else(|| rest.strip_suffix('P'))?;
    (cond.is_empty() || is_condition(cond)).then(|| canonical_condition(cond))
}

/// Is this a 26-bit PSR-writing form?
pub fn is_psr_form(m: &str) -> bool {
    psr_condition(m).is_some()
}

/// Rewrite one mnemonic into UAL order. Returns None if it needs no change.
pub fn normalise_mnemonic(m: &str) -> Option<String> {
    let up = m.to_ascii_uppercase();

    // `UND` is the pre-UAL spelling of the permanently undefined
    // instruction. UAL calls it `UDF`. BCMVideo plants them where a case
    // cannot arise.
    if let Some(rest) = up.strip_prefix("UND") {
        if rest.is_empty() || is_condition(rest) {
            return Some(format!("UDF{}", canonical_condition(rest)));
        }
    }

    // `SWI` is the pre-UAL spelling of `SVC`. It may carry a condition.
    if let Some(rest) = up.strip_prefix("SWI") {
        if rest.is_empty() || CONDS.contains(&rest) || COND_ALIASES.iter().any(|(a, _)| *a == rest) {
            let cond = COND_ALIASES
                .iter()
                .find(|(a, _)| *a == rest)
                .map(|(_, r)| r.to_string())
                .unwrap_or_else(|| rest.to_string());
            return Some(format!("SVC{cond}"));
        }
    }

    // A comparison's `S` is redundant, and the encoder rejects it. Strip it
    // and keep any condition: `CMPS` -> `CMP`, `CMPNES` -> `CMPNE`.
    for stem in COMPARE_FORMS {
        let Some(rest) = up.strip_prefix(stem) else { continue };
        // `TEQP` and friends set the PSR in 26-bit mode and have no UAL form.
        if psr_condition(&up).is_some() {
            return None;
        }
        let stripped = match rest {
            "S" => Some(String::new()),
            r if r.len() == 3 && r.ends_with('S') && is_condition(&r[..2]) => {
                Some(canonical_condition(&r[..2]))
            }
            _ => None,
        };
        if let Some(cond) = stripped {
            return Some(format!("{stem}{cond}"));
        }
        break;
    }

    // A memory access carries a size as well as a condition, and pre-UAL puts
    // the condition first: `STRVCB` is UAL's `STRBVC`.
    for stem in ["LDR", "STR"] {
        let Some(rest) = up.strip_prefix(stem) else { continue };
        let (cond, size) = rest.split_at(2.min(rest.len()));
        if is_condition(cond) && is_access_size(size) {
            return Some(format!("{stem}{size}{}", canonical_condition(cond)));
        }
        break;
    }

    // A block transfer carries an addressing mode as well as a condition, and
    // pre-UAL writes them the other way round: `LDMVSFD` is UAL's `LDMFDVS`.
    // The stack names the encoder does not take all become the modes they
    // mean, for a load or a store: `LDMED` is `LDMIB`, `STMED` is `STMDA`.
    for stem in ["LDM", "STM"] {
        let Some(rest) = up.strip_prefix(stem) else { continue };
        let (mode, cond) = match rest.len() {
            2 if is_block_mode(rest) => (rest, ""),
            4 if is_condition(&rest[..2]) && is_block_mode(&rest[2..]) => (&rest[2..], &rest[..2]),
            4 if is_block_mode(&rest[..2]) && is_condition(&rest[2..]) => (&rest[..2], &rest[2..]),
            _ => break,
        };
        let load = stem == "LDM";
        let real = match mode {
            "FD" if load => "IA",
            "ED" if load => "IB",
            "FA" if load => "DA",
            "EA" if load => "DB",
            "FD" => "DB",
            "ED" => "DA",
            "FA" => "IB",
            "EA" => "IA",
            m => m,
        };
        let ual = format!("{stem}{real}{}", canonical_condition(cond));
        return (ual != up).then_some(ual);
    }

    let (stem, cond, sets_flags) = split_mnemonic(&up)?;
    let ual = format!(
        "{stem}{}{}",
        if sets_flags { "S" } else { "" },
        cond.as_deref().unwrap_or("")
    );
    if ual == up {
        None
    } else {
        Some(ual)
    }
}

/// Substitute register aliases declared with `RN` in an operand field.
///
/// Only whole words match, so `Rx` does not match inside `Rxy`. A name that
/// happens to appear inside a string is left alone.
pub fn substitute_registers(operands: &str, aliases: &HashMap<String, u32>) -> String {
    if aliases.is_empty() {
        return operands.to_string();
    }
    let mut out = String::with_capacity(operands.len());
    let chars: Vec<char> = operands.chars().collect();
    let mut i = 0;
    let mut in_string = false;
    while i < chars.len() {
        let c = chars[i];
        if c == '"' {
            in_string = !in_string;
            out.push(c);
            i += 1;
            continue;
        }
        if in_string || !(c.is_ascii_alphabetic() || c == '_') {
            out.push(c);
            i += 1;
            continue;
        }
        let start = i;
        while i < chars.len() && (chars[i].is_ascii_alphanumeric() || chars[i] == '_') {
            i += 1;
        }
        let word: String = chars[start..i].iter().collect();
        match aliases.get(&word) {
            Some(n) => out.push_str(&format!("r{n}")),
            None => out.push_str(&word),
        }
    }
    out
}

/// Translate ObjAsm numeric literals into the encoder's spelling.
///
/// ObjAsm writes hexadecimal as `&FF` and any other base as `n_digits`. A
/// character constant is `'A'`. None of these mean anything to LLVM. The
/// first two become a plain `0x` constant, and a character constant becomes
/// its code in decimal. Text inside a string is left alone.
pub fn translate_numbers(operands: &str) -> String {
    let c: Vec<char> = operands.chars().collect();
    let mut out = String::with_capacity(operands.len());
    let mut i = 0;
    let mut in_string = false;
    while i < c.len() {
        let ch = c[i];
        if ch == '"' {
            in_string = !in_string;
            out.push(ch);
            i += 1;
            continue;
        }
        if in_string {
            out.push(ch);
            i += 1;
            continue;
        }
        // &FF
        if ch == '&' && c.get(i + 1).is_some_and(|d| d.is_ascii_hexdigit()) {
            let start = i + 1;
            let mut j = start;
            while j < c.len() && c[j].is_ascii_hexdigit() {
                j += 1;
            }
            out.push_str("0x");
            out.extend(&c[start..j]);
            i = j;
            continue;
        }
        // 'A'
        if ch == '\'' && c.get(i + 2) == Some(&'\'') {
            out.push_str(&format!("{}", c[i + 1] as u32));
            i += 3;
            continue;
        }
        // n_digits, e.g. 2_1010
        if ch.is_ascii_digit() {
            let start = i;
            let mut j = i;
            while j < c.len() && c[j].is_ascii_digit() {
                j += 1;
            }
            if c.get(j) == Some(&'_') {
                let base: u32 = c[start..j].iter().collect::<String>().parse().unwrap_or(10);
                let dstart = j + 1;
                let mut k = dstart;
                while k < c.len() && c[k].is_ascii_alphanumeric() {
                    k += 1;
                }
                let digits: String = c[dstart..k].iter().collect();
                if (2..=36).contains(&base) {
                    if let Ok(v) = u32::from_str_radix(&digits, base) {
                        out.push_str(&format!("0x{v:X}"));
                        i = k;
                        continue;
                    }
                }
            }
            out.extend(&c[start..j]);
            i = j;
            continue;
        }
        out.push(ch);
        i += 1;
    }
    out
}

/// The unique name, one the encoder can take, given to local label `number`
/// in the routine `scope`. References to it are written
/// `%«F|B»«A|T»n«routine»`. `ROUT` bounds the search for a local label, so
/// the scope name is part of the generated symbol.
pub fn local_label_name(scope: &str, number: u32) -> String {
    if scope.is_empty() {
        format!(".L_{number}")
    } else {
        format!(".L_{scope}_{number}")
    }
}

/// Is this the software-interrupt instruction, under either spelling?
///
/// This matters because the sources give the SWI's *name*, a symbol from a
/// header, as in `SWI OS_Write0`. UAL wants `svc #immediate`.
pub fn is_swi(mnemonic: &str) -> bool {
    let up = mnemonic.to_ascii_uppercase();
    let rest = match up.strip_prefix("SWI").or_else(|| up.strip_prefix("SVC")) {
        Some(r) => r,
        None => return false,
    };
    rest.is_empty() || is_condition(rest)
}

/// Is this `ADR`, with or without a condition? `ADR` is a pseudo-instruction
/// too. It stands for an `ADD` or `SUB` against `pc`, or a `MOV` where the
/// expression is just a number. LLVM knows the name, but it would try to
/// resolve the label itself, and it cannot. The labels are ours, and the
/// address is an offset within an AOF area.
pub fn is_adr(mnemonic: &str) -> bool {
    let up = mnemonic.to_ascii_uppercase();
    let Some(rest) = up.strip_prefix("ADR") else { return false };
    rest.is_empty() || is_condition(rest)
}

/// Is this `ADRL`? `ADRL` has no UAL equivalent. It is an Acorn
/// pseudo-instruction that stands for a pair of instructions, which between
/// them reach further than `ADR`. The encoder never sees it.
///
/// It is written two ways. UAL puts the condition last, as in `ADRLEQ`.
/// Pre-UAL puts it in the middle, as in `ADREQL`; the corpus has 547 of
/// those against 2,205 plain ones. Reading only the UAL spelling would take
/// `ADREQL` for an `ADR`. That is the wrong instruction and the wrong size,
/// because an `ADRL` is always two words.
///
/// The two shapes never collide. `ADRLE` is `ADR` conditional on `LE`,
/// because `LE` is a condition and `E` is not. `ADRLEL` can only be the long
/// form with the same condition.
pub fn is_adrl(mnemonic: &str) -> bool {
    let up = mnemonic.to_ascii_uppercase();
    let Some(rest) = up.strip_prefix("ADR") else { return false };
    if is_condition(rest) {
        return false;
    }
    rest == "L"
        || rest.strip_prefix('L').is_some_and(is_condition)
        || rest.strip_suffix('L').is_some_and(is_condition)
}

/// The condition on an `ADR` or `ADRL`, under either spelling.
pub fn adr_condition(mnemonic: &str) -> String {
    let up = mnemonic.to_ascii_uppercase();
    let Some(rest) = up.strip_prefix("ADR") else { return String::new() };
    if rest.is_empty() || rest == "L" {
        return String::new();
    }
    for c in [rest, rest.strip_prefix('L').unwrap_or(""), rest.strip_suffix('L').unwrap_or("")] {
        if is_condition(c) {
            return canonical_condition(c);
        }
    }
    String::new()
}

/// A whole instruction, lowered.
pub fn lower_instruction(
    mnemonic: &str,
    operands: &str,
    aliases: &HashMap<String, u32>,
) -> (String, String) {
    let m = normalise_mnemonic(mnemonic).unwrap_or_else(|| mnemonic.to_string());
    let o = translate_numbers(&substitute_registers(operands, aliases));
    (m, o)
}

/// Add the `#` that ObjAsm lets a data-processing immediate leave out.
/// ObjAsm reads the last operand as an immediate when it is not a register,
/// as in `ADD R1, R1, clipboard_flex_base`. UAL wants the `#`. The last
/// operand is either a register (perhaps shifted, in which case more
/// operands follow) or an immediate. So a single operand after the
/// registers that is not itself a register must be the immediate.
///
/// `lowered` is the operands with register aliases resolved; it is used to
/// tell whether the operand is a register. The `#` is added to `raw`, the
/// same operands as written, which will be evaluated again. Returns None
/// where no `#` is needed.
pub fn implicit_immediate(m: &str, lowered: &str, raw: &str) -> Option<String> {
    let marked = immediate_operand2(m, lowered.to_string());
    if marked == lowered {
        return None;
    }
    let n = marked[..marked.find('#')?].matches(',').count();
    let parts: Vec<&str> = raw.split(',').collect();
    if parts.len() != n + 1 || parts[n].trim_start().starts_with('#') {
        return None;
    }
    let lead = &parts[n][..parts[n].len() - parts[n].trim_start().len()];
    let mut out: Vec<String> = parts[..n].iter().map(|s| s.to_string()).collect();
    out.push(format!("{lead}#{}", parts[n].trim_start()));
    Some(out.join(","))
}

fn immediate_operand2(m: &str, o: String) -> String {
    const THREE: [&str; 10] = ["ADD", "SUB", "RSB", "ADC", "SBC", "RSC", "AND", "ORR", "EOR", "BIC"];
    const TWO: [&str; 6] = ["MOV", "MVN", "CMP", "CMN", "TST", "TEQ"];
    let up = m.to_ascii_uppercase();
    let Some((stem, _, _)) = split_mnemonic(&up) else { return o };
    let regs = if THREE.contains(&stem.as_str()) {
        2
    } else if TWO.contains(&stem.as_str()) {
        1
    } else {
        return o;
    };
    // Split the operands at commas. These forms have no commas inside
    // brackets or strings.
    let (code, comment) = match o.find(';') {
        Some(i) => (&o[..i], &o[i..]),
        None => (o.as_str(), ""),
    };
    let parts: Vec<&str> = code.split(',').collect();
    if parts.len() != regs + 1 || code.contains('"') || code.contains('\'') {
        return o;
    }
    let last = parts[regs].trim();
    let is_reg = |s: &str| {
        let l = s.to_ascii_lowercase();
        matches!(l.as_str(), "sp" | "lr" | "pc" | "ip" | "fp" | "sl" | "sb" | "wp")
            || l.strip_prefix('r').is_some_and(|n| n.parse::<u8>().is_ok_and(|n| n < 16))
            || l.strip_prefix('a').is_some_and(|n| matches!(n, "1" | "2" | "3" | "4"))
            || l.strip_prefix('v').is_some_and(|n| n.parse::<u8>().is_ok_and(|n| (1..=8).contains(&n)))
    };
    // `ORR v1, v2, LSL #8`: the shorthand whose last operand is a shift.
    let shift = last.split_whitespace().next().is_some_and(|w| {
        matches!(w.to_ascii_uppercase().as_str(), "LSL" | "LSR" | "ASR" | "ASL" | "ROR" | "RRX")
    });
    if last.is_empty() || last.starts_with('#') || is_reg(last) || shift || !last.starts_with(|c: char| c.is_ascii_alphabetic() || c == '_') {
        return o;
    }
    let lead = &parts[regs][..parts[regs].len() - parts[regs].trim_start().len()];
    let mut out: Vec<String> = parts[..regs].iter().map(|s| s.to_string()).collect();
    out.push(format!("{lead}#{}", parts[regs].trim_start()));
    format!("{}{comment}", out.join(","))
}

#[cfg(test)]
mod tests {
    use super::*;

    fn n(m: &str) -> String {
        normalise_mnemonic(m).unwrap_or_else(|| m.to_string())
    }

    #[test]
    fn swi_becomes_svc() {
        // 9,023 sites in the corpus.
        assert_eq!(n("SWI"), "SVC");
        assert_eq!(n("SWINE"), "SVCNE");
        assert_eq!(n("SWIVC"), "SVCVC");
    }

    #[test]
    fn pre_ual_puts_the_condition_before_the_s() {
        // The corpus is entirely pre-UAL: 496 sites like these, none the other
        // way round.
        assert_eq!(n("SUBNES"), "SUBSNE");
        assert_eq!(n("MOVEQS"), "MOVSEQ");
        assert_eq!(n("RSBGES"), "RSBSGE");
        assert_eq!(n("ORRNES"), "ORRSNE");
    }

    #[test]
    fn a_pre_ual_memory_suffix_is_reordered() {
        // The condition comes first in the sources and last in UAL.
        assert_eq!(normalise_mnemonic("STRVCB").as_deref(), Some("STRBVC"));
        assert_eq!(normalise_mnemonic("LDRNEB").as_deref(), Some("LDRBNE"));
        assert_eq!(normalise_mnemonic("LDRLOSH").as_deref(), Some("LDRSHCC"));
        // Already UAL, or no suffix at all: left alone.
        assert_eq!(normalise_mnemonic("LDRB"), None);
        assert_eq!(normalise_mnemonic("LDR"), None);
    }

    #[test]
    fn a_pre_ual_block_transfer_is_reordered() {
        assert_eq!(normalise_mnemonic("LDMVSFD").as_deref(), Some("LDMIAVS"));
        assert_eq!(normalise_mnemonic("STMEQIA").as_deref(), Some("STMIAEQ"));
        // Already UAL.
        assert_eq!(normalise_mnemonic("LDMFDVS").as_deref(), Some("LDMIAVS"));
        assert_eq!(normalise_mnemonic("LDMED").as_deref(), Some("LDMIB"));
        assert_eq!(normalise_mnemonic("STMNEED").as_deref(), Some("STMDANE"));
        assert_eq!(normalise_mnemonic("LDMIA"), None);
    }

    #[test]
    fn a_bare_immediate_gets_its_hash() {
        let l = |m: &str, o: &str| immediate_operand2(m, o.to_string());
        assert_eq!(l("ADD", "R1,R1,clipboard_flex_base"), "R1,R1,#clipboard_flex_base");
        assert_eq!(l("MOVNE", "R0, Foo"), "R0, #Foo");
        assert_eq!(l("ADD", "R1,R1,R2"), "R1,R1,R2");
        assert_eq!(l("ADD", "R1,R1,lr"), "R1,R1,lr");
        assert_eq!(l("ADD", "R1,R1,#4"), "R1,R1,#4");
        assert_eq!(l("ADD", "R1,R1,R2,LSL #2"), "R1,R1,R2,LSL #2");
        assert_eq!(l("LDR", "R1,Foo"), "R1,Foo");
        assert_eq!(l("ORRCS", "v1, v2, LSL #8"), "v1, v2, LSL #8");
        assert_eq!(l("MOV", "r0, r1, rrx"), "r0, r1, rrx");
        assert_eq!(implicit_immediate("ADD", "r1,r1,base", "R1, R1, base").as_deref(), Some("R1, R1, #base"));
        assert_eq!(implicit_immediate("ADD", "r1,r1,r3", "R1, R1, FACC"), None);
    }

    #[test]
    fn already_ual_text_is_left_alone() {
        // Normalising twice must not corrupt anything.
        assert_eq!(n("SUBSNE"), "SUBSNE");
        assert_eq!(n("MOV"), "MOV");
        assert_eq!(n("MOVS"), "MOVS");
        assert_eq!(n("MOVEQ"), "MOVEQ");
    }

    #[test]
    fn the_alternate_condition_spellings_are_canonicalised() {
        assert_eq!(n("RSBHSS"), "RSBSCS");
        assert_eq!(n("SUBLOS"), "SUBSCC");
    }

    #[test]
    fn a_mnemonic_merely_ending_in_s_is_not_an_s_form() {
        // These are not <op>{cond}{S}; nothing should be rearranged.
        for m in ["BICS", "ADDS"] {
            assert_eq!(n(m), m, "{m} is already a plain S form");
        }
        assert_eq!(n("LDR"), "LDR");
        assert_eq!(n("STMIA"), "STMIA");
    }

    #[test]
    fn register_aliases_are_substituted_whole_word() {
        let mut a = HashMap::new();
        a.insert("Rregno".to_string(), 3u32);
        a.insert("Rx".to_string(), 7u32);
        assert_eq!(substitute_registers("Rregno, #0", &a), "r3, #0");
        assert_eq!(substitute_registers("Rx, [Rx, #4]", &a), "r7, [r7, #4]");
        // A longer name that merely starts with an alias is untouched.
        assert_eq!(substitute_registers("Rxy, #1", &a), "Rxy, #1");
    }

    #[test]
    fn register_aliases_are_not_substituted_inside_strings() {
        let mut a = HashMap::new();
        a.insert("Rx".to_string(), 7u32);
        assert_eq!(
            substitute_registers("\"Rx is a name\", Rx", &a),
            "\"Rx is a name\", r7"
        );
    }

    #[test]
    fn local_labels_are_scoped_by_their_rout() {
        // The same number may be defined many times; ROUT bounds the search.
        assert_eq!(local_label_name("Copy", 10), ".L_Copy_10");
        assert_eq!(local_label_name("", 10), ".L_10");
        assert_ne!(local_label_name("A", 1), local_label_name("B", 1));
    }

    #[test]
    fn both_adrl_spellings_are_recognised() {
        // UAL puts the condition last, pre-UAL in the middle.
        for m in ["ADRL", "ADRLEQ", "ADREQL", "ADRCSL", "ADRLOL", "ADRLEL"] {
            assert!(is_adrl(m), "{m} is the long form");
            assert!(!is_adr(m), "{m} is not the short one");
        }
        for m in ["ADR", "ADREQ", "ADRLE", "ADRLS", "ADRLO"] {
            assert!(is_adr(m), "{m} is the short form");
            assert!(!is_adrl(m), "{m} is not the long one");
        }
        assert!(!is_adr("ADD") && !is_adrl("ADD"));
    }

    #[test]
    fn the_adr_condition_is_read_from_either_spelling() {
        assert_eq!(adr_condition("ADR"), "");
        assert_eq!(adr_condition("ADRL"), "");
        assert_eq!(adr_condition("ADREQ"), "EQ");
        assert_eq!(adr_condition("ADRLEQ"), "EQ");
        assert_eq!(adr_condition("ADREQL"), "EQ");
        // `LE` is the condition here, not a stray L.
        assert_eq!(adr_condition("ADRLE"), "LE");
        assert_eq!(adr_condition("ADRLEL"), "LE");
        // And the alternate spellings canonicalise.
        assert_eq!(adr_condition("ADRLOL"), "CC");
        assert_eq!(adr_condition("ADRHSL"), "CS");
    }

    #[test]
    fn adrl_is_recognised_as_needing_expansion() {
        assert!(is_adrl("ADRL"));
        assert!(is_adrl("ADRLEQ"));
        assert!(!is_adrl("ADR"));
        assert!(!is_adrl("ADD"));
    }

    #[test]
    fn objasm_number_literals_become_encoder_spelling() {
        assert_eq!(translate_numbers("#&10"), "#0x10");
        assert_eq!(translate_numbers("r0, #&FF"), "r0, #0xFF");
        assert_eq!(translate_numbers("#2_1010"), "#0xA");
        assert_eq!(translate_numbers("#8_777"), "#0x1FF");
        // A character constant becomes its code.
        assert_eq!(translate_numbers("#'A'"), "#65");
        // Decimals and registers are untouched.
        assert_eq!(translate_numbers("r0, r1, #12"), "r0, r1, #12");
    }

    #[test]
    fn numbers_inside_strings_are_left_alone() {
        assert_eq!(translate_numbers("\"&FF is hex\""), "\"&FF is hex\"");
    }

    #[test]
    fn lowering_handles_mnemonic_and_operands_together() {
        let mut a = HashMap::new();
        a.insert("Rcount".to_string(), 5u32);
        let (m, o) = lower_instruction("SUBNES", "Rcount, Rcount, #1", &a);
        assert_eq!(m, "SUBSNE");
        assert_eq!(o, "r5, r5, #1");
    }
}

#[cfg(test)]
mod compare_tests {
    use super::*;

    fn n(m: &str) -> String {
        normalise_mnemonic(m).unwrap_or_else(|| m.to_string())
    }

    #[test]
    fn a_comparison_loses_its_redundant_s() {
        // CMP/CMN/TST/TEQ always set flags. UAL rejects the suffix.
        assert_eq!(n("CMPS"), "CMP");
        assert_eq!(n("TEQS"), "TEQ");
        assert_eq!(n("TSTS"), "TST");
        assert_eq!(n("CMNS"), "CMN");
    }

    #[test]
    fn a_conditional_comparison_keeps_its_condition() {
        assert_eq!(n("TEQNES"), "TEQNE");
        assert_eq!(n("CMPEQS"), "CMPEQ");
        assert_eq!(n("CMPLOS"), "CMPCC");
    }

    #[test]
    fn a_plain_comparison_is_untouched() {
        for m in ["CMP", "TEQ", "TEQNE", "TSTEQ", "CMN"] {
            assert_eq!(n(m), m);
        }
    }

    #[test]
    fn the_26_bit_psr_forms_are_recognised_as_unencodable() {
        // These wrote the PSR directly on a 26-bit ARM and have no UAL form.
        for m in ["TEQP", "TSTP", "CMPP", "CMNP"] {
            assert!(is_psr_form(m), "{m}");
            assert_eq!(n(m), m, "left alone for the raw-word path");
        }
        assert!(!is_psr_form("CMP"));
        assert!(!is_psr_form("TEQNE"));
    }
}

/// How many words a line occupies once it reaches the encoder.
///
/// The location counter settled this during expansion, and every label after
/// the line was placed on that basis. So whatever the encoder is handed must
/// take the same space, even when it is handed nothing. An instruction with
/// no equivalent on the target becomes zero words of *this* many, not one.
/// Otherwise the nine `ADRL`s in BCMSupport's veneers would each lose a word,
/// and move the rest of the file four bytes with them.
pub fn instruction_words(mnemonic: &str, fpa_to_vfp: bool) -> usize {
    let up = mnemonic.to_ascii_uppercase();
    if is_adrl(&up) {
        return 2;
    }
    match crate::fpa::words(&up, fpa_to_vfp) {
        Some(n) => n as usize,
        None => 1,
    }
}
