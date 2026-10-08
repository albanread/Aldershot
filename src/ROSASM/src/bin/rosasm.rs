//! `rosasm` — assemble ObjAsm source to an AOF object, or to an ELF one.
//!
//!     rosasm <source> -o <object> [-I dir]... [-PD "Sym SETA 1"]...
//!                      [--map <file>] [--elf] [--keep-temps]
//!
//! The pipeline: expand the macro language, lower each instruction to UAL,
//! hand that to LLVM's integrated assembler for encoding, then write the
//! result out. The output is AOF, which is what the RISC OS linker reads, or
//! with `--elf` it is ELF, which is what roscc reads.
//!
//! LLVM is used only as an encoder. Everything above the mnemonic is ours:
//! the macro language, conditional assembly, the symbol table and layout. So
//! is everything below the object file.
//!
//! The two output formats are siblings. Neither is a conversion of the
//! other. Because the encoder is clang, every relocation arrives here in ELF
//! terms already. AOF's flag word and AOF's addend convention are worked out
//! from those, and so is ELF's. So `--elf` is the shorter path, not a second
//! translation on top of a first.

use std::collections::HashMap;
use std::path::{Path, PathBuf};
use std::process::Command;

use rosasm::aof::{self, area_attr, sym_attr};
use rosasm::elfread;
use rosasm::elfwrite;
use rosasm::emitc;
use rosasm::expand::{self, Expander, ExpandedLine, FileResolver};
use rosasm::legalize::{self, AdrTarget, Legalized};
use rosasm::lex;
use rosasm::reloc;
use rosasm::source::SourceFile;

/// Where to find the encoder. clang drives LLVM's integrated assembler.
///
/// Two machines means two answers, so this cannot be a constant. `--clang`
/// names it outright. `ROSASM_CLANG` names it for a whole build; the sweep
/// and the corpus harness set that. Failing both, the platform decides. On
/// Windows that is the installer's path, because by convention clang is not
/// on PATH there. Everywhere else it is the bare name, found on PATH.
/// Whichever clang is found must be able to assemble for TARGET below. That
/// is the only thing rosasm asks of it.
fn default_encoder() -> String {
    match std::env::var("ROSASM_CLANG") {
        Ok(p) if !p.is_empty() => p,
        _ => {
            if cfg!(windows) {
                r"C:\Program Files\LLVM\bin\clang.exe".to_string()
            } else {
                "clang".to_string()
            }
        }
    }
}
/// Pi 4: ARMv8-A in AArch32 with NEON.
const TARGET: &[&str] = &[
    "--target=arm-none-eabi",
    "-mcpu=cortex-a72",
    // The A72's floating point is VFPv4 with NEON. Plain `neon` is VFPv3,
    // and rejects the fused multiply-adds the sources use.
    "-mfpu=neon-fp-armv8",
];

struct Dirs {
    dirs: Vec<PathBuf>,
    vars: HashMap<String, Vec<PathBuf>>,
}

/// The host paths a RISC OS file name could stand for, relative to a search
/// directory. RISC OS writes `dir.file`, but the sources also write
/// `file.dir`. The DDE resolves both, which lets one tree be read from a
/// RISC OS host or a Unix-style one.
///
/// A name that contains a host separator is read as a host path and nothing
/// else. `GET ../VersionASM` and `GET ../../kernel/k_atomic.s` mean what
/// they say, and turning their dots into separators would make nonsense of
/// them. The type suffix still moves to a directory, because
/// `kernel/k_atomic.s` is `kernel.s.k_atomic` however it is spelt.
fn relative_forms(name: &str) -> Vec<String> {
    let name = name.trim();
    if name.contains('/') || name.contains('\\') {
        let n = name.replace('\\', "/");
        let mut out = vec![n.clone()];
        if let Some((head, tail)) = n.rsplit_once('.') {
            if !tail.contains('/') {
                if let Some((dir, leaf)) = head.rsplit_once('/') {
                    out.push(format!("{dir}/{tail}/{leaf}"));
                }
            }
        }
        return out.into_iter().map(|f| parents(&f)).collect();
    }
    let mut out = vec![name.replace('.', "/")];
    if let Some((head, tail)) = name.rsplit_once('.') {
        let head = head.replace('.', "/");
        // `s.Foo` written the other way round.
        let swapped = format!("{tail}/{head}");
        if !out.contains(&swapped) {
            out.push(swapped);
        }
        // `clib.s.cl_data` written as `clib/cl_data.s`. The type directory
        // belongs immediately before the leaf, not at the front.
        let nested = match head.rsplit_once('/') {
            Some((dir, leaf)) => format!("{dir}/{tail}/{leaf}"),
            None => format!("{tail}/{head}"),
        };
        if !out.contains(&nested) {
            out.push(nested);
        }
    }
    out.into_iter().map(|f| parents(&f)).collect()
}

/// Turn each `^` path element into `..`. `^` is RISC OS for the directory
/// above, and the sources use it to reach out of a component:
/// `GET ^.^.s.HeapMan` from `Kernel/Dev/HeapTest` is `Kernel/s/HeapMan`.
fn parents(path: &str) -> String {
    if !path.contains('^') {
        return path.to_string();
    }
    path.split('/')
        .map(|p| if p == "^" { ".." } else { p })
        .collect::<Vec<_>>()
        .join("/")
}

impl Dirs {
    /// Where on the host a RISC OS name could be, in the order to try.
    fn candidates(&self, name: &str) -> Vec<PathBuf> {
        let mut cands: Vec<PathBuf> = Vec::new();
        if let Some((var, rest)) = name.split_once(':') {
            if let Some(ds) = self.vars.get(&var.to_ascii_lowercase()) {
                for rel in relative_forms(rest) {
                    cands.extend(ds.iter().map(|d| d.join(&rel)));
                }
            }
        } else {
            for rel in relative_forms(name) {
                cands.extend(self.dirs.iter().map(|d| d.join(&rel)));
            }
        }
        cands
    }
}

impl FileResolver for Dirs {
    fn resolve(&self, name: &str) -> Option<(String, Vec<String>)> {
        let name = name.trim();
        for p in self.candidates(name) {
            if p.is_file() {
                if let Ok(sf) = SourceFile::load(&p) {
                    return Some((name.to_string(), sf.lines));
                }
            }
        }
        None
    }

    fn host_file(&self, name: &str) -> Option<(PathBuf, u32)> {
        self.candidates(name.trim()).iter().find_map(|p| rosasm::expand::typed_file(p))
    }
}

