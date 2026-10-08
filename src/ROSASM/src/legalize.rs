//! Legalization: Acorn instruction forms to something LLVM will encode.
//!
//! The pipeline runs
//!
//! ```text
//!   ObjAsm source -> expand -> lower (syntax) -> LEGALIZE -> LLVM -> AOF
//! ```
//!
//! `lower` works on the text only. It renames `SWI` to `SVC`, reorders a
//! pre-UAL suffix, substitutes register aliases and rewrites number literals.
//! Legalization is the stage that decides what an instruction can *become*.
//! It is needed because some Acorn forms have no UAL spelling at all:
//!
//! * **`ADRL`** is a pseudo-instruction. ObjAsm expands it into two
//!   data-processing instructions, so it reaches further than `ADR`'s single
//!   rotated immediate allows. LLVM has never heard of it, so we expand it
//!   ourselves. We can, because after the first pass we know both the
//!   location counter and the target.
//! * **`TEQP`, `TSTP`, `CMPP`, `CMNP`** wrote the PSR directly on a 26-bit
//!   ARM. There is no 32-bit equivalent and the encoder rejects them.
//! * **`SWP`** is deprecated: "requires armv7 or earlier".
//!
//! Anything with no legal form becomes a raw word, so one unencodable
//! instruction costs one instruction rather than the whole file.

use crate::lower;

/// What an Acorn instruction legalizes to.
#[derive(Debug, Clone, PartialEq)]
pub enum Legalized {
    /// A single UAL instruction the encoder will accept.
    One(String, String),
    /// A pseudo-instruction that expands to several real ones.
    Many(Vec<(String, String)>),
    /// No UAL form exists; emit this word directly with `.inst`.
    RawWord(u32),
    /// Cannot be legalized here, with the reason.
    Unsupported(String),
}

/// What legalization needs to know about where it is.
#[derive(Default)]
pub struct Context {
    /// Address of the instruction being legalized, within its area.
    pub here: u32,
    /// What an `ADR`/`ADRL` is aiming at, when it is known.
    pub target: Option<AdrTarget>,
    /// Whether the target is a symbol the linker has to supply. It is passed
    /// to the `ADRL` expansion, but `add_or_sub` does not use it at present.
    /// The split between the two instructions depends only on the offset
    /// (see `split_immediates`).
    pub relocated: bool,
    /// Translate the FPA instruction set into VFP instead of encoding it.
    ///
    /// Building this ROM does not want this. Nothing executes an FPA
    /// instruction here. FPEmulator reads the word back and interprets it, so
    /// the word must be the one the source asked for. The translation answers
    /// a different question: what RISC OS would look like if it used the
    /// floating point the hardware actually has. That would be a deliberate
    /// change to the sources, and this option is how it can be tried against
    /// them.
    pub fpa_to_vfp: bool,
}


/// The three kinds of expression `ADR` accepts, from the manual: "The
/// expression may be register-relative, program-relative or numeric", and each
/// produces a different instruction.
#[derive(Debug, Clone, Copy, PartialEq)]
pub enum AdrTarget {
    /// An address in this same area. `ADD`/`SUB` against `pc`.
    Program(u32),
    /// A plain number, with no program or register to be relative to.
    /// `MOV`/`MVN`, or `MOVW`+`MOVT` where the long form needs the range.
    Numeric(u32),
    /// An offset from the base register a `MAP expr,Rn` established.
    /// `ADD`/`SUB` against that register. The offset is signed, because a
    /// storage map may start below its base: `^ -12,R12` is how DragAnObj
    /// describes the words it keeps under the stack pointer.
    Register { base: u32, offset: i32 },
}

// ------------------------------------------------------- ARM immediates

/// Can this value be an ARM data-processing immediate?
///
/// The field is eight bits rotated right by an even amount, so only a small
/// set of values fits. That is the whole reason `ADRL` exists. Returns the
/// rotation (in steps of two bits) and the eight-bit value.
pub fn as_arm_immediate(v: u32) -> Option<(u32, u32)> {
    if v <= 0xFF {
        return Some((0, v));
    }
    for rot in 1..16u32 {
        let shifted = v.rotate_left(2 * rot);
        if shifted <= 0xFF {
            return Some((rot, shifted));
        }
    }
    None
}

