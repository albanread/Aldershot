//! Where control goes. This module works out the interpreter's scans at
//! compile time, and the control context of every statement (§7.1, §7.11).
//!
//! The interpreter finds `ELSE`, `ENDIF`, `ENDWHILE`, `WHEN` and `ENDCASE`
//! by scanning the program text as it runs (§7.1.2). The result of a scan
//! depends only on the text and on where the scan starts, so the compiler
//! makes each scan once. Loops are matched through the *control context*.
//! This is the kind of frame that a statement runs in, together with the
//! loops opened in that frame (§7.11.2). A statement that could run in two
//! contexts is rejected (§7.11.3). As a result, every `NEXT`, `UNTIL`,
//! `ENDWHILE` and `RETURN` knows its loop at compile time.

use std::collections::{BTreeMap, HashMap, VecDeque};

use crate::ast::*;
use crate::parse::{asm_end, Def, LineParser, Parsed};
use crate::program::Program;
use crate::tokens::*;

pub type NodeId = usize;

/// A statement, with the line it stands on.
#[derive(Debug, Clone)]
pub struct Node {
    pub line: usize,
    pub p: Parsed,
}

/// Where control goes when a scan or a jump ends.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
pub enum Dest {
    Node(NodeId),
    /// The end of the program. Running off the end acts as `END` (§7.6.4).
    End,
    /// An error, raised when control would go to this destination.
    Error(i32, &'static str),
}

/// A loop open in a frame (§7.1.1).
#[derive(Debug, Clone, PartialEq, Eq, Hash)]
pub enum Loop {
    For(NodeId),
    Repeat(NodeId),
    While(NodeId),
    /// A `LOCAL ERROR` entry (§9.5.4). Closers discard it (§7.1.1).
    LocalError(NodeId),
    /// The entry made by `LOCAL DATA` (§11.4.1).
    LocalData(NodeId),
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
pub enum FrameKind {
    Main,
    Proc,
    Fn,
    /// A GOSUB subroutine. Which GOSUB entered it is not known at compile
    /// time, because a subroutine may have many callers. Only the fact that
    /// a GOSUB entered it is known.
    Gosub,
}

/// The control context of a statement (§7.11.2).
#[derive(Debug, Clone, PartialEq, Eq, Hash)]
pub struct Ctx {
    pub frame: FrameKind,
    pub loops: Vec<Loop>,
}

/// A routine or the main program: what becomes one function.
#[derive(Debug, Clone)]
pub struct Unit {
    /// `None` for the main program.
    pub def: Option<(usize, Def)>,
    pub entry: Dest,
    /// Every statement the unit can run, with its context.
    pub ctx: BTreeMap<NodeId, Ctx>,
    /// The first statements of the handlers that run in this unit. For the
    /// main program, these are the handlers of every `ON ERROR`, wherever
    /// it stands (§9.4.5), and of its own `ON ERROR LOCAL`. For a routine,
    /// they are the handlers of its own `ON ERROR LOCAL`.
    pub handlers: Vec<NodeId>,
}

/// A compile-time rejection or a statement the compiler does not handle.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Diagnostic {
    pub line: u16,
    pub message: String,
}

pub struct Flow {
    pub lines: Vec<(u16, Vec<u8>)>,
    pub nodes: Vec<Node>,
    /// The first node of each line, if it has any.
    line_first: Vec<Option<NodeId>>,
    pub defs: Vec<(usize, Def)>,
    pub units: Vec<Unit>,
    pub errors: Vec<Diagnostic>,
    /// Statements that will raise an error when they run (§9.8.3).
    pub warnings: Vec<Diagnostic>,
    /// The segment of each line. Segment 0 is the program, and the
    /// libraries follow it (§15.3.1). `seg_end` holds the end of the
    /// segment that each line is in.
    pub seg_of: Vec<usize>,
    seg_end: Vec<usize>,
}

impl Flow {
    pub fn new(program: &Program) -> Flow {
        Flow::with_segments(program, &[0])
    }