/// Render the expanded lines as a UAL assembly file for the encoder.
///
/// Data directives are not emitted again. Their bytes were computed during
/// expansion, where `@`, `?label` and the ObjAsm operators have meaning.
/// Only instructions go to LLVM.
fn to_ual(
    lines: &[ExpandedLine],
    ex: &Expander,
    refused: &mut Vec<Unencodable>,
    allow: bool,
    fpa_to_vfp: bool,
) -> (String, Vec<usize>, Vec<AdrReloc>) {
    // The directives must agree with the command line, because they win
    // where the two disagree. `.fpu neon` is VFPv3, and would refuse the
    // A72's fused multiply-adds however the driver was invoked.
    let mut s = String::from(
        "        .syntax unified\n\
         \x20       .arch armv8-a\n\
         \x20       .fpu neon-fp-armv8\n\
         \x20       .text\n",
    );
    let mut index = Vec::new();
    let mut adr_relocs: Vec<AdrReloc> = Vec::new();
    for (i, l) in lines.iter().enumerate() {
        if l.listing_only || !l.bytes.is_empty() {
            continue;
        }
        let lx = lex::lex_line(l.origin.line, &l.text);
        let Some(op) = lx.opcode_str() else { continue };
        if is_directive(op) {
            continue;
        }
        // `SWI OS_Write0` names the SWI, and the name is a symbol from a
        // header. UAL wants an immediate, so mark it as one to evaluate.
        // Use the operands as the expander froze them, not as the line
        // reads, because a variable an operand names may have changed since.
        let raw: &str = &l.operands;
        let raw = if rosasm::lower::is_swi(op) && !raw.trim_start().starts_with('#') {
            format!("#{raw}")
        } else {
            raw.to_string()
        };
        // Everything ObjAsm understands and LLVM does not is resolved here:
        // expressions, register aliases and bar-quoted names.
        let mut operands = ex.encoder_operands(l, op, &raw);
        // An immediate ObjAsm took without its `#`: add the `#`, and
        // evaluate again.
        if let Some(marked) = rosasm::lower::implicit_immediate(op, &operands, &raw) {
            operands = ex.encoder_operands(l, op, &marked);
        }
        let operands = rosasm::lower::translate_numbers(&operands);
        // One label per line lets the encoded bytes be matched back to the
        // line that produced them, whatever the instruction expands to.
        s.push_str(&format!("__ros{i}:\n"));
        // A literal the expander could not fold into the instruction lives in
        // a pool, and the pool is in this same area, so the distance to it is
        // fixed however the area is placed.
        if let Some(target) = l.literal {
            let loaded = if rosasm::vfp::is_vfp(op) {
                vfp_pool_load(op, &operands, l.addr, target)
            } else if rosasm::fpa::is_fpa(op) {
                fpa_pool_load(op, &operands, l.addr, target)
            } else {
                pool_load(op, &operands, l.addr, target)
            };
            match loaded {
                Ok(text) => {
                    s.push_str(&format!("        {text}\n"));
                    index.push(i);
                    continue;
                }
                Err(why) => {
                    s.push_str(&placeholder(op, refused.len(), allow, fpa_to_vfp));
                    refused.push(Unencodable::of(l, why));
                    index.push(i);
                    continue;
                }
            }
        }
        // Legalization decides what the instruction can become: itself, an
        // expansion, or nothing the encoder will accept.
        // An `ADR` at an imported symbol carries a relocation of its own,
        // made here. The encoder is handed arithmetic on `pc`, so it has
        // nothing to record.
        let external = adr_external(op, &operands, ex);
        if let Some(name) = external.clone() {
            adr_relocs.push(AdrReloc {
                line: i,
                name,
                instructions: rosasm::lower::instruction_words(op, fpa_to_vfp) as u8,
            });
        }
        let ctx = legalize::Context {
            here: l.addr,
            target: adr_target(op, &operands, l, ex),
            relocated: external.is_some(),
            fpa_to_vfp,
        };
        match legalize::legalize(op, &operands, &ctx) {
            Legalized::One(m, o) => s.push_str(&format!("        {m} {o}\n")),
            Legalized::Many(v) => {
                for (m, o) in v {
                    s.push_str(&format!("        {m} {o}\n"));
                }
            }
            Legalized::RawWord(w) => s.push_str(&format!("        .inst 0x{w:08X}\n")),
            Legalized::Unsupported(why) => {
                // The space stays occupied either way, so later addresses do
                // not shift and the rest of the object stays readable.
                s.push_str(&placeholder(op, refused.len(), allow, fpa_to_vfp));
                refused.push(Unencodable::of(l, why));
            }
        }
        index.push(i);
    }
    (s, index, adr_relocs)
}

/// Something this assembler could not encode.
///
/// A zero word is `ANDEQ r0, r0, r0`. It executes, does nothing, and gives
/// no sign. So an object holding one quietly does the wrong thing where an
/// instruction should have been. Every such case is collected, and the run
/// fails unless the caller has asked otherwise.
struct Unencodable {
    /// Where the source said it, as `file:line`.
    where_: String,
    /// The line as written.
    what: String,
    why: String,
}

impl Unencodable {
    fn of(l: &ExpandedLine, why: String) -> Self {
        Self {
            where_: format!("{}:{}", l.origin.file, l.origin.line),
            what: l.text.trim().to_string(),
            why,
        }
    }
}

/// An `ADR` whose target only the linker knows.
struct AdrReloc {
    /// Index into the expanded lines, which is how its bytes are found again.
    line: usize,
    name: String,
    /// How many instructions the linker may rewrite. For `ADRL` it is two.
    instructions: u8,
}

/// Say what the encoder said, against the source rather than the lowering.
///
/// clang names a line in a temporary file, which is no use to the reader.
/// What the reader needs is which line of which `.s` it came from. Every
/// instruction in the lowered text carries a `__ros<i>` label naming the
/// expanded line it came from. So walking back from the line in the
/// diagnostic to the nearest label gives the answer.
fn report_encoder(stderr: &str, ual: &str, lines: &[ExpandedLine]) {
    let lowered: Vec<&str> = ual.lines().collect();
    for line in stderr.lines() {
        // `<path>:<line>:<col>: <severity>: <message>`. The path has a colon
        // of its own on this host, so the line is read from the right.
        let Some((head, severity, message)) = ["error", "warning", "note"]
            .iter()
            .find_map(|s| {
                let mark = format!(": {s}: ");
                line.find(&mark)
                    .map(|i| (&line[..i], *s, line[i + mark.len()..].trim()))
            })
        else {
            continue;
        };
        let mut fields = head.rsplit(':');
        let (Some(_col), Some(at)) = (fields.next(), fields.next()) else {
            continue;
        };
        let Ok(at) = at.trim().parse::<usize>() else { continue };
        match origin_of(&lowered, at, lines) {
            Some((where_, text)) => {
                eprintln!("rosasm: {where_}: {severity}: {message}");
                eprintln!("        {text}");
            }
            None => eprintln!("rosasm: {severity}: {message}"),
        }
    }
}

/// The source line an instruction in the lowered text came from.
fn origin_of<'a>(
    lowered: &[&str],
    at: usize,
    lines: &'a [ExpandedLine],
) -> Option<(String, &'a str)> {
    let mut i = at.min(lowered.len()).checked_sub(1)?;
    loop {
        if let Some(n) = lowered[i].strip_prefix("__ros").and_then(|t| t.strip_suffix(':')) {
            let l = lines.get(n.parse::<usize>().ok()?)?;
            return Some((format!("{}:{}", l.origin.file, l.origin.line), l.text.trim()));
        }
        i = i.checked_sub(1)?;
    }
}

