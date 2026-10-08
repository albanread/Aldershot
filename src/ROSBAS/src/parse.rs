//! Reading a tokenised line as the interpreter does (chapters 2-10).
//!
//! Each line becomes a list of statements. Each statement has the byte
//! offsets where it starts and ends, because the interpreter's scans
//! (§7.1.2) and its jumps work on positions in the text. A statement that
//! the interpreter would fail on when it runs is kept as `Stmt::Fail`,
//! with the parts it evaluates first (§9.8.3).

use crate::ast::*;
use crate::program::line_constant;
use crate::tokens::*;

/// One statement of a line.
#[derive(Debug, Clone, PartialEq)]
pub struct Parsed {
    /// Offset of the statement's first byte in the line's text.
    pub start: usize,
    /// Offset just past the statement. This is where the next statement's
    /// separator, an `ELSE` or the end of the line stands.
    pub end: usize,
    pub stmt: Stmt,
}

/// A routine's definition (§8.1). It holds the name from `DEF PROC`name or
/// `DEF FN`name, the formal parameters, and where the body starts.
#[derive(Debug, Clone, PartialEq)]
pub struct Def {
    pub name: Vec<u8>,
    pub is_fn: bool,
    pub params: Vec<Param>,
    /// Offset in the line where the body's first statement starts.
    pub body: usize,
    /// A form of parameter the compiler does not handle yet.
    pub unsupported: Option<&'static str>,
    /// A form of parameter the compiler rejects (§8.12.2).
    pub rejected: Option<&'static str>,
}

/// An expression that failed part way. It holds the operands already
/// complete, in the order the interpreter evaluated them, and the error.
#[derive(Debug, Clone, PartialEq)]
pub struct Partial {
    pub done: Vec<Expr>,
    pub fail: Fail,
}

type PResult<T> = Result<T, Partial>;

fn fail<T>(f: Fail) -> PResult<T> {
    Err(Partial { done: Vec::new(), fail: f })
}

fn after(mut p: Partial, first: Expr) -> Partial {
    p.done.insert(0, first);
    p
}

pub fn is_name_char(c: u8) -> bool {
    c.is_ascii_alphanumeric() || c == b'_' || c == b'`'
}

/// A character of a routine's name (§8.1.3).
pub fn is_routine_char(c: u8) -> bool {
    is_name_char(c) || c == b'@'
}

pub fn is_name_start(c: u8) -> bool {
    c.is_ascii_alphabetic() || c == b'_' || c == b'`'
}

pub struct LineParser<'a> {
    t: &'a [u8],
    i: usize,
    /// Set by a statement whose successor may follow without a colon.
    glued: bool,
    /// Set while the assignment after `LET` is parsed.
    let_: bool,
    /// Set while parsing when the statement uses something the compiler
    /// does not compile yet. The statement then becomes
    /// `Stmt::Unsupported`.
    unsupported: Option<&'static str>,
    /// Whether the statement being parsed is the first item on its line.
    first: bool,
    pub def: Option<Def>,
    /// Set when the line ends inside an assembler block, so that the `]`
    /// is on a later line (§16.1.2).
    pub asm_open: bool,
}

impl<'a> LineParser<'a> {
    pub fn new(t: &'a [u8]) -> Self {
        LineParser { t, i: 0, glued: false, let_: false, unsupported: None, first: true, def: None, asm_open: false }
    }

    fn peek(&self) -> u8 {
        *self.t.get(self.i).unwrap_or(&13)
    }
    /// The first byte that is not a space, from `k` bytes on.
    fn peek_after_spaces(&self, k: usize) -> u8 {
        let mut j = self.i + k;
        while j < self.t.len() && self.t[j] == b' ' {
            j += 1;
        }
        *self.t.get(j).unwrap_or(&13)
    }

    fn peek_at(&self, k: usize) -> u8 {
        *self.t.get(self.i + k).unwrap_or(&13)
    }
    fn at_end(&self) -> bool {
        self.i >= self.t.len()
    }
    fn skip_spaces(&mut self) {
        while self.i < self.t.len() && self.t[self.i] == b' ' {
            self.i += 1;
        }
    }
    /// The next byte that is not a space.
    fn next_sig(&mut self) -> u8 {
        self.skip_spaces();
        self.peek()
    }
    fn is_stmt_end(&mut self) -> bool {
        let c = self.next_sig();
        self.at_end() || c == b':' || c == TELSE
    }

    /// Parses the whole line.
    pub fn parse_line(&mut self) -> Vec<Parsed> {
        self.parse_from(0)
    }

    /// Parses the line from offset `from`. This reads the statements that
    /// follow an assembler block's `]` on a line of its own (§16.1.2).
    pub fn parse_from(&mut self, from: usize) -> Vec<Parsed> {
        let mut out = Vec::new();
        let mut first = from == 0;
        self.i = from;
        loop {
            self.skip_spaces();
            if self.at_end() {
                break;
            }
            let start = self.i;
            let c = self.peek();
            if c == b':' {
                self.i += 1;
                first = false;
                continue;
            }
            if c == TELSE2 && first {
                self.i += 1;
                out.push(Parsed { start, end: self.i, stmt: Stmt::BlockElse });
                first = false;
                continue;
            }
            if c == TELSE || c == TELSE2 {
                // An ELSE that is not first on its line skips the rest of
                // the line when it runs (§7.2.2). The statements after it
                // are the ELSE part of a single-line IF.
                self.i += 1;
                out.push(Parsed { start, end: self.i, stmt: Stmt::ElseSkip });
                if self.next_sig() == TCONST {
                    let s = self.i;
                    let n = self.line_ref();
                    out.push(Parsed { start: s, end: self.t.len(), stmt: Stmt::Goto(n) });
                    self.i = self.t.len();
                }
                first = false;
                continue;
            }
            self.first = first;
            first = false;
            self.glued = false;
            self.unsupported = None;
            let mut stmts = self.statement();
            if let Some(what) = self.unsupported.take() {
                let (s, e) = (stmts[0].start, self.t.len());
                stmts = vec![Parsed { start: s, end: e, stmt: Stmt::Unsupported(what) }];
                self.i = self.t.len();
            }
            // A statement that leaves the line first checks what follows
            // it. So `END PRINT` and `GOTO 40+20` are syntax errors
            // (§7.8.3, §7.9.1).
            self.skip_spaces();
            let junk = !self.at_end() && !matches!(self.peek(), b':' | TELSE | TELSE2);
            if junk && !self.glued {
                if let Some(last) = stmts.last_mut() {
                    let bad = match &last.stmt {
                        Stmt::Goto(n) | Stmt::Gosub(n) => Some(Stmt::GotoBad(*n)),
                        // An assignment and `=` evaluate their expression
                        // and then find what follows. They store or return
                        // nothing (§9.8.2).
                        Stmt::Assign(_, _, e, _) | Stmt::FnReturn(e) => Some(Stmt::Fail(vec![e.clone()], Fail::syntax())),
                        Stmt::WholeArray(_, v) => Some(Stmt::Fail(v.exprs(), Fail::syntax())),
                        Stmt::Os { exprs, .. } => Some(Stmt::Fail(exprs.iter().map(|(e, _)| e.clone()).collect(), Fail::syntax())),
                        Stmt::ModeOne(e) => Some(Stmt::Fail(vec![e.clone()], Fail::syntax())),
                        // CALL checks the statement's end before it calls (AEDONE)
                        Stmt::Call(a, _) => Some(Stmt::Fail(vec![Expr::Func(Func::Int, vec![a.clone()])], Fail::syntax())),
                        Stmt::Envelope(v) => Some(Stmt::Fail(v.clone(), Fail::syntax())),
                        Stmt::Bput(c, v, _) => Some(Stmt::Fail(vec![c.clone(), v.clone()], Fail::syntax())),
                        Stmt::End | Stmt::Stop | Stmt::EndProc | Stmt::Return | Stmt::EndWhile => {
                            Some(Stmt::Fail(Vec::new(), Fail::syntax()))
                        }
                        _ => None,
                    };
                    if let Some(b) = bad {
                        last.stmt = b;
                        last.end = self.t.len();
                        out.extend(stmts);
                        break;
                    }
                }
            }
            out.extend(stmts);
            // After a statement there must be a separator, ELSE, or the
            // end of the line. Anything else there is a syntax error
            // (§9.8.1).
            self.skip_spaces();
            if self.at_end() {
                break;
            }
            let c = self.peek();
            if c == b':' || c == TELSE || c == TELSE2 || self.continues() {
                continue;
            }
            let s = self.i;
            out.push(Parsed { start: s, end: self.t.len(), stmt: Stmt::Fail(Vec::new(), Fail::syntax()) });
            break;
        }
        out
    }

    /// Whether the byte after a statement may begin the next statement
    /// without a colon. This is so after REPEAT, a WHILE condition and
    /// THEN, because their bodies begin straight after them. The statement
    /// parsers set `self.glued` for these.
    fn continues(&mut self) -> bool {
        let g = self.glued;
        self.glued = false;
        g
    }

    fn line_ref(&mut self) -> u16 {
        let b = [self.peek_at(1), self.peek_at(2), self.peek_at(3)];
        self.i += 4;
        line_constant(b)
    }

