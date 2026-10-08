//! Macro expansion, conditional assembly and repetitive assembly.
//!
//! This module takes in source lines and gives out the lines an assembler
//! would actually see. `GET` files are read in at the point they are named,
//! macros are expanded, conditional branches not taken are removed, and
//! `WHILE` loops are unrolled.
//!
//! The rules are those of `docs/PRM/pdf/Asm.pdf`, chapters 8 and 9. The rules
//! below are easy to get wrong, so each one has a test:
//!
//! * **Substitution happens in two stages.** Macro parameters are replaced
//!   first. Then `$`-prefixed variables are "replaced by a string
//!   equivalent". An arithmetic variable becomes **eight hex digits**. That
//!   is why the manual's own example writes `DCD &$counter`: the `&` reads
//!   the digits back as hex.
//! * **Vertical bars toggle substitution off and on.** "If a line contains
//!   vertical bars, substitution will be turned off after this first vertical
//!   bar, on again after the second one, off again after the third." This is
//!   what lets `|_stub_kalloc$WS|` carry a literal `$`.
//! * **`|` as a macro argument selects the default; an omitted argument does
//!   not.** "Note that this default is not used when the macro argument is
//!   omitted — the value is then empty."
//! * **A macro's name may itself end in a parameter.** `PTOp$cc` is invoked
//!   as `PTOpEQ`, which sets `$cc` to `EQ`. 34 macros in the corpus do this.
//! * **A dot ends a substitution and is removed**: `$T33.L25`.
//! * **Skipped conditional branches are not parsed.** Two corpus files contain
//!   malformed expressions. They assemble only because their branch is never
//!   taken. So a skipped branch must be passed over as plain text, without
//!   evaluating anything in it.

use std::collections::HashMap;
use std::path::{Path, PathBuf};

use crate::expr;
use crate::layout;
use crate::lex::{self, Kind, Line};
use crate::symtab::{SymTab, Type, Value};

/// The manual sets the macro nesting limit at 255.
const MAX_MACRO_DEPTH: usize = 255;
/// The manual gives no limit for GET nesting. This one catches a file that
/// includes itself, directly or through others.
const MAX_SOURCE_DEPTH: usize = 64;

#[derive(Debug, Clone, PartialEq)]
pub struct Origin {
    pub file: String,
    pub line: usize,
    /// The macros this line was expanded from, innermost last. Empty for a
    /// line read straight from a file.
    pub macros: Vec<String>,
}

#[derive(Debug, Clone)]
pub struct ExpandedLine {
    /// The operands with every assembly-time variable replaced by the value
    /// it held on this line. Empty for a line with none.
    ///
    /// A variable is what `SETA` and its family write, and its value
    /// changes. The `Entry` macro sets `Proc_LocalStack` from its argument,
    /// and the next routine's `Entry` overwrites it. If the operands of
    /// `SUB sp, sp, #Proc_LocalStack` are read after the whole file has been
    /// expanded, they find whatever the last macro left there. That is how a
    /// routine with a 256-byte frame came out subtracting nothing.
    pub operands: String,
    /// The line after substitution, exactly as ObjAsm would list it. The
    /// expression text is *not* rewritten. ObjAsm lists `DCD 10 + 1` and puts
    /// `0000000B` in the byte column, and to match its listing we do the
    /// same.
    pub text: String,
    pub origin: Origin,
    /// Address within the current AREA.
    pub addr: u32,
    /// Bytes this line contributes, where they are known at expansion time.
    /// Data directives give their values here. Instructions wait for the
    /// encoder.
    pub bytes: Vec<u8>,
    /// True for assembly-time directives such as `AREA`, `SETA` and `WHILE`.
    /// ObjAsm lists them, so the listing needs them. But they emit nothing,
    /// and lowering (the later stage that turns lines into code) skips them.
    pub listing_only: bool,
    /// Index into `Expander::areas` of the AREA this line belongs to. A file
    /// may declare several, and each becomes its own AOF area.
    pub area_index: usize,
    /// Label of the enclosing `ROUT`. It limits the search for a local
    /// label, so `%FT05` finds the `05` in this routine, never the next one's.
    pub rout: Option<String>,
    /// For an `LDR Rd,=expression` that needed a literal pool, the offset
    /// within this area of the word it loads. `None` where the value went
    /// into the instruction itself as a `MOV` or `MVN`.
    pub literal: Option<u32>,
}

/// A storage map: `^ start«, base»` and the fields `name # size` after it.
#[derive(Debug, Clone, PartialEq)]
pub struct StorageMap {
    /// Where its `^` is.
    pub file: String,
    pub line: usize,
    pub start: u32,
    /// The register its fields are offsets from, under `^ start, Rn`.
    pub base: Option<u32>,
    /// Each field: name, offset, size.
    pub fields: Vec<(String, u32, u32)>,
}

#[derive(Debug, PartialEq)]
pub struct ExpandError {
    pub msg: String,
    pub origin: Origin,
}

impl std::fmt::Display for ExpandError {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        write!(f, "{}:{}: {}", self.origin.file, self.origin.line, self.msg)?;
        for m in self.origin.macros.iter().rev() {
            write!(f, "\n  in macro {m}")?;
        }
        Ok(())
    }
}

type R<T> = Result<T, ExpandError>;

// ------------------------------------------------------------------ macros

#[derive(Debug, Clone)]
struct Param {
    name: String,
    default: Option<String>,
}

#[derive(Debug, Clone)]
struct Macro {
    /// Name as written, minus any trailing `$param`.
    stem: String,
    /// The trailing parameter in the name, e.g. `cc` in `PTOp$cc`.
    name_param: Option<String>,
    /// The `$label` on the prototype, if any.
    label_param: Option<String>,
    params: Vec<Param>,
    body: Vec<Line>,
}

/// Split `Name$cc` into (`Name`, Some(`cc`)).
fn split_name(op: &str) -> (String, Option<String>) {
    match op.find('$') {
        Some(i) => {
            let stem = op[..i].to_string();
            let p = op[i + 1..].trim_end_matches('.').to_string();
            (stem, if p.is_empty() { None } else { Some(p) })
        }
        None => (op.to_string(), None),
    }
}

/// Parse the parameter list of a prototype: `$a,$b="x",$c`.
fn parse_params(operands: &str) -> Vec<Param> {
    let mut out = Vec::new();
    for raw in split_args(operands) {
        let raw = raw.trim();
        if raw.is_empty() {
            continue;
        }
        let (name, default) = match raw.split_once('=') {
            Some((n, d)) => (n.trim(), Some(d.trim().to_string())),
            None => (raw, None),
        };
        out.push(Param {
            name: name.trim_start_matches('$').to_string(),
            default,
        });
    }
    out
}

/// Split on commas that are not inside a string or brackets.
fn split_args(s: &str) -> Vec<String> {
    let mut out = Vec::new();
    let mut cur = String::new();
    let mut depth = 0i32;
    let mut in_str = false;
    for c in s.chars() {
        match c {
            '"' => {
                in_str = !in_str;
                cur.push(c);
            }
            '(' | '{' if !in_str => {
                depth += 1;
                cur.push(c);
            }
            ')' | '}' if !in_str => {
                depth -= 1;
                cur.push(c);
            }
            ',' if !in_str && depth == 0 => out.push(std::mem::take(&mut cur)),
            _ => cur.push(c),
        }
    }
    out.push(cur);
    out
}

// ---------------------------------------------------------- substitution

/// Render a variable's value as the text that replaces `$name`.
///
/// The manual says variables "are replaced by a string equivalent". An
/// arithmetic value becomes eight hex digits, which is why the manual's own
/// `WHILE` example writes `DCD &$counter`. A logical value becomes `T` or
/// `F`, as `:STR:` gives.
fn value_text(v: &Value) -> String {
    match v {
        Value::Str(s) => s.clone(),
        Value::Arith(n) => format!("{n:08X}"),
        Value::Logical(b) => if *b { "T" } else { "F" }.to_string(),
    }
}

/// Perform one substitution pass over a line.
///
/// `lookup` turns a name into its replacement text. If it returns `None`,
/// the `$name` is left untouched so that a later pass or stage can see it.
fn substitute(line: &str, lookup: &dyn Fn(&str) -> Option<String>) -> String {
    let chars: Vec<char> = line.chars().collect();
    let mut out = String::new();
    let mut i = 0;
    // Vertical bars toggle substitution off and on across the line.
    let mut active = true;

    while i < chars.len() {
        let c = chars[i];
        if c == '|' {
            active = !active;
            out.push(c);
            i += 1;
            continue;
        }
        if c == '$' && active {
            // `$$` is a literal dollar.
            if chars.get(i + 1) == Some(&'$') {
                out.push('$');
                i += 2;
                continue;
            }
            // `$|name|` is a name written in bars, as a symbol may be. The
            // bars are replaced along with the name, as in `DS_$|what|_Mask`
            // in DragASprite's TestAFlag. If the name has no value, the bars
            // stay and toggle substitution as usual.
            if chars.get(i + 1) == Some(&'|') {
                let start = i + 2;
                let mut j = start;
                while j < chars.len() && (chars[j].is_ascii_alphanumeric() || chars[j] == '_') {
                    j += 1;
                }
                if j > start && chars.get(j) == Some(&'|') {
                    let name: String = chars[start..j].iter().collect();
                    if let Some(text) = lookup(&name) {
                        out.push_str(&text);
                        i = j + 1;
                        continue;
                    }
                }
            }
            let start = i + 1;
            let mut j = start;
            while j < chars.len() && (chars[j].is_ascii_alphanumeric() || chars[j] == '_') {
                j += 1;
            }
            if j > start {
                let name: String = chars[start..j].iter().collect();
                match lookup(&name) {
                    Some(text) => {
                        out.push_str(&text);
                        // A dot terminates the name and is consumed.
                        i = if chars.get(j) == Some(&'.') { j + 1 } else { j };
                        continue;
                    }
                    None => {
                        out.push('$');
                        out.push_str(&name);
                        i = j;
                        continue;
                    }
                }
            }
        }
        out.push(c);
        i += 1;
    }
    out
}

// -------------------------------------------------------------- the engine

enum Source {
    File {
        name: String,
        lines: Vec<Line>,
        pos: usize,
    },
    Macro {
        name: String,
        file: String,
        lines: Vec<Line>,
        pos: usize,
        args: HashMap<String, String>,
        /// A number that tells this expansion apart from every other. Local
        /// labels belong to the expansion that wrote them.
        id: usize,
        /// How many conditionals were open when the macro was entered. This
        /// lets `MEXIT` discard whatever the body opened.
        ///
        /// `MEXIT` leaves from wherever it stands, which is normally inside
        /// the `[` that decided to leave. For example, `Immediate` in
        /// Hdr:Macros tests a rotation, sets its flag and exits, and the `]`
        /// three lines below is never reached. Reaching `MEND` in the normal
        /// way is different. In the manual, conditionals may overlap a macro
        /// expansion rather than nest inside it, and a body may deliberately
        /// open one for its caller to close.
        conds: usize,
    },
    /// A `WHILE` body, re-run until its condition goes false.
    Loop {
        file: String,
        lines: Vec<Line>,
        pos: usize,
        /// The `WHILE` and `WEND` lines themselves. ObjAsm lists both on every
        /// iteration, plus a final `WHILE` when the test fails.
        while_line: Line,
        wend_line: Option<Line>,
    },
}

/// Resolves a `GET` filename to source text.
pub trait FileResolver {
    fn resolve(&self, name: &str) -> Option<(String, Vec<String>)>;

    /// A file `INCBIN` includes or `:FSIZE:` asks about: where it is on the
    /// host and its RISC OS file type. None where files are not to hand.
    fn host_file(&self, _name: &str) -> Option<(PathBuf, u32)> {
        None
    }
}

/// The host file standing for a RISC OS one, and its type.
///
/// A source tree records RISC OS file types the NFS way, as a `,xxx` suffix.
/// So `Fonts.Homerton.Medium.IntMetric0` is `Fonts/Homerton/Medium/IntMetric0,ff6`.
/// A file with no suffix is taken as text, &FFF. That is what the untyped
/// files in the tree are, among them the oblique fonts' one-line `Outlines0`
/// aliases.
pub fn typed_file(path: &Path) -> Option<(PathBuf, u32)> {
    if path.is_file() {
        return Some((path.to_path_buf(), 0xfff));
    }
    let leaf = path.file_name()?.to_str()?;
    for e in std::fs::read_dir(path.parent()?).ok()?.flatten() {
        let name = e.file_name();
        let Some((stem, ty)) = name.to_str().and_then(|n| n.rsplit_once(',')) else { continue };
        if stem == leaf && ty.len() == 3 && e.path().is_file() {
            if let Ok(t) = u32::from_str_radix(ty, 16) {
                return Some((e.path(), t));
            }
        }
    }
    None
}

/// A host file's catalogue entry, as RISC OS would give it. A typed file's
/// load address is &FFFtttdd, where ttt is the type and dd the top byte of
/// the date. Its exec address is the low word of the date. The date is in
/// centiseconds since 1900.
pub fn file_info(path: &Path, filetype: u32) -> Option<expr::FileInfo> {
    let meta = std::fs::metadata(path).ok()?;
    let since = meta.modified().ok()?.duration_since(std::time::UNIX_EPOCH).ok()?;
    // 1900 to 1970, 70 years with 17 leap days.
    let cs = (since.as_secs() + 2_208_988_800) * 100 + u64::from(since.subsec_millis() / 10);
    #[cfg(unix)]
    let attr = {
        use std::os::unix::fs::PermissionsExt;
        let m = meta.permissions().mode();
        // Owner read and write are bits 0 and 1. Public read and write are
        // bits 4 and 5.
        (m >> 8 & 1) | (m >> 6 & 2) | (m << 2 & 16) | (m << 4 & 32)
    };
    #[cfg(not(unix))]
    let attr = if meta.permissions().readonly() { 1 } else { 3 };
    Some(expr::FileInfo {
        size: meta.len() as u32,
        load: 0xfff0_0000 | (filetype & 0xfff) << 8 | (cs >> 32) as u32 & 0xff,
        exec: cs as u32,
        attr,
    })
}

/// Loads `GET` targets from disc, translating RISC OS `dir.Leaf` to `dir/Leaf`.
pub struct DiscResolver {
    pub search: Vec<PathBuf>,
}

impl FileResolver for DiscResolver {
    fn resolve(&self, name: &str) -> Option<(String, Vec<String>)> {
        let rel = name.trim().replace('.', "/");
        for dir in &self.search {
            let p = dir.join(&rel);
            if let Ok(sf) = crate::source::SourceFile::load(&p) {
                return Some((name.to_string(), sf.lines));
            }
        }
        None
    }

    fn host_file(&self, name: &str) -> Option<(PathBuf, u32)> {
        let rel = name.trim().replace('.', "/");
        self.search.iter().find_map(|dir| typed_file(&dir.join(&rel)))
    }
}

pub struct Expander<'a> {
    stack: Vec<Source>,
    macros: Vec<Macro>,
    syms: SymTab,
    resolver: &'a dyn FileResolver,
    /// One entry per open `[`. It holds whether this branch is being
    /// assembled, whether any branch of this conditional has been taken yet,
    /// and where the `[` was written. The place is only read to say which `[`
    /// never closed. That is the only useful message, because the corpus
    /// nests these ten deep across included files.
    conds: Vec<(bool, bool, Origin)>,
    /// The `@` storage-map counter, set by MAP and advanced by FIELD.
    map_counter: u32,
    /// Label of the enclosing `ROUT`, if it had one.
    rout: Option<String>,
    /// The current AREA and its location counter.
    area: Option<layout::Area>,
    /// Every AREA declared, in order of declaration. A source file commonly
    /// has a code area and a data area, and each becomes an AOF area.
    areas: Vec<(String, layout::AreaAttrs)>,
    /// How far each area's location counter reached, parallel to `areas`. A
    /// NOINIT area emits no bytes, so this is the only record of its size.
    area_sizes: Vec<u32>,
    /// Where each label ended up, as name to (area index, offset in that
    /// area). The object writer needs both, because an AOF symbol's value is
    /// an offset within a named area, not a flat address.
    label_defs: std::collections::HashMap<String, (usize, u32)>,
    /// Names the source declared `EXPORT`/`GLOBAL`, in order.
    exports: Vec<String>,
    /// Names the source declared `IMPORT`/`EXTERN`, in order.
    imports: Vec<String>,
    /// Data fields left for the linker, gathered once the values are final.
    data_fixups: Vec<DataFixup>,
    /// Report a failed `ASSERT` and carry on, rather than stopping.
    ///
    /// The sources assert their own layout, so a failure is a real defect and
    /// the assembler treats it as one. A listing is different. The reason for
    /// producing one is to find where the layout went wrong, and stopping at
    /// the first assertion throws away the evidence.
    assert_warnings: bool,
    /// Whether FPA instructions will be converted to VFP, which changes how
    /// many words a compare takes.
    fpa_to_vfp: bool,
    /// Register names declared with `RN`. These are kept apart from the symbol
    /// table because `a1 RN 0` and `Flag EQU 0` are the same value with quite
    /// different meanings in an operand.
    reg_aliases: std::collections::HashMap<String, u32>,
    /// VFP register names declared with `DN` (double) and `SN` (single).
    vfp_aliases: std::collections::HashMap<String, (char, u32)>,
    /// FPA register names declared with `FN`. `FACC FN 0` is `f0`, and the
    /// maths sources name their working registers this way throughout.
    fpa_aliases: std::collections::HashMap<String, u32>,
    /// Coprocessor names, from `CP` (the coprocessor itself, `p15`) and `CN`
    /// (one of its registers, `c1`). The Kernel talks to CP15 entirely through
    /// these: `ARM_config_cp CP 15` and `ARM_control_reg CN 1`.
    cp_aliases: std::collections::HashMap<String, (char, u32)>,
    /// The base register the current `MAP` established, if it named one.
    map_base: Option<u32>,
    /// Symbols a `FIELD` defined under such a `MAP`, and the register they are
    /// relative to. `ADR Rd,Sym` on one of these is `ADD Rd,Rn,#offset`.
    field_bases: std::collections::HashMap<String, u32>,
    /// Every storage map, and the one fields go into now.
    maps: Vec<StorageMap>,
    cur_map: Option<usize>,
    /// How many macro expansions there have been, for naming them apart.
    expansions: usize,
    /// Whether the last thing placed in this area was data.
    ///
    /// The AOF spec says bit 8 of a symbol's attributes "encodes the `<code
    /// datum>` attribute ... It denotes that the symbol identifies a (usually
    /// read-only) datum, rather than an executable instruction." ObjAsm
    /// decides this by what it was doing when the label was defined. So a
    /// label after a string or a table is a datum, and one after an
    /// instruction is not. `ROUT` says a routine starts here, so what follows
    /// is code whatever came before it.
    in_data: bool,
    /// The length of the file the `INCBIN` being placed includes, for the
    /// location counter. The file is read once, where the line is made.
    included: Option<u32>,
    /// For each label, the value `in_data` had where it was defined: true
    /// for a datum, false for code.
    label_kinds: std::collections::HashMap<String, bool>,
    /// Labels standing at the current address that have emitted nothing.
    ///
    /// A label with no bytes of its own belongs to whatever comes next, so
    /// alignment inserted for the next instruction takes the label with it.
    /// BBCEconet writes a table of bytes, then `OpenRx ROUT`, then code. The
    /// branches to `OpenRx` are meant for the code, not for the last byte of
    /// the table, and there is no ALIGN in between to say so.
    fresh_labels: Vec<String>,
    /// The same, for local labels, as indices into `locals`.
    fresh_locals: Vec<usize>,
    /// Values waiting for the next `LTORG`.
    pending_literals: Vec<Literal>,
    /// Pools closed so far this pass, and the ones the previous pass closed.
    ///
    /// A literal is loaded from a pool that lies *ahead* of the instruction,
    /// so the instruction cannot know the address in the pass that places it.
    /// Pass one settles where every pool goes, and pass two reads it back.
    /// Local labels work the same way, for the same reason.
    pools: Vec<Pool>,
    pools_prev: Vec<Pool>,
    /// What was decided at each `LDR =`, in source order. `Some(pool, offset
    /// within it)` where a pool word was needed, and `None` where a `MOV` or
    /// `MVN` could do the job. This is carried from one pass to the next so
    /// that both lay out the same bytes, even where pass two could evaluate
    /// something pass one could not.
    literal_sites: Vec<Option<LiteralRef>>,
    literal_sites_prev: Vec<Option<LiteralRef>>,
    /// Words in pools already placed, as area, offset, and the text that made
    /// them. An `LDR Rd,=ZeroPage` after a pool that already holds ZeroPage
    /// loads from that one rather than asking for another.
    placed_literals: Vec<(usize, u32, String)>,
    /// Local label definitions: (ROUT scope, number, address).
    locals: Vec<LocalDef>,
    /// The assembly-time variables as the caller left them. These are the
    /// `-PD` predefines and the built-ins, and nothing the source declared.
    /// Pass two starts from these.
    initial_vars: std::collections::HashMap<String, Value>,
    /// The previous pass's local labels. A `%FT05` reference is resolved
    /// against these, because the definition it names may be hundreds of
    /// lines ahead. That is the reason ObjAsm has two passes.
    locals_prev: Vec<LocalDef>,
    /// Which pass is running. Assertions are reported in the second pass. The
    /// location counters they test hold the right value only at the point
    /// where the assertion sits. So they are checked as they are met during
    /// pass two, not gathered up and checked at the end.
    pass_no: u8,
    /// `EQU` definitions whose value referred to a symbol not yet defined.
    /// ObjAsm has two passes, so a header may use a constant a dozen lines
    /// before defining it. `Hdr:HighFSI` sets `OSFind_OpenIn * open_read` at
    /// line 299 and defines `open_read` at 310. These are tried again once
    /// the pass is complete.
    pending_equs: Vec<(String, String, usize)>,
    out: Vec<ExpandedLine>,
}

/// Strip one layer of surrounding double quotes from a macro argument.
///
/// This was tested against ObjAsm 4.08 running under RPCEmu, not guessed.
/// For a macro `Probe $s`, both `Probe abc` and `Probe "abc"` set `$s` to
/// `abc`, and `Probe "x,y"` passes a single argument `x,y`. The quotes at
/// the call site protect commas and are then removed. This is what makes the
/// corpus's common `[ "$str" <> ""` pattern work.
fn unquote_arg(s: &str) -> String {
    let t = s.trim();
    if t.len() >= 2 && t.starts_with('"') && t.ends_with('"') {
        t[1..t.len() - 1].to_string()
    } else {
        t.to_string()
    }
}

/// One value waiting to go into the next literal pool.
#[derive(Debug, Clone, PartialEq)]
struct Literal {
    /// The operand text after `=`. It is what the value is computed from, and
    /// what a relocation needs if it names a symbol.
    expr: String,
    /// Byte offset of this word within its pool.
    offset: u32,
    /// The area of the instruction that asked for it. A pool in another area
    /// is not at a fixed distance, so that is an error rather than a fixup.
    area: usize,
    /// Whether anything ended up loading from it.
    ///
    /// Pass one reserves a word for a value it cannot yet work out. Pass two
    /// may find that the value fits in an instruction and load it that way,
    /// so the word is reserved but never read. ObjAsm leaves such a word
    /// zero rather than filling in a value nothing asks for.
    used: bool,
    /// For a VFP floating-point literal, the whole words it occupies and
    /// their values. A double takes two. An integer or a symbol has `None`
    /// here and is evaluated from `expr` instead.
    bits: Option<Vec<u32>>,
}

impl Literal {
    fn words(&self) -> u32 {
        self.bits.as_ref().map_or(1, |b| b.len() as u32)
    }
}

/// Can an `LDR` at `here` reach a word at `there`?
///
/// The offset is twelve bits plus a sign. It is measured from `pc`, which
/// reads eight bytes past the instruction.
fn in_ldr_range(here: u32, there: u32) -> bool {
    let d = there as i64 - (here as i64 + 8);
    (-4095..=4095).contains(&d)
}

/// Can a `VLDR` at `here` reach a word at `there`?
///
/// The offset is eight bits plus a sign, counted in words and measured from
/// the same `pc`. That is about a kilobyte either way, word aligned.
fn in_vldr_range(here: u32, there: u32) -> bool {
    let d = there as i64 - (here as i64 + 8);
    (-1020..=1020).contains(&d) && d % 4 == 0
}

/// Where the word an `LDR Rd,=value` loads from is going to be.
#[derive(Debug, Clone, Copy, PartialEq)]
enum LiteralRef {
    /// In a pool this pass has not placed yet. Holds which pool, and where in
    /// it.
    Pending(usize, u32),
    /// In a pool already passed, at this offset in the area. The manual
    /// looks backwards first: "if no such literal already exists within the
    /// addressable range, place the literal in the next literal pool".
    Placed(u32),
}