/// The space an instruction was given, filled with something that says so.
///
/// It is as many words as the location counter reserved: two for `ADRL`,
/// two for an FPA compare translated to VFP, and one for everything else.
/// Emitting a single word instead would move every label after it.
///
/// `UDF #n` traps where a zero word would have run on. `n` says which of
/// the listed instructions it stands for.
fn placeholder(mnemonic: &str, n: usize, allow: bool, fpa_to_vfp: bool) -> String {
    let words = rosasm::lower::instruction_words(mnemonic, fpa_to_vfp);
    if allow {
        format!("        UDF #{n}\n").repeat(words)
    } else {
        "        .inst 0x00000000\n".repeat(words)
    }
}

/// An `LDR Rd,=value` rendered as a load from the literal pool.
///
/// The offset is twelve bits with a sign, so a pool more than 4KB away is
/// out of reach. That is the whole reason `LTORG` exists, and the error
/// says so.
fn pool_load(mnemonic: &str, operands: &str, here: u32, target: u32) -> Result<String, String> {
    let rd = operands
        .split_once('=')
        .map(|(head, _)| head.trim().trim_end_matches(',').trim())
        .unwrap_or("r0");
    let delta = target as i64 - here as i64;
    // `pc` reads eight ahead, and the encoder works that out from `.` itself.
    if !(-4087..=4103).contains(&delta) {
        return Err(format!(
            "the literal pool is {delta} bytes away; an LDR reaches 4KB, so this \
             needs an LTORG nearer the instruction"
        ));
    }
    let m = rosasm::lower::normalise_mnemonic(mnemonic).unwrap_or_else(|| mnemonic.to_string());
    Ok(if delta < 0 {
        format!("{m} {rd}, .-{}", -delta)
    } else {
        format!("{m} {rd}, .+{delta}")
    })
}

/// An FPA load from the literal pool, as the word FPEmulator will read.
///
/// The same pool and the same distance as `pool_load`, but nothing in the
/// answer goes through the encoder. There is no coprocessor 1 here for LLVM
/// to know about, so the word is built and emitted directly.
///
/// The offset is a count of words in eight bits, so an FPA load reaches
/// 1020 bytes where an `LDR` reaches four thousand. A pool that is fine for
/// the rest of a file can be out of reach from here, so the error must say
/// which limit it ran into.
fn fpa_pool_load(mnemonic: &str, operands: &str, here: u32, target: u32) -> Result<String, String> {
    let rd = operands
        .split_once('=')
        .map(|(head, _)| head.trim().trim_end_matches(',').trim())
        .unwrap_or("f0");
    let delta = target as i64 - here as i64;
    // `pc` reads eight bytes ahead, which the offset below is measured from.
    if !(-1012..=1028).contains(&delta) {
        return Err(format!(
            "the literal pool is {delta} bytes away; an FPA load reaches 1020 \
             bytes, so this needs an LTORG nearer the instruction"
        ));
    }
    let addr = if delta < 0 {
        format!(".-{}", -delta)
    } else {
        format!(".+{delta}")
    };
    match rosasm::fpa::encode(mnemonic, &format!("{rd}, {addr}")) {
        Some(Legalized::RawWord(w)) => Ok(format!(".inst 0x{w:08X}")),
        Some(Legalized::Unsupported(why)) => Err(why),
        _ => Err(format!("{mnemonic} is not an FPA load")),
    }
}

/// A VFP load from the literal pool, as the word to emit.
///
/// The pool and the distance are `pool_load`'s, but the reach is a
/// kilobyte, and the word is built here. The encoder will not parse a
/// `VLDR` whose base is a label expression. And where the base is `pc`, it
/// wants it written in a way that leaves no room for our offset arithmetic.
fn vfp_pool_load(mnemonic: &str, operands: &str, here: u32, target: u32) -> Result<String, String> {
    rosasm::vfp::pool_load_word(mnemonic, operands, here, target)
        .map(|w| format!(".inst 0x{w:08X}"))
}

/// The imported symbol an `ADR` reaches for, if that is what it names.
///
/// An example is `ADRL ip, cpuclock_Activate` in BCMSupport's device
/// veneers. The symbol is `IMPORT`ed, so its address is not known here and
/// cannot be. ObjAsm assembles the address as zero and leaves a relocation
/// on the pair of instructions for the linker to finish. This function is
/// what lets us do the same.
fn adr_external(op: &str, operands: &str, ex: &Expander) -> Option<String> {
    if !rosasm::lower::is_adr(op) && !rosasm::lower::is_adrl(op) {
        return None;
    }
    let target = operands.split_once(',')?.1.trim();
    let names = expand::identifiers(target);
    let mut wanted = names.iter().filter(|n| ex.imports().contains(n));
    let name = wanted.next()?.clone();
    // One imported symbol is a relocation. Two in one expression make a
    // distance the linker has no way to compute.
    wanted.next().is_none().then_some(name)
}

/// What an `ADR`/`ADRL` is aiming at, when we can supply it.
///
/// The manual gives three kinds of expression: register-relative,
/// program-relative and numeric. Each becomes a different instruction, so
/// the kind is worked out here rather than in the encoding.
///
/// By the time the operands reach here, `encoder_operands` has already
/// turned a label or a local label in this same area into an offset from
/// `.`, which for this instruction is its own address. That is what marks
/// the expression as program-relative. It must also be read that way rather
/// than evaluated, because `.` in the symbol table holds wherever the
/// location counter finished, not where this line is.
///
/// A target in another area has no fixed distance from here. So it is
/// refused rather than expanded into something that would be right only by
/// accident.
fn adr_target(op: &str, operands: &str, l: &ExpandedLine, ex: &Expander) -> Option<AdrTarget> {
    if !rosasm::lower::is_adr(op) && !rosasm::lower::is_adrl(op) {
        return None;
    }
    let target = operands.split_once(',')?.1.trim();

    // An imported symbol has no address here. ObjAsm assembles the
    // expression with the symbol standing at zero, and adds a relocation.
    // The instructions come out the same either way, and the relocation
    // supplies the rest.
    if let Some(name) = adr_external(op, operands, ex) {
        let text = target.replace(&name, "0");
        return match rosasm::expr::eval(&text, ex.symbols()) {
            Ok(rosasm::symtab::Value::Arith(n)) => Some(AdrTarget::Program(n)),
            _ => None,
        };
    }

    // Program-relative: `.`, or an expression built on it such as the
    // `dtanid + (16 * 3)` the sources write.
    if let Some(rest) = target.strip_prefix('.') {
        if rest.is_empty() || rest.starts_with(['+', '-']) {
            let text = format!("{}{rest}", l.addr);
            return match rosasm::expr::eval(&text, ex.symbols()) {
                Ok(rosasm::symtab::Value::Arith(n)) => Some(AdrTarget::Program(n)),
                _ => None,
            };
        }
        return None;
    }

    let names = expand::identifiers(target);
    for name in &names {
        if let Some((area, _)) = ex.label_defs().get(name) {
            if *area != l.area_index {
                eprintln!(
                    "rosasm: {}:{}: {op} reaches into another area, which has no fixed distance",
                    l.origin.file, l.origin.line
                );
                return None;
            }
        }
    }
    let value = match rosasm::expr::eval(target, ex.symbols()) {
        Ok(rosasm::symtab::Value::Arith(n)) => n,
        _ => return None,
    };
    // Register-relative: a `MAP expr,Rn` made this symbol an offset from Rn.
    // Two symbols with different bases in one expression have no meaning,
    // so that case is left unresolved rather than guessed at.
    let bases: Vec<u32> = names
        .iter()
        .filter_map(|n| ex.field_bases().get(n).copied())
        .collect();
    if let Some(base) = bases.first() {
        if bases.iter().all(|b| b == base) {
            // A storage map may be based below its register, so the offset
            // reads as signed.
            return Some(AdrTarget::Register { base: *base, offset: value as i32 });
        }
        return None;
    }
    // Numeric: not relative to anything, so it is moved rather than added.
    Some(AdrTarget::Numeric(value))
}

