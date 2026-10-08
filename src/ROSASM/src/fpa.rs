//! The FPA instruction set, and what of it VFP can express.
//!
//! The sources were written for the Floating Point Accelerator: eight
//! registers `f0`-`f7`, and instructions that name the precision they work in.
//! A Cortex-A72 has no FPA. It has VFP and Advanced SIMD, so the FPA
//! instructions have to become VFP ones — where that is possible at all.
//!
//! It is not always possible, and the difference matters:
//!
//! * **Single and double** map exactly. `ADFS` rounds to single and so does
//!   `VADD.F32`; `LDFD` moves eight bytes and so does `VLDR` on a `d`
//!   register. These are translations, and 270 of the corpus's 598 FPA sites
//!   are of this kind.
//! * **Extended precision** has no VFP format at all. The FPA's `E` operations
//!   work to about 64 bits of mantissa and store 12 bytes. VFP's widest is a
//!   64-bit double with 53. Rewriting `STFE` as `VSTR` would silently drop
//!   bits. The code that uses extended precision (`fma`, `atan2`, `log1p` in
//!   the C library's `mathasm`) exists precisely so as not to drop them.
//!   Those routines need porting, not translating, so they are refused here
//!   by name.
//! * **Transcendentals** (`SIN`, `LOG`, `POW`, ...) were FPA instructions and
//!   are library calls everywhere else. There is nothing to translate to.
//! * **`LFM`/`SFM`** transfer registers in the FPA's internal 12-byte format,
//!   so they are the extended-precision problem again.
//! * **Packed decimal** (`LDFP`, `STFP`) has no equivalent in anything.
//!
//! ## Which register is `f3`?
//!
//! An FPA register holds one value, so the mapping gives each `f<n>` one
//! place to live. `f<n>` becomes `d<n>`. A single-precision operation uses
//! `s<2n>`, the low half of that same `d<n>`. Nothing collides, because
//! `s1`, `s3`, `s5` and so on are left unused. A register written as a
//! single and read as a single comes back unchanged, and that is all the
//! convertible subset does. Code that writes one precision and reads another
//! is doing a deliberate conversion. In this corpus that always involves
//! `E`, which is refused.
//!
//! ## Sizes change
//!
//! Every FPA instruction is one word, but its translation is not always one.
//! The FPA's `CMF` set the ARM flags directly. `VCMP` sets the FPSCR
//! instead, and a following `VMRS` copies the flags across. So a compare
//! becomes two instructions. The location counter has to know that before
//! the encoder ever runs. That is why `words` exists, and why layout
//! consults it during expansion.

use crate::legalize::Legalized;

/// The format an FPA instruction works in.
#[derive(Debug, Clone, Copy, PartialEq)]
pub enum Precision {
    Single,
    Double,
    /// About 64 bits of mantissa, 12 bytes in memory. No VFP equivalent.
    Extended,
    /// Packed decimal. No equivalent anywhere.
    Packed,
}

impl Precision {
    fn from_letter(c: char) -> Option<Precision> {
        match c {
            'S' => Some(Precision::Single),
            'D' => Some(Precision::Double),
            'E' => Some(Precision::Extended),
            'P' => Some(Precision::Packed),
            _ => None,
        }
    }

    /// The VFP type suffix, for the precisions VFP has.
    fn suffix(self) -> Option<&'static str> {
        match self {
            Precision::Single => Some("F32"),
            Precision::Double => Some("F64"),
            _ => None,
        }
    }

    /// Bytes one register occupies in memory.
    fn width(self) -> u32 {
        match self {
            Precision::Single => 4,
            Precision::Double => 8,
            Precision::Extended => 12,
            Precision::Packed => 12,
        }
    }
}

/// A decoded FPA mnemonic.
#[derive(Debug, Clone, PartialEq)]
pub struct Fpa {
    pub stem: &'static str,
    /// The ARM condition, uppercase and canonical, or empty.
    pub cond: String,
    pub precision: Option<Precision>,
    /// `P`, `M` or `Z` — round towards plus infinity, minus infinity or zero.
    /// VFP has no per-instruction rounding mode, so any of these is refused.
    pub rounding: Option<char>,
    /// `CMFE`/`CNFE`: raise an exception on unordered operands.
    pub exception: bool,
}

/// Every FPA stem. All are three letters long, so none can be a prefix of
/// another and their order does not matter.
const STEMS: [&str; 41] = [
    "LDF", "STF", "LFM", "SFM", "ADF", "SUF", "RSF", "MUF", "DVF", "RDF", "RMF", "FML", "FDV",
    "FRD", "POW", "RPW", "POL", "MVF", "MNF", "ABS", "RND", "SQT", "LOG", "LGN", "EXP", "SIN",
    "COS", "TAN", "ASN", "ACS", "ATN", "URD", "NRM", "CMF", "CNF", "FLT", "FIX", "RFS", "WFS",
    "RFC", "WFC",
];

/// The register-transfer instructions, which take an ARM register and no
/// precision at all.
const STATUS: [&str; 4] = ["RFS", "WFS", "RFC", "WFC"];

/// Stems with no VFP instruction to become. These were FPA operations, and
/// are library calls on every other architecture.
const TRANSCENDENTAL: [&str; 12] = [
    "POW", "RPW", "POL", "LOG", "LGN", "EXP", "SIN", "COS", "TAN", "ASN", "ACS", "ATN",
];

/// Addressing modes an `LFM`/`SFM` may carry, as `LDM`/`STM` do.
const BLOCK_MODES: [&str; 6] = ["IA", "DB", "FD", "EA", "FA", "ED"];

const CONDS: [&str; 17] = [
    "EQ", "NE", "CS", "CC", "MI", "PL", "VS", "VC", "HI", "LS", "GE", "LT", "GT", "LE", "AL",
    "HS", "LO",
];

fn canonical_cond(c: &str) -> &'static str {
    match c {
        "HS" => "CS",
        "LO" => "CC",
        "EQ" => "EQ",
        "NE" => "NE",
        "CS" => "CS",
        "CC" => "CC",
        "MI" => "MI",
        "PL" => "PL",
        "VS" => "VS",
        "VC" => "VC",
        "HI" => "HI",
        "LS" => "LS",
        "GE" => "GE",
        "LT" => "LT",
        "GT" => "GT",
        "LE" => "LE",
        _ => "AL",
    }
}

