//! Structuring: from a routine's blocks and gotos to `if`, `while` and `do`.
//!
//! A compiled routine is a control-flow graph: basic blocks, each ending in
//! a jump, a two-way branch, a jump table or an exit. Tier 0 writes it out
//! as labels and gotos. This module writes it as structured C instead. It
//! uses the method of Ramsey's "Beyond Relooper" (ICFP 2022), which is how
//! WebAssembly's structured control flow is made from a graph:
//!
//! - Walk the dominator tree. (Block A dominates block B if every path from
//!   the entry to B passes through A.) A block that only one other block
//!   jumps forward to is placed at that jump. It becomes the arm of an `if`,
//!   or the body after a test.
//! - A block that several blocks jump forward to is a merge point. It is
//!   placed after the code of the block that dominates it. A jump to it
//!   leaves the code before it, by falling out, a `break` or a `goto`.
//! - A block that something jumps back to is the head of a loop. The loop
//!   goes around the code the head dominates, and a jump back is a
//!   `continue`.
//!
//! Then the result is written in the shapes C has names for:
//!
//! - `do ... while (c);` when the last thing the loop does is its test;
//! - `while (c) ...` when the first thing it does is the test to leave;
//! - `for (;;)` for any other loop;
//! - `else if` chains;
//! - a guard with no `else`, for a branch where one arm cannot fall through
//!   (a return or a fault).
//!
//! The graph must be reducible (every loop entered only at its head), as
//! assembler written by people almost always is. When it is not, the caller
//! keeps its labels and gotos.

use std::collections::HashSet;

/// A line of a block's code, and the comment beside it. If the code itself
/// is a comment (`/* ...`, or ` * ...` continuing one), it is the source
/// author's comment, on a line of its own.
#[derive(Clone, Debug)]
pub struct Line {
    pub code: String,
    pub comment: String,
}

/// How a block ends.
#[derive(Clone, Debug)]
pub enum Term {
    /// Control leaves at the end of its code: a return, a tail call or a
    /// fault.
    Exit,
    /// Control goes on to a block, by falling through or by a jump.
    Goto(usize),
    /// `if (cond)` go to `then`, else to `else_` (the fall-through). `inv`
    /// is the condition's negation.
    If { cond: String, inv: String, then: usize, else_: usize, comment: String },
    /// A jump table. Case `first + k` goes to `cases[k]`. Any other value
    /// faults, with the code given in `default`.
    Switch { on: String, first: i64, cases: Vec<usize>, default: String, comment: String },
}

#[derive(Clone, Debug)]
pub struct Block {
    pub label: String,
    pub lines: Vec<Line>,
    pub term: Term,
}

/// The routine's structured C, as lines. Returns None if its graph is not
/// reducible, or if a block cannot be reached from the entry.
pub fn structure(blocks: &[Block], entry: usize) -> Option<Vec<String>> {
    structure_with(blocks, entry, None)
}

/// As `structure`, but if `poll` is given, it becomes the first statement
/// of each loop. This is a safe point for the runtime's background work
/// (`--poll-loops`).
pub fn structure_with(blocks: &[Block], entry: usize, poll: Option<&str>) -> Option<Vec<String>> {
    let g = Graph::new(blocks, entry)?;
    let tree = g.do_tree(entry);
    let poll = poll.map(str::to_string);
    // Render twice. The first pass finds which blocks a goto names, and so
    // need a label.
    let mut r = Render { g: &g, stack: vec![], gotos: HashSet::new(), labels: HashSet::new(), broke: HashSet::new(), poll: poll.clone() };
    r.r(&tree, 1, Next::None);
    let labels = std::mem::take(&mut r.gotos);
    let mut r = Render { g: &g, stack: vec![], gotos: HashSet::new(), labels, broke: HashSet::new(), poll };
    let (lines, _) = r.r(&tree, 1, Next::None);
    Some(finish(lines))
}

// ---- the graph ---------------------------------------------------------------