/// Where one run of the encoder's output ended up in an area.
struct Segment {
    /// Byte range within the encoder's `.text`.
    text: std::ops::Range<usize>,
    /// Index of the area it was copied into, and the offset there.
    area: usize,
    dest: u32,
    /// The expanded line it came from.
    line: usize,
}

fn word_at(b: &[u8], off: u32) -> u32 {
    let i = off as usize;
    if i + 4 > b.len() {
        0
    } else {
        u32::from_le_bytes([b[i], b[i + 1], b[i + 2], b[i + 3]])
    }
}

fn set_word_at(b: &mut [u8], off: u32, w: u32) {
    let i = off as usize;
    if i + 4 <= b.len() {
        b[i..i + 4].copy_from_slice(&w.to_le_bytes());
    }
}

fn field_type(width: u8) -> Option<aof::FieldType> {
    match width {
        1 => Some(aof::FieldType::Byte),
        2 => Some(aof::FieldType::HalfWord),
        4 => Some(aof::FieldType::Word),
        _ => None,
    }
}

/// Index of `name` in the symbol table. If the source never mentioned it,
/// it is added as an external reference. That happens whenever a branch
/// names a symbol the source neither defines nor imports.
fn symbol_index(symbols: &mut Vec<aof::Symbol>, name: &str) -> u32 {
    if let Some(i) = symbols.iter().position(|s| s.name == name) {
        return i as u32;
    }
    symbols.push(external(name));
    (symbols.len() - 1) as u32
}

/// An undefined global: the linker resolves it against another object.
fn external(name: &str) -> aof::Symbol {
    aof::Symbol {
        name: name.to_string(),
        attributes: sym_attr::GLOBAL,
        value: 0,
        area: None,
    }
}

fn is_directive(op: &str) -> bool {
    rosasm::vocab::is_directive(op) || rosasm::vocab::is_symbolic(op)
}

/// Puts an addend back into an instruction: None if it does not fit.
type AddendSetter = fn(u32, i32) -> Option<u32>;