/// Split a value into a sum of ARM immediates.
///
/// Each part is the eight-bit field at some even rotation. Returns `None` if
/// more parts would be needed than allowed.
///
/// Which end the split starts from matters, because the result must match
/// ObjAsm's, and ObjAsm does not always start from the same end. FilterMgr's
/// `ADRL r1, <label>` at a distance of &11A is `SUB r1, pc, #&1A` /
/// `SUB r1, r1, #&100`, low field first. BCMSupport's nine device veneers,
/// which reach an imported symbol, are `SUB ip, pc, #&20` /
/// `SUB ip, ip, #&C` for a distance of &2C, high field first. From the low
/// end that would be `#&2C` and `#0`. There are fourteen such examples, and
/// they do not agree with each other. The rule used, given in the code
/// below, depends only on the value.
///
/// When the high field would leave a remainder that no single field can
/// hold, `top_field` takes a field at a lower rotation instead.
pub fn split_immediates(v: u32, max_parts: usize) -> Option<Vec<u32>> {
    if v == 0 {
        return Some(vec![0]);
    }
    if max_parts <= 1 {
        return as_arm_immediate(v).is_some().then(|| vec![v]);
    }
    // Three hundred and forty `ADR` pairs in ObjAsm's own objects show which
    // end it starts from, and it is not always the same end. An offset that
    // is a multiple of four is built from the top down. One that is not is
    // built from the bottom up. `&74` comes out as `#&40` then `#&34`, and
    // `&8A` comes out as `#&8A` then nothing.
    let first = if v.is_multiple_of(4) { top_field(v) } else { bottom_field(v) };
    let rest = v - first;
    if rest == 0 {
        return Some(vec![first]);
    }
    let mut out = vec![first];
    out.extend(split_immediates(rest, max_parts - 1)?);
    Some(out)
}

/// The eight-bit field holding the highest set bit. If what it leaves
/// behind will not fit in one field of its own, the field is moved down a
/// rotation at a time until it does.
///
/// ADFSFiler shows the need. `&34F4` taken at the highest rotation is
/// `#&3000`, leaving `&4F4`, which no single field can hold. One rotation
/// down takes `#&3400` and leaves `&F4`, which is what ObjAsm writes.
fn top_field(v: u32) -> u32 {
    let top = v.bit_length_even();
    let mut s = top;
    loop {
        let field = v & (0xFFu32 << s);
        if as_arm_immediate(v - field).is_some() || s == 0 {
            return field;
        }
        s -= 2;
    }
}

/// The eight-bit field holding the lowest set bit.
fn bottom_field(v: u32) -> u32 {
    v & (0xFF << ((v.trailing_zeros()) & !1))
}

trait BitLengthEven {
    fn bit_length_even(self) -> u32;
}

impl BitLengthEven for u32 {
    /// The position of the highest set bit, rounded down to an even number.
    /// An eight-bit field may start only at an even position.
    fn bit_length_even(self) -> u32 {
        (31 - self.leading_zeros()) & !1
    }
}

/// Expand `ADRL Rd, target` into the instructions ObjAsm would generate.
///
/// `pc` reads as the instruction's own address plus eight, so the offset is
/// measured from there. A negative offset becomes `SUB`.
pub fn expand_adrl(cond: &str, rd: &str, here: u32, target: u32) -> Legalized {
    add_or_sub(cond, rd, "pc", target as i64 - (here as i64 + 8), 2, false)
}

/// `Rd := base ± magnitude`, in exactly `count` instructions.
///
/// The immediate is eight bits rotated by an even amount, so a wide offset
/// must be built up in pieces. The first piece works from `base`, and each
/// one after it from the destination. Exactly `count` instructions come out
/// even when fewer would do. The location counter was advanced on that
/// basis, and a shorter expansion would move every label after it.
fn add_or_sub(
    cond: &str,
    rd: &str,
    base: &str,
    delta: i64,
    count: usize,
    relocated: bool,
) -> Legalized {
    let (op, mag) = if delta >= 0 {
        ("ADD", delta as u32)
    } else {
        ("SUB", (-delta) as u32)
    };
    // what did we mean to do with relocated?
    let _ = relocated;
    let Some(parts) = split_immediates(mag, count) else {
        return Legalized::Unsupported(if count == 1 {
            format!("offset {delta} does not fit one instruction; ADRL reaches further")
        } else {
            format!("offset {delta} needs more than {count} instructions")
        });
    };
    let mut out: Vec<(String, String)> = parts
        .iter()
        .enumerate()
        .map(|(i, p)| {
            let src = if i == 0 { base } else { rd };
            (format!("{op}{cond}"), format!("{rd}, {src}, #{p}"))
        })
        .collect();
    while out.len() < count {
        out.push((format!("{op}{cond}"), format!("{rd}, {rd}, #0")));
    }
    if out.len() == 1 {
        let (m, o) = out.pop().unwrap();
        Legalized::One(m, o)
    } else {
        Legalized::Many(out)
    }
}

