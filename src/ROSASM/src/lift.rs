//! Lifting: from instructions to the expressions they compute.
//!
//! Tier 0 compiles each instruction to one statement that does exactly what
//! the instruction does to the state block. That is exact, but it reads
//! like the machine. Every register lives in the state, the flags are
//! computed wherever an instruction sets them, and there is one statement
//! per instruction. This module instead compiles a *block*, meaning
//! straight-line code entered only at its top, to the C expressions its
//! instructions compute. This is tier 1 of `--emit c`.
//! It works as follows:
//!
//! - Every value is a node in one expression graph. That covers the integer
//!   registers, floating point (as standard C floating point), and
//!   each of the flags N, Z, C and V.
//! - A register is written to the state where it is assigned, if a transfer,
//!   a call or the next block needs it there. Otherwise it is not written.
//! - A value read once is folded into the expression that reads it. A value
//!   read more than once is read from the register it was assigned to, so
//!   the register serves as the variable. Where that register has since
//!   been overwritten, the value is read from a C variable such as `v1`.
//! - A flag-setting instruction sets no flags in the state unless something
//!   needs them there. Instead its flags become the conditions that read
//!   them: `R[0] == 5`, `(int32_t)R[1] < 0`, `R[1] == 1 || R[1] == 2`. Where
//!   a transfer or a call needs the flags, they are set as tier 0 sets them.
//! - A conditional instruction assigns `c ? new : old`, written out as
//!   `if (c) R[0] = new;` where the register holds the old value.
//! - A block may leave early. A conditional branch or return is a side
//!   exit, and before it, whatever is live at its target is put in the
//!   state.
//! - Loads fold only where no store could have changed what they read. A load
//!   from the unit's image, which is read-only, is a constant.
//!
//! Planning is by trial. A pass compiles the block under a plan, and notes
//! each read the plan cannot satisfy. Examples are a value wanted in the
//! state that is not there, and a register overwritten before its value is
//! read. The plan grows until a pass notes none. Since it only grows, it
//! settles, and the last pass is the output.
//!
//! The result is exact wherever it can be observed. Every register and flag
//! that is live at a transfer, a call or a SWI holds what tier 0 would have
//! put there. There is one assumption: the FPSCR's NZCV flags, which a VCMP
//! writes, are dead once the VMRS after it has read them.

use std::collections::{HashMap, HashSet};

use crate::a32::{
    Cond, Decoded, DpOp, FpaDyadic, FpaMonadic, FpaOperand, Insn, MulOp, Offset, Operand2, Prec, Round, Shift,
    ShiftType, VfpCvt, VfpOp, VfpUnary, Width,
};

// ---- registers and what is live ------------------------------------------------

/// A register the lifter tracks: ARM's r0-r14, FPA's f0-f7, VFP's d0-d31 and
/// s0-s31 (the halves of d0-d15), and the flags.
#[derive(Clone, Copy, PartialEq, Eq, PartialOrd, Ord, Hash, Debug)]
pub enum Reg {
    R(u8),
    F(u8),
    D(u8),
    S(u8),
    /// N, Z, C and V: 0 to 3.
    Flag(u8),
    /// A word of a private frame, held as a C local. It is the word at the
    /// routine's entry sp minus four times this number (1 to 128).
    Slot(u8),
}

const N: u8 = 0;
const Z: u8 = 1;
const C: u8 = 2;
const V: u8 = 3;

impl Reg {
    fn c(self) -> String {
        match self {
            Reg::R(n) => format!("R[{n}]"),
            Reg::F(n) => format!("F[{n}]"),
            Reg::D(n) => format!("D[{n}]"),
            Reg::S(n) => format!("S[{n}]"),
            Reg::Flag(k) => format!("s->{}", ["n", "z", "c", "v"][k as usize]),
            Reg::Slot(k) => format!("sp_{}", 4 * k as u32),
        }
    }

    /// Its bits in a set of FP registers. FPA's registers are bits 0-7.
    /// VFP's 32-bit halves are bits 8-71: s(k) is half k, and d(n) is halves
    /// 2n and 2n+1.
    pub fn fp_mask(self) -> u128 {
        match self {
            Reg::F(n) => 1u128 << n,
            Reg::S(k) => 1u128 << (8 + k as u32),
            Reg::D(n) => 3u128 << (8 + 2 * n as u32),
            _ => 0,
        }
    }

    fn ty(self) -> Ty {
        match self {
            Reg::R(_) | Reg::Slot(_) => Ty::I32,
            Reg::F(_) | Reg::D(_) => Ty::F64,
            Reg::S(_) => Ty::F32,
            Reg::Flag(_) => Ty::Bool,
        }
    }

    /// The other registers that share storage with this one.
    fn overlaps(self) -> Vec<Reg> {
        match self {
            Reg::D(n) if n < 16 => vec![Reg::S(2 * n), Reg::S(2 * n + 1)],
            Reg::S(k) => vec![Reg::D(k / 2)],
            _ => vec![],
        }
    }
}

/// Every floating-point register.
pub const ALL_FP: u128 = (1u128 << 72) - 1;

/// A set of registers and flags: what is live, or what an instruction reads
/// or writes.
#[derive(Clone, Copy, Default, PartialEq, Eq, Debug)]
pub struct Live {
    /// r0-r14, as bits 0-14.
    pub int: u16,
    pub fp: u128,
    /// NZCV, as bits 3-0.
    pub flags: u8,
    /// A private frame's words, `Reg::Slot(k)` as bit k-1.
    pub slots: u128,
}

impl Live {
    pub const ALL: Live = Live { int: 0x7FFF, fp: ALL_FP, flags: 0xF, slots: u128::MAX };

    pub fn has(&self, r: Reg) -> bool {
        match r {
            Reg::R(n) => self.int >> n & 1 != 0,
            Reg::Flag(k) => self.flags & (8 >> k) != 0,
            Reg::Slot(k) => self.slots >> (k - 1) & 1 != 0,
            _ => self.fp & r.fp_mask() != 0,
        }
    }

    pub fn or(self, o: Live) -> Live {
        Live { int: self.int | o.int, fp: self.fp | o.fp, flags: self.flags | o.flags, slots: self.slots | o.slots }
    }

    pub fn and_not(self, o: Live) -> Live {
        Live {
            int: self.int & !o.int,
            fp: self.fp & !o.fp,
            flags: self.flags & !o.flags,
            slots: self.slots & !o.slots,
        }
    }
}

// ---- a block, as the caller gives it ---------------------------------------------

/// One instruction of a block.
pub struct BlockInsn {
    pub addr: u32,
    pub d: Decoded,
    /// Set for an instruction the caller compiles itself: a transfer, a
    /// call, a SWI, or anything compiled tier 0's way.
    pub caller: Option<Caller>,
}

/// What an instruction the caller compiles does to the state.
#[derive(Clone, Copy, Default, Debug)]
pub struct Caller {
    /// What it reads from the state. For a transfer, this includes what is
    /// live at its target. It does not include the flags its condition
    /// reads, because the lifter supplies the condition.
    pub needs: Live,
    /// What it writes there, where control carries on in the block.
    pub writes: Live,
    pub writes_memory: bool,
    /// When it runs, control does not come back to the next instruction, as
    /// with a return, a jump or an exit. So if control carries on, its
    /// condition did not hold.
    pub leaves: bool,
}

pub struct BlockCtx<'a> {
    /// A word of the unit's image, which is read-only, and whether a
    /// relocation made it an address.
    pub rom_word: &'a dyn Fn(u32) -> Option<(u32, bool)>,
    /// The C name of an address in the image, where a label has one.
    pub label: &'a dyn Fn(u32) -> Option<String>,
    /// The name the source gave a constant used by the instruction at an
    /// address, such as `#b_WordAligned`, `[r12, #WsFlags]` or
    /// `=Service_Reset`, if the name has this value there.
    pub konst_name: &'a dyn Fn(u32, u32) -> Option<String>,
    /// For a constant's name that is a field of a storage map written as
    /// a C struct: the struct's tag (and accessor), and the field's size.
    pub field: &'a dyn Fn(&str) -> Option<(String, u32)>,
    /// In a routine whose frame is private, the frame word that the
    /// instruction at an address reaches at an offset from sp. The word is
    /// a C local (`Reg::Slot`), not memory.
    pub slot: &'a dyn Fn(u32, i32) -> Option<u8>,
    /// Whether control can fall out of the block's last instruction, and
    /// what the state must hold then.
    pub falls_out: bool,
    pub live_out: Live,
    /// Whether an FPA compare may leave the flags unset. It may not where
    /// the region writes the FPSR, because the FPSR's AC bit changes what C
    /// means after CMF.
    pub fpa_lift_ok: bool,
    /// The integer registers, of `r0`..`r14`, that are held in C locals
    /// rather than in the state block. For these, the locals are the state
    /// the lifter works with.
    pub locals: u16,
}

#[derive(Default, Debug)]
pub struct BlockOut {
    /// The C for each instruction. For one the lifter compiles, this is all
    /// of it. For the rest, it is what goes before the caller's own C.
    pub at: HashMap<u32, Vec<String>>,
    /// The C condition of each conditional instruction the caller compiles,
    /// and its negation.
    pub cond: HashMap<u32, String>,
    pub cond_inv: HashMap<u32, String>,
    /// The C variables the block's code uses, such as `uint32_t v1`. They
    /// are declared where the routine starts, so that structured code can
    /// reach them anywhere.
    pub decls: Vec<String>,
}

/// Whether the lifter compiles an instruction, rather than its caller.
pub fn lifts(d: &Decoded) -> bool {
    match d.insn {
        Insn::Dp { rd, .. } => rd != 15,
        Insn::Mul { op, s, .. } => !(s && !matches!(op, MulOp::Mul | MulOp::Mla | MulOp::Mls)),
        Insn::Mem { load, rt, .. } => !(load && rt == 15),
        Insn::Block { load, regs, user, .. } => {
            !(user && regs & 0x8000 != 0) && !(load && regs & 0x8000 != 0)
        }
        Insn::Clz { .. } | Insn::MovHalf { .. } | Insn::Nop => true,
        Insn::NeonLane { .. } | Insn::NeonDup { .. } | Insn::NeonPadd { .. } => true,
        ref i => is_fp(i),
    }
}

// ---- values -----------------------------------------------------------------------

#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum Ty {
    I32,
    I64,
    /// A condition or a flag: 0 or 1.
    Bool,
    F32,
    F64,
}

impl Ty {
    fn c(self) -> &'static str {
        match self {
            Ty::I32 => "uint32_t",
            Ty::I64 => "uint64_t",
            Ty::Bool => "int",
            Ty::F32 => "float",
            Ty::F64 => "double",
        }
    }

    fn is_fp(self) -> bool {
        matches!(self, Ty::F32 | Ty::F64)
    }
}

#[derive(Clone, Copy, PartialEq, Eq, Debug)]
enum Op {
    Add,
    Sub,
    Mul,
    Div,
    And,
    Or,
    Xor,
    Shl,
    Shr,
    Asr,
    /// Unsigned for integers.
    Eq,
    Ne,
    Lt,
    Le,
    Gt,
    Ge,
    /// Signed.
    SLt,
    SLe,
    SGt,
    SGe,
}

impl Op {
    fn c(self) -> &'static str {
        use Op::*;
        match self {
            Add => "+",
            Sub => "-",
            Mul => "*",
            Div => "/",
            And => "&",
            Or => "|",
            Xor => "^",
            Shl => "<<",
            Shr | Asr => ">>",
            Eq => "==",
            Ne => "!=",
            Lt | SLt => "<",
            Le | SLe => "<=",
            Gt | SGt => ">",
            Ge | SGe => ">=",
        }
    }

    /// C's precedence, from 13 for multiplication down to 6 for `|`. (5 is
    /// `&&` and 4 is `||`.)
    fn prec(self) -> u8 {
        use Op::*;
        match self {
            Mul | Div => 13,
            Add | Sub => 12,
            Shl | Shr | Asr => 11,
            Lt | Le | Gt | Ge | SLt | SLe | SGt | SGe => 10,
            Eq | Ne => 9,
            And => 8,
            Xor => 7,
            Or => 6,
        }
    }

    fn is_cmp(self) -> bool {
        matches!(self.prec(), 9 | 10)
    }

    fn signed(self) -> bool {
        matches!(self, Op::SLt | Op::SLe | Op::SGt | Op::SGe)
    }

    /// Whether `a op (b op' c)` needs no parentheses in integers, where op'
    /// has the same precedence. That is so for modular addition and
    /// multiplication and for the bitwise operations, which regroup exactly.
    fn assoc(self) -> bool {
        matches!(self, Op::Add | Op::Mul | Op::And | Op::Or | Op::Xor)
    }

    /// Whether `x = x op y` may be written `x op= y`.
    fn compound(self) -> bool {
        matches!(self, Op::Add | Op::Sub | Op::Mul | Op::Div | Op::And | Op::Or | Op::Xor | Op::Shl | Op::Shr)
    }

    /// The integer comparison that holds when this one does not.
    fn negated(self) -> Option<Op> {
        use Op::*;
        Some(match self {
            Eq => Ne,
            Ne => Eq,
            Lt => Ge,
            Ge => Lt,
            Le => Gt,
            Gt => Le,
            SLt => SGe,
            SGe => SLt,
            SLe => SGt,
            SGt => SLe,
            _ => return None,
        })
    }
}

/// An argument of a call: a value, or fixed C text (a rounding mode).
#[derive(Clone, Debug)]
enum Arg {
    V(usize),
    T(&'static str),
}

/// What a flag-setting instruction compared. Its flags are worked out from
/// this.
#[derive(Clone, Copy, PartialEq, Debug)]
enum Src {
    /// a - b: SUBS, RSBS, CMP, and CMN or ADDS of a constant.
    Sub { a: usize, b: usize, res: usize },
    /// a + b: ADDS and CMN.
    Add { a: usize, b: usize, res: usize },
    /// N and Z of a result: the logical operations, and MULS.
    Nz { res: usize },
    /// ADCS, SBCS and RSCS, whose flags only tier 0's helpers set.
    Carry,
    /// A floating-point compare: FPA's CMF (AC clear), or VFP's VCMP by way
    /// of VMRS.
    FCmp { a: usize, b: usize, vfp: bool },
}

#[derive(Clone, Debug)]
enum Node {
    /// A register's value in the state. It is read here for the first time,
    /// or read again after something the lifter does not follow changed
    /// the state.
    Entry(Reg),
    /// The bits of a VFP register in the state, `SW[k]` or `DW[n]`, read
    /// where the instruction at `at` is.
    Bits { r: Reg, at: usize },
    Int(u32),
    /// An address made from pc: a label's name, where it has one.
    Addr(u32),
    Lit(f64),
    Neg(usize),
    Not(usize),
    LNot(usize),
    Bin(Op, usize, usize),
    /// A load: its helper and address, and how many stores came before it.
    Load { f: &'static str, addr: usize, mem: u32 },
    /// A pure function: a runtime helper, or <math.h>'s.
    Call(&'static str, Vec<Arg>),
    Cast(Ty, usize),
    /// `(uint32_t)(int8_t)x`, `(uint32_t)(int16_t)x`.
    SExt(u8, usize),
    /// An integer converted to floating point.
    FromInt { src: usize, signed: bool },
    /// `c ? a : b`.
    Select(usize, usize, usize),
    /// A flag of the flag-setting instruction at `insn`.
    Flag { src: Src, bit: u8, insn: usize },
    /// A condition code, over the flags' values N, Z, C, V.
    Cond(Cond, [usize; 4]),
    Mul64 { signed: bool, a: usize, b: usize },
    /// `(uint64_t)hi << 32 | lo`.
    Wide { hi: usize, lo: usize },
    Lo(usize),
    Hi(usize),
}

struct Val {
    node: Node,
    ty: Ty,
    /// The instruction that made it.
    insn: usize,
}

// ---- small pieces -----------------------------------------------------------------

const FPA_CONST: [f64; 8] = [0.0, 1.0, 2.0, 3.0, 4.0, 5.0, 0.5, 10.0];

/// A floating-point literal, exactly. A single is written as its shortest
/// decimal with `f`, a double as its shortest decimal.
fn lit(v: f64, ty: Ty) -> String {
    if v.is_nan() {
        return "NAN".into();
    }
    if v.is_infinite() {
        return if v > 0.0 { "INFINITY".into() } else { "-INFINITY".into() };
    }
    let mut t = match ty {
        Ty::F32 => format!("{}", v as f32),
        _ => format!("{v}"),
    };
    if !t.contains(['.', 'e', 'E']) {
        t.push_str(".0");
    }
    if ty == Ty::F32 {
        t.push('f');
    }
    t
}

/// An integer literal. It is decimal, unless hex says more, as it does for
/// large numbers and masks.
fn int_text(k: u32, ty: Ty) -> String {
    if ty == Ty::Bool {
        return k.to_string();
    }
    let mask = k >= 15 && k.wrapping_add(1).is_power_of_two();
    if k >= 4096 || mask || (k >= 256 && k.is_power_of_two()) || k == u32::MAX {
        format!("0x{k:X}u")
    } else {
        k.to_string()
    }
}

/// A constant in a bitwise operation, written as a mask. It is in hex
/// beyond one digit, and written as `~k` where that is shorter, as in
/// `x & ~3u` for a BIC.
fn mask_text(k: u32) -> String {
    match (k, !k) {
        (k, _) if k <= 9 => k.to_string(),
        (_, n) if n <= 9 => format!("~{n}u"),
        (_, n) if n <= 0xFFFF => format!("~0x{n:X}u"),
        (k, _) => format!("0x{k:X}u"),
    }
}

/// A constant compared as a signed number.
fn signed_text(k: u32) -> String {
    match k as i32 {
        i32::MIN => "INT32_MIN".into(),
        s => s.to_string(),
    }
}

fn round_c(r: Round) -> &'static str {
    match r {
        Round::Nearest => "ROS_ROUND_NEAREST",
        Round::Plus => "ROS_ROUND_PLUS",
        Round::Minus => "ROS_ROUND_MINUS",
        Round::Zero => "ROS_ROUND_ZERO",
    }
}

/// The flags a condition reads.
fn cond_reads(c: Cond) -> &'static [u8] {
    use Cond::*;
    match c {
        Eq | Ne => &[Z],
        Cs | Cc => &[C],
        Mi | Pl => &[N],
        Vs | Vc => &[V],
        Hi | Ls => &[C, Z],
        Ge | Lt => &[N, V],
        Gt | Le => &[Z, N, V],
        Al => &[],
    }
}

/// The condition that holds when c does not.
pub fn invert(c: Cond) -> Cond {
    use Cond::*;
    match c {
        Eq => Ne,
        Ne => Eq,
        Cs => Cc,
        Cc => Cs,
        Mi => Pl,
        Pl => Mi,
        Vs => Vc,
        Vc => Vs,
        Hi => Ls,
        Ls => Hi,
        Ge => Lt,
        Lt => Ge,
        Gt => Le,
        Le => Gt,
        Al => Al,
    }
}

/// Whether a condition holds on flags N, Z, C, V.
fn holds(c: Cond, f: [bool; 4]) -> bool {
    let [n, z, c_, v] = f;
    use Cond::*;
    match c {
        Eq => z,
        Ne => !z,
        Cs => c_,
        Cc => !c_,
        Mi => n,
        Pl => !n,
        Vs => v,
        Vc => !v,
        Hi => c_ && !z,
        Ls => !c_ || z,
        Ge => n == v,
        Lt => n != v,
        Gt => !z && n == v,
        Le => z || n != v,
        Al => true,
    }
}

/// Tier 0's test of a condition, on the state's flags.
fn tier0_cond(c: Cond) -> (String, u8) {
    use Cond::*;
    let (t, p) = match c {
        Eq => ("s->z", 15),
        Ne => ("!s->z", 14),
        Cs => ("s->c", 15),
        Cc => ("!s->c", 14),
        Mi => ("s->n", 15),
        Pl => ("!s->n", 14),
        Vs => ("s->v", 15),
        Vc => ("!s->v", 14),
        Hi => ("ros_cond(s, ROS_HI)", 15),
        Ls => ("ros_cond(s, ROS_LS)", 15),
        Ge => ("ros_cond(s, ROS_GE)", 15),
        Lt => ("ros_cond(s, ROS_LT)", 15),
        Gt => ("ros_cond(s, ROS_GT)", 15),
        Le => ("ros_cond(s, ROS_LE)", 15),
        Al => ("1", 15),
    };
    (t.to_string(), p)
}

/// The C predicate for condition `c` after comparing floating-point a with
/// b. It is exact for unordered operands too. After FPA's CMF (with AC
/// clear), C means >=. After VFP's VCMP, it means >= or unordered.
pub fn predicate(c: Cond, a: &str, b: &str, vfp: bool) -> Option<String> {
    let not = |s: String| format!("!({s})");
    let (lt, le) = (format!("{a} < {b}"), format!("{a} <= {b}"));
    let (gt, ge) = (format!("{a} > {b}"), format!("{a} >= {b}"));
    Some(match (c, vfp) {
        (Cond::Eq, _) => format!("{a} == {b}"),
        (Cond::Ne, _) => format!("{a} != {b}"),
        (Cond::Mi, _) => lt,
        (Cond::Pl, _) => not(lt),
        (Cond::Vs, _) => format!("isunordered({a}, {b})"),
        (Cond::Vc, _) => format!("!isunordered({a}, {b})"),
        (Cond::Ge, _) => ge,
        (Cond::Lt, _) => not(ge),
        (Cond::Gt, _) => gt,
        (Cond::Le, _) => not(gt),
        (Cond::Cs, false) => ge,
        (Cond::Cc, false) => not(ge),
        (Cond::Hi, false) => gt,
        (Cond::Ls, false) => not(gt),
        (Cond::Cs, true) => not(lt),
        (Cond::Cc, true) => lt,
        (Cond::Hi, true) => not(le),
        (Cond::Ls, true) => le,
        (Cond::Al, _) => return None,
    })
}

/// Integer constants folded, as the machine computes them.
fn fold(op: Op, x: u32, y: u32) -> Option<u32> {
    use Op::*;
    Some(match op {
        Add => x.wrapping_add(y),
        Sub => x.wrapping_sub(y),
        Mul => x.wrapping_mul(y),
        And => x & y,
        Or => x | y,
        Xor => x ^ y,
        Shl if y < 32 => x << y,
        Shr if y < 32 => x >> y,
        Asr if y < 32 => ((x as i32) >> y) as u32,
        Eq => (x == y) as u32,
        Ne => (x != y) as u32,
        Lt => (x < y) as u32,
        Le => (x <= y) as u32,
        Gt => (x > y) as u32,
        Ge => (x >= y) as u32,
        SLt => ((x as i32) < y as i32) as u32,
        SLe => (x as i32 <= y as i32) as u32,
        SGt => (x as i32 > y as i32) as u32,
        SGe => (x as i32 >= y as i32) as u32,
        _ => return None,
    })
}

fn bits_text(r: Reg) -> String {
    match r {
        Reg::D(n) => format!("DW[{n}]"),
        Reg::S(k) => format!("SW[{k}]"),
        r => r.c(),
    }
}

/// `if (c) a;` and `if (c) b;` side by side are one `if`.
fn merge_ifs(s: Vec<String>) -> Vec<String> {
    fn split(s: &str) -> Option<(&str, &str)> {
        let rest = s.strip_prefix("if (")?;
        let mut depth = 1;
        for (k, ch) in rest.char_indices() {
            match ch {
                '(' => depth += 1,
                ')' => {
                    depth -= 1;
                    if depth == 0 {
                        let body = rest[k + 1..].trim_start();
                        return (!body.starts_with('{')).then_some((&rest[..k], body));
                    }
                }
                _ => {}
            }
        }
        None
    }
    let mut out = vec![];
    let mut i = 0;
    while i < s.len() {
        if let Some((cond, body)) = split(&s[i]) {
            let mut bodies = vec![body.to_string()];
            let mut j = i + 1;
            while let Some((c2, b2)) = s.get(j).and_then(|t| split(t)) {
                if c2 != cond {
                    break;
                }
                bodies.push(b2.to_string());
                j += 1;
            }
            if bodies.len() > 1 {
                out.push(format!("if ({cond}) {{"));
                out.extend(bodies.into_iter().map(|b| format!("    {b}")));
                out.push("}".into());
                i = j;
                continue;
            }
        }
        out.push(s[i].clone());
        i += 1;
    }
    out
}

// ---- the plan -------------------------------------------------------------------------

#[derive(Default, Clone)]
struct Plan {
    /// Values given a C variable where they are made.
    temp: HashSet<usize>,
    /// Values not to be folded: they are read more than once.
    no_inline: HashSet<usize>,
    /// Assignments, as (instruction, register), that are written to the
    /// state where they happen.
    events: HashSet<(usize, Reg)>,
    /// Flag-setting instructions that set the state's flags, as tier 0 does.
    flags: HashSet<usize>,
}

// ---- a pass ------------------------------------------------------------------------------

struct Pass<'a> {
    ctx: &'a BlockCtx<'a>,
    block: &'a [BlockInsn],
    plan: &'a Plan,
    vals: Vec<Val>,
    /// Constants the source named, keyed by value. Each is written as its
    /// name.
    names: HashMap<usize, String>,
    /// Each register's value here.
    cur: HashMap<Reg, usize>,
    /// What the state holds, where a value here stands for it.
    state: HashMap<Reg, usize>,
    /// Each register's last assignment: (instruction, value).
    last: HashMap<Reg, (usize, usize)>,
    /// Where each register's state was last written.
    written: HashMap<Reg, usize>,
    /// A value's home: the instruction that made it and the register it
    /// assigned it to there.
    home: HashMap<usize, (usize, Reg)>,
    /// How many stores there have been so far. A load's value still holds
    /// only while this is unchanged.
    mem: u32,
    temps: HashMap<usize, u32>,
    next_temp: u32,
    /// The instruction being compiled.
    i: usize,
    /// Its statements, in two parts. `pre` holds reads of the state, which
    /// are safe whatever the condition. `stmts` holds the rest.
    pre: Vec<String>,
    stmts: Vec<String>,
    /// A VCMP waiting for the VMRS that brings its result to the flags.
    vcmp: Option<(usize, usize)>,
    out: BlockOut,
    /// What this pass found the plan lacks.
    up: Plan,
    /// How many times each value was written out in full.
    inlined: HashMap<usize, u32>,
    /// Variables read.
    temp_reads: HashSet<usize>,
    /// One node per condition on the same flags.
    conds: HashMap<(u8, [usize; 4]), usize>,
    /// Loads made by a conditional instruction. Each is evaluated only
    /// where its condition holds. When the condition fails, the load does
    /// not happen, and the memory it would have read need not exist.
    guarded: HashSet<usize>,
}

