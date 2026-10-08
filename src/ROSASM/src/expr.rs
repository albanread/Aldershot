//! ObjAsm expression evaluation.
//!
//! The meaning of each operator follows the RISC OS assembler manual
//! (`docs/PRM/pdf/Asm.pdf`, chapter 7). Two rules from it drive everything
//! here:
//!
//! * **Arithmetic values are unsigned 32-bit.** The manual is explicit: "the
//!   value of `0>-1` is `{FALSE}`". So `-1` is `0xFFFFFFFF`, all arithmetic
//!   wraps at 32 bits, and every comparison is unsigned.
//! * **Precedence.** Unary operators bind tightest, and adjacent unary
//!   operators are evaluated right to left. Binary operators of equal
//!   precedence are evaluated left to right. The six binary bands, tightest
//!   first, are: multiplicative, string manipulation, shift,
//!   additive/bitwise, relational, boolean.
//!
//! Some operators need context the evaluator does not hold itself. The
//! location counters (`{PC}`, `.`, `@`) and `?label` are read from the
//! symbol table, where the expander records them. The `:F*:` file operators
//! work only when the caller supplies a file lookup. Anything the evaluator
//! cannot answer, such as an unset location counter, a local-label
//! reference or `:BASE:`, still parses, and fails with a clear message
//! rather than giving a wrong answer.

use crate::symtab::{SymTab, Value};

#[derive(Debug, PartialEq)]
pub struct EvalError {
    pub msg: String,
    pub pos: usize,
}

impl std::fmt::Display for EvalError {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        write!(f, "{} (at column {})", self.msg, self.pos + 1)
    }
}

type R<T> = Result<T, EvalError>;

fn err<T>(pos: usize, msg: impl Into<String>) -> R<T> {
    Err(EvalError { msg: msg.into(), pos })
}

// ---------------------------------------------------------------- tokens

#[derive(Debug, Clone, PartialEq)]
enum Tok {
    Num(u32),
    Str(String),
    Ident(String),
    /// `:NAME:` form.
    Named(String),
    /// Symbolic operator or punctuation.
    Sym(&'static str),
    LParen,
    RParen,
    True,
    False,
    /// `{...}` built-in other than TRUE/FALSE.
    Builtin(String),
}

struct Lexer<'a> {
    src: &'a [char],
    i: usize,
}

/// Longer symbols come first, so `<<` and `<=` are matched before `<`.
const SYMS: &[&str] = &[
    "<<", ">>", "&&", "||", "!=", "/=", "<>", "><", "<=", ">=", "==", "+", "-", "*", "/", "%", "&",
    "^", "|", "<", ">", "=", "!", "?", "~",
];

impl<'a> Lexer<'a> {
    fn new(src: &'a [char]) -> Self {
        Lexer { src, i: 0 }
    }

    fn skip_ws(&mut self) {
        while self.i < self.src.len() && (self.src[self.i] == ' ' || self.src[self.i] == '\t') {
            self.i += 1;
        }
    }

    fn peek_char(&self) -> Option<char> {
        self.src.get(self.i).copied()
    }

    fn digits(&mut self, radix: u32) -> R<u32> {
        let start = self.i;
        let mut v: u32 = 0;
        let mut any = false;
        while let Some(c) = self.peek_char() {
            let Some(d) = c.to_digit(radix) else { break };
            v = v.wrapping_mul(radix).wrapping_add(d);
            any = true;
            self.i += 1;
        }
        if !any {
            return err(start, format!("expected base-{radix} digits"));
        }
        Ok(v)
    }