/// A literal pool, once `LTORG` has said where it goes.
#[derive(Debug, Clone, Copy, PartialEq, Default)]
struct Pool {
    /// Offset of the first word within `area`.
    base: u32,
    /// Bytes the pool occupies.
    size: u32,
    area: usize,
}

/// A data item whose value is not known when the source is assembled.
///
/// `DCD ExternalThing` can only be filled in once the linker knows where
/// `ExternalThing` ended up, so the assembler leaves a zero in the field and
/// records this alongside it.
#[derive(Debug, Clone, PartialEq)]
pub struct DataFixup {
    /// Index into `Expander::areas` of the area holding the field.
    pub area: usize,
    /// Byte offset of the field within that area.
    pub offset: u32,
    /// Width of the field in bytes.
    pub width: u8,
    /// The operand text, for diagnostics.
    pub expr: String,
    pub kind: FixupKind,
}

/// What the linker has to add to a data field.
#[derive(Debug, Clone, PartialEq)]
pub enum FixupKind {
    /// The named symbol's value. Nothing here knows it.
    External(String),
    /// The base of one of our own areas. `DCD Label` holds Label's offset
    /// within its area, and the area's own address is added at link time.
    AreaBase(usize),
}

/// Render an evaluated immediate for the encoder.
///
/// Values are unsigned all the way through the evaluator, as the manual
/// requires: "the value of `0>-1` is `{FALSE}`". But an addressing offset is
/// signed. `[r13, #4294967292]` is out of range, while `[r13, #-4]` is what
/// the source wrote. So a value with the top bit set is written as negative.
pub fn immediate(v: u32) -> String {
    if v & 0x8000_0000 != 0 {
        format!("-{}", (v as i64 - 0x1_0000_0000i64).unsigned_abs())
    } else {
        format!("0x{v:X}")
    }
}

/// The symbols an expression mentions.
///
/// This is rough on purpose. It only has to find names, and an ObjAsm symbol
/// is made of letters, digits, `_` and `$`. A name inside a string would be
/// found in error, but a string is not an arithmetic operand.
pub fn identifiers(s: &str) -> Vec<String> {
    let mut out = Vec::new();
    let mut cur = String::new();
    // An operator is written between colons, as `:EOR:`, `:SHL:` or `:LNOT:`,
    // and the word inside one is not a name. Reading it as one made `DCD
    // &1A000000 :EOR: Cond_NE` ask the linker to relocate against a symbol
    // called EOR.
    let mut in_operator = false;
    // `?Label` is the size of the line that defined Label, which is a number.
    // So the name after `?` is not an address (the Wimp's
    // `DCB ?str_commands2`).
    let mut size_of = false;
    let mut chars = s.chars().peekable();
    while let Some(c) = chars.next() {
        if c == '?' && cur.is_empty() {
            size_of = true;
            continue;
        }
        if c == ':' {
            if !cur.is_empty() && !in_operator {
                out.push(std::mem::take(&mut cur));
            }
            cur.clear();
            in_operator = !in_operator;
            continue;
        }
        // `&` starts a hexadecimal number, and the digits after it are not a
        // name however they are spelled. BASIC has a label `DC` and a byte
        // `&DC` in one table. Reading the byte's digits as the label put a
        // relocation on the byte, which would have added the area's base to
        // a constant.
        if c == '&' {
            if !cur.is_empty() && !in_operator {
                out.push(std::mem::take(&mut cur));
            }
            while chars.peek().is_some_and(|d| d.is_ascii_hexdigit()) {
                chars.next();
            }
            continue;
        }
        if c.is_ascii_alphanumeric() || c == '_' || c == '$' {
            cur.push(c);
        } else if !cur.is_empty() {
            let t = std::mem::take(&mut cur);
            if !in_operator && !size_of {
                out.push(t);
            }
            size_of = false;
        } else if !c.is_whitespace() {
            size_of = false;
        }
    }
    if !cur.is_empty() && !in_operator && !size_of {
        out.push(cur);
    }
    // A leading digit means a number, not a name.
    out.retain(|t| !t.starts_with(|c: char| c.is_ascii_digit()));
    out
}

/// Rewrite the names in an expression, leaving quoted text alone.
///
/// A name may be written between bars, as in `|_Lib$Reloc$Off$DP|`. The bars
/// mark where the name begins and ends and are not part of it. They are
/// handled here so that a caller does not trip over them.
fn map_names(text: &str, mut f: impl FnMut(&str) -> Option<String>) -> String {
    let cs: Vec<char> = text.chars().collect();
    let mut out = String::with_capacity(text.len() + 16);
    let mut i = 0;
    while i < cs.len() {
        // A quoted string is not an expression.
        if cs[i] == '"' {
            out.push(cs[i]);
            i += 1;
            while i < cs.len() {
                out.push(cs[i]);
                i += 1;
                if cs[i - 1] == '"' {
                    break;
                }
            }
            continue;
        }
        // `&` and its hex digits are a number, and the digits are not a name
        // to rewrite. BASIC has both a label `DC` and a byte `&DC`.
        if cs[i] == '&' && cs.get(i + 1).is_some_and(|c| c.is_ascii_hexdigit()) {
            out.push(cs[i]);
            i += 1;
            while i < cs.len() && cs[i].is_ascii_hexdigit() {
                out.push(cs[i]);
                i += 1;
            }
            continue;
        }
        let (name, end, barred) = if cs[i] == '|' {
            let mut j = i + 1;
            while j < cs.len() && cs[j] != '|' {
                j += 1;
            }
            (cs[i + 1..j].iter().collect::<String>(), (j + 1).min(cs.len()), true)
        } else if cs[i].is_alphabetic() || cs[i] == '_' {
            let mut j = i;
            while j < cs.len() && (cs[j].is_alphanumeric() || "_$".contains(cs[j])) {
                j += 1;
            }
            (cs[i..j].iter().collect::<String>(), j, false)
        } else {
            out.push(cs[i]);
            i += 1;
            continue;
        };
        match f(&name) {
            Some(replacement) => out.push_str(&replacement),
            None if barred => out.push_str(&format!("|{name}|")),
            None => out.push_str(&name),
        }
        i = end;
    }
    out
}

/// The expression with the location counter, written `.` or `{PC}`, moved on
/// by `by`. A `.` inside a number or a name (`1.5`, `s.Wimp`) is not the
/// location counter.
fn shift_location(text: &str, by: u32) -> String {
    let cs: Vec<char> = text.chars().collect();
    let mut out = String::with_capacity(text.len() + 8);
    let mut i = 0;
    let part_of_word = |c: Option<&char>| c.is_some_and(|c| c.is_ascii_alphanumeric() || *c == '_' || *c == '.');
    while i < cs.len() {
        if cs[i] == '"' {
            out.push(cs[i]);
            i += 1;
            while i < cs.len() {
                out.push(cs[i]);
                i += 1;
                if cs[i - 1] == '"' {
                    break;
                }
            }
            continue;
        }
        if cs[i] == '.' && !part_of_word(i.checked_sub(1).and_then(|j| cs.get(j))) && !part_of_word(cs.get(i + 1)) {
            out.push_str(&format!("(.+{by})"));
            i += 1;
            continue;
        }
        if cs[i..].iter().take(4).collect::<String>().eq_ignore_ascii_case("{PC}") {
            out.push_str(&format!("({{PC}}+{by})"));
            i += 4;
            continue;
        }
        out.push(cs[i]);
        i += 1;
    }
    out
}

/// Where a data line stands, for the `.` and the local labels in its items.
struct DataSite {
    addr: u32,
    area: usize,
    rout: Option<String>,
}

/// The routine part of a local label's scope, without the expansion it was
/// written in. `%BA10` looks across every macro level, that is, across every
/// scope that belongs to the same routine.
fn routine_of(scope: &str) -> &str {
    scope.split(" in expansion ").next().unwrap_or(scope)
}

/// One local label definition: `10loop` or a bare `10`.
#[derive(Debug, Clone, PartialEq)]
struct LocalDef {
    /// The enclosing `ROUT`'s label, or the name written after the number.
    scope: String,
    number: u32,
    /// Offset within `area`.
    addr: u32,
    area: usize,
}

/// Every architecture ObjAsm knows, for the `{TARGET_ARCH_<name>}` family.
const ARCHITECTURE_NAMES: [&str; 26] = [
    "1", "2", "2A", "3", "3M", "4", "4T", "5XM", "5", "5T", "5TE", "5TEJ", "5TEWMMX",
    "5TEWMMX2", "6", "6K", "6T2", "6Z", "6_M", "6S_M", "7", "7_A", "7_R", "7_M", "7E_M",
    "8_A_32",
];

/// The architecture selected: ARMv8-A in AArch32 state, as on the Pi 4's
/// A72.
const SELECTED_ARCH: &str = "8_A_32";

/// How a directive brings in another source file.
#[derive(Debug, Clone, Copy, PartialEq)]
enum Chain {
    /// `GET`: assembly resumes after the directive when the file runs out.
    Nested,
    /// `LNK`: the current file ends here.
    Tail,
}

/// Directives that occupy no space, so a label on one has size zero.
fn is_zero_size(up: &str) -> bool {
    matches!(
        up,
        "EXPORT" | "IMPORT" | "EXTERN" | "GLOBAL" | "KEEP" | "ENTRY" | "DATA"
            | "ARM" | "CODE32" | "REQUIRE" | "EXPORTAS" | "STRONG" | "RN" | "CN" | "FN"
            | "DN" | "SN" | "CP" | "NOFP" | "OPT" | "TTL" | "SUBT" | "ALIGN" | "!"
            | "INFO"
    )
}

/// The symbol an `EXPORT`/`IMPORT` names, dropping any qualifiers.
///
/// `IMPORT name, WEAK` and `EXPORT name «,attr»` both put the name first.
/// What follows are attributes, not further symbols.
fn linkage_names(operands: &str) -> Vec<String> {
    let first = layout::split_top_level(operands)
        .into_iter()
        .next()
        .unwrap_or_default();
    let name = strip_bars(&first);
    if name.is_empty() { Vec::new() } else { vec![name] }
}

/// Remove the bars around a symbol. The bars only mark where the name begins
/// and ends, so `|C$$code|` names the symbol `C$$code`.
fn strip_bars(s: &str) -> String {
    let t = s.trim();
    match (t.strip_prefix('|'), t.strip_suffix('|')) {
        (Some(_), Some(_)) if t.len() >= 2 => t[1..t.len() - 1].to_string(),
        _ => t.to_string(),
    }
}

impl<'a> Expander<'a> {
    pub fn new(resolver: &'a dyn FileResolver) -> Self {
        let mut e = Expander {
            stack: Vec::new(),
            macros: Vec::new(),
            syms: SymTab::new(),
            resolver,
            conds: Vec::new(),
            map_counter: 0,
            rout: None,
            area: None,
            areas: Vec::new(),
            area_sizes: Vec::new(),
            label_defs: std::collections::HashMap::new(),
            exports: Vec::new(),
            imports: Vec::new(),
            data_fixups: Vec::new(),
            assert_warnings: false,
            fpa_to_vfp: false,
            reg_aliases: std::collections::HashMap::new(),
            vfp_aliases: std::collections::HashMap::new(),
            fpa_aliases: std::collections::HashMap::new(),
            cp_aliases: std::collections::HashMap::new(),
            map_base: None,
            field_bases: std::collections::HashMap::new(),
            maps: Vec::new(),
            cur_map: None,
            expansions: 0,
            in_data: false,
            included: None,
            label_kinds: std::collections::HashMap::new(),
            fresh_labels: Vec::new(),
            fresh_locals: Vec::new(),
            pending_literals: Vec::new(),
            pools: Vec::new(),
            pools_prev: Vec::new(),
            literal_sites: Vec::new(),
            literal_sites_prev: Vec::new(),
            placed_literals: Vec::new(),
            locals: Vec::new(),
            locals_prev: Vec::new(),
            initial_vars: std::collections::HashMap::new(),
            pending_equs: Vec::new(),
            pass_no: 1,
            out: Vec::new(),
        };
        // Both counters exist from the start. Without a MAP, `@` is zero, and
        // `.` is zero before the first AREA.
        e.set_builtin_at();
        e.set_builtin_dot();
        e.seed_registers();
        e
    }

    pub fn symbols(&self) -> &SymTab {
        &self.syms
    }

    /// Seed the built-in variables for the target.
    ///
    /// These are stored under their braced names, so `{CONFIG}` is looked up
    /// like any other symbol. `{PC}` and the location counters are left out
    /// on purpose. They depend on layout, and an error is better than a wrong
    /// answer.
    pub fn set_target_builtins(&mut self) {
        for (k, v) in [
            // --- what the assembler was told to target -------------------
            ("{ARCHITECTURE}", Value::Str("8-A.32".into())),
            ("{CPU}", Value::Str("Cortex-A72".into())),
            ("{FPU}", Value::Str("VFPv4".into())),
            ("{ENDIAN}", Value::Str("little".into())),
            ("{CODESIZE}", Value::Arith(32)),
            ("{CONFIG}", Value::Arith(32)),
            // Storing `pc` reads eight ahead on every ARM this targets.
            ("{PCSTOREOFFSET}", Value::Arith(8)),
            ("{OPT}", Value::Arith(0)),
            ("{ARMASM_VERSION}", Value::Arith(0)),
            ("{OBJASM_VERSION}", Value::Arith(0)),
            // Not UAL (the unified assembler syntax). The corpus is entirely
            // in the older syntax: no line in 630k puts the S before the
            // condition.
            ("{UAL}", Value::Logical(false)),

            // --- APCS qualifiers, none of which the ROM build uses --------
            ("{INTER}", Value::Logical(false)),
            ("{REENTRANT}", Value::Logical(false)),
            ("{ROPI}", Value::Logical(false)),
            ("{RWPI}", Value::Logical(false)),

            // --- instruction sets ----------------------------------------
            ("{TARGET_ARCH_ARM}", Value::Arith(8)),
            // AArch32 on an A72 has Thumb-2, which ObjAsm numbers 4.
            ("{TARGET_ARCH_THUMB}", Value::Arith(4)),

            // --- what the A72 can do -------------------------------------
            ("{TARGET_FEATURE_CLZ}", Value::Logical(true)),
            ("{TARGET_FEATURE_DIVIDE}", Value::Logical(true)),
            ("{TARGET_FEATURE_DOUBLEWORD}", Value::Logical(true)),
            ("{TARGET_FEATURE_DSPMUL}", Value::Logical(true)),
            ("{TARGET_FEATURE_MULTIPLY}", Value::Logical(true)),
            ("{TARGET_FEATURE_MULTIPROCESSING}", Value::Logical(true)),
            ("{TARGET_FEATURE_UNALIGNED}", Value::Logical(true)),
            ("{TARGET_FEATURE_NEON}", Value::Logical(true)),
            ("{TARGET_FEATURE_NEON_INTEGER}", Value::Logical(true)),
            ("{TARGET_FEATURE_NEON_FP32}", Value::Logical(true)),
            // Half precision on an A72 means conversion only, not arithmetic.
            // That is what VFPv4 provides and what this flag reports.
            ("{TARGET_FEATURE_NEON_FP16}", Value::Logical(true)),
            ("{TARGET_FEATURE_EXTENSION_REGISTER_COUNT}", Value::Arith(32)),

            // --- floating point ------------------------------------------
            // The target has VFP and NEON and no FPA. This port rests on that
            // decision. The FPA instructions in the sources have to become
            // VFP ones, and where the sources already offer both, these flags
            // select the VFP path.
            ("{TARGET_FPU_VFP}", Value::Logical(true)),
            ("{TARGET_FPU_FPA}", Value::Logical(false)),
            ("{TARGET_FPU_SOFTFPA}", Value::Logical(false)),
            ("{TARGET_FPU_SOFTFPA_FPA}", Value::Logical(false)),
            ("{TARGET_FPU_SOFTFPA_VFP}", Value::Logical(false)),
            ("{TARGET_FPU_SOFTVFP}", Value::Logical(false)),
            ("{TARGET_FPU_SOFTVFP_FPA}", Value::Logical(false)),
            ("{TARGET_FPU_SOFTVFP_VFP}", Value::Logical(false)),

            // --- position in the source ----------------------------------
            // The real values are set as each line is read. These stop a
            // reference before the first line from being an error.
            ("{AREANAME}", Value::Str(String::new())),
            ("{INPUTFILE}", Value::Str(String::new())),
            ("{LINENUM}", Value::Arith(0)),
            ("{LINENUMUP}", Value::Arith(0)),
            ("{LINENUMUPPER}", Value::Arith(0)),
        ] {
            let ty = match &v {
                Value::Arith(_) => Type::Arith,
                Value::Logical(_) => Type::Logical,
                Value::Str(_) => Type::Str,
            };
            self.syms.declare_global(k, ty);
            let _ = self.syms.set(k, v);
        }
        // `{TARGET_ARCH_<name>}` asks whether that exact architecture is the
        // one selected, so every name is false except ours.
        for a in ARCHITECTURE_NAMES {
            let k = format!("{{TARGET_ARCH_{a}}}");
            self.syms.declare_global(&k, Type::Logical);
            let _ = self.syms.set(&k, Value::Logical(a == SELECTED_ARCH));
        }
    }

    /// Apply one `-PD`/`-pd` command-line predefine, e.g.
    /// `APCS SETS "APCS-32"` or `International SETL {TRUE}`.
    ///
    /// The build passes a dozen of these. Without them, a great many
    /// conditionals refer to symbols that appear undefined.
    pub fn predefine(&mut self, text: &str) -> R<()> {
        let line = lex::lex_line(0, text);
        let (Some(name), Some(op)) = (line.label_str(), line.opcode_str()) else {
            return self.err(0, format!("malformed predefine '{text}'"));
        };
        let up = op.to_ascii_uppercase();
        let ty = match up.as_str() {
            "SETA" => Type::Arith,
            "SETL" => Type::Logical,
            "SETS" => Type::Str,
            _ => return self.err(0, format!("predefine must use SETA/SETL/SETS: '{text}'")),
        };
        let name = name.to_string();
        self.syms.declare_global(&name, ty);
        let rhs = line.operands_str().unwrap_or("").to_string();
        let v = match self.eval_expr(&rhs) {
            Ok(v) => v,
            Err(e) => return self.err(0, format!("in predefine '{text}': {e}")),
        };
        if let Err(e) = self.syms.set(&name, v) {
            return self.err(0, e.to_string());
        }
        Ok(())
    }

    fn origin(&self, line: usize) -> Origin {
        let mut macros = Vec::new();
        let mut file = String::from("<input>");
        for s in &self.stack {
            match s {
                Source::File { name, .. } => file = name.clone(),
                Source::Macro { name, file: f, .. } => {
                    macros.push(name.clone());
                    file = f.clone();
                }
                Source::Loop { file: f, .. } => file = f.clone(),
            }
        }
        Origin { file, line, macros }
    }

    fn err<T>(&self, line: usize, msg: impl Into<String>) -> R<T> {
        Err(ExpandError {
            msg: msg.into(),
            origin: self.origin(line),
        })
    }

    /// Are we inside a conditional branch that is not being assembled?
    fn skipping(&self) -> bool {
        self.conds.iter().any(|(active, _, _)| !active)
    }

    /// Assemble, in two passes, as ObjAsm does.
    ///
    /// The manual says: "ObjAsm is a two pass assembler -- it examines each
    /// source file twice." The first pass is there to find the symbols. The
    /// second produces the output with every value known. That is what lets
    /// a file use a label or constant defined further down.
    ///
    /// Only the symbol table survives between the passes. Everything else is
    /// rebuilt: the emitted lines, the area and its location counter, macro
    /// definitions and `ROUT` scopes. So the second pass is a true re-run,
    /// not a continuation. Assembly-time variables reset themselves, because
    /// declaring one again with `GBLA` and the like restores its default.
    /// That is also why the corpus can `GET` a header more than once.
    pub fn run(&mut self, file: &str, lines: Vec<String>) -> R<Vec<ExpandedLine>> {
        // Whatever the caller predefined is the baseline both passes see.
        self.initial_vars = self.syms.variables();
        self.pass_no = 1;
        self.pass(file, lines.clone())?;
        self.settle_pending_equs()?;

        self.reset_between_passes();

        self.pass_no = 2;
        self.pass(file, lines)?;
        self.settle_pending_equs()?;
        self.finalise_data_bytes();
        Ok(std::mem::take(&mut self.out))
    }

    /// One pass over the source.
    fn pass(&mut self, file: &str, lines: Vec<String>) -> R<()> {
        self.stack.push(Source::File {
            name: file.to_string(),
            lines: lex::lex(&lines),
            pos: 0,
        });
        while let Some(item) = self.next_line()? {
            self.step(item)?;
        }
        if let Some((_, _, o)) = self.conds.last() {
            let where_ = if o.macros.is_empty() {
                format!("{}:{}", o.file, o.line)
            } else {
                format!("{}:{} in {}", o.file, o.line, o.macros.join(" < "))
            };
            let depth = self.conds.len();
            let more = if depth > 1 { format!(", {} still open", depth) } else { String::new() };
            return self.err(0, format!("'[' at {where_} was never closed{more}"));
        }
        // The manual makes a missing END an error. Allowing it costs nothing,
        // while losing the pool would corrupt the output without a word.
        if !self.pending_literals.is_empty() {
            self.emit_pool(0)?;
        }
        Ok(())
    }

    /// The boundary a line has to start on.
    ///
    /// ObjAsm aligns automatically, and it has to. An ARM instruction is a
    /// word, and the processor fetches it from a word boundary. `DCD`
    /// allocates words "aligned on four-byte boundaries", and `DCDU` exists
    /// to avoid that. Only `DCB` packs into whatever byte comes next. That is
    /// why the sources can put a string in the middle of a routine and carry
    /// straight on into code with no `ALIGN` of their own.
    fn alignment_of(line: &Line) -> u32 {
        let Some(op) = line.opcode_str() else { return 1 };
        match op.to_ascii_uppercase().as_str() {
            // Bytes, and the directives that place none or place their own.
            "DCB" | "=" | "INCBIN" | "BIN" | "ALIGN" | "SPACE" | "%" | "EXPORT" | "IMPORT" | "EXTERN"
            | "GLOBAL" | "KEEP" | "ENTRY" | "DATA" | "ARM" | "CODE32" | "REQUIRE"
            | "EXPORTAS" | "STRONG" | "RN" | "CN" | "FN" | "DN" | "SN" | "CP"
            | "NOFP" | "OPT" | "TTL" | "SUBT" => 1,
            "DCW" => 2,
            _ => 4,
        }
    }

    /// Move the location counter to a boundary, emitting the padding.
    ///
    /// The padding has to reach the object file, not only the counter. The
    /// driver lays each line's bytes down in order. A gap that the counter
    /// knows about but the data does not would put everything after it two
    /// bytes early, and every branch past it would be out by two.
    fn pad_to(&mut self, boundary: u32, line_num: usize) {
        let Some(a) = self.area.as_ref() else { return };
        let (from, to) = (a.offset, layout::align_to(a.offset, boundary.max(1), 0));
        if to == from {
            return;
        }
        if let Some(a) = self.area.as_mut() {
            a.offset = to;
        }
        let area_index = self.current_area_index();
        // A label with nothing of its own moves with the alignment.
        for name in std::mem::take(&mut self.fresh_labels) {
            self.syms.define_absolute(&name, to);
            self.label_defs.insert(name, (area_index, to));
        }
        for i in std::mem::take(&mut self.fresh_locals) {
            if let Some(l) = self.locals.get_mut(i) {
                l.addr = to;
            }
        }
        let rout = self.local_scope();
        let origin = self.origin(line_num);
        self.out.push(ExpandedLine {
            operands: String::new(),
            text: String::new(),
            origin,
            addr: from,
            bytes: vec![0u8; (to - from) as usize],
            listing_only: false,
            area_index,
            rout,
            literal: None,
        });
        self.set_builtin_dot();
    }

    /// Place the pending pool as a line of its own, for an `END` or the end of
    /// the input, where there is no `LTORG` to hang it on.
    fn emit_pool(&mut self, line_num: usize) -> R<()> {
        self.pad_to(4, line_num);
        let addr = self.area.as_ref().map(|a| a.offset).unwrap_or(0);
        let area_index = self.current_area_index();
        let rout = self.local_scope();
        let origin = self.origin(line_num);
        let bytes = self.close_pool(line_num)?;
        self.out.push(ExpandedLine {
            operands: String::new(),
            text: String::new(),
            origin,
            addr,
            bytes,
            listing_only: false,
            area_index,
            rout,
            literal: None,
        });
        Ok(())
    }