/// Split a mnemonic into stem, condition, precision and rounding.
///
/// The sources write the condition before the precision. `MVFNEE` is `MVF`,
/// conditional on `NE`, in extended precision. A rounding mode follows the
/// precision, so `ADFSP` is `ADF` in single precision, rounding to plus
/// infinity. There is one ambiguous shape, `MVFPLS`. It is resolved by
/// reading a precision first and falling back to a condition. `MVFPLS`
/// cannot be packed followed by `LS`, because `LS` is not a rounding mode.
/// So it is `PL` followed by single.
pub fn parse(mnemonic: &str) -> Option<Fpa> {
    let up = mnemonic.to_ascii_uppercase();
    let stem = STEMS.iter().find(|s| up.starts_with(**s))?;
    let rest = &up[stem.len()..];

    // The comparisons take no precision. They compare whatever the registers
    // hold. Their only suffix is `E`, and the condition may sit on either
    // side of it, because ARM's own documentation writes it both ways.
    if matches!(*stem, "CMF" | "CNF") {
        let (mut rest, mut exception) = (rest, false);
        if let Some(r) = rest.strip_prefix('E') {
            rest = r;
            exception = true;
        }
        let mut cond = String::new();
        if rest.len() >= 2 && CONDS.contains(&&rest[..2]) {
            cond = canonical_cond(&rest[..2]).to_string();
            rest = &rest[2..];
        }
        if let Some(r) = rest.strip_prefix('E') {
            rest = r;
            exception = true;
        }
        if !rest.is_empty() {
            return None;
        }
        return Some(Fpa { stem, cond, precision: None, rounding: None, exception });
    }

    // Reading and writing the FPA's status and control words takes an ARM
    // register and a condition, and nothing else.
    if STATUS.contains(stem) {
        let mut cond = String::new();
        let mut rest = rest;
        if rest.len() >= 2 && CONDS.contains(&&rest[..2]) {
            cond = canonical_cond(&rest[..2]).to_string();
            rest = &rest[2..];
        }
        if !rest.is_empty() {
            return None;
        }
        return Some(Fpa { stem, cond, precision: None, rounding: None, exception: false });
    }

    // The multiple-register transfers carry an addressing mode where the
    // others carry a precision. `SFMNEFD` is `SFM`, conditional on `NE`,
    // full-descending. So they are decoded separately. Translation refuses
    // them later; this only has to recognise them.
    if matches!(*stem, "LFM" | "SFM") {
        let mut rest = rest;
        let mut cond = String::new();
        if rest.len() >= 2 && CONDS.contains(&&rest[..2]) {
            cond = canonical_cond(&rest[..2]).to_string();
            rest = &rest[2..];
        }
        if rest.len() == 2 && BLOCK_MODES.contains(&rest) {
            rest = "";
        }
        if !rest.is_empty() {
            return None;
        }
        return Some(Fpa { stem, cond, precision: None, rounding: None, exception: false });
    }

    // A precision then a rounding mode, either of which may be absent.
    // `ADFSP` is single, rounding to plus infinity. `STFP` is packed. `FIXZ`
    // is a rounding mode with no precision at all, because a fix has no
    // format to choose. Reading the precision first is what makes `P` mean
    // packed in `STFP` and part of a condition in `MVFPLS`.
    let split = |r: &str| -> Option<(Option<Precision>, Option<char>)> {
        let is_round = |c: char| matches!(c, 'P' | 'M' | 'Z');
        let mut cs = r.chars().peekable();
        let precision = match cs.peek() {
            Some(c) => match Precision::from_letter(*c) {
                Some(p) => {
                    cs.next();
                    Some(p)
                }
                None if is_round(*c) => None,
                None => return None,
            },
            None => None,
        };
        let rounding = match cs.next() {
            Some(c) if is_round(c) => Some(c),
            Some(_) => return None,
            None => None,
        };
        if cs.next().is_some() {
            return None;
        }
        Some((precision, rounding))
    };

    // No condition.
    if let Some((precision, rounding)) = split(rest) {
        return Some(Fpa {
            stem,
            cond: String::new(),
            precision,
            rounding,
            exception: false,
        });
    }
    // Condition, then precision.
    if rest.len() >= 2 && CONDS.contains(&&rest[..2]) {
        if let Some((precision, rounding)) = split(&rest[2..]) {
            return Some(Fpa {
                stem,
                cond: canonical_cond(&rest[..2]).to_string(),
                precision,
                rounding,
                exception: false,
            });
        }
    }
    None
}

/// Is this mnemonic an FPA instruction?
pub fn is_fpa(mnemonic: &str) -> bool {
    parse(mnemonic).is_some()
}

/// How many instructions this becomes, for the location counter.
///
/// Layout has to agree with what the encoder is eventually handed, and a
/// compare becomes two instructions. Anything refused still takes up its one
/// word. So a routine full of extended-precision arithmetic keeps every
/// label around it at the right address.
pub fn words(mnemonic: &str, to_vfp: bool) -> Option<u32> {
    let f = parse(mnemonic)?;
    Some(match f.stem {
        // Converted to VFP, a compare is two instructions. VCMP leaves its
        // result in the FPSCR, and VMRS brings it to the ARM flags. Left as
        // FPA, a compare is one word like any other. Counting two there
        // moved every label after it.
        "CMF" | "CNF" if to_vfp => 2,
        _ => 1,
    })
}

// ------------------------------------------------------------- registers

/// The VFP register an FPA register becomes, at this precision.
///
/// `f3` is `d3`. `f3` in single precision is `s6`, the low half of that
/// same `d3`. So each FPA register has one place to live.
fn map_register(name: &str, p: Precision) -> Option<String> {
    let n: u32 = name.trim().strip_prefix(['f', 'F'])?.parse().ok()?;
    if n > 7 {
        return None;
    }
    match p {
        Precision::Single => Some(format!("s{}", n * 2)),
        Precision::Double => Some(format!("d{n}")),
        _ => None,
    }
}

/// The double register that covers an FPA register, whatever its
/// precision. It is used for the NEON immediate that zeroes the register,
/// and for a compare, whose operands carry no precision.
fn covering_double(name: &str) -> Option<String> {
    let n: u32 = name.trim().strip_prefix(['f', 'F'])?.parse().ok()?;
    (n <= 7).then(|| format!("d{n}"))
}

/// The FPA's eight immediate constants.
///
/// They are the only ones the instruction set can encode, and VFP's own
/// 8-bit floating-point immediate happens to cover all but zero.
fn map_immediate(text: &str) -> Option<String> {
    let t = text.trim().trim_start_matches('#').trim();
    if t == "0.5" {
        return Some("0.5".into());
    }
    // By the time an operand reaches here the expression evaluator has already
    // reduced it, so `#1` arrives as `#0x1`.
    let n = match t.strip_prefix("0x").or_else(|| t.strip_prefix("0X")) {
        Some(h) => u32::from_str_radix(h, 16).ok()?,
        None => t.trim_end_matches(".0").parse::<u32>().ok()?,
    };
    match n {
        0 => Some("0".into()),
        1..=5 | 10 => Some(format!("{n}.0")),
        _ => None,
    }
}

// ------------------------------------------------------------ conversion

/// What a dyadic or monadic FPA operation becomes.
///
/// `RSF` and `RDF` are the reversed forms — `Fd := Fm - Fn`, `Fd := Fm / Fn` —
/// so they become the ordinary VFP instruction with the operands swapped.
struct DataOp {
    vfp: &'static str,
    /// Reversed: the second and third operands change places.
    reversed: bool,
}

fn data_op(stem: &str) -> Option<DataOp> {
    let (vfp, reversed) = match stem {
        "ADF" => ("VADD", false),
        "SUF" => ("VSUB", false),
        "RSF" => ("VSUB", true),
        "MUF" | "FML" => ("VMUL", false),
        "DVF" | "FDV" => ("VDIV", false),
        "RDF" | "FRD" => ("VDIV", true),
        _ => return None,
    };
    Some(DataOp { vfp, reversed })
}

fn monadic_op(stem: &str) -> Option<&'static str> {
    match stem {
        "MVF" => Some("VMOV"),
        "MNF" => Some("VNEG"),
        "ABS" => Some("VABS"),
        "SQT" => Some("VSQRT"),
        _ => None,
    }
}

fn refuse(what: &str, why: &str) -> Legalized {
    Legalized::Unsupported(format!("{what}: {why}"))
}

