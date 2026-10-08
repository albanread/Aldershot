//! ObjAsm's VFP mnemonics, lowered to the UAL the encoder accepts.
//!
//! RISC OS 5's BASICVFP is written in ObjAsm's VFP dialect. That is the
//! older, pre-UAL spelling: `FLDMIAD`, `FMRX`, `FCVTDS`, with the condition
//! where pre-UAL puts it. LLVM's assembler accepts some of those spellings
//! and refuses others. It also parses no `VLDR`/`VSTR` with writeback in
//! any spelling, although the instruction words exist.
//!
//! Every lowering here was checked against the words ObjAsm 4.08 itself
//! produced (`-cpu Cortex-A15`, from the DDE). So the object made here is
//! the object ObjAsm would have made:
//!
//! * A **post-indexed** `FLDS S0,[SP],#4` ObjAsm encodes as a one-register
//!   `VLDMIA` with writeback. A disassembler prints it as `vpop {s0}`.
//!   The offset must be exactly the register's width, or there is no
//!   one-register form for it to become.
//! * A **pre-indexed** `[Rn,#-8]!` becomes `VLDMDB`/`VSTMDB` of one
//!   register.
//! * `FLDD Fd,=value` becomes one of three things. Where the value is one
//!   of VFP's eight-bit immediates, it is an immediate `VMOV` (`=1` is
//!   `vmov.f64 d0,#1.0`). For an exact `+0.0` it is a `VMOV.I32` of zero.
//!   Otherwise it is a program-relative `VLDR` from the literal pool. A
//!   single-precision `+0.0` or `-0.0` always goes to the pool, because
//!   ObjAsm will not make an immediate of a zero mantissa.
//! * `FMRX PC,FPSCR` becomes `VMRS APSR_nzcv, FPSCR`. This is the usual
//!   way to branch on a compare. Any other register keeps its name.
//! * `FNMAC` is `VMLS`. This was found by probing ObjAsm, not assumed,
//!   because the two spellings disagree about which operand is negated.
//! * `FTOSI`/`FTOUI` round in the mode FPSCR sets, so they become `VCVTR`.
//!   The `Z` forms (`FTOSIZD`) round towards zero and become plain `VCVT`.

use crate::legalize::Legalized;

/// A decoded ObjAsm VFP mnemonic: stem, and ARM condition if one is there.
#[derive(Debug, Clone, PartialEq)]
pub struct Vfp {
    pub stem: &'static str,
    pub cond: String,
}

/// Every stem, roughly longest first, so that a prefix never wins over a
/// longer match.
///
/// No FPA stem is a prefix of any of these, and none of these begins with
/// an FPA stem. (The FPA writes `LDFS` where this dialect writes `FLDS`.)
/// So the two sets can never both claim the same instruction.
const STEMS: [&str; 62] = [
    "FLDMIAD", "FLDMDBD", "FSTMIAD", "FSTMDBD",
    "FCMPEZD", "FCMPEZS", "FCVTDS", "FCVTSD", "FTOSIZD", "FTOSIZS", "FTOUIZD", "FTOUIZS",
    "FCMPED", "FCMPES", "FCMPZD", "FCMPZS", "FMDRR", "FMRRD", "FMRRS", "FMSRR", "FSITOD",
    "FSITOS", "FUITOD", "FUITOS", "FTOSID", "FTOSIS", "FTOUID", "FTOUIS",
    "FABSD", "FABSS", "FNEGD", "FNEGS", "FSQRTD", "FSQRTS", "FCPYD", "FCPYS", "FCMPD", "FCMPS",
    "FNMACD", "FNMACS", "FNMSCD", "FNMSCS", "FMSCD", "FMSCS",
    "FADDD", "FADDS", "FSUBD", "FSUBS", "FMULD", "FMULS", "FDIVD", "FDIVS", "FMACD", "FMACS",
    "FLDD", "FLDS", "FSTD", "FSTS", "FMRX", "FMXR", "FMRS", "FMSR",
];

