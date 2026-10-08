//! Decoding A32 instruction words: the ObjAsm compiler's IR.
//!
//! The compiler reads what was assembled, not what was parsed. An encoding
//! *is* the instruction, exactly, with its addressing mode, shifter and
//! condition. The byte-identity gate is what makes rosasm's bytes the same
//! as ObjAsm's. So `--emit c` compiles the words rosasm produced. rosasm
//! supplies what the words cannot: labels, source lines, and which bytes
//! are code.
//!
//! This decodes the ARMv7-A instructions that RISC OS 5's ObjAsm sources
//! use outside the kernel's hardware code, and their floating point. That
//! is FPA on coprocessors 1 and 2, as ObjAsm and rosasm's `fpa.rs` encode
//! it for the emulator, which ROSGD never uses; and VFP on coprocessors 10
//! and 11. Anything else decodes to `Insn::Unknown`, which the compiler
//! reports rather than guessing at.

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Cond {
    Eq,
    Ne,
    Cs,
    Cc,
    Mi,
    Pl,
    Vs,
    Vc,
    Hi,
    Ls,
    Ge,
    Lt,
    Gt,
    Le,
    Al,
}

impl Cond {
    fn from_bits(b: u32) -> Option<Self> {
        use Cond::*;
        Some(match b {
            0 => Eq,
            1 => Ne,
            2 => Cs,
            3 => Cc,
            4 => Mi,
            5 => Pl,
            6 => Vs,
            7 => Vc,
            8 => Hi,
            9 => Ls,
            10 => Ge,
            11 => Lt,
            12 => Gt,
            13 => Le,
            14 => Al,
            _ => return None,
        })
    }
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum ShiftType {
    Lsl,
    Lsr,
    Asr,
    Ror,
}

impl ShiftType {
    fn from_bits(b: u32) -> Self {
        match b & 3 {
            0 => ShiftType::Lsl,
            1 => ShiftType::Lsr,
            2 => ShiftType::Asr,
            _ => ShiftType::Ror,
        }
    }
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Shift {
    /// By an immediate, normalised as the ARM ARM's DecodeImmShift does:
    /// `LSR #0` and `ASR #0` mean 32, and `LSL #0` is no shift.
    Imm(ShiftType, u32),
    /// `ROR #0`: rotate right one bit through the carry.
    Rrx,
    /// By the bottom byte of a register.
    Reg(ShiftType, u8),
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Operand2 {
    /// A rotated 8-bit immediate. When the rotation is non-zero the
    /// shifter's carry out is the value's bit 31; otherwise it is C.
    Imm { value: u32, rotated: bool },
    Reg { rm: u8, shift: Shift },
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum DpOp {
    And,
    Eor,
    Sub,
    Rsb,
    Add,
    Adc,
    Sbc,
    Rsc,
    Tst,
    Teq,
    Cmp,
    Cmn,
    Orr,
    Mov,
    Bic,
    Mvn,
}

impl DpOp {
    fn from_bits(b: u32) -> Self {
        use DpOp::*;
        [And, Eor, Sub, Rsb, Add, Adc, Sbc, Rsc, Tst, Teq, Cmp, Cmn, Orr, Mov, Bic, Mvn]
            [(b & 15) as usize]
    }

    /// Whether the result is discarded: the flag-setting comparisons.
    pub fn is_test(self) -> bool {
        matches!(self, DpOp::Tst | DpOp::Teq | DpOp::Cmp | DpOp::Cmn)
    }

    /// Whether the flags come from the result and the shifter (N, Z, C)
    /// rather than from an addition or subtraction (N, Z, C, V).
    pub fn is_logical(self) -> bool {
        matches!(
            self,
            DpOp::And | DpOp::Eor | DpOp::Tst | DpOp::Teq | DpOp::Orr | DpOp::Mov | DpOp::Bic | DpOp::Mvn
        )
    }

    /// Whether Rn is an operand. MOV and MVN ignore it.
    pub fn uses_rn(self) -> bool {
        !matches!(self, DpOp::Mov | DpOp::Mvn)
    }
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Width {
    Word,
    Byte,
    Half,
    SignedByte,
    SignedHalf,
    /// LDRD and STRD: Rt and Rt+1.
    Double,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Offset {
    /// A byte offset; its sign is the transfer's `add`.
    Imm(u32),
    Reg { rm: u8, shift: Shift },
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum MulOp {
    Mul,
    Mla,
    Mls,
    Umull,
    Umlal,
    Smull,
    Smlal,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum MsrSrc {
    Imm(u32),
    Reg(u8),
}

/// An FPA operation's precision: the format its result is rounded to, or
/// a transfer's format in memory.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Prec {
    Single,
    Double,
    /// About 64 bits of mantissa; 12 bytes in memory.
    Extended,
    /// Packed decimal, in memory only.
    Packed,
}

/// FPA rounding: to nearest, towards +infinity (P), -infinity (M), zero (Z).
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Round {
    Nearest,
    Plus,
    Minus,
    Zero,
}

/// An FPA operand: a register, or one of the eight constants the FPA holds
/// (0, 1, 2, 3, 4, 5, 0.5, 10, by index).
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum FpaOperand {
    Reg(u8),
    Const(u8),
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum FpaDyadic {
    Adf,
    Muf,
    Suf,
    Rsf,
    Dvf,
    Rdf,
    Pow,
    Rpw,
    Rmf,
    Fml,
    Fdv,
    Frd,
    Pol,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum FpaMonadic {
    Mvf,
    Mnf,
    Abs,
    Rnd,
    Sqt,
    Log,
    Lgn,
    Exp,
    Sin,
    Cos,
    Tan,
    Asn,
    Acs,
    Atn,
    Urd,
    Nrm,
}

/// VFP's three-register arithmetic.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum VfpOp {
    Mla,
    Mls,
    Nmla,
    Nmls,
    Nmul,
    Mul,
    Add,
    Sub,
    Div,
    Fma,
    Fms,
    Fnma,
    Fnms,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
/// NEON's three-register integer lanes that the corpus's BASIC uses.
pub enum NeonVecOp {
    Add,
    Sub,
    Mul,
    Mla,
}

/// VFP's one-register operations.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum VfpUnary {
    Mov,
    Abs,
    Neg,
    Sqrt,
}

/// VFP's conversions.  Register numbers are single or double as the
/// conversion's source and destination types say.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum VfpCvt {
    /// Between double (`to_double`) and single.
    Precision { to_double: bool },
    /// From a 32-bit integer in a single register.
    FromInt { signed: bool, double: bool },
    /// To a 32-bit integer in a single register. `toward_zero` false is
    /// VCVTR, which rounds as the FPSCR says.
    ToInt { signed: bool, double: bool, toward_zero: bool },
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Insn {
    /// Data processing: `op{S} rd, rn, op2`.
    Dp { op: DpOp, s: bool, rd: u8, rn: u8, op2: Operand2 },
    /// Multiplies. For the long forms `rd` is RdHi and `ra` is RdLo. For
    /// MLA and MLS `ra` is the accumulator.
    Mul { op: MulOp, s: bool, rd: u8, ra: u8, rm: u8, rs: u8 },
    /// A single load or store. `pre` false is post-indexed, which always
    /// writes back. `wback` is the W bit of a pre-indexed transfer.
    Mem { load: bool, width: Width, rt: u8, rn: u8, offset: Offset, add: bool, pre: bool, wback: bool },
    /// LDM and STM. `before` is the P bit and `add` the U bit. `user` is the
    /// `^` form, which this compiler does not model.
    Block { load: bool, rn: u8, regs: u16, before: bool, add: bool, wback: bool, user: bool },
    /// B and BL: the target is the instruction's address + 8 + `offset`.
    Branch { link: bool, offset: i32 },
    /// BX and BLX by register.
    Bx { link: bool, rm: u8 },
    Swi { number: u32 },
    Mrs { rd: u8, spsr: bool },
    /// `mask` is the field mask: bit 0 c, 1 x, 2 s, 3 f.
    Msr { spsr: bool, mask: u8, src: MsrSrc },
    Clz { rd: u8, rm: u8 },
    /// MOVW, and MOVT (which writes the top half and keeps the bottom).
    MovHalf { top: bool, rd: u8, imm: u16 },
    Swp { byte: bool, rt: u8, rt2: u8, rn: u8 },
    /// CPSIE and CPSID: interrupts on or off (`i`, `f`), and a mode to
    /// change to, if any.
    Cps { disable: bool, i: bool, f: bool, mode: Option<u8> },
    /// NOP, the hints, and PLD: nothing the compiled code can observe.
    Nop,

    // ---- FPA: registers f0-f7 ----
    FpaDyadic { op: FpaDyadic, prec: Prec, round: Round, fd: u8, fn_: u8, fm: FpaOperand },
    FpaMonadic { op: FpaMonadic, prec: Prec, round: Round, fd: u8, fm: FpaOperand },
    /// FLT Fn, Rd: an integer into the FPA.
    FpaFlt { prec: Prec, round: Round, fn_: u8, rd: u8 },
    /// FIX Rd, Fm: out again, rounded as `round` says.
    FpaFix { round: Round, rd: u8, fm: u8 },
    /// WFS and RFS write and read the status register. WFC and RFC write
    /// and read the control register.
    FpaStatus { write: bool, control: bool, rd: u8 },
    /// CMF and CNF. `negate` marks CNF, which compares against -Fm.
    /// `exception` marks CMFE and CNFE.
    FpaCompare { negate: bool, exception: bool, fn_: u8, fm: FpaOperand },
    /// LDF and STF.  `offset` is in bytes.
    FpaMem { load: bool, prec: Prec, fd: u8, rn: u8, offset: u32, add: bool, pre: bool, wback: bool },
    /// LFM and SFM: `count` registers from `fd`, 12 bytes each.
    FpaMulti { load: bool, fd: u8, count: u8, rn: u8, offset: u32, add: bool, pre: bool, wback: bool },

    // ---- VFP: d0-d31, s0-s31 the halves of d0-d15 ----
    /// VLDR and VSTR.  `v` is a single or double register number.
    VfpMem { load: bool, double: bool, v: u8, rn: u8, offset: u32, add: bool },
    /// VLDM and VSTM (and VPUSH, VPOP): `count` registers from `first`.
    VfpMulti { load: bool, double: bool, first: u8, count: u8, rn: u8, add: bool, pre: bool, wback: bool },
    VfpArith { op: VfpOp, double: bool, vd: u8, vn: u8, vm: u8 },
    VfpUnary { op: VfpUnary, double: bool, vd: u8, vm: u8 },
    /// VMOV with an immediate: the constant's bits, in the register's format.
    VfpMovImm { double: bool, vd: u8, bits: u64 },
    /// VCMP and VCMPE (`exception`); `vm` None compares with zero.
    VfpCompare { double: bool, exception: bool, vd: u8, vm: Option<u8> },
    VfpCvt { cvt: VfpCvt, vd: u8, vm: u8 },
    /// VMOV between an ARM register and a single register.
    VfpMovCore { to_core: bool, sn: u8, rt: u8 },
    /// VMOV between two ARM registers and a double register, or two
    /// consecutive single registers (`double` false).
    VfpMovCore2 { to_core: bool, double: bool, vm: u8, rt: u8, rt2: u8 },
    /// VMRS Rt, FPSCR, and VMSR FPSCR, Rt. When rt is 15, VMRS copies to
    /// the flags instead.
    VfpStatus { to_core: bool, rt: u8 },
    /// NEON's integer lanes over a quad register, as BASIC's array
    /// arithmetic uses them. The four words of Qn are the four singles
    /// `S[4n]..S[4n+3]`. The register numbers here are quad numbers.
    NeonLane { op: NeonVecOp, vd: u8, vn: u8, vm: u8 },
    /// VDUP.32 Qd, Rt: one core register's word into all four lanes.
    NeonDup { vd: u8, rt: u8 },
    /// VPADD.S32: the pairwise add that folds a quad's four words to two.
    NeonPadd { vd: u8, vn: u8, vm: u8 },

    Unknown,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct Decoded {
    pub cond: Cond,
    pub insn: Insn,
}

fn bit(w: u32, n: u32) -> bool {
    (w >> n) & 1 != 0
}

fn reg(w: u32, lsb: u32) -> u8 {
    ((w >> lsb) & 15) as u8
}

fn imm_shift(w: u32) -> Shift {
    let ty = ShiftType::from_bits(w >> 5);
    let n = (w >> 7) & 31;
    match (ty, n) {
        (ShiftType::Lsr, 0) | (ShiftType::Asr, 0) => Shift::Imm(ty, 32),
        (ShiftType::Ror, 0) => Shift::Rrx,
        _ => Shift::Imm(ty, n),
    }
}

fn rotated_imm(w: u32) -> Operand2 {
    let rot = ((w >> 8) & 15) * 2;
    Operand2::Imm { value: (w & 0xFF).rotate_right(rot), rotated: rot != 0 }
}

pub fn decode(w: u32) -> Decoded {
    let unknown = |cond| Decoded { cond, insn: Insn::Unknown };
    let Some(cond) = Cond::from_bits(w >> 28) else {
        // The unconditional space. PLD is a hint. CPS turns interrupts on
        // and off, which the runtime models. The rest (BLX to Thumb, SRS,
        // RFE, barriers) either cannot occur in RISC OS's ARM code or
        // touches what the runtime owns.
        let insn = if w & 0xFD70_F000 == 0xF550_F000 {
            Insn::Nop
        } else if w & !(0x400000 | 0xF000) == 0xF280_0010 {
            // `VMOV.I32 Dd,#0`: the word ObjAsm writes for `FLDD Dd,=0`.
            // This is NEON's one appearance in this corpus's encodings. The
            // word only zeroes the register, which gives a double's 0.0.
            Insn::VfpMovImm { double: true, vd: vreg(true, (w >> 12) & 15, bit(w, 22)), bits: 0 }
        } else if w & 0xFE200F50 == 0xF2200B10 {
            // VPADD.S32 Dd,Dn,Dm: S[2d] = S[2n]+S[2n+1], S[2d+1] = S[2m]+S[2m+1].
            let d = |hi: u32, lo: u32| ((u32::from(bit(w, hi)) << 4) | (w >> lo) & 15) as u8;
            Insn::NeonPadd { vd: d(22, 12), vn: d(7, 16), vm: d(5, 0) }
        } else if let Some(op) = neon_lane(w) {
            op
        } else if w & 0xFFF1_FE20 == 0xF100_0000 && (w >> 18) & 3 >= 2 {
            Insn::Cps {
                disable: bit(w, 18),
                i: bit(w, 7),
                f: bit(w, 6),
                mode: bit(w, 17).then_some((w & 0x1F) as u8),
            }
        } else if w & 0xFFFE_FFE0 == 0xF102_0000 {
            Insn::Cps { disable: false, i: false, f: false, mode: Some((w & 0x1F) as u8) }
        } else {
            Insn::Unknown
        };
        return Decoded { cond: Cond::Al, insn };
    };
    let insn = match (w >> 25) & 7 {
        0b000 => decode_000(w),
        0b001 => decode_001(w),
        0b010 | 0b011 => {
            if bit(w, 25) && bit(w, 4) {
                Insn::Unknown // the media instructions
            } else {
                let offset = if bit(w, 25) {
                    Offset::Reg { rm: reg(w, 0), shift: imm_shift(w) }
                } else {
                    Offset::Imm(w & 0xFFF)
                };
                Insn::Mem {
                    load: bit(w, 20),
                    width: if bit(w, 22) { Width::Byte } else { Width::Word },
                    rt: reg(w, 12),
                    rn: reg(w, 16),
                    offset,
                    add: bit(w, 23),
                    pre: bit(w, 24),
                    // Post-indexed with W set is LDRT/STRT: an unprivileged
                    // access, which is an ordinary one here.
                    wback: bit(w, 24) && bit(w, 21),
                }
            }
        }
        0b100 => Insn::Block {
            load: bit(w, 20),
            rn: reg(w, 16),
            regs: (w & 0xFFFF) as u16,
            before: bit(w, 24),
            add: bit(w, 23),
            wback: bit(w, 21),
            user: bit(w, 22),
        },
        0b101 => Insn::Branch { link: bit(w, 24), offset: (((w & 0xFF_FFFF) << 8) as i32) >> 6 },
        0b111 if bit(w, 24) => Insn::Swi { number: w & 0xFF_FFFF },
        // The coprocessors: FPA is 1 and 2, VFP 10 and 11.
        0b110 | 0b111 => match (w >> 8) & 15 {
            1 | 2 => decode_fpa(w),
            10 | 11 => decode_vfp(w),
            _ => Insn::Unknown,
        },
        _ => return unknown(cond),
    };
    Decoded { cond, insn }
}

fn decode_000(w: u32) -> Insn {
    let op2 = (w >> 4) & 15;
    if bit(w, 4) && bit(w, 7) {
        if op2 == 0b1001 {
            if !bit(w, 24) {
                let s = bit(w, 20);
                let (rd, ra, rs, rm) = (reg(w, 16), reg(w, 12), reg(w, 8), reg(w, 0));
                let op = match (w >> 21) & 7 {
                    0 => MulOp::Mul,
                    1 => MulOp::Mla,
                    3 if !s => MulOp::Mls,
                    4 => MulOp::Umull,
                    5 => MulOp::Umlal,
                    6 => MulOp::Smull,
                    7 => MulOp::Smlal,
                    _ => return Insn::Unknown, // UMAAL
                };
                return Insn::Mul { op, s, rd, ra, rm, rs };
            }
            if w & 0x0FB0_0FF0 == 0x0100_0090 {
                return Insn::Swp { byte: bit(w, 22), rt: reg(w, 12), rt2: reg(w, 0), rn: reg(w, 16) };
            }
            return Insn::Unknown; // LDREX, STREX and friends
        }
        // The extra loads and stores: halfwords, signed bytes, doublewords.
        let load = bit(w, 20);
        let width = match (op2, load) {
            (0b1011, _) => Width::Half,
            (0b1101, true) => Width::SignedByte,
            (0b1101, false) => Width::Double, // LDRD
            (0b1111, true) => Width::SignedHalf,
            (0b1111, false) => Width::Double, // STRD
            _ => return Insn::Unknown,
        };
        // LDRD is encoded with L clear, and STRD likewise: op2 says which.
        let load = if width == Width::Double { op2 == 0b1101 } else { load };
        let offset = if bit(w, 22) {
            Offset::Imm(((w >> 4) & 0xF0) | (w & 0xF))
        } else {
            Offset::Reg { rm: reg(w, 0), shift: Shift::Imm(ShiftType::Lsl, 0) }
        };
        return Insn::Mem {
            load,
            width,
            rt: reg(w, 12),
            rn: reg(w, 16),
            offset,
            add: bit(w, 23),
            pre: bit(w, 24),
            wback: bit(w, 24) && bit(w, 21),
        };
    }
    // TST, TEQ, CMP and CMN without S are the miscellaneous instructions.
    if (w >> 23) & 3 == 0b10 && !bit(w, 20) {
        let spsr = bit(w, 22);
        return match (op2, (w >> 21) & 3) {
            (0b0000, 0b00) | (0b0000, 0b10) if (w >> 16) & 15 == 15 && w & 0xFFF == 0 => {
                Insn::Mrs { rd: reg(w, 12), spsr }
            }
            (0b0000, 0b01) | (0b0000, 0b11) if (w >> 12) & 15 == 15 && (w >> 8) & 15 == 0 => {
                Insn::Msr { spsr, mask: ((w >> 16) & 15) as u8, src: MsrSrc::Reg(reg(w, 0)) }
            }
            (0b0001, 0b01) if w & 0x000F_FF00 == 0x000F_FF00 => Insn::Bx { link: false, rm: reg(w, 0) },
            (0b0011, 0b01) if w & 0x000F_FF00 == 0x000F_FF00 => Insn::Bx { link: true, rm: reg(w, 0) },
            (0b0001, 0b11) if w & 0x000F_0F00 == 0x000F_0F00 => Insn::Clz { rd: reg(w, 12), rm: reg(w, 0) },
            _ => Insn::Unknown,
        };
    }
    let op2 = if bit(w, 4) {
        // Register-shifted register; bit 7 is clear here.
        Operand2::Reg { rm: reg(w, 0), shift: Shift::Reg(ShiftType::from_bits(w >> 5), reg(w, 8)) }
    } else {
        Operand2::Reg { rm: reg(w, 0), shift: imm_shift(w) }
    };
    Insn::Dp { op: DpOp::from_bits(w >> 21), s: bit(w, 20), rd: reg(w, 12), rn: reg(w, 16), op2 }
}

fn decode_001(w: u32) -> Insn {
    match (w >> 20) & 0x1F {
        0b10000 | 0b10100 => Insn::MovHalf {
            top: bit(w, 22),
            rd: reg(w, 12),
            imm: (((w >> 4) & 0xF000) | (w & 0xFFF)) as u16,
        },
        0b10010 | 0b10110 => {
            let mask = ((w >> 16) & 15) as u8;
            if mask == 0 && !bit(w, 22) {
                Insn::Nop // NOP, YIELD, WFE, WFI, SEV
            } else {
                let Operand2::Imm { value, .. } = rotated_imm(w) else { unreachable!() };
                Insn::Msr { spsr: bit(w, 22), mask, src: MsrSrc::Imm(value) }
            }
        }
        _ => Insn::Dp {
            op: DpOp::from_bits(w >> 21),
            s: bit(w, 20),
            rd: reg(w, 12),
            rn: reg(w, 16),
            op2: rotated_imm(w),
        },
    }
}

// ---- FPA ------------------------------------------------------------------
//
// Field positions are FPEmulator's own (HWSupport/FPASC/coresrc/s/fpadefs),
// as rosasm's fpa.rs encodes them.

fn fpa_prec(pr1: bool, pr2: bool) -> Prec {
    match (pr1, pr2) {
        (false, false) => Prec::Single,
        (false, true) => Prec::Double,
        (true, false) => Prec::Extended,
        (true, true) => Prec::Packed,
    }
}

fn fpa_round(w: u32) -> Round {
    match (w >> 5) & 3 {
        0 => Round::Nearest,
        1 => Round::Plus,
        2 => Round::Minus,
        _ => Round::Zero,
    }
}

fn fpa_operand(w: u32) -> FpaOperand {
    if bit(w, 3) {
        FpaOperand::Const((w & 7) as u8)
    } else {
        FpaOperand::Reg((w & 7) as u8)
    }
}

fn decode_fpa(w: u32) -> Insn {
    let cp = (w >> 8) & 15;
    if (w >> 25) & 7 == 0b110 {
        // A data transfer: LDF/STF on coprocessor 1, LFM/SFM on 2.
        let (pr1, pr2) = (bit(w, 22), bit(w, 15));
        let (load, rn, fd) = (bit(w, 20), reg(w, 16), ((w >> 12) & 7) as u8);
        let (offset, add, pre, wback) = ((w & 0xFF) * 4, bit(w, 23), bit(w, 24), bit(w, 21));
        if !pre && !wback {
            return Insn::Unknown; // post-indexed without writeback is undefined
        }
        return if cp == 1 {
            Insn::FpaMem { load, prec: fpa_prec(pr1, pr2), fd, rn, offset, add, pre, wback }
        } else {
            let count = match (pr1, pr2) {
                (false, true) => 1,
                (true, false) => 2,
                (true, true) => 3,
                (false, false) => 4,
            };
            Insn::FpaMulti { load, fd, count, rn, offset, add, pre, wback }
        };
    }
    if cp != 1 || (w >> 24) & 15 != 0b1110 {
        return Insn::Unknown;
    }
    let prec = fpa_prec(bit(w, 19), bit(w, 7));
    let round = fpa_round(w);
    let opcode = (w >> 20) & 15;
    if !bit(w, 4) {
        // A data operation: dyadic, or monadic with bit 15 set.
        let fd = ((w >> 12) & 7) as u8;
        let fm = fpa_operand(w);
        if prec == Prec::Packed {
            return Insn::Unknown;
        }
        if bit(w, 15) {
            use FpaMonadic::*;
            let op = [Mvf, Mnf, Abs, Rnd, Sqt, Log, Lgn, Exp, Sin, Cos, Tan, Asn, Acs, Atn, Urd, Nrm]
                [opcode as usize];
            return Insn::FpaMonadic { op, prec, round, fd, fm };
        }
        use FpaDyadic::*;
        let ops = [Adf, Muf, Suf, Rsf, Dvf, Rdf, Pow, Rpw, Rmf, Fml, Fdv, Frd, Pol];
        return match ops.get(opcode as usize) {
            Some(&op) => Insn::FpaDyadic { op, prec, round, fd, fn_: ((w >> 16) & 7) as u8, fm },
            None => Insn::Unknown,
        };
    }
    // A register transfer; the ARM register is bits 15-12.
    let rd = reg(w, 12);
    let fn_ = ((w >> 16) & 7) as u8;
    match opcode {
        0 => Insn::FpaFlt { prec, round, fn_, rd },
        1 => Insn::FpaFix { round, rd, fm: (w & 7) as u8 },
        2 => Insn::FpaStatus { write: true, control: false, rd },
        3 => Insn::FpaStatus { write: false, control: false, rd },
        4 => Insn::FpaStatus { write: true, control: true, rd },
        5 => Insn::FpaStatus { write: false, control: true, rd },
        9 | 11 | 13 | 15 if rd == 15 => Insn::FpaCompare {
            negate: opcode & 2 != 0,
            exception: opcode & 4 != 0,
            fn_,
            fm: fpa_operand(w),
        },
        _ => Insn::Unknown,
    }
}

// ---- VFP ------------------------------------------------------------------
//
// ARMv7-A's VFPv3/VFPv4 (the ARM ARM, A7.5-A7.9). A single register's
// number is the four-bit field Vx followed by the extra bit (Vx:bit). A
// double's is the extra bit followed by Vx (bit:Vx).

fn vreg(double: bool, four: u32, one: bool) -> u8 {
    if double {
        ((u32::from(one) << 4) | four) as u8
    } else {
        ((four << 1) | u32::from(one)) as u8
    }
}

/// VFPExpandImm: an 8-bit VMOV immediate as a single's or double's bits.
fn vfp_expand_imm(imm8: u32, double: bool) -> u64 {
    let sign = u64::from(imm8 >> 7 & 1);
    let b6 = u64::from(imm8 >> 6 & 1);
    let low = u64::from(imm8 & 0x3F);
    if double {
        let exp = ((b6 ^ 1) << 10) | (if b6 == 1 { 0xFF << 2 } else { 0 }) | (low >> 4);
        (sign << 63) | (exp << 52) | ((low & 0xF) << 48)
    } else {
        let exp = ((b6 ^ 1) << 7) | (if b6 == 1 { 0x1F << 2 } else { 0 }) | (low >> 4);
        (sign << 31) | (exp << 23) | ((low & 0xF) << 19)
    }
}

/// NEON's `VADD/VSUB/VMUL/VMLA .i32 Q,Q,Q`. This is BASIC's array
/// arithmetic, and the whole of the corpus's NEON. The register fields hold
/// D numbers. The Q bit must be set for a quad. The quad's four words are
/// four singles, which is how the lifter takes them.
fn neon_lane(w: u32) -> Option<Insn> {
    let op = match w & 0xFE200F50 {
        0xF2200840 => NeonVecOp::Add,
        0xF3200840 => NeonVecOp::Sub,
        0xF2200950 => NeonVecOp::Mul,
        0xF2200940 => NeonVecOp::Mla,
        _ => return None,
    };
    let d = |hi: u32, lo: u32| ((u32::from(bit(w, hi)) << 4) | (w >> lo) & 15) as u8;
    Some(Insn::NeonLane {
        op,
        vd: d(22, 12) >> 1,
        vn: d(7, 16) >> 1,
        vm: d(5, 0) >> 1,
    })
}

fn decode_vfp(w: u32) -> Insn {
    let double = bit(w, 8);
    let d = bit(w, 22);
    let vd = vreg(double, (w >> 12) & 15, d);
    match (w >> 24) & 15 {
        // Loads and stores: VLDR/VSTR, VLDM/VSTM.
        0b1100 | 0b1101 => {
            let (pre, add, wback, load) = (bit(w, 24), bit(w, 23), bit(w, 21), bit(w, 20));
            let rn = reg(w, 16);
            if (w >> 21) & 0x7F == 0b1100010 {
                // VMOV between two ARM registers and D, or two singles.
                let vm = vreg(double, w & 15, bit(w, 5));
                if (w >> 4) & 0xD != 1 {
                    return Insn::Unknown;
                }
                return Insn::VfpMovCore2 { to_core: load, double, vm, rt: reg(w, 12), rt2: reg(w, 16) };
            }
            if pre && !wback {
                return Insn::VfpMem { load, double, v: vd, rn, offset: (w & 0xFF) * 4, add };
            }
            let imm8 = w & 0xFF;
            if double && imm8 & 1 != 0 {
                return Insn::Unknown; // FLDMX/FSTMX
            }
            // Only IA, IA! and DB! exist. P and U are never both set, and
            // DB always writes back.
            if (pre && add) || (pre && !wback) || (!pre && !add) {
                return Insn::Unknown;
            }
            let count = if double { imm8 / 2 } else { imm8 } as u8;
            Insn::VfpMulti { load, double, first: vd, count, rn, add, pre, wback }
        }
        0b1110 if !bit(w, 4) => {
            // Data processing.
            let vn = vreg(double, (w >> 16) & 15, bit(w, 7));
            let vm = vreg(double, w & 15, bit(w, 5));
            let op = bit(w, 6);
            let arith = |o| Insn::VfpArith { op: o, double, vd, vn, vm };
            match (bit(w, 23), bit(w, 21), bit(w, 20)) {
                (false, false, false) => arith(if op { VfpOp::Mls } else { VfpOp::Mla }),
                (false, false, true) => arith(if op { VfpOp::Nmla } else { VfpOp::Nmls }),
                (false, true, false) => arith(if op { VfpOp::Nmul } else { VfpOp::Mul }),
                (false, true, true) => arith(if op { VfpOp::Sub } else { VfpOp::Add }),
                (true, false, false) if !op => arith(VfpOp::Div),
                (true, false, true) => arith(if op { VfpOp::Fnma } else { VfpOp::Fnms }),
                (true, true, false) => arith(if op { VfpOp::Fms } else { VfpOp::Fma }),
                (true, true, true) => decode_vfp_other(w, double, vd),
                _ => Insn::Unknown,
            }
        }
        0b1110 => {
            // Register transfers, bit 4 set.
            let to_core = bit(w, 20);
            if (w >> 21) & 7 == 0b111 && (w >> 8) & 15 == 10 && w & 0xFF == 0x10 {
                // VMRS and VMSR: the FPSCR (1) only.
                return if (w >> 16) & 15 == 1 {
                    Insn::VfpStatus { to_core, rt: reg(w, 12) }
                } else {
                    Insn::Unknown
                };
            }
            if (w >> 21) & 7 == 0 && (w >> 8) & 15 == 10 && w & 0x7F == 0x10 {
                return Insn::VfpMovCore { to_core, sn: vreg(false, (w >> 16) & 15, bit(w, 7)), rt: reg(w, 12) };
            }
            // VMOV.32 Rt,Dm[0]: the double's low word to a core register.
            // This is VMOV Rt,Sm, where Sm is the double's first single. It
            // is the lane extract at the end of BASIC's vector sum.
            if w & 0x0F3F0FF0 == 0x0E100B10 {
                let m = ((u32::from(bit(w, 5)) << 4) | (w & 15)) as u8;
                return Insn::VfpMovCore { to_core: true, sn: 2 * m, rt: reg(w, 12) };
            }
            // VDUP.32 Qd, Rt: a core register into all four words of a
            // quad, as BASIC's vector-by-scalar arithmetic primes its loop.
            if w & 0x0FBF0FFF == 0x0EA00B10 {
                return Insn::NeonDup { vd: ((u32::from(bit(w, 22)) << 4 | (w >> 16) & 15) >> 1) as u8, rt: reg(w, 12) };
            }
            Insn::Unknown
        }
        _ => Insn::Unknown,
    }
}

/// The "other" data-processing row: VMOV immediate and register, VABS,
/// VNEG, VSQRT, VCMP, VCVT.
fn decode_vfp_other(w: u32, double: bool, vd: u8) -> Insn {
    let opc2 = (w >> 16) & 15;
    let vm = vreg(double, w & 15, bit(w, 5));
    if !bit(w, 6) {
        // VMOV immediate: imm8 is opc2 (bits 19-16) over bits 3-0.
        return Insn::VfpMovImm { double, vd, bits: vfp_expand_imm((opc2 << 4) | (w & 0xF), double) };
    }
    let b7 = bit(w, 7);
    match opc2 {
        0b0000 => Insn::VfpUnary { op: if b7 { VfpUnary::Abs } else { VfpUnary::Mov }, double, vd, vm },
        0b0001 => Insn::VfpUnary { op: if b7 { VfpUnary::Sqrt } else { VfpUnary::Neg }, double, vd, vm },
        0b0100 => Insn::VfpCompare { double, exception: b7, vd, vm: Some(vm) },
        0b0101 if w & 0x2F == 0 => Insn::VfpCompare { double, exception: b7, vd, vm: None },
        0b0111 if b7 => {
            // Between precisions: the destination is the other size.
            let vd2 = vreg(!double, (w >> 12) & 15, bit(w, 22));
            Insn::VfpCvt { cvt: VfpCvt::Precision { to_double: !double }, vd: vd2, vm }
        }
        0b1000 => {
            // From an integer, held in a single register.
            let sm = vreg(false, w & 15, bit(w, 5));
            Insn::VfpCvt { cvt: VfpCvt::FromInt { signed: b7, double }, vd, vm: sm }
        }
        0b1100 | 0b1101 => {
            // To an integer, into a single register.
            let sd = vreg(false, (w >> 12) & 15, bit(w, 22));
            Insn::VfpCvt { cvt: VfpCvt::ToInt { signed: opc2 & 1 != 0, double, toward_zero: b7 }, vd: sd, vm }
        }
        _ => Insn::Unknown, // half precision, fixed point
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn d(w: u32) -> Insn {
        decode(w).insn
    }

    #[test]
    fn cps() {
        // cpsid i; cpsie if; cpsid i, #19 (words from llvm-mc)
        assert_eq!(d(0xF10C_0080), Insn::Cps { disable: true, i: true, f: false, mode: None });
        assert_eq!(d(0xF108_00C0), Insn::Cps { disable: false, i: true, f: true, mode: None });
        assert_eq!(d(0xF10E_0093), Insn::Cps { disable: true, i: true, f: false, mode: Some(0x13) });
    }

    #[test]
    fn data_processing() {
        // MOV r0, #6
        assert_eq!(
            d(0xE3A0_0006),
            Insn::Dp { op: DpOp::Mov, s: false, rd: 0, rn: 0, op2: Operand2::Imm { value: 6, rotated: false } }
        );
        // ADDLO pc, pc, r11, LSL #2
        let x = decode(0x308F_F10B);
        assert_eq!(x.cond, Cond::Cc);
        assert_eq!(
            x.insn,
            Insn::Dp {
                op: DpOp::Add,
                s: false,
                rd: 15,
                rn: 15,
                op2: Operand2::Reg { rm: 11, shift: Shift::Imm(ShiftType::Lsl, 2) }
            }
        );
        // MOVS r3, r1
        assert_eq!(
            d(0xE1B0_3001),
            Insn::Dp {
                op: DpOp::Mov,
                s: true,
                rd: 3,
                rn: 0,
                op2: Operand2::Reg { rm: 1, shift: Shift::Imm(ShiftType::Lsl, 0) }
            }
        );
        // MOV r0, r1, LSR #32 and RRX
        assert_eq!(
            d(0xE1A0_0021),
            Insn::Dp {
                op: DpOp::Mov,
                s: false,
                rd: 0,
                rn: 0,
                op2: Operand2::Reg { rm: 1, shift: Shift::Imm(ShiftType::Lsr, 32) }
            }
        );
        assert!(matches!(d(0xE1A0_0061), Insn::Dp { op2: Operand2::Reg { shift: Shift::Rrx, .. }, .. }));
        // ADD r0, r1, r2, LSL r3
        assert_eq!(
            d(0xE081_0312),
            Insn::Dp {
                op: DpOp::Add,
                s: false,
                rd: 0,
                rn: 1,
                op2: Operand2::Reg { rm: 2, shift: Shift::Reg(ShiftType::Lsl, 3) }
            }
        );
        // MOV r0, #&FF000000: a rotated immediate
        assert_eq!(
            d(0xE3A0_04FF),
            Insn::Dp { op: DpOp::Mov, s: false, rd: 0, rn: 0, op2: Operand2::Imm { value: 0xFF00_0000, rotated: true } }
        );
    }

    #[test]
    fn misc_and_psr() {
        assert_eq!(d(0xE10F_0000), Insn::Mrs { rd: 0, spsr: false });
        // MSR CPSR_f, #&10000000
        assert_eq!(d(0xE328_F201), Insn::Msr { spsr: false, mask: 8, src: MsrSrc::Imm(0x1000_0000) });
        // MSR CPSR_c, r0
        assert_eq!(d(0xE121_F000), Insn::Msr { spsr: false, mask: 1, src: MsrSrc::Reg(0) });
        assert_eq!(d(0xE12F_FF1E), Insn::Bx { link: false, rm: 14 });
        assert_eq!(d(0xE12F_FF31), Insn::Bx { link: true, rm: 1 });
        assert_eq!(d(0xE16F_0F11), Insn::Clz { rd: 0, rm: 1 });
        assert_eq!(d(0xE320_F000), Insn::Nop);
        assert_eq!(d(0xF5D1_F000), Insn::Nop); // PLD [r1]
        assert_eq!(d(0xE301_2345), Insn::MovHalf { top: false, rd: 2, imm: 0x1345 });
    }

    #[test]
    fn loads_and_stores() {
        // LDR r12, [r2], #4
        assert_eq!(
            d(0xE492_C004),
            Insn::Mem { load: true, width: Width::Word, rt: 12, rn: 2, offset: Offset::Imm(4), add: true, pre: false, wback: false }
        );
        // LDR r1, [r1, r0, LSL #2]
        assert_eq!(
            d(0xE791_1100),
            Insn::Mem {
                load: true,
                width: Width::Word,
                rt: 1,
                rn: 1,
                offset: Offset::Reg { rm: 0, shift: Shift::Imm(ShiftType::Lsl, 2) },
                add: true,
                pre: true,
                wback: false
            }
        );
        // STRB r0, [r1, #-1]!
        assert_eq!(
            d(0xE561_0001),
            Insn::Mem { load: false, width: Width::Byte, rt: 0, rn: 1, offset: Offset::Imm(1), add: false, pre: true, wback: true }
        );
        // LDRH r0, [r1, #2]; LDRSB r0, [r1]; STRD r2, [sp, #8]
        assert_eq!(
            d(0xE1D1_00B2),
            Insn::Mem { load: true, width: Width::Half, rt: 0, rn: 1, offset: Offset::Imm(2), add: true, pre: true, wback: false }
        );
        assert!(matches!(d(0xE1D1_00D0), Insn::Mem { load: true, width: Width::SignedByte, .. }));
        assert_eq!(
            d(0xE1CD_20F8),
            Insn::Mem { load: false, width: Width::Double, rt: 2, rn: 13, offset: Offset::Imm(8), add: true, pre: true, wback: false }
        );
        // STMFD sp!, {r4, lr} and LDMFD sp!, {r4, pc}
        assert_eq!(
            d(0xE92D_4010),
            Insn::Block { load: false, rn: 13, regs: 0x4010, before: true, add: false, wback: true, user: false }
        );
        assert_eq!(
            d(0xE8BD_8010),
            Insn::Block { load: true, rn: 13, regs: 0x8010, before: false, add: true, wback: true, user: false }
        );
    }

    #[test]
    fn branches_and_swis() {
        // B .+8 is offset 0; BL .-8 is offset -16
        assert_eq!(d(0xEA00_0000), Insn::Branch { link: false, offset: 0 });
        assert_eq!(d(0xEBFF_FFFC), Insn::Branch { link: true, offset: -16 });
        assert_eq!(d(0xEF02_001E), Insn::Swi { number: 0x2001E });
        let x = decode(0x1AFF_FFFE);
        assert_eq!(x.cond, Cond::Ne);
        assert_eq!(x.insn, Insn::Branch { link: false, offset: -8 });
    }

    /// VFP words as llvm-mc encodes them (-triple armv7a -mattr=+vfp4).
    #[test]
    fn vfp() {
        assert_eq!(d(0xED90_1B02), Insn::VfpMem { load: true, double: true, v: 1, rn: 0, offset: 8, add: true });
        assert_eq!(d(0xED81_2B00), Insn::VfpMem { load: false, double: true, v: 2, rn: 1, offset: 0, add: true });
        assert_eq!(d(0xEE20_2B00), Insn::VfpArith { op: VfpOp::Mul, double: true, vd: 2, vn: 0, vm: 0 });
        assert_eq!(d(0xEE01_2B01), Insn::VfpArith { op: VfpOp::Mla, double: true, vd: 2, vn: 1, vm: 1 });
        assert_eq!(d(0xEEA0_3B01), Insn::VfpArith { op: VfpOp::Fma, double: true, vd: 3, vn: 0, vm: 1 });
        assert_eq!(d(0xEE1B_AB4C), Insn::VfpArith { op: VfpOp::Nmla, double: true, vd: 10, vn: 11, vm: 12 });
        assert_eq!(d(0xEE83_3A84), Insn::VfpArith { op: VfpOp::Div, double: false, vd: 6, vn: 7, vm: 8 });
        assert_eq!(d(0xEE7F_FA6E), Insn::VfpArith { op: VfpOp::Sub, double: false, vd: 31, vn: 30, vm: 29 });
        assert_eq!(d(0xEE71_0BA2), Insn::VfpArith { op: VfpOp::Add, double: true, vd: 16, vn: 17, vm: 18 });
        assert_eq!(d(0xEEB1_2BC2), Insn::VfpUnary { op: VfpUnary::Sqrt, double: true, vd: 2, vm: 2 });
        assert_eq!(d(0xEEB1_2A62), Insn::VfpUnary { op: VfpUnary::Neg, double: false, vd: 4, vm: 5 });
        assert_eq!(d(0xEEB0_8BC9), Insn::VfpUnary { op: VfpUnary::Abs, double: true, vd: 8, vm: 9 });
        assert_eq!(d(0xEEB5_2B40), Insn::VfpCompare { double: true, exception: false, vd: 2, vm: None });
        assert_eq!(d(0xEEB4_0AE0), Insn::VfpCompare { double: false, exception: true, vd: 0, vm: Some(1) });
        assert_eq!(d(0xEEF1_FA10), Insn::VfpStatus { to_core: true, rt: 15 });
        assert_eq!(d(0xEEF1_4A10), Insn::VfpStatus { to_core: true, rt: 4 });
        assert_eq!(d(0xEEE1_4A10), Insn::VfpStatus { to_core: false, rt: 4 });
        assert_eq!(d(0xEEB7_4AC1), Insn::VfpCvt { cvt: VfpCvt::Precision { to_double: true }, vd: 4, vm: 2 });
        assert_eq!(
            d(0xEEBD_0BC2),
            Insn::VfpCvt { cvt: VfpCvt::ToInt { signed: true, double: true, toward_zero: true }, vd: 0, vm: 2 }
        );
        assert_eq!(
            d(0xEEBD_0B42),
            Insn::VfpCvt { cvt: VfpCvt::ToInt { signed: true, double: true, toward_zero: false }, vd: 0, vm: 2 }
        );
        assert_eq!(d(0xEEB8_5BE0), Insn::VfpCvt { cvt: VfpCvt::FromInt { signed: true, double: true }, vd: 5, vm: 1 });
        assert_eq!(d(0xEE10_2A10), Insn::VfpMovCore { to_core: true, sn: 0, rt: 2 });
        assert_eq!(d(0xEE00_3A90), Insn::VfpMovCore { to_core: false, sn: 1, rt: 3 });
        assert_eq!(d(0xEC53_2B16), Insn::VfpMovCore2 { to_core: true, double: true, vm: 6, rt: 2, rt2: 3 });
        assert_eq!(d(0xEC43_2B16), Insn::VfpMovCore2 { to_core: false, double: true, vm: 6, rt: 2, rt2: 3 });
        assert_eq!(d(0xEEB7_7B08), Insn::VfpMovImm { double: true, vd: 7, bits: 1.5f64.to_bits() });
        assert_eq!(d(0xEEF8_1A00), Insn::VfpMovImm { double: false, vd: 3, bits: (-2.0f32).to_bits() as u64 });
        assert_eq!(
            d(0xED2D_8B04),
            Insn::VfpMulti { load: false, double: true, first: 8, count: 2, rn: 13, add: false, pre: true, wback: true }
        );
        assert_eq!(
            d(0xECBD_8B04),
            Insn::VfpMulti { load: true, double: true, first: 8, count: 2, rn: 13, add: true, pre: false, wback: true }
        );
    }

    /// FPA words as rosasm's own encoder (fpa.rs) writes them, which is how
    /// ObjAsm wrote them for the emulator.
    #[test]
    fn fpa() {
        let w = |m: &str, o: &str| match crate::fpa::encode(m, o) {
            Some(crate::legalize::Legalized::RawWord(w)) => w,
            other => panic!("{m} {o}: {other:?}"),
        };
        assert_eq!(
            d(w("ADFD", "f0, f1, f2")),
            Insn::FpaDyadic { op: FpaDyadic::Adf, prec: Prec::Double, round: Round::Nearest, fd: 0, fn_: 1, fm: FpaOperand::Reg(2) }
        );
        assert_eq!(
            d(w("RSFS", "f3, f4, #0.5")),
            Insn::FpaDyadic { op: FpaDyadic::Rsf, prec: Prec::Single, round: Round::Nearest, fd: 3, fn_: 4, fm: FpaOperand::Const(6) }
        );
        assert_eq!(
            d(w("POLE", "f1, f2, f3")),
            Insn::FpaDyadic { op: FpaDyadic::Pol, prec: Prec::Extended, round: Round::Nearest, fd: 1, fn_: 2, fm: FpaOperand::Reg(3) }
        );
        assert_eq!(
            d(w("SQTD", "f5, f6")),
            Insn::FpaMonadic { op: FpaMonadic::Sqt, prec: Prec::Double, round: Round::Nearest, fd: 5, fm: FpaOperand::Reg(6) }
        );
        assert_eq!(
            d(w("RNDDZ", "f0, f1")),
            Insn::FpaMonadic { op: FpaMonadic::Rnd, prec: Prec::Double, round: Round::Zero, fd: 0, fm: FpaOperand::Reg(1) }
        );
        assert_eq!(d(w("FLTS", "f2, r7")), Insn::FpaFlt { prec: Prec::Single, round: Round::Nearest, fn_: 2, rd: 7 });
        assert_eq!(d(w("FIXM", "r1, f3")), Insn::FpaFix { round: Round::Minus, rd: 1, fm: 3 });
        assert_eq!(d(w("WFS", "r2")), Insn::FpaStatus { write: true, control: false, rd: 2 });
        assert_eq!(d(w("RFS", "r2")), Insn::FpaStatus { write: false, control: false, rd: 2 });
        assert_eq!(
            d(w("CMF", "f0, #0")),
            Insn::FpaCompare { negate: false, exception: false, fn_: 0, fm: FpaOperand::Const(0) }
        );
        assert_eq!(
            d(w("CNFE", "f1, f2")),
            Insn::FpaCompare { negate: true, exception: true, fn_: 1, fm: FpaOperand::Reg(2) }
        );
        assert_eq!(
            d(w("LDFD", "f1, [r2, #8]")),
            Insn::FpaMem { load: true, prec: Prec::Double, fd: 1, rn: 2, offset: 8, add: true, pre: true, wback: false }
        );
        assert_eq!(
            d(w("STFE", "f7, [r13, #-12]!")),
            Insn::FpaMem { load: false, prec: Prec::Extended, fd: 7, rn: 13, offset: 12, add: false, pre: true, wback: true }
        );
        assert_eq!(
            d(w("SFM", "f4, 4, [r13, #-48]!")),
            Insn::FpaMulti { load: false, fd: 4, count: 4, rn: 13, offset: 48, add: false, pre: true, wback: true }
        );
        assert_eq!(
            d(w("LFM", "f4, 2, [r0]")),
            Insn::FpaMulti { load: true, fd: 4, count: 2, rn: 0, offset: 0, add: true, pre: true, wback: false }
        );
    }

    #[test]
    fn multiplies() {
        // MUL r0, r1, r2; UMULL r0, r1, r2, r3
        assert_eq!(d(0xE000_0291), Insn::Mul { op: MulOp::Mul, s: false, rd: 0, ra: 0, rm: 1, rs: 2 });
        assert_eq!(d(0xE081_0392), Insn::Mul { op: MulOp::Umull, s: false, rd: 1, ra: 0, rm: 2, rs: 3 });
    }
}