struct Graph<'a> {
    blocks: &'a [Block],
    /// Reverse-postorder number of each block.
    rpo: Vec<usize>,
    idom: Vec<usize>,
    children: Vec<Vec<usize>>,
    merge: Vec<bool>,
    header: Vec<bool>,
}

fn succs(b: &Block) -> Vec<usize> {
    match &b.term {
        Term::Exit => vec![],
        Term::Goto(t) => vec![*t],
        Term::If { then, else_, .. } => vec![*then, *else_],
        Term::Switch { cases, .. } => cases.clone(),
    }
}

impl<'a> Graph<'a> {
    fn new(blocks: &'a [Block], entry: usize) -> Option<Self> {
        let n = blocks.len();
        // Reverse postorder, by an explicit depth-first walk.
        let mut post = vec![];
        let mut seen = vec![false; n];
        let mut stack = vec![(entry, 0usize)];
        seen[entry] = true;
        while let Some(&mut (b, ref mut k)) = stack.last_mut() {
            let s = succs(&blocks[b]);
            if *k < s.len() {
                let t = s[*k];
                *k += 1;
                if !seen[t] {
                    seen[t] = true;
                    stack.push((t, 0));
                }
            } else {
                post.push(b);
                stack.pop();
            }
        }
        if post.len() != n {
            return None; // a block not reachable from the entry
        }
        let order: Vec<usize> = post.into_iter().rev().collect();
        let mut rpo = vec![0; n];
        for (i, &b) in order.iter().enumerate() {
            rpo[b] = i;
        }
        let mut preds = vec![vec![]; n];
        for (b, block) in blocks.iter().enumerate() {
            for t in succs(block) {
                preds[t].push(b);
            }
        }
        // Dominators (Cooper, Harvey and Kennedy).
        let none = usize::MAX;
        let mut idom = vec![none; n];
        idom[entry] = entry;
        loop {
            let mut changed = false;
            for &b in order.iter().skip(1) {
                let mut new = none;
                for &p in &preds[b] {
                    if idom[p] == none {
                        continue;
                    }
                    new = if new == none {
                        p
                    } else {
                        let (mut x, mut y) = (p, new);
                        while x != y {
                            while rpo[x] > rpo[y] {
                                x = idom[x];
                            }
                            while rpo[y] > rpo[x] {
                                y = idom[y];
                            }
                        }
                        x
                    };
                }
                if idom[b] != new {
                    idom[b] = new;
                    changed = true;
                }
            }
            if !changed {
                break;
            }
        }
        let dominates = |a: usize, mut b: usize| loop {
            if a == b {
                return true;
            }
            if b == entry {
                return false;
            }
            b = idom[b];
        };
        let mut merge = vec![false; n];
        let mut header = vec![false; n];
        for b in 0..n {
            let mut forward = 0;
            for &p in &preds[b] {
                if rpo[p] < rpo[b] {
                    forward += 1;
                } else if dominates(b, p) {
                    header[b] = true;
                } else {
                    return None; // a jump back into a loop's middle: irreducible
                }
            }
            merge[b] = forward >= 2;
        }
        let mut children = vec![vec![]; n];
        for &b in order.iter().skip(1) {
            children[idom[b]].push(b);
        }
        Some(Graph { blocks, rpo, idom, children, merge, header })
    }

    // ---- Ramsey's translation ----

    fn do_tree(&self, x: usize) -> S {
        let mut ys: Vec<usize> = self.children[x].iter().copied().filter(|&y| self.merge[y]).collect();
        // The merge point last in reverse postorder is the outermost.
        ys.sort_by_key(|&y| std::cmp::Reverse(self.rpo[y]));
        let code = self.node_within(x, &ys);
        if self.header[x] {
            S::Loop { head: x, body: Box::new(code) }
        } else {
            code
        }
    }

    fn node_within(&self, x: usize, ys: &[usize]) -> S {
        match ys.split_first() {
            Some((&y, rest)) => {
                S::Seq(vec![S::Block { follow: y, body: Box::new(self.node_within(x, rest)) }, self.do_tree(y)])
            }
            None => S::Seq(vec![S::Code(x), self.term(x)]),
        }
    }