const COND_ALIASES: [(&str, &str); 2] = [("HS", "CS"), ("LO", "CC")];

fn is_cond(c: &str) -> bool {
    crate::lower::CONDS.contains(&c) || COND_ALIASES.iter().any(|(a, _)| *a == c)
}

fn canon_cond(c: &str) -> String {
    COND_ALIASES
        .iter()
        .find(|(a, _)| *a == c)
        .map(|(_, real)| real.to_string())
        .unwrap_or_else(|| c.to_string())
}

/// Split a mnemonic into stem and condition, or `None` if it is not VFP.
pub fn parse(mnemonic: &str) -> Option<Vfp> {
    let up = mnemonic.to_ascii_uppercase();
    for stem in STEMS {
        let Some(rest) = up.strip_prefix(stem) else { continue };
        if rest.is_empty() {
            return Some(Vfp { stem, cond: String::new() });
        }
        if rest.len() == 2 && is_cond(rest) {
            return Some(Vfp { stem, cond: canon_cond(rest) });
        }
    }
    None
}

/// Is this a VFP mnemonic at all?
pub fn is_vfp(mnemonic: &str) -> bool {
    parse(mnemonic).is_some()
}

/// One instruction, mnemonic and operands both rewritten.
fn one(m: impl Into<String>, o: impl Into<String>) -> Legalized {
    Legalized::One(m.into(), o.into())
}

/// The `.f64` or `.f32` a double or single stem works in.
fn size_of(stem: &str) -> &'static str {
    if stem.ends_with('D') { ".f64" } else { ".f32" }
}