/// Translate one FPA instruction, or say why it cannot be.
///
/// Returns `None` if this is not an FPA instruction at all.
pub fn convert(mnemonic: &str, operands: &str) -> Option<Legalized> {
    let f = parse(mnemonic)?;
    let up = mnemonic.to_ascii_uppercase();

    if TRANSCENDENTAL.contains(&f.stem) {
        return Some(refuse(
            &up,
            "an FPA transcendental; VFP has no such instruction, so this needs \
             a library call rather than a translation",
        ));
    }
    if matches!(f.stem, "LFM" | "SFM") {
        return Some(refuse(
            &up,
            "transfers registers in the FPA's 12-byte internal format, which \
             VFP has no equivalent of",
        ));
    }
    if matches!(f.stem, "FLT" | "FIX" | "URD" | "NRM" | "RMF" | "RND") {
        return Some(refuse(&up, "has no single-instruction VFP equivalent"));
    }

    // --- the status and control words --------------------------------------
    if STATUS.contains(&f.stem) {
        let rd = parts_first(operands);
        let cond = &f.cond;
        return Some(match f.stem {
            // The FPA's status word and VFP's FPSCR agree in the bits the
            // sources use. Bits 0-4 are the cumulative exception flags in the
            // same order: IVO/DVZ/OFL/UFL/INX against IOC/DZC/OFC/UFC/IXC.
            // Bits 16-20 are their trap enables. Above that they disagree.
            // The FPA put a system ID in the top byte, where VFP puts N, Z, C
            // and V. So this is right for the flag handling the sources do,
            // and wrong for anything that reads the whole word as an FPA one.
            "RFS" => Legalized::One(format!("VMRS{cond}"), format!("{rd}, fpscr")),
            "WFS" => Legalized::One(format!("VMSR{cond}"), format!("fpscr, {rd}")),
            // The control word was the FPA's own privileged state, describing
            // hardware VFP does not have.
            _ => refuse(
                &up,
                "reads or writes the FPA's control word, which describes \
                 hardware VFP does not have",
            ),
        });
    }
    match f.precision {
        Some(Precision::Extended) => {
            return Some(refuse(
                &up,
                "is extended precision: about 64 bits of mantissa against VFP's \
                 53, so translating it would quietly lose the accuracy the code \
                 exists to keep",
            ))
        }
        Some(Precision::Packed) => {
            return Some(refuse(&up, "is packed decimal, which VFP has no format for"))
        }
        _ => {}
    }

    // This is tested after the precision. So a mnemonic that is both
    // extended and rounded is reported as extended. That is the more
    // fundamental problem, and it groups the mnemonic with the rest of the
    // extended-precision code.
    if f.rounding.is_some() {
        return Some(refuse(
            &up,
            "names a rounding mode; VFP takes its rounding from the FPSCR, not \
             from the instruction",
        ));
    }

    let cond = f.cond.clone();
    let parts = crate::layout::split_top_level(operands);
    let arg = |i: usize| parts.get(i).map(|s| s.trim().to_string()).unwrap_or_default();

    // --- comparisons: VCMP then bring the flags across ---------------------
    if matches!(f.stem, "CMF" | "CNF") {
        if f.stem == "CNF" {
            return Some(refuse(
                &up,
                "compares against the negation of its operand, which VFP would \
                 need a separate VNEG for",
            ));
        }
        // The operands carry no precision of their own, so the registers are
        // compared as the doubles they live in.
        let (Some(a), b) = (covering_double(&arg(0)), arg(1)) else {
            return Some(refuse(&up, "operands are not FPA registers"));
        };
        let rhs = match covering_double(&b) {
            Some(r) => r,
            None => match map_immediate(&b) {
                // VCMP against zero is the only immediate it takes.
                Some(v) if v == "0" => "#0".to_string(),
                _ => return Some(refuse(&up, "VCMP compares registers, or against zero")),
            },
        };
        let op = if f.exception { "VCMPE" } else { "VCMP" };
        return Some(Legalized::Many(vec![
            (format!("{op}{cond}.F64"), format!("{a}, {rhs}")),
            (format!("VMRS{cond}"), "APSR_nzcv, fpscr".to_string()),
        ]));
    }

    // Everything past here needs a precision VFP has.
    let Some(p) = f.precision else {
        return Some(refuse(&up, "carries no precision suffix"));
    };
    let Some(ty) = p.suffix() else {
        return Some(refuse(&up, "is not a precision VFP has"));
    };

    // --- loads and stores --------------------------------------------------
    if matches!(f.stem, "LDF" | "STF") {
        let Some(rd) = map_register(&arg(0), p) else {
            return Some(refuse(&up, "first operand is not an FPA register"));
        };
        let addr = operands
            .split_once(',')
            .map(|(_, rest)| rest.trim().to_string())
            .unwrap_or_default();
        return Some(transfer(f.stem == "LDF", &cond, &rd, &addr, p.width(), &up));
    }

    // --- monadic: a destination and one source or an immediate -------------
    if let Some(op) = monadic_op(f.stem) {
        let Some(rd) = map_register(&arg(0), p) else {
            return Some(refuse(&up, "destination is not an FPA register"));
        };
        let src = arg(1);
        if let Some(rm) = map_register(&src, p) {
            return Some(Legalized::One(
                format!("{op}{cond}.{ty}"),
                format!("{rd}, {rm}"),
            ));
        }
        // `MVF Fd, #1` is the only immediate form.
        if op == "VMOV" {
            let Some(v) = map_immediate(&src) else {
                return Some(refuse(&up, "not one of the FPA's eight immediates"));
            };
            if v == "0" {
                // VFP's floating-point immediate cannot encode zero, but the
                // Advanced SIMD one can. Clearing the whole `d` register is
                // safe, because this mapping leaves the odd `s` registers
                // unused.
                let Some(dd) = covering_double(&arg(0)) else {
                    return Some(refuse(&up, "destination is not an FPA register"));
                };
                let width = if p == Precision::Single { "I32" } else { "I64" };
                return Some(Legalized::One(
                    format!("VMOV{cond}.{width}"),
                    format!("{dd}, #0"),
                ));
            }
            return Some(Legalized::One(
                format!("VMOV{cond}.{ty}"),
                format!("{rd}, #{v}"),
            ));
        }
        return Some(refuse(&up, "source is not an FPA register"));
    }

    // --- dyadic ------------------------------------------------------------
    if let Some(d) = data_op(f.stem) {
        let (Some(rd), Some(rn)) = (map_register(&arg(0), p), map_register(&arg(1), p)) else {
            return Some(refuse(&up, "operands are not FPA registers"));
        };
        let third = arg(2);
        let Some(rm) = map_register(&third, p) else {
            // The FPA allows an immediate here; VFP does not.
            return Some(refuse(
                &up,
                "VFP arithmetic takes no immediate operand, so this needs the \
                 constant in a register first",
            ));
        };
        let (a, b) = if d.reversed { (rm, rn) } else { (rn, rm) };
        return Some(Legalized::One(
            format!("{}{cond}.{ty}", d.vfp),
            format!("{rd}, {a}, {b}"),
        ));
    }

    Some(refuse(&up, "not translated"))
}