    fn next_tok(&mut self) -> R<Option<(Tok, usize)>> {
        self.skip_ws();
        let pos = self.i;
        let Some(c) = self.peek_char() else {
            return Ok(None);
        };

        // &hex
        if c == '&' {
            // `&` is also bitwise AND. It introduces a literal only when a hex
            // digit follows immediately.
            if self.src.get(self.i + 1).is_some_and(|d| d.is_ascii_hexdigit()) {
                self.i += 1;
                return Ok(Some((Tok::Num(self.digits(16)?), pos)));
            }
        }

        // 0x hex, n_radix, plain decimal
        if c.is_ascii_digit() {
            if c == '0' && matches!(self.src.get(self.i + 1), Some('x') | Some('X')) {
                self.i += 2;
                return Ok(Some((Tok::Num(self.digits(16)?), pos)));
            }
            // `n_ddd`: digits in radix n, as in 2_1010 for binary.
            let save = self.i;
            let base = self.digits(10)?;
            if self.peek_char() == Some('_') && (2..=16).contains(&base) {
                self.i += 1;
                return Ok(Some((Tok::Num(self.digits(base)?), pos)));
            }
            self.i = save;
            return Ok(Some((Tok::Num(self.digits(10)?), pos)));
        }

        // "string", with "" as an embedded quote
        if c == '"' {
            self.i += 1;
            let mut s = String::new();
            loop {
                match self.peek_char() {
                    None => return err(pos, "unterminated string"),
                    Some('"') => {
                        if self.src.get(self.i + 1) == Some(&'"') {
                            s.push('"');
                            self.i += 2;
                        } else {
                            self.i += 1;
                            break;
                        }
                    }
                    Some('\\') => match crate::lex::escape_at(&self.src[self.i + 1..]) {
                        Some((c, n)) => {
                            s.push(c);
                            self.i += 1 + n;
                        }
                        None => {
                            s.push('\\');
                            self.i += 1;
                        }
                    },
                    Some(ch) => {
                        s.push(ch);
                        self.i += 1;
                    }
                }
            }
            return Ok(Some((Tok::Str(s), pos)));
        }

        // 'c' character constant. It is a one-character string, which the
        // manual says becomes arithmetic where the context demands it.
        if c == '\'' && self.src.get(self.i + 2) == Some(&'\'') {
            let ch = self.src[self.i + 1];
            self.i += 3;
            return Ok(Some((Tok::Str(ch.to_string()), pos)));
        }
        // '\\', and the other escapes.
        if c == '\'' && self.src.get(self.i + 1) == Some(&'\\') {
            if let Some((ch, n)) = crate::lex::escape_at(&self.src[self.i + 2..]) {
                if self.src.get(self.i + 2 + n) == Some(&'\'') {
                    self.i += 3 + n;
                    return Ok(Some((Tok::Str(ch.to_string()), pos)));
                }
            }
        }

        // {TRUE} / {FALSE} / {PC} / ...
        if c == '{' {
            let mut j = self.i + 1;
            let mut name = String::new();
            while j < self.src.len() && self.src[j] != '}' {
                name.push(self.src[j]);
                j += 1;
            }
            if j >= self.src.len() {
                return err(pos, "unterminated { }");
            }
            self.i = j + 1;
            let upper = name.to_ascii_uppercase();
            return Ok(Some((
                match upper.as_str() {
                    "TRUE" => Tok::True,
                    "FALSE" => Tok::False,
                    _ => Tok::Builtin(upper),
                },
                pos,
            )));
        }

        // :NAMED:
        if c == ':' {
            let mut j = self.i + 1;
            let mut name = String::new();
            while j < self.src.len() && self.src[j] != ':' {
                name.push(self.src[j]);
                j += 1;
            }
            if j >= self.src.len() {
                return err(pos, "unterminated :operator:");
            }
            self.i = j + 1;
            return Ok(Some((Tok::Named(name.to_ascii_uppercase()), pos)));
        }

        if c == '(' {
            self.i += 1;
            return Ok(Some((Tok::LParen, pos)));
        }
        if c == ')' {
            self.i += 1;
            return Ok(Some((Tok::RParen, pos)));
        }

        // |quoted symbol|. ObjAsm uses bars around a name that contains
        // characters which would otherwise be operators, e.g.
        // |_stub_kallocExtendsWS|. A bar starts a quoted name only when a
        // matching bar follows in the same expression. Otherwise `|` is
        // bitwise OR.
        if c == '|' {
            if let Some(close) = (self.i + 1..self.src.len()).find(|&j| self.src[j] == '|') {
                let name: String = self.src[self.i + 1..close].iter().collect();
                if !name.is_empty() && !name.contains(' ') {
                    self.i = close + 1;
                    return Ok(Some((Tok::Ident(name), pos)));
                }
            }
        }

        // Location counters. `.` is the current program counter, and `@` is
        // the current offset within a MAP. Both need layout, but both must
        // parse.
        if c == '.' || c == '@' {
            self.i += 1;
            return Ok(Some((Tok::Builtin(if c == '.' { "." } else { "@" }.to_string()), pos)));
        }

        // Local label reference: `%`, an optional search direction (F/B) and
        // level (T/A), then the label number, as in `%FT05`, `%BT30`, `%01`.
        // Resolving one needs ROUT scopes, but it must parse. Otherwise `%`
        // is modulo.
        if c == '%' {
            let mut j = self.i + 1;
            while matches!(self.src.get(j), Some('F' | 'B' | 'T' | 'A' | 'f' | 'b' | 't' | 'a')) {
                j += 1;
            }
            if self.src.get(j).is_some_and(|d| d.is_ascii_digit()) {
                while self.src.get(j).is_some_and(|d| d.is_ascii_alphanumeric()) {
                    j += 1;
                }
                let name: String = self.src[self.i..j].iter().collect();
                self.i = j;
                return Ok(Some((Tok::Builtin(name), pos)));
            }
        }

        // A `$` still here means substitution left it alone. That happens
        // when the name is not defined at all. Often it is a variable from a
        // header that has not been read by GET, not a macro parameter. The
        // error names it either way.
        if c == '$' {
            let mut j = self.i + 1;
            while self
                .src
                .get(j)
                .is_some_and(|ch| ch.is_ascii_alphanumeric() || *ch == '_')
            {
                j += 1;
            }
            let name: String = self.src[self.i + 1..j].iter().collect();
            return err(pos, format!("'${name}' is undefined (unexpanded variable or macro parameter)"));
        }

        // identifier
        if c.is_ascii_alphabetic() || c == '_' {
            let mut s = String::new();
            while let Some(ch) = self.peek_char() {
                if ch.is_ascii_alphanumeric() || ch == '_' {
                    s.push(ch);
                    self.i += 1;
                } else {
                    break;
                }
            }
            return Ok(Some((Tok::Ident(s), pos)));
        }

        for sym in SYMS {
            let n = sym.chars().count();
            if self.src.len() >= self.i + n
                && self.src[self.i..self.i + n].iter().collect::<String>() == **sym
            {
                self.i += n;
                return Ok(Some((Tok::Sym(sym), pos)));
            }
        }

        err(pos, format!("unexpected character '{c}'"))
    }
}