impl<'a> Pass<'a> {
    fn new(ctx: &'a BlockCtx<'a>, block: &'a [BlockInsn], plan: &'a Plan, next_temp: u32) -> Self {
        Pass {
            ctx,
            block,
            plan,
            vals: vec![],
            cur: HashMap::new(),
            state: HashMap::new(),
            last: HashMap::new(),
            written: HashMap::new(),
            home: HashMap::new(),
            mem: 0,
            temps: HashMap::new(),
            next_temp,
            i: 0,
            pre: vec![],
            stmts: vec![],
            vcmp: None,
            out: BlockOut::default(),
            up: Plan::default(),
            inlined: HashMap::new(),
            temp_reads: HashSet::new(),
            names: HashMap::new(),
            conds: HashMap::new(),
            guarded: HashSet::new(),
        }
    }

    // ---- growing the plan ----

    fn want_temp(&mut self, v: usize) {
        if !self.plan.temp.contains(&v) {
            self.up.temp.insert(v);
        }
    }

    fn want_event(&mut self, i: usize, r: Reg) {
        if !self.plan.events.contains(&(i, r)) {
            self.up.events.insert((i, r));
        }
    }

    fn want_flags(&mut self, i: usize) {
        if !self.plan.flags.contains(&i) {
            self.up.flags.insert(i);
        }
    }

    /// Whether this instruction sets the state's flags.
    fn flags_in_state(&self) -> bool {
        self.plan.flags.contains(&self.i)
    }

    // ---- values ----

    fn val(&mut self, node: Node, ty: Ty) -> usize {
        let load = matches!(node, Node::Load { .. });
        self.vals.push(Val { node, ty, insn: self.i });
        let v = self.vals.len() - 1;
        if load && self.block.get(self.i).is_some_and(|b| b.d.cond != Cond::Al) {
            self.guarded.insert(v);
        }
        if self.plan.temp.contains(&v) {
            self.declare(v);
        }
        v
    }

    /// A value's C variable, set where the value is made.
    fn declare(&mut self, v: usize) {
        let (text, _) = self.expr(v);
        let n = self.next_temp;
        self.next_temp += 1;
        self.out.decls.push(format!("{} v{n}", self.vals[v].ty.c()));
        let decl = format!("v{n} = {text};");
        // Reading the state is safe whatever the instruction's condition,
        // and must happen before the instruction changes it.
        if matches!(self.vals[v].node, Node::Entry(_) | Node::Bits { .. }) {
            self.pre.push(decl);
        } else {
            self.stmts.push(decl);
        }
        self.temps.insert(v, n);
    }

    fn int(&mut self, k: u32) -> usize {
        self.val(Node::Int(k), Ty::I32)
    }

    /// A named constant's C: its name, or where it is a field of a struct,
    /// its offset there.
    fn name_text(&self, v: usize) -> Option<String> {
        let n = self.names.get(&v)?;
        Some(match (self.ctx.field)(n) {
            Some((tag, _)) => format!("offsetof(struct {tag}, {n})"),
            None => n.clone(),
        })
    }

    /// A load or store at `addr`, written as a struct member such as
    /// `ws(r12)->Flags`. The address must be a base plus a named field of
    /// the access's width.
    fn member(&mut self, addr: usize, f: &str) -> Option<String> {
        let width = match f {
            "ros_ld32" | "ros_st32" => 4,
            "ros_ld16" | "ros_st16" => 2,
            "ros_ld8" | "ros_st8" => 1,
            _ => return None,
        };
        let Node::Bin(Op::Add, b, k) = self.vals[addr].node else { return None };
        let n = self.names.get(&k)?.clone();
        let (tag, size) = (self.ctx.field)(&n)?;
        if size != width {
            return None;
        }
        let bt = self.render(b);
        Some(format!("{tag}({bt})->{n}"))
    }

    /// A constant of the current instruction's, named if the source named it.
    fn named(&mut self, k: u32) -> usize {
        let v = self.int(k);
        if let Some(n) = self.block.get(self.i).and_then(|b| (self.ctx.konst_name)(b.addr, k)) {
            self.names.insert(v, n);
        }
        v
    }

    fn boolean(&mut self, b: bool) -> usize {
        self.val(Node::Int(b as u32), Ty::Bool)
    }

    fn addr(&mut self, a: u32) -> usize {
        self.val(Node::Addr(a), Ty::I32)
    }

    fn konst(&self, v: usize) -> Option<u32> {
        match self.vals[v].node {
            Node::Int(k) => Some(k),
            _ => None,
        }
    }

    fn bin(&mut self, op: Op, a: usize, b: usize) -> usize {
        let (ta, tb) = (self.vals[a].ty, self.vals[b].ty);
        let fp = ta.is_fp() || tb.is_fp();
        let ty = if op.is_cmp() {
            Ty::Bool
        } else if fp {
            if ta == Ty::F64 || tb == Ty::F64 { Ty::F64 } else { Ty::F32 }
        } else if ta == Ty::I64 || tb == Ty::I64 {
            Ty::I64
        } else {
            Ty::I32
        };
        if !fp && ty != Ty::I64 {
            // Two addresses make a number: `TEQ pc, pc` is 0.
            if let (Node::Addr(x), Node::Addr(y)) = (&self.vals[a].node, &self.vals[b].node) {
                let (x, y) = (*x, *y);
                if !matches!(op, Op::Add) {
                    if let Some(k) = fold(op, x, y) {
                        return if ty == Ty::Bool { self.boolean(k != 0) } else { self.int(k) };
                    }
                }
            }
            let (ka, kb) = (self.konst(a), self.konst(b));
            if let (Some(x), Some(y)) = (ka, kb) {
                if let Some(k) = fold(op, x, y) {
                    return if ty == Ty::Bool { self.boolean(k != 0) } else { self.int(k) };
                }
            }
            // A named field at offset 0 stays: `r12 + BufferBlockAt`.
            let named_zero = matches!(op, Op::Add | Op::Sub) && kb == Some(0) && self.names.contains_key(&b);
            match (op, ka, kb) {
                (Op::Add | Op::Sub | Op::Or | Op::Xor | Op::Shl | Op::Shr | Op::Asr, _, Some(0)) if !named_zero => return a,
                (Op::Add | Op::Or | Op::Xor, Some(0), _) => return b,
                (Op::And, _, Some(u32::MAX)) | (Op::Mul, _, Some(1)) => return a,
                (Op::And, Some(u32::MAX), _) | (Op::Mul, Some(1), _) => return b,
                (Op::And | Op::Mul, _, Some(0)) | (Op::And | Op::Mul, Some(0), _) => return self.int(0),
                (Op::Sub, Some(0), _) => return self.val(Node::Neg(b), Ty::I32),
                (Op::Add, _, Some(k)) if k >= 0x8000_0000 => {
                    let m = self.int(k.wrapping_neg());
                    return self.bin(Op::Sub, a, m);
                }
                (Op::Sub, _, Some(k)) if k > 0x8000_0000 => {
                    let m = self.int(k.wrapping_neg());
                    return self.bin(Op::Add, a, m);
                }
                _ => {}
            }
            // Pairs of shifts, written as C says them. They become a sign
            // extension, a zero extension or field extract, or a clearing
            // of the low bits.
            if let (Node::Bin(inner, x, s), Some(k)) = (self.vals[a].node.clone(), kb) {
                if let Some(j) = self.konst(s).filter(|j| (1..32).contains(j) && (1..32).contains(&k)) {
                    match (op, inner) {
                        (Op::Asr, Op::Shl) if j == k && (k == 16 || k == 24) => {
                            return self.val(Node::SExt(32 - k as u8, x), Ty::I32);
                        }
                        (Op::Shr, Op::Shl) if k >= j => {
                            let y = if k == j { x } else { let d = self.int(k - j); self.bin(Op::Shr, x, d) };
                            let m = self.int(u32::MAX >> k);
                            return self.bin(Op::And, y, m);
                        }
                        (Op::Shl, Op::Shr) if k == j => {
                            let m = self.int(u32::MAX << k);
                            return self.bin(Op::And, x, m);
                        }
                        _ => {}
                    }
                }
            }
            // An address moved by a constant is an address.
            if let (Node::Addr(x), Some(k)) = (&self.vals[a].node, kb) {
                let x = *x;
                match op {
                    Op::Add => return self.addr(x.wrapping_add(k)),
                    Op::Sub => return self.addr(x.wrapping_sub(k)),
                    _ => {}
                }
            }
        }
        self.val(Node::Bin(op, a, b), ty)
    }

    /// ~x.
    fn not_int(&mut self, a: usize) -> usize {
        // `~f_CallBackPending`, as the source's BIC named it.
        if self.names.contains_key(&a) {
            return self.val(Node::Not(a), Ty::I32);
        }
        if let Some(k) = self.konst(a) {
            return self.int(!k);
        }
        if let Node::Not(x) = self.vals[a].node {
            return x;
        }
        self.val(Node::Not(a), Ty::I32)
    }

    /// !c, for a condition or a flag.
    fn not(&mut self, b: usize) -> usize {
        match self.vals[b].node.clone() {
            Node::Int(k) => self.boolean(k == 0),
            Node::LNot(x) => x,
            Node::Bin(op, x, y) if !self.vals[x].ty.is_fp() => match op.negated() {
                Some(o) => self.bin(o, x, y),
                None => self.val(Node::LNot(b), Ty::Bool),
            },
            _ => self.val(Node::LNot(b), Ty::Bool),
        }
    }

    fn select(&mut self, c: usize, a: usize, b: usize) -> usize {
        if a == b {
            return a;
        }
        match self.konst(c) {
            Some(0) => return b,
            Some(_) => return a,
            None => {}
        }
        // c ? a : (c' ? x : y), where c' is the opposite of c, is
        // c' ? x : a. This is the shape MOVEQ followed by MOVNE makes.
        if let Node::Select(c2, x, _) = self.vals[b].node {
            if self.opposite(c, c2) && !self.is_guarded(x) {
                return self.select(c2, x, a);
            }
        }
        // Each arm knows which way c went.
        let a = self.assume(a, c, true, 4);
        let b = self.assume(b, c, false, 4);
        if a == b {
            return a;
        }
        let (ta, tb) = (self.vals[a].ty, self.vals[b].ty);
        let ty = match (ta, tb) {
            (Ty::Bool, Ty::Bool) => Ty::Bool,
            (x, y) if x.is_fp() || y.is_fp() => {
                if x == Ty::F64 || y == Ty::F64 { Ty::F64 } else { Ty::F32 }
            }
            (Ty::I64, _) | (_, Ty::I64) => Ty::I64,
            _ => Ty::I32,
        };
        self.val(Node::Select(c, a, b), ty)
    }

    /// v, given that condition c is known to have been `truth`. A select on
    /// c, or on its opposite, becomes the arm that was taken.
    fn assume(&mut self, v: usize, c: usize, truth: bool, depth: u32) -> usize {
        if depth == 0 {
            return v;
        }
        match self.vals[v].node.clone() {
            Node::Select(c2, x, y) if c2 == c && !self.is_guarded(if truth { x } else { y }) => {
                self.assume(if truth { x } else { y }, c, truth, depth - 1)
            }
            Node::Select(c2, x, y) if self.opposite(c2, c) && !self.is_guarded(if truth { y } else { x }) => {
                self.assume(if truth { y } else { x }, c, truth, depth - 1)
            }
            Node::Bin(op, x, y) => {
                let (x2, y2) = (self.assume(x, c, truth, depth - 1), self.assume(y, c, truth, depth - 1));
                if (x2, y2) == (x, y) {
                    v
                } else if self.vals[v].ty.is_fp() {
                    self.fbin(op, x2, y2)
                } else {
                    self.bin(op, x2, y2)
                }
            }
            _ => v,
        }
    }

    /// Whether two conditions are each other's negation.
    fn opposite(&self, c1: usize, c2: usize) -> bool {
        match (&self.vals[c1].node, &self.vals[c2].node) {
            (Node::Cond(a, x), Node::Cond(b, y)) => x == y && invert(*a) == *b,
            (Node::Select(s1, a1, b1), Node::Select(s2, a2, b2)) if s1 == s2 => {
                self.opposite(*a1, *a2) && self.opposite(*b1, *b2)
            }
            (Node::Int(x), Node::Int(y)) if self.vals[c1].ty == Ty::Bool => (*x != 0) != (*y != 0),
            (Node::LNot(x), _) => *x == c2,
            (_, Node::LNot(y)) => *y == c1,
            _ => false,
        }
    }

    fn call(&mut self, f: &'static str, args: Vec<Arg>, ty: Ty) -> usize {
        self.val(Node::Call(f, args), ty)
    }

    /// Bit k of an integer, 0 or 1.
    fn bit(&mut self, m: usize, k: u32) -> usize {
        let t = if k == 0 {
            m
        } else {
            let kk = self.int(k);
            self.bin(Op::Shr, m, kk)
        };
        if k == 31 {
            t
        } else {
            let one = self.int(1);
            self.bin(Op::And, t, one)
        }
    }

    // ---- registers ----

    /// A register's value here.
    fn get(&mut self, r: Reg) -> usize {
        // A register sharing storage with one whose value is not in the
        // state must find that value there.
        for o in r.overlaps() {
            if let Some(&v) = self.cur.get(&o) {
                if self.state.get(&o) != Some(&v) {
                    self.need(o);
                }
            }
        }
        if let Some(&v) = self.cur.get(&r) {
            return v;
        }
        let v = self.val(Node::Entry(r), r.ty());
        self.cur.insert(r, v);
        self.state.insert(r, v);
        v
    }

