//! The statements and expressions of a program, as the parser reads them.
//!
//! The parser (parse.rs) reads each tokenised line the way the interpreter
//! does, from left to right. flow.rs then finds where control goes. Section
//! numbers are those of spec/.

/// A variable's kind, from its suffix (chapter 4).
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
pub enum VarKind {
    Real,
    Int,
    Str,
}

/// A simple variable: its name as written (suffix included) and its kind.
#[derive(Debug, Clone, PartialEq, Eq, Hash)]
pub struct Var {
    pub name: Vec<u8>,
    pub kind: VarKind,
}

/// An array: its name as written, with its suffix, and its elements' kind.
/// `a` and `a(` are different names (§4.1).
pub type ArrVar = Var;

impl Var {
    /// `@%` and `A%` to `Z%`: always defined (chapter 4).
    pub fn resident(&self) -> bool {
        self.kind == VarKind::Int && self.name.len() == 2 && (self.name[0] == b'@' || self.name[0].is_ascii_uppercase())
    }
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum BinOp {
    Pow,
    Mul,
    Div,
    IDiv,
    Mod,
    Add,
    Sub,
    Eq,
    Ne,
    Lt,
    Gt,
    Le,
    Ge,
    Shl,
    Asr,
    Lsr,
    And,
    Or,
    Eor,
}

impl BinOp {
    /// The precedence level of §5.1.1: 2 (`^`) to 7 (`OR`, `EOR`).
    pub fn level(self) -> u8 {
        use BinOp::*;
        match self {
            Pow => 2,
            Mul | Div | IDiv | Mod => 3,
            Add | Sub => 4,
            Eq | Ne | Lt | Gt | Le | Ge | Shl | Asr | Lsr => 5,
            And => 6,
            Or | Eor => 7,
        }
    }
}

/// The built-in functions (chapters 5, 6, 9, 10).
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Func {
    Abs,
    Sgn,
    Int,
    Sqr,
    Sin,
    Cos,
    Tan,
    Atn,
    Asn,
    Acs,
    Exp,
    Ln,
    Log,
    Deg,
    Rad,
    Pi,
    True,
    False,
    Rnd,     // RND with no argument
    RndArg,  // RND(n)
    Len,
    /// EVAL (§5.9)
    Eval,
    // Files (chapter 13): a name, or a channel
    Openin,
    Openup,
    Openout,
    Bget,
    Eof,
    Ext,
    Ptr,
    GetDFile,
    // Keyboard (§11.6) and the operating system (chapter 14)
    Get,
    GetD,
    Inkey,
    InkeyD,
    Time,
    TimeD,
    Adval,
    Pos,
    Vpos,
    PointXY,
    TintXY,
    ModeFn,
    VduFn,
    Beat,
    Beats,
    Tempo,
    Asc,
    Chr,
    Str,
    StrHex,
    Val,
    Left,    // LEFT$(s,n)
    Left1,   // LEFT$(s)
    Right,
    Right1,
    Mid,     // MID$(s,m,n)
    Mid2,    // MID$(s,m)
    Instr,   // INSTR(s,t)
    Instr3,  // INSTR(s,t,p)
    StringN, // STRING$(n,s)
    Count,
    Err,
    Erl,
    ReportS, // REPORT$
    Width,
    /// `USR` address (§16.9)
    Usr,
}

#[derive(Debug, Clone, PartialEq)]
pub enum Expr {
    Int(i32),
    /// A real literal's text. The runtime converts it as the interpreter
    /// does (§2.5.6, §3.3).
    Real(Vec<u8>),
    Str(Vec<u8>),
    Var(Var),
    Unary(UnOp, Box<Expr>),
    Bin(BinOp, Box<Expr>, Box<Expr>),
    Func(Func, Vec<Expr>),
    /// `FN`name(arguments)
    Fn(Vec<u8>, Vec<Expr>),
    /// An array element, name(s1, s2 ...) (§4.5.9).
    Elem(ArrVar, Vec<Expr>),
    /// A whole array, name`()`, as an actual parameter (§8.5.1).
    ArrayRef(ArrVar),
    /// `SUM`, `SUMLEN` or `MOD` of an array (§4.8).
    ArrayFunc(ArrayFunc, ArrVar),
    /// `DIM(`name`())`, and `DIM(`name`(),` n`)` (§4.5.11).
    DimOf(ArrVar, Option<Box<Expr>>),
    /// `DIM(`name`)` of a variable: an error, which depends on whether the
    /// variable exists (§4.5.11).
    DimVar(Var),
    /// An indirection (§12.2): its kind, its base, and a dyadic offset.
    Ind(Ind, Box<Expr>, Option<Box<Expr>>),
    /// `PAGE`, `TOP`, `LOMEM`, `HIMEM` and `END` read (§12.3).
    Pseudo(Pseudo),
    /// An expression the interpreter stops reading with an error: the
    /// parts before it are evaluated, then the error is raised (§9.8.3).
    Fail(Vec<Expr>, Fail),
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum UnOp {
    Neg,
    Plus,
    Not,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum ArrayFunc {
    Sum,
    SumLen,
    Mod,
}

/// The operators of whole-array arithmetic (§4.7.1), numbered as the
/// runtime's RB_WA_*.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum WaOp {
    Add = 0,
    Sub = 1,
    Mul = 2,
    Div = 3,
}

/// The value of a whole-array assignment (§4.7.1). f is a factor.
#[derive(Debug, Clone, PartialEq)]
pub enum WaValue {
    /// B()
    Copy(ArrVar),
    /// -B()
    Neg(ArrVar),
    /// B() op C()
    Op(WaOp, ArrVar, ArrVar),
    /// B() op f
    OpRight(WaOp, ArrVar, Expr),
    /// f op B()
    OpLeft(WaOp, Expr, ArrVar),
    /// f
    Set(Expr),
    /// f, e2, ... (§4.7.12)
    List(Vec<Expr>),
    /// B().C() (§4.7.14)
    MatMul(ArrVar, ArrVar),
    /// B().f: an error that depends on the shapes (§4.7.14)
    DotFactor(ArrVar, Expr),
    /// `+=` e, or `-=` e when true (§4.7.11)
    AddEq(Expr, bool),
}

impl WaValue {
    /// The expressions, in the order they are evaluated.
    pub fn exprs(&self) -> Vec<Expr> {
        match self {
            WaValue::OpRight(_, _, e) | WaValue::OpLeft(_, e, _) | WaValue::Set(e) | WaValue::DotFactor(_, e) | WaValue::AddEq(e, _) => {
                vec![e.clone()]
            }
            WaValue::List(v) => v.clone(),
            _ => Vec::new(),
        }
    }
}

impl Target {
    /// The target read as a value.
    pub fn into_expr(self) -> Expr {
        match self {
            Target::Var(v) => Expr::Var(v),
            Target::Elem(v, s) => Expr::Elem(v, s),
            Target::Ind(k, b, o) => Expr::Ind(k, Box::new(b), o.map(Box::new)),
        }
    }
}

/// How an operating-system statement's value is taken.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum OsKind {
    /// Converted to an integer at once (§3.7.2).
    Int,
    /// A number, integer or real.
    Num,
    Str,
}

/// An argument of a runtime call: an expression's value, or a constant.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum OsRef {
    E(usize),
    /// A C int, used as a flag.
    Int(i32),
    /// A number.
    Num(i32),
}