    /// Parses one statement at self.i. A few statements yield more than
    /// one.
    fn statement(&mut self) -> Vec<Parsed> {
        let start = self.i;
        let one = |p: &mut Self, s: Stmt| vec![Parsed { start, end: p.i, stmt: s }];
        let c = self.peek();
        match c {
            TREM | TDATA => {
                self.i = self.t.len();
                one(self, Stmt::Nothing)
            }
            TENDIF | TENDCA => {
                self.i += 1;
                one(self, Stmt::Nothing)
            }
            TPRINT if self.peek_after_spaces(1) == b'#' => {
                self.i += 1;
                let s = self.print_file();
                one(self, s)
            }
            TPRINT => {
                self.i += 1;
                let s = self.print();
                one(self, s)
            }
            TINPUT if self.peek_after_spaces(1) == b'#' => {
                self.i += 1;
                let s = self.input_file();
                one(self, s)
            }
            TINPUT => {
                self.i += 1;
                let line = self.next_sig() == TLINE;
                if line {
                    self.i += 1;
                }
                let s = self.input_statement(line);
                one(self, s)
            }
            TLINE if self.peek_after_spaces(1) == TINPUT => {
                self.i += 1;
                self.skip_spaces();
                self.i += 1;
                let s = self.input_statement(true);
                one(self, s)
            }
            TVDU => {
                self.i += 1;
                let (s, f) = self.vdu_statement();
                let mut v = one(self, s);
                if let Some(f) = f {
                    v.push(Parsed { start, end: self.i, stmt: f });
                }
                v
            }
            // HIMEM= and LOMEM= (§12.3.4, §12.3.6), in either form
            THIMM2 | TLOMM2 | THIMEM | TLOMEM => {
                self.i += 1;
                let himem = c == THIMM2 || c == THIMEM;
                let s = if self.next_sig() != b'=' {
                    Stmt::Fail(Vec::new(), Fail { number: 4, message: "Missing =" })
                } else {
                    self.i += 1;
                    match self.expr() {
                        Ok(e) => os1(if himem { "rb_himem_set" } else { "rb_lomem_set" }, vec![(e, OsKind::Int)], vec![OsRef::E(0)]),
                        Err(p) => Stmt::Fail(p.done, p.fail),
                    }
                };
                one(self, s)
            }
            // END= (§12.3.8)
            TEND if self.peek_after_spaces(1) == b'=' => {
                self.i += 1;
                self.skip_spaces();
                self.i += 1;
                let s = match self.expr() {
                    Ok(e) => os1("rb_end_set", vec![(e, OsKind::Int)], vec![OsRef::E(0)]),
                    Err(p) => Stmt::Fail(p.done, p.fail),
                };
                one(self, s)
            }
            TCLEAR => {
                self.i += 1;
                one(self, os1("rb_clear", vec![], vec![]))
            }
            TRUN => {
                self.i += 1;
                one(self, Stmt::Run)
            }
            // Either form assigns at the start of a statement (§2.2.6)
            TPTR | TTIME => {
                self.i += 1;
                let s = self.os_statement(if c == TPTR { TPTR2 } else { TTIME2 });
                one(self, s)
            }
            TLINE | TOSCL | TCLOSE | TBPUT | TPTR2 | TEXT | TTIME2 | TPLOT | TMOVE | TDRAW | TCLS | TCLG | TMODE | TGRAPH | TTEXT | TOFF
            | TBEEP | TENVEL => {
                self.i += 1;
                let s = self.os_statement(c);
                one(self, s)
            }
            TESCSTMT
                if matches!(
                    self.peek_at(1),
                    TCIRCLE | TFILL | TORGIN | TPSET | TRECT | TWAIT | TMOUSE | TSYS | TTINT | TELLIPSE | TBEATS | TTEMPO | TVOICES | TVOICE | TSTEREO
                ) =>
            {
                let c = self.peek_at(1);
                self.i += 2;
                let s = self.os_statement2(c);
                one(self, s)
            }
            TIF => {
                self.i += 1;
                self.if_statement(start)
            }
            TFOR => {
                self.i += 1;
                let s = self.for_statement();
                one(self, s)
            }
            TNEXT => {
                self.i += 1;
                let s = self.next_statement();
                one(self, s)
            }
            TREPEAT => {
                self.i += 1;
                self.glued = true;
                one(self, Stmt::Repeat)
            }
            TUNTIL => {
                self.i += 1;
                let s = self.cond_statement(Stmt::Until);
                one(self, s)
            }
            TESCSTMT if self.peek_at(1) == TWHILE => {
                self.i += 2;
                let s = self.cond_statement(Stmt::While);
                if matches!(s, Stmt::While(_)) {
                    self.glued = true;
                }
                one(self, s)
            }
            TENDWH => {
                self.i += 1;
                one(self, Stmt::EndWhile)
            }
            TESCSTMT if self.peek_at(1) == TCASE => {
                self.i += 2;
                let s = self.case_statement();
                one(self, s)
            }
            TWHEN => {
                self.i += 1;
                let s = self.when_statement();
                one(self, s)
            }
            TOTHER => {
                self.i += 1;
                self.glued = true;
                one(self, Stmt::Otherwise)
            }
            TGOTO | TGOSUB => {
                self.i += 1;
                if self.next_sig() == TCONST {
                    let n = self.line_ref();
                    one(self, if c == TGOTO { Stmt::Goto(n) } else { Stmt::Gosub(n) })
                } else {
                    // A computed target is rejected (§7.11.4)
                    self.i = self.t.len();
                    one(self, Stmt::Rejected("Computed line number"))
                }
            }
            TRETURN => {
                self.i += 1;
                one(self, Stmt::Return)
            }
            TON if self.peek_after_spaces(1) == TERROR => {
                self.i += 1;
                self.skip_spaces();
                self.i += 1;
                let local = self.next_sig() == TLOCAL;
                if local {
                    self.i += 1;
                }
                let off = self.next_sig() == TOFF;
                if off {
                    self.i += 1;
                } else {
                    // The handler is the rest of the line (§9.4.1)
                    self.glued = true;
                }
                one(self, Stmt::OnError { local, off })
            }
            TLOCAL if self.peek_after_spaces(1) == TERROR => {
                self.i += 1;
                self.skip_spaces();
                self.i += 1;
                one(self, Stmt::LocalError)
            }
            TLOCAL if self.peek_after_spaces(1) == TDATA => {
                self.i += 1;
                self.skip_spaces();
                self.i += 1;
                one(self, Stmt::LocalData)
            }
            TRESTORE if self.peek_after_spaces(1) == TDATA => {
                self.i += 1;
                self.skip_spaces();
                self.i += 1;
                one(self, Stmt::RestoreData)
            }
            TRESTORE if self.peek_after_spaces(1) != TERROR => {
                self.i += 1;
                let s = if self.is_stmt_end() {
                    Stmt::Restore(RestoreTo::Start)
                } else if self.next_sig() == TCONST {
                    let n = self.line_ref();
                    Stmt::Restore(RestoreTo::Line(Expr::Int(n as i32)))
                } else if self.next_sig() == b'+' {
                    self.i += 1;
                    match self.expr() {
                        Ok(e) => Stmt::Restore(RestoreTo::Rel(e)),
                        Err(p) => Stmt::Fail(p.done, p.fail),
                    }
                } else {
                    match self.expr() {
                        Ok(e) => Stmt::Restore(RestoreTo::Line(e)),
                        Err(p) => Stmt::Fail(p.done, p.fail),
                    }
                };
                one(self, s)
            }
            TREAD => {
                self.i += 1;
                let s = self.read_statement();
                one(self, s)
            }
            TESCSTMT if self.peek_at(1) == TSWAP => {
                self.i += 2;
                let s = self.swap_statement();
                one(self, s)
            }
            TLEFTD | TMIDD | TRIGHTD => {
                self.i += 1;
                let s = self.substr_assignment(c);
                one(self, s)
            }
            TRESTORE if self.peek_after_spaces(1) == TERROR => {
                self.i += 1;
                self.skip_spaces();
                self.i += 1;
                one(self, Stmt::RestoreError)
            }
            TERROR => {
                self.i += 1;
                let s = self.error_statement();
                one(self, s)
            }
            TDIM => {
                self.i += 1;
                let s = self.dim_statement();
                one(self, s)
            }
            TREPORT if self.peek_at(1) != b'$' => {
                self.i += 1;
                one(self, Stmt::Report)
            }
            TON => {
                self.i += 1;
                // ON alone turns the cursor on (§14.6.2)
                let s = if self.is_stmt_end() { os1("rb_cursor", vec![], vec![OsRef::Int(1)]) } else { self.on_statement() };
                one(self, s)
            }
            TDEF => {
                self.i += 1;
                let s = self.def_statement();
                one(self, s)
            }
            TPROC => {
                self.i += 1;
                let s = self.proc_call();
                one(self, s)
            }
            TENDPR => {
                self.i += 1;
                one(self, Stmt::EndProc)
            }
            b'=' => {
                self.i += 1;
                let s = match self.expr() {
                    Ok(e) => Stmt::FnReturn(e),
                    Err(p) => Stmt::Fail(p.done, p.fail),
                };
                one(self, s)
            }
            TLOCAL => {
                self.i += 1;
                let s = self.local_statement();
                one(self, s)
            }
            TEND => {
                self.i += 1;
                one(self, Stmt::End)
            }
            TSTOP => {
                self.i += 1;
                one(self, Stmt::Stop)
            }
            TESCSTMT if self.peek_at(1) == TQUIT => {
                self.i += 2;
                let s = if self.is_stmt_end() {
                    Stmt::Quit(None)
                } else {
                    match self.expr() {
                        Ok(e) => Stmt::Quit(Some(e)),
                        Err(p) => Stmt::Fail(p.done, p.fail),
                    }
                };
                one(self, s)
            }
            TWIDTH => {
                self.i += 1;
                let s = match self.expr() {
                    Ok(e) => Stmt::Width(e),
                    Err(p) => Stmt::Fail(p.done, p.fail),
                };
                one(self, s)
            }
            TLET => {
                self.i += 1;
                self.skip_spaces();
                self.let_ = true;
                let s = match self.peek() {
                    c @ (b'?' | b'!' | b'|' | b'$') => self.ind_assignment(c),
                    // LET needs a variable. A pseudo-variable will not do
                    // (§4.3.1).
                    c if !is_name_start(c) && !(c == b'@' && self.peek_at(1) == b'%') => {
                        Stmt::Fail(Vec::new(), Fail::syntax())
                    }
                    _ => self.assignment(),
                };
                self.let_ = false;
                one(self, s)
            }
            b'@' => {
                let s = self.assignment();
                one(self, s)
            }
            b'?' | b'!' | b'|' | b'$' => {
                let s = self.ind_assignment(c);
                one(self, s)
            }
            c if is_name_start(c) => {
                let s = self.assignment();
                one(self, s)
            }
            b'*' => {
                // The rest of the line is the command (§14.1.1)
                let text = self.t[self.i + 1..].to_vec();
                self.i = self.t.len();
                one(self, Stmt::Star(text))
            }
            // A character that cannot start a statement, a function's or an
            // operator's token, or a command token. The interpreter raises
            // error 16 when it reaches one of these (§9.8.3, §2.2.9). `[`
            // is excluded because it starts the assembler.
            c if (c < 0x7F && c != b'[') || (0x7F..=TESCFN).contains(&c) || c == TESCCOM => {
                self.i = self.t.len();
                one(self, Stmt::Fail(Vec::new(), Fail::syntax()))
            }
            // Chapter 15's rejections
            TTRACE if self.peek_after_spaces(1) != TOFF => {
                self.i = self.t.len();
                one(self, Stmt::Rejected("TRACE is not compiled"))
            }
            TCHAIN => {
                self.i = self.t.len();
                one(self, Stmt::Rejected("CHAIN is not compiled"))
            }
            TESCSTMT if self.peek_at(1) == TOVERLAY => {
                self.i = self.t.len();
                one(self, Stmt::Rejected("OVERLAY is not compiled"))
            }
            TPAGE2 | TPAGE => {
                self.i = self.t.len();
                one(self, Stmt::Rejected("PAGE cannot be assigned in a compiled program"))
            }
            // The assembler (chapter 16). It runs to the `]`, which may be
            // on a later line, and flow.rs reads on to find it. The
            // statements after the `]` are BASIC statements.
            b'[' => {
                let from = self.i + 1;
                let s = match asm_end(self.t, from) {
                    Some(e) => {
                        self.i = e;
                        self.glued = true;
                        Stmt::Asm(AsmBlock { text: Vec::new(), start: from, ends: true })
                    }
                    None => {
                        self.i = self.t.len();
                        self.asm_open = true;
                        Stmt::Asm(AsmBlock { text: Vec::new(), start: from, ends: false })
                    }
                };
                one(self, s)
            }
            TCALL => {
                self.i += 1;
                let s = self.call_statement();
                one(self, s)
            }
            // LIBRARY is read at compile time, so its name must be a
            // literal (§15.3.1, §15.3.3).
            TESCSTMT if self.peek_at(1) == TLIBRARY => {
                self.i += 2;
                let s = if self.next_sig() == b'"' {
                    match self.string_literal() {
                        Ok(Expr::Str(name)) if self.is_stmt_end() => Stmt::Library(name),
                        _ => Stmt::Rejected("LIBRARY needs a constant file name"),
                    }
                } else {
                    Stmt::Rejected("LIBRARY needs a constant file name")
                };
                if matches!(s, Stmt::Rejected(_)) {
                    self.i = self.t.len();
                }
                one(self, s)
            }
            // The old statement token of INSTALL (§2.2.9)
            TESCSTMT if self.peek_at(1) == TINSTALLBAD => {
                self.i = self.t.len();
                one(self, Stmt::Fail(Vec::new(), Fail { number: 0, message: "INSTALL cannot be used in a program" }))
            }
            // TRACE OFF does nothing, because tracing is never on (§15.4.2).
            TTRACE if self.peek_after_spaces(1) == TOFF => {
                self.i += 1;
                self.skip_spaces();
                self.i += 1;
                one(self, Stmt::Nothing)
            }
            _ => {
                let what = unsupported_name(c, self.peek_at(1));
                self.i = self.t.len();
                one(self, Stmt::Unsupported(what))
            }
        }
    }

    fn cond_statement(&mut self, make: fn(Expr) -> Stmt) -> Stmt {
        match self.expr() {
            Ok(e) => make(e),
            Err(p) => Stmt::Fail(p.done, p.fail),
        }
    }

    // ---- PRINT (§10.2) --------------------------------------------------

    fn print(&mut self) -> Stmt {
        let mut items = Vec::new();
        loop {
            let c = self.next_sig();
            if self.at_end() || c == b':' || c == TELSE {
                break;
            }
            match c {
                b';' => {
                    self.i += 1;
                    items.push(PrintItem::Semicolon);
                }
                b',' => {
                    self.i += 1;
                    items.push(PrintItem::Comma);
                }
                b'~' => {
                    self.i += 1;
                    items.push(PrintItem::Tilde);
                }
                b'\'' => {
                    self.i += 1;
                    items.push(PrintItem::Quote);
                }
                TTAB => {
                    self.i += 1;
                    let x = match self.expr() {
                        Ok(e) => e,
                        Err(p) => return print_fail(items, p),
                    };
                    if self.next_sig() == b',' {
                        self.i += 1;
                        let y = match self.expr() {
                            Ok(e) => e,
                            Err(p) => return print_fail_with(items, vec![x], p),
                        };
                        if self.next_sig() != b')' {
                            return print_fail_with(items, vec![x, y], Partial { done: vec![], fail: missing_paren() });
                        }
                        self.i += 1;
                        items.push(PrintItem::TabXY(x, y));
                    } else {
                        if self.next_sig() != b')' {
                            return print_fail_with(items, vec![x], Partial { done: vec![], fail: missing_paren() });
                        }
                        self.i += 1;
                        items.push(PrintItem::Tab(x));
                    }
                }
                TSPC => {
                    self.i += 1;
                    match self.expr() {
                        Ok(e) => items.push(PrintItem::Spc(e)),
                        Err(p) => return print_fail(items, p),
                    }
                }
                _ => match self.expr() {
                    Ok(e) => items.push(PrintItem::Value(e)),
                    Err(p) => return print_fail(items, p),
                },
            }
        }
        Stmt::Print(items)
    }

    // ---- Assignment (§4.2) ----------------------------------------------

    // ---- Files, input and the operating system (chapters 11, 13, 14) ----

    /// `#` and a channel, a factor (§13.1.2).
    fn channel(&mut self) -> PResult<Expr> {
        if self.next_sig() != b'#' {
            return fail(Fail { number: 45, message: "Missing #" });
        }
        self.i += 1;
        self.factor()
    }

    /// n expressions separated by commas, added to out.
    fn list(&mut self, n: usize, out: &mut Vec<Expr>) -> Result<(), Stmt> {
        for k in 0..n {
            if k > 0 {
                if self.next_sig() != b',' {
                    return Err(Stmt::Fail(out.clone(), Fail { number: 5, message: "Missing ," }));
                }
                self.i += 1;
            }
            match self.expr() {
                Ok(e) => out.push(e),
                Err(p) => {
                    let mut d = out.clone();
                    d.extend(p.done);
                    return Err(Stmt::Fail(d, p.fail));
                }
            }
        }
        Ok(())
    }

    /// One to n expressions separated by commas.
    fn list_upto(&mut self, n: usize) -> Result<Vec<Expr>, Stmt> {
        let mut out = Vec::new();
        loop {
            match self.expr() {
                Ok(e) => out.push(e),
                Err(p) => {
                    out.extend(p.done);
                    return Err(Stmt::Fail(out, p.fail));
                }
            }
            if out.len() == n || self.next_sig() != b',' {
                return Ok(out);
            }
            self.i += 1;
        }
    }