    /// The areas this source declared, in order.
    pub fn areas(&self) -> &[(String, layout::AreaAttrs)] {
        &self.areas
    }

    /// How far each area's location counter reached, parallel to `areas()`.
    pub fn area_sizes(&self) -> &[u32] {
        &self.area_sizes
    }

    /// Where each label ended up, as (area index, offset within that area).
    pub fn label_defs(&self) -> &std::collections::HashMap<String, (usize, u32)> {
        &self.label_defs
    }

    /// Which labels name a datum rather than an instruction.
    pub fn label_kinds(&self) -> &std::collections::HashMap<String, bool> {
        &self.label_kinds
    }

    /// Names declared `EXPORT` or `GLOBAL`.
    pub fn exports(&self) -> &[String] {
        &self.exports
    }

    /// Names declared `IMPORT` or `EXTERN`.
    pub fn imports(&self) -> &[String] {
        &self.imports
    }

    /// FPA instructions will be converted to VFP (`--fpa-to-vfp`).
    pub fn set_fpa_to_vfp(&mut self, on: bool) {
        self.fpa_to_vfp = on;
    }

    /// Carry on past a failed `ASSERT`, reporting it. For listings.
    pub fn set_assert_warnings(&mut self, on: bool) {
        self.assert_warnings = on;
    }

    /// Data fields the linker has to fill in.
    pub fn data_fixups(&self) -> &[DataFixup] {
        &self.data_fixups
    }

    /// Every storage map the source defines, with its fields.
    pub fn storage_maps(&self) -> &[StorageMap] {
        &self.maps
    }

    /// Symbols that are relative to a base register, and which register.
    pub fn field_bases(&self) -> &std::collections::HashMap<String, u32> {
        &self.field_bases
    }

    /// Register names declared with `RN`.
    pub fn reg_aliases(&self) -> &std::collections::HashMap<String, u32> {
        &self.reg_aliases
    }

    /// VFP register names declared with `DN` or `SN`.
    pub fn vfp_aliases(&self) -> &std::collections::HashMap<String, (char, u32)> {
        &self.vfp_aliases
    }

    /// Find the local label a `%` reference names.
    ///
    /// The rules are from the manual. The number is 0..=99. `F` or `B` limits
    /// the search to forwards or backwards, and with neither the search goes
    /// both ways. The search never crosses a `ROUT` boundary. A routine name
    /// written after the number overrides the enclosing one.
    pub fn resolve_local(
        &self,
        reference: &str,
        here: u32,
        area: usize,
        rout: Option<&str>,
    ) -> Option<u32> {
        let r = layout::parse_local_ref(reference)?;
        let scope = r.routine.clone().or_else(|| rout.map(str::to_string));
        let table = if self.locals_prev.is_empty() {
            &self.locals
        } else {
            &self.locals_prev
        };
        // `%BT10` looks in this macro level only, and `%BA10` in all of them.
        // spinrw writes both, and labels its `MetaLock` invocations with a
        // `10` that the macro itself branches back to. Where the level is not
        // written, the sources expect the search to look outward.
        let this_level = r.level == layout::Level::This;
        let matching = |d: &&LocalDef| {
            let same_scope = match &scope {
                Some(s) if this_level => d.scope == *s,
                Some(s) => routine_of(&d.scope) == routine_of(s),
                // Outside any ROUT the scope is the empty one.
                None => d.scope.is_empty(),
            };
            d.number == r.number && d.area == area && same_scope
        };
        // Definition order is address order within an area, so "the nearest
        // one forwards" is the first at or after here.
        let forward = || table.iter().filter(matching).find(|d| d.addr >= here);
        let backward = || table.iter().filter(matching).rfind(|d| d.addr <= here);
        match r.dir {
            layout::Dir::Forward => forward().map(|d| d.addr),
            layout::Dir::Backward => backward().map(|d| d.addr),
            // Both ways. The manual searches backwards first.
            layout::Dir::Both => backward().or_else(forward).map(|d| d.addr),
        }
    }

    /// Evaluate an expression that has to come out as a number.
    ///
    /// `MOV r0, #"."` writes the character as a one-character string, not as
    /// the `'.'` the manual documents. The sources do this often enough that
    /// the conversion to a number belongs here. It belongs only here, where a
    /// number is the only thing that would make sense. Elsewhere `"a" = "b"`
    /// is a string comparison and must stay one.
    fn eval_immediate(&self, text: &str) -> Option<u32> {
        match self.eval_expr(text) {
            Ok(Value::Arith(n)) => Some(n),
            Ok(Value::Str(s)) if s.chars().count() == 1 => Some(s.chars().next()? as u32),
            _ => None,
        }
    }

    /// What one name in an operand becomes, or `None` to leave it alone.
    ///
    /// A name declared with `RN`, `DN`, `SN`, `FN`, `CP` or `CN` becomes the
    /// register it stands for. A label is left alone here. `fold_address`
    /// deals with labels: one in this same area is at a fixed distance, so it
    /// becomes an offset from `.` rather than a symbol the encoder would have
    /// to relocate. For a `VLDR` the encoder cannot relocate at all, and for
    /// an `ADR` we would rather do it ourselves. A label elsewhere keeps its
    /// name and becomes a relocation directive.
    fn resolve_name(&self, word: &str) -> Option<String> {
        // A label is not folded here. Where it stands for an address, it is
        // one term of an expression, as in `B SLVK + SWIRelocation`.
        // Rewriting that term alone would leave the rest measured from
        // somewhere else. `fold_address` takes the whole expression.
        if self.label_defs.contains_key(word) {
            return None;
        }
        if let Some(n) = self.reg_aliases.get(word) {
            // `r15` as a source operand is rejected on ARMv8. `pc` is the same
            // register under the name the encoder will take.
            return Some(if *n == 15 { "pc".into() } else { format!("r{n}") });
        }
        if let Some((kind, n)) = self.vfp_aliases.get(word) {
            return Some(format!("{kind}{n}"));
        }
        if let Some(n) = self.fpa_aliases.get(word) {
            // Written as the plain register name `fN`. Which VFP register it
            // becomes depends on the instruction's precision, and only the
            // FPA translation knows that.
            return Some(format!("f{n}"));
        }
        if let Some((kind, n)) = self.cp_aliases.get(word) {
            return Some(format!("{kind}{n}"));
        }
        None
    }

    /// Work out `:BASE: sym`, the register a storage map based its symbols on.
    ///
    /// The manual defines this operator on a register-relative expression,
    /// giving the register. `:INDEX:` on the same thing gives the offset from
    /// that register. The evaluator works in numbers, so by the time it holds
    /// a value the register is gone. `:INDEX:` is already right, because the
    /// value *is* the offset. `:BASE:` is worked out here instead, from the
    /// map that defined the symbol.
    ///
    /// The corpus uses it fifty-three times, always on one symbol. Nearly
    /// every use is `ASSERT (:BASE:CurrentContext) = Rwp`, which checks a
    /// workspace layout against the register the code actually uses. A name
    /// that came from no based map is left alone, so the evaluator still
    /// reports it as the error the manual says it is.
    fn substitute_bases(&self, text: &str) -> String {
        if !text.to_ascii_uppercase().contains(":BASE:") {
            return text.to_string();
        }
        let cs: Vec<char> = text.chars().collect();
        let mut out = String::with_capacity(text.len());
        let mut i = 0;
        while i < cs.len() {
            let rest: String = cs[i..].iter().take(6).collect();
            if !rest.eq_ignore_ascii_case(":BASE:") {
                out.push(cs[i]);
                i += 1;
                continue;
            }
            let mut j = i + 6;
            while j < cs.len() && cs[j].is_whitespace() {
                j += 1;
            }
            let start = j;
            if cs.get(j) == Some(&'|') {
                j += 1;
                while j < cs.len() && cs[j] != '|' {
                    j += 1;
                }
                j = (j + 1).min(cs.len());
            } else {
                while j < cs.len() && (cs[j].is_alphanumeric() || "_$".contains(cs[j])) {
                    j += 1;
                }
            }
            let name = strip_bars(&cs[start..j].iter().collect::<String>());
            match self.field_bases.get(&name) {
                Some(base) => {
                    out.push_str(&base.to_string());
                    i = j;
                }
                None => {
                    out.push(cs[i]);
                    i += 1;
                }
            }
        }
        out
    }

    /// Evaluate an expression the way this assembler's sources write them.
    ///
    /// Everything the evaluator cannot know on its own is settled first. At
    /// present that is only `:BASE:`. The rest of the expression language is
    /// handled by the evaluator itself.
    fn eval_expr(&self, text: &str) -> Result<Value, expr::EvalError> {
        let files = |name: &str| {
            let (path, ty) = self.resolver.host_file(name)?;
            file_info(&path, ty)
        };
        expr::eval_with(&self.substitute_bases(text), &self.syms, Some(&files))
    }

    /// Replace the location counter with this line's own address.
    ///
    /// `.` means where the assembler has got to. The symbol table's copy
    /// holds wherever it finished. That is right while a line is being
    /// expanded, but useless afterwards. An operand like
    /// `#(NaffSWI - (.+12))/4` is measured from the instruction that carries
    /// it, so the address is put in first.
    ///
    /// A `.` inside a number is left alone. That is what keeps `#0.5` a half
    /// rather than an address.
    fn substitute_dot(text: &str, here: u32) -> String {
        if !text.contains('.') {
            return text.to_string();
        }
        let part_of_name = |j: Option<&char>| {
            j.is_some_and(|c| c.is_ascii_alphanumeric() || *c == '_' || *c == '$')
        };
        let cs: Vec<char> = text.chars().collect();
        let mut out = String::with_capacity(text.len());
        let mut i = 0;
        while i < cs.len() {
            // A dot inside a string is a full stop. `MOV r0,#"."` asks for the
            // character, not for where the assembler has got to.
            if cs[i] == '"' || cs[i] == '\'' {
                let quote = cs[i];
                out.push(cs[i]);
                i += 1;
                while i < cs.len() {
                    out.push(cs[i]);
                    i += 1;
                    if cs[i - 1] == quote {
                        break;
                    }
                }
                continue;
            }
            if cs[i] == '.'
                && !part_of_name(cs.get(i.wrapping_sub(1)))
                && !part_of_name(cs.get(i + 1))
            {
                out.push_str(&here.to_string());
            } else {
                out.push(cs[i]);
            }
            i += 1;
        }
        out
    }

    /// Replace every local label reference in an expression with its address.
    ///
    /// The evaluator knows symbols, not local labels. It does not know where
    /// it is, and `%BT01` means nothing without that. So local labels are
    /// replaced before evaluation, in the places where the location is known.
    /// That is how `ASSERT . - %BT01 = ...` can be checked at all, and the
    /// Kernel checks its ARM operation tables that way.
    fn substitute_locals(&self, text: &str, here: u32, area: usize, rout: Option<&str>) -> String {
        if !text.contains('%') {
            return text.to_string();
        }
        let rout = rout.map(str::to_string);
        let cs: Vec<char> = text.chars().collect();
        let mut out = String::with_capacity(text.len());
        let mut i = 0;
        while i < cs.len() {
            if cs[i] != '%' {
                out.push(cs[i]);
                i += 1;
                continue;
            }
            let start = i;
            i += 1;
            while i < cs.len() && (cs[i].is_ascii_alphanumeric() || cs[i] == '_') {
                i += 1;
            }
            let text: String = cs[start..i].iter().collect();
            match self.resolve_local(&text, here, area, rout.as_deref()) {
                Some(a) => out.push_str(&a.to_string()),
                None => out.push_str(&text),
            }
        }
        out
    }

    /// Fold an operand that names an address into an offset from `.`.
    ///
    /// A branch or a literal load takes one address. It is written as an
    /// expression that may be a bare label or may be built from one: `B SLVK`,
    /// but also `B callback_checking + SWIRelocation` and `LDR r0, Table`. The
    /// distance to it from here is fixed, so the whole expression is worked
    /// out once and becomes a single offset. That is what the encoder will
    /// take, and it saves the encoder a relocation it could not resolve
    /// anyway.
    ///
    /// This is done only when every label in the expression is in this same
    /// area. A label elsewhere has no fixed distance from here, so the name
    /// is left for the linker.
    fn fold_address(&self, group: &str, line: &ExpandedLine) -> Option<String> {
        let t = group.trim();
        // An address, not an immediate, a register list or an addressing mode.
        if t.is_empty() || t.starts_with(['#', '[', '{', '=', '"']) {
            return None;
        }
        let names = identifiers(t);
        if !names.iter().any(|n| self.label_defs.contains_key(n)) {
            return None;
        }
        if names
            .iter()
            .filter_map(|n| self.label_defs.get(n))
            .any(|(a, _)| *a != line.area_index)
        {
            return None;
        }
        let text = self.substitute_locals(t, line.addr, line.area_index, line.rout.as_deref());
        let text = Self::substitute_dot(&text, line.addr);
        match self.eval_expr(&text) {
            Ok(Value::Arith(v)) => {
                let d = v as i64 - line.addr as i64;
                Some(if d < 0 {
                    format!(".-{}", -d)
                } else {
                    format!(".+{d}")
                })
            }
            _ => None,
        }
    }

    /// Rewrite `LDR Rd, sym` as `LDR Rd, [Rbase, #offset]` when `sym` came
    /// from a register-relative storage map.
    ///
    /// The manual says a label defined under `MAP expr,Rn` is an offset from
    /// that register, not an address. The sources reach their workspace by
    /// naming such a label where an address goes, as in `STR r0, NextPump`
    /// for a field of the block `wp` points at. If left alone, the name reads
    /// as an external symbol and the load becomes program-relative. That is
    /// not the same instruction and does not give the same answer.
    ///
    /// An expression may mix in ordinary absolute symbols, as
    /// `scratchbuffer1 + ms_action` does. But it may not name two symbols
    /// based on different registers, which has no meaning.
    fn fold_based_address(&self, group: &str, line: &ExpandedLine) -> Option<String> {
        let t = group.trim();
        if t.is_empty() || t.starts_with(['#', '[', '{', '=', '"', '\'']) {
            return None;
        }
        let names = identifiers(t);
        let bases: Vec<u32> = names
            .iter()
            .filter_map(|n| self.field_bases.get(n).copied())
            .collect();
        let base = *bases.first()?;
        if !bases.iter().all(|b| *b == base) {
            return None;
        }
        let text = self.substitute_locals(t, line.addr, line.area_index, line.rout.as_deref());
        let text = Self::substitute_dot(&text, line.addr);
        match self.eval_expr(&text) {
            Ok(Value::Arith(v)) => Some(format!("[r{base}, #{}]", v as i32)),
            _ => None,
        }
    }

    /// Rewrite an instruction's operands into something the encoder can parse.
    ///
    /// This is where the work is divided. ObjAsm's expression language, its
    /// symbol table and its register aliases are handled here, and only
    /// registers and numbers reach LLVM. In detail:
    ///
    /// * `#expr` and `=expr` are evaluated here. `#(a - b) :AND: 0xFF` has
    ///   no meaning to LLVM, and neither does a symbol from a header.
    /// * `|Odd$Name|` becomes a quoted symbol. The GNU syntax accepts that,
    ///   and it keeps the name unchanged for the relocation to find.
    /// * A name declared with `RN` becomes its register.
    ///
    /// An expression that will not evaluate is left alone, so the encoder's
    /// complaint names the symbol that is actually missing.
    pub fn encoder_operands(&self, line: &ExpandedLine, op: &str, operands: &str) -> String {
        // `ADR` also takes a register-relative symbol, but it does arithmetic
        // on the base rather than loading from it. So it keeps the bare name,
        // and the driver decides. Every other instruction that names one
        // wants the memory.
        let addressing = !crate::lower::is_adr(op) && !crate::lower::is_adrl(op);
        // The address an instruction refers to is always its last operand. It
        // is folded as a whole before anything else looks at the text.
        let groups = layout::split_top_level(operands);
        let operands = match groups.last().and_then(|g| {
            self.fold_address(g, line)
                .or_else(|| addressing.then(|| self.fold_based_address(g, line)).flatten())
        }) {
            Some(folded) => {
                let mut v: Vec<String> = groups[..groups.len() - 1].to_vec();
                v.push(folded);
                v.join(",")
            }
            None => operands.to_string(),
        };
        let operands: &str = &operands;
        let cs: Vec<char> = operands.chars().collect();
        let mut out = String::with_capacity(operands.len());
        let mut i = 0;
        while i < cs.len() {
            match cs[i] {
                '"' => {
                    out.push('"');
                    i += 1;
                    while i < cs.len() {
                        out.push(cs[i]);
                        i += 1;
                        if cs[i - 1] == '"' {
                            break;
                        }
                    }
                }
                '\'' => {
                    // A character constant: `'A'` is one operand item.
                    out.push('\'');
                    i += 1;
                    while i < cs.len() && cs[i] != '\'' {
                        out.push(cs[i]);
                        i += 1;
                    }
                    if i < cs.len() {
                        out.push('\'');
                        i += 1;
                    }
                }
                '|' => {
                    let mut name = String::new();
                    i += 1;
                    while i < cs.len() && cs[i] != '|' {
                        name.push(cs[i]);
                        i += 1;
                    }
                    i += 1; // the closing bar
                    // The bars only mark the ends of the name, so what they
                    // hold is a name like any other. `|_kernel_malloc|` is a
                    // label the file defines, and is treated the same way.
                    // Only a name this file does not define stays quoted.
                    // The quotes let the encoder accept the characters in it.
                    match self.resolve_name(&name) {
                        Some(text) => out.push_str(&text),
                        None => {
                            out.push('"');
                            out.push_str(&name);
                            out.push('"');
                        }
                    }
                }
                '#' | '=' => {
                    let lead = cs[i];
                    i += 1;
                    let start = i;
                    let mut depth = 0i32;
                    while i < cs.len() {
                        match cs[i] {
                            // A comma inside a string is part of it, not a
                            // separator: `SWI XOS_WriteI+","` is one operand.
                            '"' | '\'' => {
                                let quote = cs[i];
                                i += 1;
                                while i < cs.len() && cs[i] != quote {
                                    i += 1;
                                }
                            }
                            '(' | '[' | '{' => depth += 1,
                            ')' | '}' => depth -= 1,
                            // A closing bracket at the top level ends the
                            // operand. `[r0, #4]` is addressing, not arithmetic.
                            ']' if depth == 0 => break,
                            ']' => depth -= 1,
                            ',' if depth == 0 => break,
                            _ => {}
                        }
                        i += 1;
                    }
                    let text: String = cs[start..i].iter().collect();
                    // `#(%30-%10):SHR:2` measures the distance between two
                    // local labels, so they have to become numbers before the
                    // evaluator sees them.
                    let text = self.substitute_locals(
                        &text,
                        line.addr,
                        line.area_index,
                        line.rout.as_deref(),
                    );
                    let text = Self::substitute_dot(&text, line.addr);
                    // An ARM immediate is eight bits rotated by an even
                    // amount, and ObjAsm lets the two be written separately.
                    // `AND lr, lr, #all,wanted` is `all` rotated right by
                    // `wanted`. The rotation is always the last operand, and
                    // it is even and under 32. That is what tells it apart
                    // from an operand that merely follows.
                    //
                    // A register is not a rotation, whatever its number.
                    // `SSAT r2, #16, r2` saturates into r2, and the third
                    // operand is the register it works on. Taking it for a
                    // rotation turns it into `#0x10, 2`, which the encoder
                    // refuses. An odd-numbered register would have gone
                    // through untouched, which is worse.
                    let mut rotation = None;
                    if lead == '#' && i < cs.len() && cs[i] == ',' {
                        let rest: String = cs[i + 1..].iter().collect();
                        let rest = rest.trim().to_string();
                        let a_register = self.reg_aliases.contains_key(rest.as_str())
                            || Self::numbered_register(&rest).is_some();
                        if !rest.contains(',') && !a_register {
                            if let Some(r) = self.eval_immediate(&rest) {
                                if r < 32 && r % 2 == 0 {
                                    rotation = Some(r);
                                    i = cs.len();
                                }
                            }
                        }
                    }
                    match self.eval_immediate(text.trim()) {
                        // `=` introduces a 32-bit pattern to be loaded, so it
                        // is written unsigned. `#` is often an addressing
                        // offset, where the sign is what makes it legal.
                        Some(n) if lead == '=' => out.push_str(&format!("={n:#X}")),
                        Some(n) => {
                            out.push_str(&format!("{lead}{}", immediate(n)));
                            if let Some(r) = rotation {
                                out.push_str(&format!(", {r}"));
                            }
                        }
                        None => {
                            out.push(lead);
                            out.push_str(&text);
                        }
                    }
                }
                '%' => {
                    // A local label reference, such as `%FT05loop`. The
                    // distance to it is fixed, so it becomes an offset from
                    // `.`, which the encoder reads as the instruction's own
                    // address.
                    let start = i;
                    i += 1;
                    while i < cs.len() && (cs[i].is_ascii_alphanumeric() || cs[i] == '_') {
                        i += 1;
                    }
                    let text: String = cs[start..i].iter().collect();
                    match self.resolve_local(
                        &text,
                        line.addr,
                        line.area_index,
                        line.rout.as_deref(),
                    ) {
                        Some(t) => {
                            let d = t as i64 - line.addr as i64;
                            out.push_str(&if d < 0 {
                                format!(".-{}", -d)
                            } else {
                                format!(".+{d}")
                            });
                        }
                        None => out.push_str(&text),
                    }
                }
                c if c.is_ascii_alphabetic() || c == '_' => {
                    let start = i;
                    while i < cs.len()
                        && (cs[i].is_ascii_alphanumeric() || cs[i] == '_' || cs[i] == '$')
                    {
                        i += 1;
                    }
                    let word: String = cs[start..i].iter().collect();
                    match self.resolve_name(&word) {
                        Some(text) => out.push_str(&text),
                        None => out.push_str(&word),
                    }
                }
                c => {
                    out.push(c);
                    i += 1;
                }
            }
        }
        out
    }

    /// Index of the area a line belongs to.
    fn current_area_index(&self) -> usize {
        match &self.area {
            Some(a) => self
                .areas
                .iter()
                .position(|(n, _)| *n == a.name)
                .unwrap_or(0),
            None => 0,
        }
    }

    /// Discard everything the second pass rebuilds, keeping the symbols the
    /// first pass discovered.
    fn reset_between_passes(&mut self) {
        self.out.clear();
        self.stack.clear();
        self.macros.clear();
        self.conds.clear();
        // Keep these. Pass two resolves forward references against pass one.
        self.locals_prev = std::mem::take(&mut self.locals);
        self.pools_prev = std::mem::take(&mut self.pools);
        // Both passes walk every literal pool and every data directive, so
        // the fixups they find would otherwise be recorded twice.
        self.data_fixups.clear();
        self.literal_sites_prev = std::mem::take(&mut self.literal_sites);
        self.pending_literals.clear();
        self.placed_literals.clear();
        // Every `GBLx` runs again in pass two, so the variables it declared
        // must go. Otherwise a header's `[ :LNOT: :DEF: Included_Hdr_Foo ]`
        // guard finds itself already set, and the whole header, macros and
        // all, is skipped the second time round.
        self.syms.set_variables(self.initial_vars.clone());
        self.pending_equs.clear();
        // Count from zero again, so an expansion has the same name in both
        // passes and pass two can find what pass one wrote.
        self.expansions = 0;
        self.settle_labels();
        // Pass two lays every area out again from the start.
        self.area_sizes.fill(0);
        // Pass two reads every EXPORT and IMPORT again. Keeping the first
        // pass's copies would name each symbol twice in the object.
        self.exports.clear();
        self.imports.clear();
        self.area = None;
        self.map_counter = 0;
        self.rout = None;
        while self.syms.depth() > 0 {
            self.syms.pop_frame();
        }
        self.set_builtin_at();
        self.set_builtin_dot();
    }