/// Lower one VFP instruction. `None` only if the mnemonic is not VFP.
pub fn convert(mnemonic: &str, operands: &str) -> Option<Legalized> {
    let v = parse(mnemonic)?;
    let (stem, cond) = (v.stem, v.cond.as_str());
    let o = operands.trim();
    Some(match stem {
        "FLDMIAD" => one(format!("vldmia{cond}.f64"), o),
        "FLDMDBD" => one(format!("vldmdb{cond}.f64"), o),
        "FSTMIAD" => one(format!("vstmia{cond}.f64"), o),
        "FSTMDBD" => one(format!("vstmdb{cond}.f64"), o),

        "FMRX" => {
            // `FMRX PC,FPSCR` reads the compare flags into the PSR; UAL
            // spells that register `apsr_nzcv`.
            let mut parts = o.splitn(2, ',').map(str::trim);
            let rt = parts.next().unwrap_or("r0");
            let sys = parts.next().unwrap_or("fpscr").to_ascii_lowercase();
            if rt.eq_ignore_ascii_case("pc") {
                one(format!("vmrs{cond} apsr_nzcv, {sys}"), "")
            } else {
                one(format!("vmrs{cond} {rt}, {sys}"), "")
            }
        }
        "FMXR" => {
            let mut parts = o.splitn(2, ',').map(str::trim);
            let sys = parts.next().unwrap_or("fpscr").to_ascii_lowercase();
            let rt = parts.next().unwrap_or("r0");
            one(format!("vmsr{cond} {sys}, {rt}"), "")
        }
        // Core-register transfers, all `VMOV` in UAL with the operands as
        // ObjAsm wrote them.
        "FMRS" | "FMSR" | "FMRRD" | "FMDRR" | "FMRRS" | "FMSRR" => {
            one(format!("vmov{cond}"), o)
        }

        "FCMPD" => one(format!("vcmp{cond}.f64"), o),
        "FCMPS" => one(format!("vcmp{cond}.f32"), o),
        "FCMPED" => one(format!("vcmpe{cond}.f64"), o),
        "FCMPES" => one(format!("vcmpe{cond}.f32"), o),
        "FCMPZD" => one(format!("vcmp{cond}.f64 {o}, #0"), ""),
        "FCMPZS" => one(format!("vcmp{cond}.f32 {o}, #0"), ""),
        "FCMPEZD" => one(format!("vcmpe{cond}.f64 {o}, #0"), ""),
        "FCMPEZS" => one(format!("vcmpe{cond}.f32 {o}, #0"), ""),

        "FCVTDS" => one(format!("vcvt{cond}.f64.f32"), o),
        "FCVTSD" => one(format!("vcvt{cond}.f32.f64"), o),
        "FSITOD" => one(format!("vcvt{cond}.f64.s32"), o),
        "FSITOS" => one(format!("vcvt{cond}.f32.s32"), o),
        "FUITOD" => one(format!("vcvt{cond}.f64.u32"), o),
        "FUITOS" => one(format!("vcvt{cond}.f32.u32"), o),
        // Without the `Z` the rounding mode is FPSCR's, which UAL spells
        // with the R form.
        "FTOSID" => one(format!("vcvtr{cond}.s32.f64"), o),
        "FTOSIS" => one(format!("vcvtr{cond}.s32.f32"), o),
        "FTOUID" => one(format!("vcvtr{cond}.u32.f64"), o),
        "FTOUIS" => one(format!("vcvtr{cond}.u32.f32"), o),
        "FTOSIZD" => one(format!("vcvt{cond}.s32.f64"), o),
        "FTOSIZS" => one(format!("vcvt{cond}.s32.f32"), o),
        "FTOUIZD" => one(format!("vcvt{cond}.u32.f64"), o),
        "FTOUIZS" => one(format!("vcvt{cond}.u32.f32"), o),

        "FADDD" | "FADDS" => one(format!("vadd{cond}{}", size_of(stem)), o),
        "FSUBD" | "FSUBS" => one(format!("vsub{cond}{}", size_of(stem)), o),
        "FMULD" | "FMULS" => one(format!("vmul{cond}{}", size_of(stem)), o),
        "FDIVD" | "FDIVS" => one(format!("vdiv{cond}{}", size_of(stem)), o),
        "FMACD" | "FMACS" => one(format!("vmla{cond}{}", size_of(stem)), o),
        "FNMACD" | "FNMACS" => one(format!("vmls{cond}{}", size_of(stem)), o),
        "FMSCD" | "FMSCS" => one(format!("vnmls{cond}{}", size_of(stem)), o),
        "FNMSCD" | "FNMSCS" => one(format!("vnmla{cond}{}", size_of(stem)), o),
        "FABSD" | "FABSS" => one(format!("vabs{cond}{}", size_of(stem)), o),
        "FNEGD" | "FNEGS" => one(format!("vneg{cond}{}", size_of(stem)), o),
        "FSQRTD" | "FSQRTS" => one(format!("vsqrt{cond}{}", size_of(stem)), o),
        "FCPYD" | "FCPYS" => one(format!("vmov{cond}{}", size_of(stem)), o),

        "FLDD" | "FLDS" | "FSTD" | "FSTS" => transfer(stem, cond, o),
        _ => Legalized::Unsupported(format!("{stem} is VFP but not one I lower")),
    })
}