    /// `BY` after `MOVE`, `DRAW`, `FILL` and `POINT`: the next two
    /// characters, whatever follows (§14.6.3).
    fn by(&mut self) -> bool {
        self.skip_spaces();
        if self.peek() == b'B' && self.peek_at(1) == b'Y' {
            self.i += 2;
            return true;
        }
        false
    }

    /// n integer values, then a call of f with them and then extra.
    fn ints(&mut self, n: usize, f: &'static str, extra: Vec<OsRef>) -> Stmt {
        let mut v = Vec::new();
        if let Err(s) = self.list(n, &mut v) {
            return s;
        }
        let mut refs: Vec<OsRef> = (0..n).map(OsRef::E).collect();
        refs.extend(extra);
        os1(f, v.into_iter().map(|e| (e, OsKind::Int)).collect(), refs)
    }

    /// A one-byte-token statement of chapters 13 and 14.
    fn os_statement(&mut self, c: u8) -> Stmt {
        match c {
            TLINE => self.ints(4, "rb_line", vec![]),
            TOSCL => match self.expr() {
                Ok(e) => os1("rb_oscli", vec![(e, OsKind::Str)], vec![OsRef::E(0)]),
                Err(p) => Stmt::Fail(p.done, p.fail),
            },
            TCLOSE => match self.channel() {
                Ok(ch) => os1("rb_close", vec![(ch, OsKind::Int)], vec![OsRef::E(0)]),
                Err(p) => Stmt::Fail(p.done, p.fail),
            },
            TBPUT => {
                let ch = match self.channel() {
                    Ok(ch) => ch,
                    Err(p) => return Stmt::Fail(p.done, p.fail),
                };
                if self.next_sig() != b',' {
                    return Stmt::Fail(vec![ch], Fail { number: 5, message: "Missing ," });
                }
                self.i += 1;
                match self.expr() {
                    Ok(v) => {
                        let semi = self.next_sig() == b';';
                        if semi {
                            self.i += 1;
                        }
                        Stmt::Bput(ch, v, semi)
                    }
                    Err(mut p) => {
                        p.done.insert(0, ch);
                        Stmt::Fail(p.done, p.fail)
                    }
                }
            }
            // PTR# and EXT# assigned (§13.4.8, §13.4.10-13.4.11)
            TPTR2 | TEXT => {
                let ch = match self.channel() {
                    Ok(ch) => ch,
                    Err(p) => return Stmt::Fail(p.done, p.fail),
                };
                if self.next_sig() != b'=' {
                    return Stmt::Fail(vec![ch], Fail { number: 4, message: "Missing =" });
                }
                self.i += 1;
                let f = if c == TPTR2 { "rb_ptr_set" } else { "rb_ext_set" };
                match self.expr() {
                    Ok(n) => os1(f, vec![(ch, OsKind::Int), (n, OsKind::Int)], vec![OsRef::E(0), OsRef::E(1)]),
                    Err(mut p) => {
                        p.done.insert(0, ch);
                        Stmt::Fail(p.done, p.fail)
                    }
                }
            }
            // TIME= and TIME$= (§14.4.2, §14.4.4)
            TTIME2 => {
                let dollar = self.peek() == b'$';
                if dollar {
                    self.i += 1;
                }
                if self.next_sig() != b'=' {
                    return Stmt::Fail(Vec::new(), Fail { number: 4, message: "Missing =" });
                }
                self.i += 1;
                match self.expr() {
                    Ok(e) if dollar => os1("rb_time_str_set", vec![(e, OsKind::Str)], vec![OsRef::E(0)]),
                    Ok(e) => os1("rb_time_set", vec![(e, OsKind::Int)], vec![OsRef::E(0)]),
                    Err(p) => Stmt::Fail(p.done, p.fail),
                }
            }
            TPLOT => self.ints(3, "rb_plot", vec![]),
            TMOVE | TDRAW => {
                let by = self.by();
                let code = match (c, by) {
                    (TMOVE, false) => 4,
                    (TMOVE, true) => 0,
                    (_, false) => 5,
                    (_, true) => 1,
                };
                self.plot_code(code)
            }
            TCLS => os1("rb_cls", vec![], vec![]),
            TCLG => os1("rb_clg", vec![], vec![]),
            TOFF => os1("rb_cursor", vec![], vec![OsRef::Int(0)]),
            TMODE => self.mode_statement(),
            TGRAPH => self.gcol_statement(),
            TTEXT => self.colour_statement(),
            TBEEP => {
                if self.next_sig() == TON || self.next_sig() == TOFF {
                    let on = self.next_sig() == TON;
                    self.i += 1;
                    return os1("rb_sound_on", vec![], vec![OsRef::Int(on as i32)]);
                }
                match self.list_upto(5) {
                    Ok(v) if v.len() == 4 => os1("rb_sound", v.into_iter().map(|e| (e, OsKind::Int)).collect(), (0..4).map(OsRef::E).collect()),
                    Ok(v) if v.len() == 5 => os1("rb_sound_at", v.into_iter().map(|e| (e, OsKind::Int)).collect(), (0..5).map(OsRef::E).collect()),
                    Ok(v) => Stmt::Fail(v, Fail { number: 5, message: "Missing ," }),
                    Err(s) => s,
                }
            }
            TENVEL => {
                let mut v = Vec::new();
                match self.list(14, &mut v) {
                    Ok(()) => Stmt::Envelope(v),
                    Err(s) => s,
                }
            }
            _ => Stmt::Unsupported("this statement"),
        }
    }

    /// PLOT code,x,y for MOVE, DRAW, FILL and POINT.
    fn plot_code(&mut self, code: i32) -> Stmt {
        let mut v = Vec::new();
        if let Err(s) = self.list(2, &mut v) {
            return s;
        }
        os1("rb_plot", v.into_iter().map(|e| (e, OsKind::Int)).collect(), vec![OsRef::Num(code), OsRef::E(0), OsRef::E(1)])
    }

    /// A two-byte-token statement of chapter 14.
    fn os_statement2(&mut self, c: u8) -> Stmt {
        let fill_follows = |p: &mut Self| {
            if p.next_sig() == TESCSTMT && p.peek_at(1) == TFILL {
                p.i += 2;
                true
            } else {
                false
            }
        };
        match c {
            TCIRCLE => {
                let fill = fill_follows(self);
                self.ints(3, "rb_circle", vec![OsRef::Int(fill as i32)])
            }
            TFILL => {
                let by = self.by();
                self.plot_code(if by { 0x81 } else { 0x85 })
            }
            TPSET => {
                if self.next_sig() == TTO {
                    self.i += 1;
                    return self.ints(2, "rb_point_to", vec![]);
                }
                let by = self.by();
                self.plot_code(if by { 0x41 } else { 0x45 })
            }
            TORGIN => self.ints(2, "rb_origin", vec![]),
            TWAIT => os1("rb_wait", vec![], vec![]),
            TTINT => self.ints(2, "rb_tint", vec![]),
            TBEATS => self.ints(1, "rb_beats", vec![]),
            TTEMPO => self.ints(1, "rb_tempo", vec![]),
            TVOICES => self.ints(1, "rb_voices", vec![]),
            TSTEREO => self.ints(2, "rb_stereo", vec![]),
            TVOICE => {
                let mut v = Vec::new();
                match self.list(2, &mut v) {
                    Ok(()) => {
                        let name = v.pop().unwrap();
                        let ch = v.pop().unwrap();
                        os1("rb_voice", vec![(ch, OsKind::Int), (name, OsKind::Str)], vec![OsRef::E(0), OsRef::E(1)])
                    }
                    Err(s) => s,
                }
            }
            TELLIPSE => {
                let fill = fill_follows(self) as i32;
                let v = match self.list_upto(5) {
                    Ok(v) => v,
                    Err(s) => return s,
                };
                match v.len() {
                    4 => os1("rb_ellipse", v.into_iter().map(|e| (e, OsKind::Int)).collect(), vec![OsRef::E(0), OsRef::E(1), OsRef::E(2), OsRef::E(3), OsRef::Int(fill)]),
                    5 => {
                        let kinds = [OsKind::Int, OsKind::Int, OsKind::Int, OsKind::Int, OsKind::Num];
                        os1(
                            "rb_ellipse_angle",
                            v.into_iter().zip(kinds).collect(),
                            vec![OsRef::E(0), OsRef::E(1), OsRef::E(2), OsRef::E(3), OsRef::E(4), OsRef::Int(fill)],
                        )
                    }
                    _ => Stmt::Fail(v, Fail { number: 5, message: "Missing ," }),
                }
            }
            TRECT => {
                let fill = fill_follows(self) as i32;
                let mut v = match self.list_upto(4) {
                    Ok(v) => v,
                    Err(s) => return s,
                };
                if v.len() < 3 {
                    return Stmt::Fail(v, Fail { number: 5, message: "Missing ," });
                }
                // h is w when left out
                let h = if v.len() == 4 { OsRef::E(3) } else { OsRef::E(2) };
                if self.next_sig() == TTO {
                    self.i += 1;
                    let n = v.len();
                    if let Err(s) = self.list(2, &mut v) {
                        return s;
                    }
                    let refs = vec![OsRef::E(0), OsRef::E(1), OsRef::E(2), h, OsRef::E(n), OsRef::E(n + 1), OsRef::Int(fill)];
                    return os1("rb_rectangle_to", v.into_iter().map(|e| (e, OsKind::Int)).collect(), refs);
                }
                os1("rb_rectangle", v.into_iter().map(|e| (e, OsKind::Int)).collect(), vec![OsRef::E(0), OsRef::E(1), OsRef::E(2), h, OsRef::Int(fill)])
            }
            TMOUSE => self.mouse_statement(),
            TSYS => self.sys_statement(),
            _ => Stmt::Unsupported("this statement"),
        }
    }

    /// `MODE` (§14.6.6).
    fn mode_statement(&mut self) -> Stmt {
        let v = match self.list_upto(6) {
            Ok(v) => v,
            Err(s) => return s,
        };
        let n = v.len();
        let ints = |v: Vec<Expr>| -> Vec<(Expr, OsKind)> { v.into_iter().map(|e| (e, OsKind::Int)).collect() };
        match n {
            1 => Stmt::ModeOne(v.into_iter().next().unwrap()),
            3 | 4 => {
                let hz = if n == 4 { OsRef::E(3) } else { OsRef::Num(-1) };
                os1("rb_mode_sel", ints(v), vec![OsRef::E(0), OsRef::E(1), OsRef::E(2), hz])
            }
            5 | 6 => {
                let hz = if n == 6 { OsRef::E(5) } else { OsRef::Num(-1) };
                os1("rb_mode_sel5", ints(v), vec![OsRef::E(0), OsRef::E(1), OsRef::E(2), OsRef::E(3), OsRef::E(4), hz])
            }
            _ => Stmt::Fail(v, Fail::syntax()),
        }
    }

    /// The values after `OF` or `ON` in `GCOL` and `COLOUR`.
    fn of_on_part(&mut self) -> Result<Option<(bool, Vec<Expr>)>, Stmt> {
        let c = self.next_sig();
        if c != TOF && c != TON {
            return Ok(None);
        }
        self.i += 1;
        Ok(Some((c == TON, self.list_upto(4)?)))
    }

    /// `GCOL` (§14.6.2, §14.6.5).
    fn gcol_statement(&mut self) -> Stmt {
        let first = match self.of_on_part() {
            Ok(p) => p,
            Err(s) => return s,
        };
        if let Some(first) = first {
            // With both OF and ON, both parts are read, and then the ON
            // call is made first.
            let second = if !first.0 {
                match self.of_on_part() {
                    Ok(p) => p,
                    Err(s) => return s,
                }
            } else {
                None
            };
            let mut exprs = Vec::new();
            let mut calls = Vec::new();
            for (on, v) in [Some(first), second].into_iter().flatten() {
                let base = exprs.len();
                let n = v.len();
                exprs.extend(v.into_iter().map(|e| (e, OsKind::Int)));
                let e = |k: usize| OsRef::E(base + k);
                let call = match n {
                    1 => ("rb_gcol_c", vec![OsRef::Num(0), e(0), OsRef::Int(on as i32)]),
                    2 => ("rb_gcol_c", vec![e(0), e(1), OsRef::Int(on as i32)]),
                    3 => ("rb_gcol_rgb", vec![OsRef::Num(0), e(0), e(1), e(2), OsRef::Int(if on { 3 } else { 2 })]),
                    _ => ("rb_gcol_rgb", vec![e(0), e(1), e(2), e(3), OsRef::Int(if on { 3 } else { 2 })]),
                };
                calls.push((on, call));
            }
            calls.sort_by_key(|(on, _)| !*on);
            return Stmt::Os { exprs, calls: calls.into_iter().map(|(_, c)| c).collect() };
        }
        let v = match self.list_upto(4) {
            Ok(v) => v,
            Err(s) => return s,
        };
        let n = v.len();
        let mut exprs: Vec<(Expr, OsKind)> = v.into_iter().map(|e| (e, OsKind::Int)).collect();
        if self.next_sig() == TESCSTMT && self.peek_at(1) == TTINT && n <= 2 {
            self.i += 2;
            match self.expr() {
                Ok(t) => exprs.push((t, OsKind::Int)),
                Err(mut p) => {
                    let mut d: Vec<Expr> = exprs.into_iter().map(|(e, _)| e).collect();
                    d.append(&mut p.done);
                    return Stmt::Fail(d, p.fail);
                }
            }
            let refs = if n == 1 { vec![OsRef::Num(0), OsRef::E(0), OsRef::E(1)] } else { vec![OsRef::E(0), OsRef::E(1), OsRef::E(2)] };
            return Stmt::Os { exprs, calls: vec![("rb_gcol_tint", refs)] };
        }
        let call = match n {
            1 => ("rb_gcol", vec![OsRef::Num(0), OsRef::E(0)]),
            2 => ("rb_gcol", vec![OsRef::E(0), OsRef::E(1)]),
            3 => ("rb_gcol_rgb", vec![OsRef::Num(0), OsRef::E(0), OsRef::E(1), OsRef::E(2), OsRef::Int(0)]),
            _ => ("rb_gcol_rgb", vec![OsRef::E(0), OsRef::E(1), OsRef::E(2), OsRef::E(3), OsRef::Int(1)]),
        };
        Stmt::Os { exprs, calls: vec![call] }
    }

