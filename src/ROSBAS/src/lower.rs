//! From statements and their control flow (flow.rs) to the IR (ir.rs).
//!
//! Each unit of flow.rs becomes an ir::Unit. Every statement the unit can
//! run gets a block. A statement's block ends by going where flow.rs says
//! control goes. Expressions are evaluated left to right, as the
//! interpreter evaluates them (§5.2). Each operation checks the kinds of
//! its operands as chapter 5 says, and raises error 6 where it must.

use std::collections::HashMap;

use crate::ast::{self, AssignOp, BinOp, Expr, Func, PrintItem, Stmt, Target, UnOp};
use crate::flow::{find_loop, Ctx, Dest, Flow, FrameKind, Loop, NodeId};
use crate::ir::*;
use crate::tokens::TDATA;

const NUM_NEEDED: &str = "Type mismatch: number needed";
const STR_NEEDED: &str = "Type mismatch: string needed";

pub fn lower(flow: &Flow, quirks: u32) -> Program {
    lower_with_libs(flow, quirks, &[])
}

/// Lowers a program and its libraries. `libs` holds each library's name,
/// as LIBRARY gives it, and the suffix of its errors (§15.3, §9.2.8).
pub fn lower_with_libs(flow: &Flow, quirks: u32, libs: &[(Vec<u8>, Option<Vec<u8>>)]) -> Program {
    let mut l = Lower {
        f: flow,
        p: Program {
            units: Vec::new(),
            vars: Vec::new(),
            arrays: Vec::new(),
            strs: Vec::new(),
            reals: Vec::new(),
            tokenised: Vec::new(),
            quirks,
            tables: false,
            clears: false,
            runs: false,
            libs: libs.iter().map(|l| l.1.clone()).collect(),
            fns: Vec::new(),
            lines: Vec::new(),
            data: Vec::new(),
            asm: Vec::new(),
        },
        vars: HashMap::new(),
        arrays: HashMap::new(),
        strs: HashMap::new(),
        reals: HashMap::new(),
        routines: HashMap::new(),
        local_arrays: flow.nodes.iter().any(|n| match &n.p.stmt {
            Stmt::Local(items) => items.iter().any(|i| matches!(i, ast::LocalItem::Array(_))),
            _ => false,
        }),
        local_data: flow.nodes.iter().any(|n| n.p.stmt == Stmt::LocalData),
        tables: false,
        lib_names: libs.iter().map(|l| l.0.clone()).collect(),
    };
    for (k, u) in flow.units.iter().enumerate() {
        if let Some((_, d)) = &u.def {
            l.routines.entry((d.is_fn, d.name.clone())).or_insert(k);
        }
    }
    for k in 0..flow.units.len() {
        let u = l.unit(k);
        l.p.units.push(u);
    }
    if l.tables {
        l.tables();
    }
    l.p
}

/// Whether an expression begins with a variable, as `x + 1` does. The
/// interpreter reads the variable, and then fails at what follows
/// (§8.4.4).
fn starts_with_variable(e: &Expr) -> bool {
    match e {
        Expr::Bin(_, l, _) => starts_with_variable(l),
        Expr::Var(_) | Expr::Elem(..) | Expr::Ind(..) => true,
        _ => false,
    }
}

/// The bits of the quirks for-byte-word and restore-data-pop (RB_Q_* in
/// rb.h).
const FOR_BYTE_WORD: u32 = 1 << 9;
const RESTORE_DATA_POP: u32 = 1 << 12;

/// A FOR's control variable. It is a variable, or an element or
/// indirection whose index or address FOR keeps in a slot (§7.4.2).
#[derive(Debug, Clone, Copy)]
enum ForPlace {
    Var(VarId),
    Elem(u32, u32),
    Ind(ast::Ind, u32),
}

/// Where a RETURN parameter's result goes (§8.4.2).
enum RetTarget {
    Var(VarId),
    Elem(u32, Temp, bool),
    Ind(ast::Ind, Temp),
    None,
}

struct Lower<'f> {
    f: &'f Flow,
    p: Program,
    vars: HashMap<(Vec<u8>, ast::VarKind), VarId>,
    arrays: HashMap<(Vec<u8>, ast::VarKind), u32>,
    strs: HashMap<Vec<u8>, u32>,
    reals: HashMap<Vec<u8>, u32>,
    routines: HashMap<(bool, Vec<u8>), usize>,
    /// Whether the program has a LOCAL array. If it has, LOCAL checks that
    /// it is still allowed (§8.6.4).
    local_arrays: bool,
    /// Whether the program has LOCAL DATA. If it has, each routine restores
    /// the DATA pointer entries that it leaves behind (§11.4.2).
    local_data: bool,
    /// Whether the runtime needs the program's tables.
    tables: bool,
    /// The libraries' names, library 1 first (§15.3.1).
    lib_names: Vec<Vec<u8>>,
}

/// A unit being built.
struct UB {
    blocks: Vec<Block>,
    cur: BlockId,
    temps: Vec<Ty>,
    slots: Vec<Ty>,
    node_block: HashMap<NodeId, BlockId>,
    end_block: Option<BlockId>,
    /// A FOR's slots for the limit and step, and whether the loop is an
    /// integer loop.
    for_slots: HashMap<NodeId, (u32, u32, bool)>,
    /// The place of a FOR's control variable, found once (§7.4.2).
    for_places: HashMap<NodeId, ForPlace>,
    /// GOSUB return points, as their numbers and blocks.
    returns: Vec<(u32, BlockId)>,
    /// Blocks that end in RETURN. They are given the return points at the
    /// end.
    return_blocks: Vec<BlockId>,
    /// The save stack's mark on entry, for routines.
    mark: Option<Temp>,
    /// The handler state on entry, for routines. rb_routine_enter returns
    /// it.
    hm: Option<Temp>,
    /// The RETURN formals, and where their results go (§8.4.3).
    ret_formals: Vec<VarId>,
    retbuf: Option<Temp>,
    /// The depth of LOCAL DATA entries on entry, for routines.
    dd: Option<Temp>,
}

impl UB {
    fn temp(&mut self, ty: Ty) -> Temp {
        self.temps.push(ty);
        Temp { id: self.temps.len() as u32 - 1, ty }
    }
    fn block(&mut self) -> BlockId {
        self.blocks.push(Block { insts: Vec::new(), term: Term::Open });
        self.blocks.len() as u32 - 1
    }
    fn emit(&mut self, i: Inst) {
        let c = self.cur as usize;
        self.blocks[c].insts.push(i);
    }
    fn let_(&mut self, ty: Ty, rv: Rv) -> Temp {
        let t = self.temp(ty);
        self.emit(Inst::Let(t, rv));
        t
    }
    fn op(&mut self, ty: Ty, name: &'static str, args: Vec<Operand>) -> Temp {
        self.let_(ty, Rv::Op(name, args))
    }
    /// End the current block. Later code goes into a fresh block.
    fn end(&mut self, t: Term) {
        let c = self.cur as usize;
        if self.blocks[c].term == Term::Open {
            self.blocks[c].term = t;
        }
        self.cur = self.block();
    }
    /// Raise an error here. Code after it is unreachable.
    fn raise(&mut self, n: i32, m: &'static str) {
        self.end(Term::Raise(n, m));
    }
}

impl<'f> Lower<'f> {
    fn var(&mut self, v: &ast::Var) -> VarId {
        if let Some(&id) = self.vars.get(&(v.name.clone(), v.kind)) {
            return id;
        }
        let kind = if v.name == b"@%" {
            VarKind::AtPct
        } else {
            match v.kind {
                ast::VarKind::Real => VarKind::Real,
                ast::VarKind::Int => VarKind::Int,
                ast::VarKind::Str => VarKind::Str,
            }
        };
        self.p.vars.push(Var { name: v.name.clone(), kind, resident: v.resident() });
        let id = self.p.vars.len() as u32 - 1;
        self.vars.insert((v.name.clone(), v.kind), id);
        id
    }

    fn arr(&mut self, v: &ast::Var) -> u32 {
        if let Some(&id) = self.arrays.get(&(v.name.clone(), v.kind)) {
            return id;
        }
        let kind = match v.kind {
            ast::VarKind::Real => VarKind::Real,
            ast::VarKind::Int => VarKind::Int,
            ast::VarKind::Str => VarKind::Str,
        };
        self.p.arrays.push(Var { name: v.name.clone(), kind, resident: false });
        let id = self.p.arrays.len() as u32 - 1;
        self.arrays.insert((v.name.clone(), v.kind), id);
        id
    }

    /// An element's index. The array must exist and be dimensioned. Each
    /// subscript is checked as soon as it is evaluated (§4.5.9-4.5.10).
    fn elem_index(&mut self, ub: &mut UB, a: u32, subs: &[Expr]) -> Temp {
        ub.emit(Inst::Do("rb_arr_ok", vec![Operand::Arr(a)]));
        let mut acc = Operand::Imm(0);
        for (k, s) in subs.iter().enumerate() {
            let t = self.expr(ub, s);
            let n = self.num(ub, t);
            let r = ub.op(Ty::Int, "rb_arr_sub", vec![Operand::Arr(a), Operand::Imm(k as i64), acc, Operand::T(n)]);
            acc = Operand::T(r);
        }
        ub.op(Ty::Int, "rb_arr_end", vec![Operand::Arr(a), Operand::Imm(subs.len() as i64), acc])
    }

    fn str_lit(&mut self, s: &[u8]) -> u32 {
        if let Some(&k) = self.strs.get(s) {
            return k;
        }
        self.p.strs.push(s.to_vec());
        let k = self.p.strs.len() as u32 - 1;
        self.strs.insert(s.to_vec(), k);
        k
    }

    fn real_lit(&mut self, s: &[u8]) -> u32 {
        if let Some(&k) = self.reals.get(s) {
            return k;
        }
        self.p.reals.push(s.to_vec());
        let k = self.p.reals.len() as u32 - 1;
        self.reals.insert(s.to_vec(), k);
        k
    }

    fn var_ty(&self, id: VarId) -> Ty {
        match self.p.vars[id as usize].kind {
            VarKind::Str => Ty::Str,
            _ => Ty::Num,
        }
    }

    // ---- Units ----------------------------------------------------------

    fn unit(&mut self, k: usize) -> Unit {
        let fu = &self.f.units[k];
        let (kind, name, formals) = match &fu.def {
            None => (UnitKind::Main, b"main".to_vec(), Vec::new()),
            Some((_, d)) => (if d.is_fn { UnitKind::Fn } else { UnitKind::Proc }, d.name.clone(), d.params.clone()),
        };
        let mut ub = UB {
            blocks: Vec::new(),
            cur: 0,
            temps: Vec::new(),
            slots: Vec::new(),
            node_block: HashMap::new(),
            end_block: None,
            for_slots: HashMap::new(),
            for_places: HashMap::new(),
            returns: Vec::new(),
            return_blocks: Vec::new(),
            mark: None,
            hm: None,
            ret_formals: Vec::new(),
            retbuf: None,
            dd: None,
        };
        let entry = ub.block();
        // A routine's parameters. Temp 0 is the save mark. Then there is one
        // for each formal, of the formal's kind. The caller has saved the
        // formals and evaluated the arguments (§8.3.4-8.3.5)
        let mut nparams = 0;
        if kind != UnitKind::Main {
            ub.mark = Some(ub.temp(Ty::Int));
            // One C parameter for each formal. It is a value for value and
            // RETURN formals (§8.3.1, §8.4.1), or an array (§8.5.1)
            let mut bind = Vec::new();
            for p in &formals {
                if p.kind == ast::ParamKind::Array {
                    let a = self.arr(&p.var);
                    bind.push((None, Some(a), ub.temp(Ty::Arr)));
                } else {
                    let v = self.var(&p.var);
                    if p.kind == ast::ParamKind::Return {
                        ub.ret_formals.push(v);
                    }
                    bind.push((Some(v), None, ub.temp(self.var_ty(v))));
                }
            }
            nparams = formals.len();
            if !ub.ret_formals.is_empty() {
                ub.retbuf = Some(ub.temp(Ty::RetBuf(ub.ret_formals.len() as u32)));
                nparams += 1;
            }
            for (v, a, t) in bind {
                match (v, a) {
                    (Some(v), _) => ub.emit(Inst::Store(v, t)),
                    (_, Some(a)) => ub.emit(Inst::SetArr(a, t)),
                    _ => {}
                }
            }
            if self.local_data {
                ub.dd = Some(ub.op(Ty::Int, "rb_data_depth", vec![]));
            }
            let m = ub.mark.unwrap();
            ub.hm = Some(ub.op(Ty::Int, "rb_routine_enter", vec![Operand::T(m)]));
        }
        let ctx: Vec<(NodeId, Ctx)> = fu.ctx.iter().map(|(&a, b)| (a, b.clone())).collect();
        for (id, _) in &ctx {
            let b = ub.block();
            ub.node_block.insert(*id, b);
        }
        let first = self.dest(&mut ub, fu.entry);
        ub.cur = entry;
        ub.end(Term::Jump(first));
        for (id, c) in &ctx {
            ub.cur = ub.node_block[id];
            self.statement(&mut ub, *id, c);
        }
        let handlers: Vec<BlockId> = fu.handlers.iter().map(|h| ub.node_block[h]).collect();
        // RETURN goes to any of the unit's return points
        for b in ub.return_blocks.clone() {
            ub.blocks[b as usize].term = Term::PopReturn(ub.returns.clone());
        }
        // Fresh blocks that are left open are unreachable
        for b in ub.blocks.iter_mut() {
            if b.term == Term::Open {
                b.term = Term::End;
            }
        }
        Unit {
            kind,
            name,
            nparams,
            blocks: ub.blocks,
            temps: ub.temps,
            slots: ub.slots,
            saves: 0,
            gosub: !ub.returns.is_empty(),
            handlers,
        }
    }