    /// Analyses the program's lines followed by the lines of each library
    /// (§15.3.1). `seg_starts` holds the first line of each segment, with
    /// the main program's first. Control never falls from one segment into
    /// the next, and no scan runs from one segment into the next.
    pub fn with_segments(program: &Program, seg_starts: &[usize]) -> Flow {
        let mut nodes: Vec<Node> = Vec::new();
        let mut line_first = Vec::new();
        let mut defs = Vec::new();
        let mut lines = Vec::new();
        // An assembler block whose `]` is on a later line (§16.1.2). This
        // holds its node and the line it starts on. The lines up to the `]`
        // belong to the assembler and have no statements. A block that is
        // still open at the end of its segment runs to that end.
        let mut open: Option<(NodeId, usize)> = None;
        for (li, line) in program.lines.iter().enumerate() {
            lines.push((line.number, line.text.clone()));
            if let Some((id, l0)) = open {
                if li > 0 && seg_starts.contains(&li) {
                    close_block(&mut nodes[id].p.stmt, program, l0, li - 1, false);
                    open = None;
                }
            }
            let mut p = LineParser::new(&line.text);
            let parsed = match open {
                Some((id, l0)) => match asm_end(&line.text, 0) {
                    None => {
                        line_first.push(None);
                        continue;
                    }
                    Some(k) => {
                        close_block(&mut nodes[id].p.stmt, program, l0, li, true);
                        open = None;
                        p.parse_from(k)
                    }
                },
                None => p.parse_line(),
            };
            if let Some(d) = p.def.take() {
                defs.push((li, d));
            }
            line_first.push(if parsed.is_empty() { None } else { Some(nodes.len()) });
            for mut s in parsed {
                if let Stmt::Asm(b) = &s.stmt {
                    if b.ends {
                        close_block(&mut s.stmt, program, li, li, true);
                    } else {
                        open = Some((nodes.len(), li));
                    }
                }
                nodes.push(Node { line: li, p: s });
            }
        }
        if let Some((id, l0)) = open {
            close_block(&mut nodes[id].p.stmt, program, l0, program.lines.len() - 1, false);
        }
        let mut seg_of = vec![0; lines.len()];
        for (k, &s) in seg_starts.iter().enumerate() {
            for x in seg_of.iter_mut().skip(s) {
                *x = k;
            }
        }
        let seg_end = (0..lines.len())
            .map(|li| seg_starts.iter().copied().find(|&s| s > li).unwrap_or(lines.len()))
            .collect();
        let mut f = Flow {
            lines,
            nodes,
            line_first,
            defs,
            units: Vec::new(),
            errors: Vec::new(),
            warnings: Vec::new(),
            seg_of,
            seg_end,
        };
        f.analyse();
        f
    }

    pub fn line_number(&self, id: NodeId) -> u16 {
        self.lines[self.nodes[id].line].0
    }

    fn text(&self, li: usize) -> &[u8] {
        &self.lines[li].1
    }

    /// The first statement on line `li`, or on a later line of the segment
    /// that the line before `li` is in.
    pub fn first_from_line(&self, li: usize) -> Dest {
        let end = if li == 0 { self.seg_end.first().copied().unwrap_or(0) } else { self.seg_end[li - 1] };
        for l in li..end {
            if let Some(n) = self.line_first[l] {
                return Dest::Node(n);
            }
        }
        Dest::End
    }

    /// The first statement on line `li` that starts at offset `off` or
    /// later. If there is none, it is the first statement of the next line.
    pub fn at_offset(&self, li: usize, off: usize) -> Dest {
        if let Some(first) = self.line_first[li] {
            let mut id = first;
            while id < self.nodes.len() && self.nodes[id].line == li {
                if self.nodes[id].p.start >= off {
                    return Dest::Node(id);
                }
                id += 1;
            }
        }
        self.first_from_line(li + 1)
    }

    /// The statement after `id`, in text order. This order runs across DEF
    /// lines (§7.1.2).
    pub fn next(&self, id: NodeId) -> Dest {
        let n = &self.nodes[id];
        if id + 1 < self.nodes.len() && self.nodes[id + 1].line == n.line {
            return Dest::Node(id + 1);
        }
        self.first_from_line(n.line + 1)
    }

    /// The line numbered `n` (§7.8.2). This is the first line whose number
    /// is at least `n`. It is an error unless that number is `n`.
    pub fn line_dest(&self, n: u16) -> Dest {
        // Look only in the main program, because a library has no line
        // numbers of its own (§7.11.5).
        let main = self.seg_end.first().copied().unwrap_or(0);
        match self.lines[..main].iter().position(|l| l.0 >= n) {
            Some(li) if self.lines[li].0 == n => self.first_from_line(li),
            _ => Dest::Error(41, "No such line"),
        }
    }

    // ---- Scans (§7.2.3, §7.3, §7.6.3, §7.7) --------------------------------

    /// Where a false single-line IF goes. This is after the first ELSE byte
    /// on its line (§7.2.3). Also returns whether that byte is an ELSE that
    /// the parser saw. If it is not, it is a byte inside a string or a
    /// comment (§7.11.6).
    pub fn else_target(&self, id: NodeId) -> (Dest, bool) {
        let (li, from) = (self.nodes[id].line, self.nodes[id].p.end);
        let t = self.text(li);
        match t[from..].iter().position(|&b| b == TELSE) {
            None => (self.first_from_line(li + 1), true),
            Some(k) => {
                let off = from + k;
                let real = self.nodes.iter().any(|n| n.line == li && n.p.start == off && n.p.stmt == Stmt::ElseSkip);
                (self.at_offset(li, off + 1), real)
            }
        }
    }

    fn else_scan(&mut self, id: NodeId) -> Dest {
        let (d, real) = self.else_target(id);
        if !real {
            let line = self.line_number(id);
            self.reject(line, "Ambiguous text in a scan");
        }
        d
    }

    fn first_item(&self, li: usize) -> Option<(usize, u8)> {
        let t = self.text(li);
        t.iter().position(|&b| b != b' ').map(|k| (k, t[k]))
    }

    fn last_byte(&self, li: usize) -> Option<u8> {
        self.text(li).last().copied()
    }