/// Expand `ADR Rd, target` into the single instruction it stands for.
///
/// Unlike `ADRL` this is one instruction, so the offset must fit in a single
/// rotated immediate. If it does not, the source should have used `ADRL`.
pub fn expand_adr(cond: &str, rd: &str, here: u32, target: u32) -> Legalized {
    add_or_sub(cond, rd, "pc", target as i64 - (here as i64 + 8), 1, false)
}

/// `TEQP Rn, op2` and its family, as the word ObjAsm writes.
///
/// These set the PSR directly on a 26-bit ARM. Each is the comparison with
/// its `S` bit set and `pc` as the destination register. No UAL spelling
/// exists and the encoder will not take one, so the word is built here. The
/// sources use these forms a hundred and fifty-eight times. Before this,
/// every one became a zero word, which no processor would have executed.
fn psr_form(up: &str, operands: &str) -> Legalized {
    let stem = &up[..3];
    let opcode: u32 = match stem {
        "TST" => 0b1000,
        "TEQ" => 0b1001,
        "CMP" => 0b1010,
        _ => 0b1011,
    };
    // Take the condition from `lower::psr_condition`, which decided the
    // spelling. Then the two agree about which `P` is the marker and which
    // is half of a condition.
    let Some(cond) = lower::psr_condition(up).and_then(|c| condition_bits(&c)) else {
        return Legalized::Unsupported(format!("{up} has no condition I recognise"));
    };
    let mut parts = operands.split(',').map(str::trim);
    let (Some(rn), Some(op2)) = (parts.next(), parts.next()) else {
        return Legalized::Unsupported(format!("{up} needs a register and an operand"));
    };
    if parts.next().is_some() {
        return Legalized::Unsupported(format!("{up} with a shifted operand"));
    }
    let Some(rn) = register_bits(rn) else {
        return Legalized::Unsupported(format!("{up}: '{rn}' is not a register"));
    };
    // `Rd` is `pc`, which is what made it write the PSR.
    let head = (cond << 28) | (opcode << 21) | (1 << 20) | (rn << 16) | (0xF << 12);
    if let Some(text) = op2.strip_prefix('#') {
        let Ok(v) = parse_number(text) else {
            return Legalized::Unsupported(format!("{up}: '{op2}' is not a number"));
        };
        let Some((rot, imm)) = as_arm_immediate(v) else {
            return Legalized::Unsupported(format!("{up}: {v:#x} is not an ARM immediate"));
        };
        return Legalized::RawWord(head | (1 << 25) | (rot << 8) | imm);
    }
    match register_bits(op2) {
        Some(rm) => Legalized::RawWord(head | rm),
        None => Legalized::Unsupported(format!("{up}: '{op2}' is not a register")),
    }
}

/// The four condition bits, or `None` if that is not a condition.
pub fn condition_bits(name: &str) -> Option<u32> {
    if name.is_empty() {
        return Some(0xE);
    }
    lower::CONDS.iter().position(|c| *c == name).map(|i| i as u32)
}

/// A register's number, by name or by the spellings the encoder uses.
fn register_bits(name: &str) -> Option<u32> {
    let n = name.trim().to_ascii_lowercase();
    match n.as_str() {
        "pc" => Some(15),
        "lr" => Some(14),
        "sp" => Some(13),
        _ => n.strip_prefix('r')?.parse::<u32>().ok().filter(|v| *v < 16),
    }
}