/// Lower a single-register load or store, in any of ObjAsm's addressing
/// modes.
fn transfer(stem: &str, cond: &str, o: &str) -> Legalized {
    let load = stem.starts_with("FLD");
    let width: i32 = if size_of(stem) == ".f64" { 8 } else { 4 };
    let base = if load { "vldm" } else { "vstm" };

    // A literal, as in `FLDD d0,=1`. Only the immediate forms are handled
    // here, and only those reach this function. Any value the pool must
    // hold was sent to the pool earlier, during expansion.
    if !o.contains('[') {
        if let Some((reg, expr)) = o.split_once('=') {
            return match immediate_mov(stem, cond, reg.trim_end_matches([',', ' ']).trim(), expr.trim()) {
                Some(l) => l,
                None => Legalized::Unsupported(format!(
                    "{stem} {expr} needs the literal pool"
                )),
            };
        }
        // A bare label is a program-relative load, as in
        // `PI FLDD FACC,FULLPI` where `FULLPI` labels a `DCFD`. The
        // encoder can already resolve the expression against `pc`.
        if load {
            return one(format!("vldr{cond}{}", size_of(stem)), o.to_string());
        }
        return Legalized::Unsupported(format!("{stem} {o} has no addressing mode"));
    }

    let Some((reg, rest)) = o.split_once('[').map(|(r, t)| (r.trim().trim_end_matches(',').trim(), t)) else {
        return Legalized::Unsupported(format!("{stem} {o} has no addressing mode"));
    };
    let (bracket, after) = match rest.split_once(']') {
        Some((b, a)) => (b.trim(), a.trim().replace(' ', "")),
        None => return Legalized::Unsupported(format!("{stem} {o} has an unclosed bracket")),
    };
    // `[Rn]` or `[Rn,#imm]`: the encoder takes this as it stands.
    if after.is_empty() {
        return one(
            format!("{}{cond}{}", if load { "vldr" } else { "vstr" }, size_of(stem)),
            format!("{reg}, [{bracket}]"),
        );
    }
    // Post-index `[Rn],#imm` and pre-index `[Rn,#imm]!` both become a
    // one-register multiple transfer, as ObjAsm encodes them.
    let (mode, rn) = if after == "!" {
        // Pre-indexed: the offset sits inside the bracket.
        let Some((rn, imm)) = bracket.split_once(',') else {
            return Legalized::Unsupported(format!("{stem} {o} has no offset to write back"));
        };
        let Some(imm) = parse_offset(imm) else {
            return Legalized::Unsupported(format!("{stem} {o}: '{imm}' is not an offset"));
        };
        if imm != width && imm != -width {
            return Legalized::Unsupported(format!(
                "{stem} {o}: a pre-indexed transfer moves {width} bytes, not {imm}"
            ));
        }
        (if imm < 0 { "db" } else { "ia" }, rn)
    } else if let Some(imm) = after.strip_prefix(',') {
        // Post-indexed: ObjAsm allows the positive width only.
        let Some(imm) = parse_offset(imm) else {
            return Legalized::Unsupported(format!("{stem} {o}: '{imm}' is not an offset"));
        };
        if imm != width {
            return Legalized::Unsupported(format!(
                "{stem} {o}: a post-indexed transfer moves {width} bytes, not {imm}"
            ));
        }
        ("ia", bracket)
    } else {
        return Legalized::Unsupported(format!("{stem} {o} is not an addressing I know"));
    };
    one(
        format!("{base}{mode}{cond}{}", size_of(stem)),
        format!("{}!, {{{reg}}}", rn.trim()),
    )
}

/// A literal's value, in the form the earlier number translation left it.
/// That is decimal, `0x` hexadecimal, or a plain float.
fn parse_value(text: &str) -> Option<f64> {
    let t = text.trim();
    if let Some(hex) = t.strip_prefix("0x").or_else(|| t.strip_prefix("0X")) {
        return u64::from_str_radix(hex, 16).ok().map(|v| v as f64);
    }
    t.parse::<f64>().ok()
}

/// An offset's value, in the form the earlier number translation left it.
fn parse_offset(text: &str) -> Option<i32> {
    let t = text.trim().trim_start_matches('#');
    if let Some(hex) = t.strip_prefix("0x").or_else(|| t.strip_prefix("0X")) {
        return i32::from_str_radix(hex, 16).ok();
    }
    t.parse::<i32>().ok()
}