    /// Where a false block IF goes (§7.3.2).
    pub fn block_if_scan(&self, id: NodeId) -> Dest {
        let mut count = 1i32;
        for li in self.nodes[id].line + 1..self.seg_end[self.nodes[id].line] {
            match self.first_item(li) {
                Some((k, TENDIF)) => {
                    count -= 1;
                    if count == 0 {
                        return self.at_offset(li, k + 1);
                    }
                }
                Some((k, TELSE2)) if count == 1 => return self.at_offset(li, k + 1),
                _ => {}
            }
            if self.last_byte(li) == Some(TTHEN) {
                count += 1;
            }
        }
        Dest::Error(49, "Missing ENDIF")
    }

    /// Where a block ELSE goes when it runs (§7.3.3).
    pub fn block_else_scan(&self, id: NodeId) -> Dest {
        let li0 = self.nodes[id].line;
        let mut count = 0i32;
        if self.last_byte(li0) == Some(TTHEN) {
            count += 1;
        }
        for li in li0 + 1..self.seg_end[li0] {
            if let Some((k, TENDIF)) = self.first_item(li) {
                count -= 1;
                if count < 0 {
                    return self.at_offset(li, k + 1);
                }
            }
            if self.last_byte(li) == Some(TTHEN) {
                count += 1;
            }
        }
        Dest::Error(49, "Missing ENDIF")
    }

    /// Where a false WHILE goes (§7.6.3). The scan goes byte by byte from
    /// the end of its condition.
    pub fn while_scan(&self, id: NodeId) -> Dest {
        let mut count = 0i32;
        let li0 = self.nodes[id].line;
        let mut from = self.nodes[id].p.end;
        for li in li0..self.seg_end[li0] {
            let t = self.text(li);
            let mut i = if li == li0 { from } else { 0 };
            let mut quoted = false;
            while i < t.len() {
                let b = t[i];
                if quoted {
                    if b == b'"' {
                        quoted = false;
                    }
                    i += 1;
                    continue;
                }
                match b {
                    b'"' => quoted = true,
                    TREM | TDATA => break,
                    TCONST => i += 3,
                    TESCSTMT if t.get(i + 1) == Some(&TWHILE) => {
                        count += 1;
                        i += 1;
                    }
                    TESCFN | TESCCOM | TESCSTMT => i += 1,
                    TENDWH => {
                        count -= 1;
                        if count < 0 {
                            return self.at_offset(li, i + 1);
                        }
                    }
                    _ => {}
                }
                i += 1;
            }
            from = 0;
        }
        let _ = from;
        Dest::End
    }

    /// The clauses that a CASE finds, in order (§7.7.2). Each WHEN line is
    /// given by its node. The scan ends at an OTHERWISE or ENDCASE, and the
    /// destination after it is returned too.
    pub fn case_scan(&self, id: NodeId) -> (Vec<NodeId>, Dest) {
        let mut whens = Vec::new();
        let mut count = 0i32;
        for li in self.nodes[id].line + 1..self.seg_end[self.nodes[id].line] {
            let first = self.first_item(li);
            if count == 0 {
                match first {
                    Some((k, TWHEN)) => {
                        if let Dest::Node(n) = self.at_offset(li, k) {
                            whens.push(n);
                        }
                    }
                    Some((k, TOTHER)) => return (whens, self.at_offset(li, k + 1)),
                    Some((k, TENDCA)) => return (whens, self.at_offset(li, k + 1)),
                    _ => {}
                }
            } else if let Some((_, TENDCA)) = first {
                count -= 1;
            }
            if self.last_byte(li) == Some(TOF) {
                count += 1;
            }
        }
        (whens, Dest::Error(47, "Missing ENDCASE"))
    }

    /// Where a WHEN or OTHERWISE goes when it runs. This is after its
    /// ENDCASE (§7.7.6).
    pub fn clause_end_scan(&self, id: NodeId) -> Dest {
        let li0 = self.nodes[id].line;
        let mut count = 1i32;
        if self.last_byte(li0) == Some(TOF) {
            count += 1;
        }
        for li in li0 + 1..self.seg_end[li0] {
            if let Some((k, TENDCA)) = self.first_item(li) {
                count -= 1;
                if count == 0 {
                    return self.at_offset(li, k + 1);
                }
            }
            if self.last_byte(li) == Some(TOF) {
                count += 1;
            }
        }
        Dest::Error(47, "Missing ENDCASE")
    }

    /// Where `ON` goes when the index is out of range and there is an
    /// `ELSE`. This is the statements after the `ELSE` (§7.8.8).
    pub fn on_else(&self, id: NodeId) -> Dest {
        let (li, from) = (self.nodes[id].line, self.nodes[id].p.end);
        match self.text(li)[from..].iter().position(|&b| b == TELSE) {
            Some(k) => self.at_offset(li, from + k + 1),
            None => Dest::Error(40, "ON range"),
        }
    }

    /// Where `RETURN` comes back to after `ON` ... `GOSUB`. This is after
    /// the first colon that follows the list, or else the next line
    /// (§7.8.9).
    pub fn on_return(&self, id: NodeId) -> Dest {
        let (li, from) = (self.nodes[id].line, self.nodes[id].p.end);
        match self.text(li)[from..].iter().position(|&b| b == b':') {
            Some(k) => self.at_offset(li, from + k + 1),
            None => self.first_from_line(li + 1),
        }
    }