fn tokenize(s: &str) -> R<Vec<(Tok, usize)>> {
    let chars: Vec<char> = s.chars().collect();
    let mut lx = Lexer::new(&chars);
    let mut out = Vec::new();
    while let Some(t) = lx.next_tok()? {
        out.push(t);
    }
    Ok(out)
}

// ------------------------------------------------------------- precedence

/// Binary bands, tightest first, per the manual.
fn prec(t: &Tok) -> Option<u8> {
    let s: &str = match t {
        Tok::Sym(s) => s,
        Tok::Named(n) => n.as_str(),
        _ => return None,
    };
    Some(match s {
        "*" | "/" | "%" | "MOD" => 6,
        "CC" | "LEFT" | "RIGHT" => 5,
        "<<" | ">>" | "SHL" | "SHR" | "ROL" | "ROR" => 4,
        "+" | "-" | "&" | "^" | "|" | "AND" | "EOR" | "OR" => 3,
        "=" | "==" | "!=" | "/=" | "<>" | "><" | "<" | "<=" | ">" | ">=" => 2,
        "&&" | "||" | "LAND" | "LOR" | "LEOR" => 1,
        _ => return None,
    })
}

// -------------------------------------------------------------- evaluation

/// How an expression reads a file's catalogue entry, for `:FSIZE:` and the
/// other file operators. Without one, those operators are refused.
pub type FileLookup<'a> = &'a dyn Fn(&str) -> Option<FileInfo>;

pub struct Evaluator<'a> {
    toks: Vec<(Tok, usize)>,
    i: usize,
    syms: &'a SymTab,
    files: Option<FileLookup<'a>>,
}

/// A file's catalogue entry, as `:FSIZE:`, `:FLOAD:`, `:FEXEC:` and
/// `:FATTR:` read it. ResourceFS's `ResourceFile` macro puts all four in
/// front of the file it includes.
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct FileInfo {
    pub size: u32,
    pub load: u32,
    pub exec: u32,
    pub attr: u32,
}

/// Evaluate a complete expression string.
pub fn eval(src: &str, syms: &SymTab) -> R<Value> {
    eval_with(src, syms, None)
}

/// Evaluate, answering the file queries from `files`.
pub fn eval_with(
    src: &str,
    syms: &SymTab,
    files: Option<FileLookup<'_>>,
) -> R<Value> {
    let toks = tokenize(src)?;
    if toks.is_empty() {
        return err(0, "empty expression");
    }
    let mut ev = Evaluator { toks, i: 0, syms, files };
    let v = ev.expr(1)?;
    if ev.i < ev.toks.len() {
        let (t, p) = &ev.toks[ev.i];
        return err(*p, format!("unexpected trailing {t:?}"));
    }
    Ok(v)
}

impl<'a> Evaluator<'a> {
    fn peek(&self) -> Option<&(Tok, usize)> {
        self.toks.get(self.i)
    }

    fn expr(&mut self, min_prec: u8) -> R<Value> {
        let mut lhs = self.unary()?;
        while let Some((t, pos)) = self.peek().cloned() {
            let Some(p) = prec(&t) else { break };
            if p < min_prec {
                break;
            }
            self.i += 1;
            // Left associative: the right operand binds tighter than this band.
            let rhs = self.expr(p + 1)?;
            lhs = binary(&t, lhs, rhs, pos)?;
        }
        Ok(lhs)
    }

