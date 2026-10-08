//! `rosasm --emit c`: the ObjAsm compiler.
//!
//! The input is what rosasm assembled: each area's bytes and relocations,
//! where each area switches between code and data, where every label
//! landed, and the source line behind each instruction. The output is one C
//! file and a header. The C file holds the unit's ROM image, placed at a
//! fixed address, and its code compiled to C over ROSGD's state block
//! (A7232ToolChain `rosgd/include/rosgd/cpu.h`). The header gives the
//! address of every label.
//!
//! Tier 0 is exact and does nothing more. Every instruction keeps its A32
//! meaning, as a statement over the state block, and the flags are computed
//! wherever an instruction sets them. Tier 1 (lift.rs) compiles the same
//! instructions to the expressions they compute, a block at a time. It uses
//! the flow analysis here, which says which registers and flags are live
//! where. Once lifted, each routine's registers are C locals, and its
//! branches are arranged into ifs, loops and switches (structure.rs).
//! `--no-lift` compiles integer instructions the tier 0 way, with labels
//! and gotos. It is the reference to test tier 1 against.
//!
//! Code is compiled in *regions*. A region is an entry and everything
//! reachable from it without a call. An entry is:
//!
//! - an address that something takes: a module header offset, a relocated
//!   word, an `ADR` to code, or an export;
//! - or the target of a `BL`.
//!
//! A branch into another entry is a tail call, made with `ROS_TAIL_CALL`.
//! The tail call is guaranteed, so a chain of regions (such as a statement
//! loop) cannot grow the C stack. Every call's return is checked against
//! the address the call expected. Anything this cannot compile exactly is
//! an error. It never guesses.
//!
//! A label reached from more than one region is a shared join point.
//! `share` makes each one its own region, compiled once. Without that, a
//! routine whose every entry branches into the shared tails would copy the
//! whole core once per entry. BASIC is the case: every handler branches
//! into the error path and the main loop.

use std::collections::{BTreeMap, BTreeSet, HashMap};

use crate::a32::{self, Cond, DpOp, Insn, MsrSrc, MulOp, Offset, Operand2, Shift, ShiftType, Width};
use crate::aof;
use crate::lift::{self, Live};
use crate::structure::{self, Line};

/// Where an instruction came from.
#[derive(Debug, Clone)]
pub struct Source {
    pub file: String,
    pub line: usize,
    /// The line as listed, after macro expansion.
    pub text: String,
    /// The whole-line comments before it (`notes_before`), for the first
    /// word it makes.
    pub notes: Vec<String>,
}

/// A listed line, as whole-line comments are gathered from it.
pub struct Listed<'a> {
    pub text: &'a str,
    pub file: &'a str,
    /// Straight from a file, not from inside a macro's expansion.
    pub top: bool,
    /// It made an instruction, or data.
    pub code: bool,
    pub data: bool,
}

/// The whole-line comments before each line that makes an instruction.
/// They are the author's explanation, and go before the instruction's C.
/// A block of comments belongs to the next instruction in the same file,
/// if nothing comes between them but labels and directives that make and
/// define nothing. Data, a definition (`EQU`, `^`, `#`, a macro), or a
/// change of file ends the block unclaimed, because the block explained
/// that instead. Comments inside a macro's expansion belong to the macro
/// and are left out. A rule of dashes or stars, with no words, separates
/// paragraphs.
pub fn notes_before(lines: &[Listed]) -> HashMap<usize, Vec<String>> {
    let mut notes = HashMap::new();
    let mut pending: Vec<String> = vec![];
    let mut file = "";
    let mut end = |pending: &mut Vec<String>, at: Option<usize>| {
        while pending.last().is_some_and(|l| l.is_empty()) {
            pending.pop();
        }
        if let (Some(i), false) = (at, pending.is_empty()) {
            // The block's own indentation goes; what is inside it stays.
            let cut = pending.iter().filter(|l| !l.is_empty()).map(|l| l.len() - l.trim_start().len()).min().unwrap_or(0);
            notes.insert(i, pending.iter().map(|l| l.get(cut..).unwrap_or("").to_string()).collect());
        }
        pending.clear();
    };
    for (i, l) in lines.iter().enumerate() {
        if l.code {
            end(&mut pending, Some(i));
            continue;
        }
        if l.data {
            end(&mut pending, None);
            continue;
        }
        if !l.top {
            continue;
        }
        if l.file != file {
            end(&mut pending, None);
            file = l.file;
        }
        let text = l.text.trim();
        if text.is_empty() {
            if pending.last().is_some_and(|l| !l.is_empty()) {
                pending.push(String::new());
            }
        } else if text.starts_with(';') {
            match note_text(l.text) {
                Some(n) => pending.push(n),
                None if pending.last().is_some_and(|l| !l.is_empty()) => pending.push(String::new()),
                None => {}
            }
        } else if defines(l.text) {
            end(&mut pending, None);
        }
    }
    notes
}

/// A whole-line comment's words, with tabs expanded and the semicolons
/// removed. None for a rule with no words in it.
fn note_text(line: &str) -> Option<String> {
    let mut s = String::new();
    for c in line.chars() {
        if c == '\t' {
            s.push(' ');
            while !s.chars().count().is_multiple_of(8) {
                s.push(' ');
            }
        } else {
            s.push(c);
        }
    }
    let t = s.trim_start().trim_start_matches(';').trim_end();
    t.chars().any(|c| c.is_alphanumeric()).then(|| t.to_string())
}

/// Whether a statement defines something, or makes something that is not
/// code. A comment before such a statement explains it.
fn defines(text: &str) -> bool {
    let mut words = text.split_whitespace();
    let op = if text.starts_with(char::is_whitespace) { words.next() } else { words.nth(1) };
    let Some(op) = op.filter(|o| !o.starts_with(';')) else { return false };
    matches!(
        op.to_ascii_uppercase().as_str(),
        "EQU" | "*" | "#" | "^" | "FIELD" | "MAP" | "%" | "SPACE" | "FILL" | "SETA" | "SETL" | "SETS"
            | "GBLA" | "GBLL" | "GBLS" | "LCLA" | "LCLL" | "LCLS" | "MACRO" | "MEND" | "AREA" | "GET"
            | "INCLUDE" | "END" | "RN" | "FN" | "CN" | "CP" | "DN" | "SN" | "QN" | "LTORG" | "WHILE"
            | "WEND"
    )
}

/// The immediates that an instruction's source line names. These are
/// `#Name` and `=Name`, each a whole operand of one identifier. Also a load
/// or store's address when it is a storage-map field, as in
/// `LDR r2, BufferBlockAt`.
fn named_immediates(text: &str) -> Vec<&str> {
    let code = text.split(';').next().unwrap_or("");
    let b = code.as_bytes();
    let mut out = vec![];
    let ident = |e: &str| {
        e.starts_with(|ch: char| ch.is_ascii_alphabetic() || ch == '_')
            && e.chars().all(|ch| ch.is_ascii_alphanumeric() || ch == '_')
    };
    let mut words = code.split_whitespace();
    let op = if code.starts_with(char::is_whitespace) { words.next() } else { words.nth(1) };
    if op.is_some_and(|o| ["LDR", "STR"].iter().any(|m| o.to_ascii_uppercase().starts_with(m))) {
        let start = code.find(op.unwrap()).unwrap_or(0) + op.unwrap().len();
        if let Some((_, addr)) = code[start..].split_once(',') {
            if ident(addr.trim()) {
                out.push(addr.trim());
            }
        }
    }
    for (i, &c) in b.iter().enumerate() {
        if c != b'#' && c != b'=' {
            continue;
        }
        let rest = &code[i + 1..];
        let end = rest.find([',', ']', '!', '}']).unwrap_or(rest.len());
        let e = rest[..end].trim();
        if ident(e) {
            out.push(e);
        }
    }
    out
}

/// Whether a constant's name can be a macro in the C. It can if the name
/// means nothing already to C, its headers or the compiled code.
fn nameable(name: &str, unit: &str) -> bool {
    const MATH: &[&str] = &[
        "sin", "cos", "tan", "asin", "acos", "atan", "atan2", "sinh", "cosh", "tanh", "exp", "exp2", "expm1",
        "log", "log2", "log10", "log1p", "pow", "sqrt", "cbrt", "fabs", "floor", "ceil", "round", "trunc", "fmod",
        "remainder", "rint", "nearbyint", "lrint", "llrint", "lround", "llround", "copysign", "nextafter", "ldexp",
        "frexp", "modf", "scalbn", "ilogb", "fma", "fmin", "fmax", "fdim", "hypot", "erf", "erfc", "tgamma",
        "lgamma", "isnan", "isinf", "isfinite", "isnormal", "signbit", "fpclassify", "isunordered", "NAN",
        "INFINITY", "HUGE_VAL", "NULL", "errno", "mode", "irq_off", "memcpy", "memset", "fpsr", "fpscr", "vfp",
    ];
    let base = name.strip_suffix('f').or_else(|| name.strip_suffix('l')).unwrap_or(name);
    let temp = name.len() > 1 && name.starts_with('v') && name[1..].bytes().all(|b| b.is_ascii_digit());
    c_safe(name)
        && name.len() > 2
        && !temp
        && !MATH.contains(&name)
        && !MATH.contains(&base)
        && !name.starts_with("__")
        && !name.ends_with("_t")
        && !name.to_ascii_lowercase().starts_with("ros_")
        && !name.to_ascii_lowercase().starts_with("rom_")
        && !name.starts_with("INT")
        && !name.starts_with("UINT")
        && !name.to_ascii_lowercase().starts_with(&format!("{}_", unit.to_ascii_lowercase()))
}

/// Removes the assignments to any frame local that is written and never
/// read. If the right side makes a call, the call stays, for what it does
/// (`ros_subs` sets the flags). A plain value is removed with the
/// assignment.
fn drop_dead_slots(body: &str) -> String {
    let code_of = |l: &str| l.split("/*").next().unwrap_or("").to_string();
    let mut code = String::new();
    for l in body.lines() {
        code.push_str(&code_of(l));
        code.push('\n');
    }
    let names: BTreeSet<&str> = code
        .split(|c: char| !(c.is_ascii_alphanumeric() || c == '_'))
        .filter(|w| w.strip_prefix("sp_").is_some_and(|k| !k.is_empty() && k.bytes().all(|b| b.is_ascii_digit())))
        .collect();
    let mut dead: Vec<String> = vec![];
    for n in names {
        let mut reads = 0;
        let mut rest = code.as_str();
        while let Some(i) = rest.find(n) {
            let before = rest[..i].chars().last();
            let after = &rest[i + n.len()..];
            let whole = !before.is_some_and(|c| c.is_ascii_alphanumeric() || c == '_')
                && !after.starts_with(|c: char| c.is_ascii_alphanumeric() || c == '_');
            if whole && !(after.starts_with(" = ") && !after.starts_with(" == ")) {
                reads += 1;
            }
            rest = &rest[i + n.len()..];
        }
        if reads == 0 {
            dead.push(n.to_string());
        }
    }
    if dead.is_empty() {
        return body.to_string();
    }
    let calls = |x: &str| {
        let b = x.as_bytes();
        (1..b.len()).any(|i| b[i] == b'(' && (b[i - 1].is_ascii_alphanumeric() || b[i - 1] == b'_'))
    };
    let mut out = String::with_capacity(body.len());
    for l in body.lines() {
        let (c, comment) = match l.find("/*") {
            Some(k) => (&l[..k], &l[k..]),
            None => (l, ""),
        };
        let indent = c.len() - c.trim_start().len();
        let s = c.trim();
        // `if (cond) sp_N = x;` or `sp_N = x;`
        let (guard, stmt) = match s.strip_prefix("if (") {
            Some(_) if s.ends_with(';') => match s.rfind(") sp_") {
                Some(k) => (Some(&s[..k + 1]), &s[k + 2..]),
                None => (None, s),
            },
            _ => (None, s),
        };
        let target = dead.iter().find(|n| stmt.starts_with(&format!("{n} = ")));
        match target {
            Some(n) => {
                let x = stmt[n.len() + 3..].trim_end_matches(';');
                let kept = if calls(x) {
                    match guard {
                        Some(g) => format!("{g} {x};"),
                        None => format!("{x};"),
                    }
                } else {
                    String::new()
                };
                let pad = &c[..indent];
                if comment.is_empty() {
                    if !kept.is_empty() {
                        out.push_str(&format!("{pad}{kept}\n"));
                    }
                } else if kept.is_empty() {
                    out.push_str(&format!("{}{comment}\n", " ".repeat(48)));
                } else {
                    let line = format!("{pad}{kept}");
                    let w = if line.len() >= 48 { line.len() + 1 } else { 48 };
                    out.push_str(&format!("{line:<w$}{comment}\n"));
                }
            }
            None => {
                out.push_str(l);
                out.push('\n');
            }
        }
    }
    out
}

/// The blocks that control can reach from `start`, renumbered. Some may be
/// unreachable because a branch whose condition is constant never takes
/// the other way.
fn reachable(blocks: Vec<structure::Block>, start: usize) -> (Vec<structure::Block>, usize) {
    let succ = |b: &structure::Block| -> Vec<usize> {
        match &b.term {
            structure::Term::Exit => vec![],
            structure::Term::Goto(t) => vec![*t],
            structure::Term::If { then, else_, .. } => vec![*then, *else_],
            structure::Term::Switch { cases, .. } => cases.clone(),
        }
    };
    let mut seen = vec![false; blocks.len()];
    let mut todo = vec![start];
    while let Some(b) = todo.pop() {
        if !std::mem::replace(&mut seen[b], true) {
            todo.extend(succ(&blocks[b]));
        }
    }
    if seen.iter().all(|&s| s) {
        return (blocks, start);
    }
    let mut map = vec![usize::MAX; blocks.len()];
    let mut n = 0;
    for (i, &s) in seen.iter().enumerate() {
        if s {
            map[i] = n;
            n += 1;
        }
    }
    let kept = blocks
        .into_iter()
        .enumerate()
        .filter(|(i, _)| seen[*i])
        .map(|(_, mut b)| {
            b.term = match b.term {
                structure::Term::Goto(t) => structure::Term::Goto(map[t]),
                structure::Term::If { cond, inv, then, else_, comment } => {
                    structure::Term::If { cond, inv, then: map[then], else_: map[else_], comment }
                }
                structure::Term::Switch { on, first, cases, default, comment } => {
                    structure::Term::Switch { on, first, cases: cases.iter().map(|&c| map[c]).collect(), default, comment }
                }
                t => t,
            };
            b
        })
        .collect();
    (kept, map[start])
}

/// Notes as C: `/* ... */`, continued with ` * `.
fn note_lines(notes: &[String]) -> Vec<String> {
    let n = notes.len();
    notes
        .iter()
        .enumerate()
        .map(|(i, t)| {
            let t = t.replace("*/", "* /").replace("/*", "/ *").replace("??", "? ?");
            let body = match (i, t.is_empty()) {
                (0, _) => format!("/* {t}"),
                (_, true) => " *".to_string(),
                _ => format!(" * {t}"),
            };
            if i + 1 == n { format!("{body} */") } else { body }
        })
        .collect()
}

pub struct Input<'a> {
    /// The unit's name, which prefixes every C name: `t0demo`.
    pub name: String,
    /// Where area 0 is placed; later areas follow it.
    pub base: u32,
    pub areas: &'a [aof::Area],
    /// Where each area turns to code (`a`) or data (`d`): (area, offset, kind).
    pub mapping: &'a [(usize, u32, char)],
    /// Every label: name -> (area, offset).
    pub labels: &'a HashMap<String, (usize, u32)>,
    pub exports: &'a [String],
    /// The source line behind each instruction word, by (area, offset).
    pub sources: HashMap<(usize, u32), Source>,
    /// Kernel SWIs the runtime implements natively, bound statically to
    /// their thunks: number (X bit clear) -> name.
    pub native_swis: HashMap<u32, String>,
    /// The registers each SWI reads and writes, where the typed API
    /// defines it: number (X bit clear) -> (in, out). Others may read and
    /// write anything.
    pub swi_regs: HashMap<u32, (u16, u16)>,
    /// Whether area 0 begins with a RISC OS module header, whose offsets
    /// name the module's entry points.
    pub module: bool,
    /// Whether the unit follows APCS. Compiler output does; hand-written
    /// ObjAsm need not. Under APCS, FPA registers f1-f3 are dead at a
    /// return, and a call reads f0-f3 and clobbers them, keeping f4-f7.
    pub apcs: bool,
    /// Whether integer instructions are lifted to expressions (tier 1)
    /// rather than compiled one by one (tier 0). Floating point is always
    /// lifted, because tier 0 has no other way to compile it.
    pub lift: bool,
    /// Whether every loop is a safe point for the runtime's background work
    /// (`--poll-loops`). If so, ROS_POLL goes on each jump backwards and at
    /// the head of each lifted loop. Then a loop that never calls the OS
    /// can still take an interrupt. An interpreter needs this: BASIC's
    /// Escape must stop a program's loop. It is off by default. Code that
    /// calls the OS as it goes is better without it, and so is code that
    /// keeps interrupts off with the processor's I bit, as the Wimp may.
    pub poll_loops: bool,
    /// The header's file name, which the C includes: `rom_t0demo.h`.
    pub header: String,
    /// The source's numeric constants (`EQU`, `*`, storage-map fields).
    /// Lifted code writes an immediate by the name the source gave it.
    pub constants: HashMap<String, u32>,
    /// The source's storage maps. Those the code uses as record layouts
    /// become C structs.
    pub maps: Vec<Map>,
}

/// A storage map: `^ start«, base»` and its fields (name, offset, size).
pub struct Map {
    pub file: String,
    pub line: usize,
    pub start: u32,
    pub base: Option<u32>,
    pub fields: Vec<(String, u32, u32)>,
}

/// A storage map as a C struct.
struct Record {
    tag: String,
    map: usize,
}

pub struct Output {
    pub c: String,
    pub h: String,
    pub regions: usize,
    pub instructions: usize,
}

const X_BIT: u32 = 0x2_0000;
/// OS_SetVarVal, and its R4 for a code variable (VarType_Code).
const OS_SET_VAR_VAL: u32 = 0x24;
const VAR_TYPE_CODE: u32 = 16;
/// r0-r14, and lr, as sets of integer registers.
const ALL_INT: u16 = 0x7FFF;
const LR: u16 = 1 << 14;

/// What a Table flow's cases are made of, when they are not the code
/// beside the jump.
#[derive(Debug, Clone, Copy, PartialEq)]
enum TableData {
    /// A table of word offsets at `at`. Case j steps by `1 << step`.
    Offsets { at: u32, step: u32 },
    /// A look-up table. The index is one of its words, shifted, masked and
    /// checked against a bound. Each case's target is `base + 4 * index`.
    Lut { table: u32, mask: u32, min: u32, base: u32, shr: u32 },
    /// The loaded word is both the case and the offset, as in BASIC's
    /// two-character token tables: `LDR rt,[base, rt,LSL #2]` then
    /// `ADD pc,pc,rt`. Each case's value is the word. Its target is the
    /// jump's own pc plus the word, or, for a row that is code, that row's
    /// branch target.
    Words { at: u32 },
    /// The jump goes through lr, as in the Screen Blanker's SWI dispatch:
    /// `ADD lr, pc, rm, LSL #k` at `at`, then `MOV pc, lr`. The cases are
    /// the code beside the ADD, measured from its pc, just as an
    /// `ADD pc, pc, rm, LSL #k` there would reach them.
    Beside { at: u32 },
    /// A table counted downwards: `ADR rn, at` then `SUB pc, rn, rm, LSL
    /// #k`. The Wimp uses this for its window furniture, indexed by the
    /// negative icon number. Each case's target is `at - (rm << k)`, a row
    /// of the run after the SUB.
    Down { at: u32 },
}

/// What an instruction does to control flow.
#[derive(Debug, Clone, PartialEq)]
enum Flow {
    /// On to the next instruction.
    Next,
    /// B, or a write of a constant to pc.
    Jump(u32),
    /// BL: a call to an entry, returning to the next instruction.
    Call(u32),
    /// `ADD pc, pc, rm, LSL #shift`, guarded by a bound check. There are
    /// `count` cases, and the first is where rm is `first` (-1 reaches the
    /// instruction after the ADD). `data` names a table of word offsets
    /// when the cases come from one. For example, BASIC's dispatch reads
    /// `LDR rt,[pc,rm,LSL #2]` then jumps with `ADD pc,pc,rt`. Each case
    /// is then the word's offset added to the `ADD`'s own `pc`, not code
    /// that the instruction sits beside.
    Table { rm: u8, shift: u32, first: i32, count: u32, data: Option<TableData> },
    /// MOV pc, lr; BX lr; LDM or LDR of pc from the stack.
    Return,
    /// A transfer to an address known only at run time. With `cont`, lr
    /// was set just before, so it is a call that returns to `cont`.
    Indirect { cont: Option<u32> },
    Unknown(String),
    /// Something ROSGD does not model, but which a guard stops from
    /// running. An example is a 26-bit mode's return after `TEQ pc, pc`.
    /// It compiles to a fault, in case it does run.
    Fault(String),
}

struct Word {
    d: a32::Decoded,
    src: Option<Source>,
    /// The first word its source line produced, which carries the comment.
    first: bool,
}

/// A routine's frame as C locals (`slot_plan`). It holds sp at each
/// instruction, and the frame words live after and before each
/// instruction, one bit per word.
type SlotPlan = (HashMap<u32, i32>, HashMap<u32, u128>, HashMap<u32, u128>);

/// One instruction's liveness facts: what it uses, what it defines, and
/// its successors. A successor inside the region is given by its address.
/// One outside it is given by what is live there.
type LiveFacts = (Live, Live, Vec<Result<u32, Live>>);

struct Unit<'a> {
    inp: &'a Input<'a>,
    image: Vec<u8>,
    code: BTreeMap<u32, Word>,
    /// Chosen label per address, C-safe or not.
    names: BTreeMap<u32, String>,
    entries: BTreeMap<u32, String>,
    errors: Vec<String>,
    /// Lifted conditions. This is the C condition of each conditional
    /// instruction compiled here, as the lifter (lift.rs) gave it. It is
    /// kept per region.
    cond_override: HashMap<u32, String>,
    /// Words of the image that a relocation made into addresses.
    relocated: std::collections::HashSet<u32>,
    /// Entries reached other than by a BL in this unit, so their callers
    /// are unknown. These are module entry points, exports and addresses
    /// taken.
    external: BTreeSet<u32>,
    /// Routines that load sp from outside their frame, as the Wimp does
    /// with `LDR R13,longjumpSP`. Such a routine may return to a frame
    /// other than its caller's. So everything is live where it returns,
    /// and it gets no signature.
    sp_escape: BTreeSet<u32>,
    /// Routines with C signatures (tier 1), as `(ins, outs)`. `ins` are
    /// the registers each takes by value. `outs` are those it gives back
    /// through pointers.
    sig: HashMap<u32, (u16, u16)>,
    /// The signature of the routine being compiled, if it has one.
    region_sig: Option<(u16, u16)>,
    /// For each routine, what it gives back as it found it (tier 1).
    frames: HashMap<u32, Frames>,
    /// Where sp is, relative to its entry value, at each instruction of
    /// each routine whose frame was followed.
    sp_at: HashMap<u32, HashMap<u32, i32>>,
    /// Routines that touch memory at or above their entry sp, which is a
    /// caller's frame. They may do it themselves or through a callee.
    peeks: BTreeSet<u32>,
    /// The routine being compiled, when its frame is private. It holds sp
    /// at each instruction, and the frame words live after each.
    region_slots: Option<SlotPlan>,
    /// Storage maps as structs, and each member's struct and size.
    records: Vec<Record>,
    members: HashMap<String, (String, u32)>,
    /// For each entry, what the flags do across a call to it:
    ///
    /// - `ret_flags`: those live where it returns. For a routine only
    ///   called by BL in this unit, this is what its callers read after
    ///   the call.
    /// - `entry_flags`: those it reads before setting them.
    /// - `must_flags`: those it sets on every path to a return.
    ret_flags: HashMap<u32, u8>,
    entry_flags: HashMap<u32, u8>,
    must_flags: HashMap<u32, u8>,
    /// The same for the integer registers: those a routine reads before it
    /// writes them, those it may write, and those live where it returns.
    ref_regs: HashMap<u32, u16>,
    mod_regs: HashMap<u32, u16>,
    ret_regs: HashMap<u32, u16>,
    /// The registers a routine or its callees name at all. These are the
    /// ones it can read from the state block. Others pass through it
    /// untouched.
    touch_regs: HashMap<u32, u16>,
    /// For the region being compiled: `locals` are the registers held in C
    /// locals. `dirty` gives, before each instruction, the registers whose
    /// local may hold a value the state block does not. `region_live_out`
    /// is what is live after each instruction.
    locals: u16,
    dirty: HashMap<u32, u16>,
    /// For a routine with a signature: the registers that its callers keep
    /// in the state block across the call, and that the region may write
    /// there on the way. These are the ones it preserves, sp among them.
    /// Their state-block values are saved at the start (`wasN`), and each
    /// return puts them back.
    restore: u16,
    region_live_out: HashMap<u32, Live>,
    region_entry: u32,
    /// Registers that the code itself reads after each instruction, before
    /// writing them. This is what a local must hold. It leaves out reads by
    /// a call or an exit, which read the state block.
    read_after: HashMap<u32, u16>,
    /// Registers the region's own instructions write. Where a path that
    /// did not write a register meets one that did, its value may be
    /// stored. So each must hold its value wherever it is live: it is
    /// loaded, and read back after calls, whether or not the code reads
    /// it.
    written: u16,
    /// Registers that hold the caller's lr before each instruction, for
    /// the region being compiled. A transfer to one of them is a return.
    returns_via: HashMap<u32, u16>,
    /// Return addresses (the word after a BL) that the code itself can
    /// jump to later. This happens when the code stores an lr away, then
    /// loads it back and transfers to it. BASIC does this: it pushes the
    /// caller's return address, to return to once a FN body has run. Each
    /// BL site named here registers a setjmp resume point, and every
    /// indirect transfer walks that chain first.
    lr_escape: BTreeSet<u32>,
    /// Code that a table's offsets name (`& handler - table`). These become
    /// entries only if discovery does not reach them some other way.
    late: Vec<(u32, String)>,
    /// The rows of call tables, and where they return. A call table is a
    /// jump table whose lr was set just before the dispatch, as in
    /// `ADR LR,repollwimp`, `CMP`, `ADDCC PC,PC,R0,ASL #2`. Each row is a
    /// call that returns there, and so is the fall-through when the index
    /// is out of range. The Pinboard uses these for its Wimp_Poll reasons,
    /// and, after `MOV LR,PC`, for its buffered actions.
    table_lr: HashMap<u32, u32>,
}

/// Which register locals (`r0`..`r14`) a piece of C names. Its comments
/// are skipped, because they quote the source.
fn used_locals(c: &str) -> u16 {
    let mut code = String::with_capacity(c.len());
    let mut rest = c;
    while let Some(k) = rest.find("/*") {
        code.push_str(&rest[..k]);
        rest = rest[k..].find("*/").map_or("", |e| &rest[k + e + 2..]);
    }
    code.push_str(rest);
    let c = code.as_str();
    let b = c.as_bytes();
    let mut m = 0u16;
    for i in 0..b.len() {
        if b[i] != b'r' || (i > 0 && (b[i - 1].is_ascii_alphanumeric() || b[i - 1] == b'_')) {
            continue;
        }
        let mut j = i + 1;
        while j < b.len() && b[j].is_ascii_digit() {
            j += 1;
        }
        if j == i + 1 || (j < b.len() && (b[j].is_ascii_alphanumeric() || b[j] == b'_')) {
            continue;
        }
        if let Ok(n) = c[i + 1..j].parse::<u32>() {
            if n < 15 {
                m |= 1 << n;
            }
        }
    }
    m
}

/// Where a branch goes, before the routine is cut into blocks. It may go
/// to an instruction, out of the routine by a statement that ends it, or
/// into a jump table (a conditional `ADDLO pc, pc, ...`).
enum Tgt {
    At(u32),
    Out(String),
    Table { on: String, first: i32, cases: Vec<u32>, default: String },
}

/// Whether a condition holds for the flags NZCV (bits 3 to 0).
fn cond_holds(c: Cond, f: u32) -> bool {
    let (n, z, cy, v) = (f & 8 != 0, f & 4 != 0, f & 2 != 0, f & 1 != 0);
    match c {
        Cond::Eq => z,
        Cond::Ne => !z,
        Cond::Cs => cy,
        Cond::Cc => !cy,
        Cond::Mi => n,
        Cond::Pl => !n,
        Cond::Vs => v,
        Cond::Vc => !v,
        Cond::Hi => cy && !z,
        Cond::Ls => !cy || z,
        Cond::Ge => n == v,
        Cond::Lt => n != v,
        Cond::Gt => !z && n == v,
        Cond::Le => z || n != v,
        Cond::Al => true,
    }
}

/// A `Flow::Fault` that is the CallBack handler's return to user mode
/// (`Unit::user_return_at`), compiled as `ros_user_return`.
const USER_RETURN: &str = "a return to user mode from a block of seventeen words";

/// What a routine does with its frame (`Unit::frame`).
#[derive(Clone, Copy, PartialEq, Debug)]
struct Frames {
    /// sp comes back as it went.
    balanced: bool,
    /// It writes nothing above its entry sp, where its callers' frames are.
    keeps: bool,
    /// Registers that hold, at every return, what they held at entry.
    preserved: u16,
}

impl Frames {
    const NONE: Frames = Frames { balanced: false, keeps: false, preserved: 0 };
}

/// The frame at a point: sp's offset from the entry, the registers that
/// still hold their entry values, and the stack words (by offset from the
/// entry sp) that hold one of those.
#[derive(Clone, PartialEq, Debug)]
struct Fr {
    sp: i32,
    entry: u16,
    slots: BTreeMap<i32, u8>,
}

impl Fr {
    /// What holds on both of two paths. None if sp differs between them.
    fn merge(&self, o: &Fr) -> Option<Fr> {
        if self.sp != o.sp {
            return None;
        }
        let slots = self.slots.iter().filter(|(k, v)| o.slots.get(k) == Some(v)).map(|(&k, &v)| (k, v)).collect();
        Some(Fr { sp: self.sp, entry: self.entry & o.entry, slots })
    }
}

/// The frame before an instruction. It is one state, or two: one for when
/// a condition held and one for when it did not.
#[derive(Clone, PartialEq, Debug)]
enum St {
    At(Fr),
    Split(Cond, Fr, Fr),
}

impl St {
    fn join(&self, o: &St) -> Option<St> {
        match (self, o) {
            (St::At(a), St::At(b)) => a.merge(b).map(St::At),
            (St::Split(c, a, b), St::Split(d, x, y)) if c == d => Some(St::Split(*c, a.merge(x)?, b.merge(y)?)),
            _ => Some(St::At(self.flat()?.merge(&o.flat()?)?)),
        }
    }

    fn flat(&self) -> Option<Fr> {
        match self {
            St::At(f) => Some(f.clone()),
            St::Split(_, a, b) => a.merge(b),
        }
    }
}

/// How an instruction ends its block, if it does. A branch's arms may
/// start with code moved into them (`merge_conditions`).
enum End {
    Exit,
    Goto(u32),
    If { cond: String, inv: String, then: Tgt, else_: Tgt, comment: String, then_pre: Vec<Line>, else_pre: Vec<Line> },
    Table { on: String, first: i32, cases: Vec<u32>, default: String, comment: String },
}

/// An instruction's C, for structuring (structure.rs).
struct Item {
    addr: u32,
    /// The author's whole-line comments before it.
    lead: Vec<Line>,
    lines: Vec<Line>,
    end: Option<End>,
    /// Neither a transfer nor a call, and it leaves the flags alone. If it
    /// is conditional, it can join the next instruction under the same
    /// condition.
    plain: bool,
}

/// Splits `if (c) s;` or `if (c) { ... }` into the condition and the
/// statements under it. The first statement takes the `if`'s comment
/// if it has none of its own.
fn cond_group(lines: &[Line]) -> Option<(String, Vec<Line>)> {
    let first = lines.first()?;
    let rest = first.code.strip_prefix("if (")?;
    let mut depth = 1;
    let end = rest.char_indices().find_map(|(i, c)| {
        match c {
            '(' => depth += 1,
            ')' => depth -= 1,
            _ => {}
        }
        (depth == 0).then_some(i)
    })?;
    let cond = rest[..end].to_string();
    let tail = rest[end + 1..].trim_start();
    let mut inner = vec![];
    if tail == "{" {
        if lines.len() < 3 || lines.last()?.code != "}" {
            return None;
        }
        for l in &lines[1..lines.len() - 1] {
            inner.push(Line { code: l.code.strip_prefix("    ")?.to_string(), comment: l.comment.clone() });
        }
        if inner[0].comment.is_empty() {
            inner[0].comment = first.comment.clone();
        }
    } else {
        if lines.len() != 1 || tail.is_empty() || tail.starts_with('{') {
            return None;
        }
        inner.push(Line { code: tail.to_string(), comment: first.comment.clone() });
    }
    Some((cond, inner))
}

/// Joins consecutive instructions under the same condition into one `if`.
/// Examples are `MOVEQ r0, r2` then `MOVEQ pc, lr`, or `LDMNEFD` then
/// `BNE`, where the LDM joins the arm of the branch. The lifter writes a
/// condition in terms of the values it tests. So the same text means the
/// same condition, unless the flags changed in between, and a plain
/// instruction does not change them. A backward branch, such as a loop's,
/// keeps its shape.
fn merge_conditions(items: &mut [Item], starts: &BTreeSet<u32>) {
    for k in 1..items.len() {
        // A comment between them keeps them apart, as the author did.
        if starts.contains(&items[k].addr) || items[k - 1].end.is_some() || !items[k - 1].plain || !items[k].lead.is_empty() {
            continue;
        }
        let Some((c, mut inner)) = cond_group(&items[k - 1].lines) else { continue };
        let addr = items[k].addr;
        let empty = items[k].lines.is_empty();
        match &mut items[k].end {
            Some(End::If { cond, inv, then, then_pre, else_pre, .. }) => {
                let forward = !matches!(then, Tgt::At(t) if *t <= addr);
                if !empty {
                    continue;
                } else if *cond == c && forward {
                    inner.append(then_pre);
                    *then_pre = inner;
                } else if *inv == c {
                    inner.append(else_pre);
                    *else_pre = inner;
                } else {
                    continue;
                }
            }
            _ => {
                let Some((c2, rest)) = cond_group(&items[k].lines) else { continue };
                if c2 != c {
                    continue;
                }
                inner.extend(rest);
                let mut lines = vec![Line { code: format!("if ({c}) {{"), comment: String::new() }];
                lines.extend(inner.into_iter().map(|l| Line { code: format!("    {}", l.code), comment: l.comment }));
                lines.push(Line { code: "}".into(), comment: String::new() });
                items[k].lines = lines;
            }
        }
        items[k - 1].lines.clear();
    }
}

/// Whether a statement declares a variable, which C11 does not allow
/// straight after a label.
fn is_decl(s: &str) -> bool {
    ["uint32_t ", "uint64_t ", "int ", "double ", "float "].iter().any(|t| s.starts_with(t))
}

fn c_safe(name: &str) -> bool {
    const KEYWORDS: &[&str] = &[
        "auto", "break", "case", "char", "const", "continue", "default", "do", "double", "else",
        "enum", "extern", "float", "for", "goto", "if", "inline", "int", "long", "register",
        "restrict", "return", "short", "signed", "sizeof", "static", "struct", "switch",
        "typedef", "union", "unsigned", "void", "volatile", "while", "R", "s", "F", "D", "S",
        "DW", "SW", "ea", "t",
    ];
    // r0-r14 are the registers' locals.
    let register = name.len() > 1 && name.starts_with('r') && name[1..].bytes().all(|b| b.is_ascii_digit());
    let mut chars = name.chars();
    matches!(chars.next(), Some(c) if c.is_ascii_alphabetic() || c == '_')
        && chars.all(|c| c.is_ascii_alphanumeric() || c == '_')
        && !KEYWORDS.contains(&name)
        && !register
}

impl<'a> Unit<'a> {
    fn word(&self, addr: u32) -> Option<u32> {
        let off = addr.checked_sub(self.inp.base)? as usize;
        let b = self.image.get(off..off + 4)?;
        Some(u32::from_le_bytes([b[0], b[1], b[2], b[3]]))
    }

    /// Where a module header's offset word at `at` points. The word is
    /// normally an offset from the module's start. But if the source wrote
    /// it as `Label - |Area$$Base|` and it was relocated, it is already the
    /// address.
    fn header_addr(&self, at: u32, v: u32) -> u32 {
        if self.relocated.contains(&at) {
            v
        } else {
            self.inp.base.wrapping_add(v)
        }
    }

    fn is_code(&self, addr: u32) -> bool {
        self.code.contains_key(&addr)
    }

    /// The instruction at `addr`, decoded straight from its word, if it is
    /// code. The decoded code holds a branch to the next instruction as a
    /// no-op. A table reader needs the branch instead, because a row that
    /// branches on to the next word is still a row. The last row of the
    /// Wimp's openwlp3_jumptable is one.
    fn raw(&self, addr: u32) -> Option<a32::Decoded> {
        if !self.is_code(addr) {
            return None;
        }
        self.word(addr).map(a32::decode)
    }

    /// Whether the word at `addr` is an unconditional B without link, as a
    /// table's row is.
    fn raw_b(&self, addr: u32) -> bool {
        self.raw(addr).is_some_and(|d| d.cond == Cond::Al && matches!(d.insn, Insn::Branch { link: false, .. }))
    }

    fn where_(&self, addr: u32) -> String {
        match self.code.get(&addr).and_then(|w| w.src.as_ref()) {
            Some(s) => format!("{}:{} (&{addr:08X})", s.file, s.line),
            None => format!("&{addr:08X}"),
        }
    }

    fn entry(&mut self, addr: u32, why: String) {
        if !self.is_code(addr) {
            self.errors.push(format!("{} is an entry ({why}) but is not code", self.where_(addr)));
            return;
        }
        if !why.starts_with("called from") {
            self.external.insert(addr);
        }
        self.entries.entry(addr).or_insert(why);
    }

    /// The label name for an address, if it has a C-safe one.
    fn label(&self, addr: u32) -> String {
        match self.names.get(&addr) {
            Some(n) if c_safe(n) => n.clone(),
            _ => format!("L_{addr:08X}"),
        }
    }

    fn function(&self, entry: u32) -> String {
        match self.names.get(&entry) {
            Some(n) if c_safe(n) => format!("{}_{n}", self.inp.name),
            _ => format!("{}_{entry:08X}", self.inp.name),
        }
    }

    // ---- control flow -------------------------------------------------

    /// lr, if something before `addr` set it to a known address. Usually
    /// the instruction just before does it, with `MOV lr, pc` or
    /// `ADR lr, label`.
    ///
    /// It may also be an `ADR lr, label` further back, across a chain of
    /// conditional branches and compares that leave lr alone. Each branch
    /// in the chain is then a call that returns to the label. The Wimp's
    /// Wimp_TextOp does this: `ADR R14,%FT90`, then BLO, BEQ, CMP, BLO,
    /// BEQ, BHI to the handlers for its reason codes. There the label is
    /// past the chain. A branch at the label itself (TextOp's
    /// `90 B ExitWimp`) is where the calls come back to, with lr then
    /// holding whatever they left in it.
    ///
    /// Or the label may be before the ADR, at the top of a poll loop. The
    /// Resource Filer does this: `repollwimp` ... Wimp_Poll,
    /// `ADR lr, repollwimp`, CMP, BEQ to each event's handler, then
    /// `B repollwimp`. A branch back to the label is the loop, not a call.
    fn lr_before(&self, addr: u32, cond: Cond) -> Option<u32> {
        if let Some(lr) = self.lr_set_just_before(addr, cond) {
            return Some(lr);
        }
        let (at, lr) = self.lr_chain_at(addr)?;
        let to_label = match self.code.get(&addr).map(|w| w.d.insn) {
            Some(Insn::Branch { offset, .. }) => addr.wrapping_add(8).wrapping_add(offset as u32) == lr,
            _ => false,
        };
        (lr > addr || (lr < at && !to_label)).then_some(lr)
    }

    /// lr from an `ADR lr, label` before `addr`, across conditional
    /// branches and compares that leave it alone, wherever the label is.
    /// Returns the address of the ADR along with lr.
    fn lr_chain_at(&self, addr: u32) -> Option<(u32, u32)> {
        let mut a = addr.wrapping_sub(4);
        for _ in 0..8 {
            let w = self.code.get(&a)?;
            match w.d.insn {
                Insn::Branch { link: false, .. } if w.d.cond != Cond::Al => {}
                Insn::Dp { op: DpOp::Cmp | DpOp::Cmn | DpOp::Tst | DpOp::Teq, .. } => {}
                Insn::Dp { op: DpOp::Add | DpOp::Sub, s: false, rd: 14, rn: 15, op2: Operand2::Imm { .. } }
                    if w.d.cond == Cond::Al =>
                {
                    return self.lr_set_just_before(a.wrapping_add(4), Cond::Al).map(|lr| (a, lr));
                }
                _ => return None,
            }
            a = a.wrapping_sub(4);
        }
        None
    }

    /// lr, if the instruction just before `addr` set it: `MOV lr, pc` or
    /// `ADR lr, label`.
    fn lr_set_just_before(&self, addr: u32, cond: Cond) -> Option<u32> {
        let p = self.code.get(&addr.wrapping_sub(4))?;
        if p.d.cond != Cond::Al && p.d.cond != cond {
            return None;
        }
        let at = addr.wrapping_sub(4).wrapping_add(8);
        match p.d.insn {
            Insn::Dp {
                op: DpOp::Mov,
                s: false,
                rd: 14,
                op2: Operand2::Reg { rm: 15, shift: Shift::Imm(ShiftType::Lsl, 0) },
                ..
            } => Some(at),
            Insn::Dp { op: DpOp::Add, s: false, rd: 14, rn: 15, op2: Operand2::Imm { value, .. } } => {
                Some(at.wrapping_add(value))
            }
            Insn::Dp { op: DpOp::Sub, s: false, rd: 14, rn: 15, op2: Operand2::Imm { value, .. } } => {
                Some(at.wrapping_sub(value))
            }
            _ => None,
        }
    }

    /// Where a call returns. After a BL, it is the next instruction. After
    /// a B that `ADR lr, label` or `MOV lr, pc` set up, it is where lr
    /// points.
    fn call_back(&self, addr: u32) -> u32 {
        match self.code.get(&addr).map(|w| (w.d.insn, w.d.cond)) {
            Some((Insn::Branch { link: false, .. }, cond)) => self
                .table_lr
                .get(&addr)
                .copied()
                .or_else(|| self.lr_before(addr, cond))
                .unwrap_or(addr.wrapping_add(4)),
            _ => addr.wrapping_add(4),
        }
    }

    /// How many entries a jump table on `rm` has, found from the
    /// instruction that bounds it. That is `CMP rm, #n` before an `ADDLO`
    /// or `ADDLS`, or an `AND` of rm with a mask of low bits before an
    /// unconditional ADD.
    fn table_bound(&self, addr: u32, rm: u8, cond: Cond) -> Option<(i32, u32)> {
        let p = self.code.get(&addr.wrapping_sub(4))?;
        // Skip an instruction in between that leaves rm and the flags
        // alone and runs on. The Wimp's SWI dispatch puts a `MOVCC` there.
        let quiet = match p.d.insn {
            Insn::Dp { s, op, .. } => !s && !matches!(op, DpOp::Cmp | DpOp::Cmn | DpOp::Tst | DpOp::Teq),
            Insn::Mem { .. } => true,
            _ => false,
        };
        if quiet
            && matches!(self.flow(addr.wrapping_sub(4)), Flow::Next)
            && self.explicit_int(addr.wrapping_sub(4)).1 >> rm & 1 == 0
            && (p.d.cond == cond || p.d.cond == Cond::Al)
        {
            return self.table_bound(addr.wrapping_sub(4), rm, cond);
        }
        // `CMP rx, #n` then `ADDLO rm, rx, #k`. Then rm runs from k to
        // k + n - 1.
        if let (Insn::Dp { op: DpOp::Add, s: false, rd, rn, op2: Operand2::Imm { value: k, .. } }, true) =
            (p.d.insn, p.d.cond == cond && cond != Cond::Al)
        {
            if rd == rm && rd != rn && k < 0x1000 {
                let n = self.table_bound(addr.wrapping_sub(4), rn, cond)?.1;
                return Some((k as i32, n));
            }
        }
        if p.d.cond != Cond::Al {
            return None;
        }
        match (p.d.insn, cond) {
            (Insn::Dp { op: DpOp::Cmp, rn, op2: Operand2::Imm { value, .. }, .. }, Cond::Cc) if rn == rm => {
                Some((0, value))
            }
            (Insn::Dp { op: DpOp::Cmp, rn, op2: Operand2::Imm { value, .. }, .. }, Cond::Ls) if rn == rm => {
                Some((0, value + 1))
            }
            // A signed bound. An index below 0 reaches the table's default,
            // which is a fault. The Wimp's drag types use this:
            // `CMP r14,#limit` then `ADDLE`.
            (Insn::Dp { op: DpOp::Cmp, rn, op2: Operand2::Imm { value, .. }, .. }, Cond::Lt) if rn == rm => {
                Some((0, value))
            }
            (Insn::Dp { op: DpOp::Cmp, rn, op2: Operand2::Imm { value, .. }, .. }, Cond::Le) if rn == rm => {
                Some((0, value + 1))
            }
            (Insn::Dp { op: DpOp::And, rd, op2: Operand2::Imm { value, .. }, .. }, Cond::Al)
                if rd == rm && (value + 1).is_power_of_two() =>
            {
                Some((0, value + 1))
            }
            _ => None,
        }
    }

    /// BASIC's dispatch: `LDR rt,[base,ix{,LSL #s}]` reads from a table of
    /// word offsets, then `ADD pc,pc,rt` jumps by the offset.
    ///
    /// The table is its own bound, as `branch_run`'s run of transfers is.
    /// Every entry names code, so the table ends where the offsets that
    /// name code end. An index past them faults, where the original would
    /// run whatever followed. The offsets are measured from the `ADD`'s
    /// own `pc`, which reads eight past it. That is why the source puts
    /// `AJ * .+4` under every one of these tables.
    ///
    /// The base is `pc` (`LDR rt,[pc,ix,LSL #2]`), or a register that an
    /// `ADR` just before the load aimed at the table. Either way the table
    /// sits after the `ADD`. The load and the jump may share one
    /// condition, as Factor's `LDRLO`/`ADDLO` do. The condition is then
    /// the bound, and the fall-through is the instruction after.
    fn offset_table(&self, addr: u32, rt: u8, cond: Cond) -> Option<Flow> {
        let p = self.code.get(&addr.wrapping_sub(4))?;
        if p.d.cond != cond {
            return None;
        }
        let Insn::Mem {
            load: true,
            width: Width::Word,
            rt: prt,
            rn: base,
            pre: true,
            wback: false,
            add: true,
            offset: Offset::Reg { rm: ix, shift: Shift::Imm(ShiftType::Lsl, rs) },
        } = p.d.insn
        else {
            return None;
        };
        if rs == 1 || rs == 3 {
            return None;
        }
        if prt != rt {
            return None;
        }
        // Where the table is. Read through `pc`, it is the word after the
        // jump. Read through a register, the `ADR` before the load must
        // have aimed that register there, and nothing else may have
        // touched it. The ADR must be adjacent to the load, so the only
        // instruction between it and the jump is the load.
        let table = addr.wrapping_add(4);
        if base != 15 {
            let adr_at = addr.wrapping_sub(8);
            let adr = self.code.get(&adr_at)?;
            let Insn::Dp {
                op: DpOp::Add | DpOp::Sub,
                s: false,
                rd,
                rn: 15,
                op2: Operand2::Imm { value, .. },
                ..
            } = adr.d.insn
            else {
                return None;
            };
            if rd != base || adr.d.cond != Cond::Al {
                return None;
            }
            let pc = adr_at.wrapping_add(8);
            let aimed = match adr.d.insn {
                Insn::Dp { op: DpOp::Add, .. } => pc.wrapping_add(value),
                _ => pc.wrapping_sub(value),
            };
            if aimed != table {
                return None;
            }
        }
        // The case value that selects word j steps with the load's scale.
        // Word-scaled (`LSL #2`) indices count words; unscaled ones count
        // bytes. A row that is itself code is a branch veneer, and the
        // case goes where it branches. Factor's first row is one, for the
        // out-of-range error.
        // Here the load overwrites the register it indexes with, so the
        // word is both the case and the offset.
        if prt == rt && ix == rt {
            let mut count2 = 0u32;
            loop {
                let row = table.wrapping_add(4 * count2);
                if self.is_code(row) {
                    match self.code[&row].d.insn {
                        Insn::Branch { .. } if self.code[&row].d.cond == Cond::Al => {
                            count2 += 1;
                            continue;
                        }
                        _ => break,
                    }
                }
                let Some(w) = self.word(row) else { break };
                if !self.is_code(addr.wrapping_add(8).wrapping_add(w)) {
                    break;
                }
                count2 += 1;
            }
            return (count2 > 0).then_some(Flow::Table {
                rm: ix,
                shift: rs,
                first: 0,
                count: count2,
                data: Some(TableData::Words { at: table }),
            });
        }
        let step = 2 - rs;
        let mut count = 0u32;
        loop {
            let row = table.wrapping_add(4 * count);
            if self.is_code(row) {
                match self.code[&row].d.insn {
                    Insn::Branch { .. } if self.code[&row].d.cond == Cond::Al => {
                        // A branch veneer in place of the row. It may be a
                        // B, as the Filers write, or one of the BLs of
                        // BASIC's two-character token table.
                        count += 1;
                        continue;
                    }
                    _ => break,
                }
            }
            let Some(w) = self.word(row) else { break };
            let target = addr.wrapping_add(8).wrapping_add(w);
            if !self.is_code(target) {
                break;
            }
            count += 1;
        }
        (count > 0).then_some({
            Flow::Table {
                rm: ix,
                shift: rs,
                first: 0,
                count,
                data: Some(TableData::Offsets { at: table, step }),
            }
        })
    }

    /// EXPR's operator dispatch. It is a computed jump,
    /// `ADD pc,Rn,rm,LSL #2`, whose base Rn an ADR set to the handlers.
    /// Its index is a word of a look-up table, masked, and at or above a
    /// compare's bound. The table is the index's own bound, because every
    /// case is one of its words.
    fn lut_jump(&self, addr: u32, rn: u8, rm: u8, cond: Cond) -> Option<Flow> {
        if cond != Cond::Al {
            return None;
        }
        // The index comes from `BIC rm,rt,#mask`, the instruction before
        // the jump. The base is the jump's own pc, or a register that an
        // ADR before the BIC aimed at the handlers.
        let dbg = std::env::var("ROSGD_LUT_DEBUG").is_ok();
        let bic = self.code.get(&addr.wrapping_sub(4))?;
        let Insn::Dp { op: DpOp::Bic, s: false, rd, rn: src, op2: Operand2::Imm { value: mv, .. }, .. } =
            bic.d.insn
        else {
            if dbg { eprintln!("lut @ &{addr:08X}: no BIC before"); }
            return None;
        };
        if rd != rm || bic.d.cond != Cond::Al {
            if dbg { eprintln!("lut @ &{addr:08X}: BIC mismatch"); }
            return None;
        }
        let mask = !mv;

        let base = if rn == 15 {
            addr.wrapping_add(8)
        } else {
            let adr = self.code.get(&addr.wrapping_sub(8))?;
            let Insn::Dp {
                op: op @ (DpOp::Add | DpOp::Sub),
                s: false,
                rd,
                rn: 15,
                op2: Operand2::Imm { value, .. },
                ..
            } = adr.d.insn
            else {
                return None;
            };
            if rd != rn || adr.d.cond != Cond::Al {
                return None;
            }
            let pc = addr.wrapping_sub(8).wrapping_add(8);
            match op {
                DpOp::Add => pc.wrapping_add(value),
                _ => pc.wrapping_sub(value),
            }
        };
        let mut table = 0;
        let mut min = 0;
        let mut found = false;
        let mut shr = 0u32;      // a MOV rt,rt,LSR #k between load and use
        // The operator handlers dispatch again with the same table word
        // their caller loaded, which is preserved across calls. So the
        // search goes back past calls, and stops only at a write of the
        // register.
        for k in 2..=192u32 {
            let at = addr.wrapping_sub(4 * k);
            let Some(w) = self.code.get(&at) else { break };
            if let crate::emitc::Flow::Call(_) = self.flow(at) {
                continue;
            }
            if std::env::var("ROSGD_LUT_TRACE").is_ok() {
                eprintln!("  scan &{at:08X}: {:?}", w.d.insn);
            }
            match w.d.insn {
                Insn::Dp { op: DpOp::Cmp, rn: c, op2: Operand2::Imm { value, .. }, .. }
                    if c == src && w.d.cond == Cond::Al && value > min =>
                {
                    min = value;
                }
                Insn::Mem {
                    load: true,
                    width: Width::Word,
                    rt: c,
                    rn: b,
                    pre: true,
                    wback: false,
                    add: true,
                    offset: Offset::Reg { rm: _ix, shift: Shift::Imm(ShiftType::Lsl, 2) },
                } if c == src && w.d.cond == Cond::Al => {
                    // The table's own ADR, somewhere above the load.
                    for j in 1..=6u32 {
                        let aa = at.wrapping_sub(4 * j);
                        let Some(g) = self.code.get(&aa) else { break };
                        if let Insn::Dp {
                            op: aop @ (DpOp::Add | DpOp::Sub),
                            s: false,
                            rd: brd,
                            rn: 15,
                            op2: Operand2::Imm { value: v, .. },
                            ..
                        } = g.d.insn
                        {
                            if brd == b && g.d.cond == Cond::Al {
                                let apc = aa.wrapping_add(8);
                                table = match aop {
                                    DpOp::Add => apc.wrapping_add(v),
                                    _ => apc.wrapping_sub(v),
                                };
                                found = true;
                                break;
                            }
                        }
                        // Another write of b, so the ADR is not above it.
                        if matches!(g.d.insn, Insn::Dp { rd, .. } if rd == b) && j > 1 {
                            break;
                        }
                    }
                    break;
                }
                // AJ7's own prologue rebuilds the operator from the table
                // word with `MOV rt,rt,LSR #k`. The index is then the word
                // shifted, and the table still bounds it.
                Insn::Dp {
                    op: DpOp::Mov,
                    s: false,
                    rd,
                    op2: Operand2::Reg { rm, shift: Shift::Imm(ShiftType::Lsr, k) },
                    ..
                } if rd == src && rm == src => {
                    // The shift may be conditional, so the index is either
                    // the word or the word shifted. Both sets of cases are
                    // listed.
                    shr = shr.max(k);
                }
                // Another write of src, so the value is not the table's.
                Insn::Dp { rd, .. } | Insn::Mem { rt: rd, .. } if rd == src => break,
                _ => {}
            }
        }
        if !found {
            if dbg { eprintln!("lut @ &{addr:08X}: no table load found (src r{src})"); }
            return None;
        }

        // The cases are the table's distinct words at or above the bound,
        // each targeting code.
        let data = TableData::Lut { table, mask, min, base, shr };
        let cases = self.table_targets(addr, 2, 0, u32::MAX, Some(data));
        if std::env::var("ROSGD_LUT_DEBUG").is_ok() {
            eprintln!("lut_jump @ &{:08X}: table &{:X} mask &{:X} min &{:X} base &{:X} -> {} cases",
                      addr, table, mask, min, base, cases.len());
        }
        (cases.len() > 1).then_some(Flow::Table { rm, shift: 2, first: 0, count: cases.len() as u32, data: Some(data) })
    }

    /// A jump with no bound into a run of transfers, as the Filers use to
    /// decode a menu selection. It is `ADD pc, pc, r14, LSL #2`, then the
    /// instruction for -1 (no selection), then a `B` or an `EXIT` for each
    /// entry, and then the code after the run for the last entry. Those are
    /// the cases. An index past them faults, where the original would run
    /// whatever follows.
    fn branch_run(&self, addr: u32, shift: u32, cond: Cond) -> Option<(i32, u32)> {
        if shift != 2 || cond != Cond::Al {
            return None;
        }
        // Words that are not code may pad the start of the run, as in the
        // Wimp's border-icon table, whose index is never 0. An index that
        // reaches one faults.
        let mut pad = 0u32;
        while !self.is_code(addr.wrapping_add(4 + 4 * pad)) {
            pad += 1;
            if pad > 2 {
                return None;
            }
        }
        if pad > 0 {
            let start = addr.wrapping_add(4 + 4 * pad);
            let mut n = 0u32;
            while self.raw_b(start.wrapping_add(4 * n)) {
                n += 1;
            }
            return (n > 0).then_some((pad as i32 - 1, n));
        }
        // A `B` to the next word is decoded as a no-op, but it is an entry
        // too. The Pinboard's TinyDirs icon menu puts `B QuitTinyDirs`
        // just before `QuitTinyDirs`.
        let mut n = 0u32;
        while let Some(w) = self.code.get(&addr.wrapping_add(8 + 4 * n)) {
            let jumps = matches!(w.d.insn, Insn::Branch { link: false, .. })
                || w.d.insn == Insn::Nop
                || matches!(w.d.insn, Insn::Block { load: true, regs, .. } if regs & 0x8000 != 0)
                || matches!(w.d.insn, Insn::Mem { load: true, rt: 15, .. })
                || matches!(w.d.insn, Insn::Dp { rd: 15, op: DpOp::Mov, op2: Operand2::Reg { rm: 14, .. }, .. });
            if w.d.cond == Cond::Al && jumps {
                n += 1;
            } else {
                break;
            }
        }
        // The code after the run is the last entry, where there is code.
        let after = u32::from(self.is_code(addr.wrapping_add(8 + 4 * n)));
        (n > 0).then_some((-1, n + 1 + after))
    }

    /// A jump table's cases: each value of rm, and where it goes.
    fn table_cases(a: u32, shift: u32, first: i32, count: u32) -> Vec<(i32, u32)> {
        (0..count as i32)
            .map(|j| {
                let v = first + j;
                (v, a.wrapping_add(8).wrapping_add(((v as i64) << shift) as u32))
            })
            .collect()
    }

    /// The cases of a table at `a`, taken from the code beside it or from
    /// a table of word offsets. The offsets are the ones a pc-relative
    /// `ADD pc, pc, rt` adds. They are measured from the `ADD`'s own `pc`,
    /// which reads eight past it.
    fn table_targets(
        &self,
        a: u32,
        shift: u32,
        first: i32,
        count: u32,
        data: Option<TableData>,
    ) -> Vec<(i32, u32)> {
        match data {
            None => Self::table_cases(a, shift, first, count),
            Some(TableData::Beside { at }) => Self::table_cases(at, shift, first, count),
            Some(TableData::Down { at }) => (0..count as i32)
                .map(|j| {
                    let v = first + j;
                    (v, at.wrapping_sub(((v as i64) << shift) as u32))
                })
                .collect(),
            // Word j of the table is selected by the case value
            // `first + j << step`. A word-scaled load indexes words; an
            // unscaled one indexes bytes.
            Some(TableData::Offsets { at: t, step }) => (0..count as i32)
                .map(|j| {
                    let v = first + (j << step);
                    let row = t.wrapping_add(4 * j as u32);
                    // A row that is code is a branch veneer. The case goes
                    // where it branches.
                    let target = match self.code.get(&row).map(|w| (w.d.insn, w.d.cond)) {
                        Some((Insn::Branch { offset, .. }, Cond::Al)) => {
                            row.wrapping_add(8).wrapping_add(offset as u32)
                        }
                        _ => a.wrapping_add(8).wrapping_add(self.word(row).unwrap_or(0)),
                    };
                    (v, target)
                })
                .collect(),
            Some(TableData::Words { at: t }) => {
                let mut out = Vec::new();
                let mut seen = std::collections::BTreeSet::new();
                for j in 0..count as i32 {
                    let row = t.wrapping_add(4 * j as u32);
                    let w = self.word(row).unwrap_or(0);
                    if !seen.insert(w) {
                        continue;
                    }
                    let target = match self.code.get(&row).map(|x| (x.d.insn, x.d.cond)) {
                        Some((Insn::Branch { offset, .. }, Cond::Al)) => {
                            row.wrapping_add(8).wrapping_add(offset as u32)
                        }
                        _ => a.wrapping_add(8).wrapping_add(w),
                    };
                    out.push((w as i32, target));
                }
                out
            }
            Some(TableData::Lut { table, mask, min, base, shr }) => {
                // The cases are the table's own words. Each word at or
                // above the bound gives a case: its distinct masked value,
                // with target base + 4 * value. If the flow has a shift,
                // the shifted word gives a case too.
                let mut out = Vec::new();
                let mut seen = std::collections::BTreeSet::new();
                let mut p = table;
                while !self.is_code(p) && p.wrapping_sub(table) < 4 * 512 {
                    let Some(w) = self.word(p) else { break };
                    let mut family = vec![w];
                    if shr > 0 {
                        family.push(w >> shr);       // the MOVNE rebuild
                    }
                    for w in family {
                        if w >= min {
                            let v = w & mask;
                            if seen.insert(v) && self.is_code(base.wrapping_add(4 * v)) {
                                out.push((v as i32, base.wrapping_add(4 * v)));
                            }
                        }
                    }
                    p += 4;
                }
                out.truncate(count as usize);
                out
            }
        }
    }

    /// Which registers still hold lr's value at entry, before each
    /// instruction of the region at `entry`. This is a forward analysis:
    /// where two paths meet, a register keeps the value only if it holds it
    /// on both. `MOV rX, lr` copies the value into rX. Any other write to
    /// rX loses it, and so does a call, because a call writes lr. A SWI
    /// that the typed API defines writes only its outputs. The Task
    /// Manager's lookup_erroralt relies on this: it keeps lr in R8 across
    /// OS_Module and MessageTrans, and returns with `MOV pc, R8`.
    fn lr_holders(&self, entry: u32, r: &BTreeSet<u32>) -> HashMap<u32, u16> {
        let mut before: HashMap<u32, u16> = HashMap::new();
        before.insert(entry, 1 << 14);
        let mut work = vec![entry];
        while let Some(a) = work.pop() {
            let mut set = before[&a];
            let d = self.code[&a].d;
            if let Insn::Dp { op: DpOp::Mov, s: false, rd, op2: Operand2::Reg { rm, shift: Shift::Imm(ShiftType::Lsl, 0) }, .. } = d.insn {
                if d.cond == Cond::Al && rd != 15 {
                    set = if set & (1 << rm) != 0 { set | 1 << rd } else { set & !(1 << rd) };
                } else if rd != 15 {
                    set &= !(1 << rd);
                }
            } else {
                set &= !self.swi_io(a).map_or_else(|| Self::int_defs(&d), |(_, o)| o);
                if self.is_call(a) {
                    set &= !(1 << 14);
                }
            }
            for s in self.successors(a).0 {
                if !r.contains(&s) || (s != entry && self.entries.contains_key(&s)) {
                    continue;
                }
                let merged = before.get(&s).map_or(set, |&old| old & set);
                if before.get(&s) != Some(&merged) {
                    before.insert(s, merged);
                    work.push(s);
                }
            }
        }
        before
    }

    /// Whether the region returns with sp exactly where it was entered,
    /// along every path. How far each instruction moves sp is tracked. A
    /// BL to a region that is not itself sp-zero fails the proof. Exits
    /// are returns, stack pops to pc, and branches out to another entry.
    /// A branch to another entry passes the duty on: that entry must be
    /// sp-zero too. Recursion across regions leaves a region not sp-zero.
    fn region_sp_zero(&self, e: u32, r: &BTreeSet<u32>, region_of: &HashMap<u32, u32>,
                      sp_zero: &HashMap<u32, bool>) -> bool {
        let mut before: HashMap<u32, i32> = HashMap::new();
        before.insert(e, 0);
        let mut work = vec![e];
        while let Some(a) = work.pop() {
            let d0 = before[&a];
            let w = match self.code.get(&a) { Some(w) => w, None => continue };
            let d = &w.d;
            let cond_al = d.cond == Cond::Al;
            let delta: Option<i32> = match &d.insn {
                Insn::Dp { op, rd: 13, rn: 13,
                           op2: Operand2::Imm { value, .. }, .. }
                    if cond_al && matches!(op, DpOp::Add | DpOp::Sub) =>
                {
                    Some(match op { DpOp::Sub => -(*value as i32), _ => *value as i32 })
                }
                Insn::Mem { rn: 13, offset: Offset::Imm(k), add, wback, .. } if cond_al => {
                    if *wback {
                        Some(*k as i32 * if *add { 1 } else { -1 })
                    } else {
                        Some(0)
                    }
                }
                Insn::Mem { rn: 13, wback: true, .. } => None,
                Insn::Block { rn: 13, regs, add, wback, user, .. } if cond_al => {
                    let n = regs.count_ones() as i32;
                    if !*wback {
                        Some(0)
                    } else if *user || regs & (1 << 13) != 0 {
                        None
                    } else {
                        Some(4 * n * if *add { 1 } else { -1 })
                    }
                }
                _ if Self::int_defs(d) >> 13 & 1 == 1 => None,
                _ => Some(0),
            };
            if let Flow::Call(t) = self.flow(a) {
                // A call keeps sp only if the callee does. A call to code
                // this unit does not know goes to the runtime, which leaves
                // the guest stack alone.
                if region_of.get(&t).is_some_and(|&re| !sp_zero[&re]) {
                    return false;
                }
            }
            let Some(k) = delta else { return false };
            let d1 = d0 + k;
            // Where control leaves the region, by a return or a transfer
            // the unit cannot name, the frame must be back in place.
            if d1 != 0 && matches!(
                self.flow(a),
                Flow::Return | Flow::Indirect { cont: None } | Flow::Unknown(_) | Flow::Fault(_)
            ) {
                return false;
            }
            for s in self.successors(a).0 {
                if !r.contains(&s) || (s != e && self.entries.contains_key(&s)) {
                    // Out of the region. The frame must be restored here.
                    // If the target is an entry, it takes the duty with
                    // it, and must be sp-zero itself.
                    if d1 != 0 {
                        return false;
                    }
                    if self.entries.contains_key(&s) && !sp_zero[&s] {
                        return false;
                    }
                    continue;
                }
                match before.get(&s) {
                    None => {
                        before.insert(s, d1);
                        work.push(s);
                    }
                    Some(&old) if old == d1 => {}
                    _ => return false,
                }
            }
        }
        true
    }

    /// Which BL return addresses the code can jump to itself, through a
    /// value it derived from lr. The compiler follows the value both in
    /// registers and in the frame. An lr pushed and popped straight back
    /// into pc is an ordinary return, and marks nothing; every routine with
    /// a frame saves lr this way. A return address is marked (as one the
    /// code can jump to) if an lr holding it reaches an indirect transfer,
    /// is stored somewhere the frame cannot see, or is still in the frame
    /// where control leaves the region. BASIC does this: it pushes the
    /// caller's return address when an FN is called. Once the body has
    /// run, it returns there from the interpreter loop, which is in
    /// another region.
    fn find_lr_escapes(&mut self) {
        let roots: Vec<u32> = self.entries.keys().copied().collect();
        let mut region_of: HashMap<u32, u32> = HashMap::new();
        let mut regions: Vec<(u32, BTreeSet<u32>)> = Vec::new();
        for &e in &roots {
            let r = self.region(e);
            for &a in &r {
                region_of.insert(a, e);
            }
            regions.push((e, r));
        }
        // Every BL, listed by the region it calls into. The caller's lr at
        // one of the region's entries is one of these return addresses.
        let mut calls: Vec<(u32, u32)> = Vec::new();
        for &a in &self.code.keys().copied().collect::<Vec<u32>>() {
            if let Flow::Call(t) = self.flow(a) {
                if let Some(&re) = region_of.get(&t) {
                    calls.push((re, self.call_back(a)));
                }
            }
        }
        #[derive(Clone, Copy, PartialEq, Eq, PartialOrd, Ord, Debug)]
        enum Origin {
            /// The caller's lr, at the region's entry.
            Caller,
            /// The return address of a BL made in the region.
            Ret(u32),
        }
        /// The lr-derived values a path may hold, in registers and in the
        /// words of the frame below the entry sp. `sp` is the distance
        /// from the entry sp. It is `None` where the code moved sp somewhere
        /// the analysis cannot follow.
        #[derive(Clone, Default, PartialEq)]
        struct St {
            regs: HashMap<u8, BTreeSet<Origin>>,
            sp: Option<i32>,
            slots: BTreeMap<i32, BTreeSet<Origin>>,
        }
        let start = || {
            let mut st = St { sp: Some(0), ..Default::default() };
            st.regs.insert(14, [Origin::Caller].into_iter().collect());
            st
        };
        // The BL return sites through which control may have entered a
        // region. These are the region's own callers' sites, and those of
        // the regions that branch into it. The caller's lr travels across
        // region boundaries: BASIC's GTARGS branches into
        // GTARGRET..GTARGRETRET while holding it. So a Caller origin
        // stands for every site in this set, not just the region's own
        // callers.
        let mut enter: HashMap<u32, BTreeSet<u32>> =
            roots.iter().map(|&e| (e, BTreeSet::new())).collect();
        for &(re, back) in &calls {
            enter.get_mut(&re).unwrap().insert(back);
        }
        let mut edges: Vec<(u32, u32)> = Vec::new();
        for &(e, ref r) in &regions {
            for &a in r {
                for s in self.successors(a).0 {
                    if !r.contains(&s) && s != e && enter.contains_key(&s) {
                        edges.push((e, s));
                    }
                }
            }
        }
        let mut entered = true;
        while entered {
            entered = false;
            for &(from, to) in &edges {
                let add = enter[&from].clone();
                let to = enter.get_mut(&to).unwrap();
                if !add.is_subset(to) {
                    to.extend(add);
                    entered = true;
                }
            }
        }
        // Whether each region returns with sp exactly where it was
        // entered. A region that does not may pop its caller's arguments,
        // as BASIC's PULLTYPE pops what its caller pushed. After a BL to
        // it, the caller's frame is somewhere the walk below cannot name.
        let mut sp_zero: HashMap<u32, bool> = roots.iter().map(|&e| (e, false)).collect();
        let mut zeroed = true;
        while zeroed {
            zeroed = false;
            for &(e, ref r) in &regions {
                if sp_zero[&e] || !self.region_sp_zero(e, r, &region_of, &sp_zero) {
                    continue;
                }
                sp_zero.insert(e, true);
                zeroed = true;
            }
        }
        fn mark(o: Origin, escape: &mut BTreeSet<u32>, enter: &HashMap<u32, BTreeSet<u32>>, e: u32, site: u32) {
            let dbg = std::env::var("ROSASM_DEBUG_ESCAPE").is_ok();
            match o {
                Origin::Caller => {
                    if let Some(sites) = enter.get(&e) {
                        for &back in sites {
                            if dbg {
                                eprintln!("escape: at &{site:08X} caller-lr of region &{e:08X} marks &{back:08X}");
                            }
                            escape.insert(back);
                        }
                    }
                }
                Origin::Ret(b) => {
                    if dbg {
                        eprintln!("escape: at &{site:08X} ret &{b:08X} (region &{e:08X})");
                    }
                    escape.insert(b);
                }
            }
        }
        let mark_all = |st: &St, escape: &mut BTreeSet<u32>, e: u32| {
            for os in st.slots.values() {
                for &o in os {
                    mark(o, escape, &enter, e, 0);
                }
            }
        };
        let mut escape: BTreeSet<u32> = BTreeSet::new();
        let trace = std::env::var("ROSASM_DEBUG_TRACE").ok().and_then(|v| u32::from_str_radix(v.trim_start_matches("0x"), 16).ok());
        for (e, r) in &regions {
            let mut before: HashMap<u32, St> = HashMap::new();
            before.insert(*e, start());
            let mut work = vec![*e];
            while let Some(a) = work.pop() {
                let mut st = before[&a].clone();
                if trace == Some(*e) {
                    eprintln!("trace &{a:08X}: {:?} sp={:?} slots={:?} r14regs={:?}", self.code[&a].d.insn, st.sp, st.slots, st.regs.get(&14));
                }
                let d = self.code[&a].d;
                let cond_al = d.cond == Cond::Al;
                // Where the instruction takes control from a register.
                let indirect_rm = match (&d.insn, self.flow(a)) {
                    (Insn::Bx { rm, .. }, Flow::Indirect { .. }) => Some(*rm),
                    (Insn::Dp { rd: 15, op2: Operand2::Reg { rm, .. }, .. }, Flow::Indirect { .. }) => Some(*rm),
                    _ => None,
                };
                // A BL to a region that returns with sp moved. It may pop
                // arguments its caller pushed, and then the frame below sp
                // is somewhere the walk cannot name.
                if let Flow::Call(t) = self.flow(a) {
                    if region_of.get(&t).is_some_and(|&re| !sp_zero[&re]) {
                        mark_all(&st, &mut escape, *e);
                        st.sp = None;
                        st.slots.clear();
                    }
                }
                // Track sp. Adding or subtracting a constant keeps track of
                // it; any other write to sp loses it, and the frame with it.
                match &d.insn {
                    Insn::Dp {
                        op: DpOp::Add | DpOp::Sub,
                        rd: 13,
                        rn: 13,
                        op2: Operand2::Imm { value, .. },
                        ..
                    } if cond_al => {
                        let k = *value as i32;
                        if let Some(sp) = st.sp.as_mut() {
                            *sp += if matches!(d.insn, Insn::Dp { op: DpOp::Sub, .. }) { -k } else { k };
                        }
                    }
                    _ if Self::int_defs(&d) >> 13 & 1 == 1
                        && cond_al
                        // Writeback on an sp-based load or store moves sp
                        // by an amount that the frame handling below knows.
                        // A SWI keeps sp.
                        && !matches!(&d.insn, Insn::Block { rn: 13, user: false, .. } | Insn::Mem { rn: 13, .. })
                        && !self.is_call(a) =>
                    {
                        mark_all(&st, &mut escape, *e);
                        st.sp = None;
                        st.slots.clear();
                    }
                    // A push when the walk has lost sp. This sets a fresh
                    // anchor, and the pops that take the same words off
                    // are read relative to it. Whatever was in the frame
                    // was marked when the frame was lost.
                    Insn::Block { load: false, rn: 13, user: false, wback: true, .. }
                        if cond_al && st.sp.is_none() =>
                    {
                        st.slots.clear();
                        st.sp = Some(0);
                    }
                    Insn::Mem { load: false, rn: 13, wback: true, offset: Offset::Imm(_), .. }
                        if cond_al && st.sp.is_none() =>
                    {
                        st.slots.clear();
                        st.sp = Some(0);
                    }
                    _ => {}
                }
                match &d.insn {
                    // After a BL or BLX, lr holds the return address it
                    // made. The callee may have changed every other
                    // register except r4-r11, which APCS preserves, so
                    // only those keep their origins. A conditional call is
                    // treated as made, so lr's old origins are not kept
                    // beside the new one.
                    Insn::Branch { link: true, .. } | Insn::Bx { link: true, .. } => {
                        let o = Origin::Ret(a.wrapping_add(4));
                        // Under APCS the callee must preserve r4-r11, so
                        // the caller's lr-derived values survive the call
                        // in them. BASIC's GTARGS holds its caller's lr in
                        // r10 across a call.
                        st.regs.retain(|&r, _| (4..=11).contains(&r));
                        st.regs.insert(14, [o].into_iter().collect());
                    }
                    // `ADR lr, label` then `B` is a call that returns to
                    // the label.
                    Insn::Branch { link: false, .. } if matches!(self.flow(a), Flow::Call(_)) => {
                        let o = Origin::Ret(self.call_back(a));
                        st.regs.retain(|&r, _| (4..=11).contains(&r));
                        st.regs.insert(14, [o].into_iter().collect());
                    }
                    Insn::Branch { link: false, .. } | Insn::Bx { link: false, rm: 14, .. } => {}
                    // A copy carries the origin with it.
                    Insn::Dp {
                        op: DpOp::Mov,
                        s: false,
                        rd,
                        op2: Operand2::Reg { rm, shift: Shift::Imm(ShiftType::Lsl, 0) },
                        ..
                    } if *rd != 15 && *rd != 13 => {
                        let v = st.regs.get(rm).cloned().unwrap_or_default();
                        if v.is_empty() {
                            if cond_al {
                                st.regs.remove(rd);
                            }
                        } else if cond_al {
                            st.regs.insert(*rd, v);
                        } else {
                            st.regs.entry(*rd).or_default().extend(v);
                        }
                    }
                    // A load into a register of a value the frame cannot
                    // be sure of. Whatever the register held is gone.
                    Insn::Mem { load: true, rt, .. } if *rt != 13 && *rt != 15 => {
                        if cond_al {
                            st.regs.remove(rt);
                        }
                    }
                    Insn::Block { load: true, regs, .. } => {
                        for i in 0..14u8 {
                            if regs >> i & 1 == 1 && cond_al {
                                st.regs.remove(&i);
                            }
                        }
                    }
                    _ => {
                        // A SWI writes lr. It may keep any other register:
                        // a RISC OS SWI preserves what it does not return,
                        // and which registers it returns may depend on its
                        // reason code. So the other registers may still
                        // hold what they held. The Task Manager's
                        // freeworkspace keeps its lr in r6 across
                        // Wimp_CloseDown and OS_Module, and returns
                        // through it.
                        let swi = matches!(d.insn, Insn::Swi { .. });
                        let defs = if swi { 1 << 14 } else { Self::int_defs(&d) };
                        if cond_al {
                            for i in 0..15u8 {
                                if defs >> i & 1 == 1 {
                                    st.regs.remove(&i);
                                }
                            }
                            if self.is_call(a) && !swi {
                                st.regs.retain(|&r, _| (4..=11).contains(&r));
                                st.regs.remove(&14);
                            }
                        }
                    }
                }
                // The frame: stores of lr-derived values, loads of them
                // back, and the pop to pc that uses one up as a return.
                match &d.insn {
                    Insn::Mem { load, width, rt, rn, offset, add, pre, wback } if *rn == 13 && *rt != 13 && *rt != 15 => {
                        let known = st.sp.is_some() && matches!(offset, Offset::Imm(_));
                        if known {
                            let sp = st.sp.unwrap();
                            let Offset::Imm(k) = offset else { unreachable!() };
                            let k = *k as i32 * if *add { 1 } else { -1 };
                            let at = sp + if *pre { k } else { 0 };
                            let word = matches!(width, Width::Word);
                            if *load {
                                if word && at % 4 == 0 {
                                    let v = st.slots.get(&at).cloned().unwrap_or_default();
                                    if v.is_empty() {
                                        st.regs.remove(rt);
                                    } else {
                                        st.regs.insert(*rt, v);
                                    }
                                } else {
                                    st.regs.remove(rt);
                                }
                            } else if word && at % 4 == 0 {
                                let v = st.regs.get(rt).cloned().unwrap_or_default();
                                if at >= 0 {
                                    // A store into the caller's frame. No
                                    // load of ours can be trusted to see
                                    // it.
                                    for o in v {
                                        mark(o, &mut escape, &enter, *e, a);
                                    }
                                } else if v.is_empty() {
                                    st.slots.remove(&at);
                                } else {
                                    st.slots.insert(at, v);
                                }
                            } else if !*load {
                                // A part-word store loses whole words.
                                let mut w = at.div_euclid(4) * 4;
                                while w <= at {
                                    st.slots.remove(&w);
                                    w += 4;
                                }
                            }
                            if *wback || !*pre {
                                if let Some(sp) = st.sp.as_mut() {
                                    *sp += k;
                                }
                            }
                        } else {
                            // An address the analysis cannot name.
                            if *load {
                                if cond_al {
                                    st.regs.remove(rt);
                                }
                            } else if let Some(os) = st.regs.get(rt).cloned() {
                                for o in os {
                                    mark(o, &mut escape, &enter, *e, a);
                                }
                            }
                            if *wback || !*pre {
                                mark_all(&st, &mut escape, *e);
                                st.sp = None;
                                st.slots.clear();
                            }
                        }
                    }
                    Insn::Mem { load: false, rt, .. } if *rt != 13 => {
                        if let Some(os) = st.regs.get(rt).cloned() {
                            for o in os {
                                mark(o, &mut escape, &enter, *e, a);
                            }
                        }
                    }
                    Insn::Block { load, rn: 13, regs, before, add, wback, user } => {
                        let n = regs.count_ones() as i32;
                        if let Some(sp) = st.sp.filter(|_| !*user && regs & (1 << 13) == 0) {
                            let mut at = sp
                                + match (before, add) {
                                    (false, true) => 0,
                                    (true, true) => 4,
                                    (false, false) => 4 - 4 * n,
                                    (true, false) => -4 * n,
                                };
                            for i in 0..16u8 {
                                if regs >> i & 1 == 0 {
                                    continue;
                                }
                                if *load {
                                    if i == 15 {
                                        // A pop into pc. If it is a return,
                                        // the value is used up. If not, it
                                        // is jumped to, so it is marked.
                                        if !matches!(self.flow(a), Flow::Return) {
                                            if let Some(os) = st.slots.get(&at).cloned() {
                                                for o in os {
                                                    mark(o, &mut escape, &enter, *e, a);
                                                }
                                            }
                                        }
                                        st.slots.remove(&at);
                                    } else if i != 13 {
                                        let v = st.slots.get(&at).cloned().unwrap_or_default();
                                        if v.is_empty() {
                                            st.regs.remove(&i);
                                        } else {
                                            st.regs.insert(i, v);
                                        }
                                    }
                                } else if i == 15 {
                                    // ARM stores pc as the instruction's
                                    // own address, so it is not an lr.
                                    st.slots.remove(&at);
                                } else if i != 13 {
                                    let v = st.regs.get(&i).cloned().unwrap_or_default();
                                    if at >= 0 {
                                        for o in v {
                                            mark(o, &mut escape, &enter, *e, a);
                                        }
                                    } else if v.is_empty() {
                                        st.slots.remove(&at);
                                    } else {
                                        st.slots.insert(at, v);
                                    }
                                }
                                at += 4;
                            }
                            if *wback {
                                if let Some(sp) = st.sp.as_mut() {
                                    *sp += if *add { 4 * n } else { -4 * n };
                                }
                            }
                        } else if !*load {
                            // Stores to an address the analysis cannot
                            // name. The values escape the analysis.
                            for i in 0..15u8 {
                                if regs >> i & 1 == 1 {
                                    if let Some(os) = st.regs.get(&i).cloned() {
                                        for o in os {
                                            mark(o, &mut escape, &enter, *e, a);
                                        }
                                    }
                                }
                            }
                            if *wback {
                                mark_all(&st, &mut escape, *e);
                                st.sp = None;
                                st.slots.clear();
                            }
                        } else {
                            for i in 0..14u8 {
                                if regs >> i & 1 == 1 {
                                    st.regs.remove(&i);
                                }
                            }
                        }
                    }
                    _ => {}
                }
                if let Some(rm) = indirect_rm {
                    if let Some(os) = st.regs.get(&rm).cloned() {
                        for o in os {
                            mark(o, &mut escape, &enter, *e, a);
                        }
                    }
                }
                if trace == Some(*e) {
                    eprintln!("post  &{a:08X}: sp={:?} slots={:?}", st.sp, st.slots);
                }
                let succs = self.successors(a).0;
                for &s in &succs {
                    if !r.contains(&s) || (s != *e && self.entries.contains_key(&s)) {
                        // Control leaves the region with lr-derived values
                        // still in the frame. BASIC's FN marker is one: it
                        // is pushed here and returned to from the
                        // interpreter loop.
                        mark_all(&st, &mut escape, *e);
                        continue;
                    }
                    let merged = match before.get(&s) {
                        None => st.clone(),
                        Some(old) => {
                            let mut m = old.clone();
                            for (&rg, os) in &st.regs {
                                m.regs.entry(rg).or_default().extend(os.iter().copied());
                            }
                            for (&at, os) in &st.slots {
                                m.slots.entry(at).or_default().extend(os.iter().copied());
                            }
                            m.sp = match (m.sp, st.sp) {
                                (Some(x), Some(y)) if x == y => Some(x),
                                _ => None,
                            };
                            m
                        }
                    };
                    if before.get(&s) != Some(&merged) {
                        before.insert(s, merged);
                        work.push(s);
                    }
                }
            }
        }
        self.lr_escape = escape;
    }

    /// The CallBack handler's return to user mode, as the Wimp's
    /// `callbackpoll` ends. It is `LDM rN, {R0-R14}^` (the user bank, no
    /// pc), perhaps a NOP, then `LDR rX, [rN, #60]` and `MOVS pc, rX`. The
    /// block of seventeen words at rN is the context to resume. Returns the
    /// base register rN if the code at `addr` is such a return.
    fn user_return_at(&self, addr: u32) -> Option<u8> {
        let w = self.code.get(&addr)?;
        let Insn::Block { load: true, user: true, rn, regs, before: false, add: true, wback: false } = w.d.insn else {
            return None;
        };
        if regs != 0x7FFF || w.d.cond != Cond::Al {
            return None;
        }
        let nop = |a: u32| {
            self.code.get(&a).is_some_and(|x| {
                x.d.insn == Insn::Nop
                    || matches!(x.d.insn, Insn::Dp { op: DpOp::Mov, s: false, rd: 0,
                        op2: Operand2::Reg { rm: 0, shift: Shift::Imm(ShiftType::Lsl, 0) }, .. })
            })
        };
        let mut a = addr.wrapping_add(4);
        while nop(a) {
            a = a.wrapping_add(4);
        }
        let ld = self.code.get(&a)?;
        let Insn::Mem { load: true, width: Width::Word, rt, rn: base, offset: Offset::Imm(60), add: true, pre: true, wback: false } =
            ld.d.insn
        else {
            return None;
        };
        let mv = self.code.get(&a.wrapping_add(4))?;
        let back = matches!(mv.d.insn, Insn::Dp { op: DpOp::Mov, s: true, rd: 15,
            op2: Operand2::Reg { rm, shift: Shift::Imm(ShiftType::Lsl, 0) }, .. } if rm == rt);
        (base == rn && ld.d.cond == Cond::Al && mv.d.cond == Cond::Al && back).then_some(rn)
    }

    /// Whether a user return follows closely. If so, the `MSR SPSR` before
    /// it sets the PSR that the block already holds.
    fn user_return_follows(&self, addr: u32) -> bool {
        (1..=4).any(|k| self.user_return_at(addr.wrapping_add(4 * k)).is_some())
    }

    fn flow(&self, addr: u32) -> Flow {
        let Some(w) = self.code.get(&addr) else {
            return Flow::Unknown("not code".into());
        };
        let cond = w.d.cond;
        let pc = addr.wrapping_add(8);
        // After `Push "PC"`, a transfer is a call that returns to the word
        // after it, where the pushed pc points. That word is a NOP, since
        // ARM2 pushed pc+12. The callee returns with `Pull "PC"`. Examples
        // are the Wimp's starterrorbox, with `MOV PC,R14` to the routine it
        // ADR'd; its error box, with `B starterrorbox_draw`; and Free's
        // `LDR PC,[R14,#fs_entry]`.
        if let Some(&lr) = self.table_lr.get(&addr) {
            match w.d.insn {
                Insn::Branch { link: false, offset } if cond == Cond::Al => {
                    return Flow::Call(pc.wrapping_add(offset as u32))
                }
                Insn::Dp {
                    op: DpOp::Mov,
                    rd: 15,
                    s: false,
                    op2: Operand2::Reg { rm: 14, shift: Shift::Imm(ShiftType::Lsl, 0) },
                    ..
                } if cond == Cond::Al => return Flow::Jump(lr),
                _ => {}
            }
        }
        if cond == Cond::Al && self.pushes_pc(addr.wrapping_sub(4)) {
            match w.d.insn {
                Insn::Branch { link: false, offset } => return Flow::Call(pc.wrapping_add(offset as u32)),
                Insn::Dp { rd: 15, s: false, .. } | Insn::Mem { load: true, rt: 15, width: Width::Word, .. } => {
                    return Flow::Indirect { cont: Some(addr.wrapping_add(4)) }
                }
                _ => {}
            }
        }
        match w.d.insn {
            Insn::Branch { link, offset } => {
                let t = pc.wrapping_add(offset as u32);
                // `ADR lr, label` then `B` is a call that returns to the
                // label.
                if link || self.lr_before(addr, cond).is_some() {
                    Flow::Call(t)
                } else {
                    Flow::Jump(t)
                }
            }
            Insn::Bx { link: false, rm: 14 } => Flow::Return,
            Insn::Bx { link: false, .. } => Flow::Indirect { cont: self.lr_before(addr, cond) },
            Insn::Bx { link: true, .. } => Flow::Indirect { cont: Some(addr.wrapping_add(4)) },
            Insn::Dp { rd: 15, s: true, .. } => {
                Flow::Fault("a data-processing write to pc with S: an exception return, as in 26-bit mode".into())
            }
            Insn::Dp { rd: 15, op, rn, op2, .. } => {
                if let (DpOp::Add, Operand2::Reg { rm, shift: Shift::Imm(ShiftType::Lsl, 2) }) = (op, op2) {
                    if rn != 15 {
                        if let Some(f) = self.lut_jump(addr, rn, rm, cond) {
                            return f;
                        }
                    }
                }
                if let (DpOp::Add, 15, Operand2::Reg { rm, shift: Shift::Imm(ShiftType::Lsl, k) }) = (op, rn, op2) {
                    return match self
                        .offset_table(addr, rm, cond)
                        .or_else(|| self.lut_jump(addr, rn, rm, cond))
                        .or_else(|| self.table_bound(addr, rm, cond).map(|(first, count)| Flow::Table { rm, shift: k, first, count, data: None }))
                        .or_else(|| self.branch_run(addr, k, cond).map(|(first, count)| Flow::Table { rm, shift: k, first, count, data: None }))
                    {
                        Some(f) => f,
                        None => Flow::Unknown(
                            "a computed jump with no bound (CMP before ADDLO/ADDLS, or an AND mask)".into(),
                        ),
                    };
                }
                if let (DpOp::Mov, Operand2::Reg { rm: 14, shift: Shift::Imm(ShiftType::Lsl, 0) }) = (op, op2) {
                    return self.lr_table(addr, cond).unwrap_or(Flow::Return);
                }
                if let (DpOp::Sub, Operand2::Reg { rm, shift: Shift::Imm(ShiftType::Lsl, k) }) = (op, op2) {
                    if let Some(f) = self.down_table(addr, rn, rm, k, cond) {
                        return f;
                    }
                }
                // A target made only of pc and constants is a branch.
                let n = if op.uses_rn() { (rn == 15).then_some(pc) } else { Some(0) };
                let v = match op2 {
                    Operand2::Imm { value, .. } => Some(value),
                    Operand2::Reg { rm: 15, shift: Shift::Imm(ShiftType::Lsl, 0) } => Some(pc),
                    _ => None,
                };
                let t = match (op, n, v) {
                    (DpOp::Mov, _, Some(v)) => Some(v),
                    (DpOp::Add, Some(n), Some(v)) => Some(n.wrapping_add(v)),
                    (DpOp::Sub, Some(n), Some(v)) => Some(n.wrapping_sub(v)),
                    _ => None,
                };
                match t {
                    Some(t) => Flow::Jump(t),
                    None => match (op, op2) {
                        (DpOp::Mov, Operand2::Reg { rm, shift: Shift::Imm(ShiftType::Lsl, 0) })
                            if self.returns_via.get(&addr).is_some_and(|&m| m & (1 << rm) != 0) =>
                        {
                            Flow::Return
                        }
                        _ => Flow::Indirect { cont: self.lr_before(addr, cond) },
                    },
                }
            }
            Insn::Mem { load: true, rt: 15, width: Width::Word, rn, pre, offset, add, wback } => {
                // `MOV lr, pc` then popping pc is a call to the address
                // pushed. The Filer's BL_Wimp does this: Push the Wimp's
                // routine, MOV R14,PC, Pull PC, NOP.
                if let Some(back) = self.lr_set_just_before(addr, cond) {
                    return Flow::Indirect { cont: Some(back) };
                }
                // Popping pc (LDR pc, [sp], #4) is a return.
                if rn == 13 && !pre && add && offset == Offset::Imm(4) && !wback {
                    Flow::Return
                } else {
                    Flow::Indirect { cont: self.lr_before(addr, cond) }
                }
            }
            // Storing pc is data, and control goes on. The Wimp stores it
            // to spoil a cache (STRNE PC, selecttable_args). Pushing it
            // with STR pc, [sp, #-4]! gives the return address of the call
            // that follows (see above).
            Insn::Mem { load: false, rt: 15, width: Width::Word, pre: true, wback: false, .. } => Flow::Next,
            _ if self.pushes_pc(addr) => Flow::Next,
            Insn::Mem { rt: 15, .. } => Flow::Unknown("a transfer of pc that is not a word load".into()),
            Insn::Block { load: true, user: true, .. } if self.user_return_at(addr).is_some() => {
                Flow::Fault(USER_RETURN.into())
            }
            Insn::Block { load: true, regs, user: true, .. } if regs & 0x8000 != 0 => {
                Flow::Fault("an LDM restoring pc and the mode (^): an exception return, as in 26-bit mode".into())
            }
            Insn::Block { load: true, regs, rn, .. } if regs & 0x8000 != 0 => {
                if let Some(back) = self.lr_set_just_before(addr, cond) {
                    return Flow::Indirect { cont: Some(back) };
                }
                if rn == 13 {
                    Flow::Return
                } else {
                    Flow::Indirect { cont: self.lr_before(addr, cond) }
                }
            }
            Insn::Mul { rd: 15, .. } | Insn::Mrs { rd: 15, .. } | Insn::Clz { rd: 15, .. } => {
                Flow::Unknown("a write to pc".into())
            }
            Insn::Unknown => Flow::Unknown("an instruction the compiler does not model".into()),
            _ => Flow::Next,
        }
    }

    /// `ADR rn, at` just before `SUB pc, rn, rm, LSL #k` is a table counted
    /// downwards. The Wimp uses one on the negative furniture icon number:
    /// `ADR R14,wiconjump-8`, `SUB PC,R14,R4,ASL #2`. The rows are the run
    /// after the SUB. Each is a `B`, or a `MOV pc, #n` for a number kept
    /// unused. Row j is where rm is `(at - row) >> k`.
    fn down_table(&self, addr: u32, rn: u8, rm: u8, k: u32, cond: Cond) -> Option<Flow> {
        if rn == 15 || rm == rn || rm == 15 || k != 2 || cond != Cond::Al {
            return None;
        }
        let adr = self.code.get(&addr.wrapping_sub(4))?;
        let Insn::Dp { op: op @ (DpOp::Add | DpOp::Sub), s: false, rd, rn: 15, op2: Operand2::Imm { value, .. } } = adr.d.insn
        else {
            return None;
        };
        if rd != rn || adr.d.cond != Cond::Al {
            return None;
        }
        let pc = addr.wrapping_add(4);
        let at = if op == DpOp::Add { pc.wrapping_add(value) } else { pc.wrapping_sub(value) };
        let start = addr.wrapping_add(4);
        let mut n = 0u32;
        while let Some(d) = self.raw(start.wrapping_add(4 * n)) {
            let row = matches!(d.insn, Insn::Branch { link: false, .. })
                || matches!(d.insn, Insn::Dp { rd: 15, op: DpOp::Mov, s: false, op2: Operand2::Imm { .. }, .. });
            if d.cond == Cond::Al && row {
                n += 1;
            } else {
                break;
            }
        }
        let d = at.wrapping_sub(start) as i32;
        if n == 0 || d % 4 != 0 {
            return None;
        }
        // Row 0 is the largest value, and the last row the smallest.
        let top = d >> k;
        Some(Flow::Table { rm, shift: k, first: top - (n as i32 - 1), count: n, data: Some(TableData::Down { at }) })
    }

    /// `MOV pc, lr` just after `ADD lr, pc, rm, LSL #k`, under the same
    /// condition, is a jump table on rm, not a return. The cases are the
    /// code beside the ADD, bounded as an `ADD pc, pc, rm` there would be.
    fn lr_table(&self, addr: u32, cond: Cond) -> Option<Flow> {
        let at = addr.wrapping_sub(4);
        let p = self.code.get(&at)?;
        let Insn::Dp { op: DpOp::Add, s: false, rd: 14, rn: 15, op2: Operand2::Reg { rm, shift: Shift::Imm(ShiftType::Lsl, k) } } =
            p.d.insn
        else {
            return None;
        };
        if p.d.cond != cond || rm >= 14 {
            return None;
        }
        let (first, count) = self.table_bound(at, rm, cond).or_else(|| self.branch_run(at, k, cond))?;
        Some(Flow::Table { rm, shift: k, first, count, data: Some(TableData::Beside { at }) })
    }

    /// Where OS_WriteS resumes. That is the next word that is code, past
    /// its inline string's terminator and any ALIGN padding.
    fn writes_resume(&self, addr: u32) -> Option<u32> {
        let mut end = addr.wrapping_add(4);
        while let Some(w) = self.word(end) {
            for k in 0..4 {
                if w >> (8 * k) & 0xFF == 0 {
                    let mut resume = end.wrapping_add(k).wrapping_add(1);
                    resume = (resume + 3) & !3;
                    while !self.is_code(resume) && self.word(resume).is_some() {
                        resume += 4;
                    }
                    return self.is_code(resume).then_some(resume);
                }
            }
            end = end.wrapping_add(4);
        }
        None
    }

    /// A code variable's block, given to `SWI OS_SetVarVal` at `addr`. It
    /// is found from `ADR R1, block` and `MOV R4, #16` (VarType_Code) in
    /// the straight run of code just before the SWI. The Wimp's Wimp$State
    /// is set this way: `ADRVC R1,CommandWindow_var` ...
    /// `MOVVC R4,#VarType_Code`, `SWIVC XOS_SetVarVal`. The block begins
    /// with its write and read entries, which the kernel calls
    /// (runtime/sysvars.c). The ADR makes the first an entry. The read
    /// entry, one word on, is found here. The last writes of R1 and R4
    /// before the SWI must be these.
    fn code_variable_block(&self, addr: u32) -> Option<u32> {
        let (mut block, mut r1, mut r4) = (None, false, false);
        let mut a = addr;
        for _ in 0..8 {
            a = a.wrapping_sub(4);
            let Some(w) = self.code.get(&a) else { break };
            if !matches!(self.flow(a), Flow::Next) {
                break;
            }
            match w.d.insn {
                Insn::Dp { op: op @ (DpOp::Add | DpOp::Sub), s: false, rd: 1, rn: 15, op2: Operand2::Imm { value, .. } }
                    if !r1 =>
                {
                    let pc = a.wrapping_add(8);
                    block = Some(if op == DpOp::Add { pc.wrapping_add(value) } else { pc.wrapping_sub(value) });
                    r1 = true;
                }
                Insn::Dp { op: DpOp::Mov, rd: 4, op2: Operand2::Imm { value, .. }, .. } if !r4 => {
                    if value != VAR_TYPE_CODE {
                        return None;
                    }
                    r4 = true;
                }
                Insn::Dp { op: DpOp::Tst | DpOp::Teq | DpOp::Cmp | DpOp::Cmn, .. } => {}
                Insn::Dp { rd: 1, .. } | Insn::Mem { load: true, rt: 1, .. } if !r1 => return None,
                Insn::Dp { rd: 4, .. } | Insn::Mem { load: true, rt: 4, .. } if !r4 => return None,
                _ => {}
            }
        }
        if r4 { block } else { None }
    }

    /// Whether the word at `addr` is the second half of an ADRL. That is an
    /// ADD or SUB `rd, rd, #imm` under the first half's condition.
    fn adrl_second(&self, addr: u32, rd: u8, cond: Cond) -> bool {
        self.code.get(&addr).is_some_and(|w| {
            w.d.cond == cond
                && matches!(w.d.insn, Insn::Dp { op: DpOp::Add | DpOp::Sub, s: false, rd: r, rn, op2: Operand2::Imm { .. } }
                    if r == rd && rn == rd)
        })
    }

    /// Where control may go next within a region, before entries are
    /// taken into account. Also any new entries the instruction makes.
    fn successors(&self, addr: u32) -> (Vec<u32>, Vec<(u32, String)>) {
        let cond = self.code.get(&addr).map_or(Cond::Al, |w| w.d.cond);
        let next = addr.wrapping_add(4);
        let may_skip = cond != Cond::Al;
        let mut succ = Vec::new();
        let mut new = Vec::new();
        match self.flow(addr) {
            Flow::Next => {
                // OS_WriteS does not fall into its own string. Control
                // resumes past it, at a new entry.
                if matches!(
                    self.code.get(&addr).map(|w| w.d.insn),
                    Some(Insn::Swi { number }) if number & !X_BIT == 1
                ) {
                    if let Some(resume) = self.writes_resume(addr) {
                        succ.push(resume);
                        new.push((resume, format!("after OS_WriteS at {}", self.where_(addr))));
                    }
                } else {
                    succ.push(next);
                }
                // A code variable's read entry, at the block's second word.
                // The kernel calls it, and nothing in the unit reaches it.
                if matches!(
                    self.code.get(&addr).map(|w| w.d.insn),
                    Some(Insn::Swi { number }) if number & !X_BIT == OS_SET_VAR_VAL
                ) {
                    if let Some(block) = self.code_variable_block(addr) {
                        let read = block.wrapping_add(4);
                        if self.is_code(read) {
                            new.push((read, format!("the read entry of a code variable set at {}", self.where_(addr))));
                        }
                    }
                }
            }
            Flow::Jump(t) => {
                succ.push(t);
                if may_skip {
                    succ.push(next);
                }
            }
            Flow::Call(t) => {
                new.push((t, format!("called from {}", self.where_(addr))));
                let back = self.call_back(addr);
                succ.push(back);
                if back != next && may_skip {
                    succ.push(next);
                }
            }
            Flow::Table { shift, first, count, data, .. } => {
                if may_skip {
                    succ.push(next);
                }
                for (_, t) in self.table_targets(addr, shift, first, count, data) {
                    succ.push(t);
                }
            }
            Flow::Return => {
                if may_skip {
                    succ.push(next);
                }
            }
            Flow::Indirect { cont } => {
                if let Some(c) = cont {
                    succ.push(c);
                }
                if may_skip && cont != Some(next) {
                    succ.push(next);
                }
            }
            Flow::Unknown(_) => {}
            Flow::Fault(_) => {
                if may_skip {
                    succ.push(next);
                }
            }
        }
        // An address taken with ADR, into any register but lr, may be
        // called later, so it is an entry. ADR lr makes a return address,
        // unless lr is stored straight after. The Wimp does that to store
        // a new task's PC: `ADRL R14,runthetask` then `STR R14,[R5,#...]`.
        if let Some(w) = self.code.get(&addr) {
            if let Insn::Dp { op: op @ (DpOp::Add | DpOp::Sub), s: false, rd, rn: 15, op2: Operand2::Imm { value, .. } } =
                w.d.insn
            {
                let pc = addr.wrapping_add(8);
                let t = if op == DpOp::Add { pc.wrapping_add(value) } else { pc.wrapping_sub(value) };
                let taken = rd != 14 || self.stores_lr(addr.wrapping_add(4)) || self.calls_through_lr(addr.wrapping_add(4));
                if taken && rd != 15 && self.is_code(t) && !self.adrl_second(addr.wrapping_add(4), rd, w.d.cond) {
                    new.push((t, format!("its address is taken at {}", self.where_(addr))));
                    for row in self.branch_table_rows(t) {
                        new.push((row, format!("a row of the branch table whose address is taken at {}", self.where_(addr))));
                    }
                }
                if taken && rd != 15 && !self.adrl_second(addr.wrapping_add(4), rd, w.d.cond) {
                    for row in self.record_table_rows(t) {
                        new.push((row, format!("the branch of a record in the table whose address is taken at {}", self.where_(addr))));
                    }
                }
            }
            // The same for an address taken with ADRL: ADD/SUB rd,pc,#a
            // then ADD/SUB rd,rd,#b. BASIC takes MSGATLINE's address this
            // way, for CALL !ERRXLATE.
            if let Insn::Dp { op: op_b @ (DpOp::Add | DpOp::Sub), s: false, rd, rn, op2: Operand2::Imm { value: b, .. } } =
                w.d.insn
            {
                let first = addr.wrapping_sub(4);
                if let Some(p) = self.code.get(&first) {
                    if let Insn::Dp {
                        op: op_a @ (DpOp::Add | DpOp::Sub),
                        s: false,
                        rd: rd_a,
                        rn: 15,
                        op2: Operand2::Imm { value: a, .. },
                    } = p.d.insn
                    {
                        let taken = rd != 14 || self.stores_lr(addr.wrapping_add(4)) || self.calls_through_lr(addr.wrapping_add(4));
                        if rn == rd && rd_a == rd && taken && rd != 15 && p.d.cond == w.d.cond {
                            let pc = first.wrapping_add(8);
                            let base = if op_a == DpOp::Add { pc.wrapping_add(a) } else { pc.wrapping_sub(a) };
                            let t = if op_b == DpOp::Add { base.wrapping_add(b) } else { base.wrapping_sub(b) };
                            if self.is_code(t) {
                                new.push((t, format!("its address is taken at {}", self.where_(first))));
                                for row in self.branch_table_rows(t) {
                                    new.push((row, format!("a row of the branch table whose address is taken at {}", self.where_(first))));
                                }
                            }
                            for row in self.record_table_rows(t) {
                                new.push((row, format!("the branch of a record in the table whose address is taken at {}", self.where_(first))));
                            }
                        }
                    }
                }
            }
        }
        (succ, new)
    }

    /// Whether the instruction at `a` is `Push "PC"`: STR pc, [sp, #-4]!,
    /// unconditional.
    fn pushes_pc(&self, a: u32) -> bool {
        self.code.get(&a).is_some_and(|w| {
            w.d.cond == Cond::Al
                && matches!(
                    w.d.insn,
                    Insn::Mem {
                        load: false,
                        rt: 15,
                        rn: 13,
                        width: Width::Word,
                        pre: true,
                        add: false,
                        wback: true,
                        offset: Offset::Imm(4),
                    }
                )
        })
    }

    /// Whether the code from `a` calls the address in lr. The pattern is
    /// any more ADRs and ADRLs into lr (the other arm of a condition), then
    /// `Push "PC"` and `MOV PC, R14`. The Wimp's starterrorbox does this,
    /// with `ADREQL R14, iconformatted_system` and
    /// `ADRNEL R14, iconformatted_fancy`. The address in lr is then a
    /// routine's, not a return address.
    fn calls_through_lr(&self, a: u32) -> bool {
        let mut a = a;
        for _ in 0..8 {
            let Some(w) = self.code.get(&a) else { return false };
            match w.d.insn {
                Insn::Dp { op: DpOp::Add | DpOp::Sub, s: false, rd: 14, rn: 14 | 15, op2: Operand2::Imm { .. } } => {
                    a = a.wrapping_add(4)
                }
                _ => break,
            }
        }
        self.pushes_pc(a)
            && self.code.get(&a.wrapping_add(4)).is_some_and(|w| {
                w.d.cond == Cond::Al
                    && matches!(
                        w.d.insn,
                        Insn::Dp {
                            op: DpOp::Mov,
                            rd: 15,
                            s: false,
                            op2: Operand2::Reg { rm: 14, shift: Shift::Imm(ShiftType::Lsl, 0) },
                            ..
                        }
                    )
            })
    }

    /// Whether the instruction at `a` stores lr. If so, an address put in
    /// lr is kept, not returned to.
    fn stores_lr(&self, a: u32) -> bool {
        // STR lr, or an STM of lr to a block that is not the stack. (A
        // stack push of lr belongs to a prologue.) The Wimp's colour
        // mapping descriptor is built this way: `ADR R14,colourmapfunc`
        // then `STMIA R2!,{R3,R14}`. SpriteExtend later calls it.
        matches!(
            self.code.get(&a).map(|w| w.d.insn),
            Some(Insn::Mem { load: false, rt: 14, .. })
        ) || matches!(
            self.code.get(&a).map(|w| w.d.insn),
            Some(Insn::Block { load: false, rn, regs, .. }) if rn != 13 && regs & (1 << 14) != 0
        )
    }

    /// The rows after the first of a branch table. A branch table is a run
    /// of unconditional B instructions starting at an address taken with
    /// ADR. Code computes a row from the table's address and jumps to it,
    /// as the Wimp's openwlp3_jumptable does with `ADD R0,R0,x0,LSL#2` then
    /// `MOV PC,R0`. So each row is an entry, as the first is. The result is
    /// empty unless the run starts with two Bs.
    ///
    /// A row is read from its word, not from its decoded code. A row that
    /// branches to the next instruction is still a row, though the decoded
    /// code has it as a no-op. The Wimp's last row is one: it is
    /// `B openwlp3_skip_to_bottom` (method 3), just before that label.
    fn branch_table_rows(&self, t: u32) -> Vec<u32> {
        let mut rows = Vec::new();
        if !self.raw_b(t) {
            return rows;
        }
        let mut a = t.wrapping_add(4);
        while self.raw_b(a) {
            rows.push(a);
            a = a.wrapping_add(4);
        }
        rows
    }

    /// The branches of a table of records, starting at an address taken
    /// with ADR. Each record is a word and then a B. The Filer's
    /// messages_processed_start is one: `DCD Message_FilerOpenDir`,
    /// `B message_fileropendir_code`, ..., `DCD -1`. Code searches the
    /// table for a word and jumps to the B after it
    /// (`LDR r14,[r2],#8` ... `SUBEQ pc,r2,#4`). Each B is an entry. The
    /// result is empty unless the table starts with two records. Each
    /// record's word must be data, not code.
    ///
    /// Unlike the other table readers, this one reads the B from the
    /// decoded code. A record's B to the next word would branch into the
    /// next record's data word, so a no-op there ends the table.
    fn record_table_rows(&self, t: u32) -> Vec<u32> {
        let is_b = |a: u32| {
            self.code.get(&a).is_some_and(|w| w.d.cond == Cond::Al && matches!(w.d.insn, Insn::Branch { link: false, .. }))
        };
        let mut rows = Vec::new();
        let mut a = t;
        while !self.is_code(a) && is_b(a.wrapping_add(4)) {
            rows.push(a.wrapping_add(4));
            a = a.wrapping_add(8);
        }
        if rows.len() < 2 {
            rows.clear();
        }
        rows
    }

    /// The rows of each call table (see `table_lr`), and the instruction
    /// after a conditional dispatch. They are found from the code alone,
    /// before discovery.
    ///
    /// lr is set just before the dispatch, by `MOV LR,PC` or `ADR LR`. The
    /// Pinboard's ReadBufferedList uses `MOV LR,PC`: `MOV LR,PC`,
    /// `ADD PC,PC,R0,ASL #2`, `B %BT01`, then `B AddIconXY` ... Or lr is
    /// set by an `ADR LR` further back, across compares and conditional
    /// branches.
    ///
    /// Some rows are not calls. Where lr points is where the calls come
    /// back to, so it is not a call. Nor is a row that the code there runs
    /// on into (`runs_on`). Nor is a row that branches to where the range
    /// check before the dispatch sends an index out of range
    /// (`range_exits`), since that is the routine's own way out.
    fn find_call_tables(&self) -> HashMap<u32, u32> {
        let mut m = HashMap::new();
        for (&a, w) in &self.code {
            let Insn::Dp { op: DpOp::Add, rd: 15, rn: 15, s: false, op2: Operand2::Reg { .. } } = w.d.insn else {
                continue;
            };
            let Flow::Table { shift, first, count, data: None, .. } = self.flow(a) else { continue };
            let (lr, setter) = match self.lr_set_just_before(a, w.d.cond) {
                Some(lr) => (lr, a.wrapping_sub(4)),
                None => match self.lr_chain_at(a) {
                    Some((at, lr)) => (lr, at),
                    None => continue,
                },
            };
            let cases = Self::table_cases(a, shift, first, count);
            let end = cases.iter().map(|&(_, row)| row).max().unwrap_or(a);
            let back = self.runs_on(lr, a, end);
            let exits = self.range_exits(setter);
            let to_exit = |row: u32| {
                self.raw(row).is_some_and(|d| match d.insn {
                    Insn::Branch { link: false, offset } if d.cond == Cond::Al => {
                        exits.contains(&row.wrapping_add(8).wrapping_add(offset as u32))
                    }
                    _ => false,
                })
            };
            for (_, row) in cases {
                if !back.contains(&row) && !to_exit(row) {
                    m.insert(row, lr);
                }
            }
            if w.d.cond != Cond::Al && !back.contains(&a.wrapping_add(4)) {
                m.insert(a.wrapping_add(4), lr);
            }
        }
        m
    }

    /// Where the conditional branches just before `setter` go. `setter` is
    /// the instruction that sets lr for a call table. Such a branch is a
    /// range check, sending an index that is out of range to the
    /// routine's own way out. SCSIFS's MiscEntry has one:
    /// `CMPS R0,#MiscOp_DriveStatus`, `BHI %FT95`, `MOV LR,PC`,
    /// `ADD PC,PC,R0,LSL #2`. ADFS has `BCS %FT20`. A row that branches to
    /// the same place (SCSIFS's row 6, `B %FT95`) is a jump there, not a
    /// call. The walk back stops at anything that does not run on, such as
    /// a return or an unconditional transfer.
    fn range_exits(&self, setter: u32) -> BTreeSet<u32> {
        let mut out = BTreeSet::new();
        let mut q = setter;
        for _ in 0..4 {
            q = q.wrapping_sub(4);
            let Some(w) = self.code.get(&q) else { break };
            match w.d.insn {
                Insn::Branch { link: false, offset } if w.d.cond != Cond::Al => {
                    out.insert(q.wrapping_add(8).wrapping_add(offset as u32));
                }
                _ if matches!(self.flow(q), Flow::Next) => {}
                _ => break,
            }
        }
        out
    }

    /// `from`, and the words that code there runs on into without a
    /// transfer, up to `end` and stopping short of the dispatch at `at`.
    /// After `MOV LR,PC`, the calls come back to the word after the
    /// dispatch, which may run on into the table. SpriteExtend's
    /// converttrans_new comes back to a `STR`, then `B converttrans_new`.
    /// That B is row 0 as well as the loop, so it is a jump, not a call.
    fn runs_on(&self, from: u32, at: u32, end: u32) -> BTreeSet<u32> {
        let mut on = BTreeSet::from([from]);
        let mut q = from;
        while q < end {
            let Some(w) = self.code.get(&q) else { break };
            let falls = w.d.cond != Cond::Al
                || matches!(w.d.insn, Insn::Branch { link: true, .. })
                || matches!(self.flow(q), Flow::Next);
            q = q.wrapping_add(4);
            if !falls || q == at {
                break;
            }
            on.insert(q);
        }
        on
    }

    /// Every entry. These are the unit's own, then everything that code
    /// reachable from them calls or takes the address of, repeated until
    /// nothing new is found.
    fn discover(&mut self) -> BTreeSet<u32> {
        let mut todo: Vec<u32> = self.entries.keys().copied().collect();
        let mut seen = BTreeSet::new();
        while let Some(a) = todo.pop() {
            if !seen.insert(a) || !self.is_code(a) {
                continue;
            }
            let (succ, new) = self.successors(a);
            for (t, why) in new {
                if !self.entries.contains_key(&t) || !why.starts_with("called from") {
                    self.entry(t, why);
                }
                todo.push(t);
            }
            todo.extend(succ);
        }
        seen
    }

    /// Adds an entry that only this unit's own tail calls reach: a shared
    /// join point. It is not `external`, because nothing outside can enter
    /// it. So the summaries may use what they prove about it, as they do
    /// for a routine that only a BL reaches.
    fn entry_shared(&mut self, addr: u32, why: String) {
        if self.is_code(addr) {
            self.entries.entry(addr).or_insert(why);
        }
    }

    /// Splits off addresses compiled in more than one region, which are
    /// shared join points. Each becomes its own region, compiled once.
    /// The branches into it were already tail calls, and the copies inside
    /// the regions that reached it are dropped. This repeats until nothing
    /// changes, because new entries shrink regions, which can expose more
    /// sharing.
    ///
    /// The split is made at the frontier: the shared addresses that an
    /// unshared instruction reaches. An entry counts as unshared, since no
    /// other region contains it. So a shared run stays one region,
    /// entered at its head. The frontier cannot reach a shared address
    /// whose every incoming path comes through other shared runs. Such an
    /// address is split directly.
    fn share(&mut self) {
        loop {
            let mut count: HashMap<u32, u32> = HashMap::new();
            let mut edges: Vec<(u32, u32)> = Vec::new();
            for &e in self.entries.keys().collect::<Vec<_>>() {
                let r = self.region(e);
                for &a in &r {
                    *count.entry(a).or_insert(0) += 1;
                    for t in self.successors(a).0 {
                        edges.push((a, t));
                    }
                }
            }
            let shared = |a: u32| count.get(&a).is_some_and(|&n| n > 1);
            let mut split: BTreeSet<u32> = BTreeSet::new();
            for (a, t) in edges {
                if !self.entries.contains_key(&t) && shared(t) && !shared(a) {
                    split.insert(t);
                }
            }
            // What the frontier cannot reach: a shared address with only
            // shared predecessors. This is rare. Split the lowest such
            // address, and go round again.
            if split.is_empty() {
                let mut still: Vec<u32> = count
                    .keys()
                    .filter(|&a| shared(*a) && !self.entries.contains_key(a))
                    .copied()
                    .collect();
                still.sort_unstable();
                if let Some(&t) = still.first() {
                    split.insert(t);
                }
            }
            if split.is_empty() {
                break;
            }
            for t in split {
                let n = count[&t];
                self.entry_shared(t, format!("a join point of {n} regions"));
            }
        }
    }

    // ---- flags across calls ----

    /// Flags live where the routine at `entry` returns.
    fn ret_flags_of(&self, entry: u32) -> u8 {
        if self.external.contains(&entry) || self.sp_escape.contains(&entry) {
            0xF
        } else {
            self.ret_flags.get(&entry).copied().unwrap_or(0)
        }
    }

    /// Flags the routine at t reads before it sets them.
    fn entry_flags_of(&self, t: u32) -> u8 {
        if self.entries.contains_key(&t) {
            self.entry_flags.get(&t).copied().unwrap_or(0)
        } else {
            0xF
        }
    }

    /// Flags the routine at t sets on every path to a return.
    fn must_flags_of(&self, t: u32) -> u8 {
        self.must_flags.get(&t).copied().unwrap_or(0)
    }

    /// Registers live where the routine at `entry` returns.
    fn ret_regs_of(&self, entry: u32) -> u16 {
        if self.external.contains(&entry) || self.sp_escape.contains(&entry) {
            ALL_INT
        } else {
            self.ret_regs.get(&entry).copied().unwrap_or(0)
        }
    }

    /// What a return of the routine at `entry` gives back. With a
    /// signature, that is its results. Without one, it is everything its
    /// callers read, in the state block.
    fn gives_back(&self, entry: u32) -> u16 {
        self.sig.get(&entry).map_or(self.ret_regs_of(entry), |s| s.1)
    }

    /// Registers the routine at t reads before it writes them.
    fn ref_regs_of(&self, t: u32) -> u16 {
        if self.entries.contains_key(&t) {
            self.ref_regs.get(&t).copied().unwrap_or(0)
        } else {
            ALL_INT
        }
    }

    /// Registers the routine at t may write.
    fn mod_regs_of(&self, t: u32) -> u16 {
        let kept = self.frames.get(&t).map_or(0, |f| f.preserved);
        self.mod_regs.get(&t).copied().unwrap_or(ALL_INT) & !kept
    }

    /// Registers the routine at t reads from the state block. These are
    /// the ones live where it starts that it, or something it calls,
    /// names.
    fn reads_of(&self, t: u32) -> u16 {
        self.ref_regs_of(t) & self.touch_regs.get(&t).copied().unwrap_or(ALL_INT)
    }

    /// What is live where control leaves a region for another entry. That
    /// is a tail call, which returns to where this routine would have.
    fn tail_live(&self, t: u32, entry: u32) -> Live {
        Live {
            int: self.ref_regs_of(t) | self.ret_regs_of(entry),
            fp: Live::ALL.fp,
            flags: self.entry_flags_of(t) | (self.ret_flags_of(entry) & !self.must_flags_of(t)),
            slots: 0,
        }
    }

    /// Registers the region may write (`touch` false), or names at all
    /// (`touch` true). This covers its own instructions and its callees.
    /// A SWI counts its own registers if the typed API defines them, and
    /// otherwise all of them, as does an unknown call.
    fn may_write(&self, entry: u32, r: &BTreeSet<u32>, touch: bool) -> u16 {
        let of = |t: u32| {
            if touch {
                self.touch_regs.get(&t).copied().unwrap_or(ALL_INT)
            } else {
                self.mod_regs_of(t)
            }
        };
        let mut m = 0;
        for &a in r {
            let (u, d) = self.explicit_int(a);
            m |= d | if touch { u } else { 0 };
            match self.flow(a) {
                Flow::Call(t) => m |= LR | of(t),
                _ if self.swi_io(a).is_some() => {
                    let (i, o) = self.swi_io(a).unwrap();
                    m |= o | if touch { i } else { 0 };
                }
                _ if self.is_call(a) => m |= ALL_INT,
                Flow::Indirect { cont: None } | Flow::Unknown(_) | Flow::Fault(_) => m |= ALL_INT,
                _ => {}
            }
            for s in self.successors(a).0 {
                let inside = r.contains(&s) && (s == entry || !self.entries.contains_key(&s));
                if !inside && self.entries.contains_key(&s) {
                    m |= of(s);
                }
            }
        }
        m
    }

    /// Flags set on every path from the region's entry to a return. This is
    /// a forward analysis: where two paths meet, a flag counts as set only
    /// if it is set on both. A call sets what its callee always sets. A SWI
    /// or an unknown call sets nothing for certain.
    fn must_set(&self, entry: u32, r: &BTreeSet<u32>) -> u8 {
        let mut before: HashMap<u32, u8> = HashMap::from([(entry, 0)]);
        let mut work = vec![entry];
        let mut at_returns: Option<u8> = None;
        let meet = |x: &mut Option<u8>, f: u8| *x = Some(x.map_or(f, |y| y & f));
        while let Some(a) = work.pop() {
            let set = before[&a];
            let d = self.code[&a].d;
            let out = if d.cond != Cond::Al {
                set
            } else {
                match self.flow(a) {
                    Flow::Call(t) => set | self.must_flags_of(t),
                    _ if self.is_call(a) => set,
                    _ => set | Self::flags_set(&d),
                }
            };
            match self.flow(a) {
                Flow::Return => meet(&mut at_returns, set),
                Flow::Indirect { cont: None } | Flow::Unknown(_) | Flow::Fault(_) => meet(&mut at_returns, set),
                _ => {}
            }
            for s in self.successors(a).0 {
                if r.contains(&s) && (s == entry || !self.entries.contains_key(&s)) {
                    let merged = before.get(&s).map_or(out, |&old| old & out);
                    if before.get(&s) != Some(&merged) {
                        before.insert(s, merged);
                        work.push(s);
                    }
                } else if self.entries.contains_key(&s) {
                    meet(&mut at_returns, out | self.must_flags_of(s));
                } else {
                    meet(&mut at_returns, out);
                }
            }
        }
        at_returns.unwrap_or(0xF)
    }

    /// Whether the instruction at `a` writes sp with a value from
    /// elsewhere, such as `LDR sp, [..]` or `MOV sp, rN`. The frame's own
    /// writes of sp do not count: a constant step, or the writeback of a
    /// load or store.
    fn loads_sp(&self, a: u32) -> bool {
        let Some(w) = self.code.get(&a) else { return false };
        match w.d.insn {
            Insn::Dp { op: DpOp::Add | DpOp::Sub, rd: 13, rn: 13, op2: Operand2::Imm { .. }, .. } => false,
            Insn::Block { rn: 13, regs, .. } => regs & (1 << 13) != 0,
            Insn::Mem { rn: 13, rt, .. } => rt == 13,
            _ => self.explicit_int(a).1 & (1 << 13) != 0,
        }
    }

    /// Works out the summaries of every routine. Each is repeated until it
    /// stops changing. First come the registers each may write, starting
    /// from none and growing. Then the flags each always sets, starting
    /// from all and shrinking. Last come the flags and registers each
    /// reads, and those live where each returns, starting from none and
    /// growing.
    fn summaries(&mut self) {
        let entries: Vec<u32> = self.entries.keys().copied().collect();
        let regions: HashMap<u32, BTreeSet<u32>> = entries.iter().map(|&e| (e, self.region(e))).collect();
        let holders: HashMap<u32, HashMap<u32, u16>> =
            entries.iter().map(|&e| (e, self.lr_holders(e, &regions[&e]))).collect();
        for &e in &entries {
            self.must_flags.insert(e, 0xF);
            self.mod_regs.insert(e, 0);
            self.touch_regs.insert(e, 0);
            if regions[&e].iter().any(|&a| self.loads_sp(a)) {
                self.sp_escape.insert(e);
            }
        }
        loop {
            let mut changed = false;
            for &e in &entries {
                self.returns_via = holders[&e].clone();
                let m = self.mod_regs[&e] | self.may_write(e, &regions[&e], false);
                let n = self.touch_regs[&e] | self.may_write(e, &regions[&e], true);
                if self.mod_regs[&e] != m || self.touch_regs[&e] != n {
                    self.mod_regs.insert(e, m);
                    self.touch_regs.insert(e, n);
                    changed = true;
                }
            }
            if !changed {
                break;
            }
        }
        loop {
            let mut changed = false;
            for &e in &entries {
                self.returns_via = holders[&e].clone();
                let m = self.must_set(e, &regions[&e]);
                if self.must_flags[&e] != m {
                    self.must_flags.insert(e, m);
                    changed = true;
                }
            }
            if !changed {
                break;
            }
        }
        loop {
            let mut changed = false;
            for &e in &entries {
                let r = &regions[&e];
                self.returns_via = holders[&e].clone();
                let (live_in, live_out) = self.liveness(r, e);
                let reads = live_in.get(&e).map_or(0xF, |l| l.flags);
                let old = self.entry_flags.get(&e).copied().unwrap_or(0);
                if old | reads != old {
                    self.entry_flags.insert(e, old | reads);
                    changed = true;
                }
                let reads = live_in.get(&e).map_or(ALL_INT, |l| l.int);
                let old = self.ref_regs.get(&e).copied().unwrap_or(0);
                if old | reads != old {
                    self.ref_regs.insert(e, old | reads);
                    changed = true;
                }
                let mut grow = |m: &mut HashMap<u32, u8>, t: u32, f: u8| {
                    let old = m.get(&t).copied().unwrap_or(0);
                    if old | f != old {
                        m.insert(t, old | f);
                        changed = true;
                    }
                };
                let grow16 = |m: &mut HashMap<u32, u16>, t: u32, f: u16| {
                    let old = m.get(&t).copied().unwrap_or(0);
                    if old | f != old {
                        m.insert(t, old | f);
                        true
                    } else {
                        false
                    }
                };
                let mut grew = false;
                for &a in r {
                    if let Flow::Call(t) = self.flow(a) {
                        let f = live_out.get(&a).map_or(0xF, |l| l.flags);
                        grow(&mut self.ret_flags, t, f);
                        let g = live_out.get(&a).map_or(ALL_INT, |l| l.int);
                        grew |= grow16(&mut self.ret_regs, t, g);
                    }
                    for s in self.successors(a).0 {
                        let inside = r.contains(&s) && (s == e || !self.entries.contains_key(&s));
                        if !inside && self.entries.contains_key(&s) {
                            let f = self.ret_flags_of(e);
                            grow(&mut self.ret_flags, s, f);
                            let g = self.ret_regs_of(e);
                            grew |= grow16(&mut self.ret_regs, s, g);
                        }
                    }
                }
                changed |= grew;
            }
            if !changed {
                break;
            }
        }
        self.returns_via.clear();
    }

    /// Works out signatures. A routine qualifies if only BL reaches it, it
    /// is no other routine's tail, it makes no tail call itself, and its
    /// registers are all known (no SWI or call may read or write any of
    /// them). It takes the registers it reads as parameters. It gives back,
    /// through pointers, the ones it may write that a caller reads after.
    /// Its callers pass their locals, so the state block is not used.
    fn signatures(&mut self) {
        let entries: Vec<u32> = self.entries.keys().copied().collect();
        let mut barred: BTreeSet<u32> = self.external.clone();
        barred.extend(self.sp_escape.iter().copied());
        for &e in &entries {
            let r = self.region(e);
            self.returns_via = self.lr_holders(e, &r);
            for &a in &r {
                if matches!(self.flow(a), Flow::Indirect { .. } | Flow::Unknown(_) | Flow::Fault(_)) {
                    barred.insert(e);
                }
                // A call whose return address can be jumped back to later
                // needs a resume point. Only a call through the state
                // block provides one, so its callee gets no signature.
                if let Flow::Call(t) = self.flow(a) {
                    if self.lr_escape.contains(&self.call_back(a)) {
                        barred.insert(t);
                    }
                }
                for s in self.successors(a).0 {
                    let inside = r.contains(&s) && (s == e || !self.entries.contains_key(&s));
                    if !inside && self.entries.contains_key(&s) {
                        barred.insert(s);
                        barred.insert(e);
                    }
                }
            }
        }
        // Frames: what each routine gives back as it found it. Start from
        // the best case for all, and reduce to what can be proved.
        let best = Frames { balanced: true, keeps: true, preserved: ALL_INT };
        let mut facts: HashMap<u32, Frames> = entries.iter().map(|&e| (e, best)).collect();
        loop {
            let mut changed = false;
            for &e in &entries {
                let r = self.region(e);
                self.returns_via = self.lr_holders(e, &r);
                let mut sp_at = HashMap::new();
                let f = self.frame(e, &r, &facts, &mut sp_at);
                if facts[&e] != f {
                    facts.insert(e, f);
                    changed = true;
                }
                if f.balanced {
                    self.sp_at.insert(e, sp_at);
                } else {
                    self.sp_at.remove(&e);
                }
            }
            if !changed {
                break;
            }
        }
        self.returns_via.clear();
        self.frames = facts;
        self.peeking();
        for &e in &entries {
            if barred.contains(&e) || self.touch_regs.get(&e).copied().unwrap_or(ALL_INT) == ALL_INT {
                continue;
            }
            // The caller still has whatever comes back as it went: sp, and
            // what the routine saves and restores.
            let outs = self.mod_regs_of(e) & self.ret_regs_of(e) & ALL_INT;
            self.sig.insert(e, (self.reads_of(e) & !outs & ALL_INT, outs));
        }
    }

    /// One instruction's effect on the frame, if it runs. None if it does
    /// something the analysis cannot follow. `keeps` is set false for a
    /// store above the entry sp, into a caller's frame.
    fn frame_step(&self, a: u32, f: &Fr, facts: &HashMap<u32, Frames>, keeps: &mut bool) -> Option<Fr> {
        const SP: u16 = 1 << 13;
        let d = self.code[&a].d;
        let mut g = f.clone();
        let (uses, defs) = self.explicit_int(a);
        match self.flow(a) {
            Flow::Call(t) => {
                let ff = facts.get(&t).copied().unwrap_or(Frames::NONE);
                if !ff.balanced {
                    return None;
                }
                if !ff.keeps {
                    g.slots.clear();
                }
                let m = self.mod_regs.get(&t).copied().unwrap_or(ALL_INT) & !ff.preserved;
                g.entry &= !((m | LR) & !SP);
                return Some(g);
            }
            Flow::Indirect { .. } | Flow::Unknown(_) | Flow::Fault(_) => return None,
            _ if self.is_call(a) => {
                // A SWI keeps sp, and what its definition says it keeps.
                let out = self.swi_io(a).map_or(ALL_INT, |(_, o)| o | 1);
                g.entry &= !(out & !SP);
                return Some(g);
            }
            _ => {}
        }
        // sp may be used as a base, or moved by a constant, and nothing
        // else. If sp were read as a value, an address in the frame could
        // escape.
        let base_only = match d.insn {
            Insn::Mem { rn: 13, rt, offset, .. } => rt != 13 && !matches!(offset, Offset::Reg { rm: 13, .. }),
            Insn::Block { rn: 13, regs, .. } => regs & SP == 0,
            Insn::Dp { op: DpOp::Add | DpOp::Sub, rd: 13, rn: 13, op2: Operand2::Imm { .. }, .. } => true,
            _ => uses & SP == 0,
        };
        if !base_only {
            return None;
        }
        let bit = |r: u8| if r < 15 { 1u16 << r } else { 0 };
        match d.insn {
            Insn::Block { load, rn: 13, regs, before, add, wback, user } => {
                if user {
                    return None;
                }
                let n = regs.count_ones() as i32;
                let mut at = f.sp
                    + match (before, add) {
                        (false, true) => 0,
                        (true, true) => 4,
                        (false, false) => 4 - 4 * n,
                        (true, false) => -4 * n,
                    };
                for i in 0..16u8 {
                    if regs >> i & 1 == 0 {
                        continue;
                    }
                    if load {
                        if f.slots.get(&at) == Some(&i) {
                            g.entry |= bit(i);
                        } else {
                            g.entry &= !bit(i);
                        }
                    } else {
                        if at + 4 > 0 {
                            *keeps = false;
                        }
                        if i < 15 && f.entry & bit(i) != 0 {
                            g.slots.insert(at, i);
                        } else {
                            g.slots.remove(&at);
                        }
                    }
                    at += 4;
                }
                if wback {
                    g.sp = f.sp + if add { 4 * n } else { -4 * n };
                }
            }
            Insn::Mem { load, width, rt, rn: 13, offset, add, pre, wback } => {
                let Offset::Imm(k) = offset else { return None };
                let k = k as i32 * if add { 1 } else { -1 };
                let at = f.sp + if pre { k } else { 0 };
                let size = match width {
                    Width::Word => 4,
                    Width::Double => 8,
                    Width::Half | Width::SignedHalf => 2,
                    Width::Byte | Width::SignedByte => 1,
                };
                let regs: Vec<u8> = if width == Width::Double { vec![rt, rt + 1] } else { vec![rt] };
                if load {
                    for (j, &r) in regs.iter().enumerate() {
                        let whole = size >= 4 && at % 4 == 0;
                        if whole && f.slots.get(&(at + 4 * j as i32)) == Some(&r) {
                            g.entry |= bit(r);
                        } else {
                            g.entry &= !bit(r);
                        }
                    }
                } else {
                    if at + size > 0 {
                        *keeps = false;
                    }
                    // Every word the store touches.
                    let mut w = at.div_euclid(4) * 4;
                    while w < at + size {
                        g.slots.remove(&w);
                        w += 4;
                    }
                    if size >= 4 && at % 4 == 0 {
                        for (j, &r) in regs.iter().enumerate() {
                            if r < 15 && f.entry & bit(r) != 0 {
                                g.slots.insert(at + 4 * j as i32, r);
                            }
                        }
                    }
                }
                if wback || !pre {
                    g.sp = f.sp + k;
                }
            }
            Insn::Dp { op, rd: 13, rn: 13, op2: Operand2::Imm { value, .. }, .. } => {
                g.sp = f.sp + value as i32 * if op == DpOp::Add { 1 } else { -1 };
            }
            _ => {
                if defs & SP != 0 {
                    return None;
                }
                // A store through any other register is not to the frame,
                // because nothing outside the routine has a pointer into it.
                g.entry &= !defs;
            }
        }
        // What is below sp is free, and anything may write there.
        if g.sp > f.sp {
            let sp = g.sp;
            g.slots.retain(|&o, _| o >= sp);
        }
        if g.sp == 0 {
            g.entry |= SP;
        } else {
            g.entry &= !SP;
        }
        Some(g)
    }

    /// The frame of the routine at `entry`, followed forwards. It tracks
    /// sp's offset from where it started, which registers still hold what
    /// they held then, and which words of the stack hold which of those.
    /// A conditional instruction splits the state in two. An instruction
    /// on the same condition, or its opposite, resolves the split, as in
    /// `LDMNEFD sp!, {..}` then `BNE`, or then `MOVNE pc, lr`. Anything
    /// that may set the flags merges the two states again.
    fn frame(&self, entry: u32, r: &BTreeSet<u32>, facts: &HashMap<u32, Frames>, sp_at: &mut HashMap<u32, i32>) -> Frames {
        let mut keeps = true;
        let mut preserved = ALL_INT;
        let start = Fr { sp: 0, entry: ALL_INT, slots: BTreeMap::new() };
        let mut before: HashMap<u32, St> = HashMap::from([(entry, St::At(start))]);
        let mut work = vec![entry];
        let fail = Frames::NONE;
        while let Some(a) = work.pop() {
            let d = self.code[&a].d;
            let st = before[&a].clone();
            let step = |f: &Fr, keeps: &mut bool| self.frame_step(a, f, facts, keeps);
            // The state where the instruction runs and where it does not,
            // and how to write the state after it.
            let (ran, skipped, after) = match (&st, d.cond) {
                (St::At(f), Cond::Al) => {
                    sp_at.insert(a, f.sp);
                    let Some(g) = step(f, &mut keeps) else { return fail };
                    (g.clone(), None, St::At(g))
                }
                (St::At(f), c) => {
                    sp_at.insert(a, f.sp);
                    let Some(g) = step(f, &mut keeps) else { return fail };
                    (g.clone(), Some(f.clone()), St::Split(c, g, f.clone()))
                }
                (St::Split(c, h, n), dc) if *c == dc => {
                    sp_at.insert(a, h.sp);
                    let Some(g) = step(h, &mut keeps) else { return fail };
                    (g.clone(), Some(n.clone()), St::Split(*c, g, n.clone()))
                }
                (St::Split(c, h, n), dc) if lift::invert(*c) == dc => {
                    sp_at.insert(a, n.sp);
                    let Some(g) = step(n, &mut keeps) else { return fail };
                    (g.clone(), Some(h.clone()), St::Split(*c, h.clone(), g))
                }
                (St::Split(_, h, n), dc) => {
                    let Some(m) = h.merge(n) else { return fail };
                    sp_at.insert(a, m.sp);
                    let Some(g) = step(&m, &mut keeps) else { return fail };
                    if dc == Cond::Al {
                        (g.clone(), None, St::At(g))
                    } else {
                        (g.clone(), Some(m.clone()), St::Split(dc, g, m))
                    }
                }
            };
            // A flag set here changes what a later condition means.
            let sets = self.is_call(a)
                || matches!(d.insn, Insn::Dp { s: true, .. } | Insn::Mul { s: true, .. } | Insn::Msr { .. })
                || matches!(d.insn, Insn::Dp { op: DpOp::Cmp | DpOp::Cmn | DpOp::Tst | DpOp::Teq, .. })
                || lift::fp_flags_set(&d.insn) != 0;
            // The exception is an instruction that runs only where the
            // condition held, and sets flags under which it still holds,
            // as `MSREQ CPSR_f, #Z_bit + V_bit` before `MOVEQ pc, lr`.
            let still = match (&after, d.insn) {
                (St::Split(c, ..), Insn::Msr { spsr: false, mask, src: MsrSrc::Imm(v) }) if d.cond == *c && mask & 8 != 0 => {
                    cond_holds(*c, v >> 28)
                }
                _ => false,
            };
            let after = match after {
                St::Split(_, h, n) if sets && !still => match h.merge(&n) {
                    Some(m) => St::At(m),
                    None => return fail,
                },
                s => s,
            };
            if matches!(self.flow(a), Flow::Return) {
                if ran.sp != 0 {
                    return fail;
                }
                preserved &= ran.entry;
            }
            let transfer = !matches!(self.flow(a), Flow::Next | Flow::Call(_)) && !self.is_call(a);
            let fall = a.wrapping_add(4);
            for s in self.successors(a).0 {
                let inside = r.contains(&s) && (s == entry || !self.entries.contains_key(&s));
                if !inside {
                    if !self.entries.contains_key(&s) {
                        continue; // not code, so a fault
                    }
                    // A tail call returns on this routine's behalf.
                    let ff = facts.get(&s).copied().unwrap_or(Frames::NONE);
                    if ran.sp != 0 || !ff.balanced {
                        return fail;
                    }
                    keeps &= ff.keeps;
                    let m = self.mod_regs.get(&s).copied().unwrap_or(ALL_INT) & !ff.preserved;
                    preserved &= ran.entry & !m;
                    continue;
                }
                let next = if transfer && s != fall {
                    St::At(ran.clone())
                } else if transfer {
                    match &skipped {
                        Some(k) => St::At(k.clone()),
                        None => St::At(ran.clone()),
                    }
                } else {
                    after.clone()
                };
                let merged = match before.get(&s) {
                    None => Some(next),
                    Some(old) => old.join(&next),
                };
                let Some(merged) = merged else { return fail };
                if before.get(&s) != Some(&merged) {
                    before.insert(s, merged);
                    work.push(s);
                }
            }
        }
        Frames { balanced: true, keeps, preserved }
    }

    /// Makes C structs of the storage maps that the code uses as record
    /// layouts. A map counts if one of its fields is the address of a load
    /// or store. Each field becomes a member, packed at the offset the map
    /// gives. A map is left alone if a name cannot be a C member, or if its
    /// fields do not follow one another.
    fn records(&mut self) {
        let mut offsets: BTreeSet<&str> = BTreeSet::new();
        for w in self.code.values() {
            let Some(src) = &w.src else { continue };
            let code = src.text.split(';').next().unwrap_or("");
            let mut words = code.split_whitespace();
            let op = if code.starts_with(char::is_whitespace) { words.next() } else { words.nth(1) };
            if op.is_some_and(|o| ["LDR", "STR"].iter().any(|m| o.to_ascii_uppercase().starts_with(m))) {
                for n in named_immediates(&src.text) {
                    if !code.contains(&format!("={n}")) {
                        offsets.insert(n);
                    }
                }
            }
        }
        let fields: BTreeSet<&str> = self.inp.maps.iter().flat_map(|m| m.fields.iter().map(|f| f.0.as_str())).collect();
        let mut tags: BTreeSet<String> = BTreeSet::new();
        for (i, m) in self.inp.maps.iter().enumerate() {
            let real: Vec<&(String, u32, u32)> = m.fields.iter().filter(|f| f.2 > 0).collect();
            if real.is_empty() || !real.iter().any(|f| offsets.contains(f.0.as_str())) {
                continue;
            }
            let follows = m.fields.first().is_some_and(|f| f.1 == m.start)
                && m.fields.windows(2).all(|w| w[1].1 == w[0].1.wrapping_add(w[0].2));
            let end = m.fields.last().map_or(0, |f| f.1 as u64 + f.2 as u64);
            if !follows || end > 0x10_0000 || !m.fields.iter().all(|f| nameable(&f.0, &self.inp.name)) {
                continue;
            }
            // The tag is the prefix the fields' names share, such as
            // `buffer_` in buffer_Handle and buffer_Flags. A workspace on
            // r12 is `ws`.
            let mut prefix = real[0].0.as_str();
            for f in &real[1..] {
                let n = prefix.chars().zip(f.0.chars()).take_while(|(a, b)| a == b).count();
                prefix = &prefix[..n];
            }
            let prefix = prefix.rfind('_').map_or("", |k| &prefix[..k]);
            let base = if real.len() >= 2 && prefix.len() >= 2 {
                prefix.to_string()
            } else if m.base == Some(12) {
                "ws".to_string()
            } else {
                format!("map_{}", real[0].0)
            };
            // A tag is also the accessor's name, so nothing else may have
            // it. Two letters, such as `ws` or `ms`, are enough: no
            // constant that short is a macro, and no local but `ea` is
            // that short.
            let free = |t: &str| {
                !tags.contains(t)
                    && !fields.contains(t)
                    && !(self.inp.constants.contains_key(t) && nameable(t, &self.inp.name))
                    && (nameable(t, &self.inp.name) || (t.len() == 2 && c_safe(t)))
            };
            let mut tag = base.clone();
            let mut k = 2;
            while !free(&tag) {
                tag = format!("{base}_{k}");
                k += 1;
            }
            tags.insert(tag.clone());
            for f in &real {
                self.members.insert(f.0.clone(), (tag.clone(), f.2));
            }
            self.records.push(Record { tag, map: i });
        }
    }

    /// A record's C: the struct, and the accessor from an address.
    fn record_c(&self, r: &Record) -> String {
        let m = &self.inp.maps[r.map];
        let on = m.base.map_or(String::new(), |b| format!(", on r{b}"));
        let mut s = format!("/* The storage map at {}:{}{on}. */\nstruct {} {{\n", m.file, m.line, r.tag);
        if m.start > 0 {
            s.push_str(&format!("    uint8_t before[{}];\n", m.start));
        }
        for (name, _, size) in &m.fields {
            let decl = match size {
                0 => continue,
                1 => format!("uint8_t {name}"),
                2 => format!("uint16_t {name}"),
                4 => format!("uint32_t {name}"),
                n => format!("uint8_t {name}[{n}]"),
            };
            s.push_str(&format!("    {decl};\n"));
        }
        s.push_str("} __attribute__((packed, may_alias));\n");
        s.push_str(&format!(
            "static inline struct {0} *{0}(uint32_t a) {{ return (struct {0} *)ros_ptr(a); }}\n\n",
            r.tag
        ));
        s
    }

    /// The stack that an instruction reads or writes through sp: whether
    /// it loads, each word's offset from the current sp, and the width.
    /// None if it is not a load or store based on sp.
    fn sp_access(&self, a: u32) -> Option<(bool, Vec<i32>, i32)> {
        match self.code[&a].d.insn {
            Insn::Block { load, rn: 13, regs, before, add, .. } => {
                let n = regs.count_ones() as i32;
                let low = match (before, add) {
                    (false, true) => 0,
                    (true, true) => 4,
                    (false, false) => 4 - 4 * n,
                    (true, false) => -4 * n,
                };
                Some((load, (0..n).map(|k| low + 4 * k).collect(), 4))
            }
            Insn::Mem { load, width, rn: 13, offset: Offset::Imm(k), add, pre, .. } => {
                let k = if !pre { 0 } else if add { k as i32 } else { -(k as i32) };
                let (words, size) = match width {
                    Width::Word => (vec![k], 4),
                    Width::Double => (vec![k, k + 4], 4),
                    Width::Half | Width::SignedHalf => (vec![k], 2),
                    Width::Byte | Width::SignedByte => (vec![k], 1),
                };
                Some((load, words, size))
            }
            _ => None,
        }
    }

    /// Finds which routines touch their callers' frames. A routine does so
    /// directly, by an access at or above the entry sp, or through a
    /// callee that does. Any routine whose frame could not be followed is
    /// counted too.
    fn peeking(&mut self) {
        let entries: Vec<u32> = self.entries.keys().copied().collect();
        let mut peeks: BTreeSet<u32> = BTreeSet::new();
        let mut calls: HashMap<u32, Vec<u32>> = HashMap::new();
        for &e in &entries {
            let Some(sp_at) = self.sp_at.get(&e) else {
                peeks.insert(e);
                continue;
            };
            let r = self.region(e);
            for &a in &r {
                if let (Some(&sp), Some((_, words, size))) = (sp_at.get(&a), self.sp_access(a)) {
                    if words.iter().any(|&k| sp + k + size > 0) {
                        peeks.insert(e);
                    }
                }
                if let Flow::Call(t) = self.flow(a) {
                    calls.entry(e).or_default().push(t);
                }
                for s in self.successors(a).0 {
                    let inside = r.contains(&s) && (s == e || !self.entries.contains_key(&s));
                    if !inside && self.entries.contains_key(&s) {
                        calls.entry(e).or_default().push(s);
                    }
                }
            }
        }
        loop {
            let mut changed = false;
            for &e in &entries {
                if !peeks.contains(&e) && calls.get(&e).is_some_and(|c| c.iter().any(|t| peeks.contains(t))) {
                    peeks.insert(e);
                    changed = true;
                }
            }
            if !changed {
                break;
            }
        }
        self.peeks = peeks;
    }

    /// Whether the frame of the routine at `entry` can be C locals. It can
    /// if:
    ///
    /// - its frame was followed;
    /// - every access below the entry sp is a whole word within 512 bytes;
    /// - nothing it calls touches its frame;
    /// - and no word is read before it is written.
    ///
    /// If so, returns sp at each instruction, and the words live after and
    /// before each.
    fn slot_plan(
        &self,
        entry: u32,
        r: &BTreeSet<u32>,
        live_out: &HashMap<u32, Live>,
    ) -> Option<SlotPlan> {
        // What a return gives back: its results, or else everything it
        // leaves in the state block.
        let back = self.gives_back(entry);
        let sp_at = self.sp_at.get(&entry)?;
        let slot = |sp: i32, k: i32| -> Option<u32> {
            let o = sp + k;
            ((-512..0).contains(&o) && o % 4 == 0).then(|| (-o / 4) as u32)
        };
        let mut io: HashMap<u32, (u128, u128)> = HashMap::new();
        for &a in r {
            match self.flow(a) {
                Flow::Call(t) if self.peeks.contains(&t) => return None,
                _ => {}
            }
            for s in self.successors(a).0 {
                let inside = r.contains(&s) && (s == entry || !self.entries.contains_key(&s));
                if !inside && self.peeks.contains(&s) {
                    return None;
                }
            }
            let Some((load, words, size)) = self.sp_access(a) else { continue };
            let sp = *sp_at.get(&a)?;
            // The registers the words go to, in order.
            let dest: Vec<u8> = match self.code[&a].d.insn {
                Insn::Block { regs, .. } => (0..16u8).filter(|k| regs >> k & 1 != 0).collect(),
                Insn::Mem { rt, width: Width::Double, .. } => vec![rt, rt + 1],
                Insn::Mem { rt, .. } => vec![rt],
                _ => vec![],
            };
            let ret = matches!(self.flow(a), Flow::Return);
            let after = live_out.get(&a).map_or(ALL_INT, |l| l.int);
            let (mut u, mut d) = (0u128, 0u128);
            for (j, k) in words.into_iter().enumerate() {
                if sp + k >= 0 {
                    continue; // a caller's frame, which stays in memory
                }
                let i = slot(sp, k).filter(|_| size == 4)?;
                if load {
                    // It counts as a read only if what it loads is used.
                    let rx = dest.get(j).copied().unwrap_or(15);
                    let used = rx == 15 || if ret { back >> rx & 1 != 0 } else { after >> rx & 1 != 0 };
                    if used {
                        u |= 1 << (i - 1);
                    }
                } else if self.code[&a].d.cond == Cond::Al {
                    d |= 1 << (i - 1);
                }
            }
            io.insert(a, (u, d));
        }
        // Work backwards to find the words live after each instruction.
        let mut live_in: HashMap<u32, u128> = HashMap::new();
        let mut after: HashMap<u32, u128> = HashMap::new();
        loop {
            let mut changed = false;
            for &a in r.iter().rev() {
                let mut o = 0u128;
                for s in self.successors(a).0 {
                    if r.contains(&s) && (s == entry || !self.entries.contains_key(&s)) {
                        o |= live_in.get(&s).copied().unwrap_or(0);
                    }
                }
                let (u, d) = io.get(&a).copied().unwrap_or((0, 0));
                let i = u | (o & !d);
                if live_in.get(&a) != Some(&i) {
                    live_in.insert(a, i);
                    changed = true;
                }
                after.insert(a, o);
            }
            if !changed {
                break;
            }
        }
        if live_in.get(&entry).copied().unwrap_or(0) != 0 {
            return None;
        }
        Some((sp_at.clone(), after, live_in))
    }

    /// The C for the frame word at offset `k` from sp, at the instruction
    /// at `a`, if the frame is held in locals. For example, `sp_12`.
    fn slot_text(&self, a: u32, k: i32) -> Option<String> {
        let (sp_at, _, _) = self.region_slots.as_ref()?;
        let o = sp_at.get(&a)? + k;
        ((-512..0).contains(&o) && o % 4 == 0).then(|| format!("sp_{}", -o))
    }

    /// A routine's C parameters, which follow the state pointer `s`.
    fn params_decl(&self, e: u32) -> String {
        let Some(&(ins, outs)) = self.sig.get(&e) else { return String::new() };
        (0..15u8)
            .filter(|n| (ins | outs) >> n & 1 != 0)
            .map(|n| if outs >> n & 1 != 0 { format!(", uint32_t *p{n}") } else { format!(", uint32_t r{n}") })
            .collect()
    }

    /// The instructions of the region at `entry`.
    fn region(&self, entry: u32) -> BTreeSet<u32> {
        let mut out = BTreeSet::new();
        let mut todo = vec![entry];
        while let Some(a) = todo.pop() {
            if !self.is_code(a) || (a != entry && self.entries.contains_key(&a)) || !out.insert(a) {
                continue;
            }
            todo.extend(self.successors(a).0);
        }
        out
    }

    // ---- C ------------------------------------------------------------

    /// Control reaching `t` from the instruction at `a`, inside the region
    /// `r`. A tail call first stores the registers that go with it.
    fn transfer(&self, r: &BTreeSet<u32>, entry: u32, a: u32, t: u32) -> String {
        if r.contains(&t) && (t == entry || !self.entries.contains_key(&t)) {
            // A jump backwards may close a loop, and every loop has one.
            // So with --poll-loops it is a safe point for the runtime's
            // background work.
            if t <= a && self.inp.poll_loops {
                format!("{{ ROS_POLL(s, {}); goto {}; }}", self.rg(13), self.label(t))
            } else {
                format!("goto {};", self.label(t))
            }
        } else if self.entries.contains_key(&t) {
            let out = self.dirty_out(a) & (self.reads_of(t) | self.ret_regs_of(entry));
            format!("{{ {}ROS_TAIL_CALL({}) }}", self.stores_text(out).map_or(String::new(), |s| s + " "), self.function(t))
        } else {
            format!("ros_fault(s, 0x{t:08X}u, \"a transfer to an address that is not code\");")
        }
    }

    /// An instruction's C as a piece of a block: its lines, and how it
    /// ends the block. `pre` is what the lifter put before it. `body` is
    /// `pre` plus the instruction's own statements. None if it cannot be
    /// structured, such as a goto inside a statement, or a jump table case
    /// that is in another routine.
    #[allow(clippy::too_many_arguments)]
    fn item(
        &self,
        r: &BTreeSet<u32>,
        entry: u32,
        a: u32,
        pre: Vec<String>,
        body: Vec<String>,
        falls: bool,
        cond_inv: &HashMap<u32, String>,
    ) -> Option<Item> {
        let internal = |t: u32| r.contains(&t) && (t == entry || !self.entries.contains_key(&t));
        let d = self.code[&a].d;
        let comment = self.comment(a);
        let next = a.wrapping_add(4);
        let fall = || if internal(next) { Tgt::At(next) } else { Tgt::Out(self.transfer(r, entry, a, next)) };
        let (cond, inv) = if d.cond == Cond::Al {
            (String::new(), String::new())
        } else {
            (
                self.cond_override.get(&a).cloned().unwrap_or_else(|| Self::cond_expr(d.cond).into()),
                cond_inv.get(&a).cloned().unwrap_or_else(|| Self::cond_expr(lift::invert(d.cond)).into()),
            )
        };
        let plain = |v: Vec<String>| v.into_iter().map(|code| Line { code, comment: String::new() }).collect::<Vec<_>>();
        match self.flow(a) {
            Flow::Jump(t) if internal(t) => {
                let mut lines = plain(pre);
                let always = d.cond == Cond::Al || cond == "1";
                if cond == "0" && internal(next) {
                    // Never taken, so it falls through.
                    lines.push(Line { code: String::new(), comment });
                    return Some(Item { addr: a, lead: vec![], lines, end: Some(End::Goto(next)), plain: false });
                }
                let end = if always {
                    // The structure now does the branch's work, but its
                    // source line stays.
                    lines.push(Line { code: String::new(), comment });
                    End::Goto(t)
                } else {
                    End::If { cond, inv, then: Tgt::At(t), else_: fall(), comment, then_pre: vec![], else_pre: vec![] }
                };
                return Some(Item { addr: a, lead: vec![], lines, end: Some(end), plain: false });
            }
            Flow::Table { rm, shift, first, count, data } => {
                let cases: Vec<u32> = self.table_targets(a, shift, first, count, data).into_iter().map(|c| c.1).collect();
                if !cases.iter().all(|&t| internal(t)) {
                    return None;
                }
                let on = if first < 0 { format!("(int32_t){}", self.rg(rm)) } else { self.rg(rm) };
                let default = format!("ros_fault(s, 0x{a:08X}u, \"a jump table index out of range\");");
                let end = if d.cond == Cond::Al {
                    End::Table { on, first, cases, default, comment }
                } else {
                    End::If {
                        cond,
                        inv,
                        then: Tgt::Table { on, first, cases, default },
                        else_: fall(),
                        comment,
                        then_pre: vec![],
                        else_pre: vec![],
                    }
                };
                return Some(Item { addr: a, lead: vec![], lines: plain(pre), end: Some(end), plain: false });
            }
            _ => {}
        }
        let code_only = |s: &String| s.split("/*").next().unwrap_or("").contains("goto ");
        if body.iter().any(code_only) {
            return None;
        }
        let mut lines = plain(body);
        match lines.first_mut() {
            Some(l) => l.comment = comment,
            None => lines.push(Line { code: String::new(), comment }),
        }
        let end = if !falls {
            Some(End::Exit)
        } else if internal(next) {
            None
        } else {
            lines.push(Line { code: self.transfer(r, entry, a, next), comment: String::new() });
            Some(End::Exit)
        };
        let sets_flags = match d.insn {
            Insn::Dp { s, op, .. } => s || matches!(op, DpOp::Cmp | DpOp::Cmn | DpOp::Tst | DpOp::Teq),
            Insn::Mul { s, .. } => s,
            Insn::Msr { .. } => true,
            ref i => lift::fp_flags_set(i) != 0,
        };
        let plain = matches!(self.flow(a), Flow::Next) && !self.is_call(a) && !sets_flags;
        Some(Item { addr: a, lead: vec![], lines, end, plain })
    }

    /// A routine's items, cut into blocks and structured. Returns the
    /// body's C, or None if its flow graph is not reducible, and so cannot
    /// be made into ifs and loops.
    fn structured(&self, mut items: Vec<Item>, starts: &BTreeSet<u32>, entry: u32) -> Option<String> {
        merge_conditions(&mut items, starts);
        // Blocks start at labels and after anything that ends one.
        let mut first: Vec<usize> = vec![];
        for (i, it) in items.iter().enumerate() {
            if i == 0 || starts.contains(&it.addr) || items[i - 1].end.is_some() {
                first.push(i);
            }
        }
        let index: HashMap<u32, usize> = first.iter().enumerate().map(|(b, &i)| (items[i].addr, b)).collect();
        let mut blocks: Vec<structure::Block> = vec![];
        let mut extra: Vec<structure::Block> = vec![];
        let n = first.len();
        // Makes a branch's target into a block where it is not one. That
        // is where it goes out of the routine, into a jump table, or
        // through code moved into the arm.
        let tgt = |t: Tgt, mut lines: Vec<Line>, extra: &mut Vec<structure::Block>, label: &str| -> Option<usize> {
            let term = match t {
                Tgt::At(a) if lines.is_empty() => return index.get(&a).copied(),
                Tgt::At(a) => structure::Term::Goto(*index.get(&a)?),
                Tgt::Out(code) => {
                    lines.push(Line { code, comment: String::new() });
                    structure::Term::Exit
                }
                Tgt::Table { on, first, cases, default } => {
                    let cases = cases.iter().map(|a| index.get(a).copied()).collect::<Option<Vec<_>>>()?;
                    structure::Term::Switch { on, first: first as i64, cases, default, comment: String::new() }
                }
            };
            extra.push(structure::Block { label: format!("{label}_{}", extra.len()), lines, term });
            Some(n + extra.len() - 1)
        };
        let mut items = items.into_iter().enumerate().peekable();
        for b in 0..n {
            let end_at = first.get(b + 1).copied().unwrap_or(usize::MAX);
            let mut lines = vec![];
            let mut last = None;
            let mut addr0 = None;
            while let Some((_, it)) = items.next_if(|(i, _)| *i < end_at) {
                addr0.get_or_insert(it.addr);
                lines.extend(it.lead);
                lines.extend(it.lines);
                last = Some((it.addr, it.end));
            }
            let (la, end) = last?;
            let label = self.label(addr0?);
            let term = match end {
                None => structure::Term::Goto(*index.get(&la.wrapping_add(4))?),
                Some(End::Exit) => structure::Term::Exit,
                Some(End::Goto(t)) => structure::Term::Goto(*index.get(&t)?),
                Some(End::If { cond, inv, then, else_, comment, then_pre, else_pre }) => structure::Term::If {
                    cond,
                    inv,
                    then: tgt(then, then_pre, &mut extra, &label)?,
                    else_: tgt(else_, else_pre, &mut extra, &label)?,
                    comment,
                },
                Some(End::Table { on, first, cases, default, comment }) => structure::Term::Switch {
                    on,
                    first: first as i64,
                    cases: cases.iter().map(|a| index.get(a).copied()).collect::<Option<Vec<_>>>()?,
                    default,
                    comment,
                },
            };
            blocks.push(structure::Block { label, lines, term });
        }
        blocks.extend(extra);
        let start = *index.get(&entry)?;
        let (blocks, start) = reachable(blocks, start);
        let poll = self.inp.poll_loops.then(|| format!("ROS_POLL(s, {});", self.rg(13)));
        let lines = structure::structure_with(&blocks, start, poll.as_deref())?;
        Some(lines.into_iter().map(|l| l + "\n").collect())
    }

    fn compile_region(&mut self, entry: u32, out: &mut String) -> usize {
        let r = self.region(entry);
        self.returns_via.clear();
        self.returns_via = self.lr_holders(entry, &r);
        // Labels: every target that is reached other than by falling into
        // it.
        let mut targets: BTreeSet<u32> = BTreeSet::new();
        for &a in &r {
            match self.flow(a) {
                Flow::Jump(t) => {
                    targets.insert(t);
                }
                Flow::Table { shift, first, count, data, .. } => {
                    for (_, t) in self.table_targets(a, shift, first, count, data) {
                        targets.insert(t);
                    }
                }
                _ => {}
            }
            for s in self.successors(a).0 {
                if s != a.wrapping_add(4) {
                    targets.insert(s);
                }
            }
        }
        if r.first() != Some(&entry) {
            targets.insert(entry);
        }

        let src = self.code[&entry].src.clone();
        let why = self.entries[&entry].clone();
        // What the author wrote before the routine's first line explains
        // the routine.
        let about: String = self.notes(entry).iter().map(|l| format!("{l}\n")).collect();
        let head = format!(
            "/* ---- {}: {}{} ---- */\n\n{about}static void {}(struct ros_cpu *s{})\n{{\n",
            self.names.get(&entry).cloned().unwrap_or_else(|| format!("&{entry:08X}")),
            why,
            src.map(|s| format!(", {}:{}", s.file, s.line)).unwrap_or_default(),
            self.function(entry),
            self.params_decl(entry)
        );
        let whole = out;
        let mut body_out = String::new();
        let out = &mut body_out;
        if r.first() != Some(&entry) {
            out.push_str(&format!("    goto {};\n", self.label(entry)));
        }
        let addrs: Vec<u32> = r.iter().copied().collect();
        let (mut live_in, mut live_out) = self.liveness(&r, entry);
        // A private frame's words are locals, and are live where they are
        // read.
        self.region_slots = if self.inp.lift { self.slot_plan(entry, &r, &live_out) } else { None };
        if let Some((_, after, before)) = &self.region_slots {
            for (a, l) in live_out.iter_mut() {
                l.slots = after.get(a).copied().unwrap_or(0);
            }
            for (a, l) in live_in.iter_mut() {
                l.slots = before.get(a).copied().unwrap_or(0);
            }
        } else {
            for l in live_out.values_mut().chain(live_in.values_mut()) {
                l.slots = 0;
            }
        }
        // The registers this routine names live in C locals (tier 1). They
        // are loaded from the state block where it starts. They are stored
        // to it where a call, a SWI or a return needs them. In between,
        // they are only locals.
        self.region_sig = self.sig.get(&entry).copied();
        let params = self.region_sig.map_or(0, |(i, o)| i | o);
        self.locals = if self.inp.lift {
            (r.iter().fold(0u16, |m, &a| {
                let (u, d) = self.explicit_int(a);
                m | u | d
            }) | params)
                & ALL_INT
        } else {
            0
        };
        self.region_entry = entry;
        self.region_live_out = live_out.clone();
        let (read_in, read_after) = self.read_liveness(&r, entry);
        self.read_after = read_after;
        self.written = r.iter().fold(0u16, |m, &a| m | self.local_defs(a)) & self.locals;
        // A result that a callee may change is kept up to date in its
        // local, for the return to give back.
        if let Some((_, outs)) = self.region_sig {
            self.written |= outs & self.locals;
        }
        self.dirty = self.dirty_analysis(&r, entry);
        self.restore = match self.region_sig {
            Some((_, outs)) => self.state_writes(&r) & !outs & !self.mod_regs_of(entry) & ALL_INT,
            None => 0,
        };
        let fpa_lift_ok = !addrs.iter().any(|a| {
            matches!(self.code[a].d.insn, Insn::FpaStatus { write: true, control: false, .. })
        });
        self.cond_override.clear();
        let upper = self.inp.name.to_ascii_uppercase();
        let mut next_temp = 1u32;
        // When lifted, the routine is also cut into blocks, to be
        // structured into ifs and loops. Where that cannot be done, the
        // labels and gotos stay. Tier 0 keeps them, as the reference.
        let mut items: Vec<Item> = vec![];
        let mut structurable = self.inp.lift;
        let mut cond_inv: HashMap<u32, String> = HashMap::new();
        let mut temps: Vec<String> = vec![];
        let mut i = 0;
        while i < addrs.len() {
            // A block is straight-line code. It starts at a label, or where
            // the last block ended. It runs to an unconditional transfer, a
            // call, or the next label. Conditional transfers are exits from
            // it.
            let first = i;
            while !self.ends_block(addrs[i])
                && i + 1 < addrs.len()
                && addrs[i + 1] == addrs[i].wrapping_add(4)
                && !targets.contains(&addrs[i + 1])
            {
                i += 1;
            }
            let last = i;
            i += 1;
            let blk: Vec<lift::BlockInsn> = addrs[first..=last]
                .iter()
                .map(|&a| lift::BlockInsn { addr: a, d: self.code[&a].d, caller: self.caller_info(a, &live_in, &live_out, &r, entry) })
                .collect();
            let la = addrs[last];
            let falls_out =
                !(self.is_call(la) || (!matches!(self.flow(la), Flow::Next) && self.code[&la].d.cond == Cond::Al));
            let rom = |x: u32| {
                let off = x.checked_sub(self.inp.base)? as usize;
                let b = self.image.get(off..off + 4)?;
                Some((u32::from_le_bytes([b[0], b[1], b[2], b[3]]), self.relocated.contains(&x)))
            };
            let label = |x: u32| self.names.get(&x).filter(|n| c_safe(n)).map(|n| format!("{upper}_{n}"));
            let konst = |a: u32, k: u32| self.konst_name(a, k);
            let field = |n: &str| self.members.get(n).cloned();
            let slot = |a: u32, k: i32| {
                let (sp_at, _, _) = self.region_slots.as_ref()?;
                let o = sp_at.get(&a)? + k;
                ((-512..0).contains(&o) && o % 4 == 0).then(|| (-o / 4) as u8)
            };
            let ctx = lift::BlockCtx {
                rom_word: &rom,
                label: &label,
                konst_name: &konst,
                field: &field,
                slot: &slot,
                falls_out,
                live_out: live_out.get(&la).copied().unwrap_or(Live::ALL),
                fpa_lift_ok,
                locals: self.locals,
            };
            let lifted = match lift::lift(&blk, &ctx, &mut next_temp) {
                Ok(o) => Some(o),
                Err(e) => {
                    self.errors.push(format!("{}: {e}", self.where_(addrs[first])));
                    None
                }
            };
            if let Some(l) = &lifted {
                self.cond_override.extend(l.cond.clone());
                cond_inv.extend(l.cond_inv.clone());
                temps.extend(l.decls.iter().cloned());
            }
            for (k, b) in blk.iter().enumerate() {
                let a = b.addr;
                let index = first + k;
                let pre = lifted.as_ref().and_then(|l| l.at.get(&a).cloned()).unwrap_or_default();
                let mut body = pre.clone();
                if b.caller.is_some() || lifted.is_none() {
                    body.extend(self.compile(&r, entry, a));
                }
                let notes = if a == entry { vec![] } else { self.notes(a) };
                for l in &notes {
                    out.push_str(&format!("    {l}\n"));
                }
                if targets.contains(&a) {
                    // C11 allows only a statement after a label. So write
                    // `L: ;` where a declaration, or nothing, follows.
                    let bare = body.first().is_none_or(|s| is_decl(s));
                    out.push_str(&format!("{}:{}\n", self.label(a), if bare { " ;" } else { "" }));
                }
                let comment = self.comment(a);
                let mut lines = body.clone().into_iter();
                match lines.next() {
                    Some(first_line) => {
                        let pad = if first_line.len() >= 44 { " " } else { "" };
                        out.push_str(&format!("    {first_line:<44}{pad}{comment}\n"));
                        for l in lines {
                            out.push_str(&format!("    {l}\n"));
                        }
                    }
                    // The instruction was folded into an expression below,
                    // or is dead. Its source line stays.
                    None => out.push_str(&format!("    {:<44}{comment}\n", "")),
                }
                // Whether control falls out of the instruction. It may go on
                // in the region, into another region's entry, or into
                // something that is not code.
                let next = a.wrapping_add(4);
                let falls = match self.flow(a) {
                    Flow::Next => true,
                    Flow::Call(_) => self.call_back(a) == next || self.code[&a].d.cond != Cond::Al,
                    Flow::Indirect { cont } => cont == Some(next) || self.code[&a].d.cond != Cond::Al,
                    Flow::Jump(_) | Flow::Table { .. } | Flow::Return => self.code[&a].d.cond != Cond::Al,
                    Flow::Unknown(_) => false,
                    Flow::Fault(_) => self.code[&a].d.cond != Cond::Al,
                };
                if falls && addrs.get(index + 1) != Some(&next) {
                    out.push_str(&format!("    {}\n", self.transfer(&r, entry, a, next)));
                }
                if structurable {
                    match self.item(&r, entry, a, pre, body, falls, &cond_inv) {
                        Some(mut it) => {
                            it.lead = notes.into_iter().map(|code| Line { code, comment: String::new() }).collect();
                            items.push(it)
                        }
                        None => structurable = false,
                    }
                }
            }
        }
        if structurable {
            let mut starts = targets.clone();
            starts.insert(entry);
            if let Some(text) = self.structured(items, &starts, entry) {
                body_out = text;
            }
        }
        // First, remove the frame words that it writes and never reads.
        if self.region_slots.is_some() {
            body_out = drop_dead_slots(&body_out);
        }
        // The locals the code uses, loaded where the routine starts if it
        // reads them before writing them.
        let used = used_locals(&body_out) & self.locals;
        let coming_in = read_in | (live_in.get(&entry).map_or(ALL_INT, |l| l.int) & self.written);
        // With a signature, what comes in is the parameters. They come by
        // value, or through the pointer by which the result goes back.
        let (ins, outs) = self.region_sig.unwrap_or((0, 0));
        let decls: Vec<String> = (0..15u8)
            .filter(|n| used >> n & 1 != 0 && ins >> n & 1 == 0)
            .map(|n| match (outs >> n & 1 != 0, coming_in >> n & 1 != 0) {
                (true, true) => format!("r{n} = *p{n}"),
                (false, true) => format!("r{n} = R[{n}]"),
                _ => format!("r{n}"),
            })
            .collect();
        whole.push_str(&head);
        if self.restore != 0 {
            let v: Vec<String> =
                (0..15u8).filter(|n| self.restore >> n & 1 != 0).map(|n| format!("was{n} = R[{n}]")).collect();
            whole.push_str(&format!("    const uint32_t {};\n", v.join(", ")));
        }
        if !decls.is_empty() {
            whole.push_str(&format!("    uint32_t {};\n", decls.join(", ")));
        }
        let mut words: BTreeSet<u32> = BTreeSet::new();
        for w in body_out.split(|c: char| !(c.is_ascii_alphanumeric() || c == '_')) {
            if let Some(k) = w.strip_prefix("sp_").and_then(|k| k.parse::<u32>().ok()) {
                words.insert(k);
            }
        }
        if !words.is_empty() {
            let v: Vec<String> = words.iter().map(|k| format!("sp_{k}")).collect();
            whole.push_str(&format!("    uint32_t {};\n", v.join(", ")));
        }
        let named: BTreeSet<&str> = body_out.split(|c: char| !(c.is_ascii_alphanumeric() || c == '_')).collect();
        let unused: Vec<String> = (0..15u8)
            .filter(|n| used >> n & 1 == 0 && (ins | outs) >> n & 1 != 0 && !named.contains(format!("p{n}").as_str()))
            .map(|n| if outs >> n & 1 != 0 { format!("(void)p{n};") } else { format!("(void)r{n};") })
            .collect();

        // The lifter's variables, by type.
        let mut by_type: BTreeMap<&str, Vec<&str>> = BTreeMap::new();
        for d in &temps {
            if let Some((ty, name)) = d.rsplit_once(' ') {
                by_type.entry(ty).or_default().push(name);
            }
        }
        for (ty, names) in by_type {
            whole.push_str(&format!("    {ty} {};\n", names.join(", ")));
        }
        if !unused.is_empty() {
            whole.push_str(&format!("    {}\n", unused.join(" ")));
        }
        whole.push_str(&body_out);
        whole.push_str("}\n\n");
        self.locals = 0;
        self.region_sig = None;
        self.region_slots = None;
        r.len()
    }

    // ---- registers in locals: what goes out, what comes back ----

    /// A register as the code reads it: its local, or its place in the
    /// state block.
    fn rg(&self, n: u8) -> String {
        if self.locals >> n & 1 != 0 {
            format!("r{n}")
        } else {
            format!("R[{n}]")
        }
    }

    /// For a call or an exit: the registers it needs in the state block,
    /// which must be stored there if their locals are dirty. For a call,
    /// also those to read back after it.
    fn sync_sets(&self, a: u32) -> (u16, u16) {
        match self.flow(a) {
            Flow::Call(t) if self.sig.contains_key(&t) => return (0, 0),
            Flow::Return if self.region_sig.is_some() => return (0, 0),
            _ => {}
        }
        let after = self.region_live_out.get(&a).map_or(ALL_INT, |l| l.int);
        let read = self.read_after.get(&a).copied().unwrap_or(ALL_INT) | (after & self.written);
        match self.flow(a) {
            // Store what the callee reads, and what it may write that is
            // read after. The latter goes in the state block in case the
            // callee does not write it. The BL writes lr itself.
            Flow::Call(t) => {
                let m = self.mod_regs_of(t) | LR;
                ((self.reads_of(t) | (m & after)) & !LR, m & read & self.locals)
            }
            _ if self.swi_io(a).is_some() => {
                let (i, o) = self.swi_io(a).unwrap();
                (i | (o & after), o & read & self.locals)
            }
            _ if self.is_call(a) => (ALL_INT, read & self.locals),
            Flow::Return => (self.ret_regs_of(self.region_entry) & !self.explicit_int(a).1, 0),
            Flow::Indirect { cont: None } | Flow::Unknown(_) => (ALL_INT, 0),
            // A fault reads nothing, because nothing runs after it.
            Flow::Fault(_) => (0, 0),
            _ => (0, 0),
        }
    }

    /// Registers the code reads before writing them, at the region's entry
    /// and after each instruction. This is a backward analysis in which
    /// only the instructions' own operands count as reads. What a call or
    /// an exit needs, it reads from the state block. A register the code
    /// has not written since the entry is already there.
    fn read_liveness(&self, r: &BTreeSet<u32>, entry: u32) -> (u16, HashMap<u32, u16>) {
        let mut before: HashMap<u32, u16> = HashMap::new();
        let mut after: HashMap<u32, u16> = HashMap::new();
        loop {
            let mut changed = false;
            for &a in r.iter().rev() {
                let mut o = 0u16;
                for s in self.successors(a).0 {
                    if r.contains(&s) && (s == entry || !self.entries.contains_key(&s)) {
                        o |= before.get(&s).copied().unwrap_or(0);
                    }
                }
                let (mut u, d) = self.explicit_int(a);
                if let Flow::Call(t) = self.flow(a) {
                    if let Some(&(ins, outs)) = self.sig.get(&t) {
                        u |= ins | outs;
                    }
                }
                // When it does not run, a conditional instruction leaves its
                // locals as they were. So it counts as reading them, where
                // they are read after.
                let al = self.code[&a].d.cond == Cond::Al;
                let u = if al { u } else { u | (self.local_defs(a) & o) };
                let kill = if al && !self.is_call(a) { d } else { 0 };
                let i = u | (o & !kill);
                if before.get(&a) != Some(&i) {
                    before.insert(a, i);
                    changed = true;
                }
                after.insert(a, o);
            }
            if !changed {
                break;
            }
        }
        (before.get(&entry).copied().unwrap_or(0), after)
    }

    /// Locals that may hold a value the state block does not, after an
    /// instruction, on the way to the next.
    fn dirty_after(&self, a: u32, before: u16) -> u16 {
        if let Flow::Call(t) = self.flow(a) {
            // The BL's lr, and what comes back through the pointers.
            if let Some(&(_, outs)) = self.sig.get(&t) {
                return before | ((outs | LR) & self.locals);
            }
        }
        if self.is_call(a) {
            if self.code[&a].d.cond != Cond::Al {
                return before;
            }
            let (store, reload) = self.sync_sets(a);
            return before & !store & !reload;
        }
        before | (self.local_defs(a) & self.locals)
    }

    /// Registers an instruction writes to their locals. None for a
    /// transfer or a call, whose writes go to the state block, except that
    /// a call to a routine with a signature writes its results and lr.
    fn local_defs(&self, a: u32) -> u16 {
        if let Flow::Call(t) = self.flow(a) {
            if let Some(&(_, outs)) = self.sig.get(&t) {
                return outs | LR;
            }
        }
        if self.is_call(a) || !matches!(self.flow(a), Flow::Next) {
            0
        } else {
            self.explicit_int(a).1
        }
    }

    /// Registers the region may write in the state block before it
    /// returns. These are what it stores there for a call, a SWI or an
    /// exit; what its instructions write there directly, for registers
    /// that are not locals; and what the calls and SWIs themselves may
    /// write.
    fn state_writes(&self, r: &BTreeSet<u32>) -> u16 {
        let mut w = 0u16;
        for &a in r {
            if matches!(self.flow(a), Flow::Return) {
                continue;
            }
            let (store, _) = self.sync_sets(a);
            w |= store & self.dirty.get(&a).copied().unwrap_or(ALL_INT);
            match self.flow(a) {
                Flow::Call(t) if self.sig.contains_key(&t) => {}
                Flow::Call(t) => w |= self.mod_regs_of(t) | LR,
                _ if self.swi_io(a).is_some() => w |= self.swi_io(a).unwrap().1,
                _ if self.is_call(a) => w |= ALL_INT,
                _ => w |= self.explicit_int(a).1 & !self.locals,
            }
        }
        w
    }

    fn dirty_out(&self, a: u32) -> u16 {
        self.dirty_after(a, self.dirty.get(&a).copied().unwrap_or(ALL_INT))
    }

    /// Before each instruction, the locals that may differ from the state
    /// block. This is a forward analysis: where two paths meet, a local
    /// may differ if it may on either.
    fn dirty_analysis(&self, r: &BTreeSet<u32>, entry: u32) -> HashMap<u32, u16> {
        let params = self.region_sig.map_or(0, |(i, o)| i | o);
        let mut before: HashMap<u32, u16> = HashMap::from([(entry, params & self.locals)]);
        let mut work = vec![entry];
        while let Some(a) = work.pop() {
            let out = self.dirty_after(a, before[&a]);
            for s in self.successors(a).0 {
                if r.contains(&s) && (s == entry || !self.entries.contains_key(&s)) {
                    let merged = before.get(&s).map_or(out, |&old| old | out);
                    if before.get(&s) != Some(&merged) {
                        before.insert(s, merged);
                        work.push(s);
                    }
                }
            }
        }
        before
    }

    /// `R[4] = r4; R[5] = r5;`: stores locals to the state block.
    fn stores_text(&self, regs: u16) -> Option<String> {
        let v: Vec<String> =
            (0..15u8).filter(|n| (regs & self.locals) >> n & 1 != 0).map(|n| format!("R[{n}] = r{n};")).collect();
        (!v.is_empty()).then(|| v.join(" "))
    }

    /// `r0 = R[0]; r1 = R[1];`: reads locals back from the state block
    /// after a call.
    fn reloads(&self, a: u32) -> Vec<String> {
        let (_, reload) = self.sync_sets(a);
        let v: Vec<String> = (0..15u8).filter(|n| reload >> n & 1 != 0).map(|n| format!("r{n} = R[{n}];")).collect();
        if v.is_empty() { vec![] } else { vec![v.join(" ")] }
    }

    // ---- flow analysis: what each instruction reads and writes ----------

    /// Whether an instruction ends a block. An unconditional transfer does,
    /// and so does a call, which may read or write anything.
    fn ends_block(&self, a: u32) -> bool {
        self.is_call(a)
            || match self.flow(a) {
                Flow::Next => false,
                Flow::Unknown(_) => true,
                _ => self.code[&a].d.cond == Cond::Al,
            }
    }

    /// What an instruction reads from the state and writes, if it is
    /// compiled here rather than lifted. That is a transfer, a call, a SWI,
    /// or any instruction under --no-lift. For a transfer, what it reads is
    /// what is live where it goes.
    fn caller_info(
        &self,
        a: u32,
        live_in: &HashMap<u32, Live>,
        live_out: &HashMap<u32, Live>,
        r: &BTreeSet<u32>,
        entry: u32,
    ) -> Option<lift::Caller> {
        let d = self.code[&a].d;
        let transfer = !matches!(self.flow(a), Flow::Next);
        let call = self.is_call(a);
        if !transfer && !call && lift::lifts(&d) && (self.inp.lift || lift::is_fp(&d.insn) || d.insn == Insn::Nop) {
            return None;
        }
        let (iu, idef) = self.int_use_def(a);
        let (fu, _) = self.fp_use_def(a);
        // A frame word live across it must be in its local first.
        let across = live_in.get(&a).map_or(0, |l| l.slots);
        let mut needs = Live { int: iu, fp: fu, flags: self.flags_other(a), slots: across };
        if transfer || call {
            for t in self.taken(a) {
                needs = needs.or(match t {
                    Some(t) if r.contains(&t) && (t == entry || !self.entries.contains_key(&t)) => {
                        live_in.get(&t).copied().unwrap_or(Live::ALL)
                    }
                    Some(t) if self.entries.contains_key(&t) => self.tail_live(t, entry),
                    // A transfer into something that is not code faults, so
                    // nothing is needed after it.
                    Some(t) if !self.is_code(t) => Live::default(),
                    _ => Live::ALL,
                });
            }
            // A return needs what its callers read. A call needs what its
            // callee reads, and what the callee may leave for what follows.
            match self.flow(a) {
                Flow::Return => {
                    needs.flags = self.ret_flags_of(entry);
                    needs.int = self.gives_back(entry) | self.explicit_int(a).0;
                    // If it pops only frame locals, and gives no sp back,
                    // it does not read sp.
                    let locals_only = self.sp_access(a).is_some_and(|(_, w, _)| w.iter().all(|&k| self.slot_text(a, k).is_some()));
                    if locals_only && self.gives_back(entry) & (1 << 13) == 0 {
                        needs.int &= !(1 << 13);
                    }
                }
                Flow::Call(t) => {
                    let after = live_out.get(&a).map_or(0xF, |l| l.flags);
                    needs.flags = self.entry_flags_of(t) | (after & !self.must_flags_of(t));
                    needs.int = live_in.get(&a).map_or(ALL_INT, |l| l.int);
                }
                // A SWI needs what it reads, and what is live after it that
                // it keeps, in the locals.
                _ if call => needs.int = live_in.get(&a).map_or(ALL_INT, |l| l.int),
                _ => {}
            }
            // A call comes back. Any other transfer of control leaves.
            let leaves = transfer && !call && !matches!(self.flow(a), Flow::Indirect { cont: Some(_) });
            return Some(lift::Caller { needs, writes: Live::default(), writes_memory: false, leaves });
        }
        let writes = Live { int: idef, fp: 0, flags: Self::flags_set(&d), slots: 0 };
        Some(lift::Caller { needs, writes, writes_memory: Self::writes_memory(&d), leaves: false })
    }

    /// Where a transfer may go, other than on to the next instruction.
    /// None stands for out of the region: a call, or an address known only
    /// at run time. A return, a fault or a plain instruction gives nothing.
    fn taken(&self, a: u32) -> Vec<Option<u32>> {
        match self.flow(a) {
            Flow::Next | Flow::Return | Flow::Fault(_) => vec![],
            Flow::Jump(t) => vec![Some(t)],
            Flow::Table { shift, first, count, data, .. } => {
                self.table_targets(a, shift, first, count, data).into_iter().map(|c| Some(c.1)).collect()
            }
            _ => vec![None],
        }
    }

    /// Integer registers an instruction reads, and writes when it runs.
    /// Calls and exits read them all, except that a SWI the typed API
    /// defines reads only its inputs, and a fault reads none.
    fn int_use_def(&self, a: u32) -> (u16, u16) {
        if let Some((i, _)) = self.swi_io(a) {
            return (i, 0);
        }
        if let Flow::Fault(ref w) = self.flow(a) {
            // A return to user mode reads its block's base.
            return if w == USER_RETURN { (self.explicit_int_base(a), 0) } else { (0, 0) };
        }
        if self.is_call(a) || matches!(self.flow(a), Flow::Return | Flow::Indirect { cont: None } | Flow::Unknown(_)) {
            return (ALL_INT, 0);
        }
        // A table's index is read, even if the instruction does not name
        // it, as with `MOV pc, lr` after `ADD lr, pc, rm`.
        if let Flow::Table { rm, .. } = self.flow(a) {
            let (u, d) = self.explicit_int(a);
            return (u | 1 << rm, d);
        }
        self.explicit_int(a)
    }

    /// A user return's base register, as a mask.
    fn explicit_int_base(&self, a: u32) -> u16 {
        self.user_return_at(a).map_or(0, |rn| 1u16 << rn)
    }

    /// The integer registers an instruction names, read and written.
    fn explicit_int(&self, a: u32) -> (u16, u16) {
        // A fault computes nothing. A return to user mode reads its base.
        if let Flow::Fault(ref w) = self.flow(a) {
            return if w == USER_RETURN { (self.explicit_int_base(a), 0) } else { (0, 0) };
        }
        const ALL: u16 = ALL_INT;
        let b = |r: u8| if r == 15 { 0 } else { 1u16 << r };
        let op2 = |o: Operand2| match o {
            Operand2::Reg { rm, shift: Shift::Reg(_, rs) } => b(rm) | b(rs),
            Operand2::Reg { rm, .. } => b(rm),
            Operand2::Imm { .. } => 0,
        };
        match self.code[&a].d.insn {
            Insn::Dp { op, rd, rn, op2: o, .. } => {
                ((if op.uses_rn() { b(rn) } else { 0 }) | op2(o), if op.is_test() { 0 } else { b(rd) })
            }
            Insn::Mul { op, rd, ra, rm, rs, .. } => {
                let acc = match op {
                    MulOp::Mla | MulOp::Mls => b(ra),
                    MulOp::Umlal | MulOp::Smlal => b(ra) | b(rd),
                    _ => 0,
                };
                let def = match op {
                    MulOp::Mul | MulOp::Mla | MulOp::Mls => b(rd),
                    _ => b(rd) | b(ra),
                };
                (b(rm) | b(rs) | acc, def)
            }
            Insn::Mem { load, width, rt, rn, offset, pre, wback, .. } => {
                let t = if width == Width::Double { b(rt) | b(rt + 1) } else { b(rt) };
                let u = b(rn) | if let Offset::Reg { rm, .. } = offset { b(rm) } else { 0 } | if load { 0 } else { t };
                ((u), (if wback || !pre { b(rn) } else { 0 }) | if load { t } else { 0 })
            }
            Insn::Block { load, rn, regs, wback, .. } => {
                let list = regs & ALL;
                (b(rn) | if load { 0 } else { list }, (if load { list } else { 0 }) | if wback { b(rn) } else { 0 })
            }
            Insn::Branch { link: true, .. } => (0, LR),
            Insn::Bx { link, rm } => (b(rm), if link { LR } else { 0 }),
            Insn::Mrs { rd, .. } => (0, b(rd)),
            Insn::Msr { src: MsrSrc::Reg(m), .. } => (b(m), 0),
            Insn::Clz { rd, rm } => (b(rm), b(rd)),
            Insn::MovHalf { top, rd, .. } => (if top { b(rd) } else { 0 }, b(rd)),
            Insn::Swp { rt, rt2, rn, .. } => (b(rt2) | b(rn), b(rt)),
            ref i => lift::fp_int_use_def(i),
        }
    }

    /// For a SWI that the typed API defines: the registers it reads, plus
    /// sp, which every thunk records. Also those it may write, plus R0,
    /// where an error comes back. None for anything else.
    fn swi_io(&self, a: u32) -> Option<(u16, u16)> {
        let Insn::Swi { number } = self.code[&a].d.insn else { return None };
        let (i, o) = self.inp.swi_regs.get(&(number & !X_BIT))?;
        // An error from a SWI called without X goes to the program's error
        // handler (ErrHandler). The kernel enters it with the caller's
        // R10-R12, so those are read too.
        let fg = if number & X_BIT == 0 { 7 << 10 } else { 0 };
        Some((i | fg | 1 << 13, o | 1))
    }

    fn is_call(&self, a: u32) -> bool {
        matches!(self.flow(a), Flow::Call(_) | Flow::Indirect { cont: Some(_) })
            || matches!(self.code[&a].d.insn, Insn::Swi { .. })
    }

    /// FP registers read and written. A call, a SWI and any exit read
    /// them all, except as APCS allows below.
    fn fp_use_def(&self, a: u32) -> (u128, u128) {
        let d = self.code[&a].d;
        let (u, def) = lift::fp_use_def(&d);
        // Under APCS, a call takes its arguments in f0-f3 and may clobber
        // them, and keeps f4-f7. At a return, f0 may be the result, and
        // f4-f7 are the caller's. Without APCS, anything may read
        // anything.
        const F0_3: u128 = 0x0F;
        const F0_4_7: u128 = 0xF1;
        let vfp_all = lift::ALL_FP & !0xFF;
        let call = self.is_call(a) && !matches!(d.insn, Insn::Swi { .. });
        match (self.inp.apcs, call, self.flow(a)) {
            (true, true, _) => (F0_3 | vfp_all, F0_3),
            (true, false, Flow::Return) => (F0_4_7 | vfp_all, 0),
            _ if self.is_call(a) || matches!(self.flow(a), Flow::Return | Flow::Indirect { cont: None } | Flow::Unknown(_)) => {
                (lift::ALL_FP, 0)
            }
            _ => (u, def),
        }
    }

    /// Integer registers an instruction's tier-0 code writes.
    fn int_defs(d: &a32::Decoded) -> u16 {
        let bit = |r: u8| 1u16 << r;
        match d.insn {
            Insn::Dp { op, rd, .. } if !op.is_test() => bit(rd),
            Insn::Mul { op: MulOp::Umull | MulOp::Umlal | MulOp::Smull | MulOp::Smlal, rd, ra, .. } => bit(rd) | bit(ra),
            Insn::Mul { rd, .. } => bit(rd),
            Insn::Mem { load, width, rt, rn, pre, wback, .. } => {
                let mut m = if wback || !pre { bit(rn) } else { 0 };
                if load {
                    m |= bit(rt);
                    if width == Width::Double {
                        m |= bit(rt + 1);
                    }
                }
                m
            }
            Insn::Block { load, rn, regs, wback, .. } => (if load { regs } else { 0 }) | if wback { bit(rn) } else { 0 },
            Insn::Branch { link: true, .. } | Insn::Bx { link: true, .. } => bit(14),
            Insn::Mrs { rd, .. } | Insn::Clz { rd, .. } | Insn::MovHalf { rd, .. } => bit(rd),
            Insn::Swp { rt, .. } => bit(rt),
            Insn::Swi { .. } | Insn::Unknown => 0xFFFF,
            ref i => lift::fp_int_use_def(i).1,
        }
    }

    fn writes_memory(d: &a32::Decoded) -> bool {
        match d.insn {
            Insn::Mem { load: false, .. } | Insn::Block { load: false, .. } | Insn::Swp { .. } => true,
            Insn::Swi { .. } | Insn::Unknown => true,
            ref i => lift::fp_writes_memory(i),
        }
    }

    /// NZCV, as bits 3-0, that a condition reads.
    fn cond_flags(c: Cond) -> u8 {
        const N: u8 = 8;
        const Z: u8 = 4;
        const C: u8 = 2;
        const V: u8 = 1;
        match c {
            Cond::Eq | Cond::Ne => Z,
            Cond::Cs | Cond::Cc => C,
            Cond::Mi | Cond::Pl => N,
            Cond::Vs | Cond::Vc => V,
            Cond::Hi | Cond::Ls => C | Z,
            Cond::Ge | Cond::Lt => N | V,
            Cond::Gt | Cond::Le => Z | N | V,
            Cond::Al => 0,
        }
    }

    /// NZCV an instruction reads other than through its condition.
    fn flags_other(&self, a: u32) -> u8 {
        let d = self.code[&a].d;
        if self.is_call(a) || matches!(self.flow(a), Flow::Return | Flow::Indirect { .. } | Flow::Unknown(_)) {
            return 0xF;
        }
        match d.insn {
            Insn::Dp { op: DpOp::Adc | DpOp::Sbc | DpOp::Rsc, .. } => 2,
            Insn::Dp { op2: Operand2::Reg { shift: Shift::Rrx, .. }, .. } => 2,
            Insn::Mrs { .. } => 0xF,
            Insn::FpaMonadic { .. } | Insn::FpaDyadic { .. } => 0,
            _ => 0,
        }
    }

    /// NZCV an instruction sets when it executes.
    fn flags_set(d: &a32::Decoded) -> u8 {
        match d.insn {
            Insn::Dp { op, s: true, op2, .. } => {
                if op.is_logical() {
                    let carry = match op2 {
                        Operand2::Imm { rotated, .. } => rotated,
                        Operand2::Reg { shift: Shift::Imm(ShiftType::Lsl, 0), .. } => false,
                        Operand2::Reg { shift: Shift::Reg(..), .. } => false, // C changes only sometimes
                        Operand2::Reg { .. } => true,
                    };
                    0xC | if carry { 2 } else { 0 }
                } else {
                    0xF
                }
            }
            Insn::Mul { s: true, .. } => 0xC,
            Insn::Msr { mask, .. } if mask & 8 != 0 => 0xF,
            ref i => lift::fp_flags_set(i),
        }
    }

    /// Liveness over the region: the integer registers, FP registers and
    /// flags live before and after each instruction. It works backwards,
    /// and repeats until nothing changes. An exit (a return, a tail call,
    /// or a transfer out of the region) reads everything, and so does a
    /// call, except where the summaries say otherwise.
    fn liveness(&self, r: &BTreeSet<u32>, entry: u32) -> (HashMap<u32, Live>, HashMap<u32, Live>) {
        let facts: HashMap<u32, LiveFacts> = r
            .iter()
            .map(|&a| {
                let d = self.code[&a].d;
                let al = d.cond == Cond::Al;
                let (iu, idef) = self.int_use_def(a);
                let (fu, fdef) = self.fp_use_def(a);
                let mut uses = Live { int: iu, fp: fu, flags: Self::cond_flags(d.cond) | self.flags_other(a), slots: 0 };
                let mut defs = Live {
                    int: if al { idef } else { 0 },
                    fp: fdef,
                    flags: if al { Self::flags_set(&d) } else { 0 },
                    slots: 0,
                };
                // A return passes on what its callers read. A call reads what
                // its callee does, and passes on what the callee may leave.
                match self.flow(a) {
                    Flow::Return => {
                        uses.flags = Self::cond_flags(d.cond) | self.ret_flags_of(entry);
                        uses.int = self.gives_back(entry) | self.explicit_int(a).0;
                    }
                    // The BL itself writes lr, which the callee reads.
                    Flow::Call(t) => {
                        uses.flags = Self::cond_flags(d.cond) | self.entry_flags_of(t);
                        defs.flags = if al { self.must_flags_of(t) } else { 0 };
                        uses.int = self.ref_regs_of(t) & !LR;
                        defs.int = if al { LR } else { 0 };
                    }
                    _ => {}
                }
                // Successors inside the region by address. Those outside it
                // by what is live there.
                let succ = self
                    .successors(a)
                    .0
                    .into_iter()
                    .map(|s| {
                        if r.contains(&s) && (s == entry || !self.entries.contains_key(&s)) {
                            Ok(s)
                        } else if self.entries.contains_key(&s) {
                            Err(self.tail_live(s, entry))
                        } else {
                            Err(Live::ALL)
                        }
                    })
                    .collect();
                (a, (uses, defs, succ))
            })
            .collect();
        let mut live_in: HashMap<u32, Live> = HashMap::new();
        let mut live_out: HashMap<u32, Live> = HashMap::new();
        loop {
            let mut changed = false;
            for &a in r.iter().rev() {
                let (uses, defs, succ) = &facts[&a];
                let mut o = Live::default();
                for s in succ {
                    o = o.or(match s {
                        Ok(s) => live_in.get(s).copied().unwrap_or_default(),
                        Err(l) => *l,
                    });
                }
                let i = uses.or(o.and_not(*defs));
                if live_in.get(&a) != Some(&i) {
                    live_in.insert(a, i);
                    changed = true;
                }
                live_out.insert(a, o);
            }
            if !changed {
                break;
            }
        }
        (live_in, live_out)
    }

    /// The name the source gave a constant the instruction at `a` uses,
    /// if it has value `k`.
    fn konst_name(&self, a: u32, k: u32) -> Option<String> {
        let src = self.code.get(&a)?.src.as_ref()?;
        named_immediates(&src.text)
            .into_iter()
            .find(|n| self.inp.constants.get(*n) == Some(&k) && nameable(n, &self.inp.name))
            .map(str::to_string)
    }

    /// The whole-line comments before an instruction, as C lines.
    fn notes(&self, a: u32) -> Vec<String> {
        match self.code.get(&a) {
            Some(Word { src: Some(s), first: true, .. }) => note_lines(&s.notes),
            _ => vec![],
        }
    }

    fn comment(&self, a: u32) -> String {
        let Some(w) = self.code.get(&a) else { return String::new() };
        let Some(src) = &w.src else { return format!("/* &{a:08X} */") };
        if !w.first {
            return "/*   (continued) */".into();
        }
        // The instruction without its label, then the comment after it.
        let mut t = src.text.as_str();
        if !t.starts_with(char::is_whitespace) {
            t = t.split_once(char::is_whitespace).map_or("", |x| x.1);
        }
        let mut quoted = false;
        let cut = t
            .char_indices()
            .find(|&(_, c)| {
                if c == '"' {
                    quoted = !quoted;
                }
                c == ';' && !quoted
            })
            .map_or(t.len(), |(i, _)| i);
        let insn = t[..cut].split_whitespace().collect::<Vec<_>>().join(" ");
        // The author's comment stays beside the instruction it explains.
        let note = t.get(cut + 1..).unwrap_or("").trim();
        let text = if note.is_empty() { insn } else { format!("{insn} ; {note}") };
        format!("/* {} */", text.replace("*/", "* /"))
    }

    fn cond_expr(c: Cond) -> &'static str {
        match c {
            Cond::Eq => "s->z",
            Cond::Ne => "!s->z",
            Cond::Cs => "s->c",
            Cond::Cc => "!s->c",
            Cond::Mi => "s->n",
            Cond::Pl => "!s->n",
            Cond::Vs => "s->v",
            Cond::Vc => "!s->v",
            Cond::Hi => "ros_cond(s, ROS_HI)",
            Cond::Ls => "ros_cond(s, ROS_LS)",
            Cond::Ge => "ros_cond(s, ROS_GE)",
            Cond::Lt => "ros_cond(s, ROS_LT)",
            Cond::Gt => "ros_cond(s, ROS_GT)",
            Cond::Le => "ros_cond(s, ROS_LE)",
            Cond::Al => "1",
        }
    }

    /// One instruction's C: its statements, under its condition.
    fn compile(&mut self, r: &BTreeSet<u32>, entry: u32, a: u32) -> Vec<String> {
        let d = self.code[&a].d;
        let mut body = match self.body(r, entry, a) {
            Ok(b) => b,
            Err(e) => {
                self.errors.push(format!("{}: {e}", self.where_(a)));
                vec![format!("ros_fault(s, 0x{a:08X}u, \"{e}\");")]
            }
        };
        // A routine with a signature returns its results through its
        // pointers. What the return itself loads goes straight there.
        if let (Some((_, outs)), Flow::Return) = (self.region_sig, self.flow(a)) {
            let own = self.explicit_int(a).1;
            let mut dead = vec![];
            for (i, l) in body.iter_mut().enumerate() {
                let lead = l.len() - l.trim_start().len();
                for n in 0..15u8 {
                    if let Some(rest) = l[lead..].strip_prefix(&format!("R[{n}] = ")) {
                        if outs >> n & 1 != 0 {
                            *l = format!("{}*p{n} = {rest}", &l[..lead]);
                        } else {
                            // No caller reads it, so there is nothing to
                            // give back.
                            dead.push(i);
                        }
                        break;
                    }
                }
            }
            for i in dead.into_iter().rev() {
                body.remove(i);
            }
            let give: Vec<String> =
                (0..15u8).filter(|n| (outs & !own) >> n & 1 != 0).map(|n| format!("*p{n} = r{n};")).collect();
            if !give.is_empty() {
                body.insert(0, give.join(" "));
            }
            let back: Vec<String> =
                (0..15u8).filter(|n| self.restore >> n & 1 != 0).map(|n| format!("R[{n}] = was{n};")).collect();
            if !back.is_empty() {
                body.insert(0, back.join(" "));
            }
        }
        // A return may pop a frame local into a register that no caller
        // reads after. There is nothing to give back, and the push that
        // would have filled the local was dead, so the pop goes.
        if self.region_slots.is_some() && matches!(self.flow(a), Flow::Return) {
            let back = self.gives_back(entry);
            body.retain(|l| {
                let s = l.trim_start();
                let dead = s.strip_prefix("R[").and_then(|x| x.split_once("] = sp_")).is_some_and(|(n, rest)| {
                    n.parse::<u8>().is_ok_and(|n| n < 15 && back >> n & 1 == 0)
                        && rest.trim_end_matches(';').bytes().all(|b| b.is_ascii_digit())
                });
                !dead
            });
        }
        // Remove a `uint32_t ea` that nothing reads any more.
        if let Some(i) = body.iter().position(|l| l.trim_start().starts_with("uint32_t ea = ")) {
            let uses = body
                .iter()
                .enumerate()
                .filter(|&(j, l)| j != i && l.split(|c: char| !(c.is_ascii_alphanumeric() || c == '_')).any(|w| w == "ea"))
                .count();
            if uses == 0 {
                body.remove(i);
            }
        }
        // First store what a call or an exit needs from the locals into the
        // state block.
        let (store, _) = self.sync_sets(a);
        let dirty = self.dirty.get(&a).copied().unwrap_or(ALL_INT);
        if let Some(s) = self.stores_text(store & dirty) {
            body.insert(0, s);
        }
        if d.cond == Cond::Al || body.is_empty() {
            return body;
        }
        let c = self.cond_override.get(&a).cloned().unwrap_or_else(|| Self::cond_expr(d.cond).to_string());
        let c = c.as_str();
        // A condition the lifter found constant: never true, or always.
        match c {
            "0" => return vec![],
            "1" => return body,
            _ => {}
        }
        if body.len() == 1 {
            return vec![format!("if ({c}) {}", body[0])];
        }
        let mut out = vec![format!("if ({c}) {{")];
        out.extend(body.into_iter().map(|l| format!("    {l}")));
        out.push("}".into());
        out
    }

    fn body(&self, r: &BTreeSet<u32>, entry: u32, a: u32) -> Result<Vec<String>, String> {
        let d = self.code[&a].d;
        let pc = a.wrapping_add(8);
        let reg = |n: u8| if n == 15 { format!("0x{pc:08X}u") } else { self.rg(n) };
        let ret = |v: String| vec![format!("R[15] = {v};"), "return;".to_string()];
        let block = |v: Vec<String>| -> Vec<String> {
            let mut w = vec!["{".to_string()];
            w.extend(v.into_iter().map(|l| format!("    {l}")));
            w.push("}".into());
            w
        };
        let indirect = |target: String| -> Vec<String> {
            // If the unit has any escaping return addresses, an indirect
            // transfer goes to the module's resume chain first. If the ARM
            // code pushed an lr and is jumping back to it, the C stack is
            // unwound to the frame that registered that address, which is
            // the one whose BL made it.
            let dispatch = if self.lr_escape.is_empty() {
                format!("ros_call(s, {target});")
            } else {
                format!("R[15] = {target}; ros_resume(s, {target});")
            };
            match self.flow(a) {
                Flow::Indirect { cont: Some(c) } => vec![
                    dispatch,
                    format!("ros_check_return(s, 0x{c:08X}u);"),
                ]
                .into_iter()
                .chain(self.reloads(a))
                .chain((c != a.wrapping_add(4)).then(|| self.transfer(r, entry, a, c)))
                .collect(),
                _ => vec![dispatch, "return;".to_string()],
            }
        };

        Ok(match d.insn {
            // Floating point is lifted a segment at a time (lift.rs).
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
            | Insn::NeonPadd { .. } => vec![],
            Insn::Nop => vec![],
            Insn::Unknown => return Err("an instruction the compiler does not model".into()),

            Insn::Dp { op, s, rd, rn, op2 } => {
                if rd == 15 {
                    return Ok(match self.flow(a) {
                        Flow::Return => match op2 {
                            Operand2::Reg { rm, .. } => ret(self.rg(rm)),
                            _ => ret(self.rg(14)),
                        },
                        Flow::Jump(t) => vec![self.transfer(r, entry, a, t)],
                        Flow::Table { rm, shift, first, count, data } => {
                            let on = if first < 0 { format!("(int32_t){}", self.rg(rm)) } else { self.rg(rm) };
                            let mut v = vec![format!("switch ({on}) {{")];
                            for (k, t) in self.table_targets(a, shift, first, count, data) {
                                v.push(format!("case {k}: {}", self.transfer(r, entry, a, t)));
                            }
                            v.push(format!(
                                "default: ros_fault(s, 0x{a:08X}u, \"a jump table index out of range\");"
                            ));
                            v.push("}".into());
                            v
                        }
                        Flow::Indirect { .. } => {
                            let (val, _, pre) = self.operand2(op2, a)?;
                            let n = reg(rn);
                            let t = match op {
                                DpOp::Mov => val,
                                DpOp::Add => format!("{n} + {val}"),
                                DpOp::Sub => format!("{n} - {val}"),
                                DpOp::Orr => format!("({n} | {val})"),
                                DpOp::Bic => format!("({n} & ~{val})"),
                                DpOp::And => format!("({n} & {val})"),
                                _ => return Err(format!("{op:?} into pc")),
                            };
                            let mut v = pre;
                            v.extend(indirect(t));
                            v
                        }
                        Flow::Unknown(why) => return Err(why),
                        Flow::Fault(why) => vec![format!("ros_fault(s, 0x{a:08X}u, \"{why}\");")],
                        f => return Err(format!("unexpected flow {f:?}")),
                    });
                }
                if let (DpOp::Add | DpOp::Sub, false, 15, Operand2::Imm { value, .. }) = (op, s, rn, op2) {
                    let t = if op == DpOp::Add { pc.wrapping_add(value) } else { pc.wrapping_sub(value) };
                    return Ok(vec![format!("R[{rd}] = 0x{t:08X}u;")]);
                }
                let (val, carry, mut pre) = self.operand2(op2, a)?;
                let n = reg(rn);
                let carry = carry.unwrap_or_else(|| "s->c".into());
                let dst = |e: String| format!("R[{rd}] = {e};");
                let stmt = match (op, s) {
                    (DpOp::And, false) => dst(format!("{n} & {val}")),
                    (DpOp::Eor, false) => dst(format!("{n} ^ {val}")),
                    (DpOp::Sub, false) => dst(format!("{n} - {val}")),
                    (DpOp::Rsb, false) => dst(format!("{val} - {n}")),
                    (DpOp::Add, false) => dst(format!("{n} + {val}")),
                    (DpOp::Adc, false) => dst(format!("{n} + {val} + s->c")),
                    (DpOp::Sbc, false) => dst(format!("{n} - {val} - (1u - s->c)")),
                    (DpOp::Rsc, false) => dst(format!("{val} - {n} - (1u - s->c)")),
                    (DpOp::Orr, false) => dst(format!("{n} | {val}")),
                    (DpOp::Mov, false) => dst(val),
                    (DpOp::Bic, false) => dst(format!("{n} & ~{val}")),
                    (DpOp::Mvn, false) => dst(format!("~{val}")),
                    (DpOp::And, true) => dst(format!("ros_logic(s, {n} & {val}, {carry})")),
                    (DpOp::Eor, true) => dst(format!("ros_logic(s, {n} ^ {val}, {carry})")),
                    (DpOp::Orr, true) => dst(format!("ros_logic(s, {n} | {val}, {carry})")),
                    (DpOp::Mov, true) => dst(format!("ros_logic(s, {val}, {carry})")),
                    (DpOp::Bic, true) => dst(format!("ros_logic(s, {n} & ~{val}, {carry})")),
                    (DpOp::Mvn, true) => dst(format!("ros_logic(s, ~{val}, {carry})")),
                    (DpOp::Tst, _) => format!("ros_logic(s, {n} & {val}, {carry});"),
                    (DpOp::Teq, _) => format!("ros_logic(s, {n} ^ {val}, {carry});"),
                    (DpOp::Sub, true) => dst(format!("ros_subs(s, {n}, {val})")),
                    (DpOp::Rsb, true) => dst(format!("ros_subs(s, {val}, {n})")),
                    (DpOp::Add, true) => dst(format!("ros_adds(s, {n}, {val})")),
                    (DpOp::Adc, true) => dst(format!("ros_adcs(s, {n}, {val})")),
                    (DpOp::Sbc, true) => dst(format!("ros_sbcs(s, {n}, {val})")),
                    (DpOp::Rsc, true) => dst(format!("ros_sbcs(s, {val}, {n})")),
                    (DpOp::Cmp, _) => format!("ros_subs(s, {n}, {val});"),
                    (DpOp::Cmn, _) => format!("ros_adds(s, {n}, {val});"),
                };
                if pre.is_empty() {
                    vec![stmt]
                } else {
                    pre.push(stmt);
                    let mut v = vec!["{".to_string()];
                    v.extend(pre.into_iter().map(|l| format!("    {l}")));
                    v.push("}".into());
                    v
                }
            }

            Insn::Mul { op, s, rd, ra, rm, rs } => {
                if [rd, ra, rm, rs].contains(&15) {
                    return Err("a multiply involving pc".into());
                }
                let (m, sr) = (self.rg(rm), self.rg(rs));
                let (d_, a_) = (self.rg(rd), self.rg(ra));
                let short = |e: String| {
                    let mut v = vec![format!("{d_} = {e};")];
                    if s {
                        v.push(format!("ros_nz(s, {d_});"));
                    }
                    v
                };
                let long = |p: String| {
                    let mut v = vec![
                        "{".to_string(),
                        format!("    uint64_t p = {p};"),
                        format!("    {a_} = (uint32_t)p;"),
                        format!("    {d_} = (uint32_t)(p >> 32);"),
                    ];
                    if s {
                        v.push("    s->n = (uint32_t)(p >> 63);".into());
                        v.push("    s->z = p == 0;".into());
                    }
                    v.push("}".into());
                    v
                };
                let acc = format!("((uint64_t){d_} << 32 | {a_})");
                match op {
                    MulOp::Mul => short(format!("{m} * {sr}")),
                    MulOp::Mla => short(format!("{m} * {sr} + {a_}")),
                    MulOp::Mls => short(format!("{a_} - {m} * {sr}")),
                    MulOp::Umull => long(format!("(uint64_t){m} * {sr}")),
                    MulOp::Umlal => long(format!("{acc} + (uint64_t){m} * {sr}")),
                    MulOp::Smull => long(format!("(uint64_t)((int64_t)(int32_t){m} * (int32_t){sr})")),
                    MulOp::Smlal => {
                        long(format!("{acc} + (uint64_t)((int64_t)(int32_t){m} * (int32_t){sr})"))
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
                let sign = if add { "+" } else { "-" };
                let off: Option<String> = match offset {
                    Offset::Imm(0) => None,
                    Offset::Imm(i) if i < 4096 => Some(format!("{i}")),
                    Offset::Imm(i) => Some(format!("0x{i:X}")),
                    Offset::Reg { rm: 15, .. } => return Err("a register offset of pc".into()),
                    Offset::Reg { rm, shift } => {
                        let (v, _, p) = self.operand2(Operand2::Reg { rm, shift }, a)?;
                        if !p.is_empty() {
                            return Err("a register-shifted-register offset".into());
                        }
                        Some(v)
                    }
                };
                let base = reg(rn);
                // The offset address. It is a constant when it is
                // pc-relative.
                let offset_addr = match (rn, offset) {
                    (15, Offset::Imm(i)) => {
                        format!("0x{:08X}u", if add { pc.wrapping_add(i) } else { pc.wrapping_sub(i) })
                    }
                    _ => off.as_ref().map_or(base.clone(), |o| format!("{base} {sign} {o}")),
                };
                let mut v: Vec<String> = vec![];
                let mut after: Vec<String> = vec![];
                let ea = if !writes_back {
                    offset_addr
                } else if matches!(offset, Offset::Reg { .. }) {
                    // Work it out first, because a load may overwrite the
                    // offset register.
                    v.push(format!("uint32_t ea = {offset_addr};"));
                    after.push(format!("R[{rn}] = ea;"));
                    if pre { "ea".to_string() } else { base.clone() }
                } else {
                    if let Some(o) = &off {
                        let upd = format!("R[{rn}] {sign}= {o};");
                        if pre { v.push(upd) } else { after.push(upd) }
                    }
                    base.clone()
                };
                let at4 = format!("{ea} + 4");
                match (load, width) {
                    (true, Width::Word) if rt == 15 => {
                        // A load from a private frame's word reads its
                        // local.
                        let from = self
                            .sp_access(a)
                            .and_then(|(_, w, _)| self.slot_text(a, w[0]))
                            .unwrap_or_else(|| format!("ros_ld32({ea})"));
                        v.push(format!("uint32_t t = {from};"))
                    }
                    (true, Width::Word) => v.push(format!("R[{rt}] = ros_ld32({ea});")),
                    (true, Width::Byte) => v.push(format!("R[{rt}] = ros_ld8({ea});")),
                    (true, Width::Half) => v.push(format!("R[{rt}] = ros_ld16({ea});")),
                    (true, Width::SignedByte) => {
                        v.push(format!("R[{rt}] = (uint32_t)(int32_t)(int8_t)ros_ld8({ea});"))
                    }
                    (true, Width::SignedHalf) => {
                        v.push(format!("R[{rt}] = (uint32_t)(int32_t)(int16_t)ros_ld16({ea});"))
                    }
                    (true, Width::Double) => {
                        v.push(format!("R[{rt}] = ros_ld32({ea});"));
                        v.push(format!("R[{}] = ros_ld32({at4});", rt + 1));
                    }
                    (false, Width::Word) => v.push(format!("ros_st32({ea}, {});", reg(rt))),
                    (false, Width::Byte) => v.push(format!("ros_st8({ea}, {});", reg(rt))),
                    (false, Width::Half) => v.push(format!("ros_st16({ea}, {});", reg(rt))),
                    (false, Width::Double) => {
                        v.push(format!("ros_st32({ea}, R[{rt}]);"));
                        v.push(format!("ros_st32({at4}, R[{}]);", rt + 1));
                    }
                    (false, _) => return Err("a signed store".into()),
                }
                // When pc is loaded, the writeback goes to the state block,
                // computed from the base's local.
                if load && rt == 15 && self.locals != 0 {
                    for l in after.iter_mut() {
                        if let Some(rest) = l.strip_prefix(&format!("R[{rn}] ")) {
                            if let Some(op) = rest.strip_suffix(';').and_then(|x| x.split_once("= ")) {
                                if matches!(op.0.trim(), "+" | "-") {
                                    *l = format!("R[{rn}] = {} {} {};", self.rg(rn), op.0.trim(), op.1);
                                }
                            }
                        }
                    }
                }
                v.extend(after);
                if load && rt == 15 {
                    match self.flow(a) {
                        Flow::Return => v.extend(ret("t".into())),
                        Flow::Indirect { .. } => v.extend(indirect("t".into())),
                        f => return Err(format!("unexpected flow {f:?}")),
                    }
                }
                if v.iter().any(|l| l.starts_with("uint32_t")) {
                    block(v)
                } else {
                    v
                }
            }

            Insn::Block { load: true, rn, user: true, .. } if matches!(self.flow(a), Flow::Fault(ref w) if w == USER_RETURN) => {
                vec![format!("ros_user_return(s, {});", self.rg(rn)), "return;".to_string()]
            }
            Insn::Block { load, rn, regs, before, add, wback, user } => {
                // Without `pc`, the user bank is the current bank, in the
                // only mode compiled code runs in. With `pc`, the LDM
                // restores a mode. That is 26-bit mode's return, which a
                // `TEQ pc, pc` guard stops from running. It compiles to a
                // fault in case it does run, as for MOVS pc.
                if user && regs & 0x8000 != 0 {
                    if let (true, Flow::Fault(why)) = (load, self.flow(a)) {
                        return Ok(vec![format!("ros_fault(s, 0x{a:08X}u, \"{why}\");")]);
                    }
                    return Err("an LDM of the user bank restoring pc and the mode (^)".into());
                }
                if regs == 0 || rn == 15 {
                    return Err("an LDM or STM with no registers, or based on pc".into());
                }
                let n = regs.count_ones();
                if wback && regs & (1 << rn) != 0 && (load || regs & ((1u16 << rn) - 1) != 0) {
                    return Err("an LDM or STM that writes back to a register it transfers".into());
                }
                // The lowest address transferred, and the base written back.
                let (start, back) = match (before, add) {
                    (false, true) => (String::new(), format!("ea + {}", 4 * n)),
                    (true, true) => (" + 4".to_string(), format!("ea + {}", 4 * n - 4)),
                    (false, false) => (format!(" - {}", 4 * n - 4), "ea - 4".to_string()),
                    (true, false) => (format!(" - {}", 4 * n), "ea".to_string()),
                };
                let start = if start == " - 0" { String::new() } else { start };
                let pc_loaded = load && regs & 0x8000 != 0;
                let mut v = vec![format!("uint32_t ea = {}{start};", self.rg(rn))];
                if pc_loaded {
                    v.push("uint32_t t;".into());
                }
                let mut k = 0;
                let words = self.sp_access(a).map(|(_, w, _)| w);
                for i in 0..16u8 {
                    if regs & (1 << i) == 0 {
                        continue;
                    }
                    let at = if k == 0 { "ea".to_string() } else { format!("ea + {}", 4 * k) };
                    // A private frame's word is its local.
                    let word = words.as_ref().and_then(|w| self.slot_text(a, w[k]));
                    let from = word.clone().unwrap_or_else(|| format!("ros_ld32({at})"));
                    v.push(match (load, i) {
                        (true, 15) => format!("t = {from};"),
                        (true, _) => format!("R[{i}] = {from};"),
                        (false, _) => match &word {
                            Some(w) => format!("{w} = {};", reg(i)),
                            None => format!("ros_st32({at}, {});", reg(i)),
                        },
                    });
                    k += 1;
                }
                if wback {
                    v.push(format!("R[{rn}] = {back};"));
                }
                // pc is written last, after every load and the writeback.
                if pc_loaded {
                    match self.flow(a) {
                        Flow::Return => v.extend(ret("t".into())),
                        Flow::Indirect { .. } => v.extend(indirect("t".into())),
                        f => return Err(format!("unexpected flow {f:?}")),
                    }
                }
                block(v)
            }

            Insn::Branch { .. } => match self.flow(a) {
                Flow::Jump(t) => vec![self.transfer(r, entry, a, t)],
                Flow::Call(t) if self.sig.contains_key(&t) => {
                    let back = self.call_back(a);
                    if self.lr_escape.contains(&back) {
                        return Err(format!(
                            "a call to the signature routine at &{t:08X} whose return address &{back:08X} can be jumped to",
                        ));
                    }
                    let (ins, outs) = self.sig[&t];
                    let args: String = (0..15u8)
                        .filter(|n| (ins | outs) >> n & 1 != 0)
                        .map(|n| if outs >> n & 1 != 0 { format!(", &{}", self.rg(n)) } else { format!(", {}", self.rg(n)) })
                        .collect();
                    let mut v = vec![
                        format!("{} = 0x{back:08X}u;", self.rg(14)),
                        format!("{}(s{args});", self.function(t)),
                        format!("ros_check_return(s, 0x{back:08X}u);"),
                    ];
                    if back != a.wrapping_add(4) {
                        v.push(self.transfer(r, entry, a, back));
                    }
                    v
                }
                Flow::Call(t) => {
                    let back = self.call_back(a);
                    if !self.entries.contains_key(&t) {
                        vec![
                            format!("R[14] = 0x{back:08X}u;"),
                            format!("ros_fault(s, 0x{t:08X}u, \"a call to an address that is not code\");"),
                        ]
                    } else if self.lr_escape.contains(&back) {
                        // The callee may store the return address and jump
                        // back to it later, from another frame. So leave a
                        // resume point here, for a longjmp to reach then. On
                        // the normal path this is exactly the checked call.
                        let f = self.function(t);
                        [
                            "{".to_string(),
                            "    struct ros_resume rs;".to_string(),
                            format!("    rs.at = 0x{back:08X}u;"),
                            "    rs.sp = R[13];".to_string(),
                            "    rs.prev = ros_resume_top;".to_string(),
                            "    if (setjmp(rs.jb) == 0) {".to_string(),
                            "        ros_resume_top = &rs;".to_string(),
                            format!("        R[14] = 0x{back:08X}u;"),
                            format!("        {f}(s);"),
                            "        ros_resume_top = rs.prev;".to_string(),
                            "    } else {".to_string(),
                            "        ros_resume_top = rs.prev;".to_string(),
                            "    }".to_string(),
                            "}".to_string(),
                            format!("ros_check_return(s, 0x{back:08X}u);"),
                        ]
                        .into_iter()
                        .chain(self.reloads(a))
                        // Go on to where lr points, as the other calls do.
                        // After `ADR lr` then B, that is not the next
                        // instruction. (Without this, Wimp_TextOp fell into
                        // its next reason's call.)
                        .chain((back != a.wrapping_add(4)).then(|| self.transfer(r, entry, a, back)))
                        .collect()
                    } else {
                        let call = format!("{}(s);", self.function(t));
                        let mut v = vec![
                            format!("R[14] = 0x{back:08X}u;"),
                            call,
                            format!("ros_check_return(s, 0x{back:08X}u);"),
                        ];
                        v.extend(self.reloads(a));
                        if back != a.wrapping_add(4) {
                            v.push(self.transfer(r, entry, a, back));
                        }
                        v
                    }
                }
                f => return Err(format!("unexpected flow {f:?}")),
            },

            Insn::Bx { link, rm } => {
                if rm == 15 {
                    return Err("BX pc".into());
                }
                match self.flow(a) {
                    Flow::Return => ret(self.rg(14)),
                    Flow::Indirect { .. } if link => {
                        let back = a.wrapping_add(4);
                        let mut v = vec![
                            "{".into(),
                            format!("    uint32_t t = {};", self.rg(rm)),
                            format!("    R[14] = 0x{back:08X}u;"),
                            "    ros_call(s, t);".into(),
                            "}".into(),
                            format!("ros_check_return(s, 0x{back:08X}u);"),
                        ];
                        v.extend(self.reloads(a));
                        v
                    }
                    Flow::Indirect { .. } => indirect(self.rg(rm)),
                    f => return Err(format!("unexpected flow {f:?}")),
                }
            }

            Insn::Swi { number } => {
                let n = number & !X_BIT;
                let x = number & X_BIT != 0;
                let mut v = match self.inp.native_swis.get(&n) {
                    Some(name) => {
                        let mut v = vec![format!("ros_native_swi(s, ros_thunk_{name});")];
                        if !x {
                            v.push("if (s->v) ros_swi_raise(s);".into());
                        }
                        v
                    }
                    None if n == 1 => {
                        // OS_WriteS. The string is inline after the call, in
                        // the image, and execution resumes past it. The C
                        // passes the string's address to the runtime, which
                        // prints it, then transfers to the resume point.
                        let Some(resume) = self.writes_resume(a) else {
                            return Err("OS_WriteS with no code after its string".into());
                        };
                        let mut v = vec![format!("ros_writes(s, 0x{:08X}u);", a.wrapping_add(4))];
                        v.push(self.transfer(r, entry, a, resume));
                        return Ok(v);
                    }
                    None => vec![format!("ros_swi(s, 0x{number:X}u);")],
                };
                v.extend(self.reloads(a));
                v
            }

            Insn::Mrs { rd, spsr } => {
                if spsr || rd == 15 {
                    return Err("MRS of the SPSR, or into pc".into());
                }
                vec![format!("{} = ros_cpsr(s);", self.rg(rd))]
            }
            Insn::Msr { spsr, mask, src } => {
                if spsr && self.user_return_follows(a) {
                    // This sets the PSR to go back with. ros_user_return
                    // takes it from word 16 of the block, which is where
                    // this value came from, so nothing is needed here.
                    return Ok(vec![]);
                }
                if spsr {
                    return Err("MSR to the SPSR".into());
                }
                let v = match src {
                    MsrSrc::Imm(i) => format!("0x{i:08X}u"),
                    MsrSrc::Reg(15) => return Err("MSR from pc".into()),
                    MsrSrc::Reg(m) => self.rg(m),
                };
                let mut out = vec![];
                if mask & 8 != 0 {
                    out.push(format!("ros_msr_f(s, {v});"));
                }
                if mask & 1 != 0 {
                    out.push(format!("ros_msr_c(s, {v}, 0x{a:08X}u);"));
                }
                out
            }
            Insn::Clz { rd, rm } => {
                if rd == 15 || rm == 15 {
                    return Err("CLZ with pc".into());
                }
                vec![format!("R[{rd}] = R[{rm}] ? (uint32_t)__builtin_clz(R[{rm}]) : 32u;")]
            }
            Insn::MovHalf { top, rd, imm } => {
                if rd == 15 {
                    return Err("MOVW or MOVT into pc".into());
                }
                if top {
                    vec![format!("R[{rd}] = (R[{rd}] & 0xFFFFu) | 0x{:08X}u;", (imm as u32) << 16)]
                } else {
                    vec![format!("R[{rd}] = 0x{imm:X}u;")]
                }
            }
            // Interrupts on or off, by the modelled I bit. There are no
            // FIQs.
            Insn::Cps { disable, i, mode, .. } => {
                let mut v = vec![];
                if i {
                    v.push(format!("s->irq_off = {};", disable as u32));
                }
                if let Some(m) = mode {
                    v.push(format!("ros_msr_c(s, (ros_cpsr(s) & ~0x1Fu) | 0x{m:X}u, 0x{a:08X}u);"));
                }
                v
            }
            Insn::Swp { byte, rt, rt2, rn } => {
                if [rt, rt2, rn].contains(&15) || rn == rt || rn == rt2 {
                    return Err("an unpredictable SWP".into());
                }
                let (ld, st) = if byte { ("ros_ld8", "ros_st8") } else { ("ros_ld32", "ros_st32") };
                let (n, t2, t) = (self.rg(rn), self.rg(rt2), self.rg(rt));
                vec![
                    "{".into(),
                    format!("    uint32_t t = {ld}({n});"),
                    format!("    {st}({n}, {t2});"),
                    format!("    {t} = t;"),
                    "}".into(),
                ]
            }
        })
    }

    /// An operand's value, the shifter's carry out (None if C is
    /// unchanged), and any statements that must come first. A
    /// register-specified shift needs them, to work out its carry at run
    /// time.
    fn operand2(&self, o: Operand2, a: u32) -> Result<(String, Option<String>, Vec<String>), String> {
        let pc = a.wrapping_add(8);
        Ok(match o {
            Operand2::Imm { value, rotated } => {
                let v = if value < 4096 { format!("{value}") } else { format!("0x{value:X}u") };
                (v, rotated.then(|| format!("{}", value >> 31)), vec![])
            }
            Operand2::Reg { rm, shift } => {
                let m = if rm == 15 { format!("0x{pc:08X}u") } else { self.rg(rm) };
                match shift {
                    Shift::Imm(ShiftType::Lsl, 0) => (m, None, vec![]),
                    Shift::Imm(ShiftType::Lsl, n) => {
                        (format!("({m} << {n})"), Some(format!("(({m} >> {}) & 1)", 32 - n)), vec![])
                    }
                    Shift::Imm(ShiftType::Lsr, 32) => ("0".into(), Some(format!("({m} >> 31)")), vec![]),
                    Shift::Imm(ShiftType::Lsr, n) => {
                        (format!("({m} >> {n})"), Some(format!("(({m} >> {}) & 1)", n - 1)), vec![])
                    }
                    Shift::Imm(ShiftType::Asr, 32) => {
                        (format!("(uint32_t)((int32_t){m} >> 31)"), Some(format!("({m} >> 31)")), vec![])
                    }
                    Shift::Imm(ShiftType::Asr, n) => (
                        format!("(uint32_t)((int32_t){m} >> {n})"),
                        Some(format!("(({m} >> {}) & 1)", n - 1)),
                        vec![],
                    ),
                    Shift::Imm(ShiftType::Ror, n) => {
                        (format!("ros_ror({m}, {n})"), Some(format!("(({m} >> {}) & 1)", n - 1)), vec![])
                    }
                    Shift::Rrx => (format!("(s->c << 31 | {m} >> 1)"), Some(format!("({m} & 1)")), vec![]),
                    Shift::Reg(ty, rs) => {
                        if rm == 15 || rs == 15 {
                            return Err("a register-shifted register involving pc".into());
                        }
                        let t = match ty {
                            ShiftType::Lsl => "ROS_LSL",
                            ShiftType::Lsr => "ROS_LSR",
                            ShiftType::Asr => "ROS_ASR",
                            ShiftType::Ror => "ROS_ROR",
                        };
                        (
                            "op2".into(),
                            Some("sc".into()),
                            vec![
                                "uint32_t sc = s->c;".into(),
                                format!("uint32_t op2 = ros_shift({m}, {t}, {}, &sc);", self.rg(rs)),
                            ],
                        )
                    }
                }
            }
        })
    }
}

/// Whether an area begins with a RISC OS module header, as a module's
/// first area does, whatever it is called (`|Buffers$$Code|`). A header
/// has a title that is a short printable string, and entry offsets that
/// are zero or word-aligned inside the area.
pub fn looks_like_module_header(a: &aof::Area) -> bool {
    if a.attributes & aof::area_attr::CODE == 0 || a.data.len() < 13 * 4 {
        return false;
    }
    let word = |k: usize| u32::from_le_bytes(a.data[4 * k..4 * k + 4].try_into().unwrap());
    let len = a.data.len() as u32;
    let inside = |off: u32| off == 0 || (off.is_multiple_of(4) && off >= 13 * 4 && off < len);
    let text = |off: u32, min: usize| {
        let Some(rest) = a.data.get(off as usize..) else { return false };
        let n = rest.iter().position(|&b| b == 0).unwrap_or(usize::MAX);
        n >= min && n < 256 && rest[..n].iter().all(|&b| (32..127).contains(&b) || b == 9)
    };
    let title = word(4);
    title != 0
        && title < len
        && text(title, 1)
        && [0, 1, 2, 3, 8].into_iter().all(|k| inside(word(k)))
        && (word(5) == 0 || text(word(5), 1))
        && word(7) % 64 == 0
        && word(7) < 1 << 24
}

/// Compile a unit. Errors name the source line of each instruction that
/// could not be compiled exactly.
pub fn emit(inp: &Input) -> Result<Output, Vec<String>> {
    let mut errors = Vec::new();

    // ---- placement, and the image with its relocations applied ----
    let mut bases = Vec::new();
    let mut end = inp.base;
    for a in inp.areas {
        if a.attributes & aof::area_attr::ZERO_INIT != 0 {
            errors.push(format!("area {} is zero-initialised: a ROM unit has none", a.name));
        }
        let align = 1u32 << a.alignment.max(2);
        end = (end + align - 1) & !(align - 1);
        bases.push(end);
        end += a.data.len() as u32;
    }
    let mut image = vec![0u8; (end - inp.base) as usize];
    for (a, &b) in inp.areas.iter().zip(&bases) {
        let at = (b - inp.base) as usize;
        image[at..at + a.data.len()].copy_from_slice(&a.data);
    }
    let mut relocated: Vec<u32> = Vec::new();
    for (ai, a) in inp.areas.iter().enumerate() {
        for r in &a.relocs {
            let at = (bases[ai] - inp.base + r.offset) as usize;
            match (&r.field, r.pc_relative, &r.by) {
                (aof::FieldType::Word, false, aof::RelocBy::Area(t)) => {
                    let w = u32::from_le_bytes(image[at..at + 4].try_into().unwrap());
                    let v = w.wrapping_add(bases[*t as usize]);
                    image[at..at + 4].copy_from_slice(&v.to_le_bytes());
                    relocated.push(bases[ai] + r.offset);
                }
                (_, _, aof::RelocBy::Symbol(_)) => errors.push(format!(
                    "area {} offset &{:X}: a reference to an imported symbol, which needs \
                     a linker the C back end does not have yet",
                    a.name, r.offset
                )),
                _ => errors.push(format!(
                    "area {} offset &{:X}: a {:?} relocation the C back end does not apply",
                    a.name, r.offset, r.field
                )),
            }
        }
    }

    let mut unit = Unit {
        inp,
        image,
        code: BTreeMap::new(),
        names: BTreeMap::new(),
        entries: BTreeMap::new(),
        errors,
        cond_override: HashMap::new(),
        relocated: relocated.iter().copied().collect(),
        external: BTreeSet::new(),
        sp_escape: BTreeSet::new(),
        sig: HashMap::new(),
        region_sig: None,
        frames: HashMap::new(),
        records: Vec::new(),
        members: HashMap::new(),
        sp_at: HashMap::new(),
        peeks: BTreeSet::new(),
        region_slots: None,
        ret_flags: HashMap::new(),
        entry_flags: HashMap::new(),
        must_flags: HashMap::new(),
        ref_regs: HashMap::new(),
        mod_regs: HashMap::new(),
        ret_regs: HashMap::new(),
        touch_regs: HashMap::new(),
        locals: 0,
        dirty: HashMap::new(),
        restore: 0,
        region_live_out: HashMap::new(),
        region_entry: 0,
        read_after: HashMap::new(),
        written: 0,
        returns_via: HashMap::new(),
        lr_escape: BTreeSet::new(),
        late: Vec::new(),
        table_lr: HashMap::new(),
    };

    // ---- code, decoded, with its source lines ----
    for (ai, a) in inp.areas.iter().enumerate() {
        let mut marks: Vec<(u32, char)> =
            inp.mapping.iter().filter(|m| m.0 == ai).map(|m| (m.1, m.2)).collect();
        marks.sort();
        for (i, &(from, kind)) in marks.iter().enumerate() {
            if kind != 'a' {
                continue;
            }
            let to = marks.get(i + 1).map_or(a.data.len() as u32, |m| m.0);
            let mut last_line = None;
            for off in (from..to).step_by(4) {
                let addr = bases[ai] + off;
                let w = unit.word(addr).unwrap_or(0);
                let src = inp.sources.get(&(ai, off)).cloned();
                let key = src.as_ref().map(|s| (s.file.clone(), s.line, s.text.clone()));
                let first = key != last_line;
                last_line = key;
                let mut d = a32::decode(w);
                // A branch to the next instruction does nothing, whatever its
                // condition. Conditional assembly can leave one, such as
                // `BEQ %FT01` just before `01`.
                if let Insn::Branch { link: false, offset: -4 } = d.insn {
                    d = a32::Decoded { cond: Cond::Al, insn: Insn::Nop };
                }
                unit.code.insert(addr, Word { d, src, first });
            }
        }
    }

    // A branch assembled as data is code if code falls into it. ExitS has
    // one, `DCD &1A000000 :EOR: Cond_$cond + ...`, which branches over its
    // own return. It must be a conditional B into its own area, with code
    // after it.
    for (ai, a) in inp.areas.iter().enumerate() {
        let (lo, hi) = (bases[ai], bases[ai] + a.data.len() as u32);
        let mut addr = lo;
        while addr + 8 <= hi {
            let at = addr + 4;
            addr += 4;
            if unit.code.contains_key(&at) || !unit.code.contains_key(&(at - 4)) || !unit.code.contains_key(&(at + 4)) {
                continue;
            }
            let falls = matches!(unit.flow(at - 4), Flow::Next | Flow::Call(_)) || unit.code[&(at - 4)].d.cond != Cond::Al;
            let d = a32::decode(unit.word(at).unwrap_or(0));
            if let (true, Insn::Branch { link: false, offset }) = (falls, d.insn) {
                let target = at.wrapping_add(8).wrapping_add(offset as u32);
                if d.cond != Cond::Al && (lo..hi).contains(&target) && unit.code.contains_key(&target) {
                    let src = inp.sources.get(&(ai, at - lo)).cloned();
                    unit.code.insert(at, Word { d, src, first: true });
                }
            }
        }
    }

    // An FPA instruction assembled as data is code if code falls into it,
    // since the processor would execute it. The Wimp's saveFPregs writes
    // its SFMs as `DCD &ED820200 :OR: ...`, because AAsm assembled them
    // wrongly. The search goes in address order, so that each in a run of
    // them follows the one before.
    for (ai, a) in inp.areas.iter().enumerate() {
        let (lo, hi) = (bases[ai], bases[ai] + a.data.len() as u32);
        for at in (lo + 4..hi).step_by(4) {
            if unit.code.contains_key(&at) || !unit.code.contains_key(&(at - 4)) {
                continue;
            }
            let falls = matches!(unit.flow(at - 4), Flow::Next | Flow::Call(_)) || unit.code[&(at - 4)].d.cond != Cond::Al;
            let d = a32::decode(unit.word(at).unwrap_or(0));
            let fpa = matches!(
                d.insn,
                Insn::FpaMem { .. }
                    | Insn::FpaMulti { .. }
                    | Insn::FpaDyadic { .. }
                    | Insn::FpaMonadic { .. }
                    | Insn::FpaFlt { .. }
                    | Insn::FpaFix { .. }
                    | Insn::FpaStatus { .. }
                    | Insn::FpaCompare { .. }
            );
            if falls && fpa {
                let src = inp.sources.get(&(ai, at - lo)).cloned();
                unit.code.insert(at, Word { d, src, first: true });
            }
        }
    }

    // ---- labels: exported names first, then the rest alphabetically ----
    let mut by_addr: BTreeMap<u32, Vec<String>> = BTreeMap::new();
    for (name, &(ai, off)) in inp.labels {
        if let Some(b) = bases.get(ai) {
            by_addr.entry(b + off).or_default().push(name.clone());
        }
    }
    for (addr, mut names) in by_addr.clone() {
        names.sort_by_key(|n| (!inp.exports.contains(n), !c_safe(n), n.clone()));
        unit.names.insert(addr, names[0].clone());
    }

    // ---- entries the unit declares ----
    if inp.module {
        let base = inp.base;
        for (off, what) in [
            (0x00, "the module's start entry"),
            (0x04, "the module's initialise entry"),
            (0x08, "the module's finalise entry"),
            (0x0C, "the module's service call entry"),
            (0x20, "the module's SWI handler"),
            (0x28, "the module's SWI decoding code"),
        ] {
            if let Some(v) = unit.word(base + off) {
                if v != 0 {
                    unit.entry(unit.header_addr(base + off, v), what.into());
                }
            }
        }
        // The *command table: name, aligned, then code, info, syntax, help.
        if let Some(t) = unit.word(base + 0x18).filter(|&t| t != 0) {
            let mut p = unit.header_addr(base + 0x18, t);
            while let Some(off) = p.checked_sub(base) {
                let Some(rest) = unit.image.get(off as usize..) else { break };
                let len = rest.iter().position(|&b| b == 0).unwrap_or(rest.len());
                if len == 0 {
                    break;
                }
                let name = String::from_utf8_lossy(&rest[..len]).to_string();
                p = (p + len as u32 + 1 + 3) & !3;
                if let Some(code) = unit.word(p).filter(|&c| c != 0) {
                    unit.entry(unit.header_addr(p, code), format!("the *{name} command"));
                }
                p += 16;
            }
        }
    }
    for addr in relocated {
        if let Some(v) = unit.word(addr) {
            if unit.is_code(v) {
                unit.entry(v, format!("its address is in a table at &{addr:08X}"));
            }
        }
    }
    // A table of offsets between labels. The Wimp's filtertable is one:
    // `& prefilter_default - filtertable`, which code adds to the table's
    // address and calls. A data word's value may be a code label's offset
    // from another label that the source names. Then the code label is an
    // entry, if nothing else reaches it. (BASIC's statement tables are
    // jump tables that the compiler follows, and their handlers stay
    // cases.) The word counts as data by its directive (& or DCD),
    // whatever the mapping symbols say, because the Wimp's tables sit in
    // code without a $d.
    let label_at = |name: &str| -> Option<u32> { inp.labels.get(name).and_then(|&(ai, off)| bases.get(ai).map(|b| b + off)) };
    let mut offsets = Vec::new();
    for (&(ai, off), src) in inp.sources.iter() {
        let Some(&base) = bases.get(ai) else { continue };
        let addr = base + off;
        let mut words = src.text.split_whitespace();
        let first = if src.text.starts_with(char::is_whitespace) { words.next() } else { words.nth(1) };
        if !matches!(first, Some("&" | "DCD" | "DCDU")) || !src.text.contains('-') {
            continue;
        }
        let Some(v) = unit.word(addr) else { continue };
        let names: Vec<(&str, u32)> = src
            .text
            .split(|c: char| !(c.is_ascii_alphanumeric() || c == '_'))
            .filter(|w| !w.is_empty())
            .filter_map(|w| label_at(w).map(|a| (w, a)))
            .collect();
        for &(a_name, a) in &names {
            for &(b_name, b) in &names {
                if a != b && unit.is_code(a) && a.wrapping_sub(b) == v {
                    offsets.push((a, format!("its offset from {b_name} is in a table at &{addr:08X} ({a_name})")));
                }
            }
        }
        // Or the offset may be from the word itself. The Wimp's
        // defaulthandlers has `DCD Do_ErrorHandler-.-4`.
        // setdefaulthandlers adds it to the address after the word
        // (`LDR R1,[R4],#4`, `ADDNE R1,R1,R4`) and gives the result to
        // OS_ChangeEnvironment.
        let expr: String = src.text.split(';').next().unwrap_or("").chars().filter(|c| !c.is_whitespace()).collect();
        if let Some(i) = expr.find("-.") {
            let rest = &expr[i + 2..];
            let k = if rest.is_empty() { Some(0) } else { rest.strip_prefix('-').and_then(|n| n.parse::<u32>().ok()) };
            for &(a_name, a) in &names {
                if k.is_some_and(|k| unit.is_code(a) && a.wrapping_sub(addr).wrapping_sub(k) == v) {
                    offsets.push((a, format!("its offset from the word is in a table at &{addr:08X} ({a_name})")));
                }
            }
        }
    }
    offsets.sort();                     // (the sources' map has no order)
    unit.late = offsets;
    for name in inp.exports {
        if let Some(&(ai, off)) = inp.labels.get(name) {
            let addr = bases[ai] + off;
            if unit.is_code(addr) {
                unit.entry(addr, "exported".into());
            }
        }
    }
    unit.table_lr = unit.find_call_tables();
    let seen = unit.discover();
    let late: Vec<(u32, String)> = std::mem::take(&mut unit.late).into_iter().filter(|(a, _)| !seen.contains(a)).collect();
    if !late.is_empty() {
        for (a, why) in late {
            unit.entry(a, why);
        }
        unit.discover();
    }
    unit.share();
    // This must come before signatures, because a routine whose return
    // address escapes gets none.
    unit.find_lr_escapes();
    unit.summaries();
    if inp.lift {
        unit.signatures();
        unit.records();
    }

    // ---- C ----
    let name = inp.name.clone();
    let upper = name.to_ascii_uppercase();
    let mut c = String::new();
    let banner = [
        format!("/* rom_{name}.c -- {name}, compiled from ObjAsm by rosasm --emit c:"),
        " * do not edit.".to_string(),
        " *".to_string(),
        " * Each instruction's exact A32 semantics over the state block, beside".to_string(),
        " * the source line it came from, lifted to the expressions they compute".to_string(),
        " * where tier 1 can (`R` is the state's registers).  The ROM image is".to_string(),
        format!(" * here too, placed at &{:08X}: compiled code reads its tables,", inp.base),
        " * strings and error blocks at the addresses the source gave them. */".to_string(),
        "#include <stdint.h>".to_string(),
        String::new(),
        "#include \"rosgd/cpu.h\"".to_string(),
        "#include \"rosgd/error.h\"".to_string(),
        format!("#include \"{}\"", inp.header),
        String::new(),
        String::new(),
    ];
    c.push_str(&banner.join("\n"));
    c.push_str(&format!(
        "const uint32_t rom_{name}_base = 0x{:08X}u;\nconst uint32_t rom_{name}_size = {}u;\n\
         const uint8_t rom_{name}[{}] = {{\n",
        inp.base,
        unit.image.len(),
        unit.image.len().max(1)
    ));
    for chunk in unit.image.chunks(12) {
        let row: Vec<String> = chunk.iter().map(|b| format!("0x{b:02X}")).collect();
        c.push_str(&format!("    {},\n", row.join(", ")));
    }
    c.push_str("};\n\n");

    // Native SWIs bound statically, declared as the runtime defines them.
    let mut thunks: BTreeSet<String> = BTreeSet::new();
    let mut entries: Vec<u32> = unit.entries.keys().copied().collect();
    entries.sort();
    let mut regions_src = String::new();
    let mut instructions = 0;
    for &e in &entries {
        instructions += unit.compile_region(e, &mut regions_src);
    }
    for w in unit.code.values() {
        if let Insn::Swi { number } = w.d.insn {
            if let Some(n) = inp.native_swis.get(&(number & !X_BIT)) {
                if regions_src.contains(&format!("ros_thunk_{n})")) {
                    thunks.insert(n.clone());
                }
            }
        }
    }
    if !thunks.is_empty() {
        c.push_str("/* Native kernel SWIs, bound statically. */\n");
        for t in &thunks {
            c.push_str(&format!("void ros_thunk_{t}(struct ros_cpu *s);\n"));
        }
        c.push('\n');
    }
    // The source's constants the code names, as the source defined them.
    let mut named: BTreeMap<&str, u32> = BTreeMap::new();
    // The names the code uses, not the source lines quoted beside it.
    let mut code_only = String::with_capacity(regions_src.len());
    let mut rest = regions_src.as_str();
    while let Some(k) = rest.find("/*") {
        code_only.push_str(&rest[..k]);
        rest = rest[k..].find("*/").map_or("", |e| &rest[k + e + 2..]);
    }
    code_only.push_str(rest);
    let used: BTreeSet<&str> = code_only.split(|c: char| !(c.is_ascii_alphanumeric() || c == '_')).collect();
    for w in unit.code.values() {
        let Some(src) = &w.src else { continue };
        for n in named_immediates(&src.text) {
            if let Some(&k) = inp.constants.get(n) {
                if nameable(n, &inp.name) && used.contains(n) && !unit.members.contains_key(n) {
                    named.insert(n, k);
                }
            }
        }
    }
    // The storage maps the code reads as structs.
    for r in &unit.records {
        if used.contains(r.tag.as_str()) {
            c.push_str(&unit.record_c(r));
        }
    }
    if !named.is_empty() {
        c.push_str("/* The source's constants, by name. */\n");
        for (n, k) in &named {
            let v = if *k < 256 && !k.is_power_of_two() || *k < 16 { format!("{k}u") } else { format!("0x{k:X}u") };
            c.push_str(&format!("#define {n} {v}\n"));
        }
        c.push('\n');
    }
    c.push_str(
        "/* A transfer to another region is a tail call -- guaranteed, for the\n\
         * chain of them (a statement loop) must not grow the C stack.  Where\n\
         * the compiler has no musttail, an ordinary call stands in: build with\n\
         * optimisation, or a long-running loop overflows. */\n\
         #if defined(__clang__)\n\
         # define ROS_TAIL_CALL(f) __attribute__((musttail)) return f(s);\n\
         #elif defined(__GNUC__) && __GNUC__ >= 15\n\
         # define ROS_TAIL_CALL(f) return __attribute__((musttail)) f(s);\n\
         #else\n\
         # define ROS_TAIL_CALL(f) return f(s);\n\
         #endif\n\
         \n\
         #define R (s->r)\n\
         /* Floating point, as standard C floating point: FPA's registers, and\n\
          * VFP's -- doubles, singles, and the bits of each. */\n\
         #define F (s->fp->f)\n#define D (s->fp->vfp.d)\n#define S (s->fp->vfp.s)\n\
         #define DW (s->fp->vfp.dw)\n#define SW (s->fp->vfp.sw)\n\
         #pragma STDC FP_CONTRACT OFF\n\n",
    );
    if !unit.lr_escape.is_empty() {
        c.push_str("#include <setjmp.h>\n\
             /* Return addresses the code jumps to itself, through an lr it\n\
              * stored away: each BL site that can be returned to this way\n\
              * leaves a resume point on the runtime's chain, and an indirect\n\
              * transfer to one longjmps back to it -- the C stack unwinds to\n\
              * the frame whose BL made the address, which is what the ARM\n\
              * code's one register file does with a single jump (cpu.h,\n\
              * ros_resume). */\n\n");
    }
    for &e in &entries {
        c.push_str(&format!("static void {}(struct ros_cpu *s{});\n", unit.function(e), unit.params_decl(e)));
    }
    c.push('\n');
    c.push_str(&regions_src);
    c.push_str(&format!("static const struct ros_code_entry {name}_code[] = {{\n"));
    // A routine with a signature is not registered. Nothing takes its
    // address, and only its callers call it.
    let registered: Vec<u32> = entries.iter().copied().filter(|e| !unit.sig.contains_key(e)).collect();
    for &e in &registered {
        c.push_str(&format!(
            "    {{ 0x{e:08X}u, {}, \"{}:{}\" }},\n",
            unit.function(e),
            inp.name,
            unit.names.get(&e).cloned().unwrap_or_else(|| format!("&{e:08X}"))
        ));
    }
    // C has no empty initialiser. So a unit with no entries gets one
    // dummy entry, and registers none.
    if registered.is_empty() {
        c.push_str("    { 0, 0, \"\" },\n");
    }
    c.push_str(&format!(
        "}};\n\nvoid {name}_register(void)\n{{\n    ros_code_register({name}_code, {});\n}}\n",
        if registered.is_empty() { "0".to_string() } else { format!("sizeof {name}_code / sizeof {name}_code[0]") }
    ));

    // ---- the header ----
    let mut h = format!(
        "/* rom_{name}.h -- where {name}'s labels are in the ROM image.\n \
         * Generated by rosasm --emit c: do not edit. */\n\
         #ifndef ROM_{upper}_H\n#define ROM_{upper}_H\n\n#include <stdint.h>\n\n\
         extern const uint32_t rom_{name}_base;\nextern const uint32_t rom_{name}_size;\n\
         extern const uint8_t rom_{name}[];\nvoid {name}_register(void);\n\n"
    );
    for (addr, names) in &by_addr {
        for n in names.iter().filter(|n| c_safe(n)) {
            h.push_str(&format!("#define {upper}_{n} 0x{addr:08X}u\n"));
        }
    }
    h.push_str("\n#endif\n");

    if !unit.errors.is_empty() {
        let mut e = unit.errors;
        e.dedup();
        return Err(e);
    }
    Ok(Output { c, h, regions: entries.len(), instructions })
}

#[cfg(test)]
mod tests {
    use super::*;

    /// A unit of hand-encoded words at &FC000000, all in one area, with
    /// the code/data marks and labels given.
    fn unit(words: &[u32], marks: &[(u32, char)], labels: &[(&str, u32)], exports: &[&str]) -> Result<Output, Vec<String>> {
        unit_lift(words, marks, labels, exports, true)
    }

    fn unit_lift(
        words: &[u32],
        marks: &[(u32, char)],
        labels: &[(&str, u32)],
        exports: &[&str],
        lift: bool,
    ) -> Result<Output, Vec<String>> {
        unit_swis(words, marks, labels, exports, lift, HashMap::new())
    }

    fn unit_swis(
        words: &[u32],
        marks: &[(u32, char)],
        labels: &[(&str, u32)],
        exports: &[&str],
        lift: bool,
        swi_regs: HashMap<u32, (u16, u16)>,
    ) -> Result<Output, Vec<String>> {
        let mut area = aof::Area::new("Code".to_string(), aof::area_attr::CODE);
        area.alignment = 2;
        area.data = words.iter().flat_map(|w| w.to_le_bytes()).collect();
        let mapping: Vec<(usize, u32, char)> = marks.iter().map(|&(o, k)| (0, o, k)).collect();
        let labels: HashMap<String, (usize, u32)> =
            labels.iter().map(|&(n, o)| (n.to_string(), (0, o))).collect();
        let exports: Vec<String> = exports.iter().map(|s| s.to_string()).collect();
        let areas = [area];
        let input = Input {
            name: "t0".into(),
            base: 0xFC00_0000,
            areas: &areas,
            mapping: &mapping,
            labels: &labels,
            exports: &exports,
            sources: HashMap::new(),
            native_swis: HashMap::from([(0x2, "OS_Write0".to_string())]),
            swi_regs,
            module: false,
            apcs: false,
            lift,
            poll_loops: false,
            header: "rom_t0.h".into(),
            constants: HashMap::new(),
            maps: vec![],
        };
        emit(&input)
    }

    /// unit(), with source lines for some of the words: (offset, text).
    fn unit_src(words: &[u32], marks: &[(u32, char)], labels: &[(&str, u32)], lines: &[(u32, &str)]) -> Result<Output, Vec<String>> {
        let mut area = aof::Area::new("Code".to_string(), aof::area_attr::CODE);
        area.alignment = 2;
        area.data = words.iter().flat_map(|w| w.to_le_bytes()).collect();
        let mapping: Vec<(usize, u32, char)> = marks.iter().map(|&(o, k)| (0, o, k)).collect();
        let labels: HashMap<String, (usize, u32)> = labels.iter().map(|&(n, o)| (n.to_string(), (0, o))).collect();
        let exports = vec!["Entry".to_string()];
        let areas = [area];
        let sources = lines
            .iter()
            .map(|&(o, t)| ((0, o), Source { file: "t0.s".into(), line: o as usize / 4 + 1, text: t.into(), notes: vec![] }))
            .collect();
        let input = Input {
            name: "t0".into(),
            base: 0xFC00_0000,
            areas: &areas,
            mapping: &mapping,
            labels: &labels,
            exports: &exports,
            sources,
            native_swis: HashMap::new(),
            swi_regs: HashMap::new(),
            module: false,
            apcs: false,
            lift: true,
            poll_loops: false,
            header: "rom_t0.h".into(),
            constants: HashMap::new(),
            maps: vec![],
        };
        emit(&input)
    }

    #[test]
    fn a_branch_table_whose_address_is_taken() {
        // The Wimp's openwlp3_jumptable: ADR the table, add the row, then
        // MOV pc.
        let words = [
            0xE28F_0004, // 00 Entry  ADR    r0, Table
            0xE080_0101, // 04        ADD    r0, r0, r1, LSL #2
            0xE1A0_F000, // 08        MOV    pc, r0
            0xEA00_0001, // 0C Table  B      A
            0xEA00_0002, // 10        B      B2
            0xEA00_0003, // 14        B      C
            0xE3A0_0001, // 18 A      MOV    r0, #1
            0xE1A0_F00E, // 1C        MOV    pc, lr
            0xE3A0_0002, // 20 B2     MOV    r0, #2
            0xE1A0_F00E, // 24        MOV    pc, lr
            0xE3A0_0003, // 28 C      MOV    r0, #3
            0xE1A0_F00E, // 2C        MOV    pc, lr
        ];
        let o = unit(&words, &[(0, 'a')], &[("Entry", 0), ("Table", 0x0C), ("A", 0x18), ("B2", 0x20), ("C", 0x28)], &["Entry"])
            .unwrap_or_else(|e| panic!("{e:?}"));
        for row in ["0xFC00000Cu", "0xFC000010u", "0xFC000014u"] {
            assert!(o.c.contains(&format!("{{ {row}, ")), "row {row} an entry: {}", o.c);
        }
    }

    #[test]
    fn a_branch_table_whose_last_row_branches_to_the_next_instruction() {
        // The Wimp's openwlp3_jumptable as it really is. Its last row,
        // method 3 (Wimp02 5343), is `B openwlp3_skip_to_bottom`, and that
        // label is just after it. The row is an entry as the others are,
        // and runs on into the code after it. Iconising a window opens it
        // behind -3, which is method 3, and a MOV pc to the row used to
        // fail as 'not compiled code'.
        let words = [
            0xE28F_0004, // 00 Entry  ADR    r0, Table
            0xE080_0101, // 04        ADD    r0, r0, r1, LSL #2
            0xE1A0_F000, // 08        MOV    pc, r0
            0xEA00_0003, // 0C Table  B      A
            0xEA00_0004, // 10        B      B2
            0xEAFF_FFFF, // 14        B      C  (the next instruction)
            0xE3A0_0003, // 18 C      MOV    r0, #3
            0xE1A0_F00E, // 1C        MOV    pc, lr
            0xE3A0_0001, // 20 A      MOV    r0, #1
            0xE1A0_F00E, // 24        MOV    pc, lr
            0xE3A0_0002, // 28 B2     MOV    r0, #2
            0xE1A0_F00E, // 2C        MOV    pc, lr
        ];
        let o = unit(&words, &[(0, 'a')], &[("Entry", 0), ("Table", 0x0C), ("C", 0x18), ("A", 0x20), ("B2", 0x28)], &["Entry"])
            .unwrap_or_else(|e| panic!("{e:?}"));
        for row in ["0xFC00000Cu", "0xFC000010u", "0xFC000014u"] {
            assert!(o.c.contains(&format!("{{ {row}, ")), "row {row} an entry: {}", o.c);
        }
        // The row does what C does: r0 = 3, then return.
        let f = o.c.split("static void t0_FC000014(struct ros_cpu *s)\n{").nth(1).unwrap_or_else(|| panic!("no row 3: {}", o.c));
        let body = &f[..f.find("\n}\n").unwrap_or(f.len())];
        assert!(body.contains("r0 = 3;"), "row 3 runs on into C: {body}");
    }

    #[test]
    fn the_branches_of_a_table_of_records() {
        // The Filer's messages_processed_start. Each record is a message
        // number and a B to its handler. The code finds the number and
        // jumps to the B after it.
        let words = [
            0xE28F_2018, // 00 Entry  ADR    r2, Table
            0xEA00_0001, // 04        B      L1
            0xE13E_0000, // 08 L2     TEQ    r14, r0
            0x0242_F004, // 0C        SUBEQ  pc, r2, #4
            0xE492_E008, // 10 L1     LDR    r14, [r2], #8
            0xE37E_0001, // 14        CMN    r14, #1
            0x1AFF_FFFA, // 18        BNE    L2
            0xE1A0_F00E, // 1C        MOV    pc, lr
            0x0000_0001, // 20 Table  DCD    1
            0xEA00_0002, // 24        B      A
            0x0000_0002, // 28        DCD    2
            0xEA00_0002, // 2C        B      B2
            0xFFFF_FFFF, // 30        DCD    -1
            0xE3A0_0001, // 34 A      MOV    r0, #1
            0xE1A0_F00E, // 38        MOV    pc, lr
            0xE3A0_0002, // 3C B2     MOV    r0, #2
            0xE1A0_F00E, // 40        MOV    pc, lr
        ];
        let marks = [(0, 'a'), (0x20, 'd'), (0x24, 'a'), (0x28, 'd'), (0x2C, 'a'), (0x30, 'd'), (0x34, 'a')];
        let labels = [("Entry", 0), ("L2", 0x08), ("L1", 0x10), ("Table", 0x20), ("A", 0x34), ("B2", 0x3C)];
        let o = unit(&words, &marks, &labels, &["Entry"]).unwrap_or_else(|e| panic!("{e:?}"));
        for row in ["0xFC000024u", "0xFC00002Cu"] {
            assert!(o.c.contains(&format!("{{ {row}, ")), "row {row} an entry: {}", o.c);
        }
    }

    #[test]
    fn a_call_after_push_pc() {
        // `Push "PC"`, then a B to a routine, or a MOV PC to one ADR'd
        // into R14, then a NOP. Both are calls, and the callee returns
        // with `Pull "PC"`.
        let words = [
            0xE92D_4000, // 00 Entry  STMFD  sp!, {lr}
            0xE52D_F004, // 04        STR    pc, [sp, #-4]!
            0xEA00_0005, // 08        B      Sub
            0xE1A0_0000, // 0C        MOV    r0, r0
            0xE28F_E00C, // 10        ADR    lr, Sub
            0xE52D_F004, // 14        STR    pc, [sp, #-4]!
            0xE1A0_F00E, // 18        MOV    pc, lr
            0xE1A0_0000, // 1C        MOV    r0, r0
            0xE8BD_8000, // 20        LDMFD  sp!, {pc}
            0xE280_0001, // 24 Sub    ADD    r0, r0, #1
            0xE49D_F004, // 28        LDR    pc, [sp], #4
        ];
        let o = unit(&words, &[(0, 'a')], &[("Entry", 0), ("Sub", 0x24)], &["Entry"]).unwrap_or_else(|e| panic!("{e:?}"));
        let c = &o.c;
        for back in ["0xFC00000Cu", "0xFC00001Cu"] {
            assert!(c.contains(&format!("ros_check_return(s, {back});")), "a call returning to {back}: {c}");
            assert!(c.contains(&format!(", {back});")) && c.matches(back).count() >= 2, "{back} pushed: {c}");
        }
        assert!(!c.contains("not code") && !c.contains("ros_fault"), "{c}");
        assert!(c.contains("{ 0xFC000024u, t0_Sub"), "Sub an entry, its address in lr called: {c}");
    }

    #[test]
    fn a_call_through_lr_set_either_way() {
        // starterrorbox: ADREQ lr, A; ADRNE lr, B; `Push "PC"`; MOV PC, R14.
        let words = [
            0xE92D_4000, // 00 Entry  STMFD  sp!, {lr}
            0xE330_0000, // 04        TEQ    r0, #0
            0x028F_E010, // 08        ADREQ  lr, A
            0x128F_E014, // 0C        ADRNE  lr, B
            0xE52D_F004, // 10        STR    pc, [sp, #-4]!
            0xE1A0_F00E, // 14        MOV    pc, lr
            0xE1A0_0000, // 18        MOV    r0, r0
            0xE8BD_8000, // 1C        LDMFD  sp!, {pc}
            0xE3A0_0001, // 20 A      MOV    r0, #1
            0xE49D_F004, // 24        LDR    pc, [sp], #4
            0xE3A0_0002, // 28 B      MOV    r0, #2
            0xE49D_F004, // 2C        LDR    pc, [sp], #4
        ];
        let o = unit(&words, &[(0, 'a')], &[("Entry", 0), ("A", 0x20), ("B", 0x28)], &["Entry"]).unwrap_or_else(|e| panic!("{e:?}"));
        let c = &o.c;
        assert!(c.contains("{ 0xFC000020u, t0_A") && c.contains("{ 0xFC000028u, t0_B"), "both entries: {c}");
        assert!(c.contains("ros_check_return(s, 0xFC000018u);"), "a call returning to the NOP: {c}");
    }

    #[test]
    fn a_table_of_calls_returning_to_the_poll() {
        // The poll loops of the Pinboard, the Task Manager and Free:
        // `ADR LR,repollwimp`, CMP, `ADDCC PC,PC,R0,ASL #2`, `MOV PC,LR`,
        // then a row for each reason code. A row is `MOV PC,LR` or
        // `B handler`, and each handler returns through lr to the poll.
        let words = [
            0xE92D_4000, // 00 Entry  STMFD  sp!, {lr}
            0xEB00_000A, // 04 Loop   BL     Poll
            0xE24F_E00C, // 08        ADR    lr, Loop
            0xE350_0003, // 0C        CMP    r0, #3
            0x308F_F100, // 10        ADDCC  pc, pc, r0, LSL #2
            0xE1A0_F00E, // 14        MOV    pc, lr
            0xE1A0_F00E, // 18        MOV    pc, lr       ; 0
            0xEA00_0000, // 1C        B      One          ; 1
            0xEA00_0001, // 20        B      Two          ; 2
            0xE92D_4000, // 24 One    STMFD  sp!, {lr}
            0xE8BD_8000, // 28        LDMFD  sp!, {pc}
            0xE281_1001, // 2C Two    ADD    r1, r1, #1
            0xE1A0_F00E, // 30        MOV    pc, lr
            0xE3A0_0002, // 34 Poll   MOV    r0, #2
            0xE1A0_F00E, // 38        MOV    pc, lr
        ];
        let o = unit(&words, &[(0, 'a')], &[("Entry", 0), ("Loop", 4), ("One", 0x24), ("Two", 0x2C), ("Poll", 0x34)], &["Entry"])
            .unwrap_or_else(|e| panic!("{e:?}"));
        let c = &o.c;
        assert!(c.contains("t0_One(s") && c.contains("t0_Two(s"), "handlers called: {c}");
        assert!(c.matches("ros_check_return(s, 0xFC000004u);").count() >= 2, "returning to Loop: {c}");
        let body = &c[c.find("static void t0_Entry(struct ros_cpu *s)\n{").unwrap()..];
        let body = &body[..body.find("\n}\n").unwrap()];
        assert!(!body.contains("return;"), "the loop never returns: {body}");
    }

    #[test]
    fn a_table_of_calls_after_mov_lr_pc() {
        // The Pinboard's ReadBufferedList: `MOV LR,PC`,
        // `ADD PC,PC,R0,ASL #2`, then `B %BT01`, which is where lr points
        // and is the loop. Then a row for each buffered action,
        // `B AddIconXY` ..., and each handler returns through lr to the
        // `B %BT01`. The rows are calls, not jumps: a handler's
        // `MOV PC,LR` is not the routine's return.
        let words = [
            0xE92D_4002, // 00 Entry  STMFD  sp!, {r1, lr}
            0xE3A0_2000, // 04        MOV    r2, #0
            0xE491_0004, // 08 Loop   LDR    r0, [r1], #4
            0xE370_0002, // 0C        CMN    r0, #2
            0x08BD_8002, // 10        LDMEQFD sp!, {r1, pc}
            0xE1A0_E00F, // 14        MOV    lr, pc
            0xE08F_F100, // 18        ADD    pc, pc, r0, LSL #2
            0xEAFF_FFF9, // 1C        B      Loop
            0xEA00_0000, // 20        B      One          ; 0
            0xEA00_0001, // 24        B      Ten          ; 1
            0xE282_2001, // 28 One    ADD    r2, r2, #1
            0xE1A0_F00E, // 2C        MOV    pc, lr
            0xE282_200A, // 30 Ten    ADD    r2, r2, #10
            0xE1A0_F00E, // 34        MOV    pc, lr
        ];
        let o = unit(&words, &[(0, 'a')], &[("Entry", 0), ("Loop", 8), ("One", 0x28), ("Ten", 0x30)], &["Entry"])
            .unwrap_or_else(|e| panic!("{e:?}"));
        let c = &o.c;
        assert!(c.contains("t0_One(s") && c.contains("t0_Ten(s"), "handlers called: {c}");
        assert_eq!(c.matches("ros_check_return(s, 0xFC00001Cu);").count(), 2, "each row a call back to B Loop: {c}");
        let body = &c[c.find("static void t0_Entry(struct ros_cpu *s)\n{").unwrap()..];
        let body = &body[..body.find("\n}\n").unwrap()];
        assert!(!body.contains("goto One;") && !body.contains("r2 += 10;"), "no handler inlined as a jump: {body}");
        assert!(!body.contains("R[15] = r14;"), "the only return is the LDMEQFD's: {body}");
        assert!(!c.contains("ros_check_return(s, 0xFC000008u);"), "B Loop, where lr points, is the loop, not a call: {c}");
    }

    #[test]
    fn a_row_of_a_table_of_calls_to_the_range_checks_exit_is_a_jump() {
        // SCSIFS's MiscEntry: `CMPS R0,#MiscOp_DriveStatus`, `BHI %FT95`,
        // `MOV LR,PC`, `ADD PC,PC,R0,LSL #2`, `B %FT90`, then the rows.
        // The rows call the reasons' handlers, which return to the
        // `B %FT90`. But the row for one reason, kept unused, is
        // `B %FT95`. That is where the range check sends an index out of
        // range: the error and the routine's own `Pull "PC"`. It is a
        // jump, not a call. A call would check for a return to the
        // `B %FT90` that never comes.
        let words = [
            0xE52D_E004, // 00 Entry  STR    lr, [sp, #-4]!
            0xE350_0002, // 04        CMP    r0, #2
            0x8A00_0006, // 08        BHI    Err
            0xE1A0_E00F, // 0C        MOV    lr, pc
            0xE08F_F100, // 10        ADD    pc, pc, r0, LSL #2
            0xEA00_0002, // 14        B      Done
            0xEA00_0004, // 18        B      A            ; 0
            0xEA00_0001, // 1C        B      Err          ; 1, kept unused
            0xEA00_0004, // 20        B      C            ; 2
            0xE49D_F004, // 24 Done   LDR    pc, [sp], #4
            0xE3A0_0063, // 28 Err    MOV    r0, #99
            0xEAFF_FFFC, // 2C        B      Done
            0xE3A0_000A, // 30 A      MOV    r0, #10
            0xE1A0_F00E, // 34        MOV    pc, lr
            0xE3A0_000C, // 38 C      MOV    r0, #12
            0xE1A0_F00E, // 3C        MOV    pc, lr
        ];
        let labels = [("Entry", 0), ("Done", 0x24), ("Err", 0x28), ("A", 0x30), ("C", 0x38)];
        let o = unit(&words, &[(0, 'a')], &labels, &["Entry"]).unwrap_or_else(|e| panic!("{e:?}"));
        let c = &o.c;
        assert!(c.contains("t0_A(s") && c.contains("t0_C(s"), "rows 0 and 2 called: {c}");
        assert_eq!(c.matches("ros_check_return(s, 0xFC000014u);").count(), 2, "only rows 0 and 2 calls: {c}");
        assert!(!c.contains("t0_Err(s"), "row 1, B Err, a jump to the range check's exit: {c}");
    }

    #[test]
    fn a_table_of_calls_the_return_runs_on_into() {
        // SpriteExtend's converttrans_new: `MOV LR,PC`,
        // `ADD PC,PC,R9,LSL #2`, then the word the calls come back to (a
        // STR there), then `B converttrans_new`, which is both the loop
        // and row 0. The rows after it are calls. The code the calls come
        // back to runs on into `B Loop`, so that is a jump, not a call of
        // Loop.
        let words = [
            0xE92D_4000, // 00 Entry  STMFD  sp!, {lr}
            0xE258_8001, // 04 Loop   SUBS   r8, r8, #1
            0x48BD_8000, // 08        LDMMIFD sp!, {pc}
            0xE1A0_E00F, // 0C        MOV    lr, pc
            0xE08F_F109, // 10        ADD    pc, pc, r9, LSL #2
            0xE082_2000, // 14        ADD    r2, r2, r0       ; the calls come back here
            0xEAFF_FFF9, // 18        B      Loop             ; and on to the loop (row 0)
            0xEA00_0000, // 1C        B      One              ; row 1
            0xEA00_0001, // 20        B      Ten              ; row 2
            0xE3A0_0001, // 24 One    MOV    r0, #1
            0xE1A0_F00E, // 28        MOV    pc, lr
            0xE3A0_000A, // 2C Ten    MOV    r0, #10
            0xE1A0_F00E, // 30        MOV    pc, lr
        ];
        let o = unit(&words, &[(0, 'a')], &[("Entry", 0), ("Loop", 4), ("One", 0x24), ("Ten", 0x2C)], &["Entry"])
            .unwrap_or_else(|e| panic!("{e:?}"));
        let c = &o.c;
        assert!(c.contains("t0_One(s") && c.contains("t0_Ten(s"), "rows 1 and 2 called: {c}");
        assert_eq!(c.matches("ros_check_return(s, 0xFC000014u);").count(), 2, "each a call back to the ADD: {c}");
        assert!(!c.contains("t0_Loop"), "B Loop, run on into, is the loop, not a call: {c}");
    }

    #[test]
    fn a_call_through_a_popped_address() {
        // The Filer's BL_Wimp: push the routine's address, MOV R14,PC,
        // Pull PC. It is a call that returns to the NOP after it.
        let words = [
            0xE92D_4000, // 00 Entry  STMFD  sp!, {lr}
            0xE28F_0010, // 04        ADR    r0, Sub
            0xE52D_0004, // 08        STR    r0, [sp, #-4]!
            0xE1A0_E00F, // 0C        MOV    lr, pc
            0xE49D_F004, // 10        LDR    pc, [sp], #4
            0xE1A0_0000, // 14        MOV    r0, r0
            0xE8BD_8000, // 18        LDMFD  sp!, {pc}
            0xE3A0_1005, // 1C Sub    MOV    r1, #5
            0xE1A0_F00E, // 20        MOV    pc, lr
        ];
        let o = unit(&words, &[(0, 'a')], &[("Entry", 0), ("Sub", 0x1C)], &["Entry"]).unwrap_or_else(|e| panic!("{e:?}"));
        let c = &o.c;
        assert!(c.contains("ros_check_return(s, 0xFC000014u);"), "a call returning to the NOP: {c}");
        let body = &c[c.find("static void t0_Entry(struct ros_cpu *s)\n{").unwrap()..];
        let body = &body[..body.find("\n}\n").unwrap()];
        assert_eq!(body.matches("return;").count(), 1, "one return, the LDMFD's: {body}");
    }

    #[test]
    fn a_chain_of_calls_after_one_adr_lr() {
        // Wimp_TextOp: `ADR lr, Back`, then conditional branches to the
        // handlers. Each is a call that returns to Back.
        let words = [
            0xE92D_4000, // 00 Entry  STMFD  sp!, {lr}
            0xE28F_E010, // 04        ADR    lr, Back
            0xE350_0001, // 08        CMP    r0, #1
            0x3A00_0003, // 0C        BLO    Zero
            0x0A00_0004, // 10        BEQ    One
            0x8A00_0005, // 14        BHI    Two
            0xE3A0_0009, // 18        MOV    r0, #9
            0xE8BD_8000, // 1C Back   LDMFD  sp!, {pc}
            0xE3A0_000A, // 20 Zero   MOV    r0, #10
            0xE1A0_F00E, // 24        MOV    pc, lr
            0xE3A0_000B, // 28 One    MOV    r0, #11
            0xE1A0_F00E, // 2C        MOV    pc, lr
            0xE3A0_000C, // 30 Two    MOV    r0, #12
            0xE1A0_F00E, // 34        MOV    pc, lr
        ];
        let o = unit(
            &words,
            &[(0, 'a')],
            &[("Entry", 0), ("Back", 0x1C), ("Zero", 0x20), ("One", 0x28), ("Two", 0x30)],
            &["Entry"],
        )
        .unwrap_or_else(|e| panic!("{e:?}"));
        assert_eq!(o.c.matches("ros_check_return(s, 0xFC00001Cu);").count(), 3, "each branch a call to Back: {}", o.c);
        assert!(o.c.contains("t0_Two(s, &r0, r14);"), "{}", o.c);
    }

    #[test]
    fn a_branch_at_the_label_of_a_chain_is_not_a_call() {
        // Wimp_TextOp's `90 B ExitWimp`. The calls return to it, and it is
        // the jump out, not a call that returns to itself.
        let words = [
            0xE92D_4000, // 00 Entry  STMFD  sp!, {lr}
            0xE28F_E008, // 04        ADR    lr, Back
            0xE350_0001, // 08        CMP    r0, #1
            0x3A00_0001, // 0C        BLO    Zero
            0x0A00_0002, // 10        BEQ    One
            0xEA00_0003, // 14 Back   B      Exit
            0xE3A0_000A, // 18 Zero   MOV    r0, #10
            0xE1A0_F00E, // 1C        MOV    pc, lr
            0xE3A0_000B, // 20 One    MOV    r0, #11
            0xE1A0_F00E, // 24        MOV    pc, lr
            0xE1A0_500E, // 28 Exit   MOV    r5, lr
            0xEF02_0010, // 2C        SWI    XOS_EnterOS
            0xE1A0_F005, // 30        MOV    pc, r5
        ];
        let o = unit(
            &words,
            &[(0, 'a')],
            &[("Entry", 0), ("Back", 0x14), ("Zero", 0x18), ("One", 0x20), ("Exit", 0x28)],
            &["Entry", "Exit"],
        )
        .unwrap_or_else(|e| panic!("{e:?}"));
        assert_eq!(o.c.matches("ros_check_return(s, 0xFC000014u);").count(), 2, "the two calls only: {}", o.c);
        assert!(o.c.contains("ROS_TAIL_CALL(t0_Exit)"), "{}", o.c);
    }

    #[test]
    fn a_chain_of_calls_back_to_the_top_of_a_poll_loop() {
        // The Resource Filer's poll loop. Its label is before `ADR lr`.
        // Each conditional branch is a call that returns there, and
        // `B repollwimp` is the loop itself.
        let words = [
            0xE92D_4000, // 00 Entry  STMFD  sp!, {lr}
            0xE591_0000, // 04 Poll   LDR    r0, [r1]
            0xE350_0000, // 08        CMP    r0, #0
            0x08BD_8000, // 0C        LDMEQFD sp!, {pc}
            0xE24F_E014, // 10        ADR    lr, Poll
            0xE350_0001, // 14        CMP    r0, #1
            0x0A00_0002, // 18        BEQ    One
            0xE350_0002, // 1C        CMP    r0, #2
            0x0A00_0002, // 20        BEQ    Two
            0xEAFF_FFF6, // 24        B      Poll
            0xE3A0_200B, // 28 One    MOV    r2, #11
            0xE1A0_F00E, // 2C        MOV    pc, lr
            0xE3A0_200C, // 30 Two    MOV    r2, #12
            0xE1A0_F00E, // 34        MOV    pc, lr
        ];
        let o = unit(
            &words,
            &[(0, 'a')],
            &[("Entry", 0), ("Poll", 0x04), ("One", 0x28), ("Two", 0x30)],
            &["Entry"],
        )
        .unwrap_or_else(|e| panic!("{e:?}"));
        assert_eq!(o.c.matches("ros_check_return(s, 0xFC000004u);").count(), 2, "each branch a call to Poll: {}", o.c);
        assert!(!o.c.contains("ros_fault"), "{}", o.c);
    }

    #[test]
    fn a_resumable_call_after_adr_lr_goes_back_to_the_label() {
        // Wimp_TextOp's calls, as they became once their callees could be
        // returned into later. Each is a resume point, followed by the
        // jump to Back, not the next reason's call.
        let words = [
            0xE92D_4000, // 00 Entry  STMFD  sp!, {lr}
            0xE28F_E00C, // 04        ADR    lr, Back
            0xE350_0001, // 08        CMP    r0, #1
            0x0A00_0002, // 0C        BEQ    One
            0xE3A0_0009, // 10        MOV    r0, #9
            0xE3A0_0008, // 14        MOV    r0, #8
            0xE8BD_8000, // 18 Back   LDMFD  sp!, {pc}
            0xE1A0_500E, // 1C One    MOV    r5, lr
            0xEF02_0010, // 20        SWI    XOS_EnterOS
            0xE1A0_F005, // 24        MOV    pc, r5
        ];
        let o = unit(&words, &[(0, 'a')], &[("Entry", 0), ("Back", 0x18), ("One", 0x1C)], &["Entry"])
            .unwrap_or_else(|e| panic!("{e:?}"));
        let c = &o.c;
        let call = c.find("rs.at = 0xFC000018u;").unwrap_or_else(|| panic!("a resume point: {c}"));
        let after = &c[call..];
        let back = after.find("goto Back;").unwrap_or(usize::MAX);
        let nine = after.find("r0 = 9;").unwrap_or(usize::MAX);
        assert!(back < nine, "back to Back before the next instruction: {c}");
    }

    #[test]
    fn an_address_put_in_lr_and_stored() {
        // Wimp_StartTask: `ADR R14,runthetask` then `STR R14,[R5,#60]`.
        // The address is a task's PC, not a return address.
        let words = [
            0xE28F_E008, // 00 Entry  ADR    lr, Run
            0xE585_E03C, // 04        STR    lr, [r5, #60]
            0xE1A0_F00E, // 08        MOV    pc, lr
            0xE3A0_0001, // 0C        MOV    r0, #1 (unreached)
            0xE3A0_0007, // 10 Run    MOV    r0, #7
            0xE1A0_F00E, // 14        MOV    pc, lr
        ];
        let o = unit(&words, &[(0, 'a')], &[("Entry", 0), ("Run", 0x10)], &["Entry"]).unwrap_or_else(|e| panic!("{e:?}"));
        assert!(o.c.contains("{ 0xFC000010u, t0_Run"), "Run an entry: {}", o.c);
    }

    #[test]
    fn a_table_of_offsets_from_each_word() {
        // The Wimp's defaulthandlers: `DCD Do_ErrorHandler-.-4`. The word
        // is added to the address after it, and the result is given to
        // OS_ChangeEnvironment.
        let words = [
            0xE28F_1004, // 00 Entry   ADR    r1, Table
            0xE491_2004, // 04         LDR    r2, [r1], #4
            0xE081_0002, // 08         ADD    r0, r1, r2
            0xE1A0_F00E, // 0C         MOV    pc, lr
            0x0000_0004, // 10 Table   DCD    Handler-.-4
            0x0000_0000, // 14         DCD    0
            0xE3A0_0007, // 18 Handler MOV    r0, #7
            0xE1A0_F00E, // 1C         MOV    pc, lr
        ];
        let o = unit_src(
            &words,
            &[(0, 'a'), (0x10, 'd'), (0x18, 'a')],
            &[("Entry", 0), ("Table", 0x10), ("Handler", 0x18)],
            &[(0x10, "        DCD     Handler-.-4     ; the error handler"), (0x14, "        DCD     0")],
        )
        .unwrap_or_else(|e| panic!("{e:?}"));
        assert!(o.c.contains("{ 0xFC000018u, t0_Handler"), "Handler an entry: {}", o.c);
    }

    #[test]
    fn an_fpa_instruction_assembled_as_data() {
        // saveFPregs: RFS, then its SFM as a DCD, then code again.
        let words = [
            0xEE30_1110, // 00 Entry  RFS    r1
            0xED82_0200, // 04        DCD    &ED820200 (SFM f0, 4, [r2])
            0xE1A0_F00E, // 08        MOV    pc, lr
        ];
        let o = unit(&words, &[(0, 'a'), (4, 'd'), (8, 'a')], &[("Entry", 0)], &["Entry"]).unwrap_or_else(|e| panic!("{e:?}"));
        assert_eq!(o.instructions, 3, "the DCD is an instruction: {}", o.c);
        assert!(!o.c.contains("not code"), "{}", o.c);
    }

    #[test]
    fn a_table_of_offsets_between_labels() {
        // The Wimp's filtertable: `& prefilter_default - filtertable`. The
        // word is added to the table's address and jumped to.
        let words = [
            0xE28F_1008, // 00 Entry   ADR    r1, Table
            0xE791_2100, // 04         LDR    r2, [r1, r0, LSL #2]
            0xE081_F002, // 08         ADD    pc, r1, r2
            0xE1A0_F00E, // 0C         MOV    pc, lr
            0x0000_0008, // 10 Table   & Handler - Table
            0xFFFF_FFFF, // 14         & -1
            0xE3A0_0007, // 18 Handler MOV    r0, #7
            0xE1A0_F00E, // 1C         MOV    pc, lr
        ];
        let o = unit_src(
            &words,
            &[(0, 'a'), (0x10, 'd'), (0x18, 'a')],
            &[("Entry", 0), ("Table", 0x10), ("Handler", 0x18)],
            &[(0x10, "        & Handler - Table"), (0x14, "        & -1")],
        )
        .unwrap_or_else(|e| panic!("{e:?}"));
        assert!(o.c.contains("{ 0xFC000018u, t0_Handler"), "Handler an entry: {}", o.c);
    }

    #[test]
    fn a_jump_table_a_call_and_data_in_code() {
        let words = [
            0xE350_0002, // 00 Entry  CMP    r0, #2
            0x308F_F100, // 04        ADDLO  pc, pc, r0, LSL #2
            0xEA00_0005, // 08        B      Bad
            0xEA00_0000, // 0C        B      Zero
            0xEA00_0001, // 10        B      One
            0xE3A0_000A, // 14 Zero   MOV    r0, #10
            0xE1A0_F00E, // 18        MOV    pc, lr
            0xE3A0_000B, // 1C One    MOV    r0, #11
            0xE1A0_F00E, // 20        MOV    pc, lr
            0xEB00_0001, // 24 Bad    BL     Helper
            0xEF02_0002, // 28        SWI    XOS_Write0
            0x1234_5678, // 2C        DCD    &12345678
            0xE3A0_0000, // 30 Helper MOV    r0, #0
            0xE1A0_F00E, // 34        MOV    pc, lr
        ];
        let o = unit(
            &words,
            &[(0, 'a'), (0x2C, 'd'), (0x30, 'a')],
            &[("Entry", 0), ("Zero", 0x14), ("One", 0x1C), ("Bad", 0x24), ("Helper", 0x30)],
            &["Entry"],
        )
        .unwrap_or_else(|e| panic!("{e:?}"));
        assert_eq!(o.regions, 2, "Entry, and Helper because it is called");
        let c = &o.c;
        assert!(c.contains("static void t0_Entry(struct ros_cpu *s)"));
        // Only BL reaches Helper, so its registers are its parameters, and
        // r0 is given back through a pointer.
        assert!(c.contains("static void t0_Helper(struct ros_cpu *s, uint32_t *p0, uint32_t r14)"), "{c}");
        assert!(c.contains("    r14 = 0xFC000028u;"));
        assert!(c.contains("    t0_Helper(s, &r0, r14);"));
        assert!(c.contains("    *p0 = r0;"));
        assert!(!c.contains("t0_Helper, \"t0:Helper\""), "not registered: nothing takes its address");
        // Structured, with no labels and no gotos: the table inside its
        // bound check, each case in place, and after it what the bound
        // leaves out.
        assert!(c.contains("    if (r0 < 2) {"), "{c}");
        assert!(c.contains("        switch (r0) {"));
        assert!(c.contains("        case 0:\n"));
        assert!(c.contains("            r0 = 11;"));
        assert!(!c.contains("goto"));
        // The call's return is checked.
        assert!(c.contains("ros_check_return(s, 0xFC000028u);"));
        // A native SWI goes straight to its thunk. The word after it is
        // data.
        assert!(c.contains("ros_native_swi(s, ros_thunk_OS_Write0);"));
        assert!(c.contains("ros_fault(s, 0xFC00002Cu, \"a transfer to an address that is not code\");"));
        assert!(o.h.contains("#define T0_Helper 0xFC000030u"));
    }

    #[test]
    fn a_jump_table_through_lr() {
        // The Screen Blanker's SWI dispatch: lr is set to the case beside
        // the ADD, then MOV pc, lr. It is a table on r0, not a return.
        let words = [
            0xE52D_E004, // 00 Entry  STR    lr, [sp, #-4]!
            0xE350_0001, // 04        CMP    r0, #1
            0x8A00_0003, // 08        BHI    Bad
            0xE08F_E100, // 0C        ADD    lr, pc, r0, LSL #2
            0xE1A0_F00E, // 10        MOV    pc, lr
            0xEA00_0002, // 14        B      Zero
            0xEA00_0003, // 18        B      One
            0xE3A0_0063, // 1C Bad    MOV    r0, #99
            0xE49D_F004, // 20        LDR    pc, [sp], #4
            0xE3A0_000A, // 24 Zero   MOV    r0, #10
            0xE49D_F004, // 28        LDR    pc, [sp], #4
            0xE3A0_000B, // 2C One    MOV    r0, #11
            0xE49D_F004, // 30        LDR    pc, [sp], #4
        ];
        let labels = [("Entry", 0), ("Bad", 0x1C), ("Zero", 0x24), ("One", 0x2C)];
        for lift in [false, true] {
            let o = unit_lift(&words, &[(0, 'a')], &labels, &["Entry"], lift).unwrap_or_else(|e| panic!("{e:?}"));
            let c = &o.c;
            assert!(c.contains("switch ((int32_t)"), "{c}");
            assert!(c.contains("0xFC000014u + (") || c.contains("0xFC000014u+("), "lr is still set: {c}");
            assert!(c.contains("= 11;") && c.contains("= 10;"), "both cases reached: {c}");
            assert!(!c.contains("ros_check_return(s, 0xFC000014u)"), "not a return to the table: {c}");
        }
    }

    #[test]
    fn a_jump_table_counted_down() {
        // The Wimp's window furniture: ADR lr, Jump-8 then
        // SUB pc, lr, r4, LSL #2 on the negative icon number. -2 is the
        // first row and -3 the next. A number kept unused jumps to 0.
        let words = [
            0xE52D_E004, // 00 Entry  STR    lr, [sp, #-4]!
            0xE24F_E008, // 04        ADR    lr, Jump-8
            0xE04E_F104, // 08        SUB    pc, lr, r4, LSL #2
            0xEA00_0002, // 0C Jump   B      Back
            0xEA00_0003, // 10        B      Quit
            0xE3A0_F000, // 14        MOV    pc, #0
            0xEA00_0003, // 18        B      Last
            0xE3A0_000A, // 1C Back   MOV    r0, #10
            0xE49D_F004, // 20        LDR    pc, [sp], #4
            0xE3A0_000B, // 24 Quit   MOV    r0, #11
            0xE49D_F004, // 28        LDR    pc, [sp], #4
            0xE3A0_000C, // 2C Last   MOV    r0, #12
            0xE49D_F004, // 30        LDR    pc, [sp], #4
        ];
        let labels = [("Entry", 0), ("Jump", 0x0C), ("Back", 0x1C), ("Quit", 0x24), ("Last", 0x2C)];
        for lift in [false, true] {
            let o = unit_lift(&words, &[(0, 'a')], &labels, &["Entry"], lift).unwrap_or_else(|e| panic!("{e:?}"));
            let c = &o.c;
            assert!(c.contains("switch ((int32_t)"), "{c}");
            for k in ["case -2:", "case -3:", "case -4:", "case -5:"] {
                assert!(c.contains(k), "{k}: {c}");
            }
            assert!(!c.contains("case -1:") && !c.contains("case -6:"), "{c}");
            assert!(c.contains("= 10;") && c.contains("= 11;") && c.contains("= 12;"), "every row reached: {c}");
        }
    }

    #[test]
    fn a_jump_table_counted_down_whose_last_row_branches_to_the_next_instruction() {
        // As the Wimp's furniture table, but the last row is a `B` to the
        // code just after the table. It is still a row (case -5), and runs
        // on into that code.
        let words = [
            0xE52D_E004, // 00 Entry  STR    lr, [sp, #-4]!
            0xE24F_E008, // 04        ADR    lr, Jump-8
            0xE04E_F104, // 08        SUB    pc, lr, r4, LSL #2
            0xEA00_0004, // 0C Jump   B      Back
            0xEA00_0005, // 10        B      Quit
            0xE3A0_F000, // 14        MOV    pc, #0
            0xEAFF_FFFF, // 18        B      Last (the next instruction)
            0xE3A0_000C, // 1C Last   MOV    r0, #12
            0xE49D_F004, // 20        LDR    pc, [sp], #4
            0xE3A0_000A, // 24 Back   MOV    r0, #10
            0xE49D_F004, // 28        LDR    pc, [sp], #4
            0xE3A0_000B, // 2C Quit   MOV    r0, #11
            0xE49D_F004, // 30        LDR    pc, [sp], #4
        ];
        let labels = [("Entry", 0), ("Jump", 0x0C), ("Last", 0x1C), ("Back", 0x24), ("Quit", 0x2C)];
        for lift in [false, true] {
            let o = unit_lift(&words, &[(0, 'a')], &labels, &["Entry"], lift).unwrap_or_else(|e| panic!("{e:?}"));
            let c = &o.c;
            for k in ["case -2:", "case -3:", "case -4:", "case -5:"] {
                assert!(c.contains(k), "lift {lift}, {k}: {c}");
            }
            assert!(c.contains("= 10;") && c.contains("= 11;") && c.contains("= 12;"), "every row reached: {c}");
        }
    }

    #[test]
    fn a_padded_run_of_branches_whose_last_branches_to_the_next_instruction() {
        // As the Wimp's border-icon table: `ADD pc, pc, rm, LSL #2`, a word
        // that is not code where index 0 would go, then B rows. The last is
        // a `B` to the code just after the run, and is still a row
        // (index 2).
        let words = [
            0xE08F_F100, // 00 Entry  ADD    pc, pc, r0, LSL #2
            0x0000_0000, // 04        DCD    0 (the unused row, and -1's)
            0x0000_0000, // 08        DCD    0
            0xEA00_0002, // 0C        B      One
            0xEAFF_FFFF, // 10        B      Two (the next instruction)
            0xE3A0_0002, // 14 Two    MOV    r0, #2
            0xE1A0_F00E, // 18        MOV    pc, lr
            0xE3A0_0001, // 1C One    MOV    r0, #1
            0xE1A0_F00E, // 20        MOV    pc, lr
        ];
        let marks = [(0, 'a'), (0x04, 'd'), (0x0C, 'a')];
        let labels = [("Entry", 0), ("Two", 0x14), ("One", 0x1C)];
        let o = unit(&words, &marks, &labels, &["Entry"]).unwrap_or_else(|e| panic!("{e:?}"));
        let c = &o.c;
        assert!(c.contains("switch ("), "{c}");
        assert!(c.contains("case 1:") && c.contains("case 2:"), "both rows cases: {c}");
        assert!(c.contains("= 1;") && c.contains("= 2;"), "both rows reached: {c}");
    }

    #[test]
    fn a_link_kept_in_a_register_over_a_swi_is_returned_to() {
        // The Filer's FreeWorkspace keeps its lr in r5 over XOS_Module,
        // then does MOV pc, r5 back to its caller. The caller's call leaves
        // a resume point, in both tiers.
        let words = [
            0xE52D_E004, // 00 Entry  STR    lr, [sp, #-4]!
            0xEB00_0000, // 04        BL     Free
            0xE49D_F004, // 08        LDR    pc, [sp], #4
            0xE1A0_500E, // 0C Free   MOV    r5, lr
            0xE3A0_0007, // 10        MOV    r0, #7
            0xEF02_001E, // 14        SWI    XOS_Module
            0xE1A0_F005, // 18        MOV    pc, r5
        ];
        for lift in [false, true] {
            let o = unit_lift(&words, &[(0, 'a')], &[("Entry", 0), ("Free", 0x0C)], &["Entry"], lift)
                .unwrap_or_else(|e| panic!("{e:?}"));
            assert!(o.c.contains("rs.at = 0xFC000008u;"), "lift {lift}: {}", o.c);
        }
    }

    #[test]
    fn constants_by_the_names_the_source_gave_them() {
        assert_eq!(named_immediates("Reg     TST     r0, #b_WordAligned ; #NotThis"), vec!["b_WordAligned"]);
        assert_eq!(named_immediates("        LDR     r2, BufferBlockAt"), vec!["BufferBlockAt"]);
        assert_eq!(named_immediates("        LDR     r0, [r12, #WsFlags]!"), vec!["WsFlags"]);
        assert_eq!(named_immediates("        LDR     r0, =Service_Reset"), vec!["Service_Reset"]);
        // Expressions and registers are not the names of single constants.
        assert!(named_immediates("        ADD     r0, r1, #Size + 4").is_empty());
        assert!(named_immediates("        MOV     r0, sp").is_empty());
        assert!(named_immediates("        ADD     r0, r1, r2").is_empty());
        // A name that already means something to C, its headers or the
        // code stays a number.
        assert!(nameable("V_bit", "t0") && nameable("Service_Reset", "t0"));
        for n in ["sp", "wp", "exp", "floorf", "v12", "r3", "ros_x", "t0_Entry", "T0_Entry", "INT32_MAX", "size_t", "if", "mode"] {
            assert!(!nameable(n, "t0"), "{n}");
        }
    }

    #[test]
    fn whole_line_comments_go_with_the_code_after_them() {
        let l = |text: &'static str, file: &'static str, top: bool, code: bool, data: bool| Listed { text, file, top, code, data };
        let lines = [
            l("; hdr.s: a header", "hdr", true, false, false),
            l("Size    * 4", "hdr", true, false, false),
            l("; the file's own words, then a definition", "a.s", true, false, false),
            l("        AREA    |C$$Code|, CODE", "a.s", true, false, false),
            l("; ---------------------------------------", "a.s", true, false, false),
            l(";\tWalk the list.", "a.s", true, false, false),
            l(";", "a.s", true, false, false),
            l(";\t  r0 -> the head", "a.s", true, false, false),
            l("", "a.s", true, false, false),
            l("Walk    ROUT", "a.s", true, false, false),
            l("        LDR     r0, [r0]", "a.s", true, true, false),
            l("; a table", "a.s", true, false, false),
            l("        DCD     1", "a.s", true, false, true),
            l("; a macro's", "m.s", false, false, false),
            l("; then a call", "a.s", true, false, false),
            l("        Push    lr", "a.s", true, false, false),
            l("        STMFD   sp!, {lr}", "m.s", false, true, false),
        ];
        let n = notes_before(&lines);
        assert_eq!(n.len(), 2, "{n:?}");
        // The block's own indentation goes. The author's layout inside it
        // stays.
        assert_eq!(n[&10], vec!["Walk the list.", "", "  r0 -> the head"]);
        assert_eq!(n[&16], vec!["then a call"]);
        assert_eq!(note_lines(&n[&10]), vec!["/* Walk the list.", " *", " *   r0 -> the head */"]);
        assert_eq!(note_lines(&["a /* b */ c??/".into()]), vec!["/* a / * b * / c? ?/ */"]);
    }

    #[test]
    fn loops_and_guards_not_gotos() {
        // A count down, with its test at the bottom.
        let words = [
            0xE3A0_2000, // 00 Entry  MOV    r2, #0
            0xE082_2001, // 04 Loop   ADD    r2, r2, r1
            0xE251_1001, // 08        SUBS   r1, r1, #1
            0x1AFF_FFFC, // 0C        BNE    Loop
            0xE1A0_F00E, // 10        MOV    pc, lr
        ];
        let o = unit(&words, &[(0, 'a')], &[("Entry", 0), ("Loop", 4)], &["Entry"]).unwrap_or_else(|e| panic!("{e:?}"));
        assert!(o.c.contains("    do {\n"), "{}", o.c);
        assert!(o.c.contains("    } while (r1 != 0);"));
        assert!(!o.c.contains("goto"));
        // A search, with two ways out of the middle, each a guard. The
        // result is set under the same condition as the return, so it
        // joins the return.
        let words = [
            0xE201_103F, // 00 Entry  AND    r1, r1, #63
            0xE3A0_2000, // 04        MOV    r2, #0
            0xE152_0001, // 08 Loop   CMP    r2, r1
            0xA3E0_0000, // 0C        MVNGE  r0, #0
            0xA1A0_F00E, // 10        MOVGE  pc, lr
            0xE282_2001, // 14        ADD    r2, r2, #1
            0xEAFF_FFFA, // 18        B      Loop
        ];
        let o = unit(&words, &[(0, 'a')], &[("Entry", 0), ("Loop", 8)], &["Entry"]).unwrap_or_else(|e| panic!("{e:?}"));
        let c = &o.c;
        assert!(c.contains("    for (;;) {\n"), "{c}");
        assert!(c.contains("        if ((int32_t)r2 >= (int32_t)r1) {\n            r0 = 0xFFFFFFFFu;"));
        assert!(!c.contains("goto") && !c.contains("Loop:"));
    }

    #[test]
    fn flags_across_calls() {
        // Sub is only called here, and its caller sets the flags before it
        // reads them. So Sub's compare need not set any.
        let words = [
            0xE92D_4000, // 00 Entry  STMFD  sp!, {lr}
            0xEB00_0002, // 04        BL     Sub
            0xE351_0000, // 08        CMP    r1, #0
            0x03A0_2001, // 0C        MOVEQ  r2, #1
            0xE8BD_8000, // 10        LDMFD  sp!, {pc}
            0xE350_0005, // 14 Sub    CMP    r0, #5
            0x03A0_0001, // 18        MOVEQ  r0, #1
            0xE1A0_F00E, // 1C        MOV    pc, lr
        ];
        let o = unit(&words, &[(0, 'a')], &[("Entry", 0), ("Sub", 0x14)], &["Entry"]).unwrap_or_else(|e| panic!("{e:?}"));
        assert!(o.c.contains("if (r0 == 5) r0 = 1;"), "{}", o.c);
        assert!(!o.c.contains("ros_subs(s, r0, 5)"), "{}", o.c);
        // Entry's own flags are live where it returns, because it is
        // exported.
        assert!(o.c.contains("ros_subs(s, r1, 0)"), "{}", o.c);
        // Registers in locals. They are loaded where Entry starts. Before
        // the call, only what Sub reads is stored, and Sub never names sp.
        // They are stored again at the return.
        assert!(o.c.contains("uint32_t r1 = R[1], r2 = R[2], r13 = R[13], r14 = R[14];"), "{}", o.c);
        assert!(!o.c.contains("R[13] = r13;"), "{}", o.c);
        assert!(o.c.contains("R[2] = r2;"), "{}", o.c);
        // When Sub is exported, its callers are unknown, so its flags are
        // set.
        let o = unit(&words, &[(0, 'a')], &[("Entry", 0), ("Sub", 0x14)], &["Entry", "Sub"]).unwrap_or_else(|e| panic!("{e:?}"));
        assert!(o.c.contains("ros_subs(s, r0, 5)"), "{}", o.c);
    }

    #[test]
    fn an_adrl_takes_an_entry_and_a_26_bit_return_faults() {
        let words = [
            0xE28F_1000, // 00 Entry   ADD    r1, pc, #0      ; ADRL r1, Handler
            0xE281_1008, // 04         ADD    r1, r1, #8
            0xE1A0_F00E, // 08         MOV    pc, lr
            0xE1A0_F00E, // 0C         MOV    pc, lr
            0xE13F_000F, // 10 Handler TEQ    pc, pc
            0x18FD_8000, // 14         LDMNEFD sp!, {pc}^  ; 26-bit mode's return
            0xE1A0_F00E, // 18         MOV    pc, lr
        ];
        let labels = [("Entry", 0), ("Handler", 0x10)];
        let o = unit(&words, &[(0, 'a')], &labels, &["Entry"]).unwrap_or_else(|e| panic!("{e:?}"));
        assert_eq!(o.regions, 2, "Entry, and Handler because ADRL takes its address -- not &08");
        assert!(o.c.contains("t0_Handler, \"t0:Handler\""), "registered: {}", o.c);
        // When lifted, TEQ pc, pc is known to be equal, so the return is
        // dead code.
        assert!(!o.c.contains("ros_fault"), "{}", o.c);
        // Tier 0 compiles it, as a fault in case it runs.
        let o = unit_lift(&words, &[(0, 'a')], &labels, &["Entry"], false).unwrap_or_else(|e| panic!("{e:?}"));
        assert!(o.c.contains(
            "ros_fault(s, 0xFC000014u, \"an LDM restoring pc and the mode (^): an exception return, as in 26-bit mode\");"
        ), "{}", o.c);
    }

    #[test]
    fn an_lr_kept_in_a_register_across_a_swi() {
        // The Task Manager's freeworkspace keeps lr in r6 across SWIs, and
        // returns through it.
        let words = [
            0xE92D_4000, // 00 Entry  STMFD  sp!, {lr}
            0xEB00_0000, // 04        BL     Free
            0xE8BD_8000, // 08        LDMFD  sp!, {pc}
            0xE1A0_600E, // 0C Free   MOV    r6, lr
            0xEF02_0002, // 10        SWI    XOS_Write0
            0xE1A0_F006, // 14        MOV    pc, r6
        ];
        let labels = [("Entry", 0), ("Free", 0x0C)];
        // A SWI that the typed API does not define. r6 may be kept, so the
        // jump through it may be the return, and the BL leaves a resume
        // point.
        let o = unit_lift(&words, &[(0, 'a')], &labels, &["Entry"], false).unwrap_or_else(|e| panic!("{e:?}"));
        assert!(o.c.contains("rs.at = 0xFC000008u;"), "{}", o.c);
        assert!(o.c.contains("ros_resume(s, R[6]);"), "{}", o.c);
        // A SWI it defines, writing r0 only. r6 still holds lr, and the
        // jump is the return.
        let regs = HashMap::from([(0x2, (0x1, 0x1))]);
        let o = unit_swis(&words, &[(0, 'a')], &labels, &["Entry"], false, regs).unwrap_or_else(|e| panic!("{e:?}"));
        assert!(!o.c.contains("ros_resume(s, R[6]);"), "{}", o.c);
    }

    #[test]
    fn a_store_of_pc_goes_on() {
        // The Wimp's cachespritepixtable: STRNE PC, selecttable_args, then
        // the rest of the routine after it.
        let words = [
            0xE350_0000, // 00 Entry  CMP    r0, #0
            0x1581_F000, // 04        STRNE  pc, [r1]
            0xE3A0_0001, // 08        MOV    r0, #1
            0xE1A0_F00E, // 0C        MOV    pc, lr
        ];
        let o = unit_lift(&words, &[(0, 'a')], &[("Entry", 0)], &["Entry"], false).unwrap_or_else(|e| panic!("{e:?}"));
        assert!(o.c.contains("ros_st32(R[1], 0xFC00000Cu);"), "{}", o.c);
        assert!(o.c.contains("R[0] = 1;"), "the code after the store: {}", o.c);
    }

    #[test]
    fn an_address_put_in_lr_and_stored_is_an_entry() {
        // DragASprite's StartUp: ADRL r14, Plot; STR r14, dragstr +
        // dr_userDraw. The Wimp calls Plot later.
        let words = [
            0xE92D_4000, // 00 Entry  STMFD  sp!, {lr}
            0xE28F_E000, // 04        ADD    lr, pc, #0      ; ADRL lr, Plot
            0xE28E_E00C, // 08        ADD    lr, lr, #12
            0xE581_E000, // 0C        STR    lr, [r1]
            0xE8BD_8000, // 10        LDMFD  sp!, {pc}
            0xE1A0_F00E, // 14        MOV    pc, lr
            0xE3A0_0001, // 18 Plot   MOV    r0, #1
            0xE1A0_F00E, // 1C        MOV    pc, lr
        ];
        let labels = [("Entry", 0), ("Plot", 0x18)];
        let o = unit(&words, &[(0, 'a')], &labels, &["Entry"]).unwrap_or_else(|e| panic!("{e:?}"));
        assert_eq!(o.regions, 2, "Entry, and Plot because its address is stored");
        assert!(o.c.contains("t0_Plot, \"t0:Plot\""), "registered: {}", o.c);
    }

    #[test]
    fn a_code_variables_read_entry_is_an_entry() {
        // The Wimp's Wimp$State: ADR R1,CommandWindow_var;
        // MOV R4,#VarType_Code; SWI XOS_SetVarVal. The block's write entry
        // is MOV PC,LR. The kernel calls its read entry, the word after.
        let words = |r4: u32| {
            [
                0xE28F_1008,       // 00 Entry  ADD    r1, pc, #8      ; ADR r1, Var
                0xE3A0_4000 | r4,  // 04        MOV    r4, #r4
                0xEF02_0024,       // 08        SWI    XOS_SetVarVal
                0xE1A0_F00E,       // 0C        MOV    pc, lr
                0xE1A0_F00E,       // 10 Var    MOV    pc, lr          ; write
                0xE92D_4000,       // 14        STMFD  sp!, {lr}       ; read
                0xE3A0_0001,       // 18        MOV    r0, #1
                0xE8BD_8000,       // 1C        LDMFD  sp!, {pc}
            ]
        };
        let labels = [("Entry", 0), ("Var", 0x10)];
        let o = unit(&words(16), &[(0, 'a')], &labels, &["Entry"]).unwrap_or_else(|e| panic!("{e:?}"));
        assert_eq!(o.regions, 3, "Entry, Var, and Var's read entry: {}", o.c);
        assert!(o.c.contains("0xFC000014u"), "the read entry registered: {}", o.c);
        // A string variable's value is not code, so there is no entry past
        // it.
        let o = unit(&words(0), &[(0, 'a')], &labels, &["Entry"]).unwrap_or_else(|e| panic!("{e:?}"));
        assert_eq!(o.regions, 2, "{}", o.c);
    }

    #[test]
    fn what_it_cannot_model_is_an_error_where_it_can_run() {
        // MRC p15 is fine where nothing reaches it, and refused where
        // something does.
        let words = [0xE1A0_F00E, 0xEE11_0F10];
        assert!(unit(&words, &[(0, 'a')], &[("Entry", 0)], &["Entry"]).is_ok());
        let words = [0xEE11_0F10, 0xE1A0_F00E];
        let e = unit(&words, &[(0, 'a')], &[("Entry", 0)], &["Entry"]).err().expect("refused");
        assert!(e[0].contains("does not model"), "{e:?}");
        // So is a computed jump with no bound.
        let words = [0xE08F_F100, 0xE1A0_F00E];
        let e = unit(&words, &[(0, 'a')], &[("Entry", 0)], &["Entry"]).err().expect("refused");
        assert!(e[0].contains("no bound"), "{e:?}");
    }

    #[test]
    fn a_run_of_branches_may_hold_a_branch_to_the_next_word() {
        // The Pinboard's TinyDirs icon menu. Its one entry is a `B` to the
        // word after it, which decodes as a no-op.
        let words = [
            0xE591_E000, // 00 Entry  LDR    r14, [r1]
            0xE08F_F10E, // 04        ADD    pc, pc, r14, LSL #2
            0xE49D_F004, // 08        LDR    pc, [sp], #4
            0xEAFF_FFFF, // 0C        B      Quit
            0xE3A0_0001, // 10 Quit   MOV    r0, #1
            0xE1A0_F00E, // 14        MOV    pc, lr
        ];
        let o = unit(&words, &[(0, 'a')], &[("Entry", 0), ("Quit", 0x10)], &["Entry"])
            .unwrap_or_else(|e| panic!("{e:?}"));
        assert!(o.c.contains("switch ("), "{}", o.c);
    }

    #[test]
    fn a_pushed_pc_is_the_return_address_of_the_transfer_after_it() {
        // Free's CallEntry: Push "PC", LDR PC, [r14, #fs_entry], NOP. And
        // its ADFS entry's Push "PC", B adfs_GetName, MOV r0, r0. The
        // callee pops the pc that was pushed, which is eight past the
        // push: the word after the transfer. B writes no lr, and the
        // lifted code writes none. Tier 0 sets lr to that word, as for a
        // BL. Nothing in Free reads it, since each caller returns by its
        // own stacked lr.
        let direct = [
            0xE92D_4000, // 00 Entry  STMFD  sp!, {lr}
            0xE52D_F004, // 04        STR    pc, [sp, #-4]!
            0xEA00_0002, // 08        B      Sub
            0xE1A0_0000, // 0C        MOV    r0, r0
            0xE3A0_1001, // 10        MOV    r1, #1
            0xE8BD_8000, // 14        LDMFD  sp!, {pc}
            0xE3A0_0002, // 18 Sub    MOV    r0, #2
            0xE8BD_8000, // 1C        LDMFD  sp!, {pc}
        ];
        let indirect = [
            0xE92D_4000, // 00 Entry  STMFD  sp!, {lr}
            0xE52D_F004, // 04        STR    pc, [sp, #-4]!
            0xE592_F004, // 08        LDR    pc, [r2, #4]
            0xE1A0_0000, // 0C        MOV    r0, r0
            0xE3A0_1001, // 10        MOV    r1, #1
            0xE8BD_8000, // 14        LDMFD  sp!, {pc}
        ];
        for lift in [false, true] {
            let o = unit_lift(&direct, &[(0, 'a')], &[("Entry", 0), ("Sub", 0x18)], &["Entry"], lift)
                .unwrap_or_else(|e| panic!("{e:?}"));
            assert_eq!(o.regions, 2, "lift {lift}: Entry, and Sub because it is called: {}", o.c);
            assert!(o.c.contains("0xFC00000Cu);"), "lift {lift}: the pc pushed: {}", o.c);
            assert!(o.c.contains("t0_Sub(s"), "lift {lift}: {}", o.c);
            assert!(o.c.contains("ros_check_return(s, 0xFC00000Cu);"), "lift {lift}: {}", o.c);
            if lift {
                assert!(!o.c.contains("R[14] = 0xFC00000Cu;"), "B writes no lr: {}", o.c);
            }
            assert!(o.c.contains("= 1;"), "lift {lift}: the code after the call: {}", o.c);
            let o = unit_lift(&indirect, &[(0, 'a')], &[("Entry", 0)], &["Entry"], lift)
                .unwrap_or_else(|e| panic!("{e:?}"));
            assert!(o.c.contains("ros_check_return(s, 0xFC00000Cu);"), "lift {lift}: {}", o.c);
            assert!(o.c.contains("= 1;"), "lift {lift}: the code after the call: {}", o.c);
        }
    }

    #[test]
    fn a_dispatch_after_adr_lr_calls_and_returns_to_its_label() {
        // Free's poll loop: ADR LR, repollwimp, then a jump table. Its rows
        // are MOV PC, LR (to repollwimp) or B handler (a call that returns
        // there).
        let table = [
            0xE92D_4000, // 00 Entry  STMFD  sp!, {lr}
            0xE3A0_0001, // 04 Loop   MOV    r0, #1
            0xE24F_E00C, // 08        ADR    lr, Loop
            0xE350_0002, // 0C        CMP    r0, #2
            0x308F_F100, // 10        ADDCC  pc, pc, r0, LSL #2
            0xE1A0_F00E, // 14        MOV    pc, lr
            0xE1A0_F00E, // 18        MOV    pc, lr          ; row 0
            0xEA00_0000, // 1C        B      Handler         ; row 1
            0xE8BD_8000, // 20        LDMFD  sp!, {pc}
            0xE3A0_1007, // 24 Handler MOV   r1, #7
            0xE1A0_F00E, // 28        MOV    pc, lr
        ];
        // The Resource Filer's: ADR lr, repollwimp; CMP; BEQ handler.
        let beq = [
            0xE92D_4000, // 00 Entry  STMFD  sp!, {lr}
            0xE3A0_0001, // 04 Loop   MOV    r0, #1
            0xE24F_E00C, // 08        ADR    lr, Loop
            0xE350_0006, // 0C        CMP    r0, #6
            0x0A00_0000, // 10        BEQ    Click
            0xEAFF_FFFA, // 14        B      Loop
            0xE3A0_1007, // 18 Click  MOV    r1, #7
            0xE1A0_F00E, // 1C        MOV    pc, lr
        ];
        // The Wimp's Wimp_TextOp: ADR R14, %FT90, BEQ a reason's handler,
        // and at 90 a B out of the SWI, which is not a call.
        let forward = [
            0xE92D_4000, // 00 Entry  STMFD  sp!, {lr}
            0xE28F_E008, // 04        ADR    lr, Out
            0xE350_0001, // 08        CMP    r0, #1
            0x0A00_0002, // 0C        BEQ    Reason
            0xE3A0_2003, // 10        MOV    r2, #3
            0xEA00_0001, // 14 Out    B      Exit
            0xE3A0_1007, // 18        MOV    r1, #7   (unreached)
            0xE1A0_F00E, // 1C Reason MOV    pc, lr
            0xE8BD_8000, // 20 Exit   LDMFD  sp!, {pc}
        ];
        for lift in [false, true] {
            let o = unit_lift(&forward, &[(0, 'a')], &[("Entry", 0), ("Out", 0x14), ("Reason", 0x1C), ("Exit", 0x20)], &["Entry"], lift)
                .unwrap_or_else(|e| panic!("{e:?}"));
            assert!(o.c.contains("t0_Reason(s"), "lift {lift}: the BEQ calls: {}", o.c);
            assert!(o.c.contains("goto Out;"), "lift {lift}: and goes on at the label: {}", o.c);
            assert!(!o.c.contains("t0_Exit(s"), "lift {lift}: the B at the label is a jump: {}", o.c);
            // A handler that stacks its lr. The call leaves a resume point,
            // and still goes on at the label.
            let mut stacked = forward;
            stacked[7] = 0xE92D_4000;   // 1C Reason STMFD sp!, {lr}
            let stacked: Vec<u32> = stacked.iter().copied().chain([0xE8BD_8000]).collect();
            let o = unit_lift(&stacked, &[(0, 'a')], &[("Entry", 0), ("Out", 0x14), ("Reason", 0x1C)], &["Entry"], lift)
                .unwrap_or_else(|e| panic!("{e:?}"));
            assert!(o.c.contains("goto Out;"), "lift {lift}: a stacked lr's call goes on at the label: {}", o.c);
            assert!(o.c.contains("rs.at = 0xFC000014u;"), "lift {lift}: a resume point: {}", o.c);
            if lift {
                // lr, which the handler stacks, is stored in the state block
                // for it, since the B writes no lr of its own.
                assert!(o.c.contains("R[13] = r13; R[14] = r14;"), "lr stored for the call: {}", o.c);
            }
            let o = unit_lift(&table, &[(0, 'a')], &[("Entry", 0), ("Loop", 4), ("Handler", 0x24)], &["Entry"], lift)
                .unwrap_or_else(|e| panic!("{e:?}"));
            assert_eq!(o.regions, 2, "lift {lift}: Entry, and Handler because a row calls it: {}", o.c);
            assert!(o.c.contains("goto Loop;"), "lift {lift}: MOV pc, lr goes to the label: {}", o.c);
            assert!(o.c.contains("t0_Handler(s"), "lift {lift}: {}", o.c);
            assert!(o.c.contains("ros_check_return(s, 0xFC000004u);"), "lift {lift}: {}", o.c);
            let o = unit_lift(&beq, &[(0, 'a')], &[("Entry", 0), ("Loop", 4), ("Click", 0x18)], &["Entry"], lift)
                .unwrap_or_else(|e| panic!("{e:?}"));
            assert_eq!(o.regions, 2, "lift {lift}: Entry, and Click because the BEQ calls it: {}", o.c);
            assert!(o.c.contains("t0_Click(s"), "lift {lift}: {}", o.c);
            assert!(o.c.contains("ros_check_return(s, 0xFC000004u);"), "lift {lift}: {}", o.c);
        }
    }
}