/// A single load or store, given an FPA addressing mode.
///
/// `VLDR` and `VSTR` take an offset and nothing else. They have no
/// writeback and no post-indexing. So the indexed forms become
/// `VLDM`/`VSTM`, which do have writeback. That covers the FPA's stack
/// idioms exactly, because they always step by the transfer's own width.
fn transfer(load: bool, cond: &str, rd: &str, addr: &str, width: u32, up: &str) -> Legalized {
    let a = addr.trim();
    let plain = format!("{}{cond}", if load { "VLDR" } else { "VSTR" });

    // `[Rn], #n` -- post-indexed.
    if let Some((base, off)) = a.split_once("],") {
        let base = base.trim_start_matches('[').trim();
        let n = parse_offset(off);
        if n == Some(width as i64) {
            let op = if load { "VLDMIA" } else { "VSTMIA" };
            return Legalized::One(format!("{op}{cond}"), format!("{base}!, {{{rd}}}"));
        }
        return Legalized::Unsupported(format!(
            "{up}: post-indexed by {} where VFP can only step by the transfer's \
             own {width} bytes",
            off.trim()
        ));
    }

    // `[Rn, #n]!` -- pre-indexed with writeback.
    if let Some(inner) = a.strip_suffix("]!") {
        let inner = inner.trim_start_matches('[');
        let (base, off) = inner.split_once(',').unwrap_or((inner, "#0"));
        let n = parse_offset(off);
        if n == Some(-(width as i64)) {
            let op = if load { "VLDMDB" } else { "VSTMDB" };
            return Legalized::One(
                format!("{op}{cond}"),
                format!("{}!, {{{rd}}}", base.trim()),
            );
        }
        return Legalized::Unsupported(format!(
            "{up}: pre-indexed by {} where VFP can only step by the transfer's \
             own {width} bytes",
            off.trim()
        ));
    }

    // `[Rn, #n]` or `[Rn]` or a label: VLDR takes these unchanged.
    Legalized::One(plain, format!("{rd}, {a}"))
}

/// The first operand, trimmed.
fn parts_first(operands: &str) -> String {
    operands
        .split(',')
        .next()
        .unwrap_or("")
        .trim()
        .to_string()
}

/// The signed byte offset an addressing operand carries, if it is a constant.
fn parse_offset(text: &str) -> Option<i64> {
    let t = text.trim().trim_end_matches([']', '!']).trim();
    let t = t.trim_start_matches('#').trim();
    let (neg, digits) = match t.strip_prefix('-') {
        Some(d) => (true, d.trim()),
        None => (false, t),
    };
    let v = match digits.strip_prefix("0x").or_else(|| digits.strip_prefix("0X")) {
        Some(h) => i64::from_str_radix(h, 16).ok()?,
        None => digits.parse::<i64>().ok()?,
    };
    Some(if neg { -v } else { v })
}

#[cfg(test)]
mod tests {
    use super::*;

    fn one(m: &str, o: &str) -> (String, String) {
        match convert(m, o) {
            Some(Legalized::One(a, b)) => (a, b),
            other => panic!("{m} {o}: expected one instruction, got {other:?}"),
        }
    }

    fn why(m: &str, o: &str) -> String {
        match convert(m, o) {
            Some(Legalized::Unsupported(w)) => w,
            other => panic!("{m} {o}: expected a refusal, got {other:?}"),
        }
    }

    // ---- parsing --------------------------------------------------------

    #[test]
    fn a_plain_mnemonic_parses() {
        let f = parse("LDFD").unwrap();
        assert_eq!(f.stem, "LDF");
        assert_eq!(f.precision, Some(Precision::Double));
        assert!(f.cond.is_empty());
    }

    #[test]
    fn the_condition_comes_before_the_precision() {
        // `MVFNEE` is MVF, conditional on NE, extended.
        let f = parse("MVFNEE").unwrap();
        assert_eq!((f.stem, f.cond.as_str()), ("MVF", "NE"));
        assert_eq!(f.precision, Some(Precision::Extended));
    }

    #[test]
    fn pl_is_a_condition_not_a_packed_rounding_mode() {
        // The one ambiguous shape: `MVFPLS` cannot be packed then `LS`,
        // because `LS` is not a rounding mode.
        let f = parse("MVFPLS").unwrap();
        assert_eq!((f.stem, f.cond.as_str()), ("MVF", "PL"));
        assert_eq!(f.precision, Some(Precision::Single));
        // Whereas a bare P really is packed.
        assert_eq!(parse("STFP").unwrap().precision, Some(Precision::Packed));
    }

    #[test]
    fn a_rounding_mode_follows_the_precision() {
        let f = parse("ADFSP").unwrap();
        assert_eq!(f.precision, Some(Precision::Single));
        assert_eq!(f.rounding, Some('P'));
    }

    #[test]
    fn a_comparison_takes_no_precision() {
        assert_eq!(parse("CMF").unwrap().precision, None);
        assert_eq!(parse("CMFNE").unwrap().cond, "NE");
        assert!(parse("CMFE").unwrap().exception);
    }

    #[test]
    fn an_alternate_condition_spelling_is_canonicalised() {
        assert_eq!(parse("LDFLOD").unwrap().cond, "CC");
        assert_eq!(parse("LDFHSD").unwrap().cond, "CS");
    }

    #[test]
    fn ordinary_arm_mnemonics_are_not_fpa() {
        for m in ["MOV", "LDR", "STR", "ADD", "B", "BL", "SWI", "LDMFD"] {
            assert!(!is_fpa(m), "{m} must not parse as FPA");
        }
    }

    // ---- registers ------------------------------------------------------

    #[test]
    fn an_fpa_register_has_one_place_to_live() {
        // f3 is d3, and f3 as a single is the low half of that same d3.
        assert_eq!(map_register("f3", Precision::Double).unwrap(), "d3");
        assert_eq!(map_register("f3", Precision::Single).unwrap(), "s6");
        // So no two FPA registers can collide: the odd s registers are unused.
        let mut used: Vec<String> = Vec::new();
        for n in 0..8 {
            used.push(map_register(&format!("f{n}"), Precision::Single).unwrap());
            used.push(map_register(&format!("f{n}"), Precision::Double).unwrap());
        }
        assert_eq!(used.len(), 16);
        // s2n lies inside dn and nowhere else.
        assert_eq!(map_register("f0", Precision::Single).unwrap(), "s0");
        assert_eq!(map_register("f1", Precision::Single).unwrap(), "s2");
    }

    #[test]
    fn there_are_only_eight_fpa_registers() {
        assert!(map_register("f8", Precision::Double).is_none());
        assert!(map_register("r0", Precision::Double).is_none());
    }

    // ---- data operations ------------------------------------------------

    #[test]
    fn arithmetic_maps_by_precision() {
        assert_eq!(one("ADFD", "f0, f1, f2"), ("VADD.F64".into(), "d0, d1, d2".into()));
        assert_eq!(one("ADFS", "f0, f1, f2"), ("VADD.F32".into(), "s0, s2, s4".into()));
        assert_eq!(one("MUFD", "f0, f0, f1"), ("VMUL.F64".into(), "d0, d0, d1".into()));
        assert_eq!(one("DVFS", "f1, f2, f3"), ("VDIV.F32".into(), "s2, s4, s6".into()));
    }

    #[test]
    fn the_reversed_forms_swap_their_operands() {
        // RSF is Fd := Fm - Fn, so VSUB takes them the other way round.
        assert_eq!(one("RSFD", "f0, f1, f2"), ("VSUB.F64".into(), "d0, d2, d1".into()));
        assert_eq!(one("RDFD", "f0, f1, f2"), ("VDIV.F64".into(), "d0, d2, d1".into()));
        // And the forward ones do not.
        assert_eq!(one("SUFD", "f0, f1, f2"), ("VSUB.F64".into(), "d0, d1, d2".into()));
    }

    #[test]
    fn moves_negations_and_roots() {
        assert_eq!(one("MVFD", "f0, f1"), ("VMOV.F64".into(), "d0, d1".into()));
        assert_eq!(one("MNFS", "f0, f1"), ("VNEG.F32".into(), "s0, s2".into()));
        assert_eq!(one("ABSD", "f0, f0"), ("VABS.F64".into(), "d0, d0".into()));
        assert_eq!(one("SQTD", "f0, f1"), ("VSQRT.F64".into(), "d0, d1".into()));
    }