    /// Where `ON` ... `PROC` goes on after the chosen call. The call ends at
    /// offset `from` in line `li`. Control goes on after the first colon,
    /// or else at the next line (§7.8.10).
    pub fn after_colon(&self, li: usize, from: usize) -> Dest {
        match self.text(li)[from..].iter().position(|&b| b == b':') {
            Some(k) => self.at_offset(li, from + k + 1),
            None => self.first_from_line(li + 1),
        }
    }

    /// Where a WHEN goes on when it matches. This is after its list of
    /// values.
    pub fn after_when(&self, id: NodeId) -> Dest {
        let n = &self.nodes[id];
        self.at_offset(n.line, n.p.end)
    }

    fn reject(&mut self, line: u16, message: &str) {
        let d = Diagnostic { line, message: message.to_string() };
        if !self.errors.contains(&d) {
            self.errors.push(d);
        }
    }

    // ---- Units and contexts (§7.11, §8.12) -----------------------------

    fn analyse(&mut self) {
        let mut units = vec![Unit { def: None, entry: self.first_from_line(0), ctx: BTreeMap::new(), handlers: Vec::new() }];
        for (li, d) in self.defs.clone() {
            let entry = self.at_offset(li, d.body);
            units.push(Unit { def: Some((li, d)), entry, ctx: BTreeMap::new(), handlers: Vec::new() });
        }
        // The handlers of ON ERROR run in the main program, outside every
        // routine and loop (§9.4.4 to §9.4.5).
        let mut global = Vec::new();
        for id in 0..self.nodes.len() {
            if let Stmt::OnError { local: false, off: false } = self.nodes[id].p.stmt {
                if let Dest::Node(h) = self.next(id) {
                    if !global.contains(&h) {
                        global.push(h);
                    }
                }
            }
        }
        // Only the routines that the program calls are run (§8.12.1). Start
        // with the main program, then take every routine that a walked
        // statement names.
        let mut called = vec![false; units.len()];
        called[0] = true;
        let mut todo = vec![0usize];
        let mut walked: Vec<Option<BTreeMap<NodeId, Ctx>>> = vec![None; units.len()];
        while let Some(k) = todo.pop() {
            let u = &units[k];
            let frame = match &u.def {
                None => FrameKind::Main,
                Some((_, d)) if d.is_fn => FrameKind::Fn,
                Some(_) => FrameKind::Proc,
            };
            let mut starts = vec![(u.entry, Ctx { frame, loops: Vec::new() })];
            if k == 0 {
                for &h in &global {
                    starts.push((Dest::Node(h), Ctx { frame: FrameKind::Main, loops: Vec::new() }));
                }
            }
            let ctx = self.walk(starts);
            for &id in ctx.keys() {
                let mut names = Vec::new();
                calls_in(&self.nodes[id].p.stmt, &mut names);
                for (is_fn, name) in names {
                    for (j, v) in units.iter().enumerate() {
                        if let Some((_, d)) = &v.def {
                            if d.is_fn == is_fn && (d.name == name || (is_fn && name.is_empty())) && !called[j] {
                                called[j] = true;
                                todo.push(j);
                            }
                        }
                    }
                }
            }
            walked[k] = Some(ctx);
        }
        for (k, u) in units.iter_mut().enumerate() {
            let ctx = walked[k].take().unwrap_or_default();
            if k == 0 {
                u.handlers = global.clone();
            }
            // Add the unit's own local handlers.
            for (&id, _) in &ctx {
                if let Stmt::OnError { local: true, off: false } = self.nodes[id].p.stmt {
                    if let Dest::Node(h) = self.next(id) {
                        if !u.handlers.contains(&h) {
                            u.handlers.push(h);
                        }
                    }
                }
            }
            u.ctx = ctx;
        }
        // Reject a statement that is in both the main program and a
        // routine (§8.12.1).
        let mut owner: HashMap<NodeId, FrameKind> = HashMap::new();
        for u in &units {
            for (&id, c) in &u.ctx {
                if c.frame == FrameKind::Gosub {
                    continue;
                }
                if let Some(&k) = owner.get(&id) {
                    if k != c.frame || k == FrameKind::Main {
                        let line = self.line_number(id);
                        if !exit_statement(&self.nodes[id].p.stmt) {
                            self.reject(line, "Control structure depends on the path");
                        }
                    }
                }
                owner.insert(id, c.frame);
            }
        }
        // Reject routines that the compiler cannot compile yet, and formal
        // parameters that are named twice (§8.12.3).
        for (li, d) in self.defs.clone() {
            let line = self.lines[li].0;
            if let Some(what) = d.unsupported {
                self.reject(line, &format!("not compiled yet: {what}"));
            }
            if let Some(what) = d.rejected {
                self.reject(line, what);
            }
            for (i, p) in d.params.iter().enumerate() {
                if d.params[..i].iter().any(|q| q.var == p.var && (q.kind == ParamKind::Array) == (p.kind == ParamKind::Array)) {
                    self.reject(line, "Formal parameter repeated");
                }
            }
        }
        // Reject an ON ... GOSUB whose return scan stops at a colon inside a
        // string (§7.11.6).
        for n in 0..self.nodes.len() {
            if let Stmt::OnProc { ambiguous: true, .. } = self.nodes[n].p.stmt {
                let line = self.lines[self.nodes[n].line].0;
                self.reject(line, "Ambiguous text in a scan");
            }
        }
        for n in 0..self.nodes.len() {
            if let Stmt::On { gosub: true, .. } = self.nodes[n].p.stmt {
                let (li, from) = (self.nodes[n].line, self.nodes[n].p.end);
                let t = self.text(li);
                if let Some(k) = t[from..].iter().position(|&b| b == b':') {
                    let quotes = t[..from + k].iter().filter(|&&b| b == b'"').count();
                    if quotes % 2 == 1 {
                        let line = self.lines[li].0;
                        self.reject(line, "Ambiguous text in a scan");
                    }
                }
            }
        }
        // A statement that is not well formed is compiled to raise its error
        // when it runs, and a warning is given (§9.8.3).
        for n in 0..self.nodes.len() {
            if let Stmt::Fail(_, f) = &self.nodes[n].p.stmt {
                let d = Diagnostic {
                    line: self.line_number(n),
                    message: format!("warning: raises error {}, \"{}\", when it runs", f.number, f.message),
                };
                // Warn only for the first on a line, because the statements
                // after it never run.
                if !self.warnings.iter().any(|w| w.line == d.line) {
                    self.warnings.push(d);
                }
            }
        }
        // A library may not use line numbers (§7.11.5).
        for n in 0..self.nodes.len() {
            if self.seg_of[self.nodes[n].line] == 0 {
                continue;
            }
            let uses = matches!(
                self.nodes[n].p.stmt,
                Stmt::Goto(_) | Stmt::Gosub(_) | Stmt::GotoBad(_) | Stmt::On { .. } | Stmt::Restore(RestoreTo::Line(_)) | Stmt::Restore(RestoreTo::Rel(_))
            );
            if uses {
                let line = self.line_number(n);
                self.reject(line, "Line number in a library");
            }
        }
        // The rejections of chapter 15, wherever they stand.
        for n in 0..self.nodes.len() {
            if let Stmt::Rejected(what) = self.nodes[n].p.stmt {
                let line = self.line_number(n);
                self.reject(line, what);
            }
        }
        for n in 0..self.nodes.len() {
            if let Stmt::Unsupported(what) = self.nodes[n].p.stmt {
                let reached = units.iter().any(|u| u.ctx.contains_key(&n));
                if reached {
                    let line = self.line_number(n);
                    self.reject(line, &format!("not compiled yet: {what}"));
                }
            }
        }
        self.units = units;
    }