    /// `COLOUR` (§14.6.2, §14.6.5).
    fn colour_statement(&mut self) -> Stmt {
        let first = match self.of_on_part() {
            Ok(p) => p,
            Err(s) => return s,
        };
        if let Some(first) = first {
            let second = if !first.0 {
                match self.of_on_part() {
                    Ok(p) => p,
                    Err(s) => return s,
                }
            } else {
                None
            };
            let mut exprs = Vec::new();
            let mut calls = Vec::new();
            for (on, v) in [Some(first), second].into_iter().flatten() {
                let base = exprs.len();
                let n = v.len();
                if n != 1 && n != 3 {
                    let d: Vec<Expr> = exprs.into_iter().map(|(e, _)| e).chain(v).collect();
                    return Stmt::Fail(d, Fail::syntax());
                }
                exprs.extend(v.into_iter().map(|e| (e, OsKind::Int)));
                let e = |k: usize| OsRef::E(base + k);
                let call = if n == 1 {
                    ("rb_colour_c", vec![e(0), OsRef::Int(on as i32)])
                } else {
                    ("rb_colour_rgb", vec![e(0), e(1), e(2), OsRef::Int(on as i32)])
                };
                calls.push((on, call));
            }
            calls.sort_by_key(|(on, _)| !*on);
            return Stmt::Os { exprs, calls: calls.into_iter().map(|(_, c)| c).collect() };
        }
        let v = match self.list_upto(5) {
            Ok(v) => v,
            Err(s) => return s,
        };
        let n = v.len();
        let mut exprs: Vec<(Expr, OsKind)> = v.into_iter().map(|e| (e, OsKind::Int)).collect();
        if n == 1 && self.next_sig() == TESCSTMT && self.peek_at(1) == TTINT {
            self.i += 2;
            match self.expr() {
                Ok(t) => exprs.push((t, OsKind::Int)),
                Err(mut p) => {
                    let mut d: Vec<Expr> = exprs.into_iter().map(|(e, _)| e).collect();
                    d.append(&mut p.done);
                    return Stmt::Fail(d, p.fail);
                }
            }
            return Stmt::Os { exprs, calls: vec![("rb_colour_tint", vec![OsRef::E(0), OsRef::E(1)])] };
        }
        let refs: Vec<OsRef> = (0..n).map(OsRef::E).collect();
        let call = match n {
            1 => ("rb_colour", refs),
            2 => ("rb_colour_pal", refs),
            3 => ("rb_colour_rgb", vec![OsRef::E(0), OsRef::E(1), OsRef::E(2), OsRef::Int(0)]),
            4 => ("rb_colour_rgb4", refs),
            _ => ("rb_colour_rgb5", refs),
        };
        Stmt::Os { exprs, calls: vec![call] }
    }

    /// `MOUSE` (§14.6.7).
    fn mouse_statement(&mut self) -> Stmt {
        match self.next_sig() {
            TON => {
                self.i += 1;
                if self.is_stmt_end() {
                    return os1("rb_mouse_on", vec![], vec![OsRef::Num(1)]);
                }
                self.ints(1, "rb_mouse_on", vec![])
            }
            TOFF => {
                self.i += 1;
                os1("rb_mouse_on", vec![], vec![OsRef::Num(0)])
            }
            TTO => {
                self.i += 1;
                self.ints(2, "rb_mouse_to", vec![])
            }
            TSTEP => {
                self.i += 1;
                match self.list_upto(2) {
                    Ok(v) => {
                        let b = if v.len() == 2 { OsRef::E(1) } else { OsRef::E(0) };
                        os1("rb_mouse_step", v.into_iter().map(|e| (e, OsKind::Int)).collect(), vec![OsRef::E(0), b])
                    }
                    Err(s) => s,
                }
            }
            TESCSTMT if self.peek_at(1) == TRECT => {
                self.i += 2;
                self.ints(4, "rb_mouse_rect", vec![])
            }
            TTEXT => {
                self.i += 1;
                self.ints(4, "rb_mouse_colour", vec![])
            }
            _ => {
                let mut targets = Vec::new();
                loop {
                    match self.place() {
                        Some(Ok(t)) => targets.push(t),
                        Some(Err(p)) => return Stmt::Fail(p.done, p.fail),
                        None => return Stmt::Fail(Vec::new(), Fail { number: 50, message: "Bad MOUSE variable" }),
                    }
                    if targets.len() == 4 || self.next_sig() != b',' {
                        break;
                    }
                    self.i += 1;
                }
                if targets.len() < 3 {
                    return Stmt::Fail(Vec::new(), Fail { number: 5, message: "Missing ," });
                }
                Stmt::Mouse(targets)
            }
        }
    }

    /// `CALL` address [`,` variable]... (§14.3.1, §16.9). The address is
    /// converted to an integer before the parameters are read. Each
    /// parameter must be a variable, an element or an indirection.
    fn call_statement(&mut self) -> Stmt {
        let addr = match self.expr() {
            Ok(e) => e,
            Err(p) => return Stmt::Fail(p.done, p.fail),
        };
        let mut params = Vec::new();
        while self.next_sig() == b',' {
            self.i += 1;
            // A whole array's descriptor is the interpreter's own (§16.9.4)
            let at = self.i;
            self.skip_spaces();
            if self.variable_name().is_some() && self.peek() == b'(' && self.peek_after_spaces(1) == b')' {
                return Stmt::Unsupported("an array as a CALL parameter");
            }
            self.i = at;
            match self.place() {
                Some(Ok(t)) => params.push(t),
                Some(Err(mut p)) => {
                    let mut d = vec![Expr::Func(Func::Int, vec![addr])];
                    d.append(&mut p.done);
                    return Stmt::Fail(d, p.fail);
                }
                None => return Stmt::Fail(vec![Expr::Func(Func::Int, vec![addr])], Fail::syntax()),
            }
        }
        Stmt::Call(addr, params)
    }

    /// `SYS` swi [`,` in]... [`TO` [out] [`,` [out]]... [`;` flags]] (§14.2).
    fn sys_statement(&mut self) -> Stmt {
        let swi = match self.expr() {
            Ok(e) => e,
            Err(p) => return Stmt::Fail(p.done, p.fail),
        };
        let mut ins: Vec<Option<Expr>> = Vec::new();
        while self.next_sig() == b',' {
            self.i += 1;
            let c = self.next_sig();
            if c == b',' || c == TTO || self.is_stmt_end() || c == b';' {
                ins.push(None);
                continue;
            }
            match self.expr() {
                Ok(e) => ins.push(Some(e)),
                Err(mut p) => {
                    let mut d = vec![swi];
                    d.extend(ins.into_iter().flatten());
                    d.append(&mut p.done);
                    return Stmt::Fail(d, p.fail);
                }
            }
        }
        let done = |swi: Expr, ins: Vec<Option<Expr>>| -> Vec<Expr> {
            let mut d = vec![swi];
            d.extend(ins.into_iter().flatten());
            d
        };
        if ins.len() > 10 {
            return Stmt::Fail(done(swi, ins), Fail { number: 51, message: "Too many input expressions for SYS" });
        }
        let mut outs: Vec<Option<Target>> = Vec::new();
        let mut flags = None;
        if self.next_sig() == TTO {
            self.i += 1;
            loop {
                let c = self.next_sig();
                if c == b',' {
                    self.i += 1;
                    outs.push(None);
                    continue;
                }
                if c == b';' || self.is_stmt_end() {
                    break;
                }
                match self.place() {
                    Some(Ok(t)) => outs.push(Some(t)),
                    Some(Err(p)) => return Stmt::Fail(p.done, p.fail),
                    None => break,
                }
                if self.next_sig() != b',' {
                    break;
                }
                self.i += 1;
            }
            if self.next_sig() == b';' {
                self.i += 1;
                match self.place() {
                    Some(Ok(t)) => flags = Some(t),
                    Some(Err(p)) => return Stmt::Fail(p.done, p.fail),
                    None => return Stmt::Fail(done(swi, ins), Fail::syntax()),
                }
            }
        }
        if outs.len() > 10 {
            return Stmt::Fail(done(swi, ins), Fail { number: 51, message: "Too many output variables for SYS" });
        }
        Stmt::Sys { swi, ins, outs, flags }
    }

    /// `VDU` (§14.5). Returns the values and their separators, and the
    /// error that stops the list, if there is one. That error is raised
    /// after the values before it are written.
    fn vdu_statement(&mut self) -> (Stmt, Option<Stmt>) {
        let mut items = Vec::new();
        loop {
            if self.is_stmt_end() {
                break;
            }
            match self.expr() {
                Ok(e) => {
                    let sep = match self.next_sig() {
                        b',' => 0,
                        b';' => 1,
                        b'|' => 2,
                        _ => {
                            items.push((e, 0));
                            continue;
                        }
                    };
                    self.i += 1;
                    items.push((e, sep));
                }
                Err(p) => return (Stmt::Vdu(items), Some(Stmt::Fail(p.done, p.fail))),
            }
        }
        (Stmt::Vdu(items), None)
    }

    /// `PRINT#`c`,` items (§13.5.1).
    fn print_file(&mut self) -> Stmt {
        let ch = match self.channel() {
            Ok(ch) => ch,
            Err(p) => return Stmt::Fail(p.done, p.fail),
        };
        let mut items = Vec::new();
        while self.next_sig() == b',' {
            self.i += 1;
            match self.expr() {
                Ok(e) => items.push(e),
                Err(p) => {
                    // The items before it have been written
                    return Stmt::PrintFile(ch, items.into_iter().chain([Expr::Fail(p.done, p.fail)]).collect());
                }
            }
        }
        Stmt::PrintFile(ch, items)
    }

    /// `INPUT#`c`,` variables (§13.6.1).
    fn input_file(&mut self) -> Stmt {
        let ch = match self.channel() {
            Ok(ch) => ch,
            Err(p) => return Stmt::Fail(p.done, p.fail),
        };
        let mut targets = Vec::new();
        while self.next_sig() == b',' {
            self.i += 1;
            match self.place() {
                Some(Ok(t)) => targets.push(t),
                Some(Err(p)) => return Stmt::Fail(p.done, p.fail),
                None => break,
            }
        }
        Stmt::InputFile(ch, targets)
    }

    /// `INPUT` and `INPUT LINE` (§11.5.2).
    fn input_statement(&mut self, line: bool) -> Stmt {
        let mut items = Vec::new();
        // Whether the last element was a prompt with nothing after it
        let mut after_prompt = false;
        loop {
            let c = self.next_sig();
            if self.is_stmt_end() {
                break;
            }
            match c {
                b',' | b';' => {
                    self.i += 1;
                    after_prompt = false;
                }
                b'"' => match self.string_literal() {
                    Ok(e) => {
                        items.push(InputItem::Prompt(PrintItem::Value(e)));
                        after_prompt = true;
                    }
                    Err(p) => return Stmt::Fail(p.done, p.fail),
                },
                b'\'' => {
                    self.i += 1;
                    items.push(InputItem::Prompt(PrintItem::Quote));
                    after_prompt = true;
                }
                TTAB => {
                    self.i += 1;
                    let x = match self.expr() {
                        Ok(e) => e,
                        Err(p) => return Stmt::Fail(p.done, p.fail),
                    };
                    let item = if self.next_sig() == b',' {
                        self.i += 1;
                        match self.expr() {
                            Ok(y) => PrintItem::TabXY(x, y),
                            Err(p) => return Stmt::Fail(p.done, p.fail),
                        }
                    } else {
                        PrintItem::Tab(x)
                    };
                    if self.next_sig() != b')' {
                        return Stmt::Fail(Vec::new(), missing_paren());
                    }
                    self.i += 1;
                    items.push(InputItem::Prompt(item));
                    after_prompt = true;
                }
                TSPC => {
                    self.i += 1;
                    match self.expr() {
                        Ok(e) => items.push(InputItem::Prompt(PrintItem::Spc(e))),
                        Err(p) => return Stmt::Fail(p.done, p.fail),
                    }
                    after_prompt = true;
                }
                _ => match self.place() {
                    Some(Ok(t)) => {
                        items.push(InputItem::Var(t, !after_prompt));
                        after_prompt = false;
                    }
                    Some(Err(p)) => return Stmt::Fail(p.done, p.fail),
                    None => break,
                },
            }
        }
        Stmt::Input { line, items }
    }