/// An item of `INPUT`.
#[derive(Debug, Clone, PartialEq)]
pub enum InputItem {
    /// A prompt, written as `PRINT` writes it.
    Prompt(PrintItem),
    /// A variable, and whether `?` is written before a new line is read
    /// for it (§11.5.2).
    Var(Target, bool),
}

/// Where `RESTORE` sets the DATA pointer (§11.3).
#[derive(Debug, Clone, PartialEq)]
pub enum RestoreTo {
    Start,
    Line(Expr),
    /// `RESTORE +`n
    Rel(Expr),
}

/// A formal parameter (§8.3-8.5).
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum ParamKind {
    Value,
    /// `RETURN` v. The result goes back to the actual parameter (§8.4).
    Return,
    /// v`()`. This is the actual parameter's array (§8.5).
    Array,
}

#[derive(Debug, Clone, PartialEq)]
pub struct Param {
    pub var: Var,
    pub kind: ParamKind,
}

/// An item of `LOCAL` (§8.6.1).
#[derive(Debug, Clone, PartialEq)]
pub enum LocalItem {
    Var(Var),
    Array(ArrVar),
    /// `?`, `!` or `$` indirection: its kind, base and dyadic offset.
    Ind(Ind, Expr, Option<Expr>),
}

/// One item of a `DIM` statement.
#[derive(Debug, Clone, PartialEq)]
pub enum DimItem {
    Array(ArrVar, Vec<Expr>),
    /// `DIM` v n: a block of n+1 bytes (§12.1.1).
    Block(Var, Expr),
    /// `DIM` v `LOCAL` n: a block for the routine (§12.1.8).
    LocalBlock(Var, Expr),
}