    /// Take the next raw line from the innermost source. Sources that have
    /// run out are removed, and loop bodies are run again as required.
    fn next_line(&mut self) -> R<Option<Line>> {
        loop {
            let Some(top) = self.stack.last_mut() else {
                return Ok(None);
            };
            match top {
                Source::File { lines, pos, .. } | Source::Macro { lines, pos, .. } => {
                    if *pos < lines.len() {
                        let l = lines[*pos].clone();
                        *pos += 1;
                        return Ok(Some(l));
                    }
                    if matches!(top, Source::Macro { .. }) {
                        self.syms.pop_frame();
                    }
                    self.stack.pop();
                }
                Source::Loop {
                    lines,
                    pos,
                    while_line,
                    wend_line,
                    ..
                } => {
                    if *pos < lines.len() {
                        let l = lines[*pos].clone();
                        *pos += 1;
                        return Ok(Some(l));
                    }
                    // The body has finished. ObjAsm lists the WEND, then the
                    // WHILE again for the next test, even when that test fails.
                    let whl = while_line.clone();
                    let wend = wend_line.clone();
                    if let Some(w) = wend {
                        self.list_line(&w);
                    }
                    self.list_line(&whl);
                    // `$var` in the test is substituted afresh each time,
                    // because the body usually changes the variable
                    // (`WHILE :LEN:"$s" > 0`).
                    let cond = self.expand_text(whl.operands_str().unwrap_or(""));
                    let keep = self.eval_logical(&cond, whl.num)?;
                    if let Some(Source::Loop { pos, .. }) = self.stack.last_mut() {
                        if keep {
                            *pos = 0;
                        } else {
                            self.stack.pop();
                        }
                    }
                }
            }
        }
    }

    fn eval_logical(&self, src: &str, line: usize) -> R<bool> {
        match self.eval_expr(src) {
            Ok(Value::Logical(b)) => Ok(b),
            Ok(v) => self.err(line, format!("expected a logical value, got {v:?}")),
            Err(e) => self.err(line, e.to_string()),
        }
    }

    fn step(&mut self, line: Line) -> R<()> {
        let opcode = line.opcode_str().unwrap_or("").to_string();
        let up = opcode.to_ascii_uppercase();

        // ObjAsm lists blank and comment-only lines too. They carry the
        // current address and no bytes.
        if line.kind != Kind::Statement {
            if !self.skipping() {
                self.list_line(&line);
            }
            return Ok(());
        }

        // While skipping, only the directives that change conditional nesting
        // are looked at. Nothing else is parsed, because the corpus contains
        // malformed expressions that assemble only because they are skipped.
        if self.skipping() {
            match up.as_str() {
                "[" | "IF" => self.conds.push((false, true, self.origin(line.num))),
                "|" | "ELSE" => {
                    // Turn on only if every enclosing level is itself active.
                    // An ELSE inside a skipped outer branch stays skipped.
                    if !self.conds.is_empty() {
                        let outer_ok = self.conds[..self.conds.len() - 1].iter().all(|(a, _, _)| *a);
                        let (active, taken, _) = self.conds.last_mut().unwrap();
                        if outer_ok && !*taken {
                            *active = true;
                            *taken = true;
                        } else {
                            *active = false;
                        }
                    }
                }
                "ELIF" => {
                    // This is the one directive whose condition has to be
                    // evaluated while skipping, because it decides whether
                    // the skip ends here. It is evaluated only when every
                    // enclosing level is active and no earlier branch has
                    // been taken. That is also the only case where the
                    // expression is sure to be meaningful.
                    if !self.conds.is_empty() {
                        let outer_ok = self.conds[..self.conds.len() - 1].iter().all(|(a, _, _)| *a);
                        let taken = self.conds.last().unwrap().1;
                        let b = if outer_ok && !taken {
                            let cond = self.expand_text(line.operands_str().unwrap_or(""));
                            self.eval_logical(&cond, line.num)?
                        } else {
                            false
                        };
                        let (active, taken, _) = self.conds.last_mut().unwrap();
                        *active = b;
                        *taken |= b;
                    }
                }
                "]" | "ENDIF" => {
                    self.conds.pop();
                }
                _ => {}
            }
            return Ok(());
        }

        // A line whose opcode is a variable becomes a directive once it is
        // substituted. The Kernel writes `$GetMEMM`, and the build sets that
        // to `GET Hdr:MEMM.VMSAv6`, which chooses the page-table format for
        // the machine. ObjAsm substitutes before it decides what a line is,
        // so the line is expanded and read again before it is acted on. This
        // comes after the skipping check, because a skipped line may name a
        // variable that was never declared.
        let (line, up) = if opcode.starts_with('$') {
            let text = self.expand_text(&line.raw);
            let relexed = lex::lex_line(line.num, &text);
            let up = relexed.opcode_str().unwrap_or("").to_ascii_uppercase();
            (relexed, up)
        } else {
            (line, up)
        };

        // ObjAsm's listing carries every line, directives included, so record
        // them here. `emit_or_invoke` records its own, with bytes.
        if !matches!(up.as_str(), "" ) && !self.is_invocation_or_instruction(&up) {
            let text = self.expand_text(&line.raw);
            let addr = self.area.as_ref().map(|a| a.offset).unwrap_or(0);
            let origin = self.origin(line.num);
            // A SETA/SETL/SETS shows its resulting value in the byte column.
            // `assign` fills that in once the value is known.
            let area_index = self.current_area_index();
            let rout = self.local_scope();
            self.out.push(ExpandedLine {
                operands: String::new(),
                text,
                origin,
                addr,
                bytes: Vec::new(),
                listing_only: true,
                area_index,
                rout,
                literal: None,
            });
        }

        match up.as_str() {
            "[" | "IF" => {
                // Substitute before evaluating, for `[ $on = 1` inside a
                // macro.
                let cond = self.expand_text(line.operands_str().unwrap_or(""));
                let b = self.eval_logical(&cond, line.num)?;
                self.conds.push((b, b, self.origin(line.num)));
            }
            "|" | "ELSE" => match self.conds.last_mut() {
                Some((active, taken, _)) => {
                    *active = !*taken;
                    if *active {
                        *taken = true;
                    }
                }
                None => return self.err(line.num, "'|' without a matching '['"),
            },
            // Reached only with this level active, so an earlier branch was
            // taken and this one cannot be.
            "ELIF" => match self.conds.last_mut() {
                Some((active, _, _)) => *active = false,
                None => return self.err(line.num, "ELIF without a matching IF"),
            },
            "]" | "ENDIF" => {
                if self.conds.pop().is_none() {
                    return self.err(line.num, "']' without a matching '['");
                }
            }
            "GET" | "INCLUDE" => self.do_get(&line, Chain::Nested)?,
            "LNK" => self.do_get(&line, Chain::Tail)?,
            "MACRO" => self.define_macro(&line)?,
            "WHILE" => self.do_while(&line)?,
            "WEND" => return self.err(line.num, "WEND without WHILE"),
            "MEXIT" => self.do_mexit(&line)?,
            "MEND" => return self.err(line.num, "MEND outside a macro definition"),
            "GBLA" | "GBLL" | "GBLS" | "LCLA" | "LCLL" | "LCLS" => self.declare(&line, &up)?,
            "SETA" | "SETL" | "SETS" => self.assign(&line, &up)?,
            // `END` stops this file. If the file was reached by GET, assembly
            // resumes after the GET in the including file.
            "END" => {
                // "A default LTORG is executed at every END directive which is
                // not part of a nested assembly". So the end of a file read by
                // GET places no pool, but the end of the top-level one does.
                if self.stack.len() == 1 && !self.pending_literals.is_empty() {
                    self.emit_pool(line.num)?;
                }
                self.pop_source();
            }
            // Absolute symbol definition. These must happen during expansion,
            // not afterwards, because conditionals test them.
            "*" | "EQU" => self.define_equ(&line)?,
            // Storage maps. `^` sets the @ counter, and `#` reserves space and
            // names it.
            "^" | "MAP" => self.do_map(&line)?,
            "#" | "FIELD" => self.do_field(&line)?,
            "ROUT" => self.do_rout(&line),
            "!" | "INFO" => self.do_info(&line)?,
            "AREA" => self.do_area(&line)?,
            // Register/coprocessor/FP register names. The manual notes ObjAsm
            // "still permits register names in expressions (they are
            // automatically converted to the register number)", which is why
            // `ASSERT Rregno <> OP1sue` in regnames/s works at all.
            "RN" | "CN" | "FN" | "DN" | "SN" | "CP" => self.do_regname(&line)?,
            "ASSERT" => self.do_assert(&line)?,
            // Listing only. The manual's OPT table is entirely about what
            // appears in the listing, such as page throws, line numbering, and
            // whether macro expansions are shown. Nothing reaches the object.
            "OPT" | "TTL" | "SUBT" | "NOFP" => {}
            _ => self.emit_or_invoke(line)?,
        }
        Ok(())
    }

    // ---- directives ------------------------------------------------------

    fn declare(&mut self, line: &Line, up: &str) -> R<()> {
        let Some(name) = line.operands_str() else {
            return self.err(line.num, format!("{up} needs a variable name"));
        };
        let name = self.expand_text(name.trim());
        let ty = match &up[3..] {
            "A" => Type::Arith,
            "L" => Type::Logical,
            _ => Type::Str,
        };
        if up.starts_with("GBL") {
            self.syms.declare_global(&name, ty);
        } else if let Err(e) = self.syms.declare_local(&name, ty) {
            return self.err(line.num, e.to_string());
        }
        Ok(())
    }

    fn assign(&mut self, line: &Line, up: &str) -> R<()> {
        let Some(name) = line.label_str() else {
            return self.err(line.num, format!("{up} needs a variable in the label field"));
        };
        let name = self.expand_text(name);
        let rhs = self.expand_text(line.operands_str().unwrap_or(""));
        let v = match self.eval_expr(&rhs) {
            Ok(v) => v,
            Err(e) => return self.err(line.num, e.to_string()),
        };
        // The declared type rules. A mismatch is reported rather than
        // quietly converted.
        let shown = v.clone();
        if let Err(e) = self.syms.set(&name, v) {
            return self.err(line.num, e.to_string());
        }
        // ObjAsm shows an assignment's resulting value in the byte column of
        // the line `step` has already listed.
        if let (Value::Arith(n), Some(last)) = (&shown, self.out.last_mut()) {
            last.bytes = n.to_le_bytes().to_vec();
        }
        Ok(())
    }

    /// Directives handled inside `step` are listed there. Everything else
    /// goes through `emit_or_invoke`, which records its own line and bytes.
    fn is_invocation_or_instruction(&self, up: &str) -> bool {
        !matches!(
            up,
            "[" | "IF" | "|" | "ELSE" | "ELIF" | "]" | "ENDIF" | "GET" | "INCLUDE" | "LNK"
                | "MACRO" | "WHILE" | "WEND" | "MEXIT" | "MEND" | "GBLA" | "GBLL" | "GBLS"
                | "LCLA" | "LCLL" | "LCLS" | "SETA" | "SETL" | "SETS" | "END"
                | "*" | "EQU" | "^" | "MAP" | "#" | "FIELD" | "ROUT" | "AREA"
                | "RN" | "CN" | "FN" | "DN" | "SN" | "CP" | "ASSERT" | "OPT" | "TTL"
                | "SUBT" | "NOFP" | "!" | "INFO"
        )
    }

    /// Expand `<Name>` in a filename from the assembly-time variables. The
    /// real system looks these up in the RISC OS system variables. Here the
    /// same names are set by `-PD`, so they are read from the symbol table.
    fn substitute_angle_vars(&self, s: &str) -> String {
        let mut out = String::new();
        let mut rest = s;
        while let Some(i) = rest.find('<') {
            let Some(j) = rest[i..].find('>') else { break };
            let name = &rest[i + 1..i + j];
            out.push_str(&rest[..i]);
            match self.syms.get(name) {
                Some(Value::Str(v)) => out.push_str(v),
                Some(Value::Arith(n)) => out.push_str(&n.to_string()),
                _ => {
                    out.push('<');
                    out.push_str(name);
                    out.push('>');
                }
            }
            rest = &rest[i + j + 1..];
        }
        out.push_str(rest);
        out
    }

    /// Record a line in the listing without emitting anything.
    fn list_line(&mut self, line: &Line) {
        let text = self.expand_text(&line.raw);
        let addr = self.area.as_ref().map(|a| a.offset).unwrap_or(0);
        let origin = self.origin(line.num);
        let area_index = self.current_area_index();
        self.out.push(ExpandedLine {
            operands: String::new(),
            text,
            origin,
            addr,
            bytes: Vec::new(),
            listing_only: true,
            area_index,
            rout: self.local_scope(),
            literal: None,
        });
    }

    /// Like `take_until`, but also hands back the closing line, so that a
    /// `WEND` can be listed on every iteration.
    fn take_until_capturing(
        &mut self,
        at: usize,
        open: &str,
        close: &str,
    ) -> R<(Vec<Line>, Option<Line>)> {
        let mut body = Vec::new();
        let mut depth = 1usize;
        loop {
            let Some(l) = self.next_line()? else {
                return self.err(at, format!("{open} without {close}"));
            };
            let up = l.opcode_str().unwrap_or("").to_ascii_uppercase();
            if up == open {
                depth += 1;
            } else if up == close {
                depth -= 1;
                if depth == 0 {
                    return Ok((body, Some(l)));
                }
            }
            body.push(l);
        }
    }

    /// Pop the innermost source, unwinding a macro frame if that is what it is.
    fn pop_source(&mut self) {
        if let Some(Source::Macro { .. }) = self.stack.last() {
            self.syms.pop_frame();
        }
        self.stack.pop();
    }

    /// Define an absolute symbol, from `sym * expr` or `sym EQU expr`.
    fn define_equ(&mut self, line: &Line) -> R<()> {
        let Some(name) = line.label_str() else {
            return self.err(line.num, "EQU needs a symbol in the label field");
        };
        let name = strip_bars(&self.expand_text(name));
        let rhs = self.expand_text(line.operands_str().unwrap_or(""));
        self.inherit_base(&name, &rhs);
        match self.eval_expr(&rhs) {
            Ok(Value::Arith(v)) => {
                self.syms.define_absolute(&name, v);
                self.inherit_place(&name, &rhs, v);
                Ok(())
            }
            // A logical or string EQU is legal. Keep it with the variables, so
            // that later expressions can still read it.
            Ok(v) => {
                let ty = match &v {
                    Value::Logical(_) => Type::Logical,
                    _ => Type::Str,
                };
                self.syms.declare_global(&name, ty);
                let _ = self.syms.set(&name, v);
                Ok(())
            }
            // A forward reference. Keep it and resolve it once the pass ends.
            Err(e) if e.msg.contains("undefined symbol") => {
                self.pending_equs.push((name, rhs, line.num));
                Ok(())
            }
            Err(e) => self.err(line.num, e.to_string()),
        }
    }

    /// Recompute data bytes now that every symbol is known.
    ///
    /// This is the second half of having two passes. A `DCD` whose operand
    /// referred forward was evaluated during the pass, when the symbol had no
    /// value. Addresses and sizes never depended on that value, because
    /// sizing counts items, not values. So only the byte column needs to be
    /// worked out again.
    fn finalise_data_bytes(&mut self) {
        let mut out = std::mem::take(&mut self.out);
        for l in out.iter_mut() {
            if l.listing_only || l.bytes.is_empty() {
                continue;
            }
            // Lex again from the operands saved when the line was made, not
            // from the line as it reads now. `DCD Proc_LocalStack` has to
            // hold the value that variable had here, not the one the last
            // macro left in it.
            let relexed = lex::lex_line(l.origin.line, &l.text);
            let Some(op) = relexed.opcode_str().map(|s| s.to_string()) else { continue };
            let relexed = lex::lex_line(l.origin.line, &format!("        {op} {}", l.operands));
            let op: &str = &op;
            if matches!(
                op.to_ascii_uppercase().as_str(),
                "DCD" | "DCB" | "DCW" | "DCQ" | "DCI" | "&" | "="
            ) {
                let site = DataSite {
                    addr: l.addr,
                    area: l.area_index,
                    rout: l.rout.clone(),
                };
                let (bytes, holes) = self.data_bytes_with_holes(&relexed, Some(&site));
                l.bytes = bytes;
                for (off, width, expr, resolved, before_dot) in holes {
                    // A value that evaluated is still not final if it came
                    // from a label. What it holds is an offset into an area
                    // the linker has yet to place.
                    let kind = if resolved {
                        let areas: Vec<usize> = identifiers(&expr)
                            .iter()
                            .filter_map(|n| self.label_defs.get(n).map(|(a, _)| *a))
                            .collect();
                        match areas.first() {
                            // Only if the value moves when the area does.
                            // `Initialise - Module_BaseAddr` is a distance,
                            // and stays the same wherever the area lands.
                            Some(a) if areas.iter().all(|x| x == a) => {
                                let moves = match &before_dot {
                                    Some(text) => self.moves_with_area_at(text, *a, l.addr),
                                    None => self.moves_with_area(&expr, *a),
                                };
                                if !moves {
                                    continue;
                                }
                                FixupKind::AreaBase(*a)
                            }
                            Some(_) => {
                                eprintln!(
                                    "rosasm: {}:{}: `{expr}` spans more than one area",
                                    l.origin.file, l.origin.line
                                );
                                continue;
                            }
                            None => continue,
                        }
                    } else {
                        // Use the name the linker has to match, not the
                        // text it was written as. `DCD |Image$$RO$$Base|`
                        // asks for `Image$$RO$$Base`. The bars only mark the
                        // ends of the name, and no symbol table carries
                        // them. Use only a name the object actually imports.
                        // Taking the first identifier when none is imported
                        // invents a relocation against a macro's own
                        // variable, which no linker can match and no listing
                        // explains.
                        match identifiers(&expr)
                            .into_iter()
                            .find(|n| self.imports.contains(n))
                        {
                            Some(n) => FixupKind::External(n),
                            None => continue,
                        }
                    };
                    self.data_fixups.push(DataFixup {
                        area: l.area_index,
                        offset: l.addr + off,
                        width,
                        expr,
                        kind,
                    });
                }
            }
        }
        self.out = out;
    }

    /// Resolve `EQU`s that referred forward, repeating while progress is made.
    ///
    /// ObjAsm has two passes, so a header may use a constant well before
    /// defining it. `Hdr:HighFSI` sets `OSFind_OpenIn * open_read` at line 299
    /// and defines `open_read` at line 310. A single pass cannot see that. So
    /// the unresolved definitions are set aside and tried again until no more
    /// can be resolved.
    fn settle_pending_equs(&mut self) -> R<()> {
        loop {
            let before = self.pending_equs.len();
            for (name, rhs, num) in std::mem::take(&mut self.pending_equs) {
                match self.eval_expr(&rhs) {
                    Ok(Value::Arith(v)) => self.syms.define_absolute(&name, v),
                    Ok(v) => {
                        let ty = match &v {
                            Value::Logical(_) => Type::Logical,
                            _ => Type::Str,
                        };
                        self.syms.declare_global(&name, ty);
                        let _ = self.syms.set(&name, v);
                    }
                    Err(_) => self.pending_equs.push((name, rhs, num)),
                }
            }
            if self.pending_equs.is_empty() {
                return Ok(());
            }
            // No progress this round, so the rest are truly undefined.
            if self.pending_equs.len() == before {
                let (name, rhs, num) = self.pending_equs[0].clone();
                return self.err(num, format!("'{name}' cannot be resolved: {rhs}"));
            }
        }
    }

    /// Set the storage-map counter `@`, from `^ expr«,base-register»`.
    fn do_map(&mut self, line: &Line) -> R<()> {
        let operands = self.expand_text(line.operands_str().unwrap_or(""));
        let parts = layout::split_top_level(&operands);
        let base = parts.first().map(|s| s.trim().to_string()).unwrap_or_default();
        let v = if base.is_empty() {
            0
        } else {
            match self.eval_expr(&base) {
                Ok(Value::Arith(n)) => n,
                Ok(_) => return self.err(line.num, "MAP needs an arithmetic origin"),
                Err(e) => return self.err(line.num, e.to_string()),
            }
        };
        self.map_counter = v;
        // `MAP expr,Rn` makes every symbol a following FIELD defines relative
        // to Rn, until the next MAP says otherwise.
        self.map_base = parts.get(1).and_then(|r| self.register_number(r.trim()));
        // A second pass, or a macro expanded again, defines the same map.
        // It replaces what the first one said.
        let origin = self.origin(line.num);
        let map = StorageMap { file: origin.file, line: origin.line, start: v, base: self.map_base, fields: vec![] };
        self.cur_map = match self.maps.iter().position(|m| m.file == map.file && m.line == map.line) {
            Some(i) => {
                self.maps[i] = map;
                Some(i)
            }
            None => {
                self.maps.push(map);
                Some(self.maps.len() - 1)
            }
        };
        self.set_builtin_at();
        Ok(())
    }

    /// Whether a word is a register by its own spelling: `r0`..`r15`, or one
    /// of the names the encoder uses. Aliases are not looked at here, because
    /// the caller checks them separately. The named registers give 0, as the
    /// caller asks only whether the word is a register.
    fn numbered_register(word: &str) -> Option<u32> {
        let w = word.trim().to_ascii_lowercase();
        match w.as_str() {
            "pc" | "lr" | "sp" | "ip" | "fp" | "sl" => Some(0),
            _ => w.strip_prefix('r')?.parse::<u32>().ok().filter(|n| *n < 16),
        }
    }

    /// The register a name stands for, whether written `r12`, `R12` or as an
    /// `RN` alias. Storage maps are nearly always based on an alias, as in
    /// `^ 0, wp`, so reading only the numbered spelling would miss them all.
    fn register_number(&self, word: &str) -> Option<u32> {
        if let Some(n) = self.reg_aliases.get(word) {
            return Some(*n);
        }
        word.trim_start_matches(['R', 'r'])
            .parse::<u32>()
            .ok()
            .filter(|n| *n < 16)
    }

    /// Handle `«sym» # expr`. Give the symbol the current `@`, then advance
    /// `@` by the size.
    fn do_field(&mut self, line: &Line) -> R<()> {
        let operands = self.expand_text(line.operands_str().unwrap_or(""));
        let size = match self.eval_expr(&operands) {
            Ok(Value::Arith(n)) => n,
            Ok(_) => return self.err(line.num, "FIELD needs an arithmetic size"),
            Err(e) => return self.err(line.num, e.to_string()),
        };
        if let Some(label) = line.label_str() {
            let name = strip_bars(&self.expand_text(label));
            if !name.is_empty() {
                self.syms.define_absolute(&name, self.map_counter);
                // `?symbol` is the space the line defining it reserved, and a
                // FIELD reserves its size. The Kernel checks its workspace
                // this way: `ASSERT ?LargeCommon >= SpriteCBsize + ...`.
                self.syms.define_absolute(&format!("?{name}"), size);
                // Under a `MAP expr,Rn` the symbol is an offset from Rn, not
                // an address, and `ADR` on it has to take account of that.
                if let Some(base) = self.map_base {
                    self.field_bases.insert(name.clone(), base);
                }
                if let Some(m) = self.cur_map.and_then(|i| self.maps.get_mut(i)) {
                    m.fields.retain(|f| f.0 != name);
                    m.fields.push((name.clone(), self.map_counter, size));
                }
            }
        }
        self.map_counter = self.map_counter.wrapping_add(size);
        self.set_builtin_at();
        Ok(())
    }

    /// Keep `@` readable from expressions.
    fn set_builtin_at(&mut self) {
        for k in ["{@}", "{VAR}"] {
            self.syms.declare_global(k, Type::Arith);
            let _ = self.syms.set(k, Value::Arith(self.map_counter));
        }
    }

    /// Name a register, from `name RN expr`. `CP`, `CN`, `FN`, `DN` and `SN`
    /// do the same for coprocessors, coprocessor registers, FPA registers
    /// and VFP registers.
    fn do_regname(&mut self, line: &Line) -> R<()> {
        let Some(label) = line.label_str() else {
            return self.err(line.num, "RN needs a name in the label field");
        };
        let name = strip_bars(&self.expand_text(label));
        let rhs = self.expand_text(line.operands_str().unwrap_or(""));
        match self.eval_expr(&rhs) {
            Ok(Value::Arith(n)) => {
                self.syms.define_absolute(&name, n);
                // Only `RN` names an ARM register. A coprocessor or
                // floating-point register number is not one, and must not be
                // substituted as one.
                match line.opcode_str().unwrap_or("").to_ascii_uppercase().as_str() {
                    "RN" => {
                        self.reg_aliases.insert(name, n);
                    }
                    "DN" => {
                        self.vfp_aliases.insert(name, ('d', n));
                    }
                    "SN" => {
                        self.vfp_aliases.insert(name, ('s', n));
                    }
                    "FN" => {
                        self.fpa_aliases.insert(name, n);
                    }
                    "CP" => {
                        self.cp_aliases.insert(name, ('p', n));
                    }
                    "CN" => {
                        self.cp_aliases.insert(name, ('c', n));
                    }
                    _ => {}
                }
                Ok(())
            }
            Ok(_) => self.err(line.num, "RN needs a register number"),
            Err(e) => self.err(line.num, e.to_string()),
        }
    }