fn main() {
    let args: Vec<String> = std::env::args().skip(1).collect();
    let mut source: Option<PathBuf> = None;
    let mut out: Option<PathBuf> = None;
    let mut dirs: Vec<PathBuf> = Vec::new();
    let mut pds: Vec<String> = Vec::new();
    let mut keep = false;
    let mut map: Option<PathBuf> = None;
    let mut warn_assertions = false;
    let mut allow_unencodable = false;
    let mut fpa_to_vfp = false;
    let mut elf_out = false;
    let mut emit_c = false;
    let mut rom_base: Option<u32> = None;
    let mut swis: Option<PathBuf> = None;
    let mut swi_regs: Option<PathBuf> = None;
    let mut apcs = false;
    let mut lift = true;
    let mut poll_loops = false;
    let mut clang: Option<String> = None;
    let mut i = 0;
    while i < args.len() {
        match args[i].as_str() {
            "-o" => {
                i += 1;
                out = args.get(i).map(PathBuf::from);
            }
            "-I" | "-i" => {
                i += 1;
                if let Some(d) = args.get(i) {
                    dirs.push(PathBuf::from(d));
                }
            }
            "-PD" | "-pd" => {
                i += 1;
                if let Some(d) = args.get(i) {
                    pds.push(d.clone());
                }
            }
            "--keep-temps" => keep = true,
            // The sources assert their own layout, so a failure is a defect
            // and stops the build. Investigating one needs the opposite.
            "--warn-assertions" => warn_assertions = true,
            // An object with instructions missing from it is not an object
            // a ROM can be built with, so saying so is the default. This
            // asks for one anyway, with every gap trapping at run time.
            "--allow-unencodable" => allow_unencodable = true,
            // Translate FPA into VFP. This is not for building this ROM,
            // where FPEmulator reads the FPA word back and interprets it. It
            // is for asking what the sources would look like using the
            // floating point the hardware has.
            "--fpa-to-vfp" => fpa_to_vfp = true,
            // AOF is what the RISC OS linker reads, and is the default.
            // roscc reads ELF and nothing else. So this switch is what lets a
            // hand-written ObjAsm header be linked against clang's output,
            // giving a module built on the Mac with no DDE anywhere in it.
            "--elf" => elf_out = true,
            // The ObjAsm compiler. -o names a C file. It holds the unit's
            // ROM image at --rom-base and its code compiled to C. A header
            // of label addresses is written beside it (RISCOSGrandDesign,
            // design 12).
            "--emit" => {
                i += 1;
                match args.get(i).map(String::as_str) {
                    Some("c") => emit_c = true,
                    Some("aof") => {}
                    Some("elf") => elf_out = true,
                    other => {
                        eprintln!("rosasm: --emit takes c, aof or elf, not {other:?}");
                        std::process::exit(2);
                    }
                }
            }
            "--rom-base" => {
                i += 1;
                rom_base = args.get(i).and_then(|v| {
                    let v = v.trim_start_matches('&');
                    let v = v.strip_prefix("0x").or_else(|| v.strip_prefix("0X")).unwrap_or(v);
                    u32::from_str_radix(v, 16).ok()
                });
            }
            // Compile integer instructions one by one, as tier 0 does. This
            // is the reference the lifted C is tested against.
            "--no-lift" => lift = false,
            // Make every loop a safe point for the runtime's background
            // work. This is for an interpreter, whose Escape must be able to
            // stop a program's loop.
            "--poll-loops" => poll_loops = true,
            // The unit follows APCS, as compiler output does. So the C back
            // end may treat f1-f3 as dead at a return (see emitc).
            "--abi" => {
                i += 1;
                apcs = args.get(i).is_some_and(|a| a == "apcs");
            }
            // Kernel SWIs the runtime implements natively, one per line:
            // "0x0001E OS_Module". Compiled code calls their thunks directly.
            "--swis" => {
                i += 1;
                swis = args.get(i).map(PathBuf::from);
            }
            // The registers each SWI reads and writes, one per line:
            // "0x0001E in=0x0009 out=0x0005 OS_Module".
            "--swi-regs" => {
                i += 1;
                swi_regs = args.get(i).map(PathBuf::from);
            }
            // The encoder is not in the same place on two machines, and a
            // build that guesses wrong should be told, not reconfigured.
            "--clang" => {
                i += 1;
                clang = args.get(i).cloned();
            }
            "--map" => {
                i += 1;
                map = args.get(i).map(PathBuf::from);
            }
            s => source = Some(PathBuf::from(s)),
        }
        i += 1;
    }
    let (Some(source), Some(out)) = (source, out) else {
        eprintln!(
            "usage: rosasm <source> -o <object> [-I dir]... [-PD assignment]... \
             [--map file] [--elf] [--warn-assertions] [--allow-unencodable] \
             [--fpa-to-vfp] [--clang path] [--keep-temps]\n       \
             rosasm <source> --emit c --rom-base <hex> [--swis file] [--swi-regs file] [--abi apcs] [--no-lift] [--poll-loops] \
             -o <file.c> ..."
        );
        std::process::exit(2);
    };
    // --clang, then ROSASM_CLANG, then whatever the platform calls it.
    let clang = clang.unwrap_or_else(default_encoder);
    if emit_c && rom_base.is_none() {
        eprintln!("rosasm: --emit c needs --rom-base: compiled code uses the ROM's addresses");
        std::process::exit(2);
    }

    let sf = match SourceFile::load(&source) {
        Ok(s) => s,
        Err(e) => {
            eprintln!("rosasm: {}: {e}", source.display());
            std::process::exit(1);
        }
    };

    // `GET` resolves against the assembler's working directory, which the
    // makefiles set to the component root. RISC_OSLib is built with
    // `objasm -from clib.s.cl_stub`, so that file's `GET h_regs.s` finds
    // `RISC_OSLib/s/h_regs`, two levels up, not one. Both depths are
    // searched, nearest first.
    let own = source.parent().map(Path::to_path_buf).unwrap_or_default();
    let comp = own.parent().map(Path::to_path_buf).unwrap_or_default();
    let above = comp.parent().map(Path::to_path_buf);
    let mut search = dirs.clone();
    search.push(comp.clone());
    search.extend(above.clone());
    // The component's own hdr first, as the build's Hdr$Path has it.
    let mut hdr = vec![comp.join("hdr")];
    hdr.extend(above.iter().map(|d| d.join("hdr")));
    hdr.extend(dirs.iter().cloned());
    let resolver = Dirs {
        dirs: search,
        vars: HashMap::from([("hdr".to_string(), hdr)]),
    };

    let mut ex = Expander::new(&resolver);
    ex.set_target_builtins();
    ex.set_fpa_to_vfp(fpa_to_vfp);
    ex.set_assert_warnings(warn_assertions);
    for pd in &pds {
        if let Err(e) = ex.predefine(pd) {
            eprintln!("rosasm: bad -PD {pd:?}: {e}");
            std::process::exit(1);
        }
    }
    let name = source.file_name().unwrap_or_default().to_string_lossy().to_string();
    let lines = match ex.run(&name, sf.lines) {
        Ok(l) => l,
        Err(e) => {
            eprintln!("{e}");
            std::process::exit(1);
        }
    };

    // Where every line ended up. This is enough to find where the layout
    // disagrees with ObjAsm's listing, without a listing of our own. It is
    // how a failure of the Kernel's `ASSERT {PC}-SVCDespatcher =
    // SWIDespatch_Size` is tracked down: the first address that differs is
    // the line that caused it.
    if let Some(path) = &map {
        let mut out = String::from("; index  address  area  file:line  source\n");
        for (i, l) in lines.iter().enumerate() {
            if l.listing_only {
                continue;
            }
            out.push_str(&format!(
                "{i:<6} {:08X} {:>3}  {}:{}  {}\n",
                l.addr,
                l.area_index,
                l.origin.file,
                l.origin.line,
                l.text.trim_end()
            ));
        }
        if let Err(e) = std::fs::write(path, out) {
            eprintln!("rosasm: {}: {e}", path.display());
        }
    }

    // Encode the instructions.
    // Everything the object cannot honestly contain, collected rather
    // than printed and forgotten.
    let mut refused: Vec<Unencodable> = Vec::new();
    let (ual, index, adr_relocs) =
        to_ual(&lines, &ex, &mut refused, allow_unencodable, fpa_to_vfp);
    // A pid alone is not a unique name. One process may encode more than
    // once, and the system reuses pids. The clock's nanoseconds separate
    // them, so a farm of parallel jobs cannot read each other's lowered
    // assembly.
    let unique = std::time::SystemTime::now()
        .duration_since(std::time::UNIX_EPOCH)
        .map_or(0, |d| d.subsec_nanos());
    let tmp =
        std::env::temp_dir().join(format!("rosasm-{}-{unique:08x}", std::process::id()));
    let asm_path = tmp.with_extension("s");
    let obj_path = tmp.with_extension("o");
    if let Err(e) = std::fs::write(&asm_path, &ual) {
        eprintln!("rosasm: {e}");
        std::process::exit(1);
    }
    let run = Command::new(&clang)
        .args(TARGET)
        .arg("-c")
        .arg(&asm_path)
        .arg("-o")
        .arg(&obj_path)
        .output();
    match run {
        Ok(out) if out.status.success() => {
            // Warnings still say something worth hearing, and they name the
            // lowered file too.
            report_encoder(&String::from_utf8_lossy(&out.stderr), &ual, &lines);
        }
        Ok(out) => {
            report_encoder(&String::from_utf8_lossy(&out.stderr), &ual, &lines);
            eprintln!("rosasm: the encoder rejected the lowered assembly");
            eprintln!("        kept at {}", asm_path.display());
            std::process::exit(1);
        }
        Err(e) => {
            eprintln!("rosasm: cannot run the encoder at {clang}: {e}");
            eprintln!("        name one with --clang, or set ROSASM_CLANG");
            std::process::exit(1);
        }
    }

    let elf_bytes = std::fs::read(&obj_path).unwrap_or_default();
    let elf = match elfread::parse(&elf_bytes) {
        Ok(o) => o,
        Err(e) => {
            eprintln!("rosasm: cannot read the encoder's output: {e}");
            std::process::exit(1);
        }
    };

    // Map each encoded instruction back to the line that produced it, so the
    // area is assembled in source order with the data already computed.
    let text = elf.section_named(".text").map(|s| s.data.clone()).unwrap_or_default();
    let mut at: HashMap<usize, u32> = HashMap::new();
    for s in &elf.symbols {
        if let Some(n) = s.name.strip_prefix("__ros") {
            if let Ok(n) = n.parse::<usize>() {
                at.insert(n, s.value);
            }
        }
    }

    // One AOF area per source AREA, in declaration order.
    let mut areas: Vec<aof::Area> = ex
        .areas()
        .iter()
        .zip(ex.area_sizes())
        .map(|((name, attrs), size)| {
            let mut a = aof::Area::new(name.clone(), aof::from_objasm_area(attrs));
            a.alignment = attrs.align as u8;
            // A NOINIT area emits nothing, so its size must be declared.
            a.reserved = *size;
            a
        })
        .collect();
    if areas.is_empty() {
        // A source with no AREA at all still has to go somewhere.
        areas.push(aof::Area::new(
            "C$$code",
            area_attr::CODE | area_attr::READ_ONLY | area_attr::APCS_32,
        ));
    }

    // Copy each line's bytes into its area, remembering where the encoder's
    // output landed so its fixups can be found again.
    let mut segments: Vec<Segment> = Vec::new();
    // Where each data line's bytes went: (area, offset, line), so the
    // compiler can read a table's expressions (`& handler - table`).
    let mut data_lines: Vec<(usize, u32, usize)> = Vec::new();
    // Where each area changes between code and data, for the mapping symbols
    // ObjAsm emits: `$a` where ARM instructions start, `$d` where data does.
    // A disassembler cannot tell them apart without these, and the linker
    // uses them to decide what it may not reorder.
    let mut mapping: Vec<(usize, u32, char)> = Vec::new();
    let note = |area: usize, at: u32, kind: char, m: &mut Vec<(usize, u32, char)>| {
        if m.iter().rev().find(|(a, _, _)| *a == area).map(|(_, _, k)| *k) != Some(kind) {
            m.push((area, at, kind));
        }
    };
    for (i, l) in lines.iter().enumerate() {
        if l.listing_only {
            continue;
        }
        let Some(area) = areas.get_mut(l.area_index) else { continue };
        // A zero-initialised area carries no bytes in the file. Its size
        // comes from the header alone, so anything emitted into one is
        // dropped here.
        let zero_init = area.attributes & area_attr::ZERO_INIT != 0;
        if !l.bytes.is_empty() {
            if !zero_init {
                note(l.area_index, area.data.len() as u32, 'd', &mut mapping);
                data_lines.push((l.area_index, area.data.len() as u32, i));
                area.data.extend_from_slice(&l.bytes);
            }
            continue;
        }
        // The number of bytes this line produced is the distance to the next
        // labelled line. That is how an ADRL expansion contributes its eight
        // bytes without the copy needing to know about it.
        if let Some(off) = at.get(&i) {
            let from = *off as usize;
            let to = at
                .values()
                .map(|v| *v as usize)
                .filter(|v| *v > from)
                .min()
                .unwrap_or(text.len());
            if !zero_init && to <= text.len() && from < to {
                note(l.area_index, area.data.len() as u32, 'a', &mut mapping);
                segments.push(Segment {
                    text: from..to,
                    area: l.area_index,
                    dest: area.data.len() as u32,
                    line: i,
                });
                area.data.extend_from_slice(&text[from..to]);
            }
        }
    }
    // Anything that moved the location counter without emitting bytes leaves
    // a hole. `SPACE` is the main case; it is how SDFS reserves its stack.
    // An initialised area must carry every byte it declares, so the holes
    // are filled here, and the declared size becomes the data itself.
    for (a, size) in areas.iter_mut().zip(ex.area_sizes()) {
        if a.attributes & area_attr::ZERO_INIT != 0 {
            continue;
        }
        if a.data.len() > *size as usize {
            eprintln!(
                "rosasm: area {} holds {} bytes but its location counter reached {size}",
                a.name,
                a.data.len()
            );
        }
        a.data.resize((*size as usize).max(a.data.len()), 0);
        a.reserved = 0;
    }
    // The spec requires each area's length to be a multiple of four.
    for a in &mut areas {
        while a.data.len() % 4 != 0 {
            a.data.push(0);
        }
    }
    let _ = index;

    // Exported labels are defined at the address the second pass gave them.
    // Anything exported without a definition here, and every import,
    // becomes an external reference for the linker to satisfy.
    let defs = ex.label_defs();
    let mut symbols: Vec<aof::Symbol> = Vec::new();
    // The same relocations in ELF's terms, collected beside the AOF ones as
    // each is worked out. Only `--elf` uses them.
    let mut elf_rels: Vec<elfwrite::Rel> = Vec::new();
    // In the spec's words, bit 8 "denotes that the symbol identifies a
    // (usually read-only) datum, rather than an executable instruction". It
    // has meaning only inside a code area. ObjAsm decides it by what it was
    // doing where the label was written.
    let kinds = ex.label_kinds();
    let datum = |name: &String, ai: usize| {
        let code = areas
            .get(ai)
            .is_some_and(|a| a.attributes & area_attr::CODE != 0);
        if code && kinds.get(name).copied().unwrap_or(false) {
            sym_attr::CODE_DATUM
        } else {
            0
        }
    };
    for name in ex.exports() {
        match defs.get(name) {
            Some((ai, off)) => symbols.push(aof::Symbol {
                name: name.clone(),
                attributes: sym_attr::DEFINED | sym_attr::GLOBAL | datum(name, *ai),
                value: *off,
                area: areas.get(*ai).map(|a| a.name.clone()),
            }),
            None => {
                eprintln!("rosasm: {name} is exported but not defined here");
                symbols.push(external(name));
            }
        }
    }
    for name in ex.imports() {
        if !symbols.iter().any(|s| s.name == *name) {
            symbols.push(external(name));
        }
    }
    // Every area carries its own name as a local symbol, and the points where
    // it changes between code and data carry `$a` and `$d`. They are local
    // because they describe the object, and offer the linker nothing to
    // resolve. ObjAsm emits both, and a disassembler expects them.
    for a in &areas {
        symbols.push(aof::Symbol {
            name: a.name.clone(),
            attributes: sym_attr::DEFINED,
            value: 0,
            area: Some(a.name.clone()),
        });
    }
    for (area, at, kind) in &mapping {
        let Some(a) = areas.get(*area) else { continue };
        symbols.push(aof::Symbol {
            name: format!("${kind}"),
            // `$d` says the bytes after it are a datum, which is exactly what
            // the <code datum> attribute records.
            attributes: sym_attr::DEFINED
                | if *kind == 'd' { sym_attr::CODE_DATUM } else { 0 },
            value: *at,
            area: Some(a.name.clone()),
        });
    }

    // Relocations. The encoder reports every field it could not fix. Each
    // one becomes either an addend we can work out here, or a directive for
    // the linker.
    let text_section = elf
        .sections
        .iter()
        .position(|s| s.name == ".text")
        .unwrap_or(0) as u32;
    for r in &elf.rels {
        if r.section != text_section {
            continue;
        }
        let Some(seg) = segments
            .iter()
            .find(|s| s.text.contains(&(r.offset as usize)))
        else {
            continue;
        };
        let name = elf
            .symbols
            .get(r.sym as usize)
            .map(|s| s.name.clone())
            .unwrap_or_default();
        // Only a symbol the encoder could not resolve becomes a relocation.
        // A `b .+20` is resolved where it stands, yet LLVM still records a
        // fixup against a temporary symbol of its own, such as `.L0`. That
        // fixup is already accounted for in the bytes. Applying it again
        // corrupts the branch, and puts a symbol in the object that names
        // nothing.
        let defined = elf
            .symbols
            .get(r.sym as usize)
            .is_some_and(|s| s.is_defined());
        if defined || name.starts_with("__ros") || name.starts_with(".L") {
            continue;
        }
        let here = seg.dest + (r.offset - seg.text.start as u32);
        let insn = word_at(&areas[seg.area].data, here);
        // Two field shapes reach us: a branch's 24-bit word offset and a data
        // transfer's 12-bit byte offset. Both are measured from `pc`.
        let (addend, encode): (i32, AddendSetter) = if reloc::is_branch(r.kind) {
            (reloc::branch_addend(insn), reloc::set_branch_addend)
        } else if reloc::is_ldr_literal(r.kind) {
            (reloc::ldr_addend(insn), reloc::set_ldr_addend)
        } else {
            refused.push(Unencodable {
                where_: lines.get(seg.line).map_or_else(
                    || format!("&{here:X}"),
                    |l| format!("{}:{}", l.origin.file, l.origin.line),
                ),
                what: lines.get(seg.line).map(|l| l.text.trim().to_string()).unwrap_or_default(),
                why: format!("relocation type {} against {name} is not handled", r.kind),
            });
            continue;
        };
        // There are three cases. A target in this same area needs no
        // directive at all. One in another of our areas is relocated by that
        // area's base. Anything else is relocated by the symbol's value.
        //
        // Each case has two forms. AOF measures its addend from the area
        // base, which is what `pc_relative_addend` works out. ELF keeps the
        // addend the encoder already put in the field, and adds only the
        // target's offset within whatever it names. That is nothing for a
        // symbol, since the symbol is the target, and the offset for a
        // section. So the ELF form is not recovered from the AOF one. Both
        // are worked out here from the encoder's addend, and the output
        // format picks one.
        let (new_addend, by, elf_rel) = match defs.get(&name) {
            Some((ai, off)) if *ai == seg.area => {
                (reloc::local_pc_addend(here, *off), None, None)
            }
            Some((ai, off)) => (
                reloc::pc_relative_addend(addend, here, *off),
                Some(aof::RelocBy::Area(*ai as u32)),
                Some((elfwrite::Target::Section(*ai), addend + *off as i32)),
            ),
            None => {
                let idx = symbol_index(&mut symbols, &name);
                (
                    reloc::pc_relative_addend(addend, here, 0),
                    Some(aof::RelocBy::Symbol(idx)),
                    Some((elfwrite::Target::Symbol(idx), addend)),
                )
            }
        };
        let in_place = match (elf_out, &elf_rel) {
            (true, Some((_, a))) => *a,
            _ => new_addend,
        };
        match encode(insn, in_place) {
            Some(w) => set_word_at(&mut areas[seg.area].data, here, w),
            None => {
                refused.push(Unencodable {
                    where_: lines.get(seg.line).map_or_else(
                        || format!("&{here:X}"),
                        |l| format!("{}:{}", l.origin.file, l.origin.line),
                    ),
                    what: lines.get(seg.line).map(|l| l.text.trim().to_string()).unwrap_or_default(),
                    why: format!("{name} is out of reach from here"),
                });
                continue;
            }
        }
        if let Some((target, _)) = elf_rel {
            // The encoder's own relocation type is passed on unchanged. A
            // branch is R_ARM_CALL or R_ARM_JUMP24 because clang said so.
            elf_rels.push(elfwrite::Rel {
                area: seg.area,
                offset: here,
                target,
                kind: r.kind,
            });
        }
        if let Some(by) = by {
            areas[seg.area].relocs.push(aof::Reloc {
                offset: here,
                by,
                field: aof::FieldType::Instruction,
                pc_relative: true,
                based: false,
                // How many instructions the linker may rewrite. One is
                // written as none. ObjAsm's objects hold a hundred and
                // eighty-six instruction relocations with the field clear,
                // and nine `ADRL`s with it at two. None has it at one.
                max_instructions: 0,
            });
        }
    }

    // `ADR` at an imported symbol. The instructions are arithmetic on `pc`,
    // with the symbol taken to stand at zero. So the encoder had nothing to
    // record, and the relocation is added here, against the first of them.
    for a in &adr_relocs {
        let Some(seg) = segments.iter().find(|s| s.line == a.line) else {
            continue;
        };
        if elf_out {
            // AOF lets the linker rewrite a run of instructions, which is how
            // an `ADRL` at an imported symbol is resolved. ELF spells that as
            // the R_ARM_ALU_PC_G* group, and roscc implements none of it. An
            // object written here would link wrongly with no warning, so
            // report it.
            refused.push(Unencodable {
                where_: lines.get(a.line).map_or_else(
                    || format!("&{:X}", seg.dest),
                    |l| format!("{}:{}", l.origin.file, l.origin.line),
                ),
                what: lines
                    .get(a.line)
                    .map(|l| l.text.trim().to_string())
                    .unwrap_or_default(),
                why: format!(
                    "ADR at the imported symbol {} has no ELF relocation \
                     this writes (R_ARM_ALU_PC_G*); load the address instead",
                    a.name
                ),
            });
            continue;
        }
        let idx = symbol_index(&mut symbols, &a.name);
        let Some(area) = areas.get_mut(seg.area) else { continue };
        area.relocs.push(aof::Reloc {
            offset: seg.dest,
            by: aof::RelocBy::Symbol(idx),
            field: aof::FieldType::Instruction,
            pc_relative: true,
            based: false,
            // As above: a single instruction is written as none.
            max_instructions: if a.instructions > 1 { a.instructions } else { 0 },
        });
    }

    // Data fields the expander could not finish.
    for f in ex.data_fixups() {
        let Some(field) = field_type(f.width) else {
            eprintln!(
                "rosasm: cannot relocate a {}-byte field ({})",
                f.width, f.expr
            );
            continue;
        };
        let by = match &f.kind {
            expand::FixupKind::AreaBase(a) => aof::RelocBy::Area(*a as u32),
            expand::FixupKind::External(name) => {
                aof::RelocBy::Symbol(symbol_index(&mut symbols, name))
            }
        };
        if elf_out {
            // A whole word holding an address is R_ARM_ABS32. Its addend is
            // the word itself, which already holds it. Narrower fields have
            // R_ARM_ABS8 and ABS16, but roscc applies neither.
            if field == aof::FieldType::Word {
                let target = match &by {
                    aof::RelocBy::Area(a) => elfwrite::Target::Section(*a as usize),
                    aof::RelocBy::Symbol(i) => elfwrite::Target::Symbol(*i),
                };
                elf_rels.push(elfwrite::Rel {
                    area: f.area,
                    offset: f.offset,
                    target,
                    kind: reloc::elf_type::R_ARM_ABS32,
                });
            } else {
                eprintln!(
                    "rosasm: a {}-byte relocated field ({}) has no ELF \
                     relocation this writes",
                    f.width, f.expr
                );
            }
        }
        let Some(area) = areas.get_mut(f.area) else { continue };
        area.relocs.push(aof::Reloc {
            offset: f.offset,
            by,
            field,
            // Plain additive: the value is an address, not a distance.
            pc_relative: false,
            based: false,
            max_instructions: 0,
        });
    }

    // An object with instructions missing from it is not one a ROM can be
    // built with. By default we say so and write nothing. The alternative
    // must be asked for by name, and traps at run time instead.
    if !refused.is_empty() {
        for (n, r) in refused.iter().enumerate() {
            let index = if allow_unencodable {
                format!(" [UDF #{n}]")
            } else {
                String::new()
            };
            eprintln!("rosasm: {}: {}{index}", r.where_, r.why);
            eprintln!("        {}", r.what);
        }
        let n = refused.len();
        let s = if n == 1 { "" } else { "s" };
        if allow_unencodable {
            eprintln!("rosasm: {n} instruction{s} will trap if reached");
        } else {
            eprintln!("rosasm: {n} instruction{s} could not be encoded");
            eprintln!("        no object written");
            eprintln!("        --allow-unencodable writes one anyway, trapping at each");
            std::process::exit(1);
        }
    }

    if emit_c {
        let code: std::collections::HashSet<usize> = segments.iter().map(|s| s.line).collect();
        let listed: Vec<emitc::Listed> = lines
            .iter()
            .enumerate()
            .map(|(i, l)| emitc::Listed {
                text: &l.text,
                file: &l.origin.file,
                top: l.origin.macros.is_empty(),
                code: code.contains(&i),
                // What goes into the area: a directive's listed bytes do not.
                data: !l.listing_only && !l.bytes.is_empty(),
            })
            .collect();
        let mut notes = emitc::notes_before(&listed);
        let mut sources = HashMap::new();
        for seg in &segments {
            let Some(l) = lines.get(seg.line) else { continue };
            for k in (0..seg.text.len() as u32).step_by(4) {
                sources.insert(
                    (seg.area, seg.dest + k),
                    emitc::Source {
                        file: l.origin.file.clone(),
                        line: l.origin.line,
                        text: l.text.clone(),
                        notes: if k == 0 { notes.remove(&seg.line).unwrap_or_default() } else { vec![] },
                    },
                );
            }
        }
        // Record data words too, by their line, where there is no
        // instruction. A table's expressions name what it holds.
        for &(area, dest, i) in &data_lines {
            let Some(l) = lines.get(i) else { continue };
            for k in (0..l.bytes.len() as u32).step_by(4) {
                sources.entry((area, dest + k)).or_insert_with(|| emitc::Source {
                    file: l.origin.file.clone(),
                    line: l.origin.line,
                    text: l.text.clone(),
                    notes: vec![],
                });
            }
        }
        let mut native_swis = HashMap::new();
        if let Some(path) = &swis {
            let text = std::fs::read_to_string(path).unwrap_or_else(|e| {
                eprintln!("rosasm: {}: {e}", path.display());
                std::process::exit(1);
            });
            for l in text.lines() {
                let mut f = l.split_whitespace();
                if let (Some(n), Some(name)) = (f.next(), f.next()) {
                    let n = n.trim_start_matches("0x");
                    if let Ok(n) = u32::from_str_radix(n, 16) {
                        native_swis.insert(n, name.to_string());
                    }
                }
            }
        }
        let mut swi_io = HashMap::new();
        if let Some(path) = &swi_regs {
            let text = std::fs::read_to_string(path).unwrap_or_else(|e| {
                eprintln!("rosasm: {}: {e}", path.display());
                std::process::exit(1);
            });
            let hex = |s: Option<&str>, key: &str| {
                s.and_then(|s| s.strip_prefix(key)).and_then(|s| u32::from_str_radix(s.trim_start_matches("0x"), 16).ok())
            };
            for l in text.lines() {
                let mut f = l.split_whitespace();
                let n = hex(f.next(), "");
                let (i, o) = (hex(f.next(), "in="), hex(f.next(), "out="));
                if let (Some(n), Some(i), Some(o)) = (n, i, o) {
                    swi_io.insert(n, (i as u16, o as u16));
                }
            }
        }
        let stem = source.file_stem().unwrap_or_default().to_string_lossy().to_string();
        let input = emitc::Input {
            // A C identifier: `!!version` names a unit too.
            name: {
                let n: String =
                    stem.to_ascii_lowercase().chars().map(|c| if c.is_ascii_alphanumeric() { c } else { '_' }).collect();
                // `000` names a unit too: C names cannot start with a digit.
                if n.starts_with(|c: char| c.is_ascii_digit()) { format!("u{n}") } else { n }
            },
            base: rom_base.unwrap_or(0),
            areas: &areas,
            mapping: &mapping,
            labels: ex.label_defs(),
            exports: ex.exports(),
            sources,
            native_swis,
            swi_regs: swi_io,
            module: areas.first().is_some_and(|a| {
                a.name == "!!!Module$$Header" || emitc::looks_like_module_header(a)
            }),
            apcs,
            lift,
            poll_loops,
            header: out.with_extension("h").file_name().map_or_else(String::new, |f| f.to_string_lossy().to_string()),
            constants: ex.symbols().absolutes().clone(),
            maps: ex
                .storage_maps()
                .iter()
                .map(|m| emitc::Map {
                    file: m.file.clone(),
                    line: m.line,
                    start: m.start,
                    base: m.base,
                    fields: m.fields.clone(),
                })
                .collect(),
        };
        match emitc::emit(&input) {
            Ok(o) => {
                let h = out.with_extension("h");
                if let Err(e) = std::fs::write(&out, &o.c).and_then(|_| std::fs::write(&h, &o.h)) {
                    eprintln!("rosasm: {e}");
                    std::process::exit(1);
                }
                println!(
                    "{}: {} regions, {} instructions compiled; {} beside it",
                    out.display(),
                    o.regions,
                    o.instructions,
                    h.display()
                );
            }
            Err(errors) => {
                for e in &errors {
                    eprintln!("rosasm: {e}");
                }
                eprintln!("rosasm: {} thing(s) could not be compiled exactly; no C written", errors.len());
                std::process::exit(1);
            }
        }
        if !keep {
            let _ = std::fs::remove_file(&asm_path);
            let _ = std::fs::remove_file(&obj_path);
        }
        return;
    }

    let obj = aof::Object {
        areas,
        symbols,
        entry: None,
        identification: format!("rosasm {}", env!("CARGO_PKG_VERSION")),
    };
    // One object, two containers. AOF is what the RISC OS linker reads. ELF
    // is what roscc reads, and roscc is how a module is built without the
    // DDE.
    let bytes = if elf_out {
        elfwrite::write(&obj.areas, &obj.symbols, &elf_rels)
    } else {
        obj.write()
    };
    if let Err(e) = std::fs::write(&out, bytes) {
        eprintln!("rosasm: {}: {e}", out.display());
        std::process::exit(1);
    }
    if !keep {
        let _ = std::fs::remove_file(&asm_path);
        let _ = std::fs::remove_file(&obj_path);
    }
    let bytes: usize = obj.areas.iter().map(|a| a.data.len()).sum();
    let n = obj.areas.len();
    println!(
        "{}: {bytes} bytes in {n} area{}, {} symbol{}",
        out.display(),
        if n == 1 { "" } else { "s" },
        obj.symbols.len(),
        if obj.symbols.len() == 1 { "" } else { "s" }
    );
}