    #[test]
    fn a_condition_carries_onto_the_vfp_instruction() {
        assert_eq!(one("MVFMID", "f0, f1"), ("VMOVMI.F64".into(), "d0, d1".into()));
        assert_eq!(one("ADFNES", "f0, f1, f2"), ("VADDNE.F32".into(), "s0, s2, s4".into()));
    }

    #[test]
    fn the_fpa_immediates_map_except_zero() {
        assert_eq!(one("MVFD", "f0, #1"), ("VMOV.F64".into(), "d0, #1.0".into()));
        assert_eq!(one("MVFS", "f0, #0.5"), ("VMOV.F32".into(), "s0, #0.5".into()));
        // VFP's floating-point immediate cannot encode zero; the SIMD one can,
        // and clearing the whole d register is safe under this mapping.
        assert_eq!(one("MVFD", "f2, #0"), ("VMOV.I64".into(), "d2, #0".into()));
        assert_eq!(one("MVFS", "f2, #0"), ("VMOV.I32".into(), "d2, #0".into()));
        // Anything outside the FPA's eight is not an FPA immediate at all.
        assert!(why("MVFD", "f0, #7").contains("eight immediates"));
        // And they arrive already evaluated, in hex.
        assert_eq!(one("MVFD", "f0, #0x1"), ("VMOV.F64".into(), "d0, #1.0".into()));
        assert_eq!(one("MVFD", "f0, #0xA"), ("VMOV.F64".into(), "d0, #10.0".into()));
    }

    // ---- transfers ------------------------------------------------------

    #[test]
    fn a_plain_offset_load_is_a_vldr() {
        assert_eq!(one("LDFD", "f0, [sp, #8]"), ("VLDR".into(), "d0, [sp, #8]".into()));
        assert_eq!(one("STFS", "f1, [r4]"), ("VSTR".into(), "s2, [r4]".into()));
        // A label works the same way.
        assert_eq!(one("LDFD", "f0, .+136"), ("VLDR".into(), "d0, .+136".into()));
    }

    #[test]
    fn the_stack_idioms_become_writeback_transfers() {
        // VLDR has no writeback, but VLDM does, and the FPA forms always step
        // by exactly the transfer's width.
        assert_eq!(one("LDFD", "f0, [sp], #8"), ("VLDMIA".into(), "sp!, {d0}".into()));
        assert_eq!(one("STFD", "f0, [sp, #-8]!"), ("VSTMDB".into(), "sp!, {d0}".into()));
        assert_eq!(one("LDFS", "f0, [sp], #4"), ("VLDMIA".into(), "sp!, {s0}".into()));
        assert_eq!(one("STFS", "f1, [r4, #-4]!"), ("VSTMDB".into(), "r4!, {s2}".into()));
    }

    #[test]
    fn an_indexed_step_of_the_wrong_size_is_refused() {
        // Nothing in VFP steps a base register by an arbitrary amount.
        assert!(why("LDFD", "f0, [sp], #16").contains("post-indexed"));
        assert!(why("STFD", "f0, [sp, #-24]!").contains("pre-indexed"));
    }

    // ---- comparisons ----------------------------------------------------

    #[test]
    fn a_compare_becomes_two_instructions() {
        // The FPA set the ARM flags; VFP sets the FPSCR and VMRS copies across.
        let Some(Legalized::Many(v)) = convert("CMF", "f0, f1") else {
            panic!("expected an expansion")
        };
        assert_eq!(v.len(), 2);
        assert_eq!(v[0], ("VCMP.F64".into(), "d0, d1".into()));
        assert_eq!(v[1], ("VMRS".into(), "APSR_nzcv, fpscr".into()));
        // And layout has to know that before the encoder runs.
        assert_eq!(words("CMF", true), Some(2));
        assert_eq!(words("CMF", false), Some(1));
        assert_eq!(words("LDFD", true), Some(1));
    }

    #[test]
    fn a_conditional_compare_carries_its_condition_to_both_halves() {
        let Some(Legalized::Many(v)) = convert("CMFNE", "f0, f1") else { panic!() };
        assert_eq!(v[0].0, "VCMPNE.F64");
        assert_eq!(v[1].0, "VMRSNE", "the flag move must be conditional too");
    }

    #[test]
    fn the_exception_raising_compare_uses_vcmpe() {
        let Some(Legalized::Many(v)) = convert("CMFE", "f0, f1") else { panic!() };
        assert_eq!(v[0].0, "VCMPE.F64");
    }

    #[test]
    fn a_compare_against_zero_is_the_one_immediate_vcmp_takes() {
        let Some(Legalized::Many(v)) = convert("CMF", "f0, #0") else { panic!() };
        assert_eq!(v[0], ("VCMP.F64".into(), "d0, #0".into()));
        assert!(why("CMF", "f0, #1").contains("or against zero"));
    }

    // ---- what cannot be translated --------------------------------------

    #[test]
    fn extended_precision_is_refused_and_says_why() {
        for m in ["LDFE", "STFE", "ADFE", "MUFE", "MVFE", "SQTE"] {
            let w = why(m, "f0, f1, f2");
            assert!(w.contains("extended precision"), "{m}: {w}");
            assert!(w.contains("53"), "{m} must say what is lost: {w}");
        }
    }

    #[test]
    fn packed_decimal_is_refused() {
        assert!(why("STFP", "f0, [sp]").contains("packed decimal"));
    }

    #[test]
    fn the_transcendentals_are_refused_as_library_calls() {
        for m in ["SIND", "COSD", "LOGD", "LGNE", "EXPD", "POWVCD", "ATND"] {
            let w = why(m, "f0, f1");
            assert!(w.contains("library call"), "{m}: {w}");
        }
    }

    #[test]
    fn multiple_register_transfers_are_refused() {
        for m in ["LFMFD", "SFMFD", "LFM", "SFM"] {
            assert!(why(m, "f0, 4, [sp]").contains("12-byte"), "{m}");
        }
    }

    #[test]
    fn a_named_rounding_mode_is_refused() {
        let w = why("ADFSZ", "f0, f1, f2");
        assert!(w.contains("rounding mode"), "{w}");
        assert!(w.contains("FPSCR"), "{w}");
    }

    #[test]
    fn arithmetic_with_an_immediate_is_refused() {
        // The FPA takes one; VFP does not.
        assert!(why("ADFD", "f0, f1, #1").contains("no immediate operand"));
    }

    #[test]
    fn a_negated_compare_is_refused() {
        assert!(why("CNF", "f0, f1").contains("negation"));
    }

    #[test]
    fn everything_refused_still_occupies_its_word() {
        // Otherwise every label after a refused instruction would move.
        for m in ["LDFE", "STFP", "SIND", "LFMFD", "FLTD", "FIXZ"] {
            assert_eq!(words(m, true), Some(1), "{m}");
        }
    }

    #[test]
    fn the_status_word_moves_through_the_fpscr() {
        assert_eq!(one("RFS", "r2"), ("VMRS".into(), "r2, fpscr".into()));
        assert_eq!(one("WFS", "r3"), ("VMSR".into(), "fpscr, r3".into()));
        assert_eq!(one("RFSNE", "r0"), ("VMRSNE".into(), "r0, fpscr".into()));
        assert_eq!(words("RFS", true), Some(1));
    }

    #[test]
    fn the_control_word_is_refused() {
        // It described the FPA's own hardware, which is not there.
        assert!(why("RFC", "r0").contains("control word"));
        assert!(why("WFC", "r0").contains("control word"));
    }

    #[test]
    fn a_non_fpa_mnemonic_converts_to_nothing() {
        assert!(convert("MOV", "r0, #1").is_none());
        assert!(words("MOV", false).is_none());
    }

    // ---- encoding the constants -----------------------------------------