    fn dest(&mut self, ub: &mut UB, d: Dest) -> BlockId {
        match d {
            Dest::Node(id) => match ub.node_block.get(&id) {
                Some(&b) => b,
                None => {
                    let b = ub.block();
                    ub.blocks[b as usize].term = Term::Raise(16, "Syntax error");
                    b
                }
            },
            Dest::End => {
                if let Some(b) = ub.end_block {
                    return b;
                }
                let b = ub.block();
                ub.blocks[b as usize].term = Term::End;
                ub.end_block = Some(b);
                b
            }
            Dest::Error(n, m) => {
                let b = ub.block();
                ub.blocks[b as usize].term = Term::Raise(n, m);
                b
            }
        }
    }

    fn goto(&mut self, ub: &mut UB, d: Dest) {
        let b = self.dest(ub, d);
        ub.end(Term::Jump(b));
    }

    /// Leave a routine. Its LOCAL ERROR entries and local handlers end
    /// (§9.5.5, §9.5.9). Then what it saved is put back (§8.7.3).
    fn restore(&mut self, ub: &mut UB) {
        // Take the final values of the RETURN formals before anything is
        // put back (§8.4.3)
        if let Some(buf) = ub.retbuf {
            for (k, v) in ub.ret_formals.clone().into_iter().enumerate() {
                let ty = self.var_ty(v);
                let t = ub.let_(ty, Rv::Load(v));
                let a = self.to_any(ub, t);
                ub.emit(Inst::Do("rb_ret_put", vec![Operand::T(buf), Operand::Imm(k as i64), Operand::T(a)]));
            }
        }
        if let Some(dd) = ub.dd {
            ub.emit(Inst::Do("rb_data_unwind", vec![Operand::T(dd), Operand::Imm(1)]));
        }
        if let Some(h) = ub.hm {
            ub.emit(Inst::Do("rb_routine_leave", vec![Operand::T(h)]));
        }
        if let Some(m) = ub.mark {
            ub.emit(Inst::Do("rb_save_restore", vec![Operand::T(m)]));
        }
    }

    /// A closing statement discards the LOCAL ERROR entries above its loop,
    /// and restores what each one saved (§7.1.1, §9.5.5).
    fn discard(&mut self, ub: &mut UB, ctx: &Ctx, above: usize) {
        for l in ctx.loops[above..].iter().rev() {
            match l {
                Loop::LocalError(_) => ub.emit(Inst::Do("rb_restore_error", vec![])),
                // The DATA pointer is put back (§11.4.2)
                Loop::LocalData(_) => ub.emit(Inst::Do("rb_restore_data", vec![])),
                _ => {}
            }
        }
    }

    // ---- Statements -----------------------------------------------------