    /// Set up the standard register names, so that `RN` can be written in
    /// terms of them and register names work in expressions.
    fn seed_registers(&mut self) {
        // The APCS names, which ObjAsm declares in advance when no register
        // option is given. These are ARM core registers, so they are also
        // substituted in operands: `v1` has to reach the encoder as `r4`.
        const APCS: [(&str, u32); 19] = [
            ("a1", 0), ("a2", 1), ("a3", 2), ("a4", 3),
            ("v1", 4), ("v2", 5), ("v3", 6), ("v4", 7), ("v5", 8),
            ("v6", 9), ("v7", 10), ("v8", 11),
            ("sb", 9), ("sl", 10), ("fp", 11), ("ip", 12),
            ("sp", 13), ("lr", 14), ("pc", 15),
        ];
        for i in 0..16u32 {
            self.syms.define_absolute(&format!("R{i}"), i);
            self.syms.define_absolute(&format!("r{i}"), i);
        }
        for (n, v) in APCS {
            self.syms.define_absolute(n, v);
            self.syms.define_absolute(&n.to_ascii_uppercase(), v);
            // A source is free to declare any of these again with `RN`,
            // which overwrites the entry.
            self.reg_aliases.insert(n.to_string(), v);
            self.reg_aliases.insert(n.to_ascii_uppercase(), v);
        }
        // Coprocessor, FPA, VFP and Advanced SIMD register names. These are
        // spellings the encoder already accepts, so they need values for
        // expressions but no substitution.
        for i in 0..8u32 {
            self.syms.define_absolute(&format!("F{i}"), i);
            self.syms.define_absolute(&format!("f{i}"), i);
            self.syms.define_absolute(&format!("acc{i}"), i);
            self.fpa_aliases.insert(format!("F{i}"), i);
            self.fpa_aliases.insert(format!("f{i}"), i);
        }
        for i in 0..16u32 {
            self.syms.define_absolute(&format!("C{i}"), i);
            self.syms.define_absolute(&format!("c{i}"), i);
            self.syms.define_absolute(&format!("p{i}"), i);
            self.syms.define_absolute(&format!("Q{i}"), i);
            self.syms.define_absolute(&format!("q{i}"), i);
        }
        for i in 0..32u32 {
            self.syms.define_absolute(&format!("D{i}"), i);
            self.syms.define_absolute(&format!("d{i}"), i);
            self.syms.define_absolute(&format!("S{i}"), i);
            self.syms.define_absolute(&format!("s{i}"), i);
        }
    }

    /// `«label» ROUT` opens a new scope for local labels and closes the
    /// previous one.
    ///
    /// The label is an ordinary label as well as the scope's name. The
    /// sources often write `Go ROUT` and then `BL Go` from elsewhere.
    fn do_rout(&mut self, line: &Line) {
        // A routine starts here, so what follows is code, whatever came
        // before it in the area.
        self.in_data = false;
        self.define_label(line);
        // Substituted, because the label is almost always a macro parameter.
        // `Entry` writes `$label ROUT`, and 3,493 routines in the corpus are
        // named that way rather than by a literal label.
        self.rout = line
            .label_str()
            .map(|s| strip_bars(&self.expand_text(s)))
            .filter(|s| !s.is_empty());
    }

    /// `AREA name«,attr»...` starts a section. The location counter starts
    /// again, or resumes for an area already seen.
    fn do_area(&mut self, line: &Line) -> R<()> {
        // A pool is reached by a fixed offset from the instruction that loads
        // from it, so it has to be in the same area. So the pending pool is
        // placed here, as the old area is left. That is what lets SDFS's
        // `freeveneer` write `LDR a2,=free_stack_relocations` in its code
        // area and then start a data area without an LTORG of its own.
        if !self.pending_literals.is_empty() {
            self.emit_pool(line.num)?;
        }
        let operands = self.expand_text(line.operands_str().unwrap_or(""));
        match layout::parse_area(&operands) {
            Ok((name, attrs)) => {
                // A second AREA with the same name continues the first, as
                // ObjAsm allows. Otherwise this is a new one. Going back into
                // an area resumes its location counter where it was left,
                // which is what "continues the first" means.
                let offset = match self.areas.iter().position(|(n, _)| *n == name) {
                    Some(i) => self.area_sizes[i],
                    None => {
                        self.areas.push((name.clone(), attrs.clone()));
                        self.area_sizes.push(0);
                        0
                    }
                };
                self.syms.declare_global("{AREANAME}", Type::Str);
                let _ = self
                    .syms
                    .set("{AREANAME}", Value::Str(name.clone()));
                // The area's own name is a symbol standing at its base, so
                // `DCD Init - |Asm$$Code|` is the offset of Init. sdcmos
                // writes its module header that way.
                let index = self.current_area_index();
                self.syms.define_absolute(&name, 0);
                self.label_defs.insert(name.clone(), (index, 0));
                self.area = Some(layout::Area { name, attrs, offset });
                // A new area starts with nothing before it, so a label at
                // the top of one does not stand after data. scheduler puts
                // `Pollword_PreEmpted` first in `RO$$Code`, and ObjAsm does
                // not call it a datum. It does call the labels on the words
                // after it data.
                self.in_data = false;
                self.settle_labels();
                self.set_builtin_dot();
                Ok(())
            }
            Err(e) => self.err(line.num, e),
        }
    }

    /// Keep `.` readable from expressions, as an offset within the area.
    fn set_builtin_dot(&mut self) {
        let v = self.area.as_ref().map(|a| a.offset).unwrap_or(0);
        // The counter only moves through here, so this is where the furthest
        // point each area reaches is recorded.
        let i = self.current_area_index();
        if self.area.is_some() {
            if let Some(sz) = self.area_sizes.get_mut(i) {
                *sz = (*sz).max(v);
            }
        }
        for k in ["{.}", "{PC}"] {
            self.syms.declare_global(k, Type::Arith);
            let _ = self.syms.set(k, Value::Arith(v));
        }
    }

    /// Advance the location counter past whatever this emitted line occupies,
    /// and give any label on it the address it now has.
    ///
    /// Sizing is exact for data directives and for ARM instructions, which are
    /// always four bytes on this target.
    fn advance(&mut self, line: &Line) {
        let Some(op) = line.opcode_str() else {
            // A label on its own line takes the current address.
            self.define_label(line);
            return;
        };
        let up = op.to_ascii_uppercase();
        let operands = line.operands_str().unwrap_or("");
        let before = self.area.as_ref().map(|a| a.offset).unwrap_or(0);

        // The label is placed before the line's own bytes.
        self.define_label(line);

        // `?label` is "the number of bytes generated by the line defining
        // label", so record the size against the label on this line.
        if let (Some(raw), Some(n)) = (line.label_str(), self.data_size_here(&up, operands)) {
            let name = strip_bars(raw);
            if !name.is_empty() {
                self.syms.define_absolute(&format!("?{name}"), n);
            }
        } else if let Some(raw) = line.label_str() {
            let name = strip_bars(raw);
            if !name.is_empty() && !is_zero_size(&up) {
                self.syms.define_absolute(&format!("?{name}"), 4);
            }
        }

        match up.as_str() {
            // Linkage directives name symbols the object file must carry.
            // Pass two repeats them, so each list is cleared between passes.
            "EXPORT" | "GLOBAL" => self.exports.extend(linkage_names(operands)),
            "IMPORT" | "EXTERN" => self.imports.extend(linkage_names(operands)),
            _ => {}
        }

        // Worked out before the area is borrowed, because the match below
        // holds it mutably.
        let fpa_words = crate::fpa::words(&up, self.fpa_to_vfp);

        // `SPACE`'s operand is the only one read here. Reading it borrows the
        // symbol table, so it happens before the area is taken. It is done
        // only for `SPACE`, because every instruction in the corpus comes
        // through here, and evaluating their operands would be wasted work.
        let size = matches!(up.as_str(), "SPACE" | "%")
            .then(|| self.eval_expr(operands))
            .transpose();
        // Read before the area is taken, for the same reason. Counting a
        // `DCB` of a string means reading the symbol table.
        let reserved = self.data_size_here(&up, operands);
        let Some(area) = self.area.as_mut() else { return };
        if let Some(n) = reserved {
            area.offset = area.offset.wrapping_add(n);
            if n != 0 {
                self.in_data = true;
                self.settle_labels();
            }
            self.set_builtin_dot();
            return;
        }
        match up.as_str() {
            "ALIGN" => {
                let parts = layout::split_top_level(operands);
                let boundary = parts
                    .first()
                    .and_then(|s| s.trim().parse::<u32>().ok())
                    .unwrap_or(4);
                let plus = parts
                    .get(1)
                    .and_then(|s| s.trim().parse::<u32>().ok())
                    .unwrap_or(0);
                area.offset = layout::align_to(area.offset, boundary.max(1), plus);
            }
            "SPACE" | "%" => {
                // This was evaluated before the area was taken mutably,
                // because reading the expression reads the symbol table.
                if let Ok(Some(Value::Arith(n))) = size {
                    area.offset = area.offset.wrapping_add(n);
                }
            }
            // Directives that emit nothing.
            "EXPORT" | "IMPORT" | "EXTERN" | "GLOBAL" | "KEEP" | "ENTRY" | "DATA"
            | "ARM" | "CODE32" | "REQUIRE" | "EXPORTAS" | "STRONG" | "RN" | "CN" | "FN"
            | "DN" | "SN" | "CP" | "NOFP" | "OPT" | "TTL" | "SUBT" => {}
            // `LTORG` puts the pool here, word aligned. Its size is whatever
            // the literals asked for since the last one.
            "LTORG" => {
                area.offset = layout::align_to(area.offset, 4, 0);
            }
            // `ADRL` is a pseudo-instruction that always occupies two
            // instructions, whether or not the offset would fit in one.
            _ if crate::lower::is_adrl(&up) => area.offset = area.offset.wrapping_add(8),
            // An FPA instruction is one word on an FPA. On this target it
            // becomes VFP, and a compare becomes two instructions. So the
            // location counter has to ask how many words, not assume one.
            _ if fpa_words.is_some() => {
                area.offset = area.offset.wrapping_add(4 * fpa_words.unwrap_or(1))
            }
            // Anything else that reaches here is an ARM instruction.
            _ => {
                area.offset = area.offset.wrapping_add(4);
                self.in_data = false;
            }
        }
        // We are past this line's bytes, so any label on it is no longer
        // waiting for something to belong to.
        if self.area.as_ref().map(|a| a.offset) != Some(before) {
            self.settle_labels();
        }
        self.set_builtin_dot();
    }

    /// Bytes have been placed, so the labels standing over them are theirs
    /// and no longer move.
    fn settle_labels(&mut self) {
        self.fresh_labels.clear();
        self.fresh_locals.clear();
    }

    /// Bytes a data directive reserves, counted the way it will be written.
    ///
    /// `layout::data_size` counts items. That is right for everything except
    /// a `DCB` that names a string variable. SerialSpt writes `DCB
    /// ErrorString_Serial_NoSerialDevice`, and that is three characters, not
    /// one byte. The sizing has to agree with the bytes. Otherwise the layout
    /// drifts by the difference, and every error block after it lands two
    /// bytes late.
    fn data_size_here(&self, up: &str, operands: &str) -> Option<u32> {
        if matches!(up, "INCBIN" | "BIN") {
            return self.included;
        }
        let plain = layout::data_size(up, operands)?;
        if !matches!(up, "DCB" | "=") || !operands.contains(|c: char| c.is_alphabetic()) {
            return Some(plain);
        }
        let mut total = 0u32;
        for item in layout::split_top_level(operands) {
            let t = item.trim();
            if t.is_empty() {
                continue;
            }
            total += match self.eval_expr(t) {
                Ok(Value::Str(text)) if !t.starts_with('"') => text.len() as u32,
                _ => layout::data_size(up, t).unwrap_or(1),
            };
        }
        Some(total)
    }

    /// Evaluate a data directive's operands into the bytes it emits.
    ///
    /// ObjAsm resolves these itself, and it must. `DCD @`, `DCD 1 :SHL: 4` and
    /// `DCD ?Label` have no equivalent that a later assembler could compute.
    /// The values go into the byte column. The source text is left alone,
    /// which is what ObjAsm's own listing does.
    ///
    /// An item that cannot be evaluated is usually an external symbol that
    /// needs a relocation. It still fills its full width, as the caller's
    /// sizing expects. The bytes hold the rest of the expression with each
    /// imported name taken as zero, or zero if even that will not evaluate.
    fn data_bytes(&self, line: &Line) -> Vec<u8> {
        self.data_bytes_with_holes(line, None).0
    }

    /// Like `data_bytes`, but also reports every item that names something
    /// rather than being a plain constant. Each is given as `(offset within
    /// these bytes, width, operand text, did it evaluate, text before `.` was
    /// replaced)`. The last is `None` when no `DataSite` was given. An item
    /// that evaluated may still need relocating, because a label's value is
    /// an offset into an area.
    #[allow(clippy::type_complexity)]
    fn data_bytes_with_holes(
        &self,
        line: &Line,
        at: Option<&DataSite>,
    ) -> (Vec<u8>, Vec<(u32, u8, String, bool, Option<String>)>) {
        let Some(op) = line.opcode_str() else { return (Vec::new(), Vec::new()) };
        let up = op.to_ascii_uppercase();
        let width = match up.as_str() {
            "DCB" | "=" => 1usize,
            "DCW" => 2,
            "DCD" | "&" | "DCI" => 4,
            "DCQ" => 8,
            _ => {
                // `DCFD`/`DCFS` were sized in layout but emitted nothing
                // here. So twenty-two floating-point constants in the
                // corpus's BASIC dropped out of its object, while every
                // label after them stayed where the location counter said.
                // Their value is the bits of a literal. Nothing relocates a
                // fraction.
                if up == "DCFD" || up == "DCFS" {
                    let mut out = Vec::new();
                    if let Some(operands) = line.operands_str() {
                        for item in layout::split_top_level(operands) {
                            let t = item.trim();
                            if t.is_empty() {
                                continue;
                            }
                            if up == "DCFD" {
                                let v: f64 = t.parse().unwrap_or(0.0);
                                out.extend_from_slice(&v.to_bits().to_le_bytes());
                            } else {
                                let v: f32 = t.parse().unwrap_or(0.0);
                                out.extend_from_slice(&v.to_bits().to_le_bytes());
                            }
                        }
                    }
                    return (out, Vec::new());
                }
                return (Vec::new(), Vec::new());
            }
        };
        let Some(operands) = line.operands_str() else {
            return (Vec::new(), Vec::new());
        };

        let mut out = Vec::new();
        let mut holes = Vec::new();
        for item in layout::split_top_level(operands) {
            let t = item.trim();
            if t.is_empty() {
                continue;
            }
            // A string in a DCB gives its characters, with `""` standing for
            // one quote.
            if width == 1 && t.starts_with('"') {
                // Take one quote off each end only. `DCB """", 0` is an
                // escaped quote, and taking every quote leaves an empty
                // string.
                let inner = t
                    .strip_prefix('"')
                    .and_then(|s| s.strip_suffix('"'))
                    .unwrap_or(t);
                out.extend(lex::string_chars(inner).chars().map(|c| c as u8));
                continue;
            }
            // `.` in a data item is the address of the line it is on, and a
            // local label reference needs its scope. This is the same
            // treatment an instruction's operands get. DADebug builds a
            // branch word by hand:
            // `DCD &1A000000 :EOR: Cond_NE + ((%FT01 - (. + 8))/4)`.
            let owned;
            let mut before_dot = None;          /* for moves_with_area_at */
            let t: &str = match at {
                Some(site) => {
                    let s =
                        self.substitute_locals(t, site.addr, site.area, site.rout.as_deref());
                    owned = Self::substitute_dot(&s, site.addr);
                    before_dot = Some(s);
                    &owned
                }
                None => t,
            };
            // A `DCB` of a string variable lays down its characters, the
            // same as a quoted string. SerialSpt builds its error table from
            // names a macro has set to `"E01", 0` and the like.
            if width == 1 {
                if let Ok(Value::Str(text)) = self.eval_expr(t) {
                    out.extend(text.bytes());
                    continue;
                }
            }
            let v = match self.eval_expr(t) {
                Ok(Value::Arith(n)) => {
                    if !identifiers(t).is_empty() {
                        holes.push((out.len() as u32, width as u8, t.to_string(), true, before_dot.clone()));
                    }
                    n as u64
                }
                // Not an error. This is usually an imported symbol, whose
                // value only the linker knows. The bytes hold the space, and
                // the hole records what has to go there.
                _ => {
                    holes.push((out.len() as u32, width as u8, t.to_string(), false, before_dot.clone()));
                    // An imported symbol has no value here, but the rest of
                    // the expression has one, and the relocation adds to it.
                    match self.eval_expr(&self.without_imports(t)) {
                        Ok(Value::Arith(n)) => n as u64,
                        _ => 0,
                    }
                }
            };
            for b in 0..width {
                out.push(((v >> (8 * b)) & 0xFF) as u8);
            }
        }
        (out, holes)
    }


    // -------------------------------------------------------- literal pools

    /// Is this an `LDR Rd,=expression`, and if so what follows the `=`?
    ///
    /// The manual gives the whole family: `LDR`, and the byte, halfword,
    /// signed and doubleword forms, in either suffix order.
    fn literal_operand(line: &Line) -> Option<String> {
        let op = line.opcode_str()?.to_ascii_uppercase();
        let operands = line.operands_str()?;
        let (_, rest) = operands.split_once('=')?;
        // `=` introduces a literal only in the last operand position. An
        // addressing mode never contains one.
        if operands.split_once('=')?.0.contains('[') {
            return None;
        }
        let rest = rest.trim();
        if op.starts_with("LDR") {
            return Some(rest.to_string());
        }
        // `LDFS f1, =5729.57795` asks the same pool for the same thing, so
        // it takes the same path. But it is given as the bits of the value,
        // because a pool holds words and this value was written as a
        // fraction.
        if let Some(f) = crate::fpa::parse(&op) {
            return (f.stem == "LDF")
                .then(|| crate::fpa::literal_bits(f.precision?, rest))
                .flatten();
        }
        // A VFP `FLDD d0,=3.14...` goes in the pool the same way, as two
        // words for a double. The exception is a value one of VFP's eight-bit
        // immediates can hold. Then legalization makes a `VMOV` and nothing
        // goes in the pool. A zero goes in the pool too, because ObjAsm will
        // not make an immediate of it.
        if let Some(v) = crate::vfp::parse(&op) {
            if matches!(v.stem, "FLDD" | "FLDS") && !crate::vfp::literal_is_immediate(v.stem, rest)
            {
                return Some(rest.to_string());
            }
        }
        None
    }

    /// Does this literal always take a pool word?
    ///
    /// An `LDR` uses a `MOV` where the value fits one, which is the manual's
    /// order. An FPA load has no such form. Its own immediates are eight
    /// fixed constants written `#1`, not `=1`. So it goes to the pool,
    /// whatever its bits look like as an ARM immediate.
    fn always_pooled(line: &Line) -> bool {
        line.opcode_str()
            .map(|o| o.to_ascii_uppercase())
            .is_some_and(|o| !o.starts_with("LDR"))
    }

    /// Decide what an `LDR Rd,=expression` does, and reserve a pool word if it
    /// needs one.
    ///
    /// The manual gives this order. Use a `MOV` if the value fits one. If not,
    /// use an `MVN` if the complement fits. Otherwise load it program-relative
    /// from the next pool, sharing a word with an identical literal already
    /// waiting there.
    ///
    /// A value that names a label or an imported symbol always takes a pool
    /// word even when it would fit an immediate, because the linker has to be
    /// able to relocate it and there is nowhere in a `MOV` to put a
    /// relocation.
    fn note_literal(&mut self, line: &Line) -> Option<u32> {
        let expr = self.expand_text(&Self::literal_operand(line)?);
        let always_pooled = Self::always_pooled(line);
        // A VFP floating-point literal is held as its bits, because a pool
        // word is an integer and the expression evaluator does not read a
        // fraction.
        let vfp_bits = line
            .opcode_str()
            .and_then(|o| crate::vfp::parse(&o.to_ascii_uppercase()))
            .filter(|v| matches!(v.stem, "FLDD" | "FLDS"))
            .and_then(|v| crate::vfp::literal_bits(v.stem, &expr));
        let site = self.literal_sites.len();
        let area = self.current_area_index();

        // Pass two keeps pass one's layout, so both place the same bytes.
        // This holds even where pass two can evaluate something pass one
        // could not, most often a forward `EQU`.
        if let Some(prev) = self.literal_sites_prev.get(site).copied() {
            self.literal_sites.push(prev);
            // But pass two does not keep pass one's *instruction*. Taskman
            // writes `LDR r3, =HeapSize` four thousand lines before
            // `HeapSize * :INDEX: @`, so pass one had to reserve a pool word
            // for a value it could not see. Pass two can see it, and a value
            // an instruction can hold is loaded with one. The word stays
            // reserved but unread, so every address after it is where pass
            // one put it. That is what ObjAsm's own object shows.
            let now = match self.eval_expr(&expr) {
                Ok(Value::Arith(n)) => Some(n),
                _ => None,
            };
            let loadable = !always_pooled
                && now.is_some_and(|v| {
                    crate::legalize::as_arm_immediate(v).is_some()
                        || crate::legalize::as_arm_immediate(!v).is_some()
                })
                && !identifiers(&expr)
                .iter()
                .any(|n| self.label_defs.contains_key(n) || self.imports.contains(n));
            return match prev? {
                LiteralRef::Placed(at) => (!loadable).then_some(at),
                LiteralRef::Pending(pool, offset) => {
                    // Reserve it again, so the pool still knows its own size.
                    self.reserve_literal(&expr, area, Some(offset), !loadable, vfp_bits.clone());
                    if loadable {
                        return None;
                    }
                    let p = self.pools_prev.get(pool).copied()?;
                    Some(p.base + offset)
                }
            };
        }

        let value = match self.eval_expr(&expr) {
            Ok(Value::Arith(n)) => Some(n),
            _ => None,
        };
        let relocatable = identifiers(&expr)
            .iter()
            .any(|n| self.label_defs.contains_key(n) || self.imports.contains(n));
        let fits = !always_pooled
            && value.is_some_and(|v| {
                crate::legalize::as_arm_immediate(v).is_some()
                    || crate::legalize::as_arm_immediate(!v).is_some()
            });
        if fits && !relocatable {
            self.literal_sites.push(None);
            return None;
        }
        // A pool already passed may hold this value, and an LDR reaches
        // backwards as easily as forwards. A new word is asked for only when
        // none is in range.
        let here = self.area.as_ref().map(|a| a.offset).unwrap_or(0);
        let trimmed = expr.trim();
        let in_range = |at: u32| {
            if vfp_bits.is_some() {
                in_vldr_range(here, at)
            } else {
                in_ldr_range(here, at)
            }
        };
        if let Some((_, at, _)) = self
            .placed_literals
            .iter()
            .rev()
            .find(|(a, at, e)| *a == area && e == trimmed && in_range(*at))
        {
            let at = *at;
            self.literal_sites.push(Some(LiteralRef::Placed(at)));
            return Some(at);
        }
        let offset = self.reserve_literal(&expr, area, None, true, vfp_bits);
        self.literal_sites
            .push(Some(LiteralRef::Pending(self.pools.len(), offset)));
        // The pool has not been placed yet in this pass, so there is no
        // address to give back.
        None
    }

    /// Find or make room for a literal in the pool being filled. Returns its
    /// offset within that pool.
    fn reserve_literal(
        &mut self,
        expr: &str,
        area: usize,
        at: Option<u32>,
        used: bool,
        bits: Option<Vec<u32>>,
    ) -> u32 {
        let expr = expr.trim();
        if let Some(l) = self.pending_literals.iter_mut().find(|l| l.expr == expr) {
            // One site may load it from the pool while another does not.
            l.used |= used;
            return l.offset;
        }
        // New words go after the last literal's last word, because a double
        // occupies two of them.
        let offset = at.unwrap_or_else(|| {
            self.pending_literals
                .last()
                .map_or(0, |l| l.offset + 4 * l.words())
        });
        self.pending_literals.push(Literal {
            expr: expr.to_string(),
            offset,
            area,
            used,
            bits,
        });
        offset
    }