    /// A variable, an element or an indirection (§4.10.1, §11.2.1). If there
    /// is none, the result is None and the position is kept.
    fn place(&mut self) -> Option<PResult<Target>> {
        let c = self.next_sig();
        if c == b'?' || c == b'!' || c == b'|' || c == b'$' {
            let kind = match c {
                b'?' => Ind::Byte,
                b'!' => Ind::Word,
                b'|' => Ind::Real,
                _ => Ind::Str,
            };
            self.i += 1;
            return Some(self.factor().map(|a| Target::Ind(kind, a, None)));
        }
        let var = if c == b'@' && self.peek_at(1) == b'%' {
            self.i += 2;
            Var { name: b"@%".to_vec(), kind: VarKind::Int }
        } else {
            self.variable_name()?
        };
        let mut t = Target::Var(var.clone());
        if self.peek() == b'(' {
            self.i += 1;
            match self.paren_args() {
                Ok(subs) => t = Target::Elem(var, subs),
                Err(p) => return Some(Err(p)),
            }
        }
        let kind = match self.next_sig() {
            b'?' => Ind::Byte,
            b'!' => Ind::Word,
            _ => return Some(Ok(t)),
        };
        self.i += 1;
        let base = t.into_expr();
        Some(self.factor().map(|off| Target::Ind(kind, base, Some(off))))
    }

    /// `SWAP` v1`,`v2 (§4.10).
    fn swap_statement(&mut self) -> Stmt {
        let array_ref = "Can't use array reference here";
        let missing_comma = Fail { number: 5, message: "Missing ," };
        if let Some(a) = self.array_ref() {
            if self.next_sig() != b',' {
                return Stmt::Fail(Vec::new(), missing_comma);
            }
            self.i += 1;
            return match self.array_ref() {
                Some(b) => Stmt::SwapArrays(a, b),
                None => Stmt::Fail(Vec::new(), Fail { number: 26, message: array_ref }),
            };
        }
        let a = match self.place() {
            Some(Ok(t)) => t,
            Some(Err(p)) => return Stmt::Fail(p.done, p.fail),
            None => return Stmt::Fail(Vec::new(), Fail { number: 26, message: "Unknown or missing variable" }),
        };
        if self.next_sig() != b',' {
            return Stmt::Fail(Vec::new(), missing_comma);
        }
        self.i += 1;
        if self.array_ref().is_some() {
            return Stmt::Fail(Vec::new(), Fail { number: 26, message: array_ref });
        }
        match self.place() {
            Some(Ok(b)) => Stmt::Swap(a, b),
            Some(Err(p)) => Stmt::Fail(p.done, p.fail),
            None => Stmt::Fail(Vec::new(), Fail { number: 26, message: "Unknown or missing variable" }),
        }
    }

    /// `LEFT$(`v`,`n`)=`s, `MID$(`v`,`m`,`n`)=`s, `RIGHT$(`v`,`n`)=`s
    /// (§6.11.1), after the keyword and its bracket.
    fn substr_assignment(&mut self, c: u8) -> Stmt {
        let needed = Fail { number: 6, message: "Type mismatch: string variable needed" };
        let target = match self.place() {
            Some(Ok(t @ Target::Var(_))) | Some(Ok(t @ Target::Elem(..))) => t,
            Some(Ok(_)) | None => return Stmt::Fail(Vec::new(), needed),
            Some(Err(p)) => return Stmt::Fail(p.done, p.fail),
        };
        match &target {
            Target::Var(v) | Target::Elem(v, _) if v.kind != VarKind::Str => {
                let done = if let Target::Elem(_, s) = &target { s.clone() } else { Vec::new() };
                return Stmt::Fail(done, needed);
            }
            _ => {}
        }
        let mut args = Vec::new();
        while self.next_sig() == b',' {
            self.i += 1;
            match self.expr() {
                Ok(e) => args.push(e),
                Err(mut p) => {
                    args.append(&mut p.done);
                    return Stmt::Fail(args, p.fail);
                }
            }
        }
        let kind = match c {
            TLEFTD => 0,
            TMIDD => 1,
            _ => 2,
        };
        let most = if kind == 1 { 2 } else { 1 };
        if kind == 1 && args.is_empty() {
            return Stmt::Fail(args, Fail { number: 5, message: "Missing ," });
        }
        if args.len() > most || self.next_sig() != b')' {
            return Stmt::Fail(args, missing_paren());
        }
        self.i += 1;
        if self.next_sig() != b'=' {
            return Stmt::Fail(args, Fail { number: 4, message: "Mistake" });
        }
        self.i += 1;
        match self.expr() {
            Ok(value) => Stmt::SubAssign { kind, target, args, value },
            Err(mut p) => {
                args.append(&mut p.done);
                Stmt::Fail(args, p.fail)
            }
        }
    }

    /// `READ` v1, v2 ... (§11.2.1). Junk after a variable is left for the
    /// check after the statement. That check raises error 16 once the
    /// variables before the junk are read.
    fn read_statement(&mut self) -> Stmt {
        let mut targets = Vec::new();
        loop {
            if self.is_stmt_end() {
                break;
            }
            if self.next_sig() == b',' {
                self.i += 1;
                continue;
            }
            let c = self.next_sig();
            let t = if c == b'?' || c == b'!' || c == b'|' || c == b'$' {
                let kind = match c {
                    b'?' => Ind::Byte,
                    b'!' => Ind::Word,
                    b'|' => Ind::Real,
                    _ => Ind::Str,
                };
                self.i += 1;
                match self.factor() {
                    Ok(a) => Target::Ind(kind, a, None),
                    Err(mut p) => return Stmt::Fail(p.done.drain(..).collect(), p.fail),
                }
            } else {
                let var = if c == b'@' && self.peek_at(1) == b'%' {
                    self.i += 2;
                    Var { name: b"@%".to_vec(), kind: VarKind::Int }
                } else {
                    match self.variable_name() {
                        Some(v) => v,
                        None => break,
                    }
                };
                let mut t = Target::Var(var.clone());
                if self.peek() == b'(' {
                    self.i += 1;
                    match self.paren_args() {
                        Ok(subs) => t = Target::Elem(var, subs),
                        Err(p) => return Stmt::Fail(p.done, p.fail),
                    }
                }
                let kind = match self.next_sig() {
                    b'?' => Some(Ind::Byte),
                    b'!' => Some(Ind::Word),
                    _ => None,
                };
                if let Some(kind) = kind {
                    let base = match t {
                        Target::Var(v) => Expr::Var(v),
                        Target::Elem(v, s) => Expr::Elem(v, s),
                        t => t.into_expr(),
                    };
                    self.i += 1;
                    match self.factor() {
                        Ok(off) => t = Target::Ind(kind, base, Some(off)),
                        Err(p) => return Stmt::Fail(p.done, p.fail),
                    }
                }
                t
            };
            targets.push(t);
            if self.next_sig() != b',' {
                break;
            }
        }
        Stmt::Read(targets)
    }

    /// An assignment to a unary indirection (§12.2).
    fn ind_assignment(&mut self, c: u8) -> Stmt {
        let kind = match c {
            b'?' => Ind::Byte,
            b'!' => Ind::Word,
            b'|' => Ind::Real,
            _ => Ind::Str,
        };
        self.i += 1;
        match self.factor() {
            Ok(a) => self.assign_to(Target::Ind(kind, a, None)),
            Err(p) => Stmt::Fail(p.done, p.fail),
        }
    }

    fn assignment(&mut self) -> Stmt {
        let var = if self.peek() == b'@' && self.peek_at(1) == b'%' {
            self.i += 2;
            // @% is not an array (§4.4.3)
            if self.peek() == b'(' {
                return Stmt::Fail(Vec::new(), Fail::syntax());
            }
            Var { name: b"@%".to_vec(), kind: VarKind::Int }
        } else {
            match self.variable_name() {
                Some(v) => v,
                None => return Stmt::Fail(Vec::new(), Fail { number: 4, message: "Mistake" }),
            }
        };
        let mut target = Target::Var(var.clone());
        if self.peek() == b'(' {
            self.i += 1;
            if self.next_sig() == b')' {
                self.i += 1;
                return self.whole_array_assignment(var);
            }
            match self.paren_args() {
                Ok(subs) => target = Target::Elem(var, subs),
                Err(p) => return Stmt::Fail(p.done, p.fail),
            }
        }
        // A dyadic indirection as the target (§12.2.9)
        let kind = match self.next_sig() {
            b'?' => Some(Ind::Byte),
            b'!' => Some(Ind::Word),
            _ => None,
        };
        if let Some(kind) = kind {
            let base = match target {
                Target::Var(v) => Expr::Var(v),
                Target::Elem(v, s) => Expr::Elem(v, s),
                t => return self.assign_to(t),
            };
            self.i += 1;
            return match self.factor() {
                Ok(off) => self.assign_to(Target::Ind(kind, base, Some(off))),
                Err(p) => Stmt::Fail(p.done, p.fail),
            };
        }
        self.assign_to(target)
    }

    /// A whole-array assignment, after the target's `()` (§4.7.1-4.7.2).
    /// What follows a complete form is left for the check for junk. An
    /// operator after an array, or after a second array, raises error 16.
    fn whole_array_assignment(&mut self, a: ArrVar) -> Stmt {
        let op = |c: u8| match c {
            b'+' => Some(WaOp::Add),
            b'-' => Some(WaOp::Sub),
            b'*' => Some(WaOp::Mul),
            b'/' => Some(WaOp::Div),
            _ => None,
        };
        let c = self.next_sig();
        if (c == b'+' || c == b'-') && self.peek_at(1) == b'=' {
            self.i += 2;
            return match self.expr() {
                Ok(e) => Stmt::WholeArray(a, WaValue::AddEq(e, c == b'-')),
                Err(p) => Stmt::Fail(p.done, p.fail),
            };
        }
        if c != b'=' {
            return Stmt::Fail(Vec::new(), Fail { number: 4, message: "Mistake" });
        }
        self.i += 1;
        // -B()
        if self.next_sig() == b'-' {
            let save = self.i;
            self.i += 1;
            if let Some(b) = self.array_ref() {
                return Stmt::WholeArray(a, WaValue::Neg(b));
            }
            self.i = save;
        }
        if let Some(b) = self.array_ref() {
            let c = self.next_sig();
            if let Some(o) = op(c) {
                self.i += 1;
                if let Some(cc) = self.array_ref() {
                    return Stmt::WholeArray(a, WaValue::Op(o, b, cc));
                }
                return match self.factor() {
                    Ok(f) => Stmt::WholeArray(a, WaValue::OpRight(o, b, f)),
                    Err(p) => Stmt::Fail(p.done, p.fail),
                };
            }
            if c == b'.' {
                self.i += 1;
                if let Some(cc) = self.array_ref() {
                    return Stmt::WholeArray(a, WaValue::MatMul(b, cc));
                }
                return match self.factor() {
                    Ok(f) => Stmt::WholeArray(a, WaValue::DotFactor(b, f)),
                    Err(p) => Stmt::Fail(p.done, p.fail),
                };
            }
            return Stmt::WholeArray(a, WaValue::Copy(b));
        }
        let f = match self.factor() {
            Ok(f) => f,
            Err(p) => return Stmt::Fail(p.done, p.fail),
        };
        let c = self.next_sig();
        if c == b',' {
            let mut items = vec![f];
            while self.next_sig() == b',' {
                self.i += 1;
                match self.expr() {
                    Ok(e) => items.push(e),
                    Err(mut p) => {
                        items.append(&mut p.done);
                        return Stmt::Fail(items, p.fail);
                    }
                }
            }
            return Stmt::WholeArray(a, WaValue::List(items));
        }
        if let Some(o) = op(c) {
            self.i += 1;
            return match self.array_ref() {
                Some(b) => Stmt::WholeArray(a, WaValue::OpLeft(o, f, b)),
                None => Stmt::Fail(vec![f], Fail { number: 6, message: "Type mismatch: array needed" }),
            };
        }
        Stmt::WholeArray(a, WaValue::Set(f))
    }

    /// The array of `SUM`, `SUMLEN` and `MOD`. It is name`()`, in brackets
    /// or not (§4.8.5).
    fn array_operand(&mut self, f: ArrayFunc) -> PResult<Expr> {
        let save = self.i;
        if self.next_sig() == b'(' {
            self.i += 1;
            if let Some(v) = self.array_ref() {
                if self.next_sig() == b')' {
                    self.i += 1;
                    return Ok(Expr::ArrayFunc(f, v));
                }
            }
            self.i = save;
        }
        match self.array_ref() {
            Some(v) => Ok(Expr::ArrayFunc(f, v)),
            None => fail(Fail { number: 14, message: "Unknown array" }),
        }
    }

    /// `=`, `+=` or `-=`, then the value.
    fn assign_to(&mut self, target: Target) -> Stmt {
        let c = self.next_sig();
        let op = match c {
            b'=' => {
                self.i += 1;
                AssignOp::Set
            }
            b'+' if self.peek_at(1) == b'=' => {
                self.i += 2;
                AssignOp::Add
            }
            b'-' if self.peek_at(1) == b'=' => {
                self.i += 2;
                AssignOp::Sub
            }
            _ => return Stmt::Fail(Vec::new(), Fail { number: 4, message: "Mistake" }),
        };
        match self.expr() {
            Ok(e) => Stmt::Assign(target, op, e, self.let_),
            Err(p) => Stmt::Fail(p.done, p.fail),
        }
    }

    // ---- DIM (§4.5) -------------------------------------------------------