    fn term(&self, x: usize) -> S {
        match &self.blocks[x].term {
            Term::Exit => S::Exit,
            Term::Goto(t) => self.do_branch(x, *t),
            Term::If { cond, inv, then, else_, comment } => {
                if then == else_ {
                    return S::Seq(vec![S::Comment(comment.clone()), self.do_branch(x, *then)]);
                }
                S::If {
                    cond: cond.clone(),
                    inv: inv.clone(),
                    then: Box::new(self.do_branch(x, *then)),
                    else_: Box::new(self.do_branch(x, *else_)),
                    comment: comment.clone(),
                }
            }
            Term::Switch { on, first, cases, default, comment } => {
                // A case that jumps to the same place as another case
                // jumps to a merge point, which is reached with a goto. A
                // case whose target no other case reaches is written in
                // place.
                let arms = cases.iter().map(|&t| self.do_branch(x, t)).collect();
                S::Switch { on: on.clone(), first: *first, cases: arms, default: default.clone(), comment: comment.clone() }
            }
        }
    }

    fn do_branch(&self, x: usize, t: usize) -> S {
        if self.rpo[t] <= self.rpo[x] {
            S::Br { target: t, back: true }
        } else if self.merge[t] {
            S::Br { target: t, back: false }
        } else {
            debug_assert_eq!(self.idom[t], x);
            self.do_tree(t)
        }
    }
}

/// The structured routine, before it is written out.
#[derive(Debug)]
enum S {
    Seq(Vec<S>),
    /// A block's own code.
    Code(usize),
    Comment(String),
    /// Control ends here: the code before it was a return or a fault.
    Exit,
    If { cond: String, inv: String, then: Box<S>, else_: Box<S>, comment: String },
    /// A loop around the code at `head`. A jump back is a `continue`.
    Loop { head: usize, body: Box<S> },
    /// Code, then the merge point `follow`. A jump to `follow` leaves the
    /// code.
    Block { follow: usize, body: Box<S> },
    Br { target: usize, back: bool },
    Switch { on: String, first: i64, cases: Vec<S>, default: String, comment: String },
}

/// Whether `s` jumps to `target` anywhere.
fn mentions(s: &S, target: usize) -> bool {
    match s {
        S::Seq(v) => v.iter().any(|x| mentions(x, target)),
        S::If { then, else_, .. } => mentions(then, target) || mentions(else_, target),
        S::Loop { body, .. } | S::Block { body, .. } => mentions(body, target),
        S::Br { target: t, .. } => *t == target,
        S::Switch { cases, .. } => cases.iter().any(|x| mentions(x, target)),
        S::Code(_) | S::Comment(_) | S::Exit => false,
    }
}

// ---- writing it out ----------------------------------------------------------------

/// What control reaches by falling off the end of a piece of code.
#[derive(Clone, Copy, PartialEq, Debug)]
enum Next {
    /// Nothing that can be named, so every way out must be written.
    None,
    /// The top of the loop headed by a block.
    Top(usize),
    /// A merge point's code, which follows.
    Merge(usize),
}

/// What a `break` or `continue` would leave.
#[derive(Clone, Copy)]
enum Brk {
    /// A loop, and what follows it. `after` is `Next::None` where a `break`
    /// would not reach that, as with a `do` loop whose exit code is placed
    /// after it.
    Loop { head: usize, after: Next },
    Switch,
}

#[derive(Clone, Debug)]
struct Out {
    indent: usize,
    code: String,
    comment: String,
}

fn out(indent: usize, code: impl Into<String>, comment: impl Into<String>) -> Out {
    Out { indent, code: code.into(), comment: comment.into() }
}

struct Render<'a> {
    g: &'a Graph<'a>,
    stack: Vec<Brk>,
    /// Blocks that some `goto` names. On the second pass, `labels` holds
    /// those the first pass found, which are the blocks to label.
    gotos: HashSet<usize>,
    labels: HashSet<usize>,
    /// Loops that something breaks out of. These are the loops that can
    /// end.
    broke: HashSet<usize>,
    /// The statement to put first in each loop, if any. It is the safe
    /// point for `--poll-loops`.
    poll: Option<String>,
}