    fn statement(&mut self, ub: &mut UB, id: NodeId, ctx: &Ctx) {
        let line = self.f.line_number(id);
        ub.emit(Inst::Line(line));
        if !self.lib_names.is_empty() {
            ub.emit(Inst::SetLib(self.f.seg_of[self.f.nodes[id].line] as u32));
        }
        let stmt = self.f.nodes[id].p.stmt.clone();
        let next = self.f.next(id);
        match stmt {
            Stmt::Nothing => self.goto(ub, next),
            Stmt::Def => self.goto(ub, self.f.first_from_line(self.f.nodes[id].line + 1)),
            Stmt::ElseSkip => self.goto(ub, self.f.first_from_line(self.f.nodes[id].line + 1)),
            Stmt::Print(items) => {
                self.print(ub, &items);
                self.goto(ub, next);
            }
            Stmt::Assign(Target::Var(v), op, e, let_) => {
                // += and -= never make their variable (§4.2.4)
                if op != AssignOp::Set {
                    let id = self.var(&v);
                    ub.emit(Inst::MustExist(id, 4, if let_ { "Missing =" } else { "Mistake" }));
                }
                self.assign(ub, &v, op, &e);
                self.goto(ub, next);
            }
            Stmt::Assign(Target::Elem(v, subs), op, e, _) => {
                self.assign_elem(ub, &v, &subs, op, &e);
                self.goto(ub, next);
            }
            Stmt::Assign(Target::Ind(kind, base, off), op, e, _) => {
                self.assign_ind(ub, kind, &base, off.as_ref(), op, &e);
                self.goto(ub, next);
            }
            Stmt::WholeArray(a, v) => {
                self.whole_array(ub, &a, &v);
                self.goto(ub, next);
            }
            Stmt::Dim(items) => {
                for item in &items {
                    let (v, bounds) = match item {
                        ast::DimItem::Array(v, b) => (v, b),
                        ast::DimItem::LocalBlock(v, n) => {
                            // Only where LOCAL is allowed (§12.1.8)
                            if !((ctx.frame == FrameKind::Proc || ctx.frame == FrameKind::Fn) && ctx.loops.is_empty()) {
                                ub.raise(12, "Items can only be made local in a function or procedure");
                                break;
                            }
                            let id = self.var(v);
                            let t = self.expr(ub, n);
                            let t = self.num(ub, t);
                            let a = ub.op(Ty::Num, "rb_dim_local", vec![Operand::T(t)]);
                            ub.emit(Inst::Store(id, a));
                            continue;
                        }
                        ast::DimItem::Block(v, n) => {
                            // The address is stored before a negative size
                            // fails. For -1 and below, END's value is
                            // stored (§12.1.4)
                            let id = self.var(v);
                            let t = self.expr(ub, n);
                            let t = self.num(ub, t);
                            let a = ub.op(Ty::Num, "rb_dim_block", vec![Operand::T(t)]);
                            ub.emit(Inst::Store(id, a));
                            ub.emit(Inst::Do("rb_dim_block_neg", vec![Operand::T(t)]));
                            continue;
                        }
                    };
                    let a = self.arr(v);
                    let kind = match v.kind {
                        ast::VarKind::Int => 0,
                        ast::VarKind::Real => 1,
                        ast::VarKind::Str => 2,
                    };
                    ub.emit(Inst::Do("rb_dim_start", vec![Operand::Arr(a), Operand::Imm(kind)]));
                    for b in bounds {
                        let t = self.expr(ub, b);
                        let n = self.num(ub, t);
                        ub.emit(Inst::Do("rb_dim_add", vec![Operand::Arr(a), Operand::T(n)]));
                    }
                    ub.emit(Inst::Do("rb_dim_end", vec![Operand::Arr(a)]));
                }
                self.goto(ub, next);
            }
            Stmt::If(c) => {
                let t = self.condition(ub, &c);
                let yes = self.dest(ub, next);
                let no = self.dest(ub, self.f.else_target(id).0);
                ub.end(Term::Branch(t, yes, no));
            }
            Stmt::BlockIf(c) => {
                let t = self.condition(ub, &c);
                let yes = self.dest(ub, next);
                let no = self.dest(ub, self.f.block_if_scan(id));
                ub.end(Term::Branch(t, yes, no));
            }
            Stmt::BlockElse => self.goto(ub, self.f.block_else_scan(id)),
            Stmt::Goto(n) => self.goto(ub, self.f.line_dest(n)),
            Stmt::GotoBad(n) => match self.f.line_dest(n) {
                Dest::Error(e, m) => ub.raise(e, m),
                _ => ub.raise(16, "Syntax error"),
            },
            Stmt::For { var, start, limit, step } => self.for_(ub, id, &var, &start, &limit, step.as_ref(), next),
            // The parser does not yet give an element as a control variable
            Stmt::Next(vars) => self.next_(ub, id, ctx, &vars, next),
            Stmt::Repeat => self.goto(ub, next),
            Stmt::Until(c) => match find_loop(ctx, |l| matches!(l, Loop::Repeat(_))) {
                Some(k) => {
                    let rep = match ctx.loops[k] {
                        Loop::Repeat(r) => r,
                        _ => unreachable!(),
                    };
                    let t = self.condition(ub, &c);
                    self.discard(ub, ctx, k + 1);
                    let out = self.dest(ub, next);
                    let back = self.dest(ub, self.f.next(rep));
                    ub.end(Term::Branch(t, out, back));
                }
                None => {
                    self.condition(ub, &c);
                    self.discard(ub, ctx, 0);
                    ub.raise(43, "Not in a REPEAT loop");
                }
            },
            Stmt::While(c) => {
                let t = self.condition(ub, &c);
                let yes = self.dest(ub, next);
                let no = self.dest(ub, self.f.while_scan(id));
                ub.end(Term::Branch(t, yes, no));
            }
            Stmt::EndWhile => match find_loop(ctx, |l| matches!(l, Loop::While(_))) {
                Some(k) => {
                    let w = match ctx.loops[k] {
                        Loop::While(w) => w,
                        _ => unreachable!(),
                    };
                    let c = match &self.f.nodes[w].p.stmt {
                        Stmt::While(c) => c.clone(),
                        _ => unreachable!(),
                    };
                    self.discard(ub, ctx, k + 1);
                    let t = self.condition(ub, &c);
                    let body = self.dest(ub, self.f.next(w));
                    let out = self.dest(ub, next);
                    ub.end(Term::Branch(t, body, out));
                }
                None => {
                    self.discard(ub, ctx, 0);
                    ub.raise(46, "Not in a WHILE loop");
                }
            },
            Stmt::Case(e) => self.case(ub, id, &e),
            Stmt::When(_) | Stmt::Otherwise => self.goto(ub, self.f.clause_end_scan(id)),
            Stmt::Gosub(n) => {
                let ret = self.dest(ub, next);
                let k = ub.returns.len() as u32;
                ub.returns.push((k, ret));
                ub.emit(Inst::PushReturn(k));
                self.goto(ub, self.f.line_dest(n));
            }
            Stmt::Return => {
                if ctx.frame == FrameKind::Gosub {
                    self.discard(ub, ctx, 0);
                    let c = ub.cur;
                    ub.return_blocks.push(c);
                    ub.end(Term::End);
                } else {
                    ub.raise(38, "Not in a subroutine");
                }
            }
            Stmt::On { index, gosub, lines, has_else } => self.on(ub, id, &index, gosub, &lines, has_else),
            Stmt::OnProc { index, items, has_else, .. } => {
                let t = self.expr(ub, &index);
                let n = self.num(ub, t);
                let k = ub.op(Ty::Int, "rb_toint", vec![Operand::T(n)]);
                let li = self.f.nodes[id].line;
                for (i, (name, args, end)) in items.iter().enumerate() {
                    let is = ub.op(Ty::Int, "rb_int_is", vec![Operand::T(k), Operand::Imm(i as i64 + 1)]);
                    let go = ub.block();
                    let miss = ub.block();
                    ub.end(Term::Branch(is, go, miss));
                    ub.cur = go;
                    self.call(ub, false, name, args);
                    self.goto(ub, self.f.after_colon(li, *end));
                    ub.cur = miss;
                }
                if has_else {
                    self.goto(ub, self.f.on_else(id));
                } else {
                    ub.raise(40, "ON range");
                }
            }
            Stmt::Proc(name, args) => {
                self.call(ub, false, &name, &args);
                self.goto(ub, next);
            }
            Stmt::EndProc => {
                if ctx.frame == FrameKind::Proc {
                    if self.p.quirks & RESTORE_DATA_POP != 0 {
                        ub.emit(Inst::Do("rb_frame_check", vec![Operand::Imm(0)]));
                    }
                    self.restore(ub);
                    ub.end(Term::Return(None));
                } else {
                    ub.raise(13, "Not in a procedure");
                }
            }
            Stmt::FnReturn(e) => {
                let t = self.expr(ub, &e);
                if ctx.frame == FrameKind::Fn {
                    if self.p.quirks & RESTORE_DATA_POP != 0 {
                        ub.emit(Inst::Do("rb_frame_check", vec![Operand::Imm(1)]));
                    }
                    let a = self.to_any(ub, t);
                    self.restore(ub);
                    ub.end(Term::Return(Some(a)));
                } else {
                    ub.raise(7, "Not in a function");
                }
            }
            Stmt::Local(vars) => {
                if (ctx.frame == FrameKind::Proc || ctx.frame == FrameKind::Fn) && ctx.loops.is_empty() {
                    if self.local_arrays {
                        ub.emit(Inst::Do("rb_local_ok", vec![Operand::T(ub.mark.unwrap())]));
                    }
                    for item in &vars {
                        match item {
                            ast::LocalItem::Var(v) => {
                                let id = self.var(v);
                                ub.emit(Inst::Save(id, 0));
                                ub.emit(Inst::Clear(id));
                            }
                            ast::LocalItem::Array(v) => {
                                // A new undimensioned array for this call
                                // (§8.6.6)
                                let a = self.arr(v);
                                ub.emit(Inst::SaveArr(a));
                                ub.emit(Inst::Do("rb_arr_local", vec![Operand::ArrSlot(a)]));
                            }
                            ast::LocalItem::Ind(kind, base, off) => {
                                let a = self.address(ub, base, off.as_ref());
                                let k = match kind {
                                    ast::Ind::Byte => 0,
                                    ast::Ind::Word => 1,
                                    _ => 3,
                                };
                                ub.emit(Inst::Do("rb_local_mem", vec![Operand::T(a), Operand::Imm(k)]));
                            }
                        }
                    }
                    self.goto(ub, next);
                } else {
                    ub.raise(12, "Items can only be made local in a function or procedure");
                }
            }
            Stmt::End => ub.end(Term::End),
            Stmt::Stop => ub.raise(0, "Stopped"),
            Stmt::Quit(e) => {
                if let Some(e) = e {
                    let t = self.expr(ub, &e);
                    let n = self.num(ub, t);
                    ub.emit(Inst::Do("rb_quit", vec![Operand::T(n)]));
                }
                ub.end(Term::End);
            }
            Stmt::Width(e) => {
                let t = self.expr(ub, &e);
                let n = self.num(ub, t);
                ub.emit(Inst::Do("rb_width_set", vec![Operand::T(n)]));
                self.goto(ub, next);
            }
            Stmt::Fail(parts, f) => {
                for e in &parts {
                    self.expr(ub, e);
                }
                ub.raise(f.number, f.message);
            }
            Stmt::OnError { local, off } => {
                // ON ERROR OFF goes on with the next statement (s/Stmt ONERRF)
                let after = if off && !local { next } else { self.f.first_from_line(self.f.nodes[id].line + 1) };
                if off {
                    ub.emit(Inst::Do("rb_on_error_off", vec![]));
                } else if let Dest::Node(h) = next {
                    if local {
                        let k = self.f.units.iter().find(|u| u.ctx.contains_key(&id) && u.handlers.contains(&h));
                        let n = k.map(|u| u.handlers.iter().position(|&x| x == h).unwrap()).unwrap_or(0);
                        ub.emit(Inst::Do("rb_on_error_local", vec![Operand::Jb, Operand::Imm(n as i64)]));
                    } else {
                        let n = self.f.units[0].handlers.iter().position(|&x| x == h).unwrap_or(0);
                        ub.emit(Inst::Do("rb_on_error", vec![Operand::Imm(n as i64)]));
                    }
                } else {
                    // A handler with no statements, at the end of the
                    // program. Entering it ends the program
                    ub.emit(Inst::Do("rb_on_error_off", vec![]));
                }
                self.goto(ub, after);
            }
            Stmt::LocalError => {
                ub.emit(Inst::Do("rb_local_error", vec![]));
                self.goto(ub, next);
            }
            Stmt::RestoreError => match ctx.loops.last() {
                Some(Loop::LocalError(_)) => {
                    ub.emit(Inst::Do("rb_restore_error", vec![]));
                    self.goto(ub, next);
                }
                _ => ub.raise(0, "Error control status not found on stack for RESTORE ERROR"),
            },
            Stmt::Error { ext, number, message } => {
                let n = self.expr(ub, &number);
                let n = self.num(ub, n);
                let m = self.expr(ub, &message);
                let m = self.str_(ub, m);
                ub.emit(Inst::Do(if ext { "rb_error_ext" } else { "rb_error_stmt" }, vec![Operand::T(n), Operand::T(m)]));
                ub.end(Term::End);
            }
            Stmt::Report => {
                ub.emit(Inst::Do("rb_report_stmt", vec![]));
                self.goto(ub, next);
            }
            Stmt::LocalData => {
                self.tables = true;
                ub.emit(Inst::Do("rb_local_data", vec![]));
                self.goto(ub, next);
            }
            Stmt::RestoreData => match ctx.loops.last() {
                Some(Loop::LocalData(_)) => {
                    ub.emit(Inst::Do("rb_restore_data", vec![]));
                    self.goto(ub, next);
                }
                _ => {
                    // With the quirk on, the failure damages the entry of a
                    // local handler that was set here with no loop since
                    // (§11.4.5)
                    let routine = ctx.frame == FrameKind::Proc || ctx.frame == FrameKind::Fn;
                    if self.p.quirks & RESTORE_DATA_POP != 0 && routine && ctx.loops.iter().all(|l| matches!(l, Loop::LocalError(_))) {
                        ub.emit(Inst::Do("rb_frame_damage", vec![]));
                    }
                    ub.raise(42, "DATA pointer not found on stack for RESTORE DATA")
                }
            },
            Stmt::Restore(to) => {
                self.tables = true;
                match to {
                    ast::RestoreTo::Start => ub.emit(Inst::Do("rb_restore", vec![])),
                    ast::RestoreTo::Line(e) => {
                        let t = self.expr(ub, &e);
                        let n = self.num(ub, t);
                        ub.emit(Inst::Do("rb_restore_line", vec![Operand::T(n)]));
                    }
                    ast::RestoreTo::Rel(e) => {
                        let t = self.expr(ub, &e);
                        let n = self.num(ub, t);
                        let here = self.f.nodes[id].line as i64;
                        ub.emit(Inst::Do("rb_restore_rel", vec![Operand::Imm(here), Operand::T(n)]));
                    }
                }
                self.goto(ub, next);
            }
            Stmt::Swap(a, b) => {
                // Both are found and read, and then assigned. Each one
                // converts as `=` does (§4.10.2)
                let (pa, va) = self.place(ub, &a);
                let (pb, vb) = self.place(ub, &b);
                let (sa, sb) = (self.place_is_str(&pa), self.place_is_str(&pb));
                if sa != sb {
                    ub.raise(6, if sa { NUM_NEEDED } else { STR_NEEDED });
                } else {
                    self.store_place(ub, &pb, va);
                    self.store_place(ub, &pa, vb);
                    self.goto(ub, next);
                }
            }
            Stmt::SwapArrays(a, b) => {
                let (a, b) = (self.arr(&a), self.arr(&b));
                ub.emit(Inst::Do("rb_arr_swap", vec![Operand::Arr(a), Operand::Arr(b)]));
                self.goto(ub, next);
            }
            Stmt::SubAssign { kind, target, args, value } => {
                let (pl, cur) = self.place(ub, &target);
                let mut nums = Vec::new();
                for a in &args {
                    let t = self.expr(ub, a);
                    nums.push(self.num(ub, t));
                }
                let t = self.expr(ub, &value);
                let s = self.str_(ub, t);
                let zero = ub.op(Ty::Num, "rb_i", vec![Operand::Imm(0)]);
                let a0 = nums.first().copied().unwrap_or(zero);
                let a1 = nums.get(1).copied().unwrap_or(zero);
                let r = ub.op(
                    Ty::Str,
                    "rb_substr_set",
                    vec![Operand::T(cur), Operand::Imm(kind as i64), Operand::Imm(nums.len() as i64), Operand::T(a0), Operand::T(a1), Operand::T(s)],
                );
                self.store_place(ub, &pl, r);
                self.goto(ub, next);
            }
            Stmt::Run => {
                self.p.clears = true;
                self.p.runs = true;
                ub.emit(Inst::Do("rb_run", vec![]));
                ub.end(Term::End);
            }
            Stmt::Os { exprs, calls } => {
                if calls.iter().any(|(f, _)| *f == "rb_clear" || *f == "rb_lomem_set") {
                    self.p.clears = true;
                }
                let mut temps = Vec::new();
                for (e, kind) in &exprs {
                    let t = self.expr(ub, e);
                    temps.push(match kind {
                        ast::OsKind::Str => self.str_(ub, t),
                        ast::OsKind::Num => self.num(ub, t),
                        // Converted at once (§3.7.2)
                        ast::OsKind::Int => {
                            let n = self.num(ub, t);
                            ub.op(Ty::Num, "rb_toint_num", vec![Operand::T(n)])
                        }
                    });
                }
                for (f, refs) in &calls {
                    let ops = refs
                        .iter()
                        .map(|r| match r {
                            ast::OsRef::E(k) => Operand::T(temps[*k]),
                            ast::OsRef::Int(v) => Operand::Imm(*v as i64),
                            ast::OsRef::Num(v) => Operand::T(ub.op(Ty::Num, "rb_i", vec![Operand::Imm(*v as i64)])),
                        })
                        .collect();
                    ub.emit(Inst::Do(f, ops));
                }
                self.goto(ub, next);
            }
            Stmt::Star(text) => {
                let ops = if text.is_empty() { vec![Operand::Imm(0), Operand::Imm(0)] } else { vec![Operand::Str(self.str_lit(&text))] };
                ub.emit(Inst::Do("rb_star", ops));
                self.goto(ub, next);
            }
            Stmt::Vdu(items) => {
                // Each value is written before the next is evaluated
                for (e, sep) in &items {
                    let t = self.expr(ub, e);
                    let n = self.num(ub, t);
                    ub.emit(Inst::Do("rb_vdu", vec![Operand::T(n), Operand::Imm(*sep as i64)]));
                }
                self.goto(ub, next);
            }
            Stmt::ModeOne(e) => {
                let t = self.expr(ub, &e);
                match t.ty {
                    Ty::Str => ub.emit(Inst::Do("rb_mode_str", vec![Operand::T(t)])),
                    Ty::Any => ub.emit(Inst::Do("rb_mode_any", vec![Operand::T(t)])),
                    _ => {
                        let n = self.num(ub, t);
                        ub.emit(Inst::Do("rb_mode", vec![Operand::T(n)]));
                    }
                }
                self.goto(ub, next);
            }
            Stmt::Envelope(v) => {
                let buf = ub.temp(Ty::RetBuf(v.len() as u32));
                for (k, e) in v.iter().enumerate() {
                    let t = self.expr(ub, e);
                    let a = self.to_any(ub, t);
                    ub.emit(Inst::Do("rb_ret_put", vec![Operand::T(buf), Operand::Imm(k as i64), Operand::T(a)]));
                }
                ub.emit(Inst::Do("rb_envelope", vec![Operand::T(buf)]));
                self.goto(ub, next);
            }
            Stmt::Mouse(targets) => {
                let places: Vec<RetTarget> = targets.iter().map(|t| self.place_addr(ub, t)).collect();
                if places.iter().any(|p| self.place_is_str(p)) {
                    ub.raise(50, "Bad MOUSE variable");
                } else {
                    let buf = ub.temp(Ty::RetBuf(4));
                    ub.emit(Inst::Do("rb_mouse", vec![Operand::T(buf)]));
                    for (k, p) in places.iter().enumerate() {
                        let v = ub.op(Ty::Num, "rb_ret_num", vec![Operand::T(buf), Operand::Imm(k as i64)]);
                        self.store_place(ub, p, v);
                    }
                    self.goto(ub, next);
                }
            }
            Stmt::Sys { swi, ins, outs, flags } => {
                let s = self.expr(ub, &swi);
                let s = self.to_any(ub, s);
                let inbuf = ub.temp(Ty::RetBuf(ins.len().max(1) as u32));
                for (k, e) in ins.iter().enumerate() {
                    let a = match e {
                        Some(e) => {
                            let t = self.expr(ub, e);
                            self.to_any(ub, t)
                        }
                        None => {
                            let z = ub.op(Ty::Num, "rb_i", vec![Operand::Imm(0)]);
                            self.to_any(ub, z)
                        }
                    };
                    ub.emit(Inst::Do("rb_ret_put", vec![Operand::T(inbuf), Operand::Imm(k as i64), Operand::T(a)]));
                }
                let out = ub.temp(Ty::RetBuf(11));
                ub.emit(Inst::Do("rb_sys", vec![Operand::T(s), Operand::T(inbuf), Operand::Imm(ins.len() as i64), Operand::T(out)]));
                // The registers are assigned in order after the call (§14.2.5)
                let regs = outs.iter().enumerate().filter_map(|(k, t)| t.as_ref().map(|t| (k, t)));
                for (k, t) in regs.chain(flags.iter().map(|t| (10, t))) {
                    let p = self.place_addr(ub, t);
                    let v = if self.place_is_str(&p) && k < 10 {
                        ub.op(Ty::Str, "rb_sys_str", vec![Operand::T(out), Operand::Imm(k as i64)])
                    } else {
                        ub.op(Ty::Num, "rb_ret_num", vec![Operand::T(out), Operand::Imm(k as i64)])
                    };
                    self.store_place(ub, &p, v);
                }
                self.goto(ub, next);
            }
            Stmt::Bput(c, e, semi) => {
                let ch = self.channel_value(ub, &c);
                let t = self.expr(ub, &e);
                match t.ty {
                    Ty::Str => ub.emit(Inst::Do("rb_bput_str", vec![Operand::T(ch), Operand::T(t), Operand::Imm(!semi as i64)])),
                    Ty::Any => ub.emit(Inst::Do("rb_bput_any", vec![Operand::T(ch), Operand::T(t), Operand::Imm(semi as i64)])),
                    _ if semi => ub.raise(16, "Syntax error"),
                    _ => {
                        let n = self.num(ub, t);
                        ub.emit(Inst::Do("rb_bput", vec![Operand::T(ch), Operand::T(n)]));
                    }
                }
                self.goto(ub, next);
            }
            Stmt::PrintFile(c, items) => {
                let ch = self.channel_value(ub, &c);
                // Each item is written before the next is evaluated (§13.5.1)
                for e in &items {
                    let t = self.expr(ub, e);
                    let a = self.to_any(ub, t);
                    ub.emit(Inst::Do("rb_printf_item", vec![Operand::T(ch), Operand::T(a)]));
                }
                self.goto(ub, next);
            }
            Stmt::InputFile(c, targets) => {
                let ch = self.channel_value(ub, &c);
                for t in &targets {
                    let p = self.place_addr(ub, t);
                    let v = if self.place_is_str(&p) {
                        ub.op(Ty::Str, "rb_inputf_str", vec![Operand::T(ch)])
                    } else {
                        ub.op(Ty::Num, "rb_inputf_num", vec![Operand::T(ch)])
                    };
                    self.store_place(ub, &p, v);
                }
                self.goto(ub, next);
            }
            Stmt::Input { line, items } => {
                ub.emit(Inst::Do("rb_input_begin", vec![]));
                for it in &items {
                    match it {
                        ast::InputItem::Prompt(pi) => {
                            self.print_item(ub, pi);
                            ub.emit(Inst::Do("rb_input_prompt", vec![]));
                        }
                        ast::InputItem::Var(t, q) => {
                            let p = self.place_addr(ub, t);
                            let (ty, f) = match (self.place_is_str(&p), line) {
                                (true, false) => (Ty::Str, "rb_input_str"),
                                (false, false) => (Ty::Num, "rb_input_num"),
                                (true, true) => (Ty::Str, "rb_input_line"),
                                (false, true) => (Ty::Num, "rb_input_line_num"),
                            };
                            let v = ub.op(ty, f, vec![Operand::Imm(*q as i64)]);
                            self.store_place(ub, &p, v);
                        }
                    }
                }
                ub.emit(Inst::Do("rb_input_end", vec![]));
                self.goto(ub, next);
            }
            Stmt::Read(targets) => {
                self.tables = true;
                for t in &targets {
                    self.read(ub, t);
                }
                self.goto(ub, next);
            }
            Stmt::Unsupported(_) | Stmt::Rejected(_) => ub.raise(16, "Syntax error"),
            // The assembler runs the block (chapter 16). Its names are the
            // program's variables, in the tables that the runtime reads
            Stmt::Asm(b) => {
                self.tables = true;
                for c in [b'P', b'O', b'L'] {
                    self.var(&ast::Var { name: vec![c, b'%'], kind: ast::VarKind::Int });
                }
                for (name, kind, array) in asm_names(&b.text, b.start) {
                    let v = ast::Var { name, kind };
                    if array {
                        self.arr(&v);
                    } else {
                        self.var(&v);
                    }
                }
                let k = self.p.asm.len() as u32;
                self.p.asm.push(b.text.clone());
                ub.emit(Inst::Do("rb_asm_block", vec![Operand::Asm(k), Operand::Imm(b.start as i64)]));
                if b.ends {
                    self.goto(ub, next);
                } else {
                    ub.end(Term::End);          // the program ended in the block
                }
            }
            Stmt::Call(addr, params) => {
                // First the address, converted to an integer. Then the
                // parameters, each made if it does not exist (§14.3.1)
                let t = self.expr(ub, &addr);
                let n = self.num(ub, t);
                let n = ub.op(Ty::Num, "rb_toint_num", vec![Operand::T(n)]);
                if !params.is_empty() {
                    ub.emit(Inst::Do("rb_call_begin", vec![]));
                }
                for p in &params {
                    self.call_param(ub, p);
                }
                let mut args = vec![Operand::T(n)];
                args.extend(self.a_to_h(ub));
                ub.emit(Inst::Do("rb_call", args));
                self.goto(ub, next);
            }
            Stmt::Library(name) => {
                // The library's routines can be called from now on (§15.3.2)
                if let Some(k) = self.lib_names.iter().position(|n| *n == name) {
                    ub.emit(Inst::Do("rb_lib_load", vec![Operand::Imm(k as i64 + 1)]));
                }
                self.goto(ub, next);
            }
        }
    }