    /// Close the pool being filled and hand back its words.
    ///
    /// Called at `LTORG`, and at the outermost `END`. The manual has a
    /// default `LTORG` there, "not part of a nested assembly", so the end of
    /// a file read by `GET` places no pool.
    fn close_pool(&mut self, line_num: usize) -> R<Vec<u8>> {
        let base = self.area.as_ref().map(|a| a.offset).unwrap_or(0);
        let area = self.current_area_index();
        let literals = std::mem::take(&mut self.pending_literals);
        let size = literals.iter().map(|l| 4 * l.words()).sum();
        self.pools.push(Pool { base, size, area });

        let mut bytes = Vec::with_capacity(size as usize);
        for l in &literals {
            if l.area != area {
                return self.err(
                    line_num,
                    format!(
                        "the literal `{}` is used in area {} but its pool lands in area {}",
                        l.expr, l.area, area
                    ),
                );
            }
            if let Some(words) = &l.bits {
                // A VFP literal's bits, which no integer expression holds.
                // A word nothing reads is left as it was reserved.
                for v in words.iter().map(|w| if l.used { *w } else { 0 }) {
                    bytes.extend_from_slice(&v.to_le_bytes());
                }
                self.placed_literals.push((area, base + l.offset, l.expr.clone()));
                continue;
            }
            let v = match self.eval_expr(&l.expr) {
                // A word nothing reads is left as it was reserved.
                _ if !l.used => 0,
                Ok(Value::Arith(n)) => n,
                // Not an error. An imported symbol has no value here, and the
                // relocation below supplies it.
                _ => 0,
            };
            bytes.extend_from_slice(&v.to_le_bytes());
            self.placed_literals.push((area, base + l.offset, l.expr.clone()));
            // A pool word holding an address moves when the linker places the
            // area, just as `DCD Label` does.
            if let Some(kind) = self.fixup_kind(&l.expr) {
                self.data_fixups.push(DataFixup {
                    area,
                    offset: base + l.offset,
                    width: 4,
                    expr: l.expr.clone(),
                    kind,
                });
            }
        }
        if let Some(a) = self.area.as_mut() {
            a.offset = a.offset.wrapping_add(size);
        }
        self.set_builtin_dot();
        Ok(bytes)
    }

    /// `moves_with_area`, for a data item written at `here`. Its `.` is the
    /// item's own address, which moves with the area just as its labels do.
    /// The Wimp's `DCD Do_ExitHandler-.-4` is a distance, and stays put.
    fn moves_with_area_at(&self, text: &str, area: usize, here: u32) -> bool {
        const BY: u32 = 4;
        let Ok(Value::Arith(a)) = self.eval_expr(&Self::substitute_dot(text, here)) else {
            return true;
        };
        let shifted = map_names(text, |n| match self.label_defs.get(n) {
            Some((x, _)) if *x == area => Some(format!("({n}+{BY})")),
            _ => None,
        });
        let Ok(Value::Arith(b)) = self.eval_expr(&Self::substitute_dot(&shifted, here.wrapping_add(BY))) else {
            return true;
        };
        b.wrapping_sub(a) != 0
    }

    /// Does this value move when the linker places the area?
    ///
    /// `DCD Initialise - Module_BaseAddr` names two labels and moves with
    /// neither. The distance between them is the same wherever the area
    /// lands, and ObjAsm emits no relocation for it. `DCD Initialise` moves
    /// with the area and needs one. DADebug's module header is seventeen of
    /// the first kind, one after another.
    ///
    /// Rather than pick the expression apart, we test it. It is evaluated as
    /// it stands, and again with every label in that area moved along. A
    /// value that moved with them moves with the area. Anything that will
    /// not evaluate either way is left to be relocated, which is the safe
    /// answer.
    fn moves_with_area(&self, expr: &str, area: usize) -> bool {
        const BY: u32 = 4;
        let Ok(Value::Arith(a)) = self.eval_expr(expr) else {
            return true;
        };
        let shifted = self.shift_labels(expr, area, BY);
        let Ok(Value::Arith(b)) = self.eval_expr(&shifted) else {
            return true;
        };
        b.wrapping_sub(a) != 0
    }

    /// The expression with every label of one area moved along by `by`. When
    /// the area is the current one, the location counter moves with them,
    /// because `.` and `{PC}` are in that area just as its labels are.
    /// Without that, the Wimp's `DCD Do_ExitHandler-.-4`, an offset that
    /// cannot change, was taken to move with the area and was relocated.
    fn shift_labels(&self, text: &str, area: usize, by: u32) -> String {
        let named = map_names(text, |n| match self.label_defs.get(n) {
            Some((a, _)) if *a == area => Some(format!("({n}+{by})")),
            _ => None,
        });
        if self.current_area_index() != area {
            return named;
        }
        shift_location(&named, by)
    }

    /// A symbol defined from a register-relative one is register-relative.
    ///
    /// TaskWindow lays its workspace out under `^ 0, r12` and then writes
    /// `key_name * ParameterBuffer+8`. That names a field of the block `r12`
    /// points at, just as the `#` that reserved it does. So `LDR r2,
    /// key_name` is a load from that block. It is not a program-relative
    /// load of an address, which is what it became without this.
    ///
    /// Two symbols on different registers in one expression have no meaning,
    /// so nothing is inherited from that.
    fn inherit_base(&mut self, name: &str, rhs: &str) {
        // `@` is the map's own counter, so a symbol taking its value is
        // register-relative just as the map is. Taskman writes
        // `GotVectors * @` inside `^ 0, r12` and then `ADRL r0, GotVectors`.
        let mut bases: Vec<u32> = Vec::new();
        if rhs.contains('@') {
            bases.extend(self.map_base);
        }
        bases.extend(
            identifiers(rhs)
                .iter()
                .filter_map(|n| self.field_bases.get(n).copied()),
        );
        if let Some(b) = bases.first() {
            if bases.iter().all(|x| x == b) {
                self.field_bases.insert(name.to_string(), *b);
            }
        }
    }

    /// A symbol defined from an address in this area is an address in it.
    ///
    /// StringLib writes `Instance0 * .` and then `ADRL lr, Instance0`. The
    /// value came from the location counter, so it names a place in the
    /// area and moves when the area does. That is what tells `ADR` to reach
    /// it from `pc` rather than load a number.
    ///
    /// A difference of two such symbols is a distance, not a place. So the
    /// same test that decides a relocation (`moves_with_area`) decides it.
    fn inherit_place(&mut self, name: &str, rhs: &str, value: u32) {
        let Some(area) = self.area.as_ref().map(|_| self.current_area_index()) else {
            return;
        };
        let here = rhs.contains('.') || rhs.contains("{PC}");
        let names_a_label = identifiers(rhs)
            .iter()
            .any(|n| self.label_defs.get(n).is_some_and(|(a, _)| *a == area));
        if !(here || names_a_label) {
            return;
        }
        if names_a_label && !self.moves_with_area(rhs, area) {
            return;
        }
        self.label_defs.insert(name.to_string(), (area, value));
    }

    /// The operand text with every assembly-time variable replaced by the
    /// value it holds here.
    ///
    /// Labels and `EQU` symbols are left as names. A label has to stay a
    /// name so that the address folding can recognise it, and neither kind
    /// can change once set. Only what `SETA` and its family write is fixed
    /// here, because only that is rewritten as the file goes on.
    fn freeze_variables(&self, text: &str) -> String {
        map_names(text, |n| match self.syms.get(n) {
            Some(Value::Arith(v)) => Some(v.to_string()),
            Some(Value::Logical(b)) => {
                Some(if *b { "{TRUE}" } else { "{FALSE}" }.to_string())
            }
            _ => None,
        })
    }

    /// The expression with every imported name standing at zero.
    ///
    /// What the linker will supply is not known here, but what the source
    /// wrote around it is. In `DCD |_Lib$Reloc$Off$DP| + &E28AA000`, ShellCLI
    /// builds an instruction by hand. The word it wants in the object is that
    /// constant, with the relocation to add the rest.
    fn without_imports(&self, text: &str) -> String {
        map_names(text, |n| self.imports.contains(&n.to_string()).then(|| "0".into()))
    }

    /// How a value naming a symbol has to be relocated, if at all.
    fn fixup_kind(&self, expr: &str) -> Option<FixupKind> {
        let names = identifiers(expr);
        let areas: Vec<usize> = names
            .iter()
            .filter_map(|n| self.label_defs.get(n).map(|(a, _)| *a))
            .collect();
        if let Some(a) = areas.first() {
            if !areas.iter().all(|x| x == a) {
                return None;
            }
            return self.moves_with_area(expr, *a).then_some(FixupKind::AreaBase(*a));
        }
        names
            .iter()
            .find(|n| self.imports.contains(n))
            .map(|n| FixupKind::External(n.clone()))
    }

    /// The scope a local label belongs to.
    ///
    /// A `ROUT` starts one, and so does a macro expansion. `DivRem` writes
    /// an `01` and an `02` of its own. The `B %FT02` that follows it in
    /// StringLib means the caller's `02`, not the macro's, which is four
    /// instructions into the expansion. Each expansion is kept apart from
    /// every other, so a label written in one cannot be seen outside it.
    fn local_scope(&self) -> Option<String> {
        let rout = self.rout.clone().unwrap_or_default();
        match self.stack.iter().rev().find_map(|s| match s {
            Source::Macro { id, .. } => Some(*id),
            _ => None,
        }) {
            Some(id) => Some(format!("{rout} in expansion {id}")),
            None => self.rout.clone(),
        }
    }

    /// Record a label's address. Local labels (a bare number) are kept
    /// separately, scoped to the enclosing `ROUT`.
    fn define_label(&mut self, line: &Line) {
        let Some(raw) = line.label_str() else { return };
        // A directive reaches here with its line not yet substituted. Only
        // the statement path lexes the line again after expanding it. So a
        // label written as a macro parameter still says `$label` here.
        let name = strip_bars(&self.expand_text(raw));
        if name.is_empty() {
            return;
        }
        let addr = self.area.as_ref().map(|a| a.offset).unwrap_or(0);
        match layout::parse_local_def(&name) {
            Some((n, routine)) => {
                let scope = routine.or_else(|| self.local_scope()).unwrap_or_default();
                let area = self.current_area_index();
                self.locals.push(LocalDef { scope, number: n, addr, area });
                self.fresh_locals.push(self.locals.len() - 1);
            }
            None => {
                self.syms.define_absolute(&name, addr);
                // The second pass overwrites the first pass's guess. That is
                // what makes a forward reference come out right.
                let area = self.current_area_index();
                self.label_defs.insert(name.clone(), (area, addr));
                self.label_kinds.insert(name.clone(), self.in_data);
                self.fresh_labels.push(name);
            }
        }
    }

    /// `ASSERT logical-expression`.
    ///
    /// A failure is an error. Assertions state rules the sources rely on, so
    /// a mere warning would let wrong code through. The exception is a
    /// listing, where `assert_warnings` reports the failure and carries on.
    ///
    /// The manual makes this a second-pass check, so pass one skips it,
    /// because the symbols it names may not exist yet. Pass two tests it
    /// where it stands. That matters because `@` and `.` hold the value they
    /// have *here*, not the value they end up with.
    fn do_assert(&mut self, line: &Line) -> R<()> {
        if self.pass_no == 1 {
            return Ok(());
        }
        let here = self.area.as_ref().map(|a| a.offset).unwrap_or(0);
        let src = self.expand_text(line.operands_str().unwrap_or(""));
        let area = self.current_area_index();
        let rout = self.local_scope();
        let src = self.substitute_locals(&src, here, area, rout.as_deref());
        match self.eval_expr(&src) {
            Ok(Value::Logical(true)) => Ok(()),
            Ok(Value::Logical(false)) => {
                let why = self.explain_comparison(&src);
                let msg = format!("assertion failed: {src}{why}");
                if self.assert_warnings {
                    let o = self.origin(line.num);
                    eprintln!("{}:{}: {msg}", o.file, o.line);
                    return Ok(());
                }
                self.err(line.num, msg)
            }
            Ok(v) => self.err(line.num, format!("ASSERT needs a logical value, got {v:?}")),
            Err(e) => self.err(line.num, e.to_string()),
        }
    }

    /// `! is-error, string «,is-warning»`, and `INFO`, which is the same.
    ///
    /// The manual says the arithmetic expression `is-error` is evaluated. If
    /// it is not zero, the string is printed as an error and the assembly
    /// halts after pass one. If it is zero, nothing happens on pass one, and
    /// the string is printed on pass two. It is printed as a warning if the
    /// optional `is-warning` expression is non-zero, and as a plain message
    /// otherwise.
    ///
    /// It generates no code. The Kernel has four of these in its SWI
    /// despatcher, reporting where the entry points landed. Giving them a
    /// word each put the despatcher sixteen bytes over the size it asserts.
    fn do_info(&mut self, line: &Line) -> R<()> {
        let src = self.expand_text(line.operands_str().unwrap_or(""));
        let parts = layout::split_top_level(&src);
        let value = |i: usize| -> u32 {
            parts
                .get(i)
                .map(|t| match self.eval_expr(t.trim()) {
                    Ok(Value::Arith(n)) => n,
                    Ok(Value::Logical(b)) => b as u32,
                    _ => 0,
                })
                .unwrap_or(0)
        };
        let message = match parts.get(1).map(|t| self.eval_expr(t.trim())) {
            Some(Ok(Value::Str(m))) => m,
            // A message that will not evaluate is still worth showing.
            _ => parts.get(1).map(|t| t.trim().to_string()).unwrap_or_default(),
        };
        let o = self.origin(line.num);
        if value(0) != 0 {
            return self.err(line.num, message);
        }
        if self.pass_no == 2 {
            let kind = if value(2) != 0 { "warning" } else { "info" };
            eprintln!("{}:{}: {kind}: {message}", o.file, o.line);
        }
        Ok(())
    }

    /// Both sides of a failed comparison, where the assertion is one.
    ///
    /// An assertion that fails says what it was checking, but not by how
    /// much it failed. The sources use assertions to check their own layout.
    /// The Kernel asserts `{PC}-SVCDespatcher = SWIDespatch_Size`, and when
    /// that fails, the useful part is the difference between the two numbers.
    fn explain_comparison(&self, src: &str) -> String {
        let mut depth = 0i32;
        let cs: Vec<char> = src.chars().collect();
        for (i, c) in cs.iter().enumerate() {
            match c {
                '(' | '[' | '{' => depth += 1,
                ')' | ']' | '}' => depth -= 1,
                // Only a bare `=`, not the tail of `<=`, `>=` or `/=`.
                '=' if depth == 0 && !matches!(cs.get(i.wrapping_sub(1)), Some('<' | '>' | '/')) => {
                    let (a, b) = (&src[..i], &src[i + 1..]);
                    if let (Ok(Value::Arith(x)), Ok(Value::Arith(y))) =
                        (self.eval_expr(a), self.eval_expr(b))
                    {
                        let d = y as i64 - x as i64;
                        return format!(" ({x} against {y}, {d:+} out)");
                    }
                    return String::new();
                }
                _ => {}
            }
        }
        String::new()
    }

    fn do_get(&mut self, line: &Line, chain: Chain) -> R<()> {
        if self.stack.len() >= MAX_SOURCE_DEPTH {
            return self.err(line.num, "GET nested too deeply (include cycle?)");
        }
        // `LNK` closes the literal pool, as an `LTORG` would.
        //
        // The manual's default `LTORG` at `END` is "not part of a nested
        // assembly", so the end of a file read by `GET` places no pool. But a
        // chain of `LNK`s is one assembly split across files. BASICVFP's is
        // fourteen files with five `LTORG`s in eight hundred kilobytes.
        // ObjAsm 4.08 refuses that. The build's Makefile hands the component
        // to `ObjAsmVFP`, which does not refuse it. So here too the pool goes
        // where the chain crosses from one file to the next.
        if chain == Chain::Tail && !self.pending_literals.is_empty() {
            self.emit_pool(line.num)?;
        }
        let raw = self.expand_text(line.operands_str().unwrap_or(""));
        // In `Hdr:Machine.<Machine>`, the angle brackets are RISC OS
        // *filename* variable substitution. The filing system does it before
        // ObjAsm sees the name, and it is quite separate from ObjAsm's own `$`
        // substitution.
        let raw = self.substitute_angle_vars(&raw);
        let name = raw.trim();
        match self.resolver.resolve(name) {
            Some((shown, lines)) => {
                // `LNK` ends the current file rather than nesting inside it.
                // Nothing after the directive is assembled. When the named
                // file runs out, control returns to whatever brought this
                // one in. The manual's "END or LNK found before the necessary
                // ENDIF" error says the same thing from the other side.
                if chain == Chain::Tail {
                    self.stack.pop();
                }
                self.stack.push(Source::File {
                    name: shown,
                    lines: lex::lex(&lines),
                    pos: 0,
                });
                Ok(())
            }
            None => self.err(line.num, format!("cannot find '{name}'")),
        }
    }

    fn do_while(&mut self, line: &Line) -> R<()> {
        let cond = self.expand_text(line.operands_str().unwrap_or(""));
        let (body, wend) = self.take_until_capturing(line.num, "WHILE", "WEND")?;
        // Tested at the top, so the body may run zero times.
        if self.eval_logical(&cond, line.num)? {
            let file = self.origin(line.num).file;
            self.stack.push(Source::Loop {
                file,
                lines: body,
                pos: 0,
                while_line: line.clone(),
                wend_line: wend,
            });
        }
        Ok(())
    }

    fn do_mexit(&mut self, line: &Line) -> R<()> {
        // Unwind loops inside the macro, then the macro frame itself.
        while let Some(top) = self.stack.last() {
            match top {
                Source::Loop { .. } => {
                    self.stack.pop();
                }
                Source::Macro { conds, .. } => {
                    let depth = *conds;
                    self.stack.pop();
                    self.syms.pop_frame();
                    self.conds.truncate(depth);
                    return Ok(());
                }
                Source::File { .. } => break,
            }
        }
        self.err(line.num, "MEXIT outside a macro")
    }

    /// Collect lines up to the matching terminator, honouring nesting.
    fn take_until(&mut self, at: usize, open: &str, close: &str, list: bool) -> R<Vec<Line>> {
        let mut body = Vec::new();
        let mut depth = 1usize;
        loop {
            let Some(l) = self.next_line()? else {
                return self.err(at, format!("{open} without {close}"));
            };
            let up = l.opcode_str().unwrap_or("").to_ascii_uppercase();
            // A MACRO definition is listed as it is read. A WHILE body is
            // not, because it is listed afresh on each iteration.
            if list {
                self.list_line(&l);
            }
            if up == open {
                depth += 1;
            } else if up == close {
                depth -= 1;
                if depth == 0 {
                    return Ok(body);
                }
            }
            body.push(l);
        }
    }

    // ---- macro definition and invocation ---------------------------------

    fn define_macro(&mut self, line: &Line) -> R<()> {
        // The prototype is the next statement line.
        let proto = loop {
            let Some(l) = self.next_line()? else {
                return self.err(line.num, "MACRO without a prototype");
            };
            // ObjAsm lists the prototype line as part of the definition.
            self.list_line(&l);
            if l.kind == Kind::Statement {
                break l;
            }
        };
        let Some(op) = proto.opcode_str() else {
            return self.err(proto.num, "macro prototype has no name");
        };
        let (stem, name_param) = split_name(op);
        let label_param = proto
            .label_str()
            .map(|l| l.trim_start_matches('$').trim_end_matches('.').to_string());
        let params = parse_params(proto.operands_str().unwrap_or(""));
        let body = self.take_until(line.num, "MACRO", "MEND", true)?;

        self.macros.push(Macro {
            stem,
            name_param,
            label_param,
            params,
            body,
        });
        Ok(())
    }

    /// Find a macro for this opcode. An exact name is tried first. Then a
    /// macro whose name ends in a parameter matches if its stem begins the
    /// opcode (`PTOp$cc` invoked as `PTOpEQ`).
    fn find_macro(&self, op: &str) -> Option<(usize, Option<String>)> {
        if let Some(i) = self
            .macros
            .iter()
            .position(|m| m.name_param.is_none() && m.stem == op)
        {
            return Some((i, None));
        }
        // Longest stem wins, so `PTOpX$cc` beats `PTOp$cc` for `PTOpXEQ`.
        //
        // The argument may be empty, and often is. `BPIALL$cond` is invoked as
        // a bare `BPIALL` far more often than with a condition. Requiring the
        // invocation to be longer than the stem broke two cases. A plain
        // `BPIALL` matched nothing and reached the encoder as an instruction.
        // And `BPIALLIS` matched `BPIALL` with `$cond` as `IS`, so the body's
        // `MCR$cond` came out as `MCRIS`.
        let mut best: Option<(usize, Option<String>)> = None;
        let mut best_len = 0usize;
        for (i, m) in self.macros.iter().enumerate() {
            if m.name_param.is_some() && op.len() >= m.stem.len() && op.starts_with(&m.stem)
                && m.stem.len() >= best_len {
                    best_len = m.stem.len();
                    best = Some((i, Some(op[m.stem.len()..].to_string())));
                }
        }
        best
    }

    fn emit_or_invoke(&mut self, line: Line) -> R<()> {
        let text = self.expand_text(&line.raw);
        // Lex again after substitution, because a parameter can expand to an
        // opcode.
        let relexed = lex::lex_line(line.num, &text);
        let op = relexed.opcode_str().unwrap_or("").to_string();

        if !op.is_empty() {
            if let Some((idx, name_arg)) = self.find_macro(&op) {
                // ObjAsm lists the invocation, then the expanded body.
                self.list_line(&relexed);
                return self.invoke(idx, name_arg, &relexed);
            }
        }
        if relexed.kind == Kind::Statement {
            // Read here, while the variables an operand names still hold the
            // values this line was written with.
            let operands = self.freeze_variables(relexed.operands_str().unwrap_or(""));
            // Pad before the address is read, because padding moves it.
            self.pad_to(Self::alignment_of(&relexed), line.num);
            let mut addr = self.area.as_ref().map(|a| a.offset).unwrap_or(0);
            // Note the literal before advancing, so the pool sees its
            // literals in source order.
            let literal = self.note_literal(&relexed);
            let up = relexed.opcode_str().unwrap_or("").to_ascii_uppercase();
            // `INCBIN`, and `BIN` as ResourceFS's `ResourceFile` writes it.
            // These give the file's bytes exactly as they are, with no
            // alignment before or after.
            let included = if up == "INCBIN" || up == "BIN" {
                let name = self.substitute_angle_vars(relexed.operands_str().unwrap_or(""));
                let name = name.trim().trim_matches('"');
                let Some((path, _)) = self.resolver.host_file(name) else {
                    return self.err(line.num, format!("cannot find '{name}'"));
                };
                match std::fs::read(&path) {
                    Ok(b) => Some(b),
                    Err(e) => return self.err(line.num, format!("{}: {e}", path.display())),
                }
            } else {
                None
            };
            self.included = included.as_ref().map(|b| b.len() as u32);
            self.advance(&relexed);
            self.included = None;
            let mut bytes = match included {
                Some(b) => b,
                None => self.data_bytes(&relexed),
            };
            // `ALIGN` pads to the boundary and `SPACE` reserves a run of
            // bytes. Both put those bytes in the object. A gap that the
            // location counter knows about but the data does not would move
            // everything after it. Touch reserves twelve bytes in the middle
            // of a device block, and its table came out three words early.
            if up == "ALIGN" || up == "SPACE" || up == "%" {
                let after = self.area.as_ref().map(|a| a.offset).unwrap_or(addr);
                bytes = vec![0u8; after.saturating_sub(addr) as usize];
            }
            // `LTORG` is where the pool goes. `advance` has just aligned to a
            // word, so this is the pool's base.
            if up == "LTORG" {
                addr = self.area.as_ref().map(|a| a.offset).unwrap_or(addr);
                bytes = self.close_pool(line.num)?;
            }
            let origin = self.origin(line.num);
            let area_index = self.current_area_index();
            let rout = self.local_scope();
            self.out.push(ExpandedLine {
                operands,
                text,
                origin,
                addr,
                bytes,
                listing_only: false,
                area_index,
                rout,
                literal,
            });
        }
        Ok(())
    }

    fn invoke(&mut self, idx: usize, name_arg: Option<String>, call: &Line) -> R<()> {
        let depth = self
            .stack
            .iter()
            .filter(|s| matches!(s, Source::Macro { .. }))
            .count();
        if depth >= MAX_MACRO_DEPTH {
            return self.err(call.num, "macro nesting deeper than 255");
        }

        let m = self.macros[idx].clone();
        let mut args: HashMap<String, String> = HashMap::new();

        if let (Some(p), Some(v)) = (&m.name_param, name_arg) {
            args.insert(p.clone(), v);
        }
        if let Some(lp) = &m.label_param {
            args.insert(lp.clone(), call.label_str().unwrap_or("").to_string());
        }

        let given = split_args(call.operands_str().unwrap_or(""));
        for (i, p) in m.params.iter().enumerate() {
            let raw = given.get(i).map(|s| s.trim()).unwrap_or("");
            // A bar selects the default. An omitted argument does not.
            let text = if raw == "|" {
                p.default.clone().unwrap_or_default()
            } else {
                raw.to_string()
            };
            args.insert(p.name.clone(), unquote_arg(&text));
        }

        let file = self.origin(call.num).file;
        self.syms.push_frame();
        self.expansions += 1;
        self.stack.push(Source::Macro {
            name: m.stem.clone(),
            file,
            lines: m.body,
            pos: 0,
            args,
            id: self.expansions,
            conds: self.conds.len(),
        });
        Ok(())
    }