impl<'a> Render<'a> {
    /// The C for `s` at indentation `d`, and whether control can fall off
    /// its end on to `next`.
    fn r(&mut self, s: &S, d: usize, next: Next) -> (Vec<Out>, bool) {
        match s {
            S::Seq(items) => {
                let mut v = vec![];
                let mut falls = true;
                for (i, item) in items.iter().enumerate() {
                    let n = if i + 1 == items.len() { next } else { Next::None };
                    let (lines, f) = self.r(item, d, n);
                    v.extend(lines);
                    falls = f;
                }
                (v, falls)
            }
            S::Code(x) => {
                let b = &self.g.blocks[*x];
                let mut v = vec![];
                if self.labels.contains(x) {
                    v.push(out(d.saturating_sub(1), format!("{}:", b.label), ""));
                }
                for l in &b.lines {
                    v.push(out(d, l.code.clone(), l.comment.clone()));
                }
                (v, true)
            }
            S::Comment(c) => (if c.is_empty() { vec![] } else { vec![out(d, "", c.clone())] }, true),
            S::Exit => (vec![], false),
            S::Block { follow, body } => self.r(body, d, Next::Merge(*follow)),
            S::Br { target, back } => self.br(*target, *back, d, next),
            S::If { cond, inv, then, else_, comment } => self.if_(cond, inv, then, else_, comment, d, next),
            S::Loop { head, body } => self.loop_(*head, body, d, next),
            S::Switch { on, first, cases, default, comment } => {
                self.stack.push(Brk::Switch);
                let mut v = vec![out(d, format!("switch ({on}) {{"), comment.clone())];
                for (k, c) in cases.iter().enumerate() {
                    let (lines, _) = self.r(c, d + 1, Next::None);
                    v.push(out(d, format!("case {}:", first + k as i64), ""));
                    v.extend(lines);
                }
                v.push(out(d, format!("default: {default}"), ""));
                v.push(out(d, "}", ""));
                self.stack.pop();
                (v, false)
            }
        }
    }

    fn br(&mut self, t: usize, back: bool, d: usize, next: Next) -> (Vec<Out>, bool) {
        // Falling off the end here reaches the target anyway.
        if (back && next == Next::Top(t)) || (!back && next == Next::Merge(t)) {
            return (vec![], true);
        }
        let innermost_loop = self.stack.iter().rev().find_map(|b| match b {
            Brk::Loop { head, after } => Some((*head, *after)),
            Brk::Switch => None,
        });
        let in_switch = matches!(self.stack.last(), Some(Brk::Switch));
        if back {
            if innermost_loop.map(|l| l.0) == Some(t) {
                return (vec![out(d, "continue;", "")], false);
            }
        } else if !in_switch {
            if let Some((head, after)) = innermost_loop {
                if matches!(self.stack.last(), Some(Brk::Loop { .. })) && after == Next::Merge(t) {
                    self.broke.insert(head);
                    return (vec![out(d, "break;", "")], false);
                }
            }
        }
        self.gotos.insert(t);
        (vec![out(d, format!("goto {};", self.g.blocks[t].label), "")], false)
    }