    /// Every statement that can be reached from the destinations in
    /// `starts`, each in the context given with it. Each statement is
    /// returned with its context. A statement reached in two different
    /// contexts is rejected.
    fn walk(&mut self, starts: Vec<(Dest, Ctx)>) -> BTreeMap<NodeId, Ctx> {
        let mut seen: BTreeMap<NodeId, Ctx> = BTreeMap::new();
        let mut work: VecDeque<(Dest, Ctx)> = starts.into_iter().collect();
        while let Some((d, ctx)) = work.pop_front() {
            let id = match d {
                Dest::Node(id) => id,
                _ => continue,
            };
            if let Some(old) = seen.get(&id) {
                if *old != ctx && !exit_statement(&self.nodes[id].p.stmt) && !self.closes_same(id, old, &ctx) {
                    let line = self.line_number(id);
                    self.reject(line, "Control structure depends on the path");
                }
                continue;
            }
            seen.insert(id, ctx.clone());
            for (d, c) in self.successors(id, &ctx) {
                work.push_back((d, c));
            }
        }
        seen
    }

    /// Whether a closer reached in two contexts closes the same loop in
    /// both, with the same entries below it (§7.11.3). If so, what follows
    /// it is the same in both contexts.
    fn closes_same(&self, id: NodeId, a: &Ctx, b: &Ctx) -> bool {
        if a.frame != b.frame {
            return false;
        }
        let depth = |c: &Ctx| -> Option<usize> {
            match &self.nodes[id].p.stmt {
                Stmt::Next(vars) => self.match_next(id, c, vars).ok().and_then(|s| s.first().map(|&(_, d)| d)),
                Stmt::Until(_) => find_loop(c, |l| matches!(l, Loop::Repeat(_))),
                Stmt::EndWhile => find_loop(c, |l| matches!(l, Loop::While(_))),
                _ => None,
            }
        };
        match (depth(a), depth(b)) {
            (Some(x), Some(y)) => x == y && a.loops[..=x] == b.loops[..=y],
            _ => false,
        }
    }