/// `FLDD Fd,=value` as an immediate `VMOV`, where one can hold the value.
///
/// Returns `None` when the value needs the pool. That is a value that is
/// not one of VFP's eight-bit immediates, or a zero, since ObjAsm will not
/// make an immediate of zero. The exception is a double `+0.0`, which
/// ObjAsm writes as a `VMOV.I32` of zero over the whole register.
fn immediate_mov(stem: &str, cond: &str, reg: &str, expr: &str) -> Option<Legalized> {
    let double = size_of(stem) == ".f64";
    if double {
        let v = parse_value(expr)?;
        if v == 0.0 && !v.is_sign_negative() {
            return Some(one(format!("vmov{cond}.i32"), format!("{reg}, #0")));
        }
        imm8(v, true).map(|text| one(format!("vmov{cond}.f64"), format!("{reg}, #{text}")))
    } else {
        let v = parse_value(expr)?;
        let v = v as f32 as f64;
        imm8(v, false).map(|text| one(format!("vmov{cond}.f32"), format!("{reg}, #{text}")))
    }
}

/// The shortest decimal that names one of VFP's eight-bit immediates, if
/// this value is one.
///
/// The architecture's `VFPExpandImm` gives `(-1)^s * (1+m/16) * 2^e`,
/// with `m` in 0..=15 and `e` in -3..=4, for both widths. That covers
/// 0.125 to 31; anything larger goes to the literal pool. Only the decimal
/// is wanted. The encoder reads it back and works out the
/// bits itself.
fn imm8(value: f64, _double: bool) -> Option<String> {
    let sign = if value.is_sign_negative() { "-" } else { "" };
    let a = value.abs();
    if a == 0.0 || !a.is_finite() {
        return None;
    }
    for e in -3..=4i32 {
        let scaled = a * (2f64).powi(-e);
        if !(1.0..2.0).contains(&scaled) {
            continue;
        }
        let m = (scaled - 1.0) * 16.0;
        if m.fract() == 0.0 {
            // Write a decimal the encoder reads as floating point. It
            // takes `#1` for an integer and refuses it.
            let mut text = format!("{}", value.abs());
            if !text.contains(['.', 'e', 'E']) {
                text.push_str(".0");
            }
            return Some(format!("{sign}{text}"));
        }
    }
    None
}

/// The bits a pooled VFP literal holds, as whole 32-bit words.
///
/// A double takes two words of pool and a single takes one. That is why
/// the pool deals with this, not the instruction.
pub fn literal_bits(stem: &str, text: &str) -> Option<Vec<u32>> {
    if size_of(stem) == ".f64" {
        let v = parse_value(text)?;
        let b = v.to_bits();
        Some(vec![b as u32, (b >> 32) as u32])
    } else {
        let v = parse_value(text)? as f32;
        Some(vec![v.to_bits()])
    }
}

/// Is a `FLDD`/`FLDS` of a literal an immediate `VMOV`, never a pool word?
pub fn literal_is_immediate(stem: &str, text: &str) -> bool {
    let double = size_of(stem) == ".f64";
    let Some(v) = parse_value(text) else { return false };
    if double {
        (v == 0.0 && !v.is_sign_negative()) || imm8(v, true).is_some()
    } else {
        imm8(v as f32 as f64, false).is_some()
    }
}