    #[allow(clippy::too_many_arguments)]
    fn if_(&mut self, cond: &str, inv: &str, then: &S, else_: &S, comment: &str, d: usize, next: Next) -> (Vec<Out>, bool) {
        let (t, tf) = self.r(then, d + 1, next);
        let (e, ef) = self.r(else_, d + 1, next);
        let mut v = vec![];
        let falls = match (t.is_empty(), e.is_empty()) {
            (true, true) => {
                if !comment.is_empty() {
                    v.push(out(d, "", comment));
                }
                true
            }
            (false, true) => {
                v.extend(if_block(d, cond, t, comment));
                true
            }
            (true, false) => {
                v.extend(if_block(d, inv, e, comment));
                true
            }
            (false, false) => {
                if !tf {
                    // A guard. The arm that cannot fall through goes first,
                    // with no `else`.
                    v.extend(if_block(d, cond, t, comment));
                    v.extend(dedent(e));
                    ef
                } else if !ef {
                    v.extend(if_block(d, inv, e, comment));
                    v.extend(dedent(t));
                    tf
                } else {
                    v.push(out(d, format!("if ({cond}) {{"), comment));
                    v.extend(t);
                    if single_if(&e, d + 1) {
                        let mut e = dedent(e);
                        e[0].code = format!("}} else {}", e[0].code);
                        v.extend(e);
                        if !v.last().is_some_and(|l| l.indent == d && l.code.starts_with('}')) {
                            v.push(out(d, "}", ""));
                        }
                    } else {
                        v.push(out(d, "} else {", ""));
                        v.extend(e);
                        v.push(out(d, "}", ""));
                    }
                    true
                }
            }
        };
        (v, falls)
    }

    fn poll_line(&self, d: usize) -> Option<Out> {
        self.poll.as_ref().map(|p| out(d, p.clone(), ""))
    }

    fn loop_(&mut self, head: usize, body: &S, d: usize, next: Next) -> (Vec<Out>, bool) {
        let items = flatten(body);
        let back = |s: &S| matches!(s, S::Br { target, back: true } if *target == head);
        // `do { ... } while (c);`: the last thing the loop does is its test.
        // The code for leaving the loop goes after it, so a `break` would
        // not reach the place it should. Jumps out of the loop are gotos.
        if let Some((S::If { cond, inv, then, else_, comment }, prefix)) = items.split_last() {
            let (stay, exit) = if back(then) {
                (cond.as_str(), &**else_)
            } else if back(else_) {
                (inv.as_str(), &**then)
            } else {
                ("", &**then)
            };
            if !stay.is_empty() && !prefix.iter().any(|p| mentions(p, head)) && !mentions(exit, head) {
                self.stack.push(Brk::Loop { head, after: Next::None });
                let mut v = vec![out(d, "do {", "")];
                v.extend(self.poll_line(d + 1));
                for p in prefix {
                    let (lines, _) = self.r(p, d + 1, Next::None);
                    v.extend(lines);
                }
                self.stack.pop();
                v.push(out(d, format!("}} while ({stay});"), comment.clone()));
                let (after, f) = self.r(exit, d, next);
                v.extend(after);
                return (v, f);
            }
        }
        // `while (c) { ... }`: the first thing the loop does is its test,
        // and one arm of the test leaves the loop.
        if let [S::Code(x), S::If { cond, inv, then, else_, comment }] = items.as_slice() {
            let b = &self.g.blocks[*x];
            let (stay, rest, exit) = if !mentions(then, head) && mentions(else_, head) {
                (inv.as_str(), &**else_, &**then)
            } else if !mentions(else_, head) && mentions(then, head) {
                (cond.as_str(), &**then, &**else_)
            } else {
                ("", &**then, &**else_)
            };
            if *x == head && b.lines.iter().all(|l| l.code.is_empty() || note(&l.code)) && !stay.is_empty() {
                // A `break` reaches what follows the loop only if nothing
                // is placed in between.
                let plain = matches!(exit, S::Br { target, back: false } if next == Next::Merge(*target));
                let mut v = vec![];
                if self.labels.contains(x) {
                    v.push(out(d.saturating_sub(1), format!("{}:", b.label), ""));
                }
                for l in &b.lines {
                    v.push(out(d, l.code.clone(), l.comment.clone()));
                }
                self.stack.push(Brk::Loop { head, after: if plain { next } else { Next::None } });
                let (lines, _) = self.r(rest, d + 1, Next::Top(head));
                self.stack.pop();
                v.push(out(d, format!("while ({stay}) {{"), comment.clone()));
                v.extend(self.poll_line(d + 1));
                v.extend(lines);
                v.push(out(d, "}", ""));
                let (after, f) = self.r(exit, d, next);
                v.extend(after);
                return (v, f);
            }
        }
        self.stack.push(Brk::Loop { head, after: next });
        let (lines, _) = self.r(body, d + 1, Next::Top(head));
        self.stack.pop();
        let mut v = vec![out(d, "for (;;) {", "")];
        v.extend(self.poll_line(d + 1));
        v.extend(lines);
        v.push(out(d, "}", ""));
        (v, self.broke.contains(&head))
    }
}