    fn print(&mut self, ub: &mut UB, items: &[PrintItem]) {
        ub.emit(Inst::Do("rb_print_begin", vec![]));
        for it in items {
            self.print_item(ub, it);
        }
        let semi = matches!(items.last(), Some(PrintItem::Semicolon));
        ub.emit(Inst::Do("rb_print_end", vec![Operand::Imm(semi as i64)]));
    }

    fn print_item(&mut self, ub: &mut UB, it: &PrintItem) {
        {
            match it {
                PrintItem::Value(e) => {
                    let t = self.expr(ub, e);
                    let f = match t.ty {
                        Ty::Str => "rb_print_str",
                        Ty::Any => "rb_print_any",
                        _ => "rb_print_num",
                    };
                    ub.emit(Inst::Do(f, vec![Operand::T(t)]));
                }
                PrintItem::Semicolon => ub.emit(Inst::Do("rb_print_semicolon", vec![])),
                PrintItem::Comma => ub.emit(Inst::Do("rb_print_comma", vec![])),
                PrintItem::Tilde => ub.emit(Inst::Do("rb_print_tilde", vec![])),
                PrintItem::Quote => ub.emit(Inst::Do("rb_print_quote", vec![])),
                PrintItem::Tab(x) => {
                    let t = self.expr(ub, x);
                    let n = self.num(ub, t);
                    ub.emit(Inst::Do("rb_print_tab", vec![Operand::T(n)]));
                }
                PrintItem::TabXY(x, y) => {
                    let a = self.expr(ub, x);
                    let a = self.num(ub, a);
                    let b = self.expr(ub, y);
                    let b = self.num(ub, b);
                    ub.emit(Inst::Do("rb_print_tabxy", vec![Operand::T(a), Operand::T(b)]));
                }
                PrintItem::Spc(x) => {
                    let t = self.expr(ub, x);
                    let n = self.num(ub, t);
                    ub.emit(Inst::Do("rb_print_spc", vec![Operand::T(n)]));
                }
            }
        }
    }

    fn assign(&mut self, ub: &mut UB, v: &ast::Var, op: AssignOp, e: &Expr) {
        let id = self.var(v);
        let kind = self.p.vars[id as usize].kind;
        // `=` makes its variable before it evaluates the value (§4.2.2)
        if op == AssignOp::Set {
            ub.emit(Inst::Create(id));
        }
        let t = self.expr(ub, e);
        match (kind, op) {
            (VarKind::Str, AssignOp::Set) => {
                let s = self.str_(ub, t);
                ub.emit(Inst::Store(id, s));
            }
            (VarKind::Str, AssignOp::Add) => {
                let s = self.str_(ub, t);
                let cur = ub.let_(Ty::Str, Rv::Load(id));
                let r = ub.op(Ty::Str, "rb_cat", vec![Operand::T(cur), Operand::T(s)]);
                ub.emit(Inst::Store(id, r));
            }
            (VarKind::Str, AssignOp::Sub) => {
                ub.raise(6, NUM_NEEDED);
            }
            (VarKind::AtPct, AssignOp::Set) if t.ty == Ty::Str => {
                ub.emit(Inst::Do("rb_atpct_set_str", vec![Operand::T(t)]));
            }
            (_, AssignOp::Set) => {
                let n = self.num(ub, t);
                ub.emit(Inst::Store(id, n));
            }
            (_, _) => {
                let n = self.num(ub, t);
                // An integer variable converts the value first (§3.7.3)
                let n = if kind == VarKind::Real { n } else { ub.op(Ty::Num, "rb_toint_num", vec![Operand::T(n)]) };
                let cur = ub.let_(Ty::Num, Rv::Load(id));
                let f = if op == AssignOp::Add { "rb_add" } else { "rb_sub" };
                let r = ub.op(Ty::Num, f, vec![Operand::T(cur), Operand::T(n)]);
                ub.emit(Inst::Store(id, r));
            }
        }
    }

    /// Assignment to an element. Its subscripts are evaluated first, then
    /// the value (§5.2.5). `+=` reads the element after the value.
    fn assign_elem(&mut self, ub: &mut UB, v: &ast::Var, subs: &[Expr], op: AssignOp, e: &Expr) {
        let a = self.arr(v);
        let idx = self.elem_index(ub, a, subs);
        let t = self.expr(ub, e);
        if v.kind == ast::VarKind::Str {
            let s = self.str_(ub, t);
            let s = match op {
                AssignOp::Set => s,
                AssignOp::Add => {
                    let cur = ub.op(Ty::Str, "rb_arr_get_str", vec![Operand::Arr(a), Operand::T(idx)]);
                    ub.op(Ty::Str, "rb_cat", vec![Operand::T(cur), Operand::T(s)])
                }
                AssignOp::Sub => {
                    ub.raise(6, NUM_NEEDED);
                    return;
                }
            };
            ub.emit(Inst::Do("rb_arr_set_str", vec![Operand::Arr(a), Operand::T(idx), Operand::T(s)]));
            return;
        }
        let n = self.num(ub, t);
        let n = match op {
            AssignOp::Set => n,
            _ => {
                let n = if v.kind == ast::VarKind::Int { ub.op(Ty::Num, "rb_toint_num", vec![Operand::T(n)]) } else { n };
                let cur = ub.op(Ty::Num, "rb_arr_get_num", vec![Operand::Arr(a), Operand::T(idx)]);
                let f = if op == AssignOp::Add { "rb_add" } else { "rb_sub" };
                ub.op(Ty::Num, f, vec![Operand::T(cur), Operand::T(n)])
            }
        };
        ub.emit(Inst::Do("rb_arr_set_num", vec![Operand::Arr(a), Operand::T(idx), Operand::T(n)]));
    }