    fn dim_statement(&mut self) -> Stmt {
        let mut items = Vec::new();
        loop {
            self.skip_spaces();
            let v = match self.variable_name() {
                Some(v) => v,
                None => return Stmt::Fail(vec![], Fail { number: 10, message: "Bad DIM statement" }),
            };
            if self.peek() != b'(' {
                // DIM of a block of memory (§12.1.1)
                if v.kind == VarKind::Str {
                    return Stmt::Fail(vec![], Fail { number: 6, message: "Type mismatch: numeric variable needed" });
                }
                let local = self.next_sig() == TLOCAL;
                if local {
                    self.i += 1;
                }
                match self.expr() {
                    Ok(n) if local => items.push(DimItem::LocalBlock(v, n)),
                    Ok(n) => items.push(DimItem::Block(v, n)),
                    Err(p) => return Stmt::Fail(p.done, p.fail),
                }
                if self.next_sig() == b',' {
                    self.i += 1;
                    continue;
                }
                break;
            }
            self.i += 1;
            let mut bounds = Vec::new();
            loop {
                match self.expr() {
                    Ok(e) => bounds.push(e),
                    Err(p) => return Stmt::Fail(p.done, p.fail),
                }
                match self.next_sig() {
                    b',' => self.i += 1,
                    b')' => {
                        self.i += 1;
                        break;
                    }
                    _ => return Stmt::Fail(bounds, Fail { number: 10, message: "No end of dimension list )" }),
                }
            }
            items.push(DimItem::Array(v, bounds));
            if self.next_sig() == b',' {
                self.i += 1;
                continue;
            }
            break;
        }
        Stmt::Dim(items)
    }

    /// A name and its suffix, at self.i (§2.5.1, §2.5.3).
    fn variable_name(&mut self) -> Option<Var> {
        let s = self.i;
        if !is_name_start(self.peek()) {
            return None;
        }
        while is_name_char(self.peek()) {
            self.i += 1;
        }
        let kind = match self.peek() {
            b'%' => {
                self.i += 1;
                VarKind::Int
            }
            b'$' => {
                self.i += 1;
                VarKind::Str
            }
            _ => VarKind::Real,
        };
        Some(Var { name: self.t[s..self.i].to_vec(), kind })
    }

    // ---- IF (§7.2, §7.3) ------------------------------------------------

    fn if_statement(&mut self, start: usize) -> Vec<Parsed> {
        let cond = match self.expr() {
            Ok(e) => e,
            Err(p) => {
                return vec![Parsed { start, end: self.t.len(), stmt: Stmt::Fail(p.done, p.fail) }];
            }
        };
        let cond_end = self.i;
        let c = self.next_sig();
        // A relation after the condition's relation ends the condition.
        // The IF then fails when it runs (§5.1.8).
        if c == b'<' || c == b'>' || c == b'=' {
            return vec![Parsed { start, end: self.t.len(), stmt: Stmt::Fail(vec![cond], Fail::syntax()) }];
        }
        if c == TTHEN {
            self.i += 1;
            if self.i >= self.t.len() {
                // THEN is the last byte of the line, so this is a block IF
                // (§7.3.1).
                return vec![Parsed { start, end: self.i, stmt: Stmt::BlockIf(cond) }];
            }
            let mut v = vec![Parsed { start, end: cond_end, stmt: Stmt::If(cond) }];
            if self.next_sig() == TCONST {
                let s = self.i;
                let n = self.line_ref();
                // Anything after the reference is ignored (§7.2.4). But the
                // scan still finds an ELSE after it.
                v.push(Parsed { start: s, end: self.i, stmt: Stmt::Goto(n) });
                while !self.at_end() && self.peek() != TELSE {
                    self.i += 1;
                }
            }
            self.glued = true;
            return v;
        }
        // THEN may be left out (§7.2.1)
        self.glued = true;
        vec![Parsed { start, end: cond_end, stmt: Stmt::If(cond) }]
    }

    // ---- FOR, NEXT (§7.4) -----------------------------------------------

    fn for_statement(&mut self) -> Stmt {
        let bad = Fail { number: 34, message: "Bad FOR control variable" };
        // A numeric variable, element or indirection (§7.4.2)
        if self.array_ref().is_some() {
            return Stmt::Fail(vec![], bad);
        }
        let var = match self.place() {
            Some(Ok(Target::Var(v))) | Some(Ok(Target::Elem(v, _))) if v.kind == VarKind::Str => return Stmt::Fail(vec![], bad),
            Some(Ok(Target::Ind(Ind::Str, ..))) | None => return Stmt::Fail(vec![], bad),
            Some(Ok(t)) => t,
            Some(Err(p)) => return Stmt::Fail(p.done, p.fail),
        };
        if self.next_sig() != b'=' {
            return Stmt::Fail(vec![], Fail { number: 4, message: "Missing = in FOR statement" });
        }
        self.i += 1;
        let start = match self.expr() {
            Ok(e) => e,
            Err(p) => return Stmt::Fail(p.done, p.fail),
        };
        if self.next_sig() != TTO {
            return Stmt::Fail(vec![start], Fail { number: 36, message: "Missing TO" });
        }
        self.i += 1;
        let limit = match self.expr() {
            Ok(e) => e,
            Err(p) => return Stmt::Fail(after(p, start).done, Fail::syntax()),
        };
        let step = if self.next_sig() == TSTEP {
            self.i += 1;
            match self.expr() {
                Ok(e) => Some(e),
                Err(p) => {
                    let mut d = vec![start, limit];
                    d.extend(p.done);
                    return Stmt::Fail(d, p.fail);
                }
            }
        } else {
            None
        };
        Stmt::For { var, start, limit, step }
    }

    /// `NEXT` items separated by commas, each a variable or empty
    /// (§7.4.10). `NEXT ,` is two empty items. `NEXT j%,` is j% and then
    /// an empty item.
    fn next_statement(&mut self) -> Stmt {
        let mut vars = Vec::new();
        loop {
            self.skip_spaces();
            if self.is_stmt_end() || self.peek() == b',' {
                vars.push(None);
            } else {
                match self.variable_name() {
                    Some(v) if v.kind != VarKind::Str => vars.push(Some(v)),
                    _ => return Stmt::Fail(vec![], Fail::syntax()),
                }
            }
            if self.next_sig() == b',' {
                self.i += 1;
                continue;
            }
            break;
        }
        Stmt::Next(vars)
    }

    // ---- CASE (§7.7) ----------------------------------------------------

    fn case_statement(&mut self) -> Stmt {
        let e = match self.expr() {
            Ok(e) => e,
            Err(p) => return Stmt::Fail(p.done, p.fail),
        };
        if self.next_sig() != TOF {
            return Stmt::Fail(vec![e], Fail { number: 48, message: "OF missing from CASE statement" });
        }
        self.i += 1;
        if self.i < self.t.len() {
            return Stmt::Fail(vec![e], Fail { number: 48, message: "CASE..OF statement must be the last thing on a line" });
        }
        Stmt::Case(e)
    }

    fn when_statement(&mut self) -> Stmt {
        let mut values = Vec::new();
        loop {
            match self.expr() {
                Ok(e) => values.push(e),
                Err(_) => break,
            }
            if self.next_sig() == b',' {
                self.i += 1;
                continue;
            }
            break;
        }
        Stmt::When(values)
    }

    // ---- GOSUB, ON (§7.8) -------------------------------------------------

    fn on_statement(&mut self) -> Stmt {
        let index = match self.expr() {
            Ok(e) => e,
            Err(p) => return Stmt::Fail(p.done, p.fail),
        };
        let gosub = match self.next_sig() {
            TGOTO => false,
            TGOSUB => true,
            TPROC => return self.on_proc(index),
            _ => return Stmt::Fail(vec![index], Fail { number: 39, message: "ON syntax" }),
        };
        self.i += 1;
        let mut lines = Vec::new();
        loop {
            if self.next_sig() != TCONST {
                self.i = self.t.len();
                return Stmt::Rejected("Computed line number");
            }
            lines.push(self.line_ref());
            if self.next_sig() == b',' {
                self.i += 1;
                continue;
            }
            break;
        }
        // The scan for ELSE goes on past a colon to the end of the line
        // (§7.8.8)
        let has_else = self.t[self.i..].contains(&TELSE);
        Stmt::On { index, gosub, lines, has_else }
    }

    /// The list of `ON` ... `PROC` (§7.8.8, §7.8.10).
    fn on_proc(&mut self, index: Expr) -> Stmt {
        let from = self.i;
        let mut items = Vec::new();
        loop {
            if self.next_sig() != TPROC {
                return Stmt::Fail(vec![index], Fail { number: 39, message: "ON syntax" });
            }
            self.i += 1;
            let s = self.i;
            while is_routine_char(self.peek()) {
                self.i += 1;
            }
            let name = self.t[s..self.i].to_vec();
            if name.is_empty() {
                return Stmt::Fail(vec![index], Fail { number: 30, message: "Bad call of function/procedure" });
            }
            let args = match self.arguments() {
                Ok(a) => a,
                Err(p) => return Stmt::Fail(p.done, p.fail),
            };
            items.push((name, args, self.i));
            if self.next_sig() != b',' {
                break;
            }
            self.i += 1;
        }
        // The scans go byte by byte. So a string that holds a parenthesis,
        // a colon or ELSE's byte misleads them (§7.11.6).
        let mut in_string = false;
        let mut ambiguous = false;
        for &b in &self.t[from..self.i] {
            if b == b'"' {
                in_string = !in_string;
            } else if in_string && matches!(b, b'(' | b')' | b':' | TELSE) {
                ambiguous = true;
            }
        }
        let has_else = self.t[self.i..].contains(&TELSE);
        Stmt::OnProc { index, items, has_else, ambiguous }
    }

    // ---- ERROR (§9.3) -------------------------------------------------------

    fn error_statement(&mut self) -> Stmt {
        let ext = self.next_sig() == TEXT;
        if ext {
            self.i += 1;
        }
        let number = match self.expr() {
            Ok(e) => e,
            Err(p) => return Stmt::Fail(p.done, p.fail),
        };
        if self.next_sig() != b',' {
            return Stmt::Fail(vec![number], Fail { number: 5, message: "Missing ," });
        }
        self.i += 1;
        match self.expr() {
            Ok(message) => Stmt::Error { ext, number, message },
            Err(p) => {
                let mut d = vec![number];
                d.extend(p.done);
                Stmt::Fail(d, p.fail)
            }
        }
    }

    // ---- DEF, PROC, LOCAL (chapter 8) ---------------------------------------

    fn def_statement(&mut self) -> Stmt {
        let c = self.next_sig();
        let is_fn = c == TFN;
        if c != TFN && c != TPROC {
            self.i = self.t.len();
            return Stmt::Nothing;
        }
        self.i += 1;
        let s = self.i;
        while is_routine_char(self.peek()) {
            self.i += 1;
        }
        let name = self.t[s..self.i].to_vec();
        let mut params = Vec::new();
        let unsupported: Option<&'static str> = None;
        let mut rejected = None;
        if self.peek() == b'(' {
            self.i += 1;
            loop {
                self.skip_spaces();
                let mut kind = ParamKind::Value;
                if self.peek() == TRETURN {
                    self.i += 1;
                    self.skip_spaces();
                    kind = ParamKind::Return;
                }
                let v = match self.variable_name() {
                    Some(v) => v,
                    None => {
                        rejected = Some("Formal parameter must be a variable");
                        break;
                    }
                };
                if self.peek() == b'(' {
                    // name() is an array parameter (§8.5). An element is
                    // rejected (§8.12.2).
                    if self.peek_after_spaces(1) == b')' {
                        self.i += 1;
                        self.skip_spaces();
                        self.i += 1;
                        kind = ParamKind::Array;
                    } else {
                        rejected = Some("Formal parameter must be a variable");
                        break;
                    }
                }
                params.push(Param { var: v, kind });
                if self.next_sig() == b',' {
                    self.i += 1;
                    continue;
                }
                break;
            }
            if unsupported.is_some() || rejected.is_some() {
                while !self.at_end() && self.peek() != b')' {
                    self.i += 1;
                }
            }
            if self.next_sig() == b')' {
                self.i += 1;
            }
        }
        // Only a DEF that is the first item of its line defines a routine
        // (§2.6.3, §8.1.2). Any other DEF is only a comment when it runs.
        if !self.first {
            self.i = self.t.len();
            return Stmt::Def;
        }
        self.def = Some(Def { name, is_fn, params, body: self.i, unsupported, rejected });
        // The body may begin on the DEF's own line, straight after the
        // parameters. Running into a DEF treats the rest of its line as a
        // comment (§8.10.1), so flow.rs sends the Def node to the next
        // line.
        self.glued = true;
        Stmt::Def
    }

    fn proc_call(&mut self) -> Stmt {
        let s = self.i;
        while is_routine_char(self.peek()) {
            self.i += 1;
        }
        let name = self.t[s..self.i].to_vec();
        if name.is_empty() {
            return Stmt::Fail(vec![], Fail { number: 30, message: "Bad call of function/procedure" });
        }
        match self.arguments() {
            Ok(args) => Stmt::Proc(name, args),
            Err(p) => Stmt::Fail(p.done, p.fail),
        }
    }

    fn arguments(&mut self) -> PResult<Vec<Expr>> {
        let mut args = Vec::new();
        // Spaces may stand before the bracket (§2.5.12)
        if self.next_sig() != b'(' {
            return Ok(args);
        }
        self.i += 1;
        loop {
            if let Some(v) = self.whole_array() {
                args.push(Expr::ArrayRef(v));
                match self.next_sig() {
                    b',' => {
                        self.i += 1;
                        continue;
                    }
                    b')' => {
                        self.i += 1;
                        return Ok(args);
                    }
                    _ => return Err(Partial { done: args, fail: missing_paren() }),
                }
            }
            // A bracketed argument is marked, so that it is not taken as a
            // variable for a RETURN formal (§8.4.4)
            let bracketed = self.next_sig() == b'(';
            match self.expr() {
                Ok(e) if bracketed => args.push(Expr::Unary(UnOp::Plus, Box::new(e))),
                Ok(e) => args.push(e),
                Err(mut p) => {
                    let mut d = args;
                    d.append(&mut p.done);
                    p.done = d;
                    return Err(p);
                }
            }
            match self.next_sig() {
                b',' => {
                    self.i += 1;
                }
                b')' => {
                    self.i += 1;
                    return Ok(args);
                }
                _ => return Err(Partial { done: args, fail: missing_paren() }),
            }
        }
    }