/// A sequence's items, with sequences inside it opened up.
fn flatten(s: &S) -> Vec<&S> {
    match s {
        S::Seq(v) => v.iter().flat_map(flatten).collect(),
        s => vec![s],
    }
}

/// `if (c) { body }`, or one line where the body is one simple statement.
fn if_block(d: usize, cond: &str, body: Vec<Out>, comment: &str) -> Vec<Out> {
    if body.len() == 1 && simple(&body[0]) {
        return vec![out(d, format!("if ({cond}) {}", body[0].code.trim_start()), comment)];
    }
    let mut v = vec![out(d, format!("if ({cond}) {{"), comment)];
    v.extend(body);
    v.push(out(d, "}", ""));
    v
}

/// Whether a line of code is a comment on a line of its own.
fn note(code: &str) -> bool {
    code.starts_with("/*") || code.starts_with(" *")
}

/// A statement that fits after `if (c)` on one line.
fn simple(l: &Out) -> bool {
    let c = l.code.trim();
    !c.is_empty()
        && !note(&l.code)
        && l.comment.is_empty()
        && !c.ends_with('{')
        && !c.starts_with('}')
        && !c.ends_with(':')
        && !["if ", "if(", "for ", "while ", "do ", "switch ", "case ", "default"].iter().any(|k| c.starts_with(k))
}

/// Whether lines at an indentation are one `if` statement, which an `else`
/// can take as `else if`.
fn single_if(lines: &[Out], d: usize) -> bool {
    let Some(first) = lines.first() else { return false };
    if first.indent != d || !first.code.starts_with("if (") {
        return false;
    }
    let braced = first.code.ends_with('{');
    let top: Vec<&Out> = lines.iter().skip(1).filter(|l| l.indent == d).collect();
    if !braced {
        return top.is_empty();
    }
    // At the top level there may be only the if's own closing brace and its
    // `else` lines.
    top.iter().all(|l| l.code.starts_with('}')) && lines.last().is_some_and(|l| l.indent == d && l.code == "}")
}

fn dedent(v: Vec<Out>) -> Vec<Out> {
    v.into_iter().map(|mut l| {
        l.indent = l.indent.saturating_sub(1);
        l
    })
    .collect()
}

/// The lines of C, indented, with the comments in a column. A label with
/// nothing after it in its block gets an empty statement.
fn finish(lines: Vec<Out>) -> Vec<String> {
    let mut v: Vec<String> = vec![];
    for (i, l) in lines.iter().enumerate() {
        let mut code = l.code.clone();
        if code.ends_with(':') && !code.starts_with("case ") {
            // C needs a statement after a label. The next code serves, if
            // there is any before a closing brace.
            let bare = lines[i + 1..]
                .iter()
                .find(|n| !n.code.is_empty() && !note(&n.code))
                .is_none_or(|n| n.code.starts_with('}'));
            if bare {
                code.push_str(" ;");
            }
        }
        let pad = " ".repeat(4 * l.indent);
        let text = if l.comment.is_empty() {
            format!("{pad}{code}")
        } else if code.is_empty() {
            format!("{:<48}{}", pad, l.comment)
        } else {
            let t = format!("{pad}{code}");
            let w = if t.len() >= 48 { t.len() + 1 } else { 48 };
            format!("{t:<w$}{}", l.comment)
        };
        v.push(text);
    }
    v
}

#[cfg(test)]
mod tests {
    use super::*;

    fn line(c: &str) -> Line {
        Line { code: c.into(), comment: String::new() }
    }

    fn b(label: &str, lines: &[&str], term: Term) -> Block {
        Block { label: label.into(), lines: lines.iter().map(|c| line(c)).collect(), term }
    }

    fn iff(cond: &str, then: usize, else_: usize) -> Term {
        Term::If { cond: cond.into(), inv: format!("!{cond}"), then, else_, comment: String::new() }
    }