    fn unary(&mut self) -> R<Value> {
        let Some((t, pos)) = self.peek().cloned() else {
            return err(self.toks.last().map(|(_, p)| *p).unwrap_or(0), "expression expected");
        };

        match &t {
            // `:DEF:` takes a *name*, not a value. The operand must not be
            // evaluated, because the name may not exist, and that is what
            // `:DEF:` is asking.
            Tok::Named(n) if n == "DEF" => {
                self.i += 1;
                let Some((Tok::Ident(name), _)) = self.peek().cloned() else {
                    return err(pos, ":DEF: needs a symbol name");
                };
                self.i += 1;
                return Ok(Value::Logical(self.syms.is_defined(&name)));
            }
            Tok::Sym("+") => {
                self.i += 1;
                let v = self.unary()?;
                return Ok(Value::Arith(as_arith(&v, pos)?));
            }
            Tok::Sym("-") => {
                self.i += 1;
                let v = self.unary()?;
                return Ok(Value::Arith(as_arith(&v, pos)?.wrapping_neg()));
            }
            Tok::Sym("!") => {
                self.i += 1;
                let v = self.unary()?;
                return Ok(Value::Logical(!as_logical(&v, pos)?));
            }
            Tok::Sym("~") => {
                self.i += 1;
                let v = self.unary()?;
                return Ok(Value::Arith(!as_arith(&v, pos)?));
            }
            // `?A` is "the number of bytes generated by the line defining
            // label A". Like :DEF:, it takes a name rather than a value.
            Tok::Sym("?") => {
                self.i += 1;
                let Some((Tok::Ident(name), _)) = self.peek().cloned() else {
                    return err(pos, "'?' needs a label name");
                };
                self.i += 1;
                return match self.syms.absolute(&format!("?{name}")) {
                    Some(n) => Ok(Value::Arith(n)),
                    // A symbol that exists but emitted no bytes has size
                    // zero. Only an unknown name is an error.
                    None if self.syms.is_defined(&name) => Ok(Value::Arith(0)),
                    None => err(pos, format!("'?' on undefined label '{name}'")),
                };
            }
            Tok::Named(n) => {
                let n = n.clone();
                if is_unary_named(&n) {
                    self.i += 1;
                    let v = self.unary()?;
                    if let (Some(files), "FATTR" | "FEXEC" | "FLOAD" | "FSIZE") = (self.files, n.as_str()) {
                        let name = as_str(&v, pos)?;
                        let Some(f) = files(&name) else {
                            return err(pos, format!(":{n}: cannot find '{name}'"));
                        };
                        return Ok(Value::Arith(match n.as_str() {
                            "FATTR" => f.attr,
                            "FEXEC" => f.exec,
                            "FLOAD" => f.load,
                            _ => f.size,
                        }));
                    }
                    return unary_named(&n, v, pos);
                }
            }
            _ => {}
        }

        self.primary()
    }

    fn primary(&mut self) -> R<Value> {
        let Some((t, pos)) = self.peek().cloned() else {
            return err(0, "expression expected");
        };
        self.i += 1;
        match t {
            Tok::Num(n) => Ok(Value::Arith(n)),
            Tok::Str(s) => Ok(Value::Str(s)),
            Tok::True => Ok(Value::Logical(true)),
            Tok::False => Ok(Value::Logical(false)),
            // Built-in variables live in the symbol table under their braced
            // name. The driver sets some ({CONFIG}, {ENDIAN}, {CPU}, ...),
            // and the expander sets the location counters ({PC}, {.}, {@})
            // as layout moves them. One that is not set is an error, because
            // a wrong answer there is worse than an error.
            Tok::Builtin(ref b) if !b.starts_with('%') => {
                if let Some(v) = self.syms.get(&format!("{{{b}}}")) {
                    return Ok(v.clone());
                }
                err(pos, format!("{{{b}}} needs layout information, not available yet"))
            }
            Tok::Builtin(b) => {
                let what = match b.as_str() {
                    "." => "the program counter '.'".to_string(),
                    "@" => "the MAP counter '@'".to_string(),
                    s if s.starts_with('%') => format!("local label '{s}'"),
                    _ => format!("{{{b}}}"),
                };
                err(pos, format!("{what} needs layout information, not available yet"))
            }
            Tok::LParen => {
                let v = self.expr(1)?;
                match self.peek() {
                    Some((Tok::RParen, _)) => {
                        self.i += 1;
                        Ok(v)
                    }
                    _ => err(pos, "unclosed '('"),
                }
            }
            Tok::Ident(name) => {
                if let Some(v) = self.syms.get(&name) {
                    return Ok(v.clone());
                }
                if let Some(a) = self.syms.absolute(&name) {
                    return Ok(Value::Arith(a));
                }
                err(pos, format!("undefined symbol '{name}'"))
            }
            other => err(pos, format!("unexpected {other:?}")),
        }
    }
}