    /// Where control can go from `id`, and in what context.
    pub fn successors(&mut self, id: NodeId, ctx: &Ctx) -> Vec<(Dest, Ctx)> {
        let next = self.next(id);
        let same = |d: Dest| (d, ctx.clone());
        let stmt = self.nodes[id].p.stmt.clone();
        match stmt {
            // A condition that is a literal decides the branch. A path that
            // it rules out is not a path, because §7.11.2 follows only where
            // control can go.
            Stmt::If(ref c) => {
                let f = self.else_scan(id);
                match constant(c) {
                    Some(true) => vec![same(next)],
                    Some(false) => vec![same(f)],
                    None => vec![same(next), same(f)],
                }
            }
            Stmt::BlockIf(ref c) => {
                let f = self.block_if_scan(id);
                match constant(c) {
                    Some(true) => vec![same(next)],
                    Some(false) => vec![same(f)],
                    None => vec![same(next), same(f)],
                }
            }
            Stmt::ElseSkip => vec![same(self.first_from_line(self.nodes[id].line + 1))],
            Stmt::BlockElse => vec![same(self.block_else_scan(id))],
            Stmt::Goto(n) => vec![same(self.line_dest(n))],
            Stmt::For { .. } => {
                let mut c = ctx.clone();
                c.loops.push(Loop::For(id));
                vec![(next, c)]
            }
            Stmt::Repeat => {
                let mut c = ctx.clone();
                c.loops.push(Loop::Repeat(id));
                vec![(next, c)]
            }
            Stmt::While(ref cond) => {
                let mut c = ctx.clone();
                c.loops.push(Loop::While(id));
                match constant(cond) {
                    Some(false) => vec![same(self.while_scan(id))],
                    _ => vec![(next, c), same(self.while_scan(id))],
                }
            }
            Stmt::Next(ref vars) => match self.match_next(id, ctx, vars) {
                Ok(steps) => {
                    let mut out = Vec::new();
                    for (for_id, above) in steps {
                        // Looping goes back to after the FOR, with the loops
                        // up to and including it.
                        let mut c = ctx.clone();
                        c.loops.truncate(above + 1);
                        out.push((self.next(for_id), c));
                    }
                    // Ending goes on after the NEXT, with the matched loops
                    // removed.
                    let mut c = ctx.clone();
                    if let Ok(last) = self.match_next(id, ctx, vars) {
                        if let Some(&(_, depth)) = last.last() {
                            c.loops.truncate(depth);
                        }
                    }
                    out.push((next, c));
                    out
                }
                Err(_) => vec![],
            },
            Stmt::Until(_) => match find_loop(ctx, |l| matches!(l, Loop::Repeat(_))) {
                Some(k) => {
                    let rep = match ctx.loops[k] {
                        Loop::Repeat(r) => r,
                        _ => unreachable!(),
                    };
                    let mut back = ctx.clone();
                    back.loops.truncate(k + 1);
                    let mut out = ctx.clone();
                    out.loops.truncate(k);
                    vec![(self.next(rep), back), (next, out)]
                }
                None => vec![],
            },
            Stmt::EndWhile => match find_loop(ctx, |l| matches!(l, Loop::While(_))) {
                Some(k) => {
                    let w = match ctx.loops[k] {
                        Loop::While(w) => w,
                        _ => unreachable!(),
                    };
                    let mut back = ctx.clone();
                    back.loops.truncate(k + 1);
                    let mut out = ctx.clone();
                    out.loops.truncate(k);
                    vec![(self.next(w), back), (next, out)]
                }
                None => vec![],
            },
            Stmt::Case(_) => {
                let (whens, end) = self.case_scan(id);
                let mut v: Vec<(Dest, Ctx)> = whens.iter().map(|&w| same(self.after_when(w))).collect();
                v.push(same(end));
                v
            }
            Stmt::When(_) | Stmt::Otherwise => vec![same(self.clause_end_scan(id))],
            Stmt::Gosub(n) => {
                let target = self.line_dest(n);
                vec![(target, Ctx { frame: FrameKind::Gosub, loops: Vec::new() }), same(next)]
            }
            Stmt::On { gosub, ref lines, has_else, .. } => {
                let mut v = Vec::new();
                for &n in lines {
                    let d = self.line_dest(n);
                    if gosub {
                        v.push((d, Ctx { frame: FrameKind::Gosub, loops: Vec::new() }));
                    } else {
                        v.push(same(d));
                    }
                }
                if has_else {
                    v.push(same(self.on_else(id)));
                }
                if gosub {
                    v.push(same(self.on_return(id)));
                }
                v
            }
            Stmt::OnProc { ref items, has_else, .. } => {
                let li = self.nodes[id].line;
                let mut v: Vec<(Dest, Ctx)> = items.iter().map(|it| same(self.after_colon(li, it.2))).collect();
                if has_else {
                    v.push(same(self.on_else(id)));
                }
                v
            }
            Stmt::Return | Stmt::EndProc | Stmt::FnReturn(_) | Stmt::End | Stmt::Stop | Stmt::Quit(_) | Stmt::Run => vec![],
            Stmt::GotoBad(_) => vec![],
            Stmt::OnError { local, off } => {
                // Execution goes on with the next line. The statements of a
                // local handler run in the same context when an error
                // occurs. ON ERROR OFF goes on with the next statement
                // (s/Stmt ONERRF).
                let after = if off && !local { next } else { self.first_from_line(self.nodes[id].line + 1) };
                let mut v = vec![same(after)];
                if local && !off {
                    v.push(same(next));
                }
                v
            }
            Stmt::LocalError => {
                let mut c = ctx.clone();
                c.loops.push(Loop::LocalError(id));
                vec![(next, c)]
            }
            Stmt::RestoreError => match ctx.loops.last() {
                Some(Loop::LocalError(_)) => {
                    let mut c = ctx.clone();
                    c.loops.pop();
                    vec![(next, c)]
                }
                _ => vec![],
            },
            Stmt::LocalData => {
                let mut c = ctx.clone();
                c.loops.push(Loop::LocalData(id));
                vec![(next, c)]
            }
            Stmt::RestoreData => match ctx.loops.last() {
                Some(Loop::LocalData(_)) => {
                    let mut c = ctx.clone();
                    c.loops.pop();
                    vec![(next, c)]
                }
                _ => vec![],
            },
            Stmt::Error { .. } => vec![],
            Stmt::Local(_) if !((ctx.frame == FrameKind::Proc || ctx.frame == FrameKind::Fn) && ctx.loops.is_empty()) => vec![],
            Stmt::Fail(..) | Stmt::Unsupported(_) | Stmt::Rejected(_) => vec![],
            Stmt::Def => vec![same(self.first_from_line(self.nodes[id].line + 1))],
            _ => vec![same(next)],
        }
    }