    /// An indirection's address. The base and the offset are each an
    /// integer, and they are summed in 32 bits (§12.2.2).
    fn address(&mut self, ub: &mut UB, base: &Expr, off: Option<&Expr>) -> Temp {
        let b = self.expr(ub, base);
        let b = self.num(ub, b);
        match off {
            None => ub.op(Ty::Int, "rb_addr1", vec![Operand::T(b)]),
            Some(o) => {
                let o = self.expr(ub, o);
                let o = self.num(ub, o);
                ub.op(Ty::Int, "rb_addr2", vec![Operand::T(b), Operand::T(o)])
            }
        }
    }

    fn peek(&mut self, ub: &mut UB, kind: ast::Ind, a: Temp) -> Temp {
        match kind {
            ast::Ind::Byte => ub.op(Ty::Num, "rb_peek_byte", vec![Operand::T(a)]),
            ast::Ind::Word => ub.op(Ty::Num, "rb_peek_word", vec![Operand::T(a)]),
            ast::Ind::Real => ub.op(Ty::Num, "rb_peek_real", vec![Operand::T(a)]),
            ast::Ind::Str => ub.op(Ty::Str, "rb_peek_str", vec![Operand::T(a)]),
        }
    }

    /// Assignment to an indirection. Its address is evaluated first, then
    /// the value (§5.2.5). `+=` and `-=` work as for a variable of the same
    /// kind (§12.2.9).
    fn assign_ind(&mut self, ub: &mut UB, kind: ast::Ind, base: &Expr, off: Option<&Expr>, op: AssignOp, e: &Expr) {
        let a = self.address(ub, base, off);
        let t = self.expr(ub, e);
        if kind == ast::Ind::Str {
            let s = self.str_(ub, t);
            let s = match op {
                AssignOp::Set => s,
                AssignOp::Add => {
                    let cur = self.peek(ub, kind, a);
                    ub.op(Ty::Str, "rb_cat", vec![Operand::T(cur), Operand::T(s)])
                }
                AssignOp::Sub => {
                    ub.raise(6, NUM_NEEDED);
                    return;
                }
            };
            ub.emit(Inst::Do("rb_poke_str", vec![Operand::T(a), Operand::T(s)]));
            return;
        }
        let n = self.num(ub, t);
        let n = match op {
            AssignOp::Set => n,
            _ => {
                let n = if kind == ast::Ind::Real { n } else { ub.op(Ty::Num, "rb_toint_num", vec![Operand::T(n)]) };
                let cur = self.peek(ub, kind, a);
                let f = if op == AssignOp::Add { "rb_add" } else { "rb_sub" };
                ub.op(Ty::Num, f, vec![Operand::T(cur), Operand::T(n)])
            }
        };
        let f = match kind {
            ast::Ind::Byte => "rb_poke_byte",
            ast::Ind::Word => "rb_poke_word",
            _ => "rb_poke_real",
        };
        ub.emit(Inst::Do(f, vec![Operand::T(a), Operand::T(n)]));
    }

    fn condition(&mut self, ub: &mut UB, c: &Expr) -> Temp {
        let t = self.expr(ub, c);
        let n = self.num(ub, t);
        ub.op(Ty::Int, "rb_truth", vec![Operand::T(n)])
    }

    // ---- FOR and NEXT (§7.4) ----------------------------------------------

    /// A FOR's slots and its control variable's place, made when the FOR
    /// or one of its NEXTs is first lowered.
    fn for_info(&mut self, ub: &mut UB, for_id: NodeId) -> (u32, u32, bool, ForPlace) {
        if let (Some(&(a, b, int)), Some(&pl)) = (ub.for_slots.get(&for_id), ub.for_places.get(&for_id)) {
            return (a, b, int, pl);
        }
        let target = match &self.f.nodes[for_id].p.stmt {
            Stmt::For { var, .. } => var.clone(),
            _ => unreachable!(),
        };
        // An integer variable, `!` or `?` makes an integer loop (§7.4.5)
        let (place, int) = match &target {
            Target::Var(v) => {
                let id = self.var(v);
                (ForPlace::Var(id), self.p.vars[id as usize].kind != VarKind::Real)
            }
            Target::Elem(v, _) => {
                let a = self.arr(v);
                ub.slots.push(Ty::Int);
                (ForPlace::Elem(a, ub.slots.len() as u32 - 1), v.kind == ast::VarKind::Int)
            }
            Target::Ind(k, ..) => {
                ub.slots.push(Ty::Int);
                (ForPlace::Ind(*k, ub.slots.len() as u32 - 1), *k != ast::Ind::Real)
            }
        };
        let ty = if int { Ty::Num } else { Ty::Real };
        ub.slots.push(ty);
        ub.slots.push(ty);
        let a = ub.slots.len() as u32 - 2;
        ub.for_slots.insert(for_id, (a, a + 1, int));
        ub.for_places.insert(for_id, place);
        (a, a + 1, int, place)
    }

    /// The control variable's value. With the quirk for-byte-word on, NEXT
    /// reads a `?` variable as four bytes (§7.4.3).
    fn for_load(&mut self, ub: &mut UB, place: ForPlace, word: bool) -> Temp {
        match place {
            ForPlace::Var(v) => ub.let_(Ty::Num, Rv::Load(v)),
            ForPlace::Elem(a, s) => {
                let idx = ub.let_(Ty::Int, Rv::Slot(s));
                ub.op(Ty::Num, "rb_arr_get_num", vec![Operand::Arr(a), Operand::T(idx)])
            }
            ForPlace::Ind(k, s) => {
                let addr = ub.let_(Ty::Int, Rv::Slot(s));
                let k = if word && k == ast::Ind::Byte { ast::Ind::Word } else { k };
                self.peek(ub, k, addr)
            }
        }
    }

    fn for_store(&mut self, ub: &mut UB, place: ForPlace, v: Temp, word: bool) {
        match place {
            ForPlace::Var(var) => ub.emit(Inst::Store(var, v)),
            ForPlace::Elem(a, s) => {
                let idx = ub.let_(Ty::Int, Rv::Slot(s));
                ub.emit(Inst::Do("rb_arr_set_num", vec![Operand::Arr(a), Operand::T(idx), Operand::T(v)]));
            }
            ForPlace::Ind(k, s) => {
                let addr = ub.let_(Ty::Int, Rv::Slot(s));
                let k = if word && k == ast::Ind::Byte { ast::Ind::Word } else { k };
                self.store_place(ub, &RetTarget::Ind(k, addr), v);
            }
        }
    }

    fn for_(&mut self, ub: &mut UB, id: NodeId, target: &Target, start: &Expr, limit: &Expr, step: Option<&Expr>, next: Dest) {
        // First the place, found once. Then the start, which is assigned.
        // Then the limit and the step (§7.4.4)
        let (ls, ss, int, place) = self.for_info(ub, id);
        match (target, place) {
            (Target::Elem(_, subs), ForPlace::Elem(a, s)) => {
                let idx = self.elem_index(ub, a, subs);
                ub.emit(Inst::SetSlot(s, idx));
            }
            (Target::Ind(_, base, off), ForPlace::Ind(_, s)) => {
                let addr = self.address(ub, base, off.as_ref());
                ub.emit(Inst::SetSlot(s, addr));
            }
            _ => {}
        }
        let s = self.expr(ub, start);
        let s = self.num(ub, s);
        self.for_store(ub, place, s, false);
        let conv = if int { "rb_toint_num" } else { "rb_tor" };
        let lt = if int { Ty::Num } else { Ty::Real };
        let l = self.expr(ub, limit);
        let l = self.num(ub, l);
        let l = ub.op(lt, conv, vec![Operand::T(l)]);
        ub.emit(Inst::SetSlot(ls, l));
        let st = match step {
            Some(e) => {
                let t = self.expr(ub, e);
                self.num(ub, t)
            }
            None => ub.op(Ty::Num, "rb_i", vec![Operand::Imm(1)]),
        };
        let st = ub.op(lt, conv, vec![Operand::T(st)]);
        ub.emit(Inst::SetSlot(ss, st));
        let zero = ub.op(Ty::Int, if int { "rb_step_zero_int" } else { "rb_step_zero_real" }, vec![Operand::T(st)]);
        let err = ub.block();
        ub.blocks[err as usize].term = Term::Raise(35, "The step cannot be zero");
        let body = self.dest(ub, next);
        ub.end(Term::Branch(zero, err, body));
    }

    fn next_(&mut self, ub: &mut UB, id: NodeId, ctx: &Ctx, vars: &[Option<ast::Var>], next: Dest) {
        if let Some(Some(v)) = vars.first() {
            let var = self.var(v);
            ub.emit(Inst::MustExist(var, 16, "Syntax error"));
        }
        let steps = match self.f.match_next(id, ctx, vars) {
            Ok(s) => s,
            Err((n, m)) => {
                self.discard(ub, ctx, 0);
                ub.raise(n, m);
                return;
            }
        };
        if let Some(&(_, d)) = steps.first() {
            self.discard(ub, ctx, d + 1);
        }
        for (i, &(for_id, _)) in steps.iter().enumerate() {
            let (ls, ss, int, place) = self.for_info(ub, for_id);
            let word = self.p.quirks & FOR_BYTE_WORD != 0;
            let body = self.dest(ub, self.f.next(for_id));
            let cur = self.for_load(ub, place, word);
            let lim = ub.let_(if int { Ty::Num } else { Ty::Real }, Rv::Slot(ls));
            let stp = ub.let_(if int { Ty::Num } else { Ty::Real }, Rv::Slot(ss));
            let last = i + 1 == steps.len();
            let cont = if last { self.dest(ub, next) } else { ub.block() };
            if int {
                // 0 means the sum overflows and the loop ends. 1 means the
                // loop goes round again. 2 means the loop ends
                let flag = ub.op(Ty::Int, "rb_for_int", vec![Operand::T(cur), Operand::T(stp), Operand::T(lim)]);
                let store = ub.block();
                ub.end(Term::Branch(flag, store, cont));
                ub.cur = store;
                let nv = ub.op(Ty::Num, "rb_for_int_add", vec![Operand::T(cur), Operand::T(stp)]);
                self.for_store(ub, place, nv, word);
                let again = ub.op(Ty::Int, "rb_is_one", vec![Operand::T(flag)]);
                ub.end(Term::Branch(again, body, cont));
            } else {
                let nv = ub.op(Ty::Num, "rb_for_real_add", vec![Operand::T(cur), Operand::T(stp)]);
                self.for_store(ub, place, nv, word);
                let again = ub.op(Ty::Int, "rb_for_real_again", vec![Operand::T(nv), Operand::T(stp), Operand::T(lim)]);
                ub.end(Term::Branch(again, body, cont));
            }
            ub.cur = cont;
            if last {
                return;
            }
        }
    }

    // ---- CASE (§7.7) ------------------------------------------------------

    fn case(&mut self, ub: &mut UB, id: NodeId, e: &Expr) {
        let sel = self.expr(ub, e);
        let sel = if sel.ty == Ty::Any { self.num(ub, sel) } else { sel };
        let (whens, end) = self.f.case_scan(id);
        for w in whens {
            let values = match &self.f.nodes[w].p.stmt {
                Stmt::When(v) => v.clone(),
                _ => continue,
            };
            let hit = self.dest(ub, self.f.after_when(w));
            for v in &values {
                let t = self.expr(ub, v);
                let eq = if sel.ty == Ty::Str {
                    let s = self.str_(ub, t);
                    ub.op(Ty::Int, "rb_str_same", vec![Operand::T(sel), Operand::T(s)])
                } else {
                    let n = self.num(ub, t);
                    ub.op(Ty::Int, "rb_num_same", vec![Operand::T(sel), Operand::T(n)])
                };
                let miss = ub.block();
                ub.end(Term::Branch(eq, hit, miss));
                ub.cur = miss;
            }
        }
        self.goto(ub, end);
    }

