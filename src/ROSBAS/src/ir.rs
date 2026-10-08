//! The intermediate representation (design/compiler.md, "The IR").
//!
//! There is one `Unit` for each routine and one for the main program. A unit
//! is a graph of blocks. Each block is a list of instructions that ends in
//! a terminator. Values live in typed temporaries. BASIC's variables are
//! storage, and are loaded and stored explicitly. This is because under
//! dynamic scope any call may read or change any of them (§1.3.3).
//! Operations are BASIC's own, and each is named by the runtime entry point
//! that does it (runtime/rb_rt.h). A back end either emits a call or does
//! the same thing itself.

/// A temporary's kind.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Ty {
    /// A number, integer or real (`rb_num`).
    Num,
    /// A string (`rb_str`).
    Str,
    /// A number or a string (`rb_any`): an FN's result.
    Any,
    /// A machine integer: a truth value, a count, an index.
    Int,
    /// A double: a real FOR loop's limit and step.
    Real,
    /// An array: a pointer to its descriptor (§8.5).
    Arr,
    /// The results of a call's RETURN parameters, of which there are n
    /// (§8.4.3).
    RetBuf(u32),
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct Temp {
    pub id: u32,
    pub ty: Ty,
}

/// A BASIC variable: an index into `Program::vars`.
pub type VarId = u32;
pub type BlockId = u32;