    /// name`()` standing alone, followed by `,` or `)`. This is a whole
    /// array (§4.6.1). The position is kept if it is not one.
    fn whole_array(&mut self) -> Option<ArrVar> {
        let save = self.i;
        if let Some(v) = self.array_ref() {
            let c = self.next_sig();
            if c == b',' || c == b')' {
                return Some(v);
            }
        }
        self.i = save;
        None
    }

    /// name`()`, whatever follows.  The position is kept if it is not one.
    fn array_ref(&mut self) -> Option<ArrVar> {
        let save = self.i;
        self.skip_spaces();
        if let Some(v) = self.variable_name() {
            if self.peek() == b'(' && self.peek_after_spaces(1) == b')' {
                self.i += 1;
                self.skip_spaces();
                self.i += 1;
                return Some(v);
            }
        }
        self.i = save;
        None
    }

    fn local_statement(&mut self) -> Stmt {
        let mut vars = Vec::new();
        loop {
            self.skip_spaces();
            // A unary indirection (§8.6.1)
            let c = self.peek();
            if c == b'?' || c == b'!' || c == b'$' || c == b'|' {
                if c == b'|' {
                    self.i = self.t.len();
                    return Stmt::Unsupported("LOCAL of a | indirection");
                }
                self.i += 1;
                let kind = match c {
                    b'?' => Ind::Byte,
                    b'!' => Ind::Word,
                    _ => Ind::Str,
                };
                match self.factor() {
                    Ok(base) => vars.push(LocalItem::Ind(kind, base, None)),
                    Err(p) => return Stmt::Fail(p.done, p.fail),
                }
                if self.next_sig() == b',' {
                    self.i += 1;
                    continue;
                }
                break;
            }
            let v = if self.peek() == b'@' && self.peek_at(1) == b'%' {
                self.i += 2;
                Var { name: b"@%".to_vec(), kind: VarKind::Int }
            } else {
                match self.variable_name() {
                    Some(v) => v,
                    None => {
                        self.i = self.t.len();
                        return Stmt::Unsupported("LOCAL other than simple variables");
                    }
                }
            };
            // A dyadic indirection
            let c = self.next_sig();
            if c == b'?' || c == b'!' {
                self.i += 1;
                let kind = if c == b'?' { Ind::Byte } else { Ind::Word };
                match self.factor() {
                    Ok(off) => vars.push(LocalItem::Ind(kind, Expr::Var(v), Some(off))),
                    Err(p) => return Stmt::Fail(p.done, p.fail),
                }
                if self.next_sig() == b',' {
                    self.i += 1;
                    continue;
                }
                break;
            }
            if self.peek() == b'(' {
                if self.peek_after_spaces(1) != b')' {
                    return Stmt::Fail(vec![], Fail::syntax());
                }
                self.i += 1;
                self.skip_spaces();
                self.i += 1;
                vars.push(LocalItem::Array(v));
            } else {
                vars.push(LocalItem::Var(v));
            }
            if self.next_sig() == b',' {
                self.i += 1;
                continue;
            }
            break;
        }
        Stmt::Local(vars)
    }

    // ---- Expressions (§5.1) ---------------------------------------------

    pub fn expr(&mut self) -> PResult<Expr> {
        self.level7()
    }

    fn level7(&mut self) -> PResult<Expr> {
        let mut l = self.level6()?;
        loop {
            let op = match self.next_sig() {
                TOR => BinOp::Or,
                TEOR => BinOp::Eor,
                _ => return Ok(l),
            };
            self.i += 1;
            let r = self.level6().map_err(|p| after(p, l.clone()))?;
            l = Expr::Bin(op, Box::new(l), Box::new(r));
        }
    }

    fn level6(&mut self) -> PResult<Expr> {
        let mut l = self.level5()?;
        while self.next_sig() == TAND {
            self.i += 1;
            let r = self.level5().map_err(|p| after(p, l.clone()))?;
            l = Expr::Bin(BinOp::And, Box::new(l), Box::new(r));
        }
        Ok(l)
    }

    /// Level 5 does not chain (§5.1.6). It takes one operator at most.
    fn level5(&mut self) -> PResult<Expr> {
        let l = self.level4()?;
        let c = self.next_sig();
        let (op, n) = match (c, self.peek_at(1), self.peek_at(2)) {
            (b'=', _, _) => (BinOp::Eq, 1),
            (b'<', b'>', _) => (BinOp::Ne, 2),
            (b'<', b'=', _) => (BinOp::Le, 2),
            (b'<', b'<', _) => (BinOp::Shl, 2),
            (b'<', _, _) => (BinOp::Lt, 1),
            (b'>', b'=', _) => (BinOp::Ge, 2),
            (b'>', b'>', b'>') => (BinOp::Lsr, 3),
            (b'>', b'>', _) => (BinOp::Asr, 2),
            (b'>', _, _) => (BinOp::Gt, 1),
            _ => return Ok(l),
        };
        self.i += n;
        let r = self.level4().map_err(|p| after(p, l.clone()))?;
        Ok(Expr::Bin(op, Box::new(l), Box::new(r)))
    }

    fn level4(&mut self) -> PResult<Expr> {
        let mut l = self.level3()?;
        loop {
            let op = match (self.next_sig(), self.peek_at(1)) {
                (b'+', b'=') | (b'-', b'=') => return Ok(l),
                (b'+', _) => BinOp::Add,
                (b'-', _) => BinOp::Sub,
                _ => return Ok(l),
            };
            self.i += 1;
            let r = self.level3().map_err(|p| after(p, l.clone()))?;
            l = Expr::Bin(op, Box::new(l), Box::new(r));
        }
    }

    fn level3(&mut self) -> PResult<Expr> {
        let mut l = self.level2()?;
        loop {
            let op = match self.next_sig() {
                b'*' => BinOp::Mul,
                b'/' => BinOp::Div,
                TDIV => BinOp::IDiv,
                TMOD => BinOp::Mod,
                _ => return Ok(l),
            };
            self.i += 1;
            let r = self.level2().map_err(|p| after(p, l.clone()))?;
            l = Expr::Bin(op, Box::new(l), Box::new(r));
        }
    }

    /// `^`, from left to right. Each right operand is a single factor
    /// (§5.1.2-4).
    fn level2(&mut self) -> PResult<Expr> {
        let mut l = self.factor()?;
        while self.next_sig() == b'^' {
            self.i += 1;
            let r = self.factor().map_err(|p| after(p, l.clone()))?;
            l = Expr::Bin(BinOp::Pow, Box::new(l), Box::new(r));
        }
        Ok(l)
    }

    fn factor(&mut self) -> PResult<Expr> {
        let c = self.next_sig();
        match c {
            b'-' => {
                self.i += 1;
                Ok(Expr::Unary(UnOp::Neg, Box::new(self.factor()?)))
            }
            b'+' => {
                self.i += 1;
                Ok(Expr::Unary(UnOp::Plus, Box::new(self.factor()?)))
            }
            TNOT => {
                self.i += 1;
                Ok(Expr::Unary(UnOp::Not, Box::new(self.factor()?)))
            }
            b'(' => {
                self.i += 1;
                let e = self.expr()?;
                if self.next_sig() != b')' {
                    return Err(Partial { done: vec![e], fail: missing_paren() });
                }
                self.i += 1;
                Ok(e)
            }
            b'"' => self.string_literal(),
            b'0'..=b'9' | b'.' => Ok(self.decimal()),
            b'&' => self.hex(),
            b'%' => self.binary(),
            b'@' if self.peek_at(1) == b'%' => {
                self.i += 2;
                Ok(Expr::Var(Var { name: b"@%".to_vec(), kind: VarKind::Int }))
            }
            TFN => {
                self.i += 1;
                let s = self.i;
                while is_routine_char(self.peek()) {
                    self.i += 1;
                }
                let name = self.t[s..self.i].to_vec();
                if name.is_empty() {
                    return fail(Fail { number: 30, message: "Bad call of function/procedure" });
                }
                let args = self.arguments()?;
                Ok(Expr::Fn(name, args))
            }
            c if is_name_start(c) => {
                let v = self.variable_name().unwrap();
                let base = if self.peek() == b'(' {
                    self.i += 1;
                    if self.next_sig() == b')' {
                        // A whole array where none is allowed (§4.6.1)
                        self.i += 1;
                        return fail(Fail { number: 26, message: "Can't use array reference here" });
                    }
                    let subs = self.paren_args()?;
                    Expr::Elem(v, subs)
                } else {
                    Expr::Var(v)
                };
                // The dyadic ? and ! (§12.2.1). These are a variable or an
                // element, then the operator, then a factor.
                let kind = match self.next_sig() {
                    b'?' => Ind::Byte,
                    b'!' => Ind::Word,
                    _ => return Ok(base),
                };
                self.i += 1;
                let off = self.factor().map_err(|p| after(p, base.clone()))?;
                Ok(Expr::Ind(kind, Box::new(base), Some(Box::new(off))))
            }
            b'?' | b'!' | b'|' | b'$' => {
                let kind = match c {
                    b'?' => Ind::Byte,
                    b'!' => Ind::Word,
                    b'|' => Ind::Real,
                    _ => Ind::Str,
                };
                self.i += 1;
                let a = self.factor()?;
                Ok(Expr::Ind(kind, Box::new(a), None))
            }
            _ => self.function(c),
        }
    }