    /// r is now v. It is in the state only if written there.
    fn set(&mut self, r: Reg, v: usize) {
        for o in r.overlaps() {
            if let Reg::D(_) = o {
                // A single is half a double, so the other half must be in
                // the state.
                if let Some(&dv) = self.cur.get(&o) {
                    if self.state.get(&o) != Some(&dv) {
                        self.need(o);
                    }
                }
            }
            self.cur.remove(&o);
        }
        self.cur.insert(r, v);
        self.last.insert(r, (self.i, v));
        if self.vals[v].insn == self.i
            && !self.home.contains_key(&v)
            && !matches!(r, Reg::Flag(_))
            && !matches!(self.vals[v].node, Node::Entry(_) | Node::Int(_) | Node::Addr(_) | Node::Lit(_))
        {
            self.home.insert(v, (self.i, r));
        }
    }

    /// An assignment, written to the state here if the plan says so.
    fn assign(&mut self, r: Reg, v: usize) {
        self.set(r, v);
        if self.plan.events.contains(&(self.i, r)) {
            self.materialize(r, v);
        }
    }

    /// An assignment under a condition: the old value where it does not hold.
    fn cond_value(&mut self, c: Option<usize>, r: Reg, v: usize) -> usize {
        match c {
            Some(c) => {
                let old = self.get(r);
                self.select(c, v, old)
            }
            None => v,
        }
    }

    fn commit(&mut self, c: Option<usize>, r: Reg, v: usize) {
        let sel = self.cond_value(c, r, v);
        self.assign(r, sel);
        // A conditional load goes to its register here, under its condition,
        // and is read from there.
        if self.is_guarded(v) {
            self.materialize(r, sel);
        }
    }

    /// Whether v is, or is made directly from, a guarded load.
    fn is_guarded(&self, v: usize) -> bool {
        match self.vals[v].node {
            Node::SExt(_, x) => self.guarded.contains(&x),
            _ => self.guarded.contains(&v),
        }
    }

    /// r is v, and the instruction's own statement has written it to the
    /// state.
    fn written_here(&mut self, r: Reg, v: usize) {
        self.set(r, v);
        self.state_write(r, Some(v));
    }

    fn state_write(&mut self, r: Reg, v: Option<usize>) {
        for o in r.overlaps() {
            self.state.remove(&o);
            self.written.insert(o, self.i);
        }
        match v {
            Some(v) => {
                self.state.insert(r, v);
            }
            None => {
                self.state.remove(&r);
            }
        }
        self.written.insert(r, self.i);
    }

    /// The state's r has been changed by something the lifter does not
    /// follow. The next read reads the state again.
    fn overwrite(&mut self, r: Reg) {
        for o in r.overlaps() {
            self.cur.remove(&o);
            self.last.remove(&o);
        }
        self.cur.remove(&r);
        self.last.remove(&r);
        self.state_write(r, None);
    }

    /// r's value must be in the state here, so its assignment must write it
    /// there.
    fn need(&mut self, r: Reg) {
        let Some(&v) = self.cur.get(&r) else { return };
        if self.state.get(&r) == Some(&v) {
            return;
        }
        if let Some(&(i, lv)) = self.last.get(&r) {
            if lv == v {
                match r {
                    Reg::Flag(_) => self.want_flags(i),
                    _ => self.want_event(i, r),
                }
            }
        }
    }

    fn need_all(&mut self, live: Live) {
        let mut regs: Vec<Reg> = self.cur.keys().copied().filter(|r| live.has(*r)).collect();
        regs.sort();
        for r in regs {
            self.need(r);
        }
    }

    /// Write r's value to the state.
    fn materialize(&mut self, r: Reg, v: usize) {
        if self.state.get(&r) == Some(&v) {
            return;
        }
        let stmt = self.assignment(r, v);
        // Skip `R[13] = R[13];`. The state already holds the value, as when
        // a pop undoes a push.
        if stmt != format!("{0} = {0};", self.rc(r)) {
            self.stmts.push(stmt);
        }
        self.state_write(r, Some(v));
    }

    /// `r = v;`, in its most natural form.
    fn assignment(&mut self, r: Reg, v: usize) -> String {
        let lhs = self.rc(r);
        if let Some(&n) = self.temps.get(&v) {
            self.temp_reads.insert(v);
            return format!("{lhs} = v{n};");
        }
        let fresh = self.vals[v].insn == self.i && self.resident(v).is_none();
        if !fresh {
            return self.assign_value(r, v);
        }
        match self.vals[v].node.clone() {
            // A conditional assignment to the register holding the old value.
            Node::Select(c, new, old) if self.state.get(&r) == Some(&old) => {
                let (ct, _) = self.text(c);
                let inner = self.assign_value(r, new);
                format!("if ({ct}) {inner}")
            }
            // The register moved by a constant, as in `R[13] -= 4`,
            // whatever chain of offsets made the value.
            Node::Bin(Op::Add | Op::Sub, _, b)
                if self.vals[v].ty == Ty::I32
                    && self.konst(b).is_some()
                    && self.state.get(&r) == Some(&self.offset_chain(v).0) =>
            {
                if let (Node::Bin(op, x, _), Some(n)) = (self.vals[v].node.clone(), self.name_text(b)) {
                    if x == self.offset_chain(v).0 {
                        return format!("{lhs} {}= {n};", op.c());
                    }
                }
                match self.offset_chain(v).1 {
                    0 => format!("{lhs} = {lhs};"),
                    k if k < 0x8000_0000 => format!("{lhs} += {};", int_text(k, Ty::I32)),
                    k => format!("{lhs} -= {};", int_text(k.wrapping_neg(), Ty::I32)),
                }
            }
            Node::Bin(op, a, b)
                if op.compound()
                    && self.vals[v].ty != Ty::Bool
                    && self.state.get(&r) == Some(&a)
                    && !(self.vals[v].ty == Ty::I32 && self.konst(b).is_some() && matches!(op, Op::Add | Op::Sub) && self.offset_chain(v).0 != a) =>
            {
                let bt = match (op, self.konst(b), self.name_text(b)) {
                    (_, _, Some(n)) => n,
                    (Op::And | Op::Or | Op::Xor, Some(k), _) => mask_text(k),
                    _ => self.rhs(b),
                };
                format!("{lhs} {}= {bt};", op.c())
            }
            _ => {
                let (t, _) = self.expr(v);
                format!("{lhs} = {t};")
            }
        }
    }

    /// `r = x;` where x is read, or `r op= y;` when x is written out here as
    /// `r op y`.
    fn assign_value(&mut self, r: Reg, x: usize) -> String {
        let lhs = self.rc(r);
        if !self.temps.contains_key(&x) && self.resident(x).is_none() && !self.plan.no_inline.contains(&x) {
            if let Node::Bin(op, a, b) = self.vals[x].node {
                if op.compound() && self.vals[x].ty != Ty::Bool && self.state.get(&r) == Some(&a) {
                    *self.inlined.entry(x).or_insert(0) += 1;
                    let bt = self.rhs(b);
                    return format!("{lhs} {}= {bt};", op.c());
                }
            }
        }
        let t = self.render(x);
        format!("{lhs} = {t};")
    }

    /// Whether two addresses are the same place: the same value, or the
    /// same base and the same constant offset.
    fn same_place(&self, a: usize, b: usize) -> bool {
        a == b
            || match (&self.vals[a].node, &self.vals[b].node) {
                (Node::Bin(Op::Add, x, k), Node::Bin(Op::Add, y, j)) => {
                    x == y && self.konst(*k).is_some() && self.konst(*k) == self.konst(*j)
                }
                _ => false,
            }
    }

    /// The right-hand side of `op=`. A mask is written in hex, and a name as
    /// it is written.
    fn rhs_operand(&mut self, op: Op, b: usize) -> String {
        match (op, self.konst(b), self.name_text(b)) {
            (_, _, Some(n)) => n,
            (Op::And | Op::Or | Op::Xor, Some(k), _) => mask_text(k),
            _ => self.rhs(b),
        }
    }

    /// The right-hand side of a compound assignment. A condition goes in
    /// parentheses, as in `x += (a < b)`.
    fn rhs(&mut self, b: usize) -> String {
        if self.vals[b].ty == Ty::Bool {
            self.fmt(b, 14)
        } else {
            self.render(b)
        }
    }

    /// A register's C: its local, or its place in the state block.
    fn rc(&self, r: Reg) -> String {
        match r {
            Reg::R(n) if self.ctx.locals >> n & 1 != 0 => format!("r{n}"),
            Reg::Slot(_) => r.c(),
            r => r.c(),
        }
    }

    /// A register the state holds v in, the value's home first.
    fn resident(&self, v: usize) -> Option<Reg> {
        if let Some(&(_, r)) = self.home.get(&v) {
            if self.state.get(&r) == Some(&v) {
                return Some(r);
            }
        }
        self.state.iter().filter(|&(_, &x)| x == v).map(|(&r, _)| r).min()
    }

    /// Whether v can be read without writing it out: from the state or a
    /// variable.
    fn available(&self, v: usize) -> bool {
        self.temps.contains_key(&v) || self.resident(v).is_some()
    }

    // ---- C text ----

    fn render(&mut self, v: usize) -> String {
        self.fmt(v, 0)
    }

    fn fmt(&mut self, v: usize, outer: u8) -> String {
        let (t, p) = self.text(v);
        if p < outer {
            format!("({t})")
        } else {
            t
        }
    }

    /// A value's C where it is read here, and its precedence. The precedence
    /// is 15 for a primary, 14 for unary, 13 to 4 as for C's binary
    /// operators, and 3 for the conditional operator.
    fn text(&mut self, v: usize) -> (String, u8) {
        let ty = self.vals[v].ty;
        if let Some(n) = self.name_text(v) {
            return (n, 15);
        }
        // `~f_CallBackPending` is a constant too.
        if let Node::Not(a) = self.vals[v].node {
            if let Some(n) = self.name_text(a) {
                return (format!("~{n}"), 14);
            }
        }
        match self.vals[v].node {
            Node::Int(k) => return (int_text(k, ty), 15),
            Node::Addr(a) => return (self.addr_text(a), 15),
            Node::Lit(x) => {
                let s = lit(x, ty);
                let p = if s.starts_with('-') { 14 } else { 15 };
                return (s, p);
            }
            _ => {}
        }
        if let Some(r) = self.resident(v) {
            return (self.rc(r), 15);
        }
        if let Some(&n) = self.temps.get(&v) {
            self.temp_reads.insert(v);
            return (format!("v{n}"), 15);
        }
        match self.vals[v].node.clone() {
            // The state has changed since it was read.
            Node::Entry(r) => {
                self.want_temp(v);
                return (self.rc(r), 15);
            }
            Node::Bits { r, at } => {
                let stale = std::iter::once(r)
                    .chain(r.overlaps())
                    .any(|o| self.written.get(&o).is_some_and(|&w| w > at));
                if stale {
                    self.want_temp(v);
                }
                return (bits_text(r), 15);
            }
            Node::Flag { src: Src::Carry, bit, insn } => {
                self.want_flags(insn);
                return (Reg::Flag(bit).c(), 15);
            }
            _ => {}
        }
        // Conditions, flags and a register plus a constant are cheap and
        // pure, so they are written out wherever they are read. A guarded load
        // is read only in its own assignment, under its condition.
        if ty == Ty::Bool || self.cheap(v) || self.guarded.contains(&v) {
            return self.expr(v);
        }
        // Anything else is written out once, and only where it still means
        // what it meant.
        let stale = matches!(self.vals[v].node, Node::Load { mem, .. } if mem != self.mem);
        if stale || self.plan.no_inline.contains(&v) {
            self.unsatisfied(v);
            return (format!("v?{v}"), 15);
        }
        *self.inlined.entry(v).or_insert(0) += 1;
        self.expr(v)
    }

    /// A register or constant plus or minus constants. It is written out
    /// wherever it is read.
    fn cheap(&self, v: usize) -> bool {
        let (base, _) = self.offset_chain(v);
        base != v
            && (matches!(self.vals[base].node, Node::Entry(_) | Node::Int(_) | Node::Addr(_))
                || self.available(base))
    }

    /// v as a base plus a constant. It follows additions and subtractions
    /// of constants down to the base, but stops early at a value that is in
    /// the state or a variable.
    fn offset_chain(&self, v: usize) -> (usize, u32) {
        let (mut v, mut k) = (v, 0u32);
        loop {
            match self.vals[v].node {
                Node::Bin(op @ (Op::Add | Op::Sub), x, y) if self.vals[v].ty == Ty::I32 => match self.konst(y) {
                    Some(c) => {
                        k = k.wrapping_add(if op == Op::Add { c } else { c.wrapping_neg() });
                        v = x;
                        if self.available(v) {
                            return (v, k);
                        }
                    }
                    None => return (v, k),
                },
                _ => return (v, k),
            }
        }
    }

    /// A read that neither the state, nor a variable, nor writing the value
    /// out here satisfies. The value is written to its register where it is
    /// made, if it stays there until now. Otherwise it goes to a variable.
    fn unsatisfied(&mut self, v: usize) {
        match self.home.get(&v).copied() {
            Some((i, r))
                if i < self.i
                    && !self.plan.events.contains(&(i, r))
                    && self.written.get(&r).is_none_or(|&w| w <= i) =>
            {
                self.want_event(i, r)
            }
            _ => self.want_temp(v),
        }
    }

    fn addr_text(&self, a: u32) -> String {
        (self.ctx.label)(a).unwrap_or_else(|| format!("0x{a:08X}u"))
    }

    /// A value's own C, its operands read here.
    fn expr(&mut self, v: usize) -> (String, u8) {
        let ty = self.vals[v].ty;
        match self.vals[v].node.clone() {
            Node::Entry(r) => (self.rc(r), 15),
            Node::Bits { r, .. } => (bits_text(r), 15),
            Node::Int(_) if self.names.contains_key(&v) => (self.name_text(v).unwrap_or_default(), 15),
            Node::Int(k) => (int_text(k, ty), 15),
            Node::Addr(a) => (self.addr_text(a), 15),
            Node::Lit(x) => (lit(x, ty), 15),
            Node::Neg(a) => (format!("-{}", self.fmt(a, 14)), 14),
            Node::Not(a) => (format!("~{}", self.fmt(a, 14)), 14),
            Node::LNot(a) => match self.vals[a].node.clone() {
                Node::Flag { src, bit, insn } if self.resident(a).is_none() => {
                    if let Src::Carry = src {
                        self.want_flags(insn);
                    }
                    self.flag_text_neg(src, bit)
                }
                Node::Cond(cond, bits) if self.resident(a).is_none() => self.cond_text(invert(cond), bits),
                _ => (format!("!{}", self.fmt(a, 14)), 14),
            },
            // One named offset: `r12 + WsFlags`.
            Node::Bin(op @ (Op::Add | Op::Sub), a, b)
                if ty == Ty::I32 && self.names.contains_key(&b) && self.offset_chain(v).0 == a =>
            {
                let at = self.fmt(a, 12);
                (format!("{at} {} {}", op.c(), self.name_text(b).unwrap_or_default()), 12)
            }
            Node::Bin(Op::Add | Op::Sub, _, b) if ty == Ty::I32 && self.konst(b).is_some() => {
                // A base and one offset: `R[13] - 4`, not `R[13] - 8 + 4`.
                let (base, k) = self.offset_chain(v);
                let bt = self.text(base);
                match k {
                    0 => bt,
                    k => {
                        let b = if bt.1 < 12 { format!("({})", bt.0) } else { bt.0 };
                        if k < 0x8000_0000 {
                            (format!("{b} + {}", int_text(k, Ty::I32)), 12)
                        } else {
                            (format!("{b} - {}", int_text(k.wrapping_neg(), Ty::I32)), 12)
                        }
                    }
                }
            }
            Node::Bin(op, a, b) => self.bin_text(op, a, b),
            Node::Load { f, addr, .. } => match self.member(addr, f) {
                Some(m) => (m, 15),
                None => (format!("{f}({})", self.render(addr)), 15),
            },
            Node::Call(f, args) => {
                let args: Vec<String> = args
                    .iter()
                    .map(|a| match a {
                        Arg::V(x) => self.render(*x),
                        Arg::T(t) => t.to_string(),
                    })
                    .collect();
                let p = if f.starts_with('(') { 14 } else { 15 };
                (format!("{f}({})", args.join(", ")), p)
            }
            Node::Cast(t, a) => (format!("({}){}", t.c(), self.fmt(a, 14)), 14),
            Node::SExt(bits, a) => (format!("(uint32_t)(int{bits}_t){}", self.fmt(a, 14)), 14),
            Node::FromInt { src, signed } => {
                let cast = ty.c();
                let s = self.fmt(src, 14);
                if signed {
                    (format!("({cast})(int32_t){s}"), 14)
                } else {
                    (format!("({cast}){s}"), 14)
                }
            }
            Node::Select(c, a, b) => self.select_text(c, a, b, ty),
            Node::Flag { src, bit, .. } => self.flag_text(src, bit),
            Node::Cond(cond, bits) => self.cond_text(cond, bits),
            Node::Mul64 { signed, a, b } => {
                if signed {
                    let (x, y) = (self.fmt(a, 14), self.fmt(b, 14));
                    (format!("(uint64_t)((int64_t)(int32_t){x} * (int32_t){y})"), 14)
                } else {
                    let (x, y) = (self.fmt(a, 14), self.fmt(b, 14));
                    (format!("(uint64_t){x} * {y}"), 13)
                }
            }
            Node::Wide { hi, lo } => {
                let (h, l) = (self.fmt(hi, 14), self.fmt(lo, 7));
                (format!("(uint64_t){h} << 32 | {l}"), 6)
            }
            Node::Lo(a) => (format!("(uint32_t){}", self.fmt(a, 14)), 14),
            Node::Hi(a) => (format!("(uint32_t)({} >> 32)", self.fmt(a, 11)), 14),
        }
    }

    fn bin_text(&mut self, op: Op, a: usize, b: usize) -> (String, u8) {
        if op == Op::Asr {
            let x = self.fmt(a, 14);
            let n = match self.konst(b) {
                Some(k) => k.to_string(),
                None => self.fmt(b, 12),
            };
            return (format!("(uint32_t)((int32_t){x} >> {n})"), 14);
        }
        let fp = self.vals[a].ty.is_fp() || self.vals[b].ty.is_fp();
        let (l, r) = if op.signed() {
            (self.signed_operand(a), self.signed_operand(b))
        } else if let (Op::Shl | Op::Shr, Some(k)) = (op, self.konst(b)) {
            // A shift is by a number of bits, not a mask.
            (self.operand(op, a, false, fp), k.to_string())
        } else {
            (self.operand(op, a, false, fp), self.operand(op, b, true, fp))
        };
        (format!("{l} {} {r}", op.c()), op.prec())
    }

    fn signed_operand(&mut self, v: usize) -> String {
        if let Some(n) = self.name_text(v) {
            return format!("(int32_t){n}");
        }
        if let Some(k) = self.konst(v) {
            return signed_text(k);
        }
        format!("(int32_t){}", self.fmt(v, 14))
    }

    /// An operand of op, in parentheses where C needs them. It also gets
    /// them where a reader would want them, around bitwise operations mixed
    /// with others.
    fn operand(&mut self, parent: Op, v: usize, right: bool, fp: bool) -> String {
        if let Some(n) = self.name_text(v) {
            return n;
        }
        if let (Op::And | Op::Or | Op::Xor, Some(k), false) = (parent, self.konst(v), fp) {
            return mask_text(k);
        }
        let (t, cp) = self.text(v);
        let pp = parent.prec();
        let bitwise = |p: u8| matches!(p, 6 | 7 | 8 | 11);
        let paren = cp < pp
            || (right && cp == pp && (fp || !parent.assoc()))
            || (cp < 14 && cp != pp && (bitwise(pp) || bitwise(cp)))
            || (parent.is_cmp() && cp == 10 || parent.is_cmp() && cp == 9);
        if paren {
            format!("({t})")
        } else {
            t
        }
    }