    /// Matches `NEXT` v1, v2 ... (§7.4.10). For each item, returns the FOR
    /// that it steps and the depth of that loop. An error of §7.4.11 is
    /// returned as Err.
    pub fn match_next(&self, _id: NodeId, ctx: &Ctx, vars: &[Option<Var>]) -> Result<Vec<(NodeId, usize)>, (i32, &'static str)> {
        let mut depth = ctx.loops.len();
        let mut out = Vec::new();
        for v in vars {
            let loops = &ctx.loops[..depth];
            let found = match v {
                None => loops.iter().rposition(|l| matches!(l, Loop::For(_))).ok_or((32, "Not in a FOR loop"))?,
                Some(var) => {
                    let mut k = loops.len();
                    let mut discarded = false;
                    loop {
                        if k == 0 {
                            return Err(if discarded { (33, "Can't match FOR") } else { (32, "Not in a FOR loop") });
                        }
                        k -= 1;
                        let f = match loops[k] {
                            Loop::For(f) => f,
                            Loop::LocalError(_) | Loop::LocalData(_) => continue,
                            _ => return Err(if discarded { (33, "Can't match FOR") } else { (32, "Not in a FOR loop") }),
                        };
                        if let Stmt::For { var: Target::Var(ref fv), .. } = self.nodes[f].p.stmt {
                            if fv == var {
                                break k;
                            }
                        }
                        discarded = true;
                    }
                }
            };
            let f = match ctx.loops[found] {
                Loop::For(f) => f,
                _ => unreachable!(),
            };
            out.push((f, found));
            depth = found;
        }
        Ok(out)
    }
}

/// The truth of a condition that is a literal: `TRUE`, `FALSE` or an
/// integer.
fn constant(c: &Expr) -> Option<bool> {
    match c {
        Expr::Func(Func::True, _) => Some(true),
        Expr::Func(Func::False, _) => Some(false),
        Expr::Int(k) => Some(*k != 0),
        _ => None,
    }
}

/// The routines that a statement calls, as pairs of (is FN, name).
fn calls_in(s: &Stmt, out: &mut Vec<(bool, Vec<u8>)>) {
    let mut e = |x: &Expr| exprs_calls(x, out);
    match s {
        Stmt::Proc(name, args) => {
            args.iter().for_each(&mut e);
            out.push((false, name.clone()));
        }
        Stmt::Print(items) => {
            for it in items {
                match it {
                    PrintItem::Value(x) | PrintItem::Tab(x) | PrintItem::Spc(x) => e(x),
                    PrintItem::TabXY(x, y) => {
                        e(x);
                        e(y);
                    }
                    _ => {}
                }
            }
        }
        Stmt::Assign(t, _, x, _) => {
            match t {
                Target::Elem(_, subs) => subs.iter().for_each(&mut e),
                Target::Ind(_, b, o) => {
                    e(b);
                    if let Some(o) = o {
                        e(o);
                    }
                }
                Target::Var(_) => {}
            }
            e(x);
        }
        Stmt::If(x) | Stmt::BlockIf(x) | Stmt::Until(x) | Stmt::While(x) | Stmt::Case(x) | Stmt::FnReturn(x) | Stmt::Width(x) => e(x),
        Stmt::For { var, start, limit, step } => {
            e(&var.clone().into_expr());
            e(start);
            e(limit);
            if let Some(s) = step {
                e(s);
            }
        }
        Stmt::When(v) | Stmt::Fail(v, _) => v.iter().for_each(&mut e),
        Stmt::WholeArray(_, v) => v.exprs().iter().for_each(&mut e),
        Stmt::Read(targets) => {
            for t in targets {
                e(&t.clone().into_expr());
            }
        }
        Stmt::Os { exprs, .. } => exprs.iter().for_each(|(x, _)| e(x)),
        Stmt::Vdu(items) => items.iter().for_each(|(x, _)| e(x)),
        Stmt::ModeOne(x) => e(x),
        Stmt::Envelope(v) | Stmt::PrintFile(_, v) => v.iter().for_each(&mut e),
        Stmt::Bput(c, x, _) => {
            e(c);
            e(x);
        }
        Stmt::Mouse(targets) => targets.iter().for_each(|t| e(&t.clone().into_expr())),
        Stmt::InputFile(c, targets) => {
            e(c);
            targets.iter().for_each(|t| e(&t.clone().into_expr()));
        }
        Stmt::Sys { swi, ins, outs, flags } => {
            e(swi);
            ins.iter().flatten().for_each(&mut e);
            outs.iter().flatten().chain(flags.iter()).for_each(|t| e(&t.clone().into_expr()));
        }
        Stmt::Input { items, .. } => {
            for it in items {
                match it {
                    InputItem::Prompt(PrintItem::Tab(x)) | InputItem::Prompt(PrintItem::Spc(x)) => e(x),
                    InputItem::Prompt(PrintItem::TabXY(x, y)) => {
                        e(x);
                        e(y);
                    }
                    InputItem::Var(t, _) => e(&t.clone().into_expr()),
                    _ => {}
                }
            }
        }
        Stmt::Swap(a, b) => {
            e(&a.clone().into_expr());
            e(&b.clone().into_expr());
        }
        Stmt::SubAssign { target, args, value, .. } => {
            e(&target.clone().into_expr());
            args.iter().for_each(&mut e);
            e(value);
        }
        Stmt::Restore(RestoreTo::Line(x)) | Stmt::Restore(RestoreTo::Rel(x)) => e(x),
        Stmt::Local(items) => {
            for it in items {
                if let LocalItem::Ind(_, b, o) = it {
                    e(b);
                    if let Some(o) = o {
                        e(o);
                    }
                }
            }
        }
        Stmt::On { index, .. } => e(index),
        Stmt::OnProc { index, items, .. } => {
            e(index);
            for (_, args, _) in items {
                args.iter().for_each(&mut e);
            }
            for (name, _, _) in items {
                out.push((false, name.clone()));
            }
        }
        Stmt::Quit(Some(x)) => e(x),
        Stmt::Error { number, message, .. } => {
            e(number);
            e(message);
        }
        Stmt::Dim(items) => {
            for it in items {
                match it {
                    DimItem::Array(_, b) => b.iter().for_each(&mut e),
                    DimItem::Block(_, n) | DimItem::LocalBlock(_, n) => e(n),
                }
            }
        }
        // The operands of an assembler block may call any function
        // (§16.1.4).
        Stmt::Asm(_) => out.push((true, Vec::new())),
        Stmt::Call(a, params) => {
            e(a);
            for t in params {
                match t {
                    Target::Elem(_, subs) => subs.iter().for_each(&mut e),
                    Target::Ind(_, b, o) => {
                        e(b);
                        if let Some(o) = o {
                            e(o);
                        }
                    }
                    Target::Var(_) => {}
                }
            }
        }
        _ => {}
    }
}

fn exprs_calls(x: &Expr, out: &mut Vec<(bool, Vec<u8>)>) {
    match x {
        Expr::Fn(name, args) => {
            out.push((true, name.clone()));
            args.iter().for_each(|a| exprs_calls(a, out));
        }
        // EVAL may call any function (§5.9). An empty name stands for all
        // functions.
        Expr::Func(Func::Eval, v) => {
            out.push((true, Vec::new()));
            v.iter().for_each(|a| exprs_calls(a, out));
        }
        Expr::Unary(_, a) => exprs_calls(a, out),
        Expr::Bin(_, a, b) => {
            exprs_calls(a, out);
            exprs_calls(b, out);
        }
        Expr::Func(_, v) | Expr::Elem(_, v) | Expr::Fail(v, _) => v.iter().for_each(|a| exprs_calls(a, out)),
        Expr::DimOf(_, Some(n)) => exprs_calls(n, out),
        Expr::Ind(_, b, o) => {
            exprs_calls(b, out);
            if let Some(o) = o {
                exprs_calls(o, out);
            }
        }
        _ => {}
    }
}

/// The innermost loop that `pick` accepts, looking down from the top.
pub fn find_loop(ctx: &Ctx, pick: impl Fn(&Loop) -> bool) -> Option<usize> {
    ctx.loops.iter().rposition(pick)
}

/// Statements that may run in more than one context (§7.11.3). They
/// discard the frame's entries.
fn exit_statement(s: &Stmt) -> bool {
    matches!(s, Stmt::EndProc | Stmt::FnReturn(_) | Stmt::Return | Stmt::End | Stmt::Stop | Stmt::Quit(_) | Stmt::Run)
}

/// Gives an assembler block its text (§16.1.3). The text is lines `from` to
/// `to`, each in the form a program holds it, followed by the end of the
/// program. The block's start becomes an offset in that text.
fn close_block(stmt: &mut Stmt, program: &Program, from: usize, to: usize, ends: bool) {
    if let Stmt::Asm(b) = stmt {
        let mut text = Vec::new();
        for l in &program.lines[from..=to] {
            text.extend_from_slice(&[13, (l.number >> 8) as u8, l.number as u8, (l.text.len() + 4) as u8]);
            text.extend_from_slice(&l.text);
        }
        text.extend_from_slice(&[13, 0xFF, 0, 0, 0, 0]);
        b.start += 4;
        b.text = text;
        b.ends = ends;
    }
}