/// The indirection operators (§12.2.1).
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Ind {
    Byte,
    Word,
    Real,
    Str,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Pseudo {
    Page,
    Top,
    Lomem,
    Himem,
    End,
}

/// An error the interpreter raises when it reads something it cannot use.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Fail {
    pub number: i32,
    pub message: &'static str,
}

impl Fail {
    pub fn syntax() -> Fail {
        Fail { number: 16, message: "Syntax error" }
    }
    pub fn missing_var() -> Fail {
        Fail { number: 26, message: "Unknown or missing variable" }
    }
}

/// Something a statement assigns to.
#[derive(Debug, Clone, PartialEq)]
pub enum Target {
    Var(Var),
    Elem(ArrVar, Vec<Expr>),
    /// An indirection: its kind, base and dyadic offset (§12.2.9).
    Ind(Ind, Expr, Option<Expr>),
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum AssignOp {
    Set,
    Add,
    Sub,
}

#[derive(Debug, Clone, PartialEq)]
pub enum PrintItem {
    Value(Expr),
    Semicolon,
    Comma,
    Tilde,
    Quote,
    Tab(Expr),
    TabXY(Expr, Expr),
    Spc(Expr),
}

/// A statement. Control statements carry what the parser knows. flow.rs
/// adds where control goes.
#[derive(Debug, Clone, PartialEq)]
pub enum Stmt {
    /// An empty statement, `REM`, `DATA`, a `DEF` reached by running into
    /// it (§8.10.1), `ENDIF`, `ENDCASE`.
    Nothing,
    Print(Vec<PrintItem>),
    /// The target, the operator, the value, and whether `LET` came first.
    Assign(Target, AssignOp, Expr, bool),
    /// A whole-array assignment, name`()` = ... (§4.7).
    WholeArray(ArrVar, WaValue),
    /// `SWAP` of two variables, elements or indirections (§4.10.1).
    Swap(Target, Target),
    /// `SWAP` of two whole arrays (§4.10.4).
    SwapArrays(ArrVar, ArrVar),
    /// `LEFT$(`v`,`n`)=`s and the like (§6.11). `kind` is 0 for LEFT$, 1
    /// for MID$ and 2 for RIGHT$.
    SubAssign { kind: u8, target: Target, args: Vec<Expr>, value: Expr },
    /// A statement that the runtime carries out by calling the operating
    /// system (chapters 13, 14). It holds its expressions, which are
    /// evaluated in order and each converted as its kind says. Then come
    /// the calls, which are made in order.
    Os { exprs: Vec<(Expr, OsKind)>, calls: Vec<(&'static str, Vec<OsRef>)> },
    /// `RUN`: the program starts again (§4.9.3).
    Run,
    /// A statement the compiler rejects, with chapter 15's diagnostic.
    Rejected(&'static str),
    /// `LIBRARY` with a literal name, read at compile time (§15.3.1).
    Library(Vec<u8>),
    /// A `*` command: the line's text after the `*` (§14.1.1).
    Star(Vec<u8>),
    /// `VDU` and its values with their separators (§14.5.1). The separator
    /// is 0 for `,` or none, 1 for `;` and 2 for `|`.
    Vdu(Vec<(Expr, u8)>),
    /// `MODE` with one value, a number or a string (§14.6.6).
    ModeOne(Expr),
    /// `ENVELOPE` and its 14 values (§14.7.1).
    Envelope(Vec<Expr>),
    /// `MOUSE` x`,`y`,`b[`,`t] (§14.6.7).
    Mouse(Vec<Target>),
    /// `SYS` swi, inputs `TO` outputs `;` flags (§14.2).
    Sys { swi: Expr, ins: Vec<Option<Expr>>, outs: Vec<Option<Target>>, flags: Option<Target> },
    /// `BPUT#`c`,`v, and whether `;` follows (§13.4.1-13.4.2).
    Bput(Expr, Expr, bool),
    /// `PRINT#`c and its items (§13.5).
    PrintFile(Expr, Vec<Expr>),
    /// `INPUT#`c and its variables (§13.6).
    InputFile(Expr, Vec<Target>),
    /// `INPUT` or `INPUT LINE` (§11.5).
    Input { line: bool, items: Vec<InputItem> },
    /// `READ` and its variables (§11.2).
    Read(Vec<Target>),
    /// `RESTORE` (§11.3).
    Restore(RestoreTo),
    /// `LOCAL DATA` and `RESTORE DATA` (§11.4).
    LocalData,
    RestoreData,
    /// A single-line `IF` (§7.2). If the condition is false, control goes
    /// to the first `ELSE` on the line.
    If(Expr),
    /// `IF` ... `THEN` at the end of the line (§7.3).
    BlockIf(Expr),
    /// `THEN` or `ELSE` followed by a line number (§7.2.4).
    Goto(u16),
    /// `GOTO` or `GOSUB` n followed by more of an expression. This is error
    /// 41 if line n does not exist, and error 16 otherwise (§7.8.3).
    GotoBad(u16),
    /// An `ELSE` that is not first on its line (§7.2.2). It skips the rest
    /// of the line.
    ElseSkip,
    /// A block `ELSE` (§7.3.3).
    BlockElse,
    For { var: Target, start: Expr, limit: Expr, step: Option<Expr> },
    /// `NEXT` v1, v2 ... . `None` is an empty item.
    Next(Vec<Option<Var>>),
    Repeat,
    Until(Expr),
    While(Expr),
    EndWhile,
    Case(Expr),
    /// A `WHEN` that runs, which ends the clause before it (§7.7.6). The
    /// values are evaluated only by the scan of the CASE that finds it.
    When(Vec<Expr>),
    Otherwise,
    Gosub(u16),
    Return,
    /// `ON` e `GOTO`/`GOSUB` lines [`ELSE` ...]
    On { index: Expr, gosub: bool, lines: Vec<u16>, has_else: bool },
    /// `ON` ... `PROC` (§7.8.10). `items` holds each procedure with its
    /// arguments and the offset in the line where its call ends.
    /// `ambiguous` says whether a string in the list holds a byte that the
    /// interpreter's scans stop at (§7.11.6).
    OnProc { index: Expr, items: Vec<(Vec<u8>, Vec<Expr>, usize)>, has_else: bool, ambiguous: bool },
    Def,
    Proc(Vec<u8>, Vec<Expr>),
    EndProc,
    FnReturn(Expr),
    Local(Vec<LocalItem>),
    /// `ON ERROR` [`LOCAL`] statements, or `ON ERROR` [`LOCAL`] `OFF`
    /// (§9.4, §9.5). The handler is the rest of the line.
    OnError { local: bool, off: bool },
    /// `LOCAL ERROR` (§9.5.4).
    LocalError,
    /// `RESTORE ERROR` (§9.5.6).
    RestoreError,
    /// `ERROR` [`EXT`] number, message (§9.3).
    Error { ext: bool, number: Expr, message: Expr },
    /// The statement `REPORT` (§9.2.6).
    Report,
    /// `DIM` of arrays and blocks (§4.5, §12.1). The items are in order
    /// from left to right.
    Dim(Vec<DimItem>),
    End,
    Stop,
    Quit(Option<Expr>),
    Width(Expr),
    /// A statement the interpreter fails on when it runs (§9.8.3). The
    /// expressions are evaluated, then the error is raised.
    Fail(Vec<Expr>, Fail),
    /// A statement the compiler does not compile yet.
    Unsupported(&'static str),
    /// An assembler block, `[` to its `]` (chapter 16).
    Asm(AsmBlock),
    /// `CALL` address and its parameters (§16.9).
    Call(Expr, Vec<Target>),
}

/// An assembler block (§16.1). When the `]` is on the same line as the
/// `[`, the parser finds where the block ends. When it is not, flow.rs
/// searches the following lines for it and fills in the text.
#[derive(Debug, Clone, PartialEq)]
pub struct AsmBlock {
    /// The tokenised lines from the `[` line to the `]` line, each as a
    /// program holds it (13, the number, the length, the text), followed
    /// by 13 and &FF. This is what the runtime assembles (rb_asm_block).
    pub text: Vec<u8>,
    /// The offset in `text` of the byte after the `[`. In the parser's
    /// result it is the offset in the line.
    pub start: usize,
    /// Whether the block has a `]`. If it has not, the block runs to the
    /// end of the program, and the program then ends.
    pub ends: bool,
}