    fn function(&mut self, c: u8) -> PResult<Expr> {
        let simple = |f: Func| Some(f);
        let one = match c {
            TABS => simple(Func::Abs),
            TSGN => simple(Func::Sgn),
            TINT => simple(Func::Int),
            TSQR => simple(Func::Sqr),
            TSIN => simple(Func::Sin),
            TCOS => simple(Func::Cos),
            TTAN => simple(Func::Tan),
            TATN => simple(Func::Atn),
            TASN => simple(Func::Asn),
            TACS => simple(Func::Acs),
            TEXP => simple(Func::Exp),
            TLN => simple(Func::Ln),
            TLOG => simple(Func::Log),
            TDEG => simple(Func::Deg),
            TRAD => simple(Func::Rad),
            TLEN => simple(Func::Len),
            TASC => simple(Func::Asc),
            TCHRD => simple(Func::Chr),
            TVAL => simple(Func::Val),
            TEVAL => simple(Func::Eval),
            // USR's address is a factor (§14.3.1)
            TUSR => simple(Func::Usr),
            _ => None,
        };
        if let Some(f) = one {
            self.i += 1;
            let a = self.factor()?;
            return Ok(Expr::Func(f, vec![a]));
        }
        let none = match c {
            TPI => Some(Func::Pi),
            TTRUE => Some(Func::True),
            TFALSE => Some(Func::False),
            TCOUNT => Some(Func::Count),
            TERR => Some(Func::Err),
            TERL => Some(Func::Erl),
            TWIDTH => Some(Func::Width),
            _ => None,
        };
        if let Some(f) = none {
            self.i += 1;
            return Ok(Expr::Func(f, vec![]));
        }
        match c {
            TSTRD => {
                self.i += 1;
                if self.next_sig() == b'~' {
                    self.i += 1;
                    let a = self.factor()?;
                    return Ok(Expr::Func(Func::StrHex, vec![a]));
                }
                let a = self.factor()?;
                Ok(Expr::Func(Func::Str, vec![a]))
            }
            TREPORT if self.peek_at(1) == b'$' => {
                self.i += 2;
                Ok(Expr::Func(Func::ReportS, vec![]))
            }
            TRND => {
                self.i += 1;
                if self.peek() == b'(' {
                    self.i += 1;
                    let a = self.expr()?;
                    if self.next_sig() != b')' {
                        return Err(Partial { done: vec![a], fail: missing_paren() });
                    }
                    self.i += 1;
                    return Ok(Expr::Func(Func::RndArg, vec![a]));
                }
                Ok(Expr::Func(Func::Rnd, vec![]))
            }
            TLEFTD | TRIGHTD | TMIDD | TINSTR | TSTRND => {
                self.i += 1;
                let args = self.paren_args()?;
                let f = match (c, args.len()) {
                    (TLEFTD, 1) => Func::Left1,
                    (TLEFTD, 2) => Func::Left,
                    (TRIGHTD, 1) => Func::Right1,
                    (TRIGHTD, 2) => Func::Right,
                    (TMIDD, 2) => Func::Mid2,
                    (TMIDD, 3) => Func::Mid,
                    (TINSTR, 2) => Func::Instr,
                    (TINSTR, 3) => Func::Instr3,
                    (TSTRND, 2) => Func::StringN,
                    _ => return Err(Partial { done: args, fail: missing_paren() }),
                };
                Ok(Expr::Func(f, args))
            }
            // Files (chapter 13). The argument is a name, which is a factor
            // (§13.2.4), or a channel.
            TOPENI | TOPENO | TOPENU => {
                self.i += 1;
                // The names in hdr/Tokens are the wrong way round. OPENIN
                // is &8E (TOPENU) and OPENUP is &AD (TOPENI), as the keyword
                // table has them.
                let f = match c {
                    TOPENU => Func::Openin,
                    TOPENI => Func::Openup,
                    _ => Func::Openout,
                };
                Ok(Expr::Func(f, vec![self.factor()?]))
            }
            TBGET | TEOF | TEXT | TPTR => {
                self.i += 1;
                let f = match c {
                    TBGET => Func::Bget,
                    TEOF => Func::Eof,
                    TEXT => Func::Ext,
                    _ => Func::Ptr,
                };
                Ok(Expr::Func(f, vec![self.channel()?]))
            }
            TGETD if self.peek_after_spaces(1) == b'#' => {
                self.i += 1;
                Ok(Expr::Func(Func::GetDFile, vec![self.channel()?]))
            }
            // The keyboard (§11.6)
            TGET | TGETD => {
                self.i += 1;
                Ok(Expr::Func(if c == TGET { Func::Get } else { Func::GetD }, vec![]))
            }
            TINKEY | TINKED | TADC => {
                self.i += 1;
                let f = match c {
                    TINKEY => Func::Inkey,
                    TINKED => Func::InkeyD,
                    _ => Func::Adval,
                };
                Ok(Expr::Func(f, vec![self.factor()?]))
            }
            TPOS | TVPOS => {
                self.i += 1;
                Ok(Expr::Func(if c == TPOS { Func::Pos } else { Func::Vpos }, vec![]))
            }
            // POINT( is one token (§14.6.8)
            TPOINT => {
                self.i += 1;
                let args = self.paren_args()?;
                if args.len() != 2 {
                    return Err(Partial { done: args, fail: missing_paren() });
                }
                Ok(Expr::Func(Func::PointXY, args))
            }
            TMODE => {
                self.i += 1;
                Ok(Expr::Func(Func::ModeFn, vec![]))
            }
            TVDU => {
                self.i += 1;
                Ok(Expr::Func(Func::VduFn, vec![self.factor()?]))
            }
            TESCSTMT if self.peek_at(1) == TTINT => {
                self.i += 2;
                if self.next_sig() != b'(' {
                    return fail(Fail::syntax());
                }
                self.i += 1;
                let args = self.paren_args()?;
                if args.len() != 2 {
                    return Err(Partial { done: args, fail: missing_paren() });
                }
                Ok(Expr::Func(Func::TintXY, args))
            }
            TESCSTMT if self.peek_at(1) == TBEATS || self.peek_at(1) == TTEMPO => {
                let f = if self.peek_at(1) == TBEATS { Func::Beats } else { Func::Tempo };
                self.i += 2;
                Ok(Expr::Func(f, vec![]))
            }
            // QUIT is TRUE in a compiled program (§14.8.4)
            TESCSTMT if self.peek_at(1) == TQUIT => {
                self.i += 2;
                Ok(Expr::Func(Func::True, vec![]))
            }
            // SYS is not a function (§14.2.9)
            TESCSTMT if self.peek_at(1) == TSYS => fail(Fail::missing_var()),
            TPAGE => {
                self.i += 1;
                Ok(Expr::Pseudo(Pseudo::Page))
            }
            TLOMEM => {
                self.i += 1;
                Ok(Expr::Pseudo(Pseudo::Lomem))
            }
            THIMEM => {
                self.i += 1;
                Ok(Expr::Pseudo(Pseudo::Himem))
            }
            TEND => {
                self.i += 1;
                Ok(Expr::Pseudo(Pseudo::End))
            }
            TTO if self.peek_at(1) == b'P' => {
                // TOP is TO then P (§2.4)
                self.i += 2;
                Ok(Expr::Pseudo(Pseudo::Top))
            }
            TTIME => {
                self.i += 1;
                if self.peek() == b'$' {
                    self.i += 1;
                    return Ok(Expr::Func(Func::TimeD, vec![]));
                }
                Ok(Expr::Func(Func::Time, vec![]))
            }
            TESCFN if self.peek_at(1) == TSUM => {
                self.i += 2;
                if self.next_sig() == TLEN {
                    self.i += 1;
                    self.array_operand(ArrayFunc::SumLen)
                } else {
                    self.array_operand(ArrayFunc::Sum)
                }
            }
            TMOD => {
                self.i += 1;
                self.array_operand(ArrayFunc::Mod)
            }
            TESCFN if self.peek_at(1) == TBEAT => {
                self.i += 2;
                Ok(Expr::Func(Func::Beat, vec![]))
            }
            TESCFN => self.not_yet("this function"),
            TDIM if self.peek_at(1) == b'(' => {
                self.i += 2;
                self.skip_spaces();
                let needs = Fail { number: 10, message: "DIM() function needs an array" };
                let v = match self.variable_name() {
                    Some(v) if self.peek() == b'(' => v,
                    // A variable gives error 10 if it exists, and error 14
                    // if it does not (§4.5.11).
                    Some(v) => return Ok(Expr::DimVar(v)),
                    None => return fail(Fail { number: 14, message: "Unknown array in DIM() function" }),
                };
                self.i += 1;
                if self.next_sig() != b')' {
                    // An element
                    let subs = self.paren_args()?;
                    return Err(Partial { done: subs, fail: needs });
                }
                self.i += 1;
                let n = if self.next_sig() == b',' {
                    self.i += 1;
                    Some(Box::new(self.expr()?))
                } else {
                    None
                };
                if self.next_sig() != b')' {
                    return Err(Partial { done: vec![], fail: missing_paren() });
                }
                self.i += 1;
                Ok(Expr::DimOf(v, n))
            }
            // `(` must follow DIM at once (§4.5.11)
            TDIM => fail(Fail { number: 14, message: "Reference array incorrect" }),
            // The trace file's channel. It is 0, because tracing is never
            // on (§15.4.2).
            TTRACE => {
                self.i += 1;
                Ok(Expr::Int(0))
            }
            TQUIT => self.not_yet("QUIT"),
            _ => fail(Fail::missing_var()),
        }
    }

    fn not_yet(&mut self, what: &'static str) -> PResult<Expr> {
        self.unsupported = Some(what);
        fail(Fail::syntax())
    }

    /// The arguments of a keyword that ends in `(`. They are expressions
    /// separated by commas, then `)`.
    fn paren_args(&mut self) -> PResult<Vec<Expr>> {
        let mut args = Vec::new();
        loop {
            match self.expr() {
                Ok(e) => args.push(e),
                Err(mut p) => {
                    let mut d = args;
                    d.append(&mut p.done);
                    p.done = d;
                    return Err(p);
                }
            }
            match self.next_sig() {
                b',' => self.i += 1,
                b')' => {
                    self.i += 1;
                    return Ok(args);
                }
                _ => return Err(Partial { done: args, fail: missing_paren() }),
            }
        }
    }

    // ---- Literals (§2.5.4-2.5.9) ------------------------------------------

    fn decimal(&mut self) -> Expr {
        let s = self.i;
        let mut real = false;
        while self.peek().is_ascii_digit() {
            self.i += 1;
        }
        if self.peek() == b'.' {
            real = true;
            self.i += 1;
            while self.peek().is_ascii_digit() {
                self.i += 1;
            }
        }
        if self.peek() == b'E' {
            real = true;
            self.i += 1;
            if self.peek() == b'+' || self.peek() == b'-' {
                self.i += 1;
            }
            let mut k = 0;
            while k < 3 && self.peek().is_ascii_digit() {
                self.i += 1;
                k += 1;
            }
        }
        let mut text = self.t[s..self.i].to_vec();
        // A second point ends the number and changes its value (§2.5.4).
        // The point is passed to the runtime's reader, and the next number
        // starts there.
        if real && self.peek() == b'.' {
            text.push(b'.');
        }
        if !real {
            // An integer below 213909504 (§2.5.5)
            let mut v: u64 = 0;
            for &d in &text {
                v = v * 10 + u64::from(d - b'0');
                if v >= 213909504 {
                    return Expr::Real(text);
                }
            }
            return Expr::Int(v as i32);
        }
        Expr::Real(text)
    }

    fn hex(&mut self) -> PResult<Expr> {
        self.i += 1;
        let mut v: u32 = 0;
        let mut digits = 0;
        let mut significant = 0;
        while self.peek().is_ascii_hexdigit() {
            let d = (self.peek() as char).to_digit(16).unwrap();
            self.i += 1;
            digits += 1;
            if significant > 0 || d != 0 {
                significant += 1;
            }
            v = v.wrapping_shl(4) | d;
        }
        if digits == 0 {
            return fail(Fail { number: 28, message: "Bad Hex" });
        }
        if significant > 8 {
            return fail(Fail { number: 28, message: "Hex number too large" });
        }
        Ok(Expr::Int(v as i32))
    }

    fn binary(&mut self) -> PResult<Expr> {
        self.i += 1;
        let mut v: u32 = 0;
        let mut digits = 0;
        while self.peek() == b'0' || self.peek() == b'1' {
            v = v.wrapping_shl(1) | u32::from(self.peek() - b'0');
            self.i += 1;
            digits += 1;
        }
        if digits == 0 {
            return fail(Fail { number: 28, message: "Bad Binary" });
        }
        Ok(Expr::Int(v as i32))
    }

    fn string_literal(&mut self) -> PResult<Expr> {
        self.i += 1;
        let mut s = Vec::new();
        loop {
            if self.at_end() {
                return fail(Fail { number: 9, message: "Missing \"\"" });
            }
            let c = self.peek();
            self.i += 1;
            if c == b'"' {
                if self.peek() == b'"' {
                    self.i += 1;
                    s.push(b'"');
                    continue;
                }
                return Ok(Expr::Str(s));
            }
            s.push(c);
        }
    }
}

fn missing_paren() -> Fail {
    Fail { number: 27, message: "Missing )" }
}

fn print_fail(items: Vec<PrintItem>, p: Partial) -> Stmt {
    print_fail_with(items, Vec::new(), p)
}

/// A PRINT that fails part way (§9.8.3). The items before the failing one
/// run first. Then the failing item's completed operands are evaluated,
/// and then the error is raised.
fn print_fail_with(mut items: Vec<PrintItem>, done: Vec<Expr>, p: Partial) -> Stmt {
    let mut d = done;
    d.extend(p.done);
    items.push(PrintItem::Value(Expr::Fail(d, p.fail)));
    Stmt::Print(items)
}

fn unsupported_name(c: u8, next: u8) -> &'static str {
    match c {
        TDIM => "DIM",
        TREAD => "READ",
        TRESTORE => "RESTORE",
        TINPUT => "INPUT",
        TVDU => "VDU",
        TOSCL => "OSCLI",
        TCALL => "CALL",
        TRUN => "RUN",
        TCLEAR => "CLEAR",
        TTRACE => "TRACE",
        TERROR => "ERROR",
        TCLOSE => "CLOSE#",
        TBPUT => "BPUT#",
        TMODE | TCLS | TCLG | TGRAPH | TTEXT | TMOVE | TPLOT | TDRAW => "graphics",
        TESCSTMT => match next {
            TSYS => "SYS",
            TSWAP => "SWAP",
            TLIBRARY => "LIBRARY",
            t => crate::program::two_byte(TESCSTMT, t).unwrap_or("this statement"),
        },
        TESCCOM | TESCFN => crate::program::two_byte(c, next).unwrap_or("this statement"),
        b'?' | b'!' | b'$' | b'|' => "indirection",
        c if c >= 0x7F => crate::program::one_byte(c).unwrap_or("this statement"),
        _ => "this statement",
    }
}

/// A statement of one call.
fn os1(f: &'static str, exprs: Vec<(Expr, OsKind)>, refs: Vec<OsRef>) -> Stmt {
    Stmt::Os { exprs, calls: vec![(f, refs)] }
}

/// Where an assembler block that is open at `t[from]` ends. The result is
/// the offset just after its `]`, or `None` if the line ends inside the
/// block (§16.1.2).
///
/// The statements follow the assembler's rules. A statement ends at `:` or
/// at the end of the line, outside strings. `;`, `\` and `REM` begin a
/// comment, which runs to the next `:`. A statement may begin with a
/// label, `.`name. A `]` that begins a statement, after any label, ends
/// the block. The runtime (rb_asm.c) reads the block by the same rules.
pub fn asm_end(t: &[u8], from: usize) -> Option<usize> {
    let at = |i: usize| *t.get(i).unwrap_or(&13);
    let mut i = from;
    loop {
        while at(i) == b' ' {
            i += 1;
        }
        if i >= t.len() {
            return None;
        }
        if at(i) == b'.' {
            i += 1;
            while is_name_char(at(i)) {
                i += 1;
            }
            if at(i) == b'%' || at(i) == b'$' {
                i += 1;
            }
            if at(i) == b'(' {
                let mut depth = 0;
                let mut quote = false;
                while i < t.len() {
                    let c = at(i);
                    i += 1;
                    if quote {
                        quote = c != b'"';
                    } else if c == b'"' {
                        quote = true;
                    } else if c == b'(' {
                        depth += 1;
                    } else if c == b')' {
                        depth -= 1;
                        if depth == 0 {
                            break;
                        }
                    }
                }
            }
            while at(i) == b' ' {
                i += 1;
            }
        }
        if at(i) == b']' && i < t.len() {
            return Some(i + 1);
        }
        // The statement's body, then any comment.
        let mut quote = false;
        while i < t.len() {
            let c = t[i];
            if quote {
                quote = c != b'"';
                i += 1;
                continue;
            }
            match c {
                b'"' => quote = true,
                b':' | b';' | b'\\' | TREM => break,
                TESCFN | TESCCOM | TESCSTMT => i += 1,
                TCONST => i += 3,
                _ => {}
            }
            i += 1;
        }
        while i < t.len() && t[i] != b':' {
            i += 1;
        }
        if i >= t.len() {
            return None;
        }
        i += 1;
    }
}