    fn word(m: &str, o: &str) -> u32 {
        match encode(m, o) {
            Some(Legalized::RawWord(w)) => w,
            other => panic!("{m} {o}: expected a word, got {other:?}"),
        }
    }

    /// A post-indexed transfer's offset is an operand of its own, after the
    /// bracket. It was once being dropped, and the writeback with it.
    #[test]
    fn post_indexed_transfers_keep_their_offset() {
        // LFM f4, 4, [sp], #48: P clear, U and W set, 12 words.
        let w = word("LFM", "f4, 4, [sp], #48");
        assert_eq!((w >> 24 & 1, w >> 23 & 1, w >> 21 & 1, w & 0xFF), (0, 1, 1, 12));
        // SFM f4, 4, [sp, #-48]!: P, W set, U clear.
        let w = word("SFM", "f4, 4, [sp, #-48]!");
        assert_eq!((w >> 24 & 1, w >> 23 & 1, w >> 21 & 1, w & 0xFF), (1, 0, 1, 12));
        // LDFD f0, [r1], #8 and STFS f2, [r3], #-4.
        let w = word("LDFD", "f0, [r1], #8");
        assert_eq!((w >> 24 & 1, w >> 23 & 1, w >> 21 & 1, w & 0xFF), (0, 1, 1, 2));
        let w = word("STFS", "f2, [r3], #-4");
        assert_eq!((w >> 24 & 1, w >> 23 & 1, w >> 21 & 1, w & 0xFF), (0, 0, 1, 1));
    }

    fn encoding_refused(m: &str, o: &str) -> bool {
        matches!(encode(m, o), Some(Legalized::Unsupported(_)))
    }

    #[test]
    fn an_immediate_encodes_the_same_however_the_source_spelled_it() {
        // `#0` is an integer expression, so it reaches the encoder folded to
        // eight hex digits. `#0.0` is not an integer, so it arrives as
        // written. They are the same instruction and must be the same word.
        for (folded, written) in [
            ("f0,#00000000", "f0,#0.0"),
            ("f0,#0x0", "f0,#0.0"),
            ("f0,#00000005", "f0,#5.0"),
            ("f0,#0000000A", "f0,#10.0"),
        ] {
            assert_eq!(word("MVFD", folded), word("MVFD", written), "{folded}");
        }
    }

    #[test]
    fn comparing_against_zero_is_the_constant_not_register_one() {
        // The constants share their field with the register numbers. They
        // are told apart by the bit above them. Losing that bit turns
        // `CMF f0,#0` into `CMF f0,f0`, an instruction that assembles and
        // is wrong.
        assert_eq!(word("CMF", "f0,#00000000"), 0xEE90F118);
        assert_eq!(word("CMF", "f0,f1"), 0xEE90F111);
    }

    #[test]
    fn a_value_that_is_not_one_of_the_eight_is_refused() {
        // Six and seven are not FPA constants, and hex 10 is sixteen. A wrong
        // constant gives a wrong instruction. A refusal gives only a zero
        // word.
        for v in ["f0,#00000006", "f0,#00000007", "f0,#00000010", "f0,#000003E8"] {
            assert!(encoding_refused("MVFD", v), "{v} must not encode");
        }
    }

    #[test]
    fn a_float_literal_becomes_the_bits_a_pool_can_hold() {
        // The three the corpus actually writes.
        assert_eq!(literal_bits(Precision::Single, "-0.0").unwrap(), "0x80000000");
        assert_eq!(literal_bits(Precision::Single, "5729.57795").unwrap(), "0x45B30CA0");
        assert_eq!(literal_bits(Precision::Single, "-1000").unwrap(), "0xC47A0000");
        // Negative zero is the reason one of them is tested. It is not the
        // same word as zero, and reading the text as an integer loses it.
        assert_ne!(
            literal_bits(Precision::Single, "-0.0"),
            literal_bits(Precision::Single, "0.0")
        );
    }

    #[test]
    fn a_literal_wider_than_a_pool_word_is_refused() {
        // A pool reserves one word. A double needs two and an extended three,
        // so these are refused rather than truncated to a different value.
        assert!(literal_bits(Precision::Double, "1.5").is_none());
        assert!(literal_bits(Precision::Extended, "1.5").is_none());
        // And something that is not a number at all is not a literal.
        assert!(literal_bits(Precision::Single, "SomeSymbol").is_none());
    }

    #[test]
    fn a_label_load_reaches_through_pc() {
        // As in `LDFE f1, SqrtHalf` in mathasm. The expander folds a label
        // in this area to a distance from the instruction. `pc` reads eight
        // bytes past the instruction, so eight comes off the distance.
        assert_eq!(word("LDFD", "f0, .+36"), word("LDFD", "f0, [pc, #28]"));
        assert_eq!(word("LDFD", "f0, .-8"), word("LDFD", "f0, [pc, #-16]"));
        assert_eq!(word("LDFS", "f2, .+8"), word("LDFS", "f2, [pc, #0]"));
        // A load of what follows the instruction is the one that reads as
        // zero, and it is a real offset rather than a missing one.
        assert_eq!(word("LDFE", "f1, .+8"), 0xEDDF1100);
    }

    #[test]
    fn a_label_beyond_the_fpas_reach_is_refused() {
        // The offset is eight bits counting words. That is 1020 bytes either
        // way, a quarter of what an ordinary LDR reaches. Wrapping it would
        // load the wrong address.
        assert!(encoding_refused("LDFD", "f0, .+1036"));
        assert!(encoding_refused("LDFD", "f0, .-1020"));
        // The edge itself still encodes, either way.
        assert!(!encoding_refused("LDFD", "f0, .+1028"));
        assert!(!encoding_refused("LDFD", "f0, .-1012"));
        // And an address the offset cannot name exactly is refused, not
        // rounded to one it can.
        assert!(encoding_refused("LDFD", "f0, .+10"));
    }

    #[test]
    fn a_bare_decimal_is_not_read_as_hex() {
        // `10` is ten as the source writes it, and sixteen once folded. So
        // only spellings that make clear which one they are get a value.
        assert_eq!(word("MVFD", "f0,#10"), word("MVFD", "f0,#10.0"));
        assert_eq!(word("MVFD", "f0,#0000000A"), word("MVFD", "f0,#10.0"));
        assert!(encoding_refused("MVFD", "f0,#00000010"));
    }
}

// ---------------------------------------------------------------- encoding

// Encoding, rather than translating.
//
// An FPA instruction on this processor is not executed at all. There is no
// coprocessor 1 or 2, so the word takes the undefined-instruction trap.
// FPEmulator, which this ROM contains, then reads the word back out of the
// instruction stream as data and interprets it:
//
// ```text
//     SUB     Rtmp2,LR,#4           ;Point at bouncing instruction
//     LDREQT  Rins,[Rtmp2]          ;Get instruction, taking care about
//     LDRNE   Rins,[Rtmp2]          ; user mode, and advance pointer
// ```
//
// So the word has to be the word the source asked for, bit for bit, since
// the emulator decodes the same fields ObjAsm encoded. The field positions
// below are FPEmulator's own, from `HWSupport/FPASC/coresrc/s/fpadefs`.