    /// x compared with a constant.
    fn cmp_k(&mut self, op: Op, x: usize, k: u32) -> (String, u8) {
        if let Some(v) = self.konst(x).and_then(|y| fold(op, y, k)) {
            return (if v != 0 { "1" } else { "0" }.to_string(), 15);
        }
        let t = if op.signed() { self.signed_operand(x) } else { self.fmt(x, 11) };
        let kt = if op.signed() { signed_text(k) } else { int_text(k, Ty::I32) };
        (format!("{t} {} {kt}", op.c()), op.prec())
    }

    fn select_text(&mut self, c: usize, a: usize, b: usize, ty: Ty) -> (String, u8) {
        if ty == Ty::Bool {
            let and = |s: &mut Self, x: usize, y: usize| {
                let (xt, yt) = (s.fmt(x, 5), s.fmt(y, 6));
                (format!("{xt} && {yt}"), 5)
            };
            // `&&` inside `||` in parentheses, as a reader (and clang) want.
            let or = |s: &mut Self, x: usize, y: usize| {
                let (xt, yt) = (s.or_side(x), s.or_side(y));
                (format!("{xt} || {yt}"), 4)
            };
            if b == c {
                return and(self, c, a);
            }
            if a == c {
                return or(self, c, b);
            }
            if self.opposite(b, c) {
                return or(self, b, a);
            }
            if self.opposite(a, c) {
                return and(self, a, b);
            }
            // A conditional compare: CMPEQ is `and`, CMPNE is `or`.
            match self.cond_is(c, b) {
                Some(true) => {
                    let (x, y) = (self.fmt(b, 5), self.fmt(a, 6));
                    return (format!("{x} && {y}"), 5);
                }
                Some(false) => {
                    let (x, y) = (self.or_side(b), self.or_side(a));
                    return (format!("{x} || {y}"), 4);
                }
                None => {}
            }
        }
        match (self.konst(a), self.konst(b)) {
            (Some(1), Some(0)) => return self.text(c),
            (Some(0), Some(1)) => {
                let t = self.fmt(c, 14);
                return (format!("!{t}"), 14);
            }
            _ => {}
        }
        let (ct, at, bt) = (self.fmt(c, 6), self.fmt(a, 4), self.fmt(b, 3));
        (format!("{ct} ? {at} : {bt}"), 3)
    }

    /// An operand of `||`: another `||` as it is, `&&` in parentheses.
    fn or_side(&mut self, v: usize) -> String {
        let (t, p) = self.text(v);
        if p == 5 || p < 4 {
            format!("({t})")
        } else {
            t
        }
    }

    /// Whether condition c is flag value y itself (true) or its negation.
    fn cond_is(&self, c: usize, y: usize) -> Option<bool> {
        let Node::Cond(cond, bits) = &self.vals[c].node else { return None };
        let (k, pos) = match cond {
            Cond::Eq => (Z, true),
            Cond::Ne => (Z, false),
            Cond::Cs => (C, true),
            Cond::Cc => (C, false),
            Cond::Mi => (N, true),
            Cond::Pl => (N, false),
            Cond::Vs => (V, true),
            Cond::Vc => (V, false),
            _ => return None,
        };
        (bits[k as usize] == y).then_some(pos)
    }

    /// `(int32_t)x < 0`, or `>= 0`.
    fn sign_text(&mut self, x: usize, negative: bool) -> (String, u8) {
        if let Some(t) = self.bit_test(x, N, negative) {
            return t;
        }
        let t = self.fmt(x, 14);
        (format!("(int32_t){t} {} 0", if negative { "<" } else { ">=" }), 10)
    }

    /// N or Z of a shift whose result nothing holds, written as a test of
    /// the bits it came from. Such a shift moves bits into the flags to
    /// test them. `MOVS r1, r0, LSL #31` tests bit 0, and
    /// `MOVS r1, r0, LSR #8` tests whether r0 is below 256.
    fn bit_test(&mut self, res: usize, bit: u8, set: bool) -> Option<(String, u8)> {
        if self.available(res) {
            return None;
        }
        let Node::Bin(op, x, s) = self.vals[res].node else { return None };
        let j = self.konst(s).filter(|j| (1..32).contains(j))?;
        let (test, k) = match (op, bit) {
            (Op::Shl, N) => (if set { Op::Ne } else { Op::Eq }, 1u32 << (31 - j)),
            (Op::Shl, Z) => (if set { Op::Eq } else { Op::Ne }, u32::MAX >> j),
            (Op::Shr | Op::Asr, Z) => {
                // Unsigned, so that a negative x is not below it.
                let t = self.fmt(x, 11);
                let (c, kt) = (if set { "<" } else { ">=" }, format!("0x{:X}u", 1u64 << j));
                return Some((format!("{t} {c} {kt}"), 10));
            }
            (Op::Asr, N) => return Some(self.sign_text(x, set)),
            _ => return None,
        };
        let t = self.operand(Op::And, x, false, false);
        Some((format!("({t} & {}) {} 0", mask_text(k), test.c()), 9))
    }

    /// A 0-or-1 value that nothing holds and that is one bit of another
    /// value, as in `(x >> k) & 1`, `x >> 31` or `x & 1`. Returns the other
    /// value and the bit's mask.
    fn one_bit(&self, v: usize) -> Option<(usize, u32)> {
        if self.available(v) {
            return None;
        }
        match self.vals[v].node {
            Node::Bin(Op::And, y, one) if self.konst(one) == Some(1) => match self.vals[y].node {
                Node::Bin(Op::Shr, x, s) if !self.available(y) => {
                    self.konst(s).filter(|k| *k < 32).map(|k| (x, 1u32 << k))
                }
                _ => Some((y, 1)),
            },
            Node::Bin(Op::Shr, x, s) if self.konst(s) == Some(31) => Some((x, 1 << 31)),
            _ => None,
        }
    }

    /// One flag's C.
    fn flag_text(&mut self, src: Src, bit: u8) -> (String, u8) {
        match (src, bit) {
            (Src::Sub { res, .. } | Src::Add { res, .. } | Src::Nz { res }, N) => self.sign_text(res, true),
            (Src::Sub { a, b, res }, Z) => {
                if self.available(res) {
                    self.cmp_k(Op::Eq, res, 0)
                } else {
                    self.bin_text(Op::Eq, a, b)
                }
            }
            (Src::Add { res, .. } | Src::Nz { res }, Z) => {
                self.bit_test(res, Z, true).unwrap_or_else(|| self.cmp_k(Op::Eq, res, 0))
            }
            (Src::Sub { a, b, .. }, C) => self.bin_text(Op::Ge, a, b),
            (Src::Add { b, res, .. }, C) => self.bin_text(Op::Lt, res, b),
            (Src::Sub { a, b, .. }, _) => {
                let (x, y) = (self.render(a), self.render(b));
                (format!("ros_subv({x}, {y})"), 15)
            }
            (Src::Add { a, b, .. }, _) => {
                let (x, y) = (self.render(a), self.render(b));
                (format!("ros_addv({x}, {y})"), 15)
            }
            (Src::FCmp { a, b, vfp }, k) => {
                let (x, y) = (self.fmt(a, 11), self.fmt(b, 11));
                match k {
                    N => (format!("{x} < {y}"), 10),
                    Z => (format!("{x} == {y}"), 9),
                    C if vfp => (format!("!({x} < {y})"), 14),
                    C => (format!("{x} >= {y}"), 10),
                    _ => (format!("isunordered({x}, {y})"), 15),
                }
            }
            // Never here: text() sends these to the state.
            (Src::Nz { .. } | Src::Carry, k) => (Reg::Flag(k).c(), 15),
        }
    }

    /// The negation of one flag's C.
    fn flag_text_neg(&mut self, src: Src, bit: u8) -> (String, u8) {
        match (src, bit) {
            (Src::Sub { res, .. } | Src::Add { res, .. } | Src::Nz { res }, N) => self.sign_text(res, false),
            (Src::Sub { a, b, res }, Z) => {
                if self.available(res) {
                    self.cmp_k(Op::Ne, res, 0)
                } else {
                    self.bin_text(Op::Ne, a, b)
                }
            }
            (Src::Add { res, .. } | Src::Nz { res }, Z) => {
                self.bit_test(res, Z, false).unwrap_or_else(|| self.cmp_k(Op::Ne, res, 0))
            }
            (Src::Sub { a, b, .. }, C) => self.bin_text(Op::Lt, a, b),
            (Src::Add { b, res, .. }, C) => self.bin_text(Op::Ge, res, b),
            _ => {
                let t = self.flag_text(src, bit);
                let t = if t.1 < 14 { format!("({})", t.0) } else { t.0 };
                (format!("!{t}"), 14)
            }
        }
    }

    /// A condition code's C.
    fn cond_text(&mut self, cond: Cond, bits: [usize; 4]) -> (String, u8) {
        let reads = cond_reads(cond);
        if reads.is_empty() {
            return ("1".into(), 15);
        }
        // The flags are in the state, so use tier 0's test. The exception
        // is where what the flags came from is still at hand, since that
        // says what was compared.
        if reads.iter().all(|&k| self.state.get(&Reg::Flag(k)) == Some(&bits[k as usize])) {
            if let Some(src) = self.same_src(reads, &bits) {
                if self.at_hand(cond, src) {
                    if let Some(t) = self.predicate_text(cond, src) {
                        return t;
                    }
                }
            }
            return tier0_cond(cond);
        }
        // All from one flag-setting instruction: what it compared.
        if let Some(src) = self.one_src(reads, &bits) {
            if let Some(t) = self.predicate_text(cond, src) {
                return t;
            }
        }
        self.flags_cond(cond, bits)
    }

    /// The flag-setting instruction all the flags a condition reads came
    /// from, if one did.
    fn same_src(&self, reads: &[u8], bits: &[usize; 4]) -> Option<Src> {
        let mut found: Option<(usize, Src)> = None;
        for &k in reads {
            let Node::Flag { src, insn, .. } = self.vals[bits[k as usize]].node else { return None };
            match found {
                None => found = Some((insn, src)),
                Some((i, _)) if i == insn => {}
                _ => return None,
            }
        }
        found.map(|(_, s)| s)
    }

    /// Whether a condition's predicate can be written from constants and
    /// values the state or a variable holds, with nothing to fold and
    /// nothing stale.
    fn at_hand(&self, cond: Cond, src: Src) -> bool {
        let ok = |x: usize| {
            matches!(self.vals[x].node, Node::Int(_) | Node::Addr(_) | Node::Lit(_)) || self.available(x)
        };
        use Cond::*;
        match src {
            Src::Sub { a, b, res } => match cond {
                Eq | Ne => ok(res) || (ok(a) && ok(b)),
                Mi | Pl => ok(res),
                _ => ok(a) && ok(b),
            },
            Src::Add { b, res, .. } => ok(res) && (matches!(cond, Eq | Ne | Mi | Pl) || ok(b)),
            Src::Nz { res } => ok(res),
            Src::FCmp { a, b, .. } => ok(a) && ok(b),
            Src::Carry => false,
        }
    }

    fn one_src(&self, reads: &[u8], bits: &[usize; 4]) -> Option<Src> {
        let mut found: Option<(usize, Src)> = None;
        for &k in reads {
            let b = bits[k as usize];
            let Node::Flag { src, insn, .. } = self.vals[b].node else { return None };
            if self.state.get(&Reg::Flag(k)) == Some(&b) {
                return None;
            }
            match found {
                None => found = Some((insn, src)),
                Some((i, _)) if i == insn => {}
                _ => return None,
            }
        }
        found.map(|(_, s)| s)
    }

    fn predicate_text(&mut self, cond: Cond, src: Src) -> Option<(String, u8)> {
        use Cond::*;
        Some(match src {
            Src::Sub { a, b, res } => {
                let zero = self.konst(b) == Some(0);
                match cond {
                    Eq | Ne => {
                        let op = if cond == Eq { Op::Eq } else { Op::Ne };
                        if self.available(res) {
                            self.cmp_k(op, res, 0)
                        } else {
                            self.bin_text(op, a, b)
                        }
                    }
                    Cs if zero => ("1".into(), 15),
                    Cc if zero => ("0".into(), 15),
                    Hi if zero => self.cmp_k(Op::Ne, a, 0),
                    Ls if zero => self.cmp_k(Op::Eq, a, 0),
                    Cs => self.bin_text(Op::Ge, a, b),
                    Cc => self.bin_text(Op::Lt, a, b),
                    Hi => self.bin_text(Op::Gt, a, b),
                    Ls => self.bin_text(Op::Le, a, b),
                    Ge => self.bin_text(Op::SGe, a, b),
                    Lt => self.bin_text(Op::SLt, a, b),
                    Gt => self.bin_text(Op::SGt, a, b),
                    Le => self.bin_text(Op::SLe, a, b),
                    Mi => self.sign_text(res, true),
                    Pl => self.sign_text(res, false),
                    Vs if zero => ("0".into(), 15),
                    Vc if zero => ("1".into(), 15),
                    Vs | Vc => {
                        let (x, y) = (self.render(a), self.render(b));
                        let t = format!("ros_subv({x}, {y})");
                        if cond == Vs {
                            (t, 15)
                        } else {
                            (format!("!{t}"), 14)
                        }
                    }
                    Al => return None,
                }
            }
            Src::Add { b, res, .. } => match cond {
                Eq => self.cmp_k(Op::Eq, res, 0),
                Ne => self.cmp_k(Op::Ne, res, 0),
                Cs => self.bin_text(Op::Lt, res, b),
                Cc => self.bin_text(Op::Ge, res, b),
                Mi => self.sign_text(res, true),
                Pl => self.sign_text(res, false),
                _ => return None,
            },
            Src::Nz { res } => match cond {
                Eq => self.bit_test(res, Z, true).unwrap_or_else(|| self.cmp_k(Op::Eq, res, 0)),
                Ne => self.bit_test(res, Z, false).unwrap_or_else(|| self.cmp_k(Op::Ne, res, 0)),
                Mi => self.sign_text(res, true),
                Pl => self.sign_text(res, false),
                _ => return None,
            },
            Src::FCmp { a, b, vfp } => {
                let (x, y) = (self.fmt(a, 11), self.fmt(b, 11));
                let t = predicate(cond, &x, &y, vfp)?;
                let p = if t.starts_with('!') { 14 } else if t.starts_with("isunordered") { 15 } else { 9 };
                (t, p)
            }
            Src::Carry => return None,
        })
    }

    /// A condition from its flags one by one.
    fn flags_cond(&mut self, cond: Cond, bits: [usize; 4]) -> (String, u8) {
        use Cond::*;
        let [n, z, c, v] = bits;
        let neg = |s: &mut Self, x: usize| {
            let t = s.fmt(x, 14);
            format!("!{t}")
        };
        // A carry shifted out of a register is one of its bits, so test it
        // as that bit.
        if let Some((x, m)) = matches!(cond, Cs | Cc).then(|| self.one_bit(c)).flatten() {
            let t = self.operand(Op::And, x, false, false);
            return (format!("({t} & {}) {} 0", mask_text(m), if cond == Cs { "!=" } else { "==" }), 9);
        }
        match cond {
            Eq => self.text(z),
            Ne => (neg(self, z), 14),
            Cs => self.text(c),
            Cc => (neg(self, c), 14),
            Mi => self.text(n),
            Pl => (neg(self, n), 14),
            Vs => self.text(v),
            Vc => (neg(self, v), 14),
            Hi => {
                let (x, y) = (self.fmt(c, 6), neg(self, z));
                (format!("{x} && {y}"), 5)
            }
            Ls => {
                let (x, y) = (neg(self, c), self.or_side(z));
                (format!("{x} || {y}"), 4)
            }
            Ge | Lt => {
                let (x, y) = (self.fmt(n, 11), self.fmt(v, 11));
                (format!("{x} {} {y}", if cond == Ge { "==" } else { "!=" }), 9)
            }
            Gt => {
                let (w, x, y) = (neg(self, z), self.fmt(n, 11), self.fmt(v, 11));
                (format!("{w} && {x} == {y}"), 5)
            }
            Le => {
                let (w, x, y) = (self.or_side(z), self.fmt(n, 11), self.fmt(v, 11));
                (format!("{w} || {x} != {y}"), 4)
            }
            Al => ("1".into(), 15),
        }
    }

    // ---- building blocks of instructions ----

    fn cond_node(&mut self, cond: Cond) -> usize {
        let mut bits = [usize::MAX; 4];
        for &k in cond_reads(cond) {
            bits[k as usize] = self.get(Reg::Flag(k));
        }
        self.cond_of(cond, bits)
    }

    fn cond_of(&mut self, cond: Cond, bits: [usize; 4]) -> usize {
        let reads = cond_reads(cond);
        // Flags that are constants make a constant.
        let ks: Vec<Option<u32>> = reads.iter().map(|&k| self.konst(bits[k as usize])).collect();
        if ks.iter().all(Option::is_some) {
            let mut f = [false; 4];
            for (&k, x) in reads.iter().zip(&ks) {
                f[k as usize] = x.unwrap() != 0;
            }
            return self.boolean(holds(cond, f));
        }
        // Flags that a conditional instruction set under its condition c.
        // The result is c ? (the condition on its flags) : (the condition
        // on the flags before).
        let mut split = None;
        for &k in reads {
            match self.vals[bits[k as usize]].node {
                Node::Select(c, _, _) if split.is_none_or(|s| s == c) => split = Some(c),
                _ => {
                    split = None;
                    break;
                }
            }
        }
        if let Some(c) = split {
            let (mut new, mut old) = (bits, bits);
            for &k in reads {
                if let Node::Select(_, x, y) = self.vals[bits[k as usize]].node {
                    new[k as usize] = x;
                    old[k as usize] = y;
                }
            }
            let a = self.cond_of(cond, new);
            let b = self.cond_of(cond, old);
            return self.select(c, a, b);
        }
        let key = (cond as u8, bits);
        if let Some(&v) = self.conds.get(&key) {
            return v;
        }
        let v = self.val(Node::Cond(cond, bits), Ty::Bool);
        self.conds.insert(key, v);
        v
    }

    fn flags_of(&mut self, src: Src, which: &[u8]) -> Vec<(u8, usize)> {
        which
            .iter()
            .map(|&k| (k, self.val(Node::Flag { src, bit: k, insn: self.i }, Ty::Bool)))
            .collect()
    }

    /// The flags an instruction sets, where nothing needs them in the state.
    fn set_flags(&mut self, c: Option<usize>, bits: &[(u8, usize)]) {
        for &(k, b) in bits {
            let v = self.cond_value(c, Reg::Flag(k), b);
            self.set(Reg::Flag(k), v);
        }
    }

    /// A statement, under the instruction's condition.
    fn effect(&mut self, c: Option<usize>, stmt: String) {
        match c {
            None => self.stmts.push(stmt),
            Some(c) => {
                let (t, _) = self.text(c);
                self.stmts.push(format!("if ({t}) {stmt}"));
            }
        }
    }

    /// A flag-setting instruction compiled tier 0's way, because something
    /// needs its flags in the state. `call` sets the flags and gives the
    /// result, which goes to dest.
    fn tier0(&mut self, c: Option<usize>, dest: Option<Reg>, res: usize, call: String, bits: &[(u8, usize)]) {
        if c.is_some() {
            // If it does not run, it leaves the state as it was, so the old
            // values must already be there.
            if let Some(r) = dest {
                self.need(r);
            }
            for &(k, _) in bits {
                self.need(Reg::Flag(k));
            }
        }
        let stmt = match dest {
            Some(r) => format!("{} = {call};", self.rc(r)),
            None => format!("{call};"),
        };
        self.effect(c, stmt);
        if let Some(r) = dest {
            let v = self.cond_value(c, r, res);
            self.written_here(r, v);
        }
        for &(k, b) in bits {
            let v = self.cond_value(c, Reg::Flag(k), b);
            self.written_here(Reg::Flag(k), v);
        }
    }

    /// Operand 2, and the shifter's carry out (None means C is unchanged).
    fn op2(&mut self, o: Operand2, pc: u32) -> (usize, Option<usize>) {
        match o {
            Operand2::Imm { value, rotated } => {
                let v = self.named(value);
                let c = rotated.then(|| self.boolean(value >> 31 != 0));
                (v, c)
            }
            Operand2::Reg { rm, shift } => {
                let m = if rm == 15 { self.addr(pc) } else { self.get(Reg::R(rm)) };
                self.shifted(m, shift)
            }
        }
    }