    // ---- ON (§7.8.7-7.8.9) ---------------------------------------------

    fn on(&mut self, ub: &mut UB, id: NodeId, index: &Expr, gosub: bool, lines: &[u16], has_else: bool) {
        let t = self.expr(ub, index);
        let n = self.num(ub, t);
        let k = ub.op(Ty::Int, "rb_toint", vec![Operand::T(n)]);
        let ret = if gosub { Some(self.dest(ub, self.f.on_return(id))) } else { None };
        for (i, &ln) in lines.iter().enumerate() {
            let is = ub.op(Ty::Int, "rb_int_is", vec![Operand::T(k), Operand::Imm(i as i64 + 1)]);
            let go = ub.block();
            let miss = ub.block();
            ub.end(Term::Branch(is, go, miss));
            ub.cur = go;
            if let Some(r) = ret {
                let rk = ub.returns.len() as u32;
                ub.returns.push((rk, r));
                ub.emit(Inst::PushReturn(rk));
            }
            let d = self.f.line_dest(ln);
            self.goto(ub, d);
            ub.cur = miss;
        }
        if has_else {
            self.goto(ub, self.f.on_else(id));
        } else {
            ub.raise(40, "ON range");
        }
    }

    // ---- Calls (chapter 8) ----------------------------------------------

    /// A call of `PROC`name or `FN`name. It takes the save mark, saves the
    /// formals, and evaluates the arguments left to right (§8.3.4-8.3.5).
    fn call(&mut self, ub: &mut UB, is_fn: bool, name: &[u8], args: &[Expr]) -> Option<Temp> {
        let unit = match self.routines.get(&(is_fn, name.to_vec())) {
            Some(&u) => u,
            None => {
                for a in args {
                    self.expr(ub, a);
                }
                ub.raise(29, "No such function/procedure");
                return if is_fn { Some(ub.temp(Ty::Any)) } else { None };
            }
        };
        // A library's routine exists once its LIBRARY has run (§15.3.2)
        let seg = self.f.seg_of[self.f.units[unit].def.as_ref().unwrap().0];
        if seg > 0 {
            ub.emit(Inst::Do("rb_lib_check", vec![Operand::Imm(seg as i64)]));
        }
        let params: Vec<ast::Param> = self.f.units[unit].def.as_ref().unwrap().1.params.clone();
        if params.len() != args.len() {
            ub.raise(31, "Arguments of function/procedure incorrect");
            return if is_fn { Some(ub.temp(Ty::Any)) } else { None };
        }
        let mark = ub.op(Ty::Int, "rb_save_mark", vec![]);
        // The formals' old values are saved before the arguments are
        // evaluated (§8.3.5)
        for p in &params {
            if p.kind == ast::ParamKind::Array {
                let a = self.arr(&p.var);
                ub.emit(Inst::SaveArr(a));
            } else {
                let v = self.var(&p.var);
                ub.emit(Inst::Save(v, 0));
            }
        }
        let mut temps = vec![mark];
        let mut targets: Vec<(RetTarget, Ty)> = Vec::new();
        for (a, p) in args.iter().zip(&params) {
            match p.kind {
                ast::ParamKind::Array => {
                    let t = match a {
                        Expr::ArrayRef(v) if v.kind == p.var.kind => {
                            let arr = self.arr(v);
                            ub.emit(Inst::Do("rb_arr_param", vec![Operand::Arr(arr)]));
                            ub.op(Ty::Arr, "rb_arr_id", vec![Operand::Arr(arr)])
                        }
                        Expr::ArrayRef(_) => {
                            ub.raise(6, "Array type mismatch as parameter");
                            ub.temp(Ty::Arr)
                        }
                        _ => {
                            ub.raise(31, "Invalid array actual parameter");
                            ub.temp(Ty::Arr)
                        }
                    };
                    temps.push(t);
                }
                ast::ParamKind::Value => {
                    let v = self.var(&p.var);
                    let t = self.expr(ub, a);
                    let t = if self.var_ty(v) == Ty::Str { self.str_(ub, t) } else { self.num(ub, t) };
                    temps.push(t);
                }
                ast::ParamKind::Return => {
                    // The actual's address is found now (§8.4.2). The formal
                    // starts with its value (§8.4.1)
                    let v = self.var(&p.var);
                    let ty = self.var_ty(v);
                    // The actual's kind must be the formal's (§8.3.3)
                    let actual_str = match a {
                        Expr::Var(av) | Expr::Elem(av, _) => Some(av.kind == ast::VarKind::Str),
                        Expr::Ind(k, ..) => Some(*k == ast::Ind::Str),
                        _ => None,
                    };
                    if let Some(s) = actual_str {
                        if s != (ty == Ty::Str) {
                            ub.raise(6, if s { NUM_NEEDED } else { STR_NEEDED });
                            temps.push(ub.temp(ty));
                            targets.push((RetTarget::None, ty));
                            continue;
                        }
                    }
                    let (target, t) = match a {
                        Expr::Var(av) => {
                            let id = self.var(av);
                            ub.emit(Inst::Create(id));
                            self.place(ub, &Target::Var(av.clone()))
                        }
                        Expr::Elem(av, subs) => self.place(ub, &Target::Elem(av.clone(), subs.clone())),
                        Expr::Ind(kind, base, off) => {
                            self.place(ub, &Target::Ind(*kind, (**base).clone(), off.as_deref().cloned()))
                        }
                        Expr::Bin(..) if starts_with_variable(a) => {
                            ub.raise(31, "Arguments of function/procedure incorrect");
                            (RetTarget::None, ub.temp(ty))
                        }
                        _ => {
                            ub.raise(31, "Invalid RETURN actual parameter");
                            (RetTarget::None, ub.temp(ty))
                        }
                    };
                    let t = if ty == Ty::Str { self.str_(ub, t) } else { self.num(ub, t) };
                    temps.push(t);
                    targets.push((target, ty));
                }
            }
        }
        let buf = if targets.is_empty() {
            None
        } else {
            let b = ub.temp(Ty::RetBuf(targets.len() as u32));
            temps.push(b);
            Some(b)
        };
        let result = if is_fn {
            Some(ub.let_(Ty::Any, Rv::Call(unit, temps)))
        } else {
            ub.emit(Inst::CallProc(unit, temps));
            None
        };
        // Each result goes to its actual, the last first (§8.4.3)
        if let Some(buf) = buf {
            for (k, (target, ty)) in targets.iter().enumerate().rev() {
                let get = if *ty == Ty::Str { "rb_ret_str" } else { "rb_ret_num" };
                let v = ub.op(*ty, get, vec![Operand::T(buf), Operand::Imm(k as i64)]);
                self.store_place(ub, target, v);
            }
        }
        result
    }

    /// A variable, element or indirection. This gives where it is, and its
    /// value now. A variable that does not exist raises error 26.
    fn place(&mut self, ub: &mut UB, t: &Target) -> (RetTarget, Temp) {
        match t {
            Target::Var(v) => {
                let id = self.var(v);
                (RetTarget::Var(id), ub.let_(self.var_ty(id), Rv::Load(id)))
            }
            Target::Elem(v, subs) => {
                let arr = self.arr(v);
                let idx = self.elem_index(ub, arr, subs);
                let s = v.kind == ast::VarKind::Str;
                let t = if s {
                    ub.op(Ty::Str, "rb_arr_get_str", vec![Operand::Arr(arr), Operand::T(idx)])
                } else {
                    ub.op(Ty::Num, "rb_arr_get_num", vec![Operand::Arr(arr), Operand::T(idx)])
                };
                (RetTarget::Elem(arr, idx, s), t)
            }
            Target::Ind(kind, base, off) => {
                let addr = self.address(ub, base, off.as_ref());
                let t = self.peek(ub, *kind, addr);
                (RetTarget::Ind(*kind, addr), t)
            }
        }
    }

    /// Where a variable, element or indirection is, so that it can be
    /// assigned. A variable that does not exist is made (§4.2.1).
    fn place_addr(&mut self, ub: &mut UB, t: &Target) -> RetTarget {
        match t {
            Target::Var(v) => {
                let id = self.var(v);
                ub.emit(Inst::Create(id));
                RetTarget::Var(id)
            }
            Target::Elem(v, subs) => {
                let arr = self.arr(v);
                let idx = self.elem_index(ub, arr, subs);
                RetTarget::Elem(arr, idx, v.kind == ast::VarKind::Str)
            }
            Target::Ind(kind, base, off) => {
                let addr = self.address(ub, base, off.as_ref());
                RetTarget::Ind(*kind, addr)
            }
        }
    }

    /// A channel, converted to an integer (§13.1.2).
    fn channel_value(&mut self, ub: &mut UB, c: &Expr) -> Temp {
        let t = self.expr(ub, c);
        let n = self.num(ub, t);
        ub.op(Ty::Num, "rb_toint_num", vec![Operand::T(n)])
    }

    fn place_is_str(&self, p: &RetTarget) -> bool {
        match p {
            RetTarget::Var(id) => self.var_ty(*id) == Ty::Str,
            RetTarget::Elem(_, _, s) => *s,
            RetTarget::Ind(k, _) => *k == ast::Ind::Str,
            RetTarget::None => false,
        }
    }

    /// Assigns to a place, converting as `=` does (§4.3.2).
    fn store_place(&mut self, ub: &mut UB, p: &RetTarget, v: Temp) {
        let s = self.place_is_str(p);
        let v = if s { self.str_(ub, v) } else { self.num(ub, v) };
        match p {
            RetTarget::Var(id) => ub.emit(Inst::Store(*id, v)),
            RetTarget::Elem(arr, idx, _) => {
                let f = if s { "rb_arr_set_str" } else { "rb_arr_set_num" };
                ub.emit(Inst::Do(f, vec![Operand::Arr(*arr), Operand::T(*idx), Operand::T(v)]));
            }
            RetTarget::Ind(kind, addr) => {
                let f = match kind {
                    ast::Ind::Byte => "rb_poke_byte",
                    ast::Ind::Word => "rb_poke_word",
                    ast::Ind::Real => "rb_poke_real",
                    ast::Ind::Str => "rb_poke_str",
                };
                ub.emit(Inst::Do(f, vec![Operand::T(*addr), Operand::T(v)]));
            }
            RetTarget::None => {}
        }
    }

    /// One variable of READ. Its address is found first, then the item is
    /// read and stored as `=` stores it (§11.2.4).
    fn read(&mut self, ub: &mut UB, t: &Target) {
        match t {
            Target::Var(v) => {
                let id = self.var(v);
                ub.emit(Inst::Create(id));
                let (ty, f) = if self.var_ty(id) == Ty::Str { (Ty::Str, "rb_read_str") } else { (Ty::Num, "rb_read_num") };
                let x = ub.op(ty, f, vec![]);
                ub.emit(Inst::Store(id, x));
            }
            Target::Elem(v, subs) => {
                let a = self.arr(v);
                let idx = self.elem_index(ub, a, subs);
                if v.kind == ast::VarKind::Str {
                    let x = ub.op(Ty::Str, "rb_read_str", vec![]);
                    ub.emit(Inst::Do("rb_arr_set_str", vec![Operand::Arr(a), Operand::T(idx), Operand::T(x)]));
                } else {
                    let x = ub.op(Ty::Num, "rb_read_num", vec![]);
                    ub.emit(Inst::Do("rb_arr_set_num", vec![Operand::Arr(a), Operand::T(idx), Operand::T(x)]));
                }
            }
            Target::Ind(kind, base, off) => {
                let addr = self.address(ub, base, off.as_ref());
                let (ty, f, poke) = match kind {
                    ast::Ind::Byte => (Ty::Num, "rb_read_num", "rb_poke_byte"),
                    ast::Ind::Word => (Ty::Num, "rb_read_num", "rb_poke_word"),
                    ast::Ind::Real => (Ty::Num, "rb_read_num", "rb_poke_real"),
                    ast::Ind::Str => (Ty::Str, "rb_read_str", "rb_poke_str"),
                };
                let x = ub.op(ty, f, vec![]);
                ub.emit(Inst::Do(poke, vec![Operand::T(addr), Operand::T(x)]));
            }
        }
        // The DATA pointer moves past the item once it is stored (§11.2.6)
        ub.emit(Inst::Do("rb_read_done", vec![]));
    }