/// Where each field sits, named as FPEmulator names them.
mod field {
    pub const COND: u32 = 28;
    pub const COPROC: u32 = 8;
    /// CPDT (coprocessor data transfer): the offset, counted in words.
    pub const DT_OFFSET: u32 = 0;
    pub const DT_FD: u32 = 12;
    pub const DT_PR2: u32 = 15;
    pub const DT_RN: u32 = 16;
    pub const DT_LOAD: u32 = 20;
    pub const DT_WRITEBACK: u32 = 21;
    pub const DT_PR1: u32 = 22;
    pub const DT_UP: u32 = 23;
    pub const DT_PREINDEX: u32 = 24;
    /// Data operations (CPDO) and register transfers (CPRT) share these.
    pub const S2: u32 = 0;
    pub const OP3: u32 = 4;
    pub const RM: u32 = 5;
    pub const PR2: u32 = 7;
    pub const DS: u32 = 12;
    pub const OP2: u32 = 15;
    pub const S1: u32 = 16;
    pub const PR1: u32 = 19;
    pub const OP1: u32 = 20;
}

/// The condition's four bits.
fn cond_bits(cond: &str) -> u32 {
    const ORDER: [&str; 16] = [
        "EQ", "NE", "CS", "CC", "MI", "PL", "VS", "VC", "HI", "LS", "GE", "LT", "GT", "LE", "AL",
        "NV",
    ];
    if cond.is_empty() {
        return 0xE;
    }
    ORDER.iter().position(|c| *c == cond).unwrap_or(0xE) as u32
}

impl Precision {
    /// The two precision bits, which live apart in every format.
    fn bits(self) -> (u32, u32) {
        match self {
            Precision::Single => (0, 0),
            Precision::Double => (0, 1),
            Precision::Extended => (1, 0),
            Precision::Packed => (1, 1),
        }
    }


}

/// `f0`..`f7`, by name.
fn fpa_register(name: &str) -> Option<u32> {
    let n = name.trim().to_ascii_lowercase();
    n.strip_prefix('f')?.parse::<u32>().ok().filter(|v| *v < 8)
}

/// An ARM register, by the spellings that reach here.
fn arm_register(name: &str) -> Option<u32> {
    let n = name.trim().to_ascii_lowercase();
    match n.as_str() {
        "pc" => Some(15),
        "lr" => Some(14),
        "sp" => Some(13),
        _ => n.strip_prefix('r')?.parse::<u32>().ok().filter(|v| *v < 16),
    }
}

/// The rounding mode's two bits: nearest, plus infinity, minus infinity, zero.
fn rounding_bits(r: Option<char>) -> u32 {
    match r {
        Some('P') => 1,
        Some('M') => 2,
        Some('Z') => 3,
        _ => 0,
    }
}

/// The four bits that say which data operation this is.
///
/// Dyadic operations take two source operands and leave `Op2` clear.
/// Monadic ones take one and set it.
fn data_opcode(stem: &str) -> Option<(u32, bool)> {
    let dyadic = [
        "ADF", "MUF", "SUF", "RSF", "DVF", "RDF", "POW", "RPW", "RMF", "FML", "FDV", "FRD", "POL",
    ];
    if let Some(i) = dyadic.iter().position(|s| *s == stem) {
        return Some((i as u32, false));
    }
    let monadic = [
        "MVF", "MNF", "ABS", "RND", "SQT", "LOG", "LGN", "EXP", "SIN", "COS", "TAN", "ASN", "ACS",
        "ATN", "URD", "NRM",
    ];
    monadic.iter().position(|s| *s == stem).map(|i| (i as u32, true))
}

/// The four bits that say which register transfer this is, `L` included.
fn transfer_opcode(stem: &str, exception: bool) -> Option<u32> {
    Some(match stem {
        "FLT" => 0b0000,
        "FIX" => 0b0001,
        "WFS" => 0b0010,
        "RFS" => 0b0011,
        "WFC" => 0b0100,
        "RFC" => 0b0101,
        "CMF" if exception => 0b1101,
        "CMF" => 0b1001,
        "CNF" if exception => 0b1111,
        "CNF" => 0b1011,
        _ => return None,
    })
}

/// The value an FPA literal pool must hold, written as an integer.
///
/// `LDFS f1, =5729.57795` loads from a pool exactly as `LDR r0, =n` does.
/// But a pool holds words, and the source wrote a decimal fraction. So the
/// fraction is turned into the bits that represent it here, rather than
/// teaching the pool about floating point. That way the pool's sharing of
/// identical literals, its two-pass layout and its relocations all go on
/// working unchanged.
///
/// Single precision only. A double needs two words and an extended three,
/// but the pool reserves one. So those are refused, rather than truncated
/// to a value that is not the one the source asked for.
pub fn literal_bits(precision: Precision, text: &str) -> Option<String> {
    if precision != Precision::Single {
        return None;
    }
    let v: f32 = text.trim().parse().ok()?;
    Some(format!("0x{:08X}", v.to_bits()))
}

/// An FPA instruction as the word FPEmulator will read.
///
/// `None` means this is not an FPA mnemonic at all. Anything else is one. A
/// refusal is a gap in this encoder, not a limit of the target, because the
/// FPA has no instruction its own encoding cannot hold.
pub fn encode(mnemonic: &str, operands: &str) -> Option<Legalized> {
    let f = parse(mnemonic)?;
    let cond = cond_bits(&f.cond) << field::COND;
    let parts = split_operands(operands);
    let word = match f.stem {
        "LDF" | "STF" | "LFM" | "SFM" => data_transfer(&f, cond, &parts),
        "FLT" | "FIX" | "WFS" | "RFS" | "WFC" | "RFC" | "CMF" | "CNF" => {
            register_transfer(&f, cond, &parts)
        }
        _ => data_operation(&f, cond, &parts),
    };
    Some(match word {
        Some(w) => Legalized::RawWord(w),
        None => refuse(mnemonic, "its operands are not ones I know how to encode"),
    })
}

/// Operands split on commas, with a bracketed addressing mode kept whole.
fn split_operands(operands: &str) -> Vec<String> {
    let mut out = Vec::new();
    let mut depth = 0i32;
    let mut cur = String::new();
    for c in operands.chars() {
        match c {
            '[' | '{' => {
                depth += 1;
                cur.push(c);
            }
            ']' | '}' => {
                depth -= 1;
                cur.push(c);
            }
            ',' if depth == 0 => out.push(std::mem::take(&mut cur)),
            _ => cur.push(c),
        }
    }
    if !cur.trim().is_empty() {
        out.push(cur);
    }
    out.into_iter().map(|s| s.trim().to_string()).collect()
}

/// `LDF`, `STF`, `LFM` and `SFM`: a coprocessor data transfer.
fn data_transfer(f: &Fpa, cond: u32, parts: &[String]) -> Option<u32> {
    let load = matches!(f.stem, "LDF" | "LFM");
    let multiple = matches!(f.stem, "LFM" | "SFM");
    let fd = fpa_register(parts.first()?)?;

    // `LFM f0, 4, [r0, #n]` gives the number of registers before the
    // address. The address is everything after that. `[r0], #8` splits
    // into two operands at its comma. Dropping the second operand lost the
    // post-indexed offset, and the writeback with it.
    let (count, addr) = if multiple {
        (parts.get(1)?.trim().parse::<u32>().ok()?, parts.get(2..)?.join(", "))
    } else {
        (0, parts.get(1..)?.join(", "))
    };
    let addr = &addr;

    // The two precision bits carry the format for a single transfer and the
    // number of registers for a multiple one, where four is written as zero.
    let (pr1, pr2) = if multiple {
        match count {
            1 => (0, 1),
            2 => (1, 0),
            3 => (1, 1),
            4 => (0, 0),
            _ => return None,
        }
    } else {
        f.precision?.bits()
    };
    // The coprocessor number says which kind of transfer this is, not which
    // format. A single transfer is coprocessor 1 whatever its precision. A
    // multiple one is coprocessor 2. The format is given by the two
    // precision bits alone. For example, `LDFE f0,[r0]` is &EDD00100 in
    // ObjAsm's object: coprocessor 1 with `pr1` set.
    let coproc = if multiple { 2 } else { 1 };
    if !multiple {
        f.precision?;
    }

    let (rn, offset, pre, up, writeback) = addressing(addr)?;
    Some(
        cond | (0b110 << 25)
            | (pre << field::DT_PREINDEX)
            | (up << field::DT_UP)
            | (pr1 << field::DT_PR1)
            | (writeback << field::DT_WRITEBACK)
            | (u32::from(load) << field::DT_LOAD)
            | (rn << field::DT_RN)
            | (pr2 << field::DT_PR2)
            | (fd << field::DT_FD)
            | (coproc << field::COPROC)
            | (offset << field::DT_OFFSET),
    )
}