    fn shifted(&mut self, m: usize, shift: Shift) -> (usize, Option<usize>) {
        match shift {
            Shift::Imm(ShiftType::Lsl, 0) => (m, None),
            Shift::Imm(ShiftType::Lsl, n) => {
                let k = self.int(n);
                let v = self.bin(Op::Shl, m, k);
                (v, Some(self.bit(m, 32 - n)))
            }
            Shift::Imm(ShiftType::Lsr, 32) => {
                let v = self.int(0);
                (v, Some(self.bit(m, 31)))
            }
            Shift::Imm(ShiftType::Lsr, n) => {
                let k = self.int(n);
                let v = self.bin(Op::Shr, m, k);
                (v, Some(self.bit(m, n - 1)))
            }
            Shift::Imm(ShiftType::Asr, n) => {
                let k = self.int(n.min(31));
                let v = self.bin(Op::Asr, m, k);
                (v, Some(self.bit(m, n - 1)))
            }
            Shift::Imm(ShiftType::Ror, n) => {
                let k = self.int(n);
                let v = self.call("ros_ror", vec![Arg::V(m), Arg::V(k)], Ty::I32);
                (v, Some(self.bit(m, n - 1)))
            }
            Shift::Rrx => {
                let cin = self.get(Reg::Flag(C));
                let (k31, k1) = (self.int(31), self.int(1));
                let hi = self.bin(Op::Shl, cin, k31);
                let lo = self.bin(Op::Shr, m, k1);
                let v = self.bin(Op::Or, hi, lo);
                (v, Some(self.bit(m, 0)))
            }
            Shift::Reg(ty, rs) => {
                let s = self.get(Reg::R(rs));
                let (f, t) = match ty {
                    ShiftType::Lsl => ("ros_lsl", "ROS_LSL"),
                    ShiftType::Lsr => ("ros_lsr", "ROS_LSR"),
                    ShiftType::Asr => ("ros_asr", "ROS_ASR"),
                    ShiftType::Ror => ("ros_rorr", "ROS_ROR"),
                };
                let v = self.call(f, vec![Arg::V(m), Arg::V(s)], Ty::I32);
                let cin = self.get(Reg::Flag(C));
                let c = self.call("ros_shift_c", vec![Arg::V(m), Arg::T(t), Arg::V(s), Arg::V(cin)], Ty::Bool);
                (v, Some(c))
            }
        }
    }

    /// A word, halfword or byte of the image: (value, whether an address).
    fn rom_int(&self, x: u32, bytes: u32) -> Option<(u32, bool)> {
        let w = x & !3;
        let (word, reloc) = (self.ctx.rom_word)(w)?;
        match bytes {
            4 if x == w => Some((word, reloc)),
            2 if x & 1 == 0 => Some(((word >> (8 * (x & 2))) & 0xFFFF, false)),
            1 => Some(((word >> (8 * (x & 3))) & 0xFF, false)),
            _ => None,
        }
    }

    /// An integer load. It is a constant where it reads the image.
    fn load_int(&mut self, width: Width, ea: usize) -> usize {
        let (f, bytes, sext) = match width {
            Width::Word | Width::Double => ("ros_ld32", 4, None),
            Width::Byte => ("ros_ld8", 1, None),
            Width::Half => ("ros_ld16", 2, None),
            Width::SignedByte => ("ros_ld8", 1, Some(8u8)),
            Width::SignedHalf => ("ros_ld16", 2, Some(16u8)),
        };
        if let Node::Addr(x) = self.vals[ea].node {
            if let Some((k, reloc)) = self.rom_int(x, bytes) {
                if reloc {
                    return self.addr(k);
                }
                let k = match sext {
                    Some(8) => k as u8 as i8 as i32 as u32,
                    Some(_) => k as u16 as i16 as i32 as u32,
                    None => k,
                };
                return self.named(k);
            }
        }
        let v = self.val(Node::Load { f, addr: ea, mem: self.mem }, Ty::I32);
        match sext {
            Some(b) => self.val(Node::SExt(b, v), Ty::I32),
            None => v,
        }
    }

    fn store(&mut self, c: Option<usize>, f: &str, ea: usize, x: usize) {
        // `ws(r12)->Flags &= ~1u`: the member, read here and written back.
        let rmw = match self.vals[x].node {
            Node::Bin(op, l, y) if op.compound() && self.vals[x].ty != Ty::Bool => match self.vals[l].node {
                Node::Load { addr, mem, .. } if self.same_place(addr, ea) && mem == self.mem && !self.available(l) => {
                    Some((op, l, y))
                }
                _ => None,
            },
            _ => None,
        };
        let text = match self.member(ea, f) {
            Some(m) => match rmw {
                Some((op, l, y)) if !self.plan.no_inline.contains(&l) => {
                    *self.inlined.entry(l).or_insert(0) += 1;
                    format!("{m} {}= {};", op.c(), self.rhs_operand(op, y))
                }
                _ => format!("{m} = {};", self.render(x)),
            },
            None => {
                let (at, xt) = (self.render(ea), self.render(x));
                format!("{f}({at}, {xt});")
            }
        };
        self.effect(c, text);
        self.mem += 1;
    }

    fn rom_value(&self, addr: u32, bytes: u32, fpa_order: bool) -> Option<f64> {
        let (w0, _) = (self.ctx.rom_word)(addr)?;
        match bytes {
            4 => Some(f32::from_bits(w0) as f64),
            8 => {
                let (w1, _) = (self.ctx.rom_word)(addr + 4)?;
                let bits = if fpa_order { (w0 as u64) << 32 | w1 as u64 } else { (w1 as u64) << 32 | w0 as u64 };
                Some(f64::from_bits(bits))
            }
            _ => None,
        }
    }

    fn fpa_operand(&mut self, o: FpaOperand) -> usize {
        match o {
            FpaOperand::Reg(n) => self.get(Reg::F(n)),
            FpaOperand::Const(i) => self.val(Node::Lit(FPA_CONST[i as usize]), Ty::F64),
        }
    }

    fn fpa_round(&mut self, v: usize, prec: Prec) -> usize {
        match prec {
            Prec::Single if self.vals[v].ty == Ty::F64 => self.val(Node::Cast(Ty::F32, v), Ty::F32),
            _ => v,
        }
    }

    /// A double-precision FPA operation on two singles must widen one, or C
    /// computes in single. A single-precision one on two singles is exactly
    /// C's float arithmetic.
    fn fp_operands(&mut self, a: usize, b: usize, prec: Prec) -> (usize, usize) {
        let both_single = self.vals[a].ty == Ty::F32 && self.vals[b].ty == Ty::F32;
        match (prec, both_single) {
            (Prec::Single, true) | (_, false) => (a, b),
            (_, true) => (self.val(Node::Cast(Ty::F64, a), Ty::F64), b),
        }
    }

    fn fbin(&mut self, op: Op, a: usize, b: usize) -> usize {
        let ty = if self.vals[a].ty == Ty::F64 || self.vals[b].ty == Ty::F64 { Ty::F64 } else { Ty::F32 };
        self.val(Node::Bin(op, a, b), ty)
    }

    fn fneg(&mut self, a: usize) -> usize {
        let t = self.vals[a].ty;
        self.val(Node::Neg(a), t)
    }

    /// VFP's cumulative exceptions for an operation just lifted. `f` (from
    /// the runtime's cpu.h, with an `f` suffix for single precision) works
    /// out invalid operation, division by zero and overflow from the result
    /// and the operands. They are ORed into the FPSCR, where BASIC looks
    /// for them after each operation.
    fn vfp_ex(&mut self, c: Option<usize>, f: &str, single: bool, args: &[usize], more: &str) {
        let mut ts: Vec<String> = args.iter().map(|&a| self.render(a)).collect();
        if !more.is_empty() {
            ts.push(more.to_string());
        }
        let f = if single { format!("{f}f") } else { f.to_string() };
        self.effect(c, format!("s->fp->fpscr |= {f}({});", ts.join(", ")));
    }

    /// A multiple transfer's addresses: the lowest one, and the one written
    /// back.
    fn block_addrs(&mut self, base: usize, n: u32, unit: u32, before: bool, add: bool) -> (usize, usize) {
        let size = self.int(n * unit);
        let (start, back) = match (before, add) {
            (false, true) => (base, self.bin(Op::Add, base, size)),
            (true, true) => {
                let u = self.int(unit);
                (self.bin(Op::Add, base, u), self.bin(Op::Add, base, size))
            }
            (false, false) => {
                let back = self.bin(Op::Sub, base, size);
                let u = self.int(unit);
                (self.bin(Op::Add, back, u), back)
            }
            (true, false) => {
                let back = self.bin(Op::Sub, base, size);
                (back, back)
            }
        };
        (start, back)
    }

    fn at_offset(&mut self, start: usize, off: u32) -> usize {
        let k = self.int(off);
        self.bin(Op::Add, start, k)
    }

    // ---- one instruction ----