/// A literal already reduced to a number by the expression evaluator.
fn parse_number(s: &str) -> Result<u32, ()> {
    let s = s.trim();
    let v = match s.strip_prefix("0x").or_else(|| s.strip_prefix("0X")) {
        Some(h) => u32::from_str_radix(h, 16),
        None => s.parse::<u32>(),
    };
    v.map_err(|_| ())
}

/// Legalize one instruction.
pub fn legalize(mnemonic: &str, operands: &str, ctx: &Context) -> Legalized {
    let up = mnemonic.to_ascii_uppercase();

    if lower::is_adr(&up) || lower::is_adrl(&up) {
        // The long form is always two instructions, the short one always one.
        let count = if lower::is_adrl(&up) { 2 } else { 1 };
        let cond = lower::adr_condition(&up);
        let rd = operands.split(',').next().unwrap_or("r0").trim().to_string();
        let Some(target) = ctx.target else {
            return Legalized::Unsupported(format!("{up} to an unresolved target"));
        };
        return match target {
            AdrTarget::Program(t) => {
                match add_or_sub(
                    &cond,
                    &rd,
                    "pc",
                    t as i64 - (ctx.here as i64 + 8),
                    count,
                    ctx.relocated,
                ) {
                    Legalized::Unsupported(why) => Legalized::Unsupported(format!("{up} {why}")),
                    other => other,
                }
            }
            AdrTarget::Register { base, offset } => {
                match add_or_sub(&cond, &rd, &format!("r{base}"), offset as i64, count, false) {
                    Legalized::Unsupported(why) => Legalized::Unsupported(format!("{up} {why}")),
                    other => other,
                }
            }
            // A number is not relative to anything, so it is moved rather
            // than added. The long form always takes two instructions, and
            // `MOVW`/`MOVT` between them cover every 32-bit value.
            AdrTarget::Numeric(v) if count == 2 => Legalized::Many(vec![
                (format!("MOVW{cond}"), format!("{rd}, #{}", v & 0xFFFF)),
                (format!("MOVT{cond}"), format!("{rd}, #{}", v >> 16)),
            ]),
            AdrTarget::Numeric(v) if as_arm_immediate(v).is_some() => {
                Legalized::One(format!("MOV{cond}"), format!("{rd}, #{v}"))
            }
            AdrTarget::Numeric(v) if as_arm_immediate(!v).is_some() => {
                Legalized::One(format!("MVN{cond}"), format!("{rd}, #{}", !v))
            }
            AdrTarget::Numeric(v) => Legalized::Unsupported(format!(
                "{up} needs &{v:X} in one instruction, which no MOV or MVN can do; \
                 ADRL reaches further"
            )),
        };
    }

    // `LDR Rd, =value` asks for the value in a register by whatever means.
    // ObjAsm counts it as one instruction, so it must stay one. A value that
    // fits an immediate becomes MOV, and one whose complement fits becomes
    // MVN. Anything else needs a literal pool. That is LTORG's business, and
    // it is not yet built.
    if up.starts_with("LDR") {
        if let Some(rest) = operands.split_once('=') {
            let cond = up.strip_prefix("LDR").unwrap_or("").to_string();
            let rd = rest.0.trim_end_matches([',', ' ']).trim().to_string();
            if let Ok(v) = parse_number(rest.1.trim()) {
                if as_arm_immediate(v).is_some() {
                    return Legalized::One(format!("MOV{cond}"), format!("{rd}, #{v}"));
                }
                if as_arm_immediate(!v).is_some() {
                    return Legalized::One(format!("MVN{cond}"), format!("{rd}, #{}", !v));
                }
                return Legalized::Unsupported(format!(
                    "LDR {rd},=&{v:X} needs a literal pool"
                ));
            }
            return Legalized::Unsupported(format!("LDR {rd},={} is unresolved", rest.1.trim()));
        }
    }

    // The FPA instruction set, which nothing here executes. There is no
    // coprocessor 1 or 2 on this processor, so the word takes the undefined
    // instruction trap. FPEmulator, which the ROM contains, then reads the
    // word back out of the instruction stream as data and interprets it. So
    // the word is emitted bit for bit as the source asked for it. It is not
    // translated into something else that would never be read, unless
    // `fpa_to_vfp` asks for that.
    if ctx.fpa_to_vfp {
        if let Some(l) = crate::fpa::convert(&up, operands) {
            return l;
        }
    }
    if let Some(l) = crate::fpa::encode(&up, operands) {
        return l;
    }

    // ObjAsm's VFP dialect, which the encoder takes only in part. The
    // multiple transfers, the register transfers, the conversions and every
    // conditional data operation are re-spelled as UAL. The loads and stores
    // with writeback become one-register multiple transfers, which is how
    // ObjAsm itself encodes them.
    if let Some(l) = crate::vfp::convert(&up, operands) {
        return l;
    }

    // ObjAsm lets a single-register `VPUSH`/`VPOP` go without its braces, and
    // `Trig64` writes it both ways in one file. UAL always wants them.
    if up.starts_with("VPUSH") || up.starts_with("VPOP") {
        let o = operands.trim();
        if !o.starts_with('{') {
            return Legalized::One(up, format!("{{{o}}}"));
        }
    }

    // A coprocessor transfer, written the way ObjAsm allows and UAL does not.
    // `MRC p14,0,pc,c14,c0` reads the coprocessor into the flags, which UAL
    // spells `apsr_nzcv`, and the final operand may be left off when it is
    // zero.
    if (up.starts_with("MRC") || up.starts_with("MCR")) && !up.starts_with("MRRC")
        && !up.starts_with("MCRR")
    {
        let mut parts: Vec<String> = operands
            .split(',')
            .map(|p| p.trim().to_string())
            .filter(|p| !p.is_empty())
            .collect();
        if parts.len() >= 3 && up.starts_with("MRC") && parts[2].eq_ignore_ascii_case("pc") {
            parts[2] = "apsr_nzcv".into();
        }
        if parts.len() == 5 {
            parts.push("0".into());
        }
        return Legalized::One(up, parts.join(", "));
    }

    if lower::is_psr_form(&up) {
        return psr_form(&up, operands);
    }

    if up.starts_with("SWP") {
        return Legalized::Unsupported(format!("{up} is deprecated beyond ARMv7"));
    }

    let m = lower::normalise_mnemonic(&up).unwrap_or(up);
    Legalized::One(m, operands.to_string())
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn small_values_are_immediates_without_rotation() {
        assert_eq!(as_arm_immediate(0), Some((0, 0)));
        assert_eq!(as_arm_immediate(0xFF), Some((0, 0xFF)));
    }

    #[test]
    fn a_rotated_byte_is_an_immediate() {
        // 0xFF00 is 0xFF rotated right by 24, and 0xFF000000 is 0xFF
        // rotated right by 8.
        assert!(as_arm_immediate(0xFF00).is_some());
        assert!(as_arm_immediate(0xFF00_0000).is_some());
    }

    #[test]
    fn a_value_spanning_more_than_eight_bits_is_not_an_immediate() {
        // This is exactly the case ADRL exists to handle.
        assert_eq!(as_arm_immediate(0x1234), None);
        assert_eq!(as_arm_immediate(0x101), None);
    }

    #[test]
    fn splitting_reproduces_the_value() {
        for v in [0x1234u32, 0x101, 0xABCD, 0xFFFF, 0x12345] {
            let parts = split_immediates(v, 4).unwrap_or_else(|| panic!("{v:#x}"));
            assert_eq!(parts.iter().sum::<u32>(), v, "{v:#x} must be reconstructed");
            for p in &parts {
                assert!(as_arm_immediate(*p).is_some(), "{p:#x} must be encodable");
            }
        }
    }

    #[test]
    fn a_value_needing_too_many_parts_is_refused() {
        // Alternating bits cannot be covered by two 8-bit windows.
        assert_eq!(split_immediates(0x5555_5555, 2), None);
    }

    #[test]
    fn adrl_expands_to_two_instructions_from_pc() {
        // pc reads as the instruction's address + 8.
        let l = expand_adrl("", "r0", 0x1000, 0x1000 + 8 + 0x1234);
        let Legalized::Many(v) = l else { panic!("expected an expansion") };
        assert_eq!(v.len(), 2, "ADRL always occupies eight bytes");
        assert_eq!(v[0].0, "ADD");
        assert!(v[0].1.starts_with("r0, pc, #"), "first works from pc: {}", v[0].1);
        assert!(v[1].1.starts_with("r0, r0, #"), "second from the destination");
        // The parts must add up to the offset.
        let sum: u32 = v
            .iter()
            .map(|(_, o)| o.rsplit('#').next().unwrap().parse::<u32>().unwrap())
            .sum();
        assert_eq!(sum, 0x1234);
    }

    #[test]
    fn a_backward_adrl_subtracts() {
        let l = expand_adrl("", "r1", 0x2000, 0x1000);
        let Legalized::Many(v) = l else { panic!("expected an expansion") };
        assert!(v.iter().all(|(m, _)| m == "SUB"), "backwards means SUB");
        let sum: u32 = v
            .iter()
            .map(|(_, o)| o.rsplit('#').next().unwrap().parse::<u32>().unwrap())
            .sum();
        assert_eq!(sum, 0x2000 + 8 - 0x1000);
    }

    #[test]
    fn a_conditional_adrl_keeps_its_condition() {
        let l = expand_adrl("EQ", "r2", 0, 0x108);
        let Legalized::Many(v) = l else { panic!() };
        assert!(v.iter().all(|(m, _)| m == "ADDEQ"), "{v:?}");
    }

    #[test]
    fn adrl_still_takes_eight_bytes_when_one_would_do() {
        // The location counter was advanced by eight during expansion, so the
        // expansion must occupy eight bytes even for a short offset.
        let l = expand_adrl("", "r0", 0, 8 + 4);
        let Legalized::Many(v) = l else { panic!() };
        assert_eq!(v.len(), 2);
        assert!(v[1].1.ends_with("#0"), "padded with a no-op add");
    }

    #[test]
    fn adr_is_one_instruction_measured_from_pc() {
        // pc reads as the instruction's address + 8, so from &04 a label at
        // &24 is 24 bytes away, not 32.
        assert_eq!(
            expand_adr("", "r0", 0x04, 0x24),
            Legalized::One("ADD".into(), "r0, pc, #24".into())
        );
    }

    #[test]
    fn a_backward_adr_subtracts() {
        assert_eq!(
            expand_adr("NE", "r1", 0x40, 0x10),
            Legalized::One("SUBNE".into(), "r1, pc, #56".into())
        );
    }

    #[test]
    fn an_adr_too_far_for_one_immediate_is_refused() {
        // This is what ADRL is for, and the message says so.
        match expand_adr("", "r0", 0, 0x1234 + 8) {
            Legalized::Unsupported(why) => assert!(why.contains("ADRL"), "{why}"),
            other => panic!("expected a refusal, got {other:?}"),
        }
    }

    #[test]
    fn an_adr_with_no_known_target_is_reported() {
        let ctx = Context { here: 0, target: None, relocated: false, fpa_to_vfp: false };
        assert!(matches!(
            legalize("ADR", "r0, Somewhere", &ctx),
            Legalized::Unsupported(_)
        ));
    }

    #[test]
    fn an_adrl_with_no_known_target_is_reported() {
        let ctx = Context { here: 0, target: None, relocated: false, fpa_to_vfp: false };
        let l = legalize("ADRL", "r0, Somewhere", &ctx);
        assert!(matches!(l, Legalized::Unsupported(_)));
    }

    #[test]
    fn an_ldr_of_a_small_literal_becomes_a_move() {
        let ctx = Context { here: 0, target: None, relocated: false, fpa_to_vfp: false };
        assert_eq!(
            legalize("LDR", "r0, =0x10", &ctx),
            Legalized::One("MOV".into(), "r0, #16".into())
        );
        // A condition carries across.
        assert_eq!(
            legalize("LDREQ", "r1, =0xFF", &ctx),
            Legalized::One("MOVEQ".into(), "r1, #255".into())
        );
    }

    #[test]
    fn an_ldr_of_a_complemented_literal_becomes_mvn() {
        let ctx = Context { here: 0, target: None, relocated: false, fpa_to_vfp: false };
        // -1 is not an immediate; its complement, 0, is.
        assert_eq!(
            legalize("LDR", "r0, =0xFFFFFFFF", &ctx),
            Legalized::One("MVN".into(), "r0, #0".into())
        );
    }

    #[test]
    fn an_ldr_needing_a_pool_says_so() {
        let ctx = Context { here: 0, target: None, relocated: false, fpa_to_vfp: false };
        match legalize("LDR", "r0, =0x12345678", &ctx) {
            Legalized::Unsupported(why) => assert!(why.contains("literal pool"), "{why}"),
            other => panic!("expected a refusal, got {other:?}"),
        }
    }

    #[test]
    fn a_numeric_adr_moves_rather_than_adds() {
        // "Numeric: MOV|MVN register,#constant will be produced."
        let ctx = |v| Context { here: 0, target: Some(AdrTarget::Numeric(v)), relocated: false, fpa_to_vfp: false };
        assert_eq!(
            legalize("ADR", "r5, 44", &ctx(44)),
            Legalized::One("MOV".into(), "r5, #44".into())
        );
        // -1 is not an immediate, but its complement is.
        assert_eq!(
            legalize("ADR", "r0, x", &ctx(0xFFFF_FFFF)),
            Legalized::One("MVN".into(), "r0, #0".into())
        );
        // Neither: the manual says an error, and the message points at ADRL.
        match legalize("ADR", "r0, x", &ctx(0x1234_5678)) {
            Legalized::Unsupported(why) => assert!(why.contains("ADRL"), "{why}"),
            other => panic!("expected a refusal, got {other:?}"),
        }
    }

    #[test]
    fn a_numeric_adrl_builds_the_word_in_two_halves() {
        // MOV32, which is MOVW then MOVT, and always two instructions.
        let ctx = Context { here: 0, target: Some(AdrTarget::Numeric(0x1234_5678)), relocated: false, fpa_to_vfp: false };
        let Legalized::Many(v) = legalize("ADRL", "r6, x", &ctx) else {
            panic!("expected two instructions")
        };
        assert_eq!(v[0], ("MOVW".into(), "r6, #22136".into()));
        assert_eq!(v[1], ("MOVT".into(), "r6, #4660".into()));
    }

    #[test]
    fn a_register_relative_adr_adds_to_its_base() {
        // `MAP 0,r9` then `Slot # 4` makes Slot four past r9.
        let ctx = Context {
            here: 0,
            target: Some(AdrTarget::Register { base: 9, offset: 4 }),
            relocated: false,
            fpa_to_vfp: false,
        };
        assert_eq!(
            legalize("ADR", "r4, Slot", &ctx),
            Legalized::One("ADD".into(), "r4, r9, #4".into())
        );
    }

    #[test]
    fn a_storage_map_may_sit_below_its_base() {
        // `^ -12,R12` is how DragAnObj describes words under the stack
        // pointer, so the offset is negative and the instruction subtracts.
        let ctx = Context {
            here: 0,
            target: Some(AdrTarget::Register { base: 12, offset: -8 }),
            relocated: false,
            fpa_to_vfp: false,
        };
        assert_eq!(
            legalize("ADR", "r1, area1", &ctx),
            Legalized::One("SUB".into(), "r1, r12, #8".into())
        );
    }

    #[test]
    fn the_pre_ual_adrl_spelling_gets_two_instructions_and_its_condition() {
        // `ADREQL` is ADRL conditional on EQ, and must not be read as ADR.
        // That would give one instruction where the layout counted two.
        let ctx = Context { here: 8, target: Some(AdrTarget::Program(44)), relocated: false, fpa_to_vfp: false };
        let Legalized::Many(v) = legalize("ADREQL", "r2, Msg", &ctx) else {
            panic!("expected two instructions")
        };
        assert_eq!(v.len(), 2);
        assert!(v.iter().all(|(m, _)| m == "ADDEQ"), "{v:?}");
        // `pc` reads eight past the first, so the pair adds up to 28, and
        // the first takes the field holding the highest set bit.
        assert_eq!(v[0].1, "r2, pc, #16");
        assert_eq!(v[1].1, "r2, r2, #12");
    }

    #[test]
    fn a_relocated_adrl_is_split_from_the_other_end() {
        // BCMSupport's device veneers reach an imported symbol, so the pair
        // is a placeholder the linker rewrites. ObjAsm writes the field
        // holding the highest set bit first: `SUB ip, pc, #&20` /
        // `SUB ip, ip, #&C` for a distance of &2C.
        let ctx = Context { here: 0x24, target: Some(AdrTarget::Program(0)), relocated: true, fpa_to_vfp: false };
        let Legalized::Many(v) = legalize("ADRL", "ip, Imported", &ctx) else {
            panic!("expected two instructions")
        };
        assert_eq!(v[0].1, "ip, pc, #32");
        assert_eq!(v[1].1, "ip, ip, #12");
    }

    #[test]
    fn the_26_bit_psr_forms_are_encoded_here() {
        // There is no UAL spelling and the encoder will not take one, so the
        // word is built here. It is the comparison with its `S` bit set and
        // `pc` as the destination, which is what made it write the PSR.
        // IICMod's `TEQP R2, #0` is &E332F000 in ObjAsm's object.
        let ctx = Context { here: 0, target: None, relocated: false, fpa_to_vfp: false };
        assert_eq!(legalize("TEQP", "r2, #0", &ctx), Legalized::RawWord(0xE332_F000));
        assert_eq!(legalize("TEQP", "pc, lr", &ctx), Legalized::RawWord(0xE13F_F00E));
        assert_eq!(legalize("TSTP", "r0, #1", &ctx), Legalized::RawWord(0xE310_F001));
        assert_eq!(legalize("CMPP", "r1, r2", &ctx), Legalized::RawWord(0xE151_F002));
        assert_eq!(legalize("CMNP", "r1, #0", &ctx), Legalized::RawWord(0xE371_F000));
    }

    #[test]
    fn a_psr_form_may_carry_a_condition() {
        let ctx = Context { here: 0, target: None, relocated: false, fpa_to_vfp: false };
        assert_eq!(legalize("TEQNEP", "r2, #0", &ctx), Legalized::RawWord(0x1332_F000));
        assert_eq!(legalize("TEQPNE", "r2, #0", &ctx), Legalized::RawWord(0x1332_F000));
    }

    #[test]
    fn an_fpa_instruction_is_encoded_here() {
        // Nothing executes these. The word takes the undefined instruction
        // trap and FPEmulator reads it back as data, so what goes in the
        // object must be what the source asked for. These words were checked
        // against ObjAsm's own objects.
        let ctx = Context { here: 0, target: None, relocated: false, fpa_to_vfp: false };
        assert_eq!(legalize("RFS", "r1", &ctx), Legalized::RawWord(0xEE30_1110));
        assert_eq!(legalize("WFS", "r1", &ctx), Legalized::RawWord(0xEE20_1110));
        assert_eq!(
            legalize("SFM", "f0, 4, [r0, #4]", &ctx),
            Legalized::RawWord(0xED80_0201)
        );
        assert_eq!(
            legalize("LFM", "f0, 4, [r0, #4]", &ctx),
            Legalized::RawWord(0xED90_0201)
        );
    }

    #[test]
    fn a_braceless_register_list_gets_its_braces() {
        let ctx = Context { here: 0, target: None, relocated: false, fpa_to_vfp: false };
        assert_eq!(
            legalize("VPUSH", "d8", &ctx),
            Legalized::One("VPUSH".into(), "{d8}".into())
        );
        // One that already has them is left alone.
        assert_eq!(
            legalize("VPOP", "{ d8-d9 }", &ctx),
            Legalized::One("VPOP".into(), "{ d8-d9 }".into())
        );
    }

    #[test]
    fn swp_is_reported_as_deprecated() {
        let ctx = Context { here: 0, target: None, relocated: false, fpa_to_vfp: false };
        assert!(matches!(
            legalize("SWP", "r0, r1, [r2]", &ctx),
            Legalized::Unsupported(_)
        ));
    }

    #[test]
    fn an_ordinary_instruction_passes_through_normalised() {
        let ctx = Context { here: 0, target: None, relocated: false, fpa_to_vfp: false };
        assert_eq!(
            legalize("SUBNES", "r1, r1, #1", &ctx),
            Legalized::One("SUBSNE".into(), "r1, r1, #1".into())
        );
        assert_eq!(
            legalize("SWI", "&10", &ctx),
            Legalized::One("SVC".into(), "&10".into())
        );
        assert_eq!(
            legalize("CMPS", "r0, #1", &ctx),
            Legalized::One("CMP".into(), "r0, #1".into())
        );
    }
}