/// `[Rn, #off]`, `[Rn, #off]!`, `[Rn], #off` and `[Rn]`.
///
/// Gives back the base register, the offset in words, and the P, U and W bits.
fn addressing(text: &str) -> Option<(u32, u32, u32, u32, u32)> {
    let t = text.trim();

    // A label in this same area, as in `LDFE f1, SqrtHalf`. The expander
    // has already folded it to a distance from the instruction. The FPA
    // loads it the way any ARM load reaches a literal, through `pc`. `pc`
    // reads eight bytes ahead of the instruction that names it.
    if let Some(rest) = t.strip_prefix('.') {
        let rest = rest.trim();
        let distance = if rest.is_empty() {
            0
        } else {
            parse_offset(rest.strip_prefix('+').unwrap_or(rest))?
        };
        let from_pc = distance - 8;
        if from_pc % 4 != 0 {
            return None;
        }
        let words = from_pc / 4;
        let magnitude = words.unsigned_abs() as u32;
        // The offset is eight bits counting words. That is 1020 bytes
        // either way, a quarter of what an ordinary `LDR` reaches.
        if magnitude > 0xFF {
            return None;
        }
        return Some((15, magnitude, 1, u32::from(words >= 0), 0));
    }

    let close = t.find(']')?;
    let inside = t.get(1..close)?;
    let after = t.get(close + 1..)?.trim();

    let mut inner = inside.splitn(2, ',');
    let rn = arm_register(inner.next()?)?;
    let written = inner.next().map(str::trim).unwrap_or("");

    // Post-indexed writes the offset after the bracket, and always writes back.
    let (pre, writeback, offset_text) = if let Some(rest) = after.strip_prefix(',') {
        (0, 1, rest.trim())
    } else {
        (1, u32::from(after == "!"), written)
    };

    let bytes = if offset_text.is_empty() { 0 } else { parse_offset(offset_text)? };
    if bytes % 4 != 0 {
        return None;
    }
    let words = bytes / 4;
    let magnitude = words.unsigned_abs() as u32;
    if magnitude > 0xFF {
        return None;
    }
    Some((rn, magnitude, pre, u32::from(words >= 0), writeback))
}

/// `ADF`, `MVF` and the rest: a coprocessor data operation.
fn data_operation(f: &Fpa, cond: u32, parts: &[String]) -> Option<u32> {
    let (opcode, monadic) = data_opcode(f.stem)?;
    let (pr1, pr2) = f.precision?.bits();
    let fd = fpa_register(parts.first()?)?;
    let (fn_, last) = if monadic {
        (0, parts.get(1)?)
    } else {
        (fpa_register(parts.get(1)?)?, parts.get(2)?)
    };
    let s2 = operand_or_constant(last)?;
    Some(
        cond | (0b1110 << 24)
            | (opcode << field::OP1)
            | (pr1 << field::PR1)
            | (fn_ << field::S1)
            | (u32::from(monadic) << field::OP2)
            | (fd << field::DS)
            | (1 << field::COPROC)
            | (pr2 << field::PR2)
            | (rounding_bits(f.rounding) << field::RM)
            | (s2 << field::S2),
    )
}

/// `FLT`, `FIX`, `CMF` and the status transfers: a coprocessor register
/// transfer, where the ARM register sits in the four bits `Ds` and `Op2`
/// share.
fn register_transfer(f: &Fpa, cond: u32, parts: &[String]) -> Option<u32> {
    let opcode = transfer_opcode(f.stem, f.exception)?;
    let (pr1, pr2) = f.precision.map_or((0, 0), Precision::bits);
    let (rd, fn_, s2) = match f.stem {
        // `FLT Fn, Rd` puts an integer into the FPA.
        "FLT" => (arm_register(parts.get(1)?)?, fpa_register(parts.first()?)?, 0),
        // `FIX Rd, Fm` takes one out.
        "FIX" => (arm_register(parts.first()?)?, 0, fpa_register(parts.get(1)?)?),
        // A comparison puts its answer in the flags, which is written as `pc`
        // where the ARM register goes.
        "CMF" | "CNF" => (
            15,
            fpa_register(parts.first()?)?,
            operand_or_constant(parts.get(1)?)?,
        ),
        // The status words take an ARM register and nothing else.
        _ => (arm_register(parts.first()?)?, 0, 0),
    };
    Some(
        cond | (0b1110 << 24)
            | (opcode << field::OP1)
            | (pr1 << field::PR1)
            | (fn_ << field::S1)
            | (rd << field::DS)
            | (1 << field::COPROC)
            | (pr2 << field::PR2)
            | (rounding_bits(f.rounding) << field::RM)
            | (1 << field::OP3)
            | (s2 << field::S2),
    )
}

/// A register, or one of the eight constants an FPA holds. The constants
/// are written in the same field, with the bit above the register numbers
/// set.
fn operand_or_constant(text: &str) -> Option<u32> {
    if let Some(r) = fpa_register(text) {
        return Some(r);
    }
    let t = text.trim().trim_start_matches('#').trim();
    let n = match t {
        "0" | "0.0" => 0,
        "1" | "1.0" => 1,
        "2" | "2.0" => 2,
        "3" | "3.0" => 3,
        "4" | "4.0" => 4,
        "5" | "5.0" => 5,
        "0.5" => 6,
        "10" | "10.0" => 7,
        // Only the ones with a decimal point ever arrive spelled as the
        // source wrote them. `#0` is an integer expression, so the evaluator
        // folds it and hands the encoder `00000000`. Matching the source's
        // spelling alone refused `CMF f0,#0`. That compares against zero,
        // and is the commonest FPA instruction in the corpus.
        _ => constant_index(evaluated_number(t)?)?,
    };
    Some(0b1000 | n)
}

/// A number as the expression evaluator leaves it.
///
/// Arithmetic is folded and written again as eight hex digits. Before that,
/// `&` became `0x`. A bare decimal is not accepted. `10` could be sixteen,
/// read as folded hex, or ten, read as the source wrote it. A
/// wrong constant gives a wrong instruction, whereas refusing it gives only
/// a zero word.
fn evaluated_number(t: &str) -> Option<u32> {
    if let Some(h) = t.strip_prefix("0x").or_else(|| t.strip_prefix("0X")) {
        return u32::from_str_radix(h, 16).ok();
    }
    (t.len() == 8 && t.chars().all(|c| c.is_ascii_hexdigit()))
        .then(|| u32::from_str_radix(t, 16).ok())
        .flatten()
}

/// Which of the eight constants a value is, if it is one of them.
///
/// Zero to five sit at their own index, and ten sits at seven. A half has
/// no integer spelling, so it only ever arrives as `0.5`, handled by the
/// caller.
fn constant_index(v: u32) -> Option<u32> {
    match v {
        0..=5 => Some(v),
        10 => Some(7),
        _ => None,
    }
}