    fn step(&mut self, i: usize) -> Result<(), String> {
        let (d, a) = (self.block[i].d, self.block[i].addr);
        let pc = a.wrapping_add(8);
        let c = (d.cond != Cond::Al).then(|| self.cond_node(d.cond));
        let vcmp = self.vcmp.take();
        match d.insn {
            Insn::Nop => {}

            Insn::Dp { op, s, rd, rn, op2 } => {
                if rd == 15 {
                    return Err("a data-processing write to pc".into());
                }
                if let Operand2::Reg { rm, shift: Shift::Reg(_, rs) } = op2 {
                    if rm == 15 || rs == 15 {
                        return Err("a register-shifted register involving pc".into());
                    }
                }
                let n = if !op.uses_rn() {
                    None
                } else if rn == 15 {
                    Some(self.addr(pc))
                } else {
                    Some(self.get(Reg::R(rn)))
                };
                let (m, shc) = self.op2(op2, pc);
                let x = n.unwrap_or(m);
                let cin = matches!(op, DpOp::Adc | DpOp::Sbc | DpOp::Rsc).then(|| self.get(Reg::Flag(C)));
                use DpOp::*;
                let res = match op {
                    And | Tst => self.bin(Op::And, x, m),
                    Eor | Teq => self.bin(Op::Xor, x, m),
                    Sub | Cmp => self.bin(Op::Sub, x, m),
                    Rsb => self.bin(Op::Sub, m, x),
                    Add | Cmn => self.bin(Op::Add, x, m),
                    // x + (m + C) and x - (m + !C), so that `x += m + C`
                    // reads as the carry it is.
                    Adc => {
                        let t = self.bin(Op::Add, m, cin.unwrap());
                        self.bin(Op::Add, x, t)
                    }
                    Sbc | Rsc => {
                        let borrow = self.not(cin.unwrap());
                        if op == Sbc {
                            let t = self.bin(Op::Add, m, borrow);
                            self.bin(Op::Sub, x, t)
                        } else {
                            let t = self.bin(Op::Add, x, borrow);
                            self.bin(Op::Sub, m, t)
                        }
                    }
                    Orr => self.bin(Op::Or, x, m),
                    Mov => m,
                    Bic => {
                        let nm = self.not_int(m);
                        self.bin(Op::And, x, nm)
                    }
                    Mvn => self.not_int(m),
                };
                let dest = (!op.is_test()).then_some(Reg::R(rd));
                if !s {
                    if let Some(r) = dest {
                        self.commit(c, r, res);
                    }
                    return Ok(());
                }
                // The flags: an addition or subtraction's four, or N and Z
                // of the result and C from the shifter.
                let src = match op {
                    Sub | Cmp => Some(Src::Sub { a: x, b: m, res }),
                    Rsb => Some(Src::Sub { a: m, b: x, res }),
                    Add | Cmn => Some(match self.konst(m) {
                        // Adding a constant is subtracting its negation,
                        // flags and all, except for 0 and INT32_MIN.
                        Some(k) if k != 0 && k != 0x8000_0000 => {
                            let b = self.int(k.wrapping_neg());
                            Src::Sub { a: x, b, res }
                        }
                        _ => Src::Add { a: x, b: m, res },
                    }),
                    Adc | Sbc | Rsc => Some(Src::Carry),
                    _ => None,
                };
                let bits = match src {
                    // A value compared with itself: equal, no borrow, no
                    // overflow, whatever it is.
                    Some(Src::Sub { a: x, b: y, .. }) if x == y => {
                        [(N, false), (Z, true), (C, true), (V, false)].map(|(k, f)| (k, self.boolean(f))).to_vec()
                    }
                    Some(src) => self.flags_of(src, &[N, Z, C, V]),
                    None => {
                        let mut b = self.flags_of(Src::Nz { res }, &[N, Z]);
                        b.extend(shc.map(|v| (C, v)));
                        b
                    }
                };
                if self.flags_in_state() {
                    let call = match op {
                        Sub | Cmp | Rsb | Add | Cmn | Adc | Sbc | Rsc => {
                            if matches!(op, Adc | Sbc | Rsc) {
                                self.need(Reg::Flag(C));
                            }
                            let (xt, mt) = (self.render(x), self.render(m));
                            match op {
                                Sub | Cmp => format!("ros_subs(s, {xt}, {mt})"),
                                Rsb => format!("ros_subs(s, {mt}, {xt})"),
                                Add | Cmn => format!("ros_adds(s, {xt}, {mt})"),
                                Adc => format!("ros_adcs(s, {xt}, {mt})"),
                                Sbc => format!("ros_sbcs(s, {xt}, {mt})"),
                                _ => format!("ros_sbcs(s, {mt}, {xt})"),
                            }
                        }
                        _ => {
                            let (r, _) = self.expr(res);
                            let ct = match shc {
                                Some(v) => self.render(v),
                                None => "s->c".into(),
                            };
                            format!("ros_logic(s, {r}, {ct})")
                        }
                    };
                    self.tier0(c, dest, res, call, &bits);
                } else {
                    if let Some(r) = dest {
                        self.commit(c, r, res);
                    }
                    self.set_flags(c, &bits);
                }
            }

            Insn::Mul { op, s, rd, ra, rm, rs } => {
                if [rd, ra, rm, rs].contains(&15) {
                    return Err("a multiply involving pc".into());
                }
                let (x, y) = (self.get(Reg::R(rm)), self.get(Reg::R(rs)));
                match op {
                    MulOp::Mul | MulOp::Mla | MulOp::Mls => {
                        let p = self.bin(Op::Mul, x, y);
                        let res = match op {
                            MulOp::Mla => {
                                let acc = self.get(Reg::R(ra));
                                self.bin(Op::Add, p, acc)
                            }
                            MulOp::Mls => {
                                let acc = self.get(Reg::R(ra));
                                self.bin(Op::Sub, acc, p)
                            }
                            _ => p,
                        };
                        if !s {
                            self.commit(c, Reg::R(rd), res);
                        } else {
                            let bits = self.flags_of(Src::Nz { res }, &[N, Z]);
                            if self.flags_in_state() {
                                let (r, _) = self.expr(res);
                                self.tier0(c, Some(Reg::R(rd)), res, format!("ros_logic(s, {r}, s->c)"), &bits);
                            } else {
                                self.commit(c, Reg::R(rd), res);
                                self.set_flags(c, &bits);
                            }
                        }
                    }
                    _ => {
                        let signed = matches!(op, MulOp::Smull | MulOp::Smlal);
                        let p = self.val(Node::Mul64 { signed, a: x, b: y }, Ty::I64);
                        let p = if matches!(op, MulOp::Umlal | MulOp::Smlal) {
                            let (hi, lo) = (self.get(Reg::R(rd)), self.get(Reg::R(ra)));
                            let w = self.val(Node::Wide { hi, lo }, Ty::I64);
                            self.bin(Op::Add, w, p)
                        } else {
                            p
                        };
                        let lo = self.val(Node::Lo(p), Ty::I32);
                        let hi = self.val(Node::Hi(p), Ty::I32);
                        self.commit(c, Reg::R(ra), lo);
                        self.commit(c, Reg::R(rd), hi);
                    }
                }
            }

            Insn::Mem { load, width, rt, rn, offset, add, pre, wback } => {
                let writes_back = wback || !pre;
                let pair = width == Width::Double;
                if writes_back && (rn == 15 || rn == rt || (pair && rn == rt + 1)) {
                    return Err("a transfer that writes back to pc or to a register it transfers".into());
                }
                if pair && (rt % 2 != 0 || rt == 14) {
                    return Err("LDRD or STRD of an odd register pair".into());
                }
                if !load && matches!(width, Width::SignedByte | Width::SignedHalf) {
                    return Err("a signed store".into());
                }
                let base = if rn == 15 { self.addr(pc) } else { self.get(Reg::R(rn)) };
                let off = match offset {
                    Offset::Imm(k) => self.named(k),
                    Offset::Reg { rm: 15, .. } => return Err("a register offset of pc".into()),
                    Offset::Reg { rm, shift } => {
                        if let Shift::Reg(..) = shift {
                            return Err("a register-shifted-register offset".into());
                        }
                        let m = self.get(Reg::R(rm));
                        self.shifted(m, shift).0
                    }
                };
                let moved = if add { self.bin(Op::Add, base, off) } else { self.bin(Op::Sub, base, off) };
                let ea = if pre { moved } else { base };
                // Write back first where the address is the new base, so
                // the transfer names it. Otherwise write back last.
                let wb_first = writes_back && pre && c.is_none();
                // A word of a private frame is a local.
                let at = match (rn, offset) {
                    (13, Offset::Imm(k)) if matches!(width, Width::Word | Width::Double) => {
                        let k = if !pre { 0 } else if add { k as i32 } else { -(k as i32) };
                        let addr = self.block[self.i].addr;
                        let s0 = (self.ctx.slot)(addr, k);
                        let s1 = if pair { (self.ctx.slot)(addr, k + 4).map(Some) } else { Some(None) };
                        s0.zip(s1)
                    }
                    _ => None,
                };
                if let Some((s0, s1)) = at {
                    if wb_first {
                        self.commit(c, Reg::R(rn), moved);
                    }
                    if load {
                        let v = self.get(Reg::Slot(s0));
                        self.commit(c, Reg::R(rt), v);
                        if let Some(s1) = s1 {
                            let v2 = self.get(Reg::Slot(s1));
                            self.commit(c, Reg::R(rt + 1), v2);
                        }
                    } else {
                        let x = self.get(Reg::R(rt));
                        self.commit(c, Reg::Slot(s0), x);
                        if let Some(s1) = s1 {
                            let x2 = self.get(Reg::R(rt + 1));
                            self.commit(c, Reg::Slot(s1), x2);
                        }
                    }
                    if writes_back && !wb_first {
                        self.commit(c, Reg::R(rn), moved);
                    }
                    return Ok(());
                }
                if load {
                    let v = self.load_int(width, ea);
                    let v2 = pair.then(|| {
                        let ea4 = self.at_offset(ea, 4);
                        self.load_int(Width::Word, ea4)
                    });
                    if wb_first {
                        self.commit(c, Reg::R(rn), moved);
                    }
                    self.commit(c, Reg::R(rt), v);
                    if let Some(v2) = v2 {
                        self.commit(c, Reg::R(rt + 1), v2);
                    }
                    if writes_back && !wb_first {
                        self.commit(c, Reg::R(rn), moved);
                    }
                } else {
                    let x = if rt == 15 { self.addr(pc) } else { self.get(Reg::R(rt)) };
                    let x2 = pair.then(|| self.get(Reg::R(rt + 1)));
                    if wb_first {
                        self.commit(c, Reg::R(rn), moved);
                    }
                    let f = match width {
                        Width::Byte => "ros_st8",
                        Width::Half => "ros_st16",
                        _ => "ros_st32",
                    };
                    self.store(c, f, ea, x);
                    if let Some(x2) = x2 {
                        let ea4 = self.at_offset(ea, 4);
                        self.store(c, "ros_st32", ea4, x2);
                    }
                    if writes_back && !wb_first {
                        self.commit(c, Reg::R(rn), moved);
                    }
                }
            }

            Insn::Block { load, rn, regs, before, add, wback, user } => {
                // `^` without `pc` transfers the user bank's registers.
                // Compiled code runs only in user mode, because a mode
                // switch faults. In user mode the user bank *is* the
                // current bank. So BASIC's `STMIA SP,{SP}^`, which reads
                // the stack it is on, is an ordinary store. With `pc` in the
                // list it is the exception-return form, which restores a
                // mode, and it stays refused.
                if user && regs & 0x8000 != 0 {
                    return Err("an LDM of the user bank restoring pc and the mode (^)".into());
                }
                if regs == 0 || rn == 15 {
                    return Err("an LDM or STM with no registers, or based on pc".into());
                }
                if wback && regs & (1 << rn) != 0 && (load || regs & ((1u16 << rn) - 1) != 0) {
                    return Err("an LDM or STM that writes back to a register it transfers".into());
                }
                let n = regs.count_ones();
                let base = self.get(Reg::R(rn));
                let (start, back) = self.block_addrs(base, n, 4, before, add);
                let wb_first = wback && start == back && c.is_none();
                let list: Vec<u8> = (0..16u8).filter(|k| regs & (1 << k) != 0).collect();
                // A private frame's words are locals.
                if rn == 13 {
                    let low = match (before, add) {
                        (false, true) => 0,
                        (true, true) => 4,
                        (false, false) => 4 - 4 * n as i32,
                        (true, false) => -4 * n as i32,
                    };
                    let addr = self.block[self.i].addr;
                    let slots: Option<Vec<u8>> =
                        (0..list.len()).map(|k| (self.ctx.slot)(addr, low + 4 * k as i32)).collect();
                    if let Some(slots) = slots {
                        if load {
                            let loaded: Vec<(u8, usize)> =
                                list.iter().zip(&slots).map(|(&r, &sl)| (r, self.get(Reg::Slot(sl)))).collect();
                            if wb_first {
                                self.commit(c, Reg::R(rn), back);
                            }
                            for (r, v) in loaded {
                                self.commit(c, Reg::R(r), v);
                            }
                        } else {
                            let vals: Vec<usize> =
                                list.iter().map(|&r| if r == 15 { self.addr(pc) } else { self.get(Reg::R(r)) }).collect();
                            if wb_first {
                                self.commit(c, Reg::R(rn), back);
                            }
                            for (&sl, x) in slots.iter().zip(vals) {
                                self.commit(c, Reg::Slot(sl), x);
                            }
                        }
                        if wback && !wb_first {
                            self.commit(c, Reg::R(rn), back);
                        }
                        return Ok(());
                    }
                }
                if load {
                    let mut loaded = vec![];
                    for (k, &r) in list.iter().enumerate() {
                        let ea = self.at_offset(start, 4 * k as u32);
                        let v = self.load_int(Width::Word, ea);
                        loaded.push((r, v));
                    }
                    if wb_first {
                        self.commit(c, Reg::R(rn), back);
                    }
                    // If the base is loaded, it goes last, because the
                    // others' addresses read it.
                    loaded.sort_by_key(|&(r, _)| r == rn);
                    for (r, v) in loaded {
                        self.commit(c, Reg::R(r), v);
                    }
                    if wback && !wb_first {
                        self.commit(c, Reg::R(rn), back);
                    }
                } else {
                    let vals: Vec<usize> =
                        list.iter().map(|&r| if r == 15 { self.addr(pc) } else { self.get(Reg::R(r)) }).collect();
                    if wb_first {
                        self.commit(c, Reg::R(rn), back);
                    }
                    for (k, x) in vals.into_iter().enumerate() {
                        let ea = self.at_offset(start, 4 * k as u32);
                        self.store(c, "ros_st32", ea, x);
                    }
                    if wback && !wb_first {
                        self.commit(c, Reg::R(rn), back);
                    }
                }
            }

            Insn::Clz { rd, rm } => {
                if rd == 15 || rm == 15 {
                    return Err("CLZ with pc".into());
                }
                let x = self.get(Reg::R(rm));
                let v = self.call("ros_clz", vec![Arg::V(x)], Ty::I32);
                self.commit(c, Reg::R(rd), v);
            }

            Insn::MovHalf { top, rd, imm } => {
                if rd == 15 {
                    return Err("MOVW or MOVT into pc".into());
                }
                let v = if top {
                    let old = self.get(Reg::R(rd));
                    let (lo, hi) = (self.int(0xFFFF), self.int((imm as u32) << 16));
                    let low = self.bin(Op::And, old, lo);
                    self.bin(Op::Or, low, hi)
                } else {
                    self.int(imm as u32)
                };
                self.commit(c, Reg::R(rd), v);
            }

            // ---- FPA ----
            Insn::FpaDyadic { op, prec, round, fd, fn_, fm } => {
                if round != Round::Nearest {
                    return Err("an FPA operation with directed rounding".into());
                }
                let (x, y) = (self.get(Reg::F(fn_)), self.fpa_operand(fm));
                let (x, y) = self.fp_operands(x, y, prec);
                let f64call = |s: &mut Self, f, l, r| s.call(f, vec![Arg::V(l), Arg::V(r)], Ty::F64);
                let v = match op {
                    FpaDyadic::Adf => self.fbin(Op::Add, x, y),
                    FpaDyadic::Suf => self.fbin(Op::Sub, x, y),
                    FpaDyadic::Rsf => self.fbin(Op::Sub, y, x),
                    FpaDyadic::Muf | FpaDyadic::Fml => self.fbin(Op::Mul, x, y),
                    FpaDyadic::Dvf | FpaDyadic::Fdv => self.fbin(Op::Div, x, y),
                    FpaDyadic::Rdf | FpaDyadic::Frd => self.fbin(Op::Div, y, x),
                    FpaDyadic::Pow => f64call(self, "pow", x, y),
                    FpaDyadic::Rpw => f64call(self, "pow", y, x),
                    FpaDyadic::Rmf => f64call(self, "remainder", x, y),
                    FpaDyadic::Pol => f64call(self, "atan2", y, x),
                };
                // The fast operations deliver single precision whatever the
                // instruction says.
                let prec = if matches!(op, FpaDyadic::Fml | FpaDyadic::Fdv | FpaDyadic::Frd) { Prec::Single } else { prec };
                let v = self.fpa_round(v, prec);
                self.commit(c, Reg::F(fd), v);
            }
            Insn::FpaMonadic { op, prec, round, fd, fm } => {
                use FpaMonadic::*;
                if round != Round::Nearest && !matches!(op, Rnd | Urd) {
                    return Err("an FPA operation with directed rounding".into());
                }
                let x = self.fpa_operand(fm);
                let f = |s: &mut Self, f| s.call(f, vec![Arg::V(x)], Ty::F64);
                let v = match op {
                    Mvf | Nrm => x,
                    Mnf => self.fneg(x),
                    Abs => f(self, "fabs"),
                    Rnd | Urd => f(
                        self,
                        match round {
                            Round::Nearest => "nearbyint",
                            Round::Plus => "ceil",
                            Round::Minus => "floor",
                            Round::Zero => "trunc",
                        },
                    ),
                    Sqt => f(self, "sqrt"),
                    Log => f(self, "log10"),
                    Lgn => f(self, "log"),
                    Exp => f(self, "exp"),
                    Sin => f(self, "sin"),
                    Cos => f(self, "cos"),
                    Tan => f(self, "tan"),
                    Asn => f(self, "asin"),
                    Acs => f(self, "acos"),
                    Atn => f(self, "atan"),
                };
                let v = self.fpa_round(v, prec);
                self.commit(c, Reg::F(fd), v);
            }
            Insn::FpaFlt { prec, round, fn_, rd } => {
                let src = self.get(Reg::R(rd));
                let v = match (prec, round) {
                    (Prec::Single, Round::Nearest) => self.val(Node::FromInt { src, signed: true }, Ty::F32),
                    (Prec::Single, r) => {
                        let x = self.val(Node::FromInt { src, signed: true }, Ty::F64);
                        self.call("ros_to_float", vec![Arg::V(x), Arg::T(round_c(r))], Ty::F32)
                    }
                    _ => self.val(Node::FromInt { src, signed: true }, Ty::F64),
                };
                self.commit(c, Reg::F(fn_), v);
            }
            Insn::FpaFix { round, rd, fm } => {
                if rd == 15 {
                    return Err("FIX into pc".into());
                }
                let x = self.get(Reg::F(fm));
                let v = self.call("(uint32_t)ros_to_int", vec![Arg::V(x), Arg::T(round_c(round))], Ty::I32);
                self.commit(c, Reg::R(rd), v);
            }
            Insn::FpaCompare { negate, fn_, fm, .. } => {
                let x = self.get(Reg::F(fn_));
                let mut y = self.fpa_operand(fm);
                if negate {
                    y = self.fneg(y);
                }
                let bits = self.flags_of(Src::FCmp { a: x, b: y, vfp: false }, &[N, Z, C, V]);
                if self.flags_in_state() || !self.ctx.fpa_lift_ok {
                    let (xt, yt) = (self.render(x), self.render(y));
                    self.tier0(c, None, x, format!("ros_fpa_cmp(s, {xt}, {yt})"), &bits);
                } else {
                    self.set_flags(c, &bits);
                }
            }
            Insn::FpaStatus { write, control, rd } => {
                if control {
                    return Err("the FPA control register, which only the emulator ROSGD never runs had".into());
                }
                if write {
                    let x = self.get(Reg::R(rd));
                    let t = self.render(x);
                    self.effect(c, format!("s->fp->fpsr = {t};"));
                } else {
                    if c.is_some() {
                        self.need(Reg::R(rd));
                    }
                    let d = self.rc(Reg::R(rd));
                    self.effect(c, format!("{d} = s->fp->fpsr;"));
                    self.overwrite(Reg::R(rd));
                }
            }
            Insn::FpaMem { load, prec, fd, rn, offset, add, pre, wback } => {
                let writes_back = wback || !pre;
                if writes_back && rn == 15 {
                    return Err("an FPA transfer writing back to pc".into());
                }
                if prec == Prec::Packed {
                    // A packed decimal load, of the kind BASIC's number
                    // reader makes. The reader packs decimal digits itself
                    // and reads them back through FPEmulator. So the format
                    // is the one the reader wrote, and the runtime unpacks
                    // exactly that. Nothing in the corpus stores one.
                    let base = if rn == 15 { self.addr(pc) } else { self.get(Reg::R(rn)) };
                    let k = self.int(offset);
                    let moved = if add { self.bin(Op::Add, base, k) } else { self.bin(Op::Sub, base, k) };
                    let ea = if pre { moved } else { base };
                    let wb_first = writes_back && pre && c.is_none();
                    if wb_first {
                        self.commit(c, Reg::R(rn), moved);
                    }
                    if load {
                        // Out of a double's range, the load traps if the
                        // FPSR enables the trap, as BASIC does. FPEmulator
                        // then enters the error handler with the program's
                        // R10-R12, and BASIC finds ERL from R12. So those
                        // registers are passed with the load.
                        let fg: Vec<usize> = (10..13).map(|n| self.get(Reg::R(n))).collect();
                        let at = self.render(ea);
                        let fg: Vec<String> = fg.into_iter().map(|v| self.render(v)).collect();
                        if c.is_some() {
                            self.need(Reg::F(fd));
                        }
                        let d = self.rc(Reg::F(fd));
                        self.effect(c, format!("{d} = ros_fpa_ldp_at({at}, {});", fg.join(", ")));
                        self.overwrite(Reg::F(fd));
                    } else {
                        let x = self.get(Reg::F(fd));
                        self.store(c, "ros_fpa_stp", ea, x);
                    }
                    if writes_back && !wb_first {
                        self.commit(c, Reg::R(rn), moved);
                    }
                } else {
                let (bytes, ld, st, ty) = match prec {
                    Prec::Single => (4, "ros_fpa_lds", "ros_fpa_sts", Ty::F32),
                    Prec::Double => (8, "ros_fpa_ldd", "ros_fpa_std", Ty::F64),
                    _ => (12, "ros_fpa_lde", "ros_fpa_ste", Ty::F64),
                };
                let base = if rn == 15 { self.addr(pc) } else { self.get(Reg::R(rn)) };
                let k = self.int(offset);
                let moved = if add { self.bin(Op::Add, base, k) } else { self.bin(Op::Sub, base, k) };
                let ea = if pre { moved } else { base };
                let wb_first = writes_back && pre && c.is_none();
                if load {
                    let konst = match self.vals[ea].node {
                        Node::Addr(x) => self.rom_value(x, bytes, true),
                        _ => None,
                    };
                    let v = match konst {
                        Some(x) => self.val(Node::Lit(x), ty),
                        None => self.val(Node::Load { f: ld, addr: ea, mem: self.mem }, ty),
                    };
                    if wb_first {
                        self.commit(c, Reg::R(rn), moved);
                    }
                    self.commit(c, Reg::F(fd), v);
                    if writes_back && !wb_first {
                        self.commit(c, Reg::R(rn), moved);
                    }
                } else {
                    let x = self.get(Reg::F(fd));
                    if wb_first {
                        self.commit(c, Reg::R(rn), moved);
                    }
                    self.store(c, st, ea, x);
                    if writes_back && !wb_first {
                        self.commit(c, Reg::R(rn), moved);
                    }
                }
                }
            }
            Insn::FpaMulti { load, fd, count, rn, offset, add, pre, wback } => {
                let regs: Vec<Reg> = (0..count).map(|k| Reg::F((fd + k) & 7)).collect();
                let base = self.get(Reg::R(rn));
                let k = self.int(offset);
                let moved = if add { self.bin(Op::Add, base, k) } else { self.bin(Op::Sub, base, k) };
                let start = if pre { moved } else { base };
                let writes_back = wback || !pre;
                let wb_first = writes_back && pre && c.is_none();
                if load {
                    let mut vs = vec![];
                    for (k, &r) in regs.iter().enumerate() {
                        let ea = self.at_offset(start, 12 * k as u32);
                        let v = self.val(Node::Load { f: "ros_fpa_lde", addr: ea, mem: self.mem }, Ty::F64);
                        vs.push((r, v));
                    }
                    if wb_first {
                        self.commit(c, Reg::R(rn), moved);
                    }
                    for (r, v) in vs {
                        self.commit(c, r, v);
                    }
                    if writes_back && !wb_first {
                        self.commit(c, Reg::R(rn), moved);
                    }
                } else {
                    let xs: Vec<usize> = regs.iter().map(|&r| self.get(r)).collect();
                    if wb_first {
                        self.commit(c, Reg::R(rn), moved);
                    }
                    for (k, x) in xs.into_iter().enumerate() {
                        let ea = self.at_offset(start, 12 * k as u32);
                        self.store(c, "ros_fpa_ste", ea, x);
                    }
                    if writes_back && !wb_first {
                        self.commit(c, Reg::R(rn), moved);
                    }
                }
            }

            // ---- VFP ----
            Insn::VfpArith { op, double, vd, vn, vm } => {
                let r = |n: u8| if double { Reg::D(n) } else { Reg::S(n) };
                let (x, y) = (self.get(r(vn)), self.get(r(vm)));
                let accumulates = !matches!(op, VfpOp::Add | VfpOp::Sub | VfpOp::Mul | VfpOp::Div | VfpOp::Nmul);
                let acc = if accumulates { Some(self.get(r(vd))) } else { None };
                let ty = if double { Ty::F64 } else { Ty::F32 };
                let fma = if double { "fma" } else { "fmaf" };
                let single = !double;
                // Each rounding step, with its exceptions.
                let step = |s: &mut Self, o: Op, a: usize, b: usize| {
                    let v = s.fbin(o, a, b);
                    let f = if o == Op::Div { "ros_vfp_div_ex" } else { "ros_vfp_ex2" };
                    s.vfp_ex(c, f, single, &[v, a, b], "");
                    v
                };
                let fused = |s: &mut Self, a, b, d| {
                    let v = s.call(fma, vec![Arg::V(a), Arg::V(b), Arg::V(d)], ty);
                    s.vfp_ex(c, "ros_vfp_ex3", single, &[v, a, b, d], "");
                    v
                };
                let v = match op {
                    VfpOp::Add => step(self, Op::Add, x, y),
                    VfpOp::Sub => step(self, Op::Sub, x, y),
                    VfpOp::Mul => step(self, Op::Mul, x, y),
                    VfpOp::Div => step(self, Op::Div, x, y),
                    VfpOp::Nmul => {
                        let p = step(self, Op::Mul, x, y);
                        self.fneg(p)
                    }
                    // Not fused: the product is rounded, then the sum.
                    VfpOp::Mla => {
                        let p = step(self, Op::Mul, x, y);
                        step(self, Op::Add, acc.unwrap(), p)
                    }
                    VfpOp::Mls => {
                        let p = step(self, Op::Mul, x, y);
                        step(self, Op::Sub, acc.unwrap(), p)
                    }
                    VfpOp::Nmla => {
                        let p = step(self, Op::Mul, x, y);
                        let nd = self.fneg(acc.unwrap());
                        step(self, Op::Sub, nd, p)
                    }
                    VfpOp::Nmls => {
                        let p = step(self, Op::Mul, x, y);
                        let nd = self.fneg(acc.unwrap());
                        step(self, Op::Add, nd, p)
                    }
                    VfpOp::Fma => fused(self, x, y, acc.unwrap()),
                    VfpOp::Fms => {
                        let nx = self.fneg(x);
                        fused(self, nx, y, acc.unwrap())
                    }
                    VfpOp::Fnma => {
                        let nx = self.fneg(x);
                        let nd = self.fneg(acc.unwrap());
                        fused(self, nx, y, nd)
                    }
                    VfpOp::Fnms => {
                        let nd = self.fneg(acc.unwrap());
                        fused(self, x, y, nd)
                    }
                };
                self.commit(c, r(vd), v);
            }
            Insn::VfpUnary { op, double, vd, vm } => {
                let r = |n: u8| if double { Reg::D(n) } else { Reg::S(n) };
                let ty = if double { Ty::F64 } else { Ty::F32 };
                let x = self.get(r(vm));
                let v = match op {
                    VfpUnary::Mov => x,
                    VfpUnary::Neg => self.fneg(x),
                    VfpUnary::Abs => self.call(if double { "fabs" } else { "fabsf" }, vec![Arg::V(x)], ty),
                    VfpUnary::Sqrt => {
                        self.vfp_ex(c, "ros_vfp_sqrt_ex", !double, &[x], "");
                        self.call(if double { "sqrt" } else { "sqrtf" }, vec![Arg::V(x)], ty)
                    }
                };
                self.commit(c, r(vd), v);
            }
            Insn::VfpMovImm { double, vd, bits } => {
                let v = if double {
                    self.val(Node::Lit(f64::from_bits(bits)), Ty::F64)
                } else {
                    self.val(Node::Lit(f32::from_bits(bits as u32) as f64), Ty::F32)
                };
                self.commit(c, if double { Reg::D(vd) } else { Reg::S(vd) }, v);
            }
            Insn::VfpMem { load, double, v: vr, rn, offset, add } => {
                let (ty, bytes, r) = if double { (Ty::F64, 8, Reg::D(vr)) } else { (Ty::F32, 4, Reg::S(vr)) };
                let base = if rn == 15 { self.addr(pc) } else { self.get(Reg::R(rn)) };
                let k = self.int(offset);
                let ea = if add { self.bin(Op::Add, base, k) } else { self.bin(Op::Sub, base, k) };
                if load {
                    let konst = match self.vals[ea].node {
                        Node::Addr(x) => self.rom_value(x, bytes, false),
                        _ => None,
                    };
                    let v = match konst {
                        Some(x) => self.val(Node::Lit(x), ty),
                        None => {
                            let f = if double { "ros_ldd" } else { "ros_lds" };
                            self.val(Node::Load { f, addr: ea, mem: self.mem }, ty)
                        }
                    };
                    self.commit(c, r, v);
                } else {
                    let x = self.get(r);
                    self.store(c, if double { "ros_std" } else { "ros_sts" }, ea, x);
                }
            }
            Insn::VfpMulti { load, double, first, count, rn, add, pre, wback } => {
                let regs: Vec<Reg> =
                    (0..count).map(|k| if double { Reg::D(first + k) } else { Reg::S(first + k) }).collect();
                let unit = if double { 8u32 } else { 4 };
                let base = self.get(Reg::R(rn));
                let (start, back) = self.block_addrs(base, count as u32, unit, pre, add);
                let wb_first = wback && start == back && c.is_none();
                let (ld, st, ty) = if double { ("ros_ldd", "ros_std", Ty::F64) } else { ("ros_lds", "ros_sts", Ty::F32) };
                if load {
                    let mut vs = vec![];
                    for (k, &r) in regs.iter().enumerate() {
                        let ea = self.at_offset(start, unit * k as u32);
                        let v = self.val(Node::Load { f: ld, addr: ea, mem: self.mem }, ty);
                        vs.push((r, v));
                    }
                    if wb_first {
                        self.commit(c, Reg::R(rn), back);
                    }
                    for (r, v) in vs {
                        self.commit(c, r, v);
                    }
                    if wback && !wb_first {
                        self.commit(c, Reg::R(rn), back);
                    }
                } else {
                    let xs: Vec<usize> = regs.iter().map(|&r| self.get(r)).collect();
                    if wb_first {
                        self.commit(c, Reg::R(rn), back);
                    }
                    for (k, x) in xs.into_iter().enumerate() {
                        let ea = self.at_offset(start, unit * k as u32);
                        self.store(c, st, ea, x);
                    }
                    if wback && !wb_first {
                        self.commit(c, Reg::R(rn), back);
                    }
                }
            }
            Insn::VfpCompare { double, exception, vd, vm } => {
                let r = |n: u8| if double { Reg::D(n) } else { Reg::S(n) };
                let x = self.get(r(vd));
                let y = match vm {
                    Some(m) => self.get(r(m)),
                    None => self.val(Node::Lit(0.0), if double { Ty::F64 } else { Ty::F32 }),
                };
                self.vfp_ex(c, "ros_vfp_cmp_ex", !double, &[x, y], if exception { "1" } else { "0" });
                // Paired with the VMRS after it, the two make one compare
                // that sets the flags. Otherwise it sets the FPSCR's flags.
                let paired = c.is_none()
                    && self.block.get(i + 1).is_some_and(|n| {
                        n.caller.is_none()
                            && n.d == Decoded { cond: Cond::Al, insn: Insn::VfpStatus { to_core: true, rt: 15 } }
                    });
                if paired {
                    self.vcmp = Some((x, y));
                } else {
                    let (xt, yt) = (self.render(x), self.render(y));
                    self.effect(c, format!("ros_vfp_cmp(s, {xt}, {yt});"));
                }
            }
            Insn::VfpCvt { cvt, vd, vm } => match cvt {
                VfpCvt::Precision { to_double } => {
                    let (src, dst, ty) = if to_double {
                        (Reg::S(vm), Reg::D(vd), Ty::F64)
                    } else {
                        (Reg::D(vm), Reg::S(vd), Ty::F32)
                    };
                    let x = self.get(src);
                    let v = self.val(Node::Cast(ty, x), ty);
                    if to_double {
                        self.vfp_ex(c, "ros_vfp_widen_ex", false, &[x], "");
                    } else {
                        self.vfp_ex(c, "ros_vfp_narrow_ex", false, &[v, x], "");
                    }
                    self.commit(c, dst, v);
                }
                VfpCvt::FromInt { signed, double } => {
                    // The integer's bits are in the state's single.
                    self.need(Reg::S(vm));
                    self.need(Reg::D(vm / 2));
                    let src = self.val(Node::Bits { r: Reg::S(vm), at: i }, Ty::I32);
                    let ty = if double { Ty::F64 } else { Ty::F32 };
                    let v = self.val(Node::FromInt { src, signed }, ty);
                    self.commit(c, if double { Reg::D(vd) } else { Reg::S(vd) }, v);
                }
                VfpCvt::ToInt { signed, double, toward_zero } => {
                    let x = self.get(if double { Reg::D(vm) } else { Reg::S(vm) });
                    self.need(Reg::D(vd / 2));
                    if c.is_some() {
                        self.need(Reg::S(vd));
                    }
                    let xt = self.render(x);
                    let mode = if toward_zero { "ROS_ROUND_ZERO" } else { "(int)(s->fp->fpscr >> 22 & 3)" };
                    let f = if signed { "(uint32_t)ros_to_int" } else { "ros_to_uint" };
                    self.effect(c, format!("s->fp->fpscr |= ros_vfp_int_ex({xt}, {mode}, {});", signed as u8));
                    self.effect(c, format!("SW[{vd}] = {f}({xt}, {mode});"));
                    self.overwrite(Reg::S(vd));
                }
            },
            Insn::VfpMovCore { to_core, sn, rt } => {
                if rt == 15 {
                    return Err("VMOV with pc".into());
                }
                if to_core {
                    self.need(Reg::S(sn));
                    self.need(Reg::D(sn / 2));
                    let v = self.val(Node::Bits { r: Reg::S(sn), at: i }, Ty::I32);
                    self.commit(c, Reg::R(rt), v);
                } else {
                    let x = self.get(Reg::R(rt));
                    self.need(Reg::D(sn / 2));
                    if c.is_some() {
                        self.need(Reg::S(sn));
                    }
                    let t = self.render(x);
                    self.effect(c, format!("SW[{sn}] = {t};"));
                    self.overwrite(Reg::S(sn));
                }
            }
            Insn::VfpMovCore2 { to_core, double, vm, rt, rt2 } => {
                if rt == 15 || rt2 == 15 {
                    return Err("VMOV with pc".into());
                }
                let regs: Vec<Reg> = if double { vec![Reg::D(vm)] } else { vec![Reg::S(vm), Reg::S(vm + 1)] };
                for &r in &regs {
                    self.need(r);
                    for o in r.overlaps() {
                        self.need(o);
                    }
                }
                if to_core {
                    if double {
                        let w = self.val(Node::Bits { r: Reg::D(vm), at: i }, Ty::I64);
                        let (lo, hi) = (self.val(Node::Lo(w), Ty::I32), self.val(Node::Hi(w), Ty::I32));
                        self.commit(c, Reg::R(rt), lo);
                        self.commit(c, Reg::R(rt2), hi);
                    } else {
                        let lo = self.val(Node::Bits { r: Reg::S(vm), at: i }, Ty::I32);
                        let hi = self.val(Node::Bits { r: Reg::S(vm + 1), at: i }, Ty::I32);
                        self.commit(c, Reg::R(rt), lo);
                        self.commit(c, Reg::R(rt2), hi);
                    }
                } else {
                    let (x, y) = (self.get(Reg::R(rt)), self.get(Reg::R(rt2)));
                    let (xt, yt) = (self.render(x), self.render(y));
                    if double {
                        let yt = if self.vals[y].ty == Ty::I32 { format!("(uint64_t){yt}") } else { yt };
                        self.effect(c, format!("DW[{vm}] = {yt} << 32 | {xt};"));
                    } else {
                        self.effect(c, format!("SW[{vm}] = {xt};"));
                        self.effect(c, format!("SW[{}] = {yt};", vm + 1));
                    }
                    for r in regs {
                        self.overwrite(r);
                    }
                }
            }
            Insn::VfpStatus { to_core, rt } => match (to_core, rt, vcmp) {
                // The compare before it, setting the flags.
                (true, 15, Some((x, y))) => {
                    let bits = self.flags_of(Src::FCmp { a: x, b: y, vfp: true }, &[N, Z, C, V]);
                    if self.flags_in_state() {
                        let (xt, yt) = (self.render(x), self.render(y));
                        self.stmts.push(format!("ros_vfp_cmp(s, {xt}, {yt});"));
                        self.tier0(None, None, x, "ros_vmrs_flags(s)".into(), &bits);
                    } else {
                        self.set_flags(None, &bits);
                    }
                }
                (true, 15, None) => {
                    if c.is_some() {
                        for k in [N, Z, C, V] {
                            self.need(Reg::Flag(k));
                        }
                    }
                    self.effect(c, "ros_vmrs_flags(s);".into());
                    for k in [N, Z, C, V] {
                        self.overwrite(Reg::Flag(k));
                    }
                }
                (true, _, _) => {
                    if c.is_some() {
                        self.need(Reg::R(rt));
                    }
                    let d = self.rc(Reg::R(rt));
                    self.effect(c, format!("{d} = s->fp->fpscr;"));
                    self.overwrite(Reg::R(rt));
                }
                (false, _, _) => {
                    let x = self.get(Reg::R(rt));
                    let t = self.render(x);
                    self.effect(c, format!("s->fp->fpscr = {t};"));
                }
            },

            // NEON's integer lanes, treated as the four singles that make
            // a quad. The words travel as bits, since SW[n] reads and
            // writes exactly what the lanes hold.
            Insn::NeonLane { op, vd, vn, vm } => {
                for r in [vn, vm] {
                    for i in 0..4u8 {
                        self.need(Reg::S(4 * r + i));
                    }
                }
                if matches!(op, crate::a32::NeonVecOp::Mla) {
                    for i in 0..4u8 {
                        self.need(Reg::S(4 * vd + i));
                    }
                }
                for i in 0..4u8 {
                    let (a, b, m) = (4 * vd + i, 4 * vn + i, 4 * vm + i);
                    let expr = match op {
                        crate::a32::NeonVecOp::Add => {
                            format!("(uint32_t)((int32_t)SW[{b}] + (int32_t)SW[{m}])")
                        }
                        crate::a32::NeonVecOp::Sub => {
                            format!("(uint32_t)((int32_t)SW[{b}] - (int32_t)SW[{m}])")
                        }
                        crate::a32::NeonVecOp::Mul => {
                            format!("(uint32_t)((int32_t)SW[{b}] * (int32_t)SW[{m}])")
                        }
                        crate::a32::NeonVecOp::Mla => {
                            format!("(uint32_t)((int32_t)SW[{a}] + (int32_t)SW[{b}] * (int32_t)SW[{m}])")
                        }
                    };
                    self.effect(c, format!("SW[{a}] = {expr};"));
                }
                for i in 0..4u8 {
                    self.overwrite(Reg::S(4 * vd + i));
                }
            }
            Insn::NeonDup { vd, rt } => {
                self.need(Reg::R(rt));
                for i in 0..4u8 {
                    self.effect(c, format!("SW[{}] = R[{rt}];", 4 * vd + i));
                }
                for i in 0..4u8 {
                    self.overwrite(Reg::S(4 * vd + i));
                }
            }
            // The pairwise add that folds a quad to a double. Each half of
            // the result is the sum of the two words of one source double.
            Insn::NeonPadd { vd, vn, vm } => {
                for r in [2 * vn, 2 * vn + 1, 2 * vm, 2 * vm + 1] {
                    self.need(Reg::S(r));
                }
                self.effect(
                    c,
                    format!(
                        "SW[{}] = (uint32_t)((int32_t)SW[{}] + (int32_t)SW[{}]);",
                        2 * vd, 2 * vn, 2 * vn + 1
                    ),
                );
                self.effect(
                    c,
                    format!(
                        "SW[{}] = (uint32_t)((int32_t)SW[{}] + (int32_t)SW[{}]);",
                        2 * vd + 1, 2 * vm, 2 * vm + 1
                    ),
                );
                self.overwrite(Reg::S(2 * vd));
                self.overwrite(Reg::S(2 * vd + 1));
            }

            _ => return Err(format!("{:?}: not an instruction the lifter compiles", d.insn)),
        }
        Ok(())
    }