    /// `A%` to `H%`, for CALL and USR (§16.9).
    fn a_to_h(&mut self, ub: &mut UB) -> Vec<Operand> {
        (b'A'..=b'H')
            .map(|c| {
                let id = self.var(&ast::Var { name: vec![c, b'%'], kind: ast::VarKind::Int });
                Operand::T(ub.let_(Ty::Num, Rv::Load(id)))
            })
            .collect()
    }

    /// One of CALL's parameters, given by its type and address (§14.3.3).
    fn call_param(&mut self, ub: &mut UB, p: &Target) {
        match p {
            Target::Var(v) if v.name == b"@%" => {
                ub.emit(Inst::Do("rb_call_param", vec![Operand::Imm(4), Operand::Raw("(int32_t)(intptr_t)&rb_atpct")]));
            }
            Target::Var(v) => {
                let id = self.var(v);
                ub.emit(Inst::Create(id));
                match v.kind {
                    ast::VarKind::Int => ub.emit(Inst::Do("rb_call_param", vec![Operand::Imm(4), Operand::VarAddr(id)])),
                    ast::VarKind::Real => ub.emit(Inst::Do("rb_call_param", vec![Operand::Imm(8), Operand::VarAddr(id)])),
                    ast::VarKind::Str => ub.emit(Inst::Do("rb_call_str", vec![Operand::VarPtr(id)])),
                }
            }
            Target::Elem(v, subs) => {
                let a = self.arr(v);
                let idx = self.elem_index(ub, a, subs);
                ub.emit(Inst::Do("rb_call_elem", vec![Operand::Arr(a), Operand::T(idx)]));
            }
            Target::Ind(kind, base, off) => {
                let addr = self.address(ub, base, off.as_ref());
                let ty = match kind {
                    ast::Ind::Byte => 0,
                    ast::Ind::Word => 4,
                    ast::Ind::Real => 8,
                    ast::Ind::Str => 0x81,
                };
                ub.emit(Inst::Do("rb_call_param", vec![Operand::Imm(ty), Operand::T(addr)]));
            }
        }
    }

    /// The tables that EVAL and READ look names up in, and the lines and
    /// DATA that RESTORE and READ use.
    fn tables(&mut self) {
        self.p.tables = true;
        // The resident integers and @% exist whether or not the program
        // names them (§4.4)
        for c in b'A'..=b'Z' {
            self.var(&ast::Var { name: vec![c, b'%'], kind: ast::VarKind::Int });
        }
        self.var(&ast::Var { name: b"@%".to_vec(), kind: ast::VarKind::Int });
        let mut fns: Vec<(Vec<u8>, usize)> = self.routines.iter().filter(|((f, _), _)| *f).map(|((_, n), &k)| (n.clone(), k)).collect();
        fns.sort();
        for (name, k) in fns {
            let d = &self.f.units[k].def.as_ref().unwrap().1;
            if self.f.units[k].ctx.is_empty() {
                continue;
            }
            let params = d
                .params
                .clone()
                .iter()
                .map(|p| match p.kind {
                    ast::ParamKind::Value => FnParam::Value(self.var(&p.var)),
                    ast::ParamKind::Return => FnParam::Return(self.var(&p.var)),
                    ast::ParamKind::Array => FnParam::Array,
                })
                .collect();
            self.p.fns.push(FnSym { unit: k, name, params });
        }
        for (i, (n, text)) in self.f.lines.iter().enumerate() {
            self.p.lines.push(*n);
            let mut j = 0;
            while j < text.len() && text[j] == b' ' {
                j += 1;
            }
            if j < text.len() && text[j] == TDATA {
                self.p.data.push((i as u32, text[j + 1..].to_vec()));
            }
        }
    }

    /// A whole-array assignment (§4.7). The runtime does the work.
    fn whole_array(&mut self, ub: &mut UB, a: &ast::ArrVar, v: &ast::WaValue) {
        use ast::WaValue as W;
        let a = Operand::Arr(self.arr(a));
        let op = |o: ast::WaOp| Operand::Imm(o as i64);
        // The target, and an array that is written first, are checked
        // before the value is evaluated
        let first = match v {
            W::OpRight(_, b, _) | W::DotFactor(b, _) => Some(Operand::Arr(self.arr(b))),
            _ => None,
        };
        if !v.exprs().is_empty() {
            let b = first.unwrap_or(Operand::Imm(0));
            ub.emit(Inst::Do("rb_wa_pre", vec![a.clone(), b]));
        }
        match v {
            W::Copy(b) => {
                let b = self.arr(b);
                ub.emit(Inst::Do("rb_wa_copy", vec![a, Operand::Arr(b)]));
            }
            W::Neg(b) => {
                let b = self.arr(b);
                ub.emit(Inst::Do("rb_wa_neg", vec![a, Operand::Arr(b)]));
            }
            W::Op(o, b, c) => {
                let (b, c) = (self.arr(b), self.arr(c));
                ub.emit(Inst::Do("rb_wa_op", vec![a, op(*o), Operand::Arr(b), Operand::Arr(c)]));
            }
            W::OpRight(o, b, f) => {
                let b = self.arr(b);
                let t = self.expr(ub, f);
                let f = self.to_any(ub, t);
                ub.emit(Inst::Do("rb_wa_op_right", vec![a, op(*o), Operand::Arr(b), Operand::T(f)]));
            }
            W::OpLeft(o, f, b) => {
                let b = self.arr(b);
                let t = self.expr(ub, f);
                let f = self.to_any(ub, t);
                ub.emit(Inst::Do("rb_wa_op_left", vec![a, op(*o), Operand::T(f), Operand::Arr(b)]));
            }
            W::Set(f) => {
                let t = self.expr(ub, f);
                let f = self.to_any(ub, t);
                ub.emit(Inst::Do("rb_wa_set", vec![a, Operand::T(f)]));
            }
            W::List(items) => {
                for (k, e) in items.iter().enumerate() {
                    let t = self.expr(ub, e);
                    let f = self.to_any(ub, t);
                    ub.emit(Inst::Do("rb_wa_list", vec![a.clone(), Operand::Imm(k as i64), Operand::T(f)]));
                }
                ub.emit(Inst::Do("rb_wa_list_end", vec![a, Operand::Imm(items.len() as i64)]));
            }
            W::MatMul(b, c) => {
                let (b, c) = (self.arr(b), self.arr(c));
                ub.emit(Inst::Do("rb_wa_matmul", vec![a, Operand::Arr(b), Operand::Arr(c)]));
            }
            W::DotFactor(b, f) => {
                let b = self.arr(b);
                let t = self.expr(ub, f);
                let f = self.to_any(ub, t);
                ub.emit(Inst::Do("rb_wa_op_right", vec![a, Operand::Imm(4), Operand::Arr(b), Operand::T(f)]));
            }
            W::AddEq(e, sub) => {
                let t = self.expr(ub, e);
                let f = self.to_any(ub, t);
                ub.emit(Inst::Do("rb_wa_add_eq", vec![a, Operand::T(f), Operand::Imm(*sub as i64)]));
            }
        }
    }

    // ---- Expressions (chapter 5) ------------------------------------------

    /// A number, from a temporary of any kind. A string raises error 6.
    fn num(&mut self, ub: &mut UB, t: Temp) -> Temp {
        match t.ty {
            Ty::Num => t,
            Ty::Any => ub.op(Ty::Num, "rb_any_num", vec![Operand::T(t)]),
            Ty::Str => {
                ub.raise(6, NUM_NEEDED);
                ub.temp(Ty::Num)
            }
            Ty::Int => ub.op(Ty::Num, "rb_i", vec![Operand::T(t)]),
            Ty::Real => ub.op(Ty::Num, "rb_r", vec![Operand::T(t)]),
            Ty::Arr | Ty::RetBuf(_) => unreachable!("not a value"),
        }
    }

    fn str_(&mut self, ub: &mut UB, t: Temp) -> Temp {
        match t.ty {
            Ty::Str => t,
            Ty::Any => ub.op(Ty::Str, "rb_any_str", vec![Operand::T(t)]),
            _ => {
                ub.raise(6, STR_NEEDED);
                ub.temp(Ty::Str)
            }
        }
    }

    fn to_any(&mut self, ub: &mut UB, t: Temp) -> Temp {
        match t.ty {
            Ty::Any => t,
            Ty::Str => ub.op(Ty::Any, "rb_any_of_str", vec![Operand::T(t)]),
            _ => {
                let n = self.num(ub, t);
                ub.op(Ty::Any, "rb_any_of_num", vec![Operand::T(n)])
            }
        }
    }

    fn expr(&mut self, ub: &mut UB, e: &Expr) -> Temp {
        match e {
            Expr::Int(k) => ub.op(Ty::Num, "rb_i", vec![Operand::Imm(i64::from(*k))]),
            Expr::Real(text) => {
                let k = self.real_lit(text);
                ub.op(Ty::Num, "rb_lit", vec![Operand::Real(k)])
            }
            Expr::Str(s) => {
                let k = self.str_lit(s);
                ub.op(Ty::Str, "rb_s", vec![Operand::Str(k)])
            }
            Expr::Var(v) => {
                let id = self.var(v);
                let ty = self.var_ty(id);
                ub.let_(ty, Rv::Load(id))
            }
            Expr::Unary(op, a) => {
                let t = self.expr(ub, a);
                match (op, t.ty) {
                    (UnOp::Plus, _) => t,
                    (_, Ty::Str) => {
                        ub.raise(6, NUM_NEEDED);
                        ub.temp(Ty::Num)
                    }
                    (UnOp::Neg, _) => {
                        let n = self.num(ub, t);
                        ub.op(Ty::Num, "rb_neg", vec![Operand::T(n)])
                    }
                    (UnOp::Not, _) => {
                        let n = self.num(ub, t);
                        ub.op(Ty::Num, "rb_not", vec![Operand::T(n)])
                    }
                }
            }
            Expr::Bin(op, a, b) => self.binary(ub, *op, a, b),
            Expr::Func(f, args) => self.func(ub, *f, args),
            Expr::Fn(name, args) => self.call(ub, true, name, args).unwrap(),
            Expr::ArrayRef(_) => {
                ub.raise(26, "Can't use array reference here");
                ub.temp(Ty::Num)
            }
            Expr::DimVar(v) => {
                let id = self.var(v);
                ub.emit(Inst::MustExist(id, 14, "Unknown array in DIM() function"));
                ub.raise(10, "DIM() function needs an array");
                ub.temp(Ty::Num)
            }
            Expr::ArrayFunc(f, v) => {
                let a = Operand::Arr(self.arr(v));
                match f {
                    ast::ArrayFunc::Sum => ub.op(Ty::Any, "rb_wa_sum", vec![a]),
                    ast::ArrayFunc::SumLen => ub.op(Ty::Num, "rb_wa_sumlen", vec![a]),
                    ast::ArrayFunc::Mod => ub.op(Ty::Num, "rb_wa_mod", vec![a]),
                }
            }
            Expr::Elem(v, subs) => {
                let a = self.arr(v);
                let idx = self.elem_index(ub, a, subs);
                if v.kind == ast::VarKind::Str {
                    ub.op(Ty::Str, "rb_arr_get_str", vec![Operand::Arr(a), Operand::T(idx)])
                } else {
                    ub.op(Ty::Num, "rb_arr_get_num", vec![Operand::Arr(a), Operand::T(idx)])
                }
            }
            Expr::Ind(kind, base, off) => {
                let a = self.address(ub, base, off.as_deref());
                self.peek(ub, *kind, a)
            }
            Expr::Pseudo(k) => {
                let f = match k {
                    ast::Pseudo::Page => "rb_page",
                    ast::Pseudo::Top => "rb_top",
                    ast::Pseudo::Lomem => "rb_lomem",
                    ast::Pseudo::Himem => "rb_himem",
                    ast::Pseudo::End => "rb_endp",
                };
                ub.op(Ty::Num, f, vec![])
            }
            Expr::DimOf(v, n) => {
                let a = self.arr(v);
                match n {
                    None => ub.op(Ty::Num, "rb_arr_dims", vec![Operand::Arr(a)]),
                    Some(n) => {
                        let t = self.expr(ub, n);
                        let t = self.num(ub, t);
                        ub.op(Ty::Num, "rb_arr_bound", vec![Operand::Arr(a), Operand::T(t)])
                    }
                }
            }
            Expr::Fail(parts, f) => {
                for p in parts {
                    self.expr(ub, p);
                }
                ub.raise(f.number, f.message);
                ub.temp(Ty::Num)
            }
        }
    }