#[derive(Debug, Clone, PartialEq)]
pub enum Operand {
    T(Temp),
    /// A constant integer, passed as a C int.
    Imm(i64),
    /// A literal of the program. For a string this is an index into
    /// `Program::strs`, and for a real an index into `Program::reals`.
    Str(u32),
    Real(u32),
    /// A C string constant (an error message).
    Text(&'static str),
    /// The place the unit's handlers come back to. This is the unit's own
    /// C `jmp_buf` (§9.5).
    Jb,
    /// An array, as an index into `Program::arrays`. Its value is the
    /// pointer in its slot.
    Arr(u32),
    /// An array's slot itself, to rebind it.
    ArrSlot(u32),
    /// An assembler block's text: an index into `Program::asm`.
    Asm(u32),
    /// A variable's address as a BASIC address (an `int32_t`), and as a
    /// pointer. These are used for `CALL`'s parameters (§16.9).
    VarAddr(VarId),
    VarPtr(VarId),
    /// A C expression provided by the runtime.
    Raw(&'static str),
}

#[derive(Debug, Clone, PartialEq)]
pub enum Rv {
    /// A runtime operation, given by its entry point and its operands. Its
    /// result is the value.
    Op(&'static str, Vec<Operand>),
    /// A variable's value (checked for being defined, §4.2.3).
    Load(VarId),
    /// A call of routine `unit` with arguments. The result is `Any` for a
    /// function and nothing for a procedure.
    Call(usize, Vec<Temp>),
    /// A FOR loop's saved limit or step (`Real` or `Num`).
    Slot(u32),
    Copy(Operand),
}

#[derive(Debug, Clone, PartialEq)]
pub enum Inst {
    /// The running statement is on line `n`. This is what `ERL` reports
    /// (§9.2).
    Line(u16),
    Let(Temp, Rv),
    /// A runtime operation for its effect.
    Do(&'static str, Vec<Operand>),
    Store(VarId, Temp),
    /// Save a variable in save slot `slot`, and restore it. LOCAL and
    /// parameters use these (§8.4, §8.6).
    Save(VarId, u32),
    Restore(VarId, u32),
    /// Make a variable defined with its kind's zero (LOCAL, §8.6.2).
    Clear(VarId),
    /// Raise error 16 if the variable does not exist yet (NEXT, §7.4.11).
    MustExist(VarId, i32, &'static str),
    /// Make the variable, holding 0 or the empty string, if it does not
    /// exist yet (a RETURN actual, §8.4.1).
    Create(VarId),
    /// The segment the statement is in. 0 is the program and k is library
    /// k. It gives the suffix of an error's message (§9.2.8).
    SetLib(u32),
    /// Save an array's slot, and point it at an array. Array parameters
    /// and LOCAL arrays use these.
    SaveArr(u32),
    SetArr(u32, Temp),
    SetSlot(u32, Temp),
    /// Call a procedure.
    CallProc(usize, Vec<Temp>),
    /// GOSUB: push the return point's number.
    PushReturn(u32),
}

#[derive(Debug, Clone, PartialEq)]
pub enum Term {
    Jump(BlockId),
    /// Branch on an `Int` temporary. Non-zero goes to the first block.
    Branch(Temp, BlockId, BlockId),
    /// RETURN: pop a return point and go there.
    PopReturn(Vec<(u32, BlockId)>),
    /// Leave the unit. This is a procedure's end, or a function's value.
    Return(Option<Temp>),
    Raise(i32, &'static str),
    End,
    /// Not yet filled in.
    Open,
}

#[derive(Debug, Clone, PartialEq)]
pub struct Block {
    pub insts: Vec<Inst>,
    pub term: Term,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub enum UnitKind {
    Main,
    Proc,
    Fn,
}

#[derive(Debug, Clone)]
pub struct Unit {
    pub kind: UnitKind,
    /// The routine's name (without PROC or FN), for the C function's name.
    pub name: Vec<u8>,
    /// The C parameters after the save mark. There is one for each formal,
    /// and the buffer for the RETURN results if there are RETURN formals.
    pub nparams: usize,
    pub blocks: Vec<Block>,
    pub temps: Vec<Ty>,
    /// The kinds of the FOR loop slots.
    pub slots: Vec<Ty>,
    /// Save slots used by LOCAL and parameters.
    pub saves: u32,
    /// Whether the unit uses GOSUB.
    pub gosub: bool,
    /// The blocks of the handlers that run in this unit, by handler number
    /// (flow::Unit::handlers). An error comes back to the unit and goes to
    /// one of them (§9.4, §9.5).
    pub handlers: Vec<BlockId>,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum VarKind {
    Real,
    Int,
    Str,
    /// `@%`, which the runtime holds.
    AtPct,
}

#[derive(Debug, Clone)]
pub struct Var {
    pub name: Vec<u8>,
    pub kind: VarKind,
    /// A resident integer, always defined.
    pub resident: bool,
}

#[derive(Debug, Clone)]
pub struct Program {
    pub units: Vec<Unit>,
    pub vars: Vec<Var>,
    /// Arrays, by name. `a(` is a different name from the variable `a`
    /// (§4.1).
    pub arrays: Vec<Var>,
    pub strs: Vec<Vec<u8>>,
    /// Real literals, as their text.
    pub reals: Vec<Vec<u8>>,
    /// The program's tokenised form, which PAGE points to (§12.3.3).
    pub tokenised: Vec<u8>,
    pub quirks: u32,
    /// Whether the runtime needs the tables of names, lines and DATA
    /// (EVAL, READ, RESTORE, LOCAL DATA).
    pub tables: bool,
    /// Whether the program forgets its variables (CLEAR, RUN, LOMEM=), and
    /// whether it starts again (RUN).
    pub clears: bool,
    pub runs: bool,
    /// The libraries' suffixes for errors (§9.2.8), library 1 first.
    pub libs: Vec<Option<Vec<u8>>>,
    /// The functions EVAL can call.
    pub fns: Vec<FnSym>,
    /// Every line's number, in program order.
    pub lines: Vec<u16>,
    /// The assembler blocks' texts (chapter 16).
    pub asm: Vec<Vec<u8>>,
    /// The DATA lines. Each is the line's index and its text after DATA.
    pub data: Vec<(u32, Vec<u8>)>,
}

#[derive(Debug, Clone)]
pub struct FnSym {
    pub unit: usize,
    pub name: Vec<u8>,
    pub params: Vec<FnParam>,
}

/// A function's formal parameter, as EVAL sees it. EVAL can pass values
/// only (§5.9).
#[derive(Debug, Clone, Copy)]
pub enum FnParam {
    Value(VarId),
    Return(VarId),
    Array,
}