    /// An instruction the caller compiles. What it reads must be in the
    /// state first. Its condition comes from the lifter. Afterwards, what it
    /// wrote is what the state holds.
    fn caller(&mut self, i: usize, cl: Caller) {
        let d = self.block[i].d;
        let c = (d.cond != Cond::Al).then(|| self.cond_node(d.cond));
        self.vcmp = None;
        self.need_all(cl.needs);
        if c.is_some() {
            // If it does not run, what it would have written stays as it
            // was, so the old values must be in the state.
            self.need_all(cl.writes);
        }
        if let Some(c) = c {
            let (t, _) = self.text(c);
            self.out.cond.insert(self.block[i].addr, t);
            let ci = self.cond_node(invert(d.cond));
            let (t, _) = self.text(ci);
            self.out.cond_inv.insert(self.block[i].addr, t);
        }
        let mut touched: Vec<Reg> = self
            .cur
            .keys()
            .chain(self.state.keys())
            .copied()
            .filter(|r| cl.writes.has(*r))
            .collect();
        touched.extend((0..15u8).filter(|n| cl.writes.int >> n & 1 != 0).map(Reg::R));
        touched.extend([N, Z, C, V].into_iter().filter(|&k| cl.writes.flags & (8 >> k) != 0).map(Reg::Flag));
        touched.sort();
        touched.dedup();
        for r in touched {
            self.overwrite(r);
        }
        if cl.writes_memory {
            self.mem += 1;
        }
        // Past `if (c) return;`, c did not hold. So a register waiting to
        // be written a value it already holds on this path need not be
        // written.
        if let (Some(c), true) = (c, cl.leaves) {
            let regs: Vec<Reg> = self.cur.keys().copied().collect();
            for r in regs {
                let v = self.cur[&r];
                if let Some(&s) = self.state.get(&r) {
                    if s != v && self.assume(v, c, false, 4) == s {
                        self.cur.insert(r, s);
                    }
                }
            }
        }
    }
}

/// One pass over the block, under a plan.
fn run<'a>(ctx: &'a BlockCtx<'a>, block: &'a [BlockInsn], plan: &'a Plan, next_temp: u32) -> Result<Pass<'a>, String> {
    let mut p = Pass::new(ctx, block, plan, next_temp);
    for (i, b) in block.iter().enumerate() {
        p.i = i;
        match b.caller {
            Some(cl) => p.caller(i, cl),
            None => p.step(i).map_err(|e| format!("&{:08X}: {e}", b.addr))?,
        }
        let mut s = std::mem::take(&mut p.pre);
        s.extend(std::mem::take(&mut p.stmts));
        p.out.at.insert(b.addr, merge_ifs(s));
    }
    // Falling out of the block, what the next one reads is in the state.
    if ctx.falls_out {
        p.i = block.len();
        p.need_all(ctx.live_out);
    }
    Ok(p)
}

/// Lift one block. `next_temp` numbers variables across a function.
pub fn lift(block: &[BlockInsn], ctx: &BlockCtx, next_temp: &mut u32) -> Result<BlockOut, String> {
    let mut plan = Plan::default();
    let mut settled = None;
    for _ in 0..500 {
        let p = run(ctx, block, &plan, *next_temp)?;
        let over = over_read(&p);
        if p.up.temp.is_empty() && p.up.events.is_empty() && p.up.flags.is_empty() && over.is_empty() {
            settled = Some(p.into_parts());
            break;
        }
        let up = p.up;
        plan.temp.extend(up.temp);
        plan.events.extend(up.events);
        plan.flags.extend(up.flags);
        plan.no_inline.extend(over);
    }
    let Some((mut out, mut next, mut read)) = settled else {
        return Err("the lifter's plan did not settle".into());
    };
    // The plan only grew, so a later pass may not read at all a variable
    // that an earlier pass wanted. Leave those out, provided nothing else
    // changes.
    loop {
        let unused: Vec<usize> = plan.temp.iter().copied().filter(|v| !read.contains(v)).collect();
        if unused.is_empty() {
            break;
        }
        let mut trial = plan.clone();
        for v in &unused {
            trial.temp.remove(v);
        }
        let parts = {
            let q = run(ctx, block, &trial, *next_temp)?;
            let clean = q.up.temp.is_empty() && q.up.events.is_empty() && q.up.flags.is_empty() && over_read(&q).is_empty();
            clean.then(|| q.into_parts())
        };
        let Some(parts) = parts else { break };
        plan = trial;
        (out, next, read) = parts;
    }
    *next_temp = next;
    Ok(out)
}

/// Values written out in full more than once, counting only the outermost.
/// A value inside one of them was counted once for each time the outer
/// value was written out.
fn over_read(p: &Pass) -> Vec<usize> {
    let over: HashSet<usize> = p.inlined.iter().filter(|&(_, &n)| n > 1).map(|(&v, _)| v).collect();
    let mut inside: HashSet<usize> = HashSet::new();
    for &u in &over {
        let mut stack = p.operands(u);
        while let Some(x) = stack.pop() {
            if inside.insert(x) {
                stack.extend(p.operands(x));
            }
        }
    }
    let mut v: Vec<usize> = over.into_iter().filter(|x| !inside.contains(x)).collect();
    v.sort();
    v
}

impl<'a> Pass<'a> {
    fn into_parts(self) -> (BlockOut, u32, HashSet<usize>) {
        (self.out, self.next_temp, self.temp_reads)
    }

    /// The values a value is made of.
    fn operands(&self, v: usize) -> Vec<usize> {
        match &self.vals[v].node {
            Node::Neg(a) | Node::Not(a) | Node::LNot(a) | Node::Cast(_, a) | Node::SExt(_, a) => vec![*a],
            Node::Lo(a) | Node::Hi(a) => vec![*a],
            Node::FromInt { src, .. } => vec![*src],
            Node::Load { addr, .. } => vec![*addr],
            Node::Bin(_, a, b) | Node::Mul64 { a, b, .. } => vec![*a, *b],
            Node::Wide { hi, lo } => vec![*hi, *lo],
            Node::Select(c, a, b) => vec![*c, *a, *b],
            Node::Call(_, args) => args.iter().filter_map(|a| if let Arg::V(x) = a { Some(*x) } else { None }).collect(),
            Node::Flag { src, .. } => match *src {
                Src::Sub { a, b, res } | Src::Add { a, b, res } => vec![a, b, res],
                Src::Nz { res } => vec![res],
                Src::FCmp { a, b, .. } => vec![a, b],
                Src::Carry => vec![],
            },
            Node::Cond(_, bits) => bits.iter().copied().filter(|&b| b != usize::MAX).collect(),
            _ => vec![],
        }
    }
}

// ---- facts about instructions, for the caller's flow analysis ------------------------

pub fn is_fp(i: &Insn) -> bool {
    matches!(
        i,
        Insn::FpaDyadic { .. }
            | Insn::FpaMonadic { .. }
            | Insn::FpaFlt { .. }
            | Insn::FpaFix { .. }
            | Insn::FpaStatus { .. }
            | Insn::FpaCompare { .. }
            | Insn::FpaMem { .. }
            | Insn::FpaMulti { .. }
            | Insn::VfpMem { .. }
            | Insn::VfpMulti { .. }
            | Insn::VfpArith { .. }
            | Insn::VfpUnary { .. }
            | Insn::VfpMovImm { .. }
            | Insn::VfpCompare { .. }
            | Insn::VfpCvt { .. }
            | Insn::VfpMovCore { .. }
            | Insn::VfpMovCore2 { .. }
            | Insn::VfpStatus { .. }
            | Insn::NeonLane { .. }
            | Insn::NeonDup { .. }
            | Insn::NeonPadd { .. }
    )
}

/// Integer registers an FP instruction reads and writes.
pub fn fp_int_use_def(i: &Insn) -> (u16, u16) {
    let b = |r: u8| if r == 15 { 0 } else { 1u16 << r };
    match *i {
        Insn::FpaFlt { rd, .. } => (b(rd), 0),
        Insn::FpaFix { rd, .. } => (0, b(rd)),
        Insn::FpaStatus { write: true, rd, .. } => (b(rd), 0),
        Insn::FpaStatus { write: false, rd, .. } => (0, b(rd)),
        // LDFP reads R10-R12 as well, for the error handler (see the lifting)
        Insn::FpaMem { load: true, prec: Prec::Packed, rn, wback, pre, .. } => {
            (b(rn) | 7 << 10, if wback || !pre { b(rn) } else { 0 })
        }
        Insn::FpaMem { rn, wback, pre, .. } | Insn::FpaMulti { rn, wback, pre, .. } => {
            (b(rn), if wback || !pre { b(rn) } else { 0 })
        }
        Insn::VfpMem { rn, .. } => (b(rn), 0),
        Insn::VfpMulti { rn, wback, .. } => (b(rn), if wback { b(rn) } else { 0 }),
        Insn::VfpMovCore { to_core: true, rt, .. } => (0, b(rt)),
        Insn::VfpMovCore { to_core: false, rt, .. } => (b(rt), 0),
        Insn::VfpMovCore2 { to_core: true, rt, rt2, .. } => (0, b(rt) | b(rt2)),
        Insn::VfpMovCore2 { to_core: false, rt, rt2, .. } => (b(rt) | b(rt2), 0),
        Insn::VfpStatus { to_core: true, rt, .. } => (0, b(rt)),
        Insn::VfpStatus { to_core: false, rt, .. } => (b(rt), 0),
        _ => (0, 0),
    }
}