// ------------------------------------------------------------ conversions

/// The manual allows a single-character string to become arithmetic "if the
/// context demands it".
fn as_arith(v: &Value, pos: usize) -> R<u32> {
    match v {
        Value::Arith(n) => Ok(*n),
        Value::Str(s) if s.chars().count() == 1 => Ok(s.chars().next().unwrap() as u32),
        Value::Str(_) => err(pos, "a multi-character string is not arithmetic"),
        Value::Logical(_) => err(pos, "a logical value is not arithmetic"),
    }
}

fn as_logical(v: &Value, pos: usize) -> R<bool> {
    match v {
        Value::Logical(b) => Ok(*b),
        _ => err(pos, "expected a logical value"),
    }
}

fn as_str(v: &Value, pos: usize) -> R<String> {
    match v {
        Value::Str(s) => Ok(s.clone()),
        _ => err(pos, "expected a string value"),
    }
}

// ------------------------------------------------------------ unary named

fn is_unary_named(n: &str) -> bool {
    matches!(
        n,
        "LNOT" | "NOT" | "STR" | "CHR" | "LEN" | "UPPERCASE" | "LOWERCASE" | "BASE" | "INDEX"
            | "FATTR" | "FEXEC" | "FLOAD" | "FSIZE" | "RCONST" | "CC_ENCODING" | "REVERSE_CC"
    )
}

/// Condition codes in the order their encoding demands (bits 28-31).
const CONDS: [&str; 16] = [
    "EQ", "NE", "CS", "CC", "MI", "PL", "VS", "VC", "HI", "LS", "GE", "LT", "GT", "LE", "AL", "NV",
];

fn unary_named(n: &str, v: Value, pos: usize) -> R<Value> {
    Ok(match n {
        "LNOT" => Value::Logical(!as_logical(&v, pos)?),
        "NOT" => Value::Arith(!as_arith(&v, pos)?),
        // Eight-digit hex for arithmetic; "T"/"F" for logical.
        "STR" => match &v {
            Value::Logical(b) => Value::Str(if *b { "T".into() } else { "F".into() }),
            _ => Value::Str(format!("{:08X}", as_arith(&v, pos)?)),
        },
        "CHR" => {
            let c = as_arith(&v, pos)?;
            match char::from_u32(c) {
                Some(ch) => Value::Str(ch.to_string()),
                None => return err(pos, "value is not a character"),
            }
        }
        "LEN" => Value::Arith(as_str(&v, pos)?.chars().count() as u32),
        "UPPERCASE" => Value::Str(as_str(&v, pos)?.to_uppercase()),
        "LOWERCASE" => Value::Str(as_str(&v, pos)?.to_lowercase()),
        // A no-op in ObjAsm, kept for armasm compatibility.
        "RCONST" => v,
        "CC_ENCODING" => {
            let s = as_str(&v, pos)?.to_ascii_uppercase();
            match CONDS.iter().position(|c| *c == s) {
                Some(i) => Value::Arith((i as u32) << 28),
                None => return err(pos, format!("'{s}' is not a condition name")),
            }
        }
        "REVERSE_CC" => {
            let s = as_str(&v, pos)?.to_ascii_uppercase();
            match CONDS.iter().position(|c| *c == s) {
                // Conditions pair up: the inverse flips the low bit.
                Some(i) => Value::Str(CONDS[i ^ 1].to_string()),
                None => return err(pos, format!("'{s}' is not a condition name")),
            }
        }
        // The manual: "If A has no register offsets and no relocations, BASE
        // produces an error and INDEX has no effect." The evaluator's values
        // carry no register offsets, so INDEX returns its operand and BASE
        // is an error. That is the specified behaviour, not a gap.
        "INDEX" => v,
        "BASE" => {
            return err(pos, ":BASE: on a value with no register offset")
        }
        "FATTR" | "FEXEC" | "FLOAD" | "FSIZE" => {
            return err(pos, format!(":{n}: needs the files, and this evaluation has none"))
        }
        _ => return err(pos, format!("unknown unary operator :{n}:")),
    })
}

// ----------------------------------------------------------------- binary