/// A pooled literal load, as the word to emit.
///
/// The word is `VLDR Fd,[pc,#imm]`. The pool must lie ahead of the
/// instruction. The eight-bit offset counts words, so the pool must be
/// within a kilobyte. The word is built here because the encoder parses no
/// `VLDR` with `pc` written as a label expression.
pub fn pool_load_word(mnemonic: &str, operands: &str, here: u32, target: u32) -> Result<u32, String> {
    let v = parse(mnemonic).ok_or_else(|| format!("{mnemonic} is not VFP"))?;
    let stem = v.stem;
    if !matches!(stem, "FLDD" | "FLDS") {
        return Err(format!("{stem} does not load a literal"));
    }
    let reg: String = operands
        .split_once(',')
        .map(|(r, _)| r.trim().to_ascii_lowercase())
        .unwrap_or_default();
    let (d, n) = if let Some(num) = reg.strip_prefix('d') {
        let n = num.parse::<u32>().map_err(|_| format!("'{reg}' is not a VFP register"))?;
        (true, n)
    } else if let Some(num) = reg.strip_prefix('s') {
        let n = num.parse::<u32>().map_err(|_| format!("'{reg}' is not a VFP register"))?;
        (false, n)
    } else {
        return Err(format!("'{reg}' is not a VFP register"));
    };
    let delta = target as i64 - here as i64 - 8;
    if delta < 0 || delta % 4 != 0 || delta / 4 > 255 {
        return Err(format!(
            "the literal pool is {delta} bytes away; a VFP load reaches 1020, so this \
             needs an LTORG nearer the instruction"
        ));
    }
    let cond = crate::legalize::condition_bits(&v.cond)
        .ok_or_else(|| format!("{} is not a condition", v.cond))?;
    let double = size_of(stem) == ".f64";
    // A load addressed from pc: cond 1101 U D 0 1 1111 Vd 101 sz imm8.
    let word = (cond << 28)
        | (0b1101 << 24)
        | (1 << 23)
        | (1 << 20)
        | (0xF << 16)
        | ((n >> (if d { 4 } else { 1 }) & 1) << 22)
        | ((if d { n & 0xF } else { n >> 1 } & 0xF) << 12)
        | 0xA << 8
        | (u32::from(double) << 8)
        | ((delta / 4) as u32);
    Ok(word)
}

#[cfg(test)]
mod tests {
    use super::*;

    fn one_of(m: &str, o: &str) -> (String, String) {
        match convert(m, o).expect("is VFP") {
            Legalized::One(m, o) => (m, o),
            other => panic!("expected one instruction: {other:?}"),
        }
    }

    #[test]
    fn the_stems_and_conditions_parse() {
        assert_eq!(parse("FLDMIAD").unwrap().stem, "FLDMIAD");
        assert_eq!(parse("FLDMIADHS").unwrap().cond, "CS");
        assert_eq!(parse("FADDDHS").unwrap().stem, "FADDD");
        assert_eq!(parse("FMRX").unwrap().stem, "FMRX");
        assert!(parse("LDFS").is_none(), "that is the FPA spelling");
        assert!(parse("FCOMPS").is_none(), "that is a label in BASIC");
    }

    #[test]
    fn a_multiple_transfer_keeps_its_operands() {
        assert_eq!(
            one_of("FLDMIAD", "r1!, {d0-d3}"),
            ("vldmia.f64".into(), "r1!, {d0-d3}".into())
        );
        assert_eq!(
            one_of("FSTMIADHS", "r1!, {d4-d7}"),
            ("vstmiaCS.f64".into(), "r1!, {d4-d7}".into())
        );
    }

    #[test]
    fn a_post_indexed_load_becomes_a_one_register_vldm() {
        // ObjAsm encodes `FLDS S0,[SP],#4` as `vpop {s0}`. The encoder
        // takes the longer spelling of the same word.
        assert_eq!(
            one_of("FLDS", "s0, [sp], #4"),
            ("vldmia.f32".into(), "sp!, {s0}".into())
        );
        assert_eq!(
            one_of("FLDD", "d1, [r2], #8"),
            ("vldmia.f64".into(), "r2!, {d1}".into())
        );
        assert_eq!(
            one_of("FSTD", "d0, [r2], #8"),
            ("vstmia.f64".into(), "r2!, {d0}".into())
        );
        // Any other distance is not a transfer of one register.
        assert!(matches!(
            convert("FLDS", "s0, [r2], #8"),
            Some(Legalized::Unsupported(_))
        ));
    }

    #[test]
    fn a_pre_indexed_transfer_decrements_when_the_offset_is_negative() {
        assert_eq!(
            one_of("FLDD", "d1, [r4, #-8]!"),
            ("vldmdb.f64".into(), "r4!, {d1}".into())
        );
        assert_eq!(
            one_of("FSTD", "d0, [r4, #-8]!"),
            ("vstmdb.f64".into(), "r4!, {d0}".into())
        );
    }