    /// §5.2.4: a string left operand of an arithmetic operator raises error
    /// 6 before the right operand is evaluated. `+` and the relations
    /// evaluate the right operand before they raise it.
    fn binary(&mut self, ub: &mut UB, op: BinOp, a: &Expr, b: &Expr) -> Temp {
        let l = self.expr(ub, a);
        let str_ok = matches!(op, BinOp::Add | BinOp::Eq | BinOp::Ne | BinOp::Lt | BinOp::Gt | BinOp::Le | BinOp::Ge);
        if l.ty == Ty::Str && !str_ok {
            ub.raise(6, NUM_NEEDED);
            return ub.temp(Ty::Num);
        }
        let r = self.expr(ub, b);
        if l.ty == Ty::Any || r.ty == Ty::Any {
            let la = self.to_any(ub, l);
            let ra = self.to_any(ub, r);
            return ub.op(Ty::Any, "rb_any_bin", vec![Operand::Imm(op as i64), Operand::T(la), Operand::T(ra)]);
        }
        match (l.ty, r.ty) {
            (Ty::Str, Ty::Str) => {
                if op == BinOp::Add {
                    ub.op(Ty::Str, "rb_cat", vec![Operand::T(l), Operand::T(r)])
                } else {
                    ub.op(Ty::Num, "rb_srel", vec![Operand::Imm(op as i64), Operand::T(l), Operand::T(r)])
                }
            }
            (Ty::Str, _) => {
                ub.raise(6, STR_NEEDED);
                ub.temp(Ty::Num)
            }
            (_, Ty::Str) => {
                ub.raise(6, NUM_NEEDED);
                ub.temp(Ty::Num)
            }
            _ => {
                let f = match op {
                    BinOp::Pow => "rb_powr",
                    BinOp::Mul => "rb_mul",
                    BinOp::Div => "rb_div",
                    BinOp::IDiv => "rb_idiv",
                    BinOp::Mod => "rb_imod",
                    BinOp::Add => "rb_add",
                    BinOp::Sub => "rb_sub",
                    BinOp::Shl => "rb_shl",
                    BinOp::Asr => "rb_asr",
                    BinOp::Lsr => "rb_lsr",
                    BinOp::And => "rb_and",
                    BinOp::Or => "rb_or",
                    BinOp::Eor => "rb_eor",
                    _ => {
                        return ub.op(Ty::Num, "rb_nrel", vec![Operand::Imm(op as i64), Operand::T(l), Operand::T(r)]);
                    }
                };
                ub.op(Ty::Num, f, vec![Operand::T(l), Operand::T(r)])
            }
        }
    }

    fn func(&mut self, ub: &mut UB, f: Func, args: &[Expr]) -> Temp {
        use Func::*;
        let numeric = |f: Func| -> Option<&'static str> {
            Some(match f {
                Abs => "rb_f_abs",
                Sgn => "rb_f_sgn",
                Int => "rb_f_int",
                Sqr => "rb_f_sqr",
                Sin => "rb_f_sin",
                Cos => "rb_f_cos",
                Tan => "rb_f_tan",
                Atn => "rb_f_atn",
                Asn => "rb_f_asn",
                Acs => "rb_f_acs",
                Exp => "rb_f_exp",
                Ln => "rb_f_ln",
                Log => "rb_f_log",
                Deg => "rb_f_deg",
                Rad => "rb_f_rad",
                RndArg => "rb_f_rnd",
                _ => return None,
            })
        };
        if let Some(name) = numeric(f) {
            let t = self.expr(ub, &args[0]);
            let n = self.num(ub, t);
            return ub.op(Ty::Num, name, vec![Operand::T(n)]);
        }
        let none = |f: Func| -> Option<&'static str> {
            Some(match f {
                Pi => "rb_f_pi",
                True => "rb_f_true",
                False => "rb_f_false",
                Rnd => "rb_f_rnd0",
                Count => "rb_f_count",
                Err => "rb_f_err",
                Erl => "rb_f_erl",
                Width => "rb_f_width",
                _ => return None,
            })
        };
        if let Some(name) = none(f) {
            return ub.op(Ty::Num, name, vec![]);
        }
        match f {
            ReportS => ub.op(Ty::Str, "rb_f_report", vec![]),
            Openin | Openup | Openout => {
                let t = self.expr(ub, &args[0]);
                let s = self.str_(ub, t);
                let name = match f {
                    Openin => "rb_openin",
                    Openup => "rb_openup",
                    _ => "rb_openout",
                };
                ub.op(Ty::Num, name, vec![Operand::T(s)])
            }
            Bget | Eof | Ext | Ptr | GetDFile => {
                let ch = self.channel_value(ub, &args[0]);
                let (ty, name) = match f {
                    Bget => (Ty::Num, "rb_bget"),
                    Eof => (Ty::Num, "rb_eof"),
                    Ext => (Ty::Num, "rb_ext"),
                    Ptr => (Ty::Num, "rb_ptr"),
                    _ => (Ty::Str, "rb_getdollar_file"),
                };
                ub.op(ty, name, vec![Operand::T(ch)])
            }
            Get | GetD | Time | TimeD | Pos | Vpos | ModeFn | Beat | Beats | Tempo => {
                let (ty, name) = match f {
                    Get => (Ty::Num, "rb_f_get"),
                    GetD => (Ty::Str, "rb_f_getdollar"),
                    Time => (Ty::Num, "rb_time"),
                    TimeD => (Ty::Str, "rb_time_str"),
                    Pos => (Ty::Num, "rb_f_pos"),
                    Vpos => (Ty::Num, "rb_f_vpos"),
                    ModeFn => (Ty::Num, "rb_f_mode"),
                    Beat => (Ty::Num, "rb_f_beat"),
                    Beats => (Ty::Num, "rb_f_beats"),
                    _ => (Ty::Num, "rb_f_tempo"),
                };
                ub.op(ty, name, vec![])
            }
            Inkey | InkeyD | Adval | VduFn => {
                let t = self.expr(ub, &args[0]);
                let n = self.num(ub, t);
                let (ty, name) = match f {
                    Inkey => (Ty::Num, "rb_f_inkey"),
                    InkeyD => (Ty::Str, "rb_f_inkeydollar"),
                    Adval => (Ty::Num, "rb_f_adval"),
                    _ => (Ty::Num, "rb_f_vdu"),
                };
                ub.op(ty, name, vec![Operand::T(n)])
            }
            PointXY | TintXY => {
                let x = self.expr(ub, &args[0]);
                let x = self.num(ub, x);
                let y = self.expr(ub, &args[1]);
                let y = self.num(ub, y);
                ub.op(Ty::Num, if f == PointXY { "rb_f_point" } else { "rb_f_tint" }, vec![Operand::T(x), Operand::T(y)])
            }
            Usr => {
                let t = self.expr(ub, &args[0]);
                let n = self.num(ub, t);
                let n = ub.op(Ty::Num, "rb_toint_num", vec![Operand::T(n)]);
                let mut a = vec![Operand::T(n)];
                a.extend(self.a_to_h(ub));
                ub.op(Ty::Num, "rb_usr", a)
            }
            Eval => {
                self.tables = true;
                let t = self.expr(ub, &args[0]);
                let s = self.str_(ub, t);
                ub.op(Ty::Any, "rb_eval", vec![Operand::T(s)])
            }
            Len | Asc | Val => {
                let t = self.expr(ub, &args[0]);
                let s = self.str_(ub, t);
                let name = match f {
                    Len => "rb_f_len",
                    Asc => "rb_f_asc",
                    _ => "rb_f_val",
                };
                ub.op(Ty::Num, name, vec![Operand::T(s)])
            }
            Chr | Str | StrHex => {
                let t = self.expr(ub, &args[0]);
                let n = self.num(ub, t);
                let name = match f {
                    Chr => "rb_f_chr",
                    Str => "rb_f_str",
                    _ => "rb_f_strhex",
                };
                ub.op(Ty::Str, name, vec![Operand::T(n)])
            }
            Left | Right | Left1 | Right1 | Mid | Mid2 => {
                let t = self.expr(ub, &args[0]);
                let s = self.str_(ub, t);
                let mut ops = vec![Operand::T(s)];
                for a in &args[1..] {
                    let t = self.expr(ub, a);
                    let n = self.num(ub, t);
                    ops.push(Operand::T(n));
                }
                let name = match f {
                    Left => "rb_f_left",
                    Left1 => "rb_f_left1",
                    Right => "rb_f_right",
                    Right1 => "rb_f_right1",
                    Mid => "rb_f_mid",
                    _ => "rb_f_mid2",
                };
                ub.op(Ty::Str, name, ops)
            }
            Instr | Instr3 => {
                let a = self.expr(ub, &args[0]);
                let a = self.str_(ub, a);
                let b = self.expr(ub, &args[1]);
                let b = self.str_(ub, b);
                if f == Instr3 {
                    let c = self.expr(ub, &args[2]);
                    let c = self.num(ub, c);
                    ub.op(Ty::Num, "rb_f_instr3", vec![Operand::T(a), Operand::T(b), Operand::T(c)])
                } else {
                    ub.op(Ty::Num, "rb_f_instr", vec![Operand::T(a), Operand::T(b)])
                }
            }
            StringN => {
                let n = self.expr(ub, &args[0]);
                let n = self.num(ub, n);
                let s = self.expr(ub, &args[1]);
                let s = self.str_(ub, s);
                ub.op(Ty::Str, "rb_f_string", vec![Operand::T(n), Operand::T(s)])
            }
            _ => unreachable!(),
        }
    }
}

/// The names that an assembler block's text may use (§16.1.4), from
/// `start` to the end. It gives each name outside strings and comments,
/// with its kind, and whether it names an array (that is, whether `(`
/// follows it). Routine names after `FN` or `PROC` are not variables.
fn asm_names(t: &[u8], start: usize) -> Vec<(Vec<u8>, ast::VarKind, bool)> {
    use crate::parse::{is_name_char, is_name_start};
    use crate::tokens::{TCONST, TESCCOM, TESCFN, TESCSTMT, TFN, TPROC, TREM};
    let mut out: Vec<(Vec<u8>, ast::VarKind, bool)> = Vec::new();
    let at = |i: usize| *t.get(i).unwrap_or(&13);
    let mut i = start;
    let mut quote = false;
    while i < t.len() {
        let c = t[i];
        if quote {
            quote = c != b'"';
            i += 1;
            continue;
        }
        match c {
            13 => {
                if at(i + 1) == 0xFF {
                    break;
                }
                i += 4;
            }
            b'"' => {
                quote = true;
                i += 1;
            }
            b';' | b'\\' | TREM => {
                while i < t.len() && t[i] != b':' && t[i] != 13 {
                    i += 1;
                }
            }
            TFN | TPROC => {
                i += 1;
                while is_name_char(at(i)) || at(i) == b'@' {
                    i += 1;
                }
            }
            TESCFN | TESCCOM | TESCSTMT => i += 2,
            TCONST => i += 4,
            b'&' => {
                i += 1;
                while at(i).is_ascii_hexdigit() {
                    i += 1;
                }
            }
            b'0'..=b'9' => {
                while at(i).is_ascii_digit() || at(i) == b'.' {
                    i += 1;
                }
                if at(i) == b'E' {
                    i += 1;
                    if at(i) == b'-' || at(i) == b'+' {
                        i += 1;
                    }
                    while at(i).is_ascii_digit() {
                        i += 1;
                    }
                }
            }
            c if is_name_start(c) => {
                let b = i;
                while is_name_char(at(i)) {
                    i += 1;
                }
                let kind = match at(i) {
                    b'%' => {
                        i += 1;
                        ast::VarKind::Int
                    }
                    b'$' => {
                        i += 1;
                        ast::VarKind::Str
                    }
                    _ => ast::VarKind::Real,
                };
                let name = t[b..i].to_vec();
                let array = at(i) == b'(';
                if !out.iter().any(|(n, k, a)| *n == name && *k == kind && *a == array) {
                    out.push((name, kind, array));
                }
            }
            _ => i += 1,
        }
    }
    out
}