pub fn fp_writes_memory(i: &Insn) -> bool {
    matches!(
        *i,
        Insn::FpaMem { load: false, .. }
            | Insn::FpaMulti { load: false, .. }
            | Insn::VfpMem { load: false, .. }
            | Insn::VfpMulti { load: false, .. }
    )
}

/// FP registers an instruction reads and writes. Only writes that always
/// happen are counted, because a conditional instruction's writes do not
/// kill the old value.
pub fn fp_use_def(d: &Decoded) -> (u128, u128) {
    let f = |n: u8| Reg::F(n).fp_mask();
    let v = |double: bool, n: u8| if double { Reg::D(n).fp_mask() } else { Reg::S(n).fp_mask() };
    let op = |o: FpaOperand| if let FpaOperand::Reg(n) = o { f(n) } else { 0 };
    let (u, def) = match d.insn {
        Insn::FpaDyadic { fd, fn_, fm, .. } => (f(fn_) | op(fm), f(fd)),
        Insn::FpaMonadic { fd, fm, .. } => (op(fm), f(fd)),
        Insn::FpaFlt { fn_, .. } => (0, f(fn_)),
        Insn::FpaFix { fm, .. } => (f(fm), 0),
        Insn::FpaCompare { fn_, fm, .. } => (f(fn_) | op(fm), 0),
        Insn::FpaMem { load: true, fd, .. } => (0, f(fd)),
        Insn::FpaMem { load: false, fd, .. } => (f(fd), 0),
        Insn::FpaMulti { load, fd, count, .. } => {
            let m = (0..count).fold(0, |m, k| m | f((fd + k) & 7));
            if load { (0, m) } else { (m, 0) }
        }
        Insn::VfpArith { op: o, double, vd, vn, vm } => {
            let acc = !matches!(o, VfpOp::Add | VfpOp::Sub | VfpOp::Mul | VfpOp::Div | VfpOp::Nmul);
            (v(double, vn) | v(double, vm) | if acc { v(double, vd) } else { 0 }, v(double, vd))
        }
        Insn::VfpUnary { double, vd, vm, .. } => (v(double, vm), v(double, vd)),
        Insn::VfpMovImm { double, vd, .. } => (0, v(double, vd)),
        Insn::VfpMem { load: true, double, v: r, .. } => (0, v(double, r)),
        Insn::VfpMem { load: false, double, v: r, .. } => (v(double, r), 0),
        Insn::VfpMulti { load, double, first, count, .. } => {
            let m = (0..count).fold(0, |m, k| m | v(double, first + k));
            if load { (0, m) } else { (m, 0) }
        }
        Insn::VfpCompare { double, vd, vm, .. } => (v(double, vd) | vm.map_or(0, |m| v(double, m)), 0),
        Insn::VfpCvt { cvt, vd, vm } => match cvt {
            VfpCvt::Precision { to_double } => (v(!to_double, vm), v(to_double, vd)),
            VfpCvt::FromInt { double, .. } => (v(false, vm), v(double, vd)),
            VfpCvt::ToInt { double, .. } => (v(double, vm), v(false, vd)),
        },
        Insn::VfpMovCore { to_core: true, sn, .. } => (v(false, sn), 0),
        Insn::VfpMovCore { to_core: false, sn, .. } => (0, v(false, sn)),
        Insn::VfpMovCore2 { to_core, double, vm, .. } => {
            let m = if double { v(true, vm) } else { v(false, vm) | v(false, vm + 1) };
            if to_core { (m, 0) } else { (0, m) }
        }
        _ => (0, 0),
    };
    if d.cond == Cond::Al {
        (u, def)
    } else {
        // If not taken, the old value stays, so the destination is also
        // read.
        (u | def, 0)
    }
}

/// NZCV an FP instruction sets.
pub fn fp_flags_set(i: &Insn) -> u8 {
    match i {
        Insn::FpaCompare { .. } => 0xF,
        Insn::VfpStatus { to_core: true, rt: 15 } => 0xF,
        _ => 0,
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::a32::decode;

    /// A block of words at &FC000000, every instruction lifted except
    /// branches, which exit to wherever `exit` says is live.
    fn block(words: &[u32], exit: Live) -> Vec<BlockInsn> {
        words
            .iter()
            .enumerate()
            .map(|(k, &w)| {
                let d = decode(w);
                let caller = (!lifts(&d)).then_some(Caller { needs: exit, writes: Live::default(), writes_memory: false, leaves: false });
                BlockInsn { addr: 0xFC00_0000 + 4 * k as u32, d, caller }
            })
            .collect()
    }

    fn run_with(words: &[u32], exit: Live, live_out: Live, rom: &dyn Fn(u32) -> Option<(u32, bool)>) -> (Vec<String>, BlockOut) {
        run_locals(words, exit, live_out, rom, 0)
    }

    fn run_locals(
        words: &[u32],
        exit: Live,
        live_out: Live,
        rom: &dyn Fn(u32) -> Option<(u32, bool)>,
        locals: u16,
    ) -> (Vec<String>, BlockOut) {
        let b = block(words, exit);
        let falls_out = b.last().is_some_and(|x| x.caller.is_none() || x.d.cond != Cond::Al);
        let label = |a: u32| (a == 0xFC00_0100).then(|| "T_Table".to_string());
        let ctx = BlockCtx {
            rom_word: rom,
            label: &label,
            konst_name: &|_, _| None,
            field: &|_| None,
            slot: &|_, _| None,
            falls_out,
            live_out,
            fpa_lift_ok: true,
            locals,
        };
        let mut n = 1;
        let out = lift(&b, &ctx, &mut n).unwrap_or_else(|e| panic!("{e}"));
        let mut all = vec![];
        for x in &b {
            all.extend(out.at[&x.addr].clone());
            if let Some(c) = out.cond.get(&x.addr) {
                all.push(format!("<{c}>"));
            }
        }
        (all, out)
    }

    fn run(words: &[u32], live_out: Live) -> Vec<String> {
        run_with(words, Live::ALL, live_out, &|_| None).0
    }

    fn fpa(mnemonic: &str, operands: &str) -> u32 {
        match crate::fpa::encode(mnemonic, operands) {
            Some(crate::legalize::Legalized::RawWord(w)) => w,
            other => panic!("{mnemonic} {operands}: {other:?}"),
        }
    }

    fn ints(regs: &[u8]) -> Live {
        Live { int: regs.iter().fold(0, |m, &r| m | 1 << r), fp: 0, flags: 0, slots: 0 }
    }

    fn fp(regs: &[Reg]) -> Live {
        Live { int: 0, fp: regs.iter().fold(0, |m, r| m | r.fp_mask()), flags: 0, slots: 0 }
    }

    // ---- floating point ----

    #[test]
    fn get_angle_is_one_expression() {
        // MakePSFont's get_angle, with its two single-precision literals in
        // a pool at &FC000100 (the instructions start at &FC000000).
        let words = [
            fpa("FLTD", "f0, r0"),
            fpa("LDFS", "f1, [pc, #244]"), // &FC000100
            fpa("DVFD", "f0, f0, f1"),
            fpa("ATND", "f0, f0"),
            fpa("LDFS", "f1, [pc, #236]"), // &FC000104
            fpa("MUFD", "f0, f0, f1"),
            fpa("FIXZ", "r0, f0"),
        ];
        let rom = |a: u32| match a {
            0xFC00_0100 => Some((5_729.578_f32.to_bits(), false)),
            0xFC00_0104 => Some(((-1000.0f32).to_bits(), false)),
            _ => None,
        };
        // With f0 and f1 dead afterwards, nothing is written back.
        let (c, _) = run_with(&words, Live::ALL, ints(&[0]), &rom);
        assert_eq!(
            c,
            vec!["R[0] = (uint32_t)ros_to_int(atan((double)(int32_t)R[0] / 5729.578f) * -1000.0f, ROS_ROUND_ZERO);"],
            "{c:#?}"
        );
        // With f0 live, it is written where it is made, and read from there.
        let (c, _) = run_with(&words, Live::ALL, ints(&[0]).or(fp(&[Reg::F(0)])), &rom);
        assert_eq!(
            c,
            vec![
                "F[0] = atan((double)(int32_t)R[0] / 5729.578f) * -1000.0f;",
                "R[0] = (uint32_t)ros_to_int(F[0], ROS_ROUND_ZERO);",
            ],
            "{c:#?}"
        );
    }

    #[test]
    fn a_hypotenuse_in_vfp() {
        let words = [
            0xED90_0B00, // vldr d0, [r0]
            0xED90_1B02, // vldr d1, [r0, #8]
            0xEE20_2B00, // vmul.f64 d2, d0, d0
            0xEE01_2B01, // vmla.f64 d2, d1, d1
            0xEEB1_2BC2, // vsqrt.f64 d2, d2
            0xED81_2B00, // vstr d2, [r1]
        ];
        let c = run(&words, Live::default());
        // Each rounding step ORs its exceptions into the FPSCR, so its
        // result is kept to be looked at.
        assert_eq!(
            c,
            vec![
                "D[0] = ros_ldd(R[0]);",
                "D[1] = ros_ldd(R[0] + 8);",
                "v1 = D[0] * D[0];",
                "s->fp->fpscr |= ros_vfp_ex2(v1, D[0], D[0]);",
                "D[2] = v1;",
                "v2 = D[1] * D[1];",
                "s->fp->fpscr |= ros_vfp_ex2(v2, D[1], D[1]);",
                "v3 = D[2] + v2;",
                "s->fp->fpscr |= ros_vfp_ex2(v3, D[2], v2);",
                "D[2] = v3;",
                "s->fp->fpscr |= ros_vfp_sqrt_ex(D[2]);",
                "ros_std(R[1], sqrt(D[2]));",
            ],
            "{c:#?}"
        );
    }

    #[test]
    fn a_compare_becomes_the_branch_condition() {
        // vcmp.f64 d2, #0; vmrs APSR_nzcv, fpscr; blt ...
        let words = [0xEEB5_2B40, 0xEEF1_FA10, 0xBA00_0010];
        let (c, out) = run_with(&words, Live::default(), Live::default(), &|_| None);
        assert_eq!(out.cond.get(&0xFC00_0008).map(String::as_str), Some("!(D[2] >= 0.0)"), "{c:#?}");
        // Nothing is set but the exception a signalling NaN would raise.
        assert_eq!(c, vec!["s->fp->fpscr |= ros_vfp_cmp_ex(D[2], 0.0, 0);", "<!(D[2] >= 0.0)>"], "{c:#?}");
        // With the flags live where the branch goes, they must really be set.
        let (c, _) = run_with(&words, Live { flags: 0xF, ..Live::default() }, Live::default(), &|_| None);
        // The branch still reads what was compared, which is at hand.
        assert_eq!(
            c,
            vec![
                "s->fp->fpscr |= ros_vfp_cmp_ex(D[2], 0.0, 0);",
                "ros_vfp_cmp(s, D[2], 0.0);",
                "ros_vmrs_flags(s);",
                "<!(D[2] >= 0.0)>"
            ],
            "{c:#?}"
        );
    }

    #[test]
    fn a_compare_feeds_a_conditional_move() {
        // CMF f0, #0; MVFLTD f0, #0: clamp at zero, NaN included.
        let words = [fpa("CMF", "f0, #0"), fpa("MVFLTD", "f0, #0")];
        let c = run(&words, fp(&[Reg::F(0)]));
        assert_eq!(c, vec!["if (!(F[0] >= 0.0)) F[0] = 0.0;"], "{c:#?}");
    }

    #[test]
    fn a_swap_is_written_back_through_a_variable() {
        // MVFD f2, f0; MVFD f0, f1; MVFD f1, f2 -- f0 and f1 exchanged.
        let words = [fpa("MVFD", "f2, f0"), fpa("MVFD", "f0, f1"), fpa("MVFD", "f1, f2")];
        let c = run(&words, fp(&[Reg::F(0), Reg::F(1)]));
        assert_eq!(c, vec!["v1 = F[0];", "F[0] = F[1];", "F[1] = v1;"], "{c:#?}");
    }

    // ---- integers ----

    #[test]
    fn a_single_use_folds_and_a_dead_register_is_not_written() {
        let words = [
            0xE591_2004, // LDR r2, [r1, #4]
            0xE082_2102, // ADD r2, r2, r2, LSL #2
            0xE2820001, // ADD r0, r2, #1
        ];
        let c = run(&words, ints(&[0]));
        // r2's value is read twice, so it is written out once and then read
        // from R[2].
        assert_eq!(c, vec!["R[2] = ros_ld32(R[1] + 4);", "R[0] = R[2] + (R[2] << 2) + 1;"], "{c:#?}");
    }

    #[test]
    fn a_loop_counter_is_a_condition_not_flags() {
        // SUBS r3, r3, #1; BNE back. The flags are dead where the branch
        // goes and after it.
        let words = [0xE253_3001, 0x1AFF_FFFD];
        let (c, out) = run_with(&words, ints(&[3]), ints(&[3]), &|_| None);
        assert_eq!(c, vec!["R[3] -= 1;", "<R[3] != 0>"], "{c:#?}");
        assert_eq!(out.cond[&0xFC00_0004], "R[3] != 0");
        // Where the flags are needed after, they are set tier 0's way.
        let all = Live { flags: 0xF, ..ints(&[3]) };
        let (c, _) = run_with(&words, all, all, &|_| None);
        assert_eq!(c, vec!["R[3] = ros_subs(s, R[3], 1);", "<R[3] != 0>"], "{c:#?}");
    }

    #[test]
    fn compares_and_conditional_compares_are_conditions() {
        // CMP r0, #1; CMPNE r0, #2; MOVEQ r1, #5; MOVNE r1, #6
        let words = [0xE350_0001, 0x1350_0002, 0x03A0_1005, 0x13A0_1006];
        let c = run(&words, ints(&[1]));
        assert_eq!(c, vec!["R[1] = (R[0] == 1 || R[0] == 2) ? 5 : 6;"], "{c:#?}");
        // Signed and unsigned: CMP r0, r1; MOVLT r2, #1; MOVHS r3, #1.
        let words = [0xE150_0001, 0xB3A0_2001, 0x23A0_3001];
        let c = run(&words, ints(&[2, 3]));
        assert_eq!(
            c,
            vec!["if ((int32_t)R[0] < (int32_t)R[1]) R[2] = 1;", "if (R[0] >= R[1]) R[3] = 1;"],
            "{c:#?}"
        );
    }

    #[test]
    fn a_set_flag_is_a_boolean() {
        // MOV r0, #0; TST r1, #4; MOVNE r0, #1
        let words = [0xE3A0_0000, 0xE311_0004, 0x13A0_0001];
        let c = run(&words, ints(&[0]));
        assert_eq!(c, vec!["R[0] = (R[1] & 4) != 0;"], "{c:#?}");
    }

    #[test]
    fn a_64_bit_addition_carries() {
        // ADDS r0, r0, r2; ADC r1, r1, r3
        let words = [0xE090_0002, 0xE0A1_1003];
        let c = run(&words, ints(&[0, 1]));
        assert_eq!(c, vec!["R[0] += R[2];", "R[1] += R[3] + (R[0] < R[2]);"], "{c:#?}");
    }

    #[test]
    fn transfers_with_writeback() {
        // STMFD sp!, {r4, lr}; then a call, which needs sp.
        let words = [0xE92D_4010, 0xEB00_0000];
        let c = run(&words, Live::default());
        assert_eq!(c, vec!["R[13] -= 8;", "ros_st32(R[13], R[4]);", "ros_st32(R[13] + 4, R[14]);"], "{c:#?}");
        // A push and a pop with nothing between that needs sp: it never moves.
        // STMFD sp!, {r4, lr}; LDR r0, [r1], #4; LDMFD sp!, {r4, r5}
        let words = [0xE92D_4010, 0xE491_0004, 0xE8BD_0030];
        let c = run(&words, ints(&[0, 1, 4, 5, 13]));
        assert_eq!(
            c,
            vec![
                "ros_st32(R[13] - 8, R[4]);",
                "ros_st32(R[13] - 4, R[14]);",
                "R[0] = ros_ld32(R[1]);",
                "R[1] += 4;",
                "R[4] = ros_ld32(R[13] - 8);",
                "R[5] = ros_ld32(R[13] - 4);",
            ],
            "{c:#?}"
        );
    }

    #[test]
    fn the_image_is_constants_and_names() {
        // LDR r0, [pc, #248] (&FC000100, a relocated address); ADR r1, T_Table
        let words = [0xE59F_00F8, 0xE28F_10F4];
        let rom = |a: u32| (a == 0xFC00_0100).then_some((0xFC00_0100, true));
        let (c, _) = run_with(&words, Live::ALL, ints(&[0, 1]), &rom);
        assert_eq!(c, vec!["R[0] = T_Table;", "R[1] = T_Table;"], "{c:#?}");
    }

    #[test]
    fn a_conditional_load_keeps_its_old_value() {
        // LDRNE r0, [r1]  with Z from the state.
        let words = [0x1591_0000];
        let c = run(&words, ints(&[0]));
        assert_eq!(c, vec!["if (!s->z) R[0] = ros_ld32(R[1]);"], "{c:#?}");
    }

    #[test]
    fn a_conditional_load_stays_under_its_condition() {
        // CMP r0, r4; LDRNE r1, [r2]; ADDNE r3, r1, r1. The load must not
        // run where NE does not hold, whoever reads it.
        let words = [0xE150_0004, 0x1592_1000, 0x1081_3001];
        let c = run(&words, ints(&[1, 3]));
        assert_eq!(
            c,
            vec!["if (R[0] != R[4]) R[1] = ros_ld32(R[2]);", "if (R[0] != R[4]) R[3] = R[1] + R[1];"],
            "{c:#?}"
        );
        // A value compared with itself: the flags are known, and a load
        // under NE never happens.
        let words = [0xE150_0000, 0x1592_1000, 0x1081_3001];
        assert!(run(&words, ints(&[1, 3])).is_empty());
    }

    #[test]
    fn registers_in_locals() {
        // SUBS r3, r3, #1; BNE back, with r3 a local and r4 not.
        let words = [0xE253_3001, 0xE584_3000, 0x1AFF_FFFC];
        let (c, _) = run_locals(&words, ints(&[3]), ints(&[3]), &|_| None, 1 << 3);
        assert_eq!(c, vec!["r3 -= 1;", "ros_st32(R[4], r3);", "<r3 != 0>"], "{c:#?}");
    }

    #[test]
    fn shifts_in_pairs_and_bits_in_flags_read_as_c() {
        // MOV r1, r0, LSL #24; MOV r1, r1, ASR #24   -- a signed byte
        // MOVS r2, r3, LSR #8; ADDEQ r4, r4, #1      -- r3 below 256, r2 dead
        // ANDS r5, r5, r6, LSL #4; ADDCS r7, r7, #&100 -- bit 28 of r6
        let words = [0xE1A0_1C00, 0xE1A0_1C41, 0xE1B0_2423, 0x0284_4001, 0xE015_5206, 0x2287_7C01];
        let c = run(&words, ints(&[1, 4, 5, 7]));
        assert_eq!(
            c,
            vec![
                "R[1] = (uint32_t)(int8_t)R[0];",
                "if (R[3] < 0x100u) R[4] += 1;",
                "R[5] &= R[6] << 4;",
                "if ((R[6] & 0x10000000u) != 0) R[7] += 0x100u;",
            ],
            "{c:#?}"
        );
    }

    #[test]
    fn a_store_orders_the_loads_before_it() {
        // LDR r2, [r0]; STR r3, [r0]; ADD r1, r1, r2  -- the load must come first.
        let words = [0xE590_2000, 0xE580_3000, 0xE081_1002];
        let c = run(&words, ints(&[1]));
        assert_eq!(c, vec!["R[2] = ros_ld32(R[0]);", "ros_st32(R[0], R[3]);", "R[1] += R[2];"], "{c:#?}");
    }
}