    #[test]
    fn an_offset_addressing_passes_through() {
        assert_eq!(
            one_of("FLDD", "d1, [r2, #-8]"),
            ("vldr.f64".into(), "d1, [r2, #-8]".into())
        );
        assert_eq!(
            one_of("FSTS", "s1, [r2]"),
            ("vstr.f32".into(), "s1, [r2]".into())
        );
    }

    #[test]
    fn the_flag_read_reaches_the_psr() {
        assert_eq!(
            one_of("FMRX", "pc, FPSCR"),
            ("vmrs apsr_nzcv, fpscr".into(), "".into())
        );
        assert_eq!(
            one_of("FMRX", "r0, FPSCR"),
            ("vmrs r0, fpscr".into(), "".into())
        );
        assert_eq!(
            one_of("FMRXHS", "pc, FPSCR"),
            ("vmrsCS apsr_nzcv, fpscr".into(), "".into())
        );
        assert_eq!(
            one_of("FMXR", "FPSCR, r2"),
            ("vmsr fpscr, r2".into(), "".into())
        );
    }

    #[test]
    fn the_conversions_name_their_direction() {
        // FCVTDS: to Double, from Single.
        assert_eq!(
            one_of("FCVTDS", "d0, s2"),
            ("vcvt.f64.f32".into(), "d0, s2".into())
        );
        assert_eq!(
            one_of("FCVTSD", "s0, d0"),
            ("vcvt.f32.f64".into(), "s0, d0".into())
        );
        assert_eq!(
            one_of("FSITODPL", "d4, s0"),
            ("vcvtPL.f64.s32".into(), "d4, s0".into())
        );
        // The unsigned conversions too (#194): they were never recognised.
        assert_eq!(
            one_of("FUITOD", "d4, s0"),
            ("vcvt.f64.u32".into(), "d4, s0".into())
        );
        assert_eq!(
            one_of("FUITOSNE", "s1, s0"),
            ("vcvtNE.f32.u32".into(), "s1, s0".into())
        );
        assert_eq!(
            one_of("FTOSIZD", "s0, d0"),
            ("vcvt.s32.f64".into(), "s0, d0".into())
        );
        assert_eq!(
            one_of("FTOSID", "s0, d0"),
            ("vcvtr.s32.f64".into(), "s0, d0".into())
        );
    }

    #[test]
    fn the_data_operations_carry_their_conditions() {
        assert_eq!(
            one_of("FADDDHS", "d4, d0, d4"),
            ("vaddCS.f64".into(), "d4, d0, d4".into())
        );
        assert_eq!(
            one_of("FDIVDHS", "d4, d0, d4"),
            ("vdivCS.f64".into(), "d4, d0, d4".into())
        );
        assert_eq!(
            one_of("FMACDHS", "d4, d12, d12"),
            ("vmlaCS.f64".into(), "d4, d12, d12".into())
        );
        // Probed: ObjAsm's FNMAC is VMLS.
        assert_eq!(
            one_of("FNMACD", "d4, d5, d6"),
            ("vmls.f64".into(), "d4, d5, d6".into())
        );
        assert_eq!(
            one_of("FABSDMI", "d0, d0"),
            ("vabsMI.f64".into(), "d0, d0".into())
        );
        assert_eq!(
            one_of("FCPYDMI", "d1, d0"),
            ("vmovMI.f64".into(), "d1, d0".into())
        );
    }

    #[test]
    fn a_compare_with_zero_appends_it() {
        assert_eq!(
            one_of("FCMPZD", "d2"),
            ("vcmp.f64 d2, #0".into(), "".into())
        );
        assert_eq!(
            one_of("FCMPEZS", "s3"),
            ("vcmpe.f32 s3, #0".into(), "".into())
        );
    }