fn binary(t: &Tok, a: Value, b: Value, pos: usize) -> R<Value> {
    let op: &str = match t {
        Tok::Sym(s) => s,
        Tok::Named(n) => n.as_str(),
        _ => return err(pos, "not an operator"),
    };

    // String manipulation keeps its operands as strings.
    match op {
        "CC" => return Ok(Value::Str(format!("{}{}", as_str(&a, pos)?, as_str(&b, pos)?))),
        "LEFT" => {
            let s = as_str(&a, pos)?;
            let n = as_arith(&b, pos)? as usize;
            return Ok(Value::Str(s.chars().take(n).collect()));
        }
        "RIGHT" => {
            let s = as_str(&a, pos)?;
            let n = as_arith(&b, pos)? as usize;
            let len = s.chars().count();
            return Ok(Value::Str(s.chars().skip(len.saturating_sub(n)).collect()));
        }
        _ => {}
    }

    // Relational. Two strings compare in ASCII order, and two logicals
    // compare with each other. Anything else compares as unsigned numbers.
    if matches!(op, "=" | "==" | "!=" | "/=" | "<>" | "><" | "<" | "<=" | ">" | ">=") {
        let ord = match (&a, &b) {
            (Value::Str(x), Value::Str(y)) => x.cmp(y),
            (Value::Logical(x), Value::Logical(y)) => x.cmp(y),
            _ => as_arith(&a, pos)?.cmp(&as_arith(&b, pos)?),
        };
        use std::cmp::Ordering::*;
        return Ok(Value::Logical(match op {
            "=" | "==" => ord == Equal,
            "!=" | "/=" | "<>" | "><" => ord != Equal,
            "<" => ord == Less,
            "<=" => ord != Greater,
            ">" => ord == Greater,
            ">=" => ord != Less,
            _ => unreachable!(),
        }));
    }

    // Boolean.
    if matches!(op, "&&" | "||" | "LAND" | "LOR" | "LEOR") {
        let x = as_logical(&a, pos)?;
        let y = as_logical(&b, pos)?;
        return Ok(Value::Logical(match op {
            "&&" | "LAND" => x && y,
            "||" | "LOR" => x || y,
            "LEOR" => x ^ y,
            _ => unreachable!(),
        }));
    }

    // Everything else is arithmetic, unsigned, wrapping at 32 bits.
    let x = as_arith(&a, pos)?;
    let y = as_arith(&b, pos)?;
    Ok(Value::Arith(match op {
        "*" => x.wrapping_mul(y),
        "/" => {
            if y == 0 {
                return err(pos, "division by zero");
            }
            x / y
        }
        "%" | "MOD" => {
            if y == 0 {
                return err(pos, "modulo by zero");
            }
            x % y
        }
        // A shift by 32 or more gives zero. The count does not wrap.
        "<<" | "SHL" => {
            if y >= 32 {
                0
            } else {
                x << y
            }
        }
        // Logical shift: the manual notes SHR does not propagate the sign bit.
        ">>" | "SHR" => {
            if y >= 32 {
                0
            } else {
                x >> y
            }
        }
        "ROL" => x.rotate_left(y % 32),
        "ROR" => x.rotate_right(y % 32),
        "+" => x.wrapping_add(y),
        "-" => x.wrapping_sub(y),
        "&" | "AND" => x & y,
        "|" | "OR" => x | y,
        "^" | "EOR" => x ^ y,
        _ => return err(pos, format!("unknown operator '{op}'")),
    }))
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::symtab::Type;

    fn ev(s: &str) -> Value {
        eval(s, &SymTab::new()).unwrap_or_else(|e| panic!("{s}: {e}"))
    }
    fn a(s: &str) -> u32 {
        match ev(s) {
            Value::Arith(n) => n,
            v => panic!("{s}: expected arithmetic, got {v:?}"),
        }
    }
    fn b(s: &str) -> bool {
        match ev(s) {
            Value::Logical(x) => x,
            v => panic!("{s}: expected logical, got {v:?}"),
        }
    }
    fn s(x: &str) -> String {
        match ev(x) {
            Value::Str(t) => t,
            v => panic!("{x}: expected string, got {v:?}"),
        }
    }

    #[test]
    fn number_literals_in_every_base() {
        assert_eq!(a("42"), 42);
        assert_eq!(a("&FF"), 0xFF);
        assert_eq!(a("&ff"), 0xFF);
        assert_eq!(a("0x1234"), 0x1234);
        assert_eq!(a("2_1010"), 0b1010);
        assert_eq!(a("8_777"), 0o777);
    }

    #[test]
    fn arithmetic_is_unsigned_32_bit() {
        // The manual's own example: 0 > -1 is FALSE because -1 is 0xFFFFFFFF.
        assert!(!b("0 > -1"));
        assert_eq!(a("-1"), 0xFFFF_FFFF);
        assert_eq!(a("0 - 1"), 0xFFFF_FFFF);
    }

    #[test]
    fn arithmetic_wraps_rather_than_panicking() {
        assert_eq!(a("&FFFFFFFF + 1"), 0);
        assert_eq!(a("&80000000 * 2"), 0);
    }

    #[test]
    fn precedence_multiplicative_over_additive() {
        assert_eq!(a("2 + 3 * 4"), 14);
        assert_eq!(a("(2 + 3) * 4"), 20);
    }

    #[test]
    fn precedence_shift_binds_tighter_than_additive() {
        // 1 << 4 = 16, then + 1
        assert_eq!(a("1 :SHL: 4 + 1"), 17);
    }

    #[test]
    fn precedence_relational_below_arithmetic() {
        assert!(b("1 + 1 = 2"));
    }

    #[test]
    fn precedence_boolean_is_weakest() {
        assert!(b("1 = 1 :LAND: 2 = 2"));
        assert!(b("1 = 2 :LOR: 3 = 3"));
    }

    #[test]
    fn equal_precedence_is_left_associative() {
        assert_eq!(a("100 / 10 / 2"), 5); // (100/10)/2, not 100/(10/2)
        assert_eq!(a("10 - 3 - 2"), 5);
    }

    #[test]
    fn adjacent_unary_operators_bind_right_to_left() {
        assert_eq!(a("- - 5"), 5);
        assert!(b("!!{TRUE}"));
    }

    #[test]
    fn shifts_of_32_or_more_are_zero() {
        assert_eq!(a("1 :SHL: 32"), 0);
        assert_eq!(a("&FFFFFFFF :SHR: 32"), 0);
    }

    #[test]
    fn shr_is_logical_not_arithmetic() {
        // Sign bit must not propagate.
        assert_eq!(a("&80000000 :SHR: 31"), 1);
    }

    #[test]
    fn rotates_wrap_the_count() {
        assert_eq!(a("1 :ROR: 1"), 0x8000_0000);
        assert_eq!(a("&80000000 :ROL: 1"), 1);
        assert_eq!(a("1 :ROL: 32"), 1);
    }

    #[test]
    fn bitwise_operators_both_spellings() {
        assert_eq!(a("&F0 :OR: &0F"), 0xFF);
        assert_eq!(a("&F0 | &0F"), 0xFF);
        assert_eq!(a("&FF :AND: &0F"), 0x0F);
        assert_eq!(a("&FF & &0F"), 0x0F);
        assert_eq!(a("&FF :EOR: &0F"), 0xF0);
        assert_eq!(a("&FF ^ &0F"), 0xF0);
        assert_eq!(a(":NOT: 0"), 0xFFFF_FFFF);
    }

    #[test]
    fn modulo_both_spellings() {
        assert_eq!(a("17 :MOD: 5"), 2);
        assert_eq!(a("17 % 5"), 2);
    }

    #[test]
    fn division_by_zero_is_an_error() {
        assert!(eval("1 / 0", &SymTab::new()).is_err());
        assert!(eval("1 :MOD: 0", &SymTab::new()).is_err());
    }

    #[test]
    fn all_relational_spellings() {
        assert!(b("1 = 1"));
        assert!(b("1 == 1"));
        assert!(b("1 != 2"));
        assert!(b("1 /= 2"));
        assert!(b("1 <> 2"));
        assert!(b("1 >< 2"));
        assert!(b("1 < 2"));
        assert!(b("1 <= 1"));
        assert!(b("2 > 1"));
        assert!(b("2 >= 2"));
    }

    #[test]
    fn strings_compare_in_ascii_order() {
        assert!(b("\"abc\" < \"abd\""));
        assert!(b("\"ab\" < \"abc\""), "leading substring sorts first");
        assert!(b("\"A\" < \"a\""));
    }

    #[test]
    fn string_operators() {
        assert_eq!(s("\"foo\" :CC: \"bar\""), "foobar");
        assert_eq!(s("\"abcdef\" :LEFT: 3"), "abc");
        assert_eq!(s("\"abcdef\" :RIGHT: 3"), "def");
        assert_eq!(a(":LEN: \"hello\""), 5);
        assert_eq!(s(":CHR: 65"), "A");
        assert_eq!(s(":UPPERCASE: \"abc\""), "ABC");
        assert_eq!(s(":LOWERCASE: \"ABC\""), "abc");
    }

    #[test]
    fn str_gives_eight_hex_digits_or_t_f() {
        assert_eq!(s(":STR: 255"), "000000FF");
        assert_eq!(s(":STR: {TRUE}"), "T");
        assert_eq!(s(":STR: {FALSE}"), "F");
    }

    #[test]
    fn right_of_a_short_string_is_the_whole_string() {
        assert_eq!(s("\"ab\" :RIGHT: 5"), "ab");
    }

    #[test]
    fn single_character_string_converts_to_arithmetic_in_context() {
        // The manual: conversion happens "if the context demands it". An
        // arithmetic operator is such a context...
        assert_eq!(a("\"A\" + 1"), 66);
        assert_eq!(a("'A' + 1"), 66);
        assert_eq!(a("'A' * 2"), 130);
        // ...a bare character constant is not. It stays a one-character string.
        assert_eq!(s("'A'"), "A");
        assert_eq!(a(":LEN: 'A'"), 1);
        // A longer string never converts.
        assert!(eval("\"AB\" + 1", &SymTab::new()).is_err());
    }

    #[test]
    fn condition_code_operators() {
        assert_eq!(a(":CC_ENCODING: \"EQ\""), 0x0000_0000);
        assert_eq!(a(":CC_ENCODING: \"NE\""), 0x1000_0000);
        assert_eq!(a(":CC_ENCODING: \"AL\""), 0xE000_0000);
        assert_eq!(s(":REVERSE_CC: \"EQ\""), "NE");
        assert_eq!(s(":REVERSE_CC: \"GE\""), "LT");
        assert_eq!(s(":REVERSE_CC: \"LT\""), "GE");
    }

    #[test]
    fn symbols_resolve_from_the_table() {
        let mut t = SymTab::new();
        t.declare_global("Count", Type::Arith);
        t.set("Count", Value::Arith(7)).unwrap();
        t.define_absolute("Base", 0x8000);
        assert_eq!(eval("Count * 2", &t).unwrap(), Value::Arith(14));
        assert_eq!(eval("Base + 4", &t).unwrap(), Value::Arith(0x8004));
    }

    #[test]
    fn def_does_not_evaluate_its_operand() {
        let mut t = SymTab::new();
        t.declare_global("Known", Type::Logical);
        assert_eq!(eval(":DEF: Known", &t).unwrap(), Value::Logical(true));
        // This must not fail on the undefined name. That is what :DEF: is for.
        assert_eq!(eval(":DEF: Missing", &t).unwrap(), Value::Logical(false));
    }

    #[test]
    fn undefined_symbol_is_an_error_outside_def() {
        assert!(eval("Missing + 1", &SymTab::new()).is_err());
    }

    #[test]
    fn ampersand_is_and_when_not_starting_a_hex_literal() {
        assert_eq!(a("&FF & &F0"), 0xF0);
        assert_eq!(a("12 & 10"), 8);
    }

    #[test]
    fn context_dependent_operators_fail_loudly_not_silently() {
        for src in [
            "{PC}", ":FSIZE: \"x\"", "%FT05", "%BT30", "%01",
        ] {
            let e = eval(src, &SymTab::new()).unwrap_err();
            assert!(
                e.msg.contains("not available yet")
                    || e.msg.contains("not implemented")
                    || e.msg.contains("needs the files"),
                "{src} gave {e}"
            );
        }
    }

    #[test]
    fn index_is_identity_without_a_register_offset() {
        // The manual: with no register offsets, "BASE produces an error and
        // INDEX has no effect".
        assert_eq!(a(":INDEX: 42"), 42);
        assert!(eval(":BASE: 42", &SymTab::new()).is_err());
    }

    #[test]
    fn bar_quoted_symbols_are_identifiers_not_or() {
        let mut t = SymTab::new();
        t.define_absolute("_stub_kallocExtendsWS", 0x115);
        assert_eq!(
            eval("|_stub_kallocExtendsWS| + 1", &t).unwrap(),
            Value::Arith(0x116)
        );
        // A bare `|` between operands is still bitwise OR.
        assert_eq!(a("&F0 | &0F"), 0xFF);
    }

    #[test]
    fn percent_is_modulo_unless_it_starts_a_local_label() {
        assert_eq!(a("17 % 5"), 2);
        assert!(eval("%FT05", &SymTab::new())
            .unwrap_err()
            .msg
            .contains("local label"));
    }

    #[test]
    fn unexpanded_macro_parameter_says_so() {
        let e = eval("$Flag :LAND: {TRUE}", &SymTab::new()).unwrap_err();
        assert!(e.msg.contains("'$Flag' is undefined"), "{e}");
    }

    #[test]
    fn unterminated_constructs_are_errors() {
        assert!(eval("\"abc", &SymTab::new()).is_err());
        assert!(eval(":SHL", &SymTab::new()).is_err());
        assert!(eval("(1 + 2", &SymTab::new()).is_err());
        assert!(eval("{TRUE", &SymTab::new()).is_err());
    }

    #[test]
    fn trailing_junk_is_rejected() {
        assert!(eval("1 2", &SymTab::new()).is_err());
    }
}