    fn text(v: Vec<String>) -> String {
        v.iter().map(|l| l.trim_end()).collect::<Vec<_>>().join("\n")
    }

    #[test]
    fn a_counted_loop_is_a_do_while() {
        let blocks = [
            b("Start", &["r2 = 0;"], Term::Goto(1)),
            b("Loop", &["r2 += r3;", "r3 -= 1;"], iff("r3 != 0", 1, 2)),
            b("Done", &["return;"], Term::Exit),
        ];
        let c = text(structure(&blocks, 0).unwrap());
        assert_eq!(c, "    r2 = 0;\n    do {\n        r2 += r3;\n        r3 -= 1;\n    } while (r3 != 0);\n    return;", "\n{c}");
    }

    #[test]
    fn a_skip_is_an_if_and_a_diamond_an_if_else() {
        // if (c) goto Skip; x; Skip: y
        let blocks = [
            b("A", &[], iff("c", 2, 1)),
            b("B", &["x;"], Term::Goto(2)),
            b("Skip", &["y;", "return;"], Term::Exit),
        ];
        assert_eq!(text(structure(&blocks, 0).unwrap()), "    if (!c) x;\n    y;\n    return;");
        // if (c) goto T; e; goto M; T: t; M: m
        let blocks = [
            b("A", &[], iff("c", 2, 1)),
            b("E", &["e;"], Term::Goto(3)),
            b("T", &["t;"], Term::Goto(3)),
            b("M", &["m;", "return;"], Term::Exit),
        ];
        assert_eq!(text(structure(&blocks, 0).unwrap()), "    if (c) {\n        t;\n    } else {\n        e;\n    }\n    m;\n    return;");
    }

    #[test]
    fn a_polled_loop_polls_first() {
        // --poll-loops: every loop's first statement is the safe point
        let blocks = [
            b("Start", &["r2 = 0;"], Term::Goto(1)),
            b("Loop", &["r2 += r3;", "r3 -= 1;"], iff("r3 != 0", 1, 2)),
            b("Done", &["return;"], Term::Exit),
        ];
        let c = text(structure_with(&blocks, 0, Some("ROS_POLL(s, r13);")).unwrap());
        assert_eq!(
            c,
            "    r2 = 0;\n    do {\n        ROS_POLL(s, r13);\n        r2 += r3;\n        r3 -= 1;\n    } while (r3 != 0);\n    return;",
            "\n{c}"
        );
    }

    #[test]
    fn a_test_at_the_top_is_a_while() {
        // Head: if (c) goto Out; body; goto Head; Out: return
        let blocks = [
            b("Start", &["i = 0;"], Term::Goto(1)),
            b("Head", &[], iff("i >= n", 3, 2)),
            b("Body", &["i += 1;"], Term::Goto(1)),
            b("Out", &["return;"], Term::Exit),
        ];
        assert_eq!(
            text(structure(&blocks, 0).unwrap()),
            "    i = 0;\n    while (!i >= n) {\n        i += 1;\n    }\n    return;"
        );
    }

    #[test]
    fn an_early_return_is_a_guard() {
        let blocks = [
            b("A", &[], iff("bad", 1, 2)),
            b("Fail", &["r0 = 1;", "return;"], Term::Exit),
            b("Go", &["r0 = 0;", "return;"], Term::Exit),
        ];
        assert_eq!(
            text(structure(&blocks, 0).unwrap()),
            "    if (bad) {\n        r0 = 1;\n        return;\n    }\n    r0 = 0;\n    return;"
        );
    }

    #[test]
    fn an_irreducible_graph_is_refused() {
        // Two ways into a cycle: A -> B, A -> C, B <-> C.
        let blocks = [
            b("A", &[], iff("c", 1, 2)),
            b("B", &["b;"], iff("d", 2, 3)),
            b("C", &["c;"], Term::Goto(1)),
            b("D", &["return;"], Term::Exit),
        ];
        assert!(structure(&blocks, 0).is_none());
    }
}