    #[test]
    fn the_core_transfers_are_all_vmov() {
        assert_eq!(
            one_of("FMRS", "r0, s1"),
            ("vmov".into(), "r0, s1".into())
        );
        assert_eq!(
            one_of("FMRRD", "r4, r5, d6"),
            ("vmov".into(), "r4, r5, d6".into())
        );
    }

    #[test]
    fn vfp_immediates_are_the_sixteen_sixteenths() {
        assert_eq!(imm8(1.0, true).as_deref(), Some("1.0"));
        assert_eq!(imm8(2.0, true).as_deref(), Some("2.0"));
        assert_eq!(imm8(0.5, true).as_deref(), Some("0.5"));
        assert_eq!(imm8(-1.0, false).as_deref(), Some("-1.0"));
        assert_eq!(imm8(0.25, false).as_deref(), Some("0.25"));
        assert_eq!(imm8(31.0, true).as_deref(), Some("31.0"));
        // 32 needs e = 5, past the top of the range: it goes to the pool
        // (#193), and so do the larger powers of two.
        assert_eq!(imm8(32.0, true), None);
        assert_eq!(imm8(64.0, true), None);
        assert_eq!(imm8(65536.0, false), None);
        assert!(!literal_is_immediate("FLDD", "64"));
        assert!(literal_is_immediate("FLDD", "31"));
        // Not one of them: 255 needs a nine-bit mantissa.
        assert_eq!(imm8(255.0, true), None);
        // Zero is deliberately not an immediate: ObjAsm pools it, except
        // the double +0.0 handled before this is asked.
        assert_eq!(imm8(0.0, true), None);
    }

    #[test]
    fn the_double_literals_objasm_makes_immediate() {
        assert_eq!(
            one_of("FLDD", "d0, =1"),
            ("vmov.f64".into(), "d0, #1.0".into())
        );
        assert_eq!(
            one_of("FLDD", "d0, =2.0"),
            ("vmov.f64".into(), "d0, #2.0".into())
        );
        assert_eq!(
            one_of("FLDD", "d0, =0"),
            ("vmov.i32".into(), "d0, #0".into())
        );
        assert_eq!(
            one_of("FLDS", "s0, =-1.0"),
            ("vmov.f32".into(), "s0, #-1.0".into())
        );
        // These go to the pool instead.
        assert!(matches!(
            convert("FLDD", "d0, =3.141592653589793"),
            Some(Legalized::Unsupported(_))
        ));
        assert!(matches!(
            convert("FLDS", "s0, =0"),
            Some(Legalized::Unsupported(_))
        ));
        assert!(matches!(
            convert("FLDD", "d0, =-0.0"),
            Some(Legalized::Unsupported(_))
        ));
    }

    #[test]
    fn pooled_literals_hold_their_bits() {
        assert_eq!(literal_bits("FLDD", "1.0").unwrap(), vec![0, 0x3FF00000]);
        assert_eq!(literal_bits("FLDD", "0").unwrap(), vec![0, 0]);
        assert_eq!(literal_bits("FLDS", "2.5").unwrap(), vec![0x40200000]);
        assert!(!literal_is_immediate("FLDD", "3.14159"));
        assert!(literal_is_immediate("FLDD", "1"));
        assert!(!literal_is_immediate("FLDS", "0"));
    }

    #[test]
    fn a_pool_load_word_is_the_vldr_objasm_writes() {
        // ObjAsm: `FLDD D0,=3.14...` at +0x18 loads from +0x28 -- word
        // 0xED9F0B08, a VLDR d0 from pc+8*4.
        let w = pool_load_word("FLDD", "d0, =x", 0x18, 0x40).unwrap();
        assert_eq!(w, 0xED9F0B08);
        // Out of the kilobyte a VLDR reaches.
        assert!(pool_load_word("FLDD", "d0, =x", 0, 0x500).is_err());
    }
}