    // ---- substitution ----------------------------------------------------

    /// Apply the manual's two stages of substitution: macro parameters, then
    /// variables.
    fn expand_text(&self, text: &str) -> String {
        let args: Option<&HashMap<String, String>> = self.stack.iter().rev().find_map(|s| match s {
            Source::Macro { args, .. } => Some(args),
            _ => None,
        });

        let after_params = match args {
            Some(a) => substitute(text, &|n: &str| a.get(n).cloned()),
            None => text.to_string(),
        };
        substitute(&after_params, &|n: &str| {
            self.syms.get(n).map(value_text)
        })
    }
}

/// A resolver that finds no files, for expanding source held in memory
/// where `GET` is not needed.
pub struct NoFiles;
impl FileResolver for NoFiles {
    fn resolve(&self, _: &str) -> Option<(String, Vec<String>)> {
        None
    }
}

/// A resolver over an in-memory map, for tests.
pub struct MapResolver(pub HashMap<String, Vec<String>>);
impl FileResolver for MapResolver {
    fn resolve(&self, name: &str) -> Option<(String, Vec<String>)> {
        self.0.get(name).map(|v| (name.to_string(), v.clone()))
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn run(src: &str) -> Vec<String> {
        run_with(src, MapResolver(HashMap::new()))
    }

    fn run_with(src: &str, r: impl FileResolver) -> Vec<String> {
        let lines: Vec<String> = src.lines().map(|s| s.to_string()).collect();
        let mut e = Expander::new(&r);
        match e.run("test", lines) {
            Ok(out) => out
                .into_iter()
                .filter(|l| !l.listing_only)
                .map(|l| l.text.trim().to_string())
                .filter(|s| !s.is_empty())
                .collect(),
            Err(err) => panic!("{err}"),
        }
    }

    fn fails(src: &str) -> String {
        let lines: Vec<String> = src.lines().map(|s| s.to_string()).collect();
        let r = MapResolver(HashMap::new());
        let mut e = Expander::new(&r);
        match e.run("test", lines) {
            Ok(o) => panic!("expected failure, got {o:?}"),
            Err(err) => err.to_string(),
        }
    }

    /// Bytes emitted by each non-blank line.
    fn bytes_of(src: &str) -> Vec<Vec<u8>> {
        let lines: Vec<String> = src.lines().map(|s| s.to_string()).collect();
        let r = MapResolver(HashMap::new());
        let mut e = Expander::new(&r);
        e.run("test", lines)
            .expect("expansion")
            .into_iter()
            .filter(|l| !l.listing_only && !l.bytes.is_empty())
            .map(|l| l.bytes)
            .collect()
    }

    #[test]
    fn an_offset_from_here_is_not_relocated() {
        // The Wimp's defaulthandlers write `DCD Do_ExitHandler-.-4`, which is
        // added back to the word's own address at run time. The distance
        // cannot change, so nothing may relocate it. But a word holding the
        // label itself moves with the area.
        let src = "        AREA    |Test$$Code|, CODE, READONLY\n\
                   Start   DCD     Handler-.-4\n\
                   \x20       DCD     Handler\n\
                   Handler MOV     pc, lr\n\
                   \x20       END\n";
        let lines: Vec<String> = src.lines().map(|s| s.to_string()).collect();
        let r = MapResolver(HashMap::new());
        let mut e = Expander::new(&r);
        e.run("test", lines).expect("expansion");
        let fixed: Vec<&str> = e.data_fixups().iter().map(|f| f.expr.trim()).collect();
        assert_eq!(fixed, vec!["Handler"], "only the address is relocated");
        assert_eq!(shift_location("a-.-4", 4), "a-(.+4)-4");
        assert_eq!(shift_location("s.Wimp + 1.5 + {pc}", 4), "s.Wimp + 1.5 + ({PC}+4)");
    }

    #[test]
    fn a_line_size_names_no_label() {
        // `?Label` is a number, the size of Label's line, so it needs no
        // relocation.
        assert!(identifiers("?str_commands2").is_empty());
        assert_eq!(identifiers("?a + b"), vec!["b"]);
        assert_eq!(identifiers("Label - ?Label"), vec!["Label"]);
    }

    // ---- conditionals ---------------------------------------------------

    #[test]
    fn conditional_takes_the_true_branch() {
        let out = run("        [ {TRUE}\n        MOV r0, #1\n        ]\n");
        assert_eq!(out, vec!["MOV r0, #1"]);
    }

    #[test]
    fn conditional_skips_the_false_branch() {
        assert!(run("        [ {FALSE}\n        MOV r0, #1\n        ]\n").is_empty());
    }

    #[test]
    fn else_branch_runs_when_condition_is_false() {
        let out = run("        [ {FALSE}\n        MOV r0, #1\n        |\n        MOV r0, #2\n        ]\n");
        assert_eq!(out, vec!["MOV r0, #2"]);
    }

    #[test]
    fn else_branch_skipped_when_condition_is_true() {
        let out = run("        [ {TRUE}\n        MOV r0, #1\n        |\n        MOV r0, #2\n        ]\n");
        assert_eq!(out, vec!["MOV r0, #1"]);
    }

    #[test]
    fn conditionals_nest() {
        let src = "        [ {TRUE}\n        [ {FALSE}\n        MOV r0, #1\n        |\n        MOV r0, #2\n        ]\n        ]\n";
        assert_eq!(run(src), vec!["MOV r0, #2"]);
    }

    #[test]
    fn a_skipped_branch_is_not_parsed() {
        // The corpus contains malformed lines that assemble only because they
        // are skipped: an ASSERT with two operands, and `:OR` missing a colon.
        let src = "        [ {FALSE}\n        ASSERT  F,\"not supported\"\n        Foo :OR (1)\n        ]\n";
        assert!(run(src).is_empty());
    }

    #[test]
    fn keyword_spellings_work_too() {
        let out = run("        IF {TRUE}\n        MOV r0, #1\n        ELSE\n        MOV r0, #2\n        ENDIF\n");
        assert_eq!(out, vec!["MOV r0, #1"]);
    }

    #[test]
    fn unclosed_conditional_is_an_error() {
        assert!(fails("        [ {TRUE}\n        MOV r0, #1\n").contains("never closed"));
    }

    // ---- variables ------------------------------------------------------

    #[test]
    fn global_variables_declare_and_assign() {
        let out = run("        GBLA n\nn       SETA 5\n        DCD $n\n");
        // GBLA and SETA act only at assembly time. The expander uses them up
        // and they never reach the assembler. `$n` becomes eight hex digits.
        // They evaluate to 5 in the byte column, while the text keeps what
        // was written, exactly as ObjAsm lists it.
        assert_eq!(out, vec!["DCD 00000005"]);
        assert_eq!(
            bytes_of("        GBLA n
n       SETA 5
        DCD $n
"),
            vec![vec![5, 0, 0, 0]]
        );
    }

    #[test]
    fn logical_variable_substitutes_as_t_or_f() {
        let out = run("        GBLL f\nf       SETL {TRUE}\n        DCB \"$f\"\n");
        assert_eq!(out.last().unwrap(), "DCB \"T\"");
    }

    #[test]
    fn string_variable_substitutes_verbatim() {
        let out = run("        GBLS s\ns       SETS \"hello\"\n        DCB \"$s\"\n");
        assert_eq!(out.last().unwrap(), "DCB \"hello\"");
    }

    // ---- WHILE ----------------------------------------------------------

    #[test]
    fn while_unrolls_the_manuals_own_example() {
        // GBLA counter / SETA 100 ... DCD &$counter. The & reads the eight
        // hex digits back as a number.
        let src = "        GBLA counter\ncounter SETA 3\n        WHILE counter > 0\n        DCD &$counter\ncounter SETA counter - 1\n        WEND\n";
        let out = run(src);
        // The manual: "produces the same result as ... DCD 3 / DCD 2 / DCD 1".
        // The values land in the byte column. The text keeps `&$counter` as
        // substituted, which is what ObjAsm's listing shows.
        assert_eq!(out.iter().filter(|l| l.starts_with("DCD")).count(), 3);
        assert_eq!(
            bytes_of(src),
            vec![vec![3, 0, 0, 0], vec![2, 0, 0, 0], vec![1, 0, 0, 0]]
        );
    }

    #[test]
    fn while_may_run_zero_times() {
        let src = "        GBLA n\nn       SETA 0\n        WHILE n > 0\n        DCD 1\n        WEND\n";
        assert!(!run(src).iter().any(|l| l.starts_with("DCD")));
    }

    #[test]
    fn while_substitutes_its_test_afresh() {
        // In ROMFonts' `MyDesk`, the test reads `$temp`, which the body
        // shortens. When it was substituted only once, the test never changed
        // and the loop never ended.
        let src = concat!(
            "        GBLS src\n",
            "        GBLS temp\n",
            "temp    SETS \"a/b\"\n",
            "        WHILE (:LEN:\"$temp\" > 0)\n",
            "      [ \"$temp\":LEFT:1 = \"/\"\n",
            "src     SETS \"$src\" :CC: \"_\"\n",
            "      |\n",
            "src     SETS \"$src\" :CC: (\"$temp\":LEFT:1)\n",
            "      ]\n",
            "temp    SETS \"$temp\":RIGHT:(:LEN:\"$temp\" - 1)\n",
            "        WEND\n",
            "        DCB \"$src\"\n",
        );
        assert_eq!(bytes_of(src), vec![b"a_b".to_vec()]);
    }

    #[test]
    fn while_without_wend_is_an_error() {
        let src = "        GBLA n\nn       SETA 1\n        WHILE n > 0\n        DCD 1\n";
        assert!(fails(src).contains("without"));
    }

    // ---- included files -------------------------------------------------

    /// A directory of files for one test, removed when it is done.
    struct Scratch(PathBuf);
    impl Scratch {
        fn new(tag: &str, files: &[(&str, &[u8])]) -> Scratch {
            let dir = std::env::temp_dir().join(format!("rosasm-{tag}-{}", std::process::id()));
            for (name, bytes) in files {
                let p = dir.join(name);
                std::fs::create_dir_all(p.parent().unwrap()).unwrap();
                std::fs::write(p, bytes).unwrap();
            }
            Scratch(dir)
        }
        /// Each line's address and bytes.
        fn placed(&self, src: &str) -> Vec<(u32, Vec<u8>)> {
            let lines: Vec<String> = src.lines().map(|s| s.to_string()).collect();
            let r = DiscResolver { search: vec![self.0.clone()] };
            let mut e = Expander::new(&r);
            e.run("test", lines)
                .expect("expansion")
                .into_iter()
                .filter(|l| !l.listing_only && !l.bytes.is_empty())
                .map(|l| (l.addr, l.bytes))
                .collect()
        }
    }
    impl Drop for Scratch {
        fn drop(&mut self) {
            let _ = std::fs::remove_dir_all(&self.0);
        }
    }

    #[test]
    fn incbin_places_the_files_bytes_unaligned() {
        let d = Scratch::new("incbin", &[("Data/Three", b"xyz")]);
        // `BIN` as ResourceFS's macro spells it. There is no padding before
        // it, the counter moves on by the file's length after it, and a
        // `DCD` after that is aligned as usual.
        let src = "        AREA |D|, DATA\n        DCB 1\n        BIN Data.Three\n        DCB 2\nlab     DCD ?lab\n";
        assert_eq!(
            d.placed(src),
            vec![
                (0, vec![1]),
                (1, b"xyz".to_vec()),
                (4, vec![2]),
                (5, vec![0, 0, 0]),
                (8, vec![4, 0, 0, 0]),
            ]
        );
    }

    #[test]
    fn incbin_of_a_missing_file_is_an_error() {
        let d = Scratch::new("incbin-missing", &[]);
        let lines = vec!["        INCBIN Nothing.Here".to_string()];
        let r = DiscResolver { search: vec![d.0.clone()] };
        let mut e = Expander::new(&r);
        let err = e.run("test", lines).expect_err("no such file").to_string();
        assert!(err.contains("cannot find 'Nothing.Here'"), "{err}");
    }

    #[test]
    fn file_queries_read_the_type_from_the_suffix() {
        let d = Scratch::new("fquery", &[("Fonts/IntMetric0,ff6", b"12345"), ("Fonts/Text", b"ab")]);
        let src = concat!(
            "        DCD :FSIZE:\"Fonts.IntMetric0\"\n",
            "        DCD :FLOAD:\"Fonts.IntMetric0\" :AND: &FFFFFF00\n",
            "        DCD :FLOAD:\"Fonts.Text\" :AND: &FFFFFF00\n",
            "        DCD :FSIZE:\"Fonts.Text\"\n",
        );
        let words: Vec<u32> = d
            .placed(src)
            .iter()
            .map(|(_, b)| u32::from_le_bytes(b[..4].try_into().unwrap()))
            .collect();
        assert_eq!(words, vec![5, 0xfffff600, 0xffffff00, 2]);
    }

    // ---- macros ---------------------------------------------------------

    #[test]
    fn the_manuals_test_and_branch_example() {
        // Tests a macro *name* with a parameter, and the $label parameter.
        let src = concat!(
            "        MACRO\n",
            "$label  TestAndBranch$cc $dest,$reg\n",
            "$label  CMP $reg,#0\n",
            "        B$cc $dest\n",
            "        MEND\n",
            "Test    TestAndBranchNE NonZero,R0\n"
        );
        // Substitution works on the text. `$label` (6 characters) becomes
        // `Test` (4), so the next column shifts. The layout is not kept, but
        // the values are.
        assert_eq!(run(src), vec!["Test  CMP R0,#0", "BNE NonZero"]);
    }

    #[test]
    fn omitted_arguments_are_empty() {
        let src = concat!(
            "        MACRO\n",
            "        Two $a,$b\n",
            "        DCB \"[$a][$b]\"\n",
            "        MEND\n",
            "        Two 1\n"
        );
        assert_eq!(run(src), vec!["DCB \"[1][]\""]);
    }

    #[test]
    fn a_bar_argument_selects_the_default_but_omission_does_not() {
        // The manual is explicit that these differ.
        let src = concat!(
            "        MACRO\n",
            "        M $num=10\n",
            "        DCB \"[$num]\"\n",
            "        MEND\n",
            "        M |\n",
            "        M\n"
        );
        assert_eq!(run(src), vec!["DCB \"[10]\"", "DCB \"[]\""]);
    }

    #[test]
    fn dot_terminates_a_parameter_and_is_removed() {
        let src = concat!(
            "        MACRO\n",
            "$T33    Named\n",
            "$T33.L25 MOV r0, #1\n",
            "        MEND\n",
            "Ab      Named\n"
        );
        assert_eq!(run(src), vec!["AbL25 MOV r0, #1"]);
    }

    #[test]
    fn macros_nest() {
        let src = concat!(
            "        MACRO\n",
            "        Inner $x\n",
            "        DCB \"inner $x\"\n",
            "        MEND\n",
            "        MACRO\n",
            "        Outer $y\n",
            "        Inner $y\n",
            "        MEND\n",
            "        Outer 7\n"
        );
        assert_eq!(run(src), vec!["DCB \"inner 7\""]);
    }

    #[test]
    fn mexit_leaves_the_macro_early() {
        let src = concat!(
            "        MACRO\n",
            "        M\n",
            "        DCB \"first\"\n",
            "        MEXIT\n",
            "        DCB \"unreachable\"\n",
            "        MEND\n",
            "        M\n"
        );
        assert_eq!(run(src), vec!["DCB \"first\""]);
    }

    #[test]
    fn local_variables_are_scoped_to_the_expansion() {
        let src = concat!(
            "        GBLA v\n",
            "v       SETA 1\n",
            "        MACRO\n",
            "        M\n",
            "        LCLA v\n",
            "v       SETA 9\n",
            "        DCD $v\n",
            "        MEND\n",
            "        M\n",
            "        DCD $v\n"
        );
        let out = run(src);
        assert_eq!(out.iter().filter(|l| l.starts_with("DCD")).count(), 2);
        assert_eq!(bytes_of(src), vec![vec![9, 0, 0, 0], vec![1, 0, 0, 0]]);
    }

    #[test]
    fn conditional_inside_a_macro() {
        let src = concat!(
            "        MACRO\n",
            "        M $on\n",
            "        [ $on = 1\n",
            "        DCB \"yes\"\n",
            "        |\n",
            "        DCB \"no\"\n",
            "        ]\n",
            "        MEND\n",
            "        M 1\n",
            "        M 0\n"
        );
        assert_eq!(run(src), vec!["DCB \"yes\"", "DCB \"no\""]);
    }

    // ---- substitution rules ---------------------------------------------

    #[test]
    fn vertical_bars_toggle_substitution_off_and_on() {
        // Between the first and second bar, `$x` must survive untouched.
        let src = concat!(
            "        MACRO\n",
            "        M $x\n",
            "        DCB \"$x\",|lit$x|,\"$x\"\n",
            "        MEND\n",
            "        M A\n"
        );
        assert_eq!(run(src), vec!["DCB \"A\",|lit$x|,\"A\""]);
    }

    #[test]
    fn a_parameter_may_be_named_in_bars() {
        // DragASprite's TestAFlag builds `DS_$|what|_Mask`. The bars are
        // replaced with the name, and the line's other bars still toggle.
        let src = concat!(
            "        MACRO\n",
            "        M $what\n",
            "        DCD DS_$|what|_Mask,|lit$what|\n",
            "        MEND\n",
            "        M Drop\n"
        );
        assert_eq!(run(src), vec!["DCD DS_Drop_Mask,|lit$what|"]);
    }

    #[test]
    fn double_dollar_is_a_literal_dollar() {
        let src = concat!(
            "        MACRO\n",
            "        M $x\n",
            "        DCB \"$$x is $x\"\n",
            "        MEND\n",
            "        M 5\n"
        );
        assert_eq!(run(src), vec!["DCB \"$x is 5\""]);
    }

    #[test]
    fn unknown_dollar_names_are_left_alone() {
        assert_eq!(run("        DCB \"$notavar\"\n"), vec!["DCB \"$notavar\""]);
    }

    // ---- GET ------------------------------------------------------------

    #[test]
    fn get_splices_a_file() {
        let mut m = HashMap::new();
        m.insert("hdr.Thing".to_string(), vec!["        DCB \"included\"".to_string()]);
        let out = run_with("        GET hdr.Thing\n        DCB \"after\"\n", MapResolver(m));
        assert_eq!(out, vec!["DCB \"included\"", "DCB \"after\""]);
    }

    #[test]
    fn get_nests() {
        let mut m = HashMap::new();
        m.insert("a".to_string(), vec!["        GET b".to_string()]);
        m.insert("b".to_string(), vec!["        DCB \"deep\"".to_string()]);
        assert_eq!(run_with("        GET a\n", MapResolver(m)), vec!["DCB \"deep\""]);
    }

    #[test]
    fn a_missing_get_names_the_file() {
        assert!(fails("        GET hdr.Nope\n").contains("hdr.Nope"));
    }

    #[test]
    fn get_inside_a_skipped_branch_is_not_performed() {
        // Nothing to resolve, so this would fail if it were attempted.
        assert!(run("        [ {FALSE}\n        GET hdr.Nope\n        ]\n").is_empty());
    }

    // ---- diagnostics ----------------------------------------------------

    #[test]
    fn errors_report_the_macro_expansion_stack() {
        let src = concat!(
            "        MACRO\n",
            "        Bad\n",
            "        [ 1 / 0 = 1\n",
            "        ]\n",
            "        MEND\n",
            "        Bad\n"
        );
        let e = fails(src);
        assert!(e.contains("in macro Bad"), "{e}");
        assert!(e.contains("division by zero"), "{e}");
    }
}

#[cfg(test)]
mod layout_tests {
    use super::*;

    fn run(src: &str) -> Vec<String> {
        let lines: Vec<String> = src.lines().map(|s| s.to_string()).collect();
        let r = MapResolver(HashMap::new());
        let mut e = Expander::new(&r);
        match e.run("test", lines) {
            Ok(out) => out
                .into_iter()
                .filter(|l| !l.listing_only)
                .map(|l| l.text.trim().to_string())
                .filter(|s| !s.is_empty())
                .collect(),
            Err(err) => panic!("{err}"),
        }
    }

    fn fails(src: &str) -> String {
        let lines: Vec<String> = src.lines().map(|s| s.to_string()).collect();
        let r = MapResolver(HashMap::new());
        let mut e = Expander::new(&r);
        match e.run("test", lines) {
            Ok(o) => panic!("expected failure, got {o:?}"),
            Err(err) => err.to_string(),
        }
    }

    /// Bytes emitted by each line that emits any.
    fn bytes_of(src: &str) -> Vec<Vec<u8>> {
        let lines: Vec<String> = src.lines().map(|s| s.to_string()).collect();
        let r = MapResolver(HashMap::new());
        let mut e = Expander::new(&r);
        e.run("test", lines)
            .expect("expansion")
            .into_iter()
            .filter(|l| !l.listing_only && !l.bytes.is_empty())
            .map(|l| l.bytes)
            .collect()
    }

    /// Assert a value at expansion time. The operands of emitted lines are
    /// *not* rewritten, but passed through for the assembler. So an `ASSERT`
    /// is how the expander's own arithmetic is checked.
    fn check(setup: &str, assertion: &str) {
        run(&format!("{setup}        ASSERT {assertion}
"));
    }

    #[test]
    fn data_operands_are_evaluated_into_bytes() {
        // ObjAsm resolves these itself, and it must. `@`, `:SHL:` and
        // `?Label` have no equivalent a later assembler could compute. The
        // text is left as written, matching ObjAsm's listing.
        assert_eq!(bytes_of("Sym  *  7
        DCD Sym
"), vec![vec![7, 0, 0, 0]]);
        assert_eq!(bytes_of("        DCD 1 :SHL: 4
"), vec![vec![16, 0, 0, 0]]);
        assert_eq!(
            bytes_of("        ^ &100
X # 4
        DCD @
"),
            vec![vec![4, 1, 0, 0]]
        );
        assert_eq!(run("Sym  *  7
        DCD Sym
"), vec!["DCD Sym"]);
    }

    #[test]
    fn dcw_and_dcb_widths() {
        assert_eq!(bytes_of("        DCW &1234
"), vec![vec![0x34, 0x12]]);
        assert_eq!(bytes_of("        DCB \"hi\", 0
"), vec![vec![b'h', b'i', 0]]);
    }

    #[test]
    fn strings_in_dcb_are_left_exactly_as_written() {
        assert_eq!(run("        DCB \"abc\", 0
"), vec!["DCB \"abc\", 0"]);
    }

    #[test]
    fn instruction_operands_are_not_folded() {
        // Instruction operand rewriting belongs to lowering, not here.
        assert_eq!(run("Sym * 7
        MOV r0, #Sym
"), vec!["MOV r0, #Sym"]);
    }

    #[test]
    fn equ_defines_an_absolute_usable_in_conditionals() {
        let src = "Sym     *       4
        [ Sym = 4
        MOV r0, #1
        ]
";
        assert_eq!(run(src), vec!["MOV r0, #1"]);
    }

    #[test]
    fn equ_keyword_spelling_works() {
        check("Sym     EQU     7
", "Sym = 7");
    }

    #[test]
    fn map_and_field_build_a_record() {
        // ^ sets @. # gives a name the current @, then advances it.
        let setup = "        ^       0
A       #       4
B       #       2
C       #       0
";
        check(setup, "A = 0 :LAND: B = 4 :LAND: C = 6");
    }

    #[test]
    fn map_origin_is_honoured_and_at_is_readable() {
        check("        ^   &100
X   #   4
", "X = &100 :LAND: @ = &104");
    }

    #[test]
    fn at_starts_at_zero_without_a_map() {
        check("", "@ = 0");
    }

    #[test]
    fn keyword_spellings_of_map_and_field() {
        check("        MAP 0
A       FIELD 4
", "A = 0 :LAND: @ = 4");
    }

    #[test]
    fn assert_passes_quietly_and_fails_loudly() {
        assert!(run("        ASSERT 1 = 1
").is_empty());
        assert!(fails("        ASSERT 1 = 2
").contains("assertion failed"));
    }

    #[test]
    fn listing_directives_emit_nothing() {
        assert!(run("        OPT 2
        TTL A title
        SUBT Sub
").is_empty());
    }

    #[test]
    fn end_stops_the_file() {
        let out = run("        MOV r0, #1
        END
        MOV r1, #2
");
        assert_eq!(out, vec!["MOV r0, #1"]);
    }

    #[test]
    fn end_in_an_included_file_returns_to_the_includer() {
        // The manual says assembly continues after the GET.
        let mut m = HashMap::new();
        m.insert("inc".to_string(), vec!["        DCB 1".into(), "        END".into()]);
        let lines: Vec<String> = "        GET inc
        DCB 2
"
            .lines()
            .map(|s| s.to_string())
            .collect();
        let r = MapResolver(m);
        let mut e = Expander::new(&r);
        let out: Vec<String> = e
            .run("test", lines)
            .unwrap()
            .into_iter()
            .filter(|l| !l.listing_only)
            .map(|l| l.text.trim().to_string())
            .filter(|s| !s.is_empty())
            .collect();
        assert_eq!(out, vec!["DCB 1", "DCB 2"]);
    }

    #[test]
    fn the_location_counter_tracks_emitted_bytes() {
        check("        AREA Test, CODE
        MOV r0, #1
        MOV r1, #2
", ". = 8");
    }

    #[test]
    fn labels_take_the_current_address() {
        let setup = "        AREA Test, CODE
Start   MOV r0, #1
Next    MOV r1, #2
";
        check(setup, "Next - Start = 4");
    }

    #[test]
    fn dcb_strings_advance_by_their_length() {
        check("        AREA Test, DATA
        DCB \"abc\", 0
", ". = 4");
    }

    #[test]
    fn align_rounds_the_counter_up() {
        check("        AREA Test, DATA
        DCB 1
        ALIGN
", ". = 4");
    }

    #[test]
    fn area_restarts_the_counter() {
        let setup = "        AREA One, CODE
        MOV r0, #1
        AREA Two, CODE
";
        check(setup, ". = 0");
    }

    #[test]
    fn a_bad_area_attribute_is_reported() {
        assert!(fails("        AREA Test, WIBBLE
").contains("WIBBLE"));
    }
}

#[cfg(test)]
mod literal_pool_tests {
    use super::*;
    use std::collections::HashMap;

    /// The expanded lines, so a test can look at addresses and bytes.
    fn lines(src: &str) -> Vec<ExpandedLine> {
        let src: Vec<String> = src.lines().map(|s| s.to_string()).collect();
        let r = MapResolver(HashMap::new());
        let mut e = Expander::new(&r);
        match e.run("test", src) {
            Ok(o) => o,
            Err(err) => panic!("{err}"),
        }
    }

    /// The pool words, as the bytes of whichever line carries them.
    fn pool(src: &str) -> Vec<u32> {
        lines(src)
            .iter()
            .filter(|l| !l.listing_only && !l.bytes.is_empty())
            .flat_map(|l| l.bytes.as_chunks::<4>().0)
            .map(|w| u32::from_le_bytes(*w))
            .collect()
    }

    /// Where each `LDR =` was told to load from, in order.
    fn targets(src: &str) -> Vec<Option<u32>> {
        lines(src)
            .iter()
            .filter(|l| !l.listing_only && l.text.to_ascii_uppercase().contains("LDR"))
            .map(|l| l.literal)
            .collect()
    }

    const HEAD: &str = "        AREA c, CODE, READONLY\n";

    #[test]
    fn a_value_that_fits_an_immediate_takes_no_pool_word() {
        // MOV covers it, so nothing is reserved and the pool stays empty.
        let src = format!("{HEAD}        LDR r0, =0\n        LTORG\n        END\n");
        assert_eq!(targets(&src), vec![None]);
        assert!(pool(&src).is_empty());
    }

    #[test]
    fn a_complemented_immediate_takes_no_pool_word_either() {
        // MVN covers &FFFFFFFF.
        let src = format!("{HEAD}        LDR r0, =&FFFFFFFF\n        LTORG\n        END\n");
        assert_eq!(targets(&src), vec![None]);
        assert!(pool(&src).is_empty());
    }

    #[test]
    fn a_value_that_fits_neither_goes_in_the_pool() {
        let src = format!("{HEAD}        LDR r0, =&12345678\n        LTORG\n        END\n");
        assert_eq!(pool(&src), vec![0x1234_5678]);
        // The instruction is at 0 and the pool follows it.
        assert_eq!(targets(&src), vec![Some(4)]);
    }

    #[test]
    fn two_uses_of_one_value_share_a_word() {
        let src = format!(
            "{HEAD}        LDR r0, =&12345678\n        LDR r1, =&12345678\n        LTORG\n        END\n"
        );
        assert_eq!(pool(&src), vec![0x1234_5678], "one word, not two");
        assert_eq!(targets(&src), vec![Some(8), Some(8)], "both load the same word");
    }

    #[test]
    fn different_values_get_a_word_each() {
        let src = format!(
            "{HEAD}        LDR r0, =&12345678\n        LDR r1, =&AABBCCDD\n        LTORG\n        END\n"
        );
        assert_eq!(pool(&src), vec![0x1234_5678, 0xAABB_CCDD]);
        assert_eq!(targets(&src), vec![Some(8), Some(12)]);
    }

    #[test]
    fn ltorg_places_the_pool_where_it_stands() {
        // Two instructions, then the pool, so the word is at 8.
        let src = format!(
            "{HEAD}        LDR r0, =&12345678\n        MOV r1, #1\n        LTORG\n        END\n"
        );
        assert_eq!(targets(&src), vec![Some(8)]);
    }

    #[test]
    fn a_second_ltorg_starts_a_second_pool() {
        let src = format!(
            "{HEAD}        LDR r0, =&11111111\n        LTORG\n\
             \x20       LDR r1, =&22222222\n        LTORG\n        END\n"
        );
        assert_eq!(pool(&src), vec![0x1111_1111, 0x2222_2222]);
        // The first is loaded from 4 and the second from 12. Each is loaded
        // from its own pool, not both from the first.
        assert_eq!(targets(&src), vec![Some(4), Some(12)]);
    }

    #[test]
    fn end_flushes_the_pool_when_no_ltorg_did() {
        // "A default LTORG is executed at every END directive".
        let src = format!("{HEAD}        LDR r0, =&12345678\n        MOV pc, lr\n        END\n");
        assert_eq!(pool(&src), vec![0x1234_5678]);
        assert_eq!(targets(&src), vec![Some(8)]);
    }

    #[test]
    fn a_label_always_takes_a_pool_word() {
        // This holds even though the value would fit a MOV. The linker has to
        // be able to relocate it, and there is nowhere in a MOV to put a
        // relocation.
        let src = format!(
            "{HEAD}        LDR r0, =Here\n        MOV pc, lr\nHere    DCD 0\n        END\n"
        );
        // The default LTORG at END puts the pool after Here, at 12, and
        // the word holds Here's own offset for the linker to relocate.
        assert_eq!(targets(&src), vec![Some(12)]);
        assert_eq!(pool(&src), vec![0, 8], "Here's DCD, then the pool word");
    }

    #[test]
    fn the_pool_is_word_aligned() {
        // A byte directive leaves the counter odd. The pool must not start
        // there.
        let src = format!(
            "{HEAD}        LDR r0, =&12345678\n        DCB 1\n        LTORG\n        END\n"
        );
        // Instruction at 0, DCB at 4, so the pool aligns from 5 up to 8.
        assert_eq!(targets(&src), vec![Some(8)]);
    }

    #[test]
    fn an_ldr_with_an_addressing_mode_is_not_a_literal() {
        // `LDR r0,[r1,#=4]` is not valid, but in any case an `=` inside
        // brackets must not be taken as a literal.
        let src = format!("{HEAD}        LDR r0, [r1, #4]\n        LTORG\n        END\n");
        assert!(pool(&src).is_empty());
    }
}

#[cfg(test)]
mod linkage_tests {
    use super::*;
    use std::collections::HashMap;

    fn imports_of(src: &str) -> Vec<String> {
        let lines: Vec<String> = src.lines().map(|s| s.to_string()).collect();
        let r = MapResolver(HashMap::new());
        let mut e = Expander::new(&r);
        e.run("test", lines).expect("assembles");
        e.imports().to_vec()
    }

    #[test]
    fn bars_are_delimiters_not_part_of_an_imported_name() {
        // `IMPORT |Image$$RO$$Base|` names the symbol `Image$$RO$$Base`, and
        // the linker will not match it if the bars are kept.
        let got = imports_of(
            "        IMPORT  |Image$$RO$$Base|\n\
             \x20       AREA    x, DATA, REL\n\
             \x20       DCD     |Image$$RO$$Base|\n\
             \x20       END\n",
        );
        assert_eq!(got, vec!["Image$$RO$$Base".to_string()]);
    }

    #[test]
    fn an_unbarred_import_is_unchanged() {
        let got = imports_of(
            "        IMPORT  OS_Write0\n        AREA x, CODE\n        END\n",
        );
        assert_eq!(got, vec!["OS_Write0".to_string()]);
    }
}

#[cfg(test)]
mod storage_map_addressing_tests {
    use super::*;
    use std::collections::HashMap;

    /// A storage map based on a register, as the sources write one.
    const MAP: [&str; 7] = [
        "        AREA    x, CODE, READONLY",
        "wp      RN      12",
        "        ^       0, wp",
        "Slot1   #       4",
        "Slot2   #       4",
        "        ^       0",
        "mfield  #       4",
    ];

    fn expand(tail: &[&str]) -> (Expander<'static>, Vec<ExpandedLine>) {
        let mut lines: Vec<String> = MAP.iter().map(|s| s.to_string()).collect();
        lines.extend(tail.iter().map(|s| s.to_string()));
        lines.push("        END".into());
        // The resolver outlives the call because nothing here reads a file.
        let r: &'static MapResolver = Box::leak(Box::new(MapResolver(HashMap::new())));
        let mut e = Expander::new(r);
        let out = e.run("test", lines).expect("assembles");
        (e, out)
    }

    /// What each instruction hands the encoder, which is what these rewrites
    /// decide.
    fn lowered(tail: &[&str]) -> Vec<String> {
        let (e, out) = expand(tail);
        out.iter()
            .filter(|l| !l.listing_only)
            .filter_map(|l| {
                let lx = lex::lex_line(0, &l.text);
                let op = lx.opcode_str()?.to_string();
                let ops = e.encoder_operands(l, &op, lx.operands_str().unwrap_or(""));
                Some(format!("{op} {ops}"))
            })
            .collect()
    }

    #[test]
    fn a_register_relative_symbol_names_memory_through_its_base() {
        // `STR r0, Slot2` stores into the block `wp` points at, not to an
        // address. If left as a name, the access would become
        // program-relative.
        assert_eq!(lowered(&["        STR     r0, Slot2"]), ["STR r0,[r12, #0x4]"]);
    }

    #[test]
    fn an_expression_over_two_maps_keeps_the_based_one() {
        // As in `scratchbuffer1 + ms_action`. One symbol is an offset from
        // `wp`, and the other is an offset within the message block it names.
        assert_eq!(
            lowered(&["        LDR     r1, Slot2 + mfield"]),
            ["LDR r1,[r12, #0x4]"]
        );
    }

    #[test]
    fn adr_keeps_the_name_because_it_wants_the_address_not_the_contents() {
        // ADR on the same symbol is arithmetic on the base register, and the
        // driver decides it. Folding it to a load would give the wrong
        // instruction.
        assert_eq!(lowered(&["        ADR     r2, Slot2"]), ["ADR r2, Slot2"]);
    }

    #[test]
    fn the_base_may_be_named_by_an_alias() {
        // `^ 0, wp` rather than `^ 0, r12`, which is how the sources write it.
        let (e, _) = expand(&[]);
        assert_eq!(e.field_bases().get("Slot2"), Some(&12));
    }
}

#[cfg(test)]
mod macro_label_tests {
    use super::*;
    use std::collections::HashMap;

    /// `Entry` writes `$label ROUT`, so a routine's name reaches a directive
    /// as a macro parameter. Directives are acted on from the line before
    /// substitution, so the label has to be expanded where it is read.
    /// Otherwise the three and a half thousand routines written that way
    /// have no name, and every `ADR` at one of them has no target.
    #[test]
    fn a_label_written_as_a_macro_parameter_is_substituted() {
        let src = [
            "        AREA    x, CODE, READONLY",
            "        MACRO",
            "$label  Ent",
            "$label  ROUT",
            "        MOV     r0, #0",
            "        MEND",
            "        MOV     r1, #1",
            "Target  Ent",
            "        END",
        ];
        let r = MapResolver(HashMap::new());
        let mut e = Expander::new(&r);
        e.run("test", src.iter().map(|s| s.to_string()).collect())
            .expect("assembles");
        assert_eq!(e.label_defs().get("Target").map(|(_, a)| *a), Some(4));
        assert!(!e.label_defs().contains_key("$label"));
    }
}

#[cfg(test)]
mod macro_conditional_tests {
    use super::*;
    use std::collections::HashMap;

    fn assemble(src: &[&str]) -> R<Vec<ExpandedLine>> {
        let r = MapResolver(HashMap::new());
        let mut e = Expander::new(&r);
        e.run("test", src.iter().map(|s| s.to_string()).collect())
    }

    /// In `Immediate` in Hdr:Macros, the `MEXIT` that reports success stands
    /// inside the `[` that found it, and the `]` three lines below is never
    /// reached. Ten units in the corpus fail to assemble at all if what the
    /// macro left open lasts beyond it.
    #[test]
    fn mexit_from_inside_a_conditional_closes_it() {
        let out = assemble(&[
            "        AREA    x, CODE, READONLY",
            "        GBLL    found",
            "        MACRO",
            "        Look    $n",
            "found   SETL    {FALSE}",
            " [ $n = 1",
            "found   SETL    {TRUE}",
            "        MEXIT",
            " ]",
            "        MEND",
            "        Look    1",
            "        MOV     r0, #0",
            "        END",
        ])
        .expect("assembles");
        // The instruction after the invocation is reached and is not skipped.
        assert!(out.iter().any(|l| !l.listing_only && l.text.contains("MOV")));
    }

    /// A conditional the file itself left open is still an error, and the
    /// error says where it was opened. The corpus nests these across included
    /// files, and a bare "unclosed conditional" names nothing to look at.
    #[test]
    fn an_unclosed_conditional_in_a_file_names_its_line() {
        let e = assemble(&[
            "        AREA    x, CODE, READONLY",
            " [ {TRUE}",
            "        MOV     r0, #0",
            "        END",
        ])
        .expect_err("does not assemble");
        let text = e.to_string();
        assert!(text.contains("test:2"), "{text}");
        assert!(text.contains("never closed"), "{text}");
    }
}

#[cfg(test)]
mod base_operator_tests {
    use super::*;
    use std::collections::HashMap;

    fn run(src: &[&str]) -> R<Vec<ExpandedLine>> {
        let r = MapResolver(HashMap::new());
        let mut e = Expander::new(&r);
        e.run("test", src.iter().map(|s| s.to_string()).collect())
    }

    const MAP: [&str; 5] = [
        "        AREA    x, CODE, READONLY",
        "wp      RN      12",
        "        ^       0, wp",
        "Mutex   #       4",
        "Saved   #       4",
    ];

    fn with(tail: &[&str]) -> R<Vec<ExpandedLine>> {
        let mut v: Vec<&str> = MAP.to_vec();
        v.extend_from_slice(tail);
        v.push("        END");
        run(&v)
    }

    /// `ASSERT (:BASE:CurrentContext) = Rwp` checks a workspace layout
    /// against the register the code around it actually uses. The corpus
    /// uses `:BASE:` fifty-three times.
    #[test]
    fn base_is_the_register_the_map_was_based_on() {
        with(&["        ASSERT  :BASE:Mutex = wp"]).expect("assembles");
        with(&["        ASSERT  (:BASE:Saved) = 12"]).expect("assembles");
    }

    /// The offset within the map, which is the value the symbol already has.
    #[test]
    fn index_is_the_offset_from_that_register() {
        with(&["        ASSERT  :INDEX: Saved = 4"]).expect("assembles");
    }

    /// The manual says that with no register offsets, "BASE produces an
    /// error". A name from no based map is left for the evaluator to report.
    #[test]
    fn base_on_an_ordinary_symbol_is_still_an_error() {
        let e = run(&[
            "        AREA    x, CODE, READONLY",
            "Plain   *       4",
            "        ASSERT  :BASE:Plain = 0",
            "        END",
        ])
        .expect_err("does not assemble");
        assert!(e.to_string().contains("BASE"), "{e}");
    }
}

#[cfg(test)]
mod automatic_alignment_tests {
    use super::*;
    use std::collections::HashMap;

    /// Every byte the object gets, in order. That is what alignment is
    /// about. A gap that the location counter knows about but the data does
    /// not puts everything after it two bytes early.
    fn bytes(src: &[&str]) -> Vec<u8> {
        let r = MapResolver(HashMap::new());
        let mut e = Expander::new(&r);
        let out = e
            .run("test", src.iter().map(|s| s.to_string()).collect())
            .expect("assembles");
        out.iter().flat_map(|l| l.bytes.clone()).collect()
    }

    fn addresses(src: &[&str]) -> Vec<(String, u32)> {
        let r = MapResolver(HashMap::new());
        let mut e = Expander::new(&r);
        let out = e
            .run("test", src.iter().map(|s| s.to_string()).collect())
            .expect("assembles");
        out.iter()
            .filter(|l| !l.listing_only && !l.text.trim().is_empty())
            .map(|l| (l.text.trim().to_string(), l.addr))
            .collect()
    }

    /// DADebug puts `= "Debug start", 13, 10, 0` in the middle of a routine
    /// and carries straight on into `CMP`, with no ALIGN. The instruction is
    /// a word and is fetched from a word boundary.
    #[test]
    fn an_instruction_after_a_string_starts_on_a_word() {
        let got = addresses(&[
            "        AREA    x, CODE, READONLY",
            "        =       \"abc\"",
            "        CMP     r0, #1",
            "        END",
        ]);
        assert_eq!(got.last().map(|(_, a)| *a), Some(4));
    }

    /// And the padding is in the object, not only in the counter.
    #[test]
    fn the_padding_is_emitted() {
        let got = bytes(&[
            "        AREA    x, CODE, READONLY",
            "        =       \"abc\"",
            "        DCD     &12345678",
            "        END",
        ]);
        assert_eq!(got, vec![b'a', b'b', b'c', 0, 0x78, 0x56, 0x34, 0x12]);
    }

    /// `DCD` allocates words "aligned on four-byte boundaries". `DCW` aligns
    /// to a halfword. `DCB` takes whatever byte comes next.
    #[test]
    fn each_width_aligns_to_itself() {
        let got = bytes(&[
            "        AREA    x, DATA",
            "        DCB     1",
            "        DCW     &0203",
            "        DCB     4",
            "        END",
        ]);
        assert_eq!(got, vec![1, 0, 3, 2, 4]);
    }

    /// Touch reserves twelve bytes in the middle of a device block and then
    /// carries on with the block's words. They have to be in the object, not
    /// only in the counter, or everything after arrives three words early.
    #[test]
    fn space_reserves_its_bytes_in_the_object() {
        let got = bytes(&[
            "        AREA    x, DATA",
            "        DCD     &11111111",
            "        %       8",
            "        DCD     &22222222",
            "        END",
        ]);
        assert_eq!(got.len(), 16);
        assert_eq!(&got[4..12], &[0u8; 8]);
        assert_eq!(&got[12..], &[0x22, 0x22, 0x22, 0x22]);
    }

    /// NetFiler writes `quote DCB """", 0`. That is a string holding one
    /// quote, written as two, and a terminator. Taking every quote off each
    /// end leaves nothing, so the line was one byte where it should be two,
    /// and every address after it in the file was four bytes early.
    #[test]
    fn an_escaped_quote_is_a_character() {
        let got = bytes(&[
            "        AREA    x, DATA",
            "        DCB     \"\"\"\", 0",
            "        END",
        ]);
        assert_eq!(got, vec![b'"', 0]);
    }

    #[test]
    fn a_string_may_still_run_on_unaligned() {
        let got = bytes(&[
            "        AREA    x, DATA",
            "        DCB     \"ab\"",
            "        DCB     \"c\"",
            "        END",
        ]);
        assert_eq!(got, vec![b'a', b'b', b'c']);
    }
}

#[cfg(test)]
mod waiting_label_tests {
    use super::*;
    use std::collections::HashMap;

    fn labels(src: &[&str]) -> std::collections::HashMap<String, (usize, u32)> {
        let r = MapResolver(HashMap::new());
        let mut e = Expander::new(&r);
        e.run("test", src.iter().map(|s| s.to_string()).collect())
            .expect("assembles");
        e.label_defs().clone()
    }

    /// BBCEconet writes a table of thirteen bytes, then `OpenRx ROUT`, then
    /// code, with no ALIGN in between. The branches to `OpenRx` are meant
    /// for the code. A label with nothing of its own belongs to whatever
    /// comes next, and goes wherever the alignment puts it.
    #[test]
    fn a_label_with_no_bytes_moves_with_the_alignment() {
        let got = labels(&[
            "        AREA    x, CODE, READONLY",
            "        DCB     1, 2, 3",
            "Here    ROUT",
            "        MOV     r0, #0",
            "        END",
        ]);
        assert_eq!(got.get("Here").map(|(_, a)| *a), Some(4));
    }

    /// A label with bytes of its own stays with them.
    #[test]
    fn a_label_on_a_data_line_keeps_its_address() {
        let got = labels(&[
            "        AREA    x, CODE, READONLY",
            "        DCB     1",
            "Here    DCB     2, 3",
            "        MOV     r0, #0",
            "        END",
        ]);
        assert_eq!(got.get("Here").map(|(_, a)| *a), Some(1));
    }

    /// And one that needs no alignment does not move.
    #[test]
    fn a_label_before_aligned_code_stays_put() {
        let got = labels(&[
            "        AREA    x, CODE, READONLY",
            "        DCB     1, 2, 3, 4",
            "Here",
            "        MOV     r0, #0",
            "        END",
        ]);
        assert_eq!(got.get("Here").map(|(_, a)| *a), Some(4));
    }
}

#[cfg(test)]
mod local_label_scope_tests {
    use super::*;
    use std::collections::HashMap;

    fn words(src: &[&str]) -> Vec<i64> {
        let r = MapResolver(HashMap::new());
        let mut e = Expander::new(&r);
        let out = e
            .run("test", src.iter().map(|s| s.to_string()).collect())
            .expect("assembles");
        out.iter()
            .filter(|l| !l.listing_only)
            .filter_map(|l| {
                let lx = lex::lex_line(0, &l.text);
                let op = lx.opcode_str()?.to_string();
                if !op.eq_ignore_ascii_case("B") {
                    return None;
                }
                let ops = e.encoder_operands(l, &op, lx.operands_str().unwrap_or(""));
                // `.±N` is what the folding leaves for the encoder.
                let t = ops.trim().to_string();
                match t.strip_prefix(".+") {
                    Some(n) => n.parse::<i64>().ok(),
                    None => t
                        .strip_prefix(".-")
                        .and_then(|n| n.parse::<i64>().ok())
                        .map(|n| -n),
                }
            })
            .collect()
    }

    /// `DivRem` writes an `01` and an `02` of its own. StringLib's `B %FT02`
    /// after it means the caller's `02`, four instructions past the end of
    /// the expansion. It does not mean the macro's, which is inside it.
    #[test]
    fn a_macro_keeps_its_local_labels_to_itself() {
        let got = words(&[
            "        AREA    x, CODE, READONLY",
            "        MACRO",
            "        Twice",
            "02",
            "        MOV     r0, #0",
            "        MOV     r1, #1",
            "        MEND",
            "        B       %FT02",
            "        Twice",
            "02",
            "        MOV     r2, #2",
            "        END",
        ]);
        // The branch goes past the two instructions the macro laid down, to
        // the `02` the caller wrote, not to the one four bytes inside it.
        assert_eq!(got, vec![12]);
    }

    /// `%BA` looks across every level, which is how spinrw's `MetaLock`
    /// branches back to a `10` its caller wrote.
    #[test]
    fn the_all_levels_form_reaches_the_caller() {
        let got = words(&[
            "        AREA    x, CODE, READONLY",
            "        MACRO",
            "        Back",
            "        B       %BA10",
            "        MEND",
            "10",
            "        MOV     r0, #0",
            "        Back",
            "        END",
        ]);
        assert_eq!(got, vec![-4]);
    }
}
