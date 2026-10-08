//! ARM Object Format — the object files ObjAsm and the DDE linker exchange.
//!
//! Specification: `docs/DDE/CodeStds/AOF`, version 3.10.
//!
//! An AOF file is not a format in its own right so much as five chunks inside
//! a **chunk file**, a small container that lists each chunk's name, offset
//! and size in a fixed header. The five are:
//!
//! | Chunk      | Holds                                              |
//! |------------|----------------------------------------------------|
//! | `OBJ_HEAD` | file type, version, and one 5-word header per area |
//! | `OBJ_AREA` | each area's bytes, each followed by its relocations |
//! | `OBJ_IDFN` | a string naming the tool that produced the file     |
//! | `OBJ_SYMT` | four words per symbol                              |
//! | `OBJ_STRT` | every name, as NUL-terminated strings              |
//!
//! Only `OBJ_HEAD` and `OBJ_AREA` are required, and chunks may appear in any
//! order. The spec notes that the C compiler and the assembler emit them in
//! different orders. Names never appear inline. Each is an offset into the
//! string table, which keeps the entries in the other chunks fixed-width.

/// Marks a chunk file. Reading it byte-reversed means the file is the other
/// endianness, which is how a reader detects that without a flag.
pub const CHUNK_FILE_ID: u32 = 0xC3CB_C6C5;
/// Marks the object as relocatable, the usual output of an assembler.
pub const OBJECT_FILE_TYPE: u32 = 0xC5E2_D080;
/// Version 3.10, encoded as decimal 310.
pub const AOF_VERSION: u32 = 310;
/// The number of chunk entries in the header. The spec calls eight
/// conventional for a producer that writes all five chunks. It leaves room
/// for a tool to add its own chunks without rewriting the file.
const MAX_CHUNKS: u32 = 8;

/// Translate an ObjAsm `AREA` directive's attributes into AOF's bits.
///
/// The two sets are close but not identical. ObjAsm's `READONLY` and AOF's
/// read-only bit agree. But AOF keeps "not initialised" separate from "read
/// only", and forbids the two together. AOF also carries APCS variant bits,
/// which ObjAsm sets through the assembler's own options rather than the
/// directive's attribute list.
pub fn from_objasm_area(a: &crate::layout::AreaAttrs) -> u32 {
    let mut v = 0;
    if a.code {
        // Everything we assemble is 32-bit APCS. The corpus has no 26-bit
        // path for this target.
        v |= area_attr::CODE | area_attr::APCS_32;
    }
    if a.readonly {
        v |= area_attr::READ_ONLY;
    }
    if a.noinit {
        // AOF forbids read-only and zero-initialised together, and the
        // directive's NOINIT is the zero-initialised one.
        v = (v & !area_attr::READ_ONLY) | area_attr::ZERO_INIT;
    }
    if a.abs {
        v |= area_attr::ABSOLUTE;
    }
    if a.pic {
        v |= area_attr::POSITION_INDEPENDENT;
    }
    if a.common {
        v |= area_attr::COMMON_REF;
    }
    if a.comdef {
        v |= area_attr::COMMON_DEF;
    }
    if a.reentrant {
        v |= area_attr::REENTRANT;
    }
    if a.interwork {
        v |= area_attr::INTERWORKING;
    }
    if a.based.is_some() {
        v |= area_attr::BASED;
    }
    v
}

// ---------------------------------------------------------------- areas

/// Area attribute bits, from the specification's summary table. The low eight
/// bits of the same word hold alignment as a power of two.
pub mod area_attr {
    pub const ABSOLUTE: u32 = 0x0000_0100;
    pub const CODE: u32 = 0x0000_0200;
    pub const COMMON_DEF: u32 = 0x0000_0400;
    pub const COMMON_REF: u32 = 0x0000_0800;
    /// No initialising bytes in `OBJ_AREA`; incompatible with `READ_ONLY`.
    pub const ZERO_INIT: u32 = 0x0000_1000;
    pub const READ_ONLY: u32 = 0x0000_2000;
    pub const POSITION_INDEPENDENT: u32 = 0x0000_4000;
    pub const DEBUG_TABLES: u32 = 0x0000_8000;
    // Code areas only.
    pub const APCS_32: u32 = 0x0001_0000;
    pub const REENTRANT: u32 = 0x0002_0000;
    pub const EXTENDED_FP: u32 = 0x0004_0000;
    pub const NO_STACK_CHECK: u32 = 0x0008_0000;
    pub const THUMB: u32 = 0x0010_0000;
    pub const HALFWORD_INSTRS: u32 = 0x0020_0000;
    pub const INTERWORKING: u32 = 0x0040_0000;
    // Data areas only.
    pub const BASED: u32 = 0x0010_0000;
    pub const SHARED_LIB_STUB: u32 = 0x0020_0000;
}

/// Symbol attribute bits.
pub mod sym_attr {
    /// Defined in this object file.
    pub const DEFINED: u32 = 0x01;
    /// Visible to the linker outside this file. With `DEFINED` clear this is
    /// an external reference. The spec reserves the case where both are
    /// clear.
    pub const GLOBAL: u32 = 0x02;
    pub const ABSOLUTE: u32 = 0x04;
    pub const CASE_INSENSITIVE: u32 = 0x08;
    pub const WEAK: u32 = 0x10;
    pub const STRONG: u32 = 0x20;
    pub const COMMON: u32 = 0x40;
    /// The symbol marks a datum rather than an instruction. The spec calls it
    /// meaningful only inside a code area. ObjAsm sets it on every `$d`.
    pub const CODE_DATUM: u32 = 0x100;
}

/// What a relocation directive modifies.
#[derive(Debug, Clone, Copy, PartialEq)]
pub enum FieldType {
    Byte = 0,
    HalfWord = 1,
    Word = 2,
    /// An instruction or instruction sequence. A `B`/`BL` is always valid here.
    Instruction = 3,
}

/// What the subject field is relocated by.
#[derive(Debug, Clone, Copy, PartialEq)]
pub enum RelocBy {
    /// The base of the area with this 0-origin index (the `A` bit clear).
    Area(u32),
    /// The value of the symbol with this 0-origin index (the `A` bit set).
    Symbol(u32),
}

#[derive(Debug, Clone)]
pub struct Reloc {
    /// Byte offset of the subject field within the preceding area.
    pub offset: u32,
    pub by: RelocBy,
    pub field: FieldType,
    /// The `R` bit: relocate by the difference from the subject's own address.
    pub pc_relative: bool,
    /// The `B` bit: relocate relative to the area's base register.
    pub based: bool,
    /// The `II` field: how many instructions the linker may rewrite, 0 for
    /// "as many as needed". Must be zero unless `field` is `Instruction`.
    pub max_instructions: u8,
}

impl Reloc {
    /// The flag word as it is written, which is also its identity.
    pub fn flags(&self) -> u32 {
        let (a_bit, sid) = match self.by {
            RelocBy::Area(i) => (0, i),
            RelocBy::Symbol(i) => (1 << 27, i),
        };
        let ii = if self.field == FieldType::Instruction {
            (self.max_instructions as u32 & 0x3) << 29
        } else {
            0
        };
        // Bit 31 shall be 1.
        (1 << 31)
            | ii
            | ((self.based as u32) << 28)
            | a_bit
            | ((self.pc_relative as u32) << 26)
            | ((self.field as u32) << 24)
            | (sid & 0x00FF_FFFF)
    }
}

#[derive(Debug, Clone)]
pub struct Area {
    pub name: String,
    /// Attribute bits from `area_attr`, without the alignment.
    pub attributes: u32,
    /// Alignment as a power of two. The spec allows 2..=32, and 2 (a word)
    /// is the usual choice.
    pub alignment: u8,
    /// Contents. Empty when `ZERO_INIT` is set, which is how the area avoids
    /// carrying a run of zeroes in the file.
    pub data: Vec<u8>,
    /// Bytes the linker must reserve beyond `data`. A `ZERO_INIT` area has no
    /// contents at all, so without this its declared size would be zero and
    /// the linker would reserve nothing.
    pub reserved: u32,
    pub relocs: Vec<Reloc>,
    /// Only meaningful with `ABSOLUTE`.
    pub base: u32,
}

impl Area {
    pub fn new(name: impl Into<String>, attributes: u32) -> Self {
        Area {
            name: name.into(),
            attributes,
            alignment: 2,
            data: Vec::new(),
            reserved: 0,
            relocs: Vec::new(),
            base: 0,
        }
    }

    /// Area size must be a multiple of four.
    fn padded_len(&self) -> u32 {
        let n = (self.data.len() as u32).max(self.reserved);
        (n + 3) & !3
    }
}

#[derive(Debug, Clone)]
pub struct Symbol {
    pub name: String,
    pub attributes: u32,
    /// The symbol's value if absolute, a common block's length if common,
    /// otherwise an offset from the base of `area`.
    pub value: u32,
    /// The area this symbol is defined in. It is required where a
    /// non-absolute symbol is defined, and ignored otherwise.
    pub area: Option<String>,
}

#[derive(Debug, Default)]
pub struct Object {
    pub areas: Vec<Area>,
    pub symbols: Vec<Symbol>,
    /// `(1-origin area index, byte offset)`. The spec uses index 0 to mean
    /// "this file defines no entry point".
    pub entry: Option<(u32, u32)>,
    pub identification: String,
}

// ------------------------------------------------------------ string table

/// Builds `OBJ_STRT`. The first word is the table's own length including that
/// word, so no valid offset is below 4.
#[derive(Default)]
struct StringTable {
    bytes: Vec<u8>,
    seen: std::collections::HashMap<String, u32>,
}

impl StringTable {
    fn new() -> Self {
        StringTable {
            bytes: vec![0, 0, 0, 0],
            seen: std::collections::HashMap::new(),
        }
    }

    fn add(&mut self, s: &str) -> u32 {
        if let Some(off) = self.seen.get(s) {
            return *off;
        }
        let off = self.bytes.len() as u32;
        self.bytes.extend_from_slice(s.as_bytes());
        self.bytes.push(0);
        self.seen.insert(s.to_string(), off);
        off
    }

    fn finish(mut self) -> Vec<u8> {
        let len = self.bytes.len() as u32;
        self.bytes[..4].copy_from_slice(&len.to_le_bytes());
        self.bytes
    }
}

// ----------------------------------------------------------------- writing

fn push_u32(v: &mut Vec<u8>, x: u32) {
    v.extend_from_slice(&x.to_le_bytes());
}

fn pad_to_word(v: &mut Vec<u8>) {
    while !v.len().is_multiple_of(4) {
        v.push(0);
    }
}

impl Object {
    /// Serialise to a complete AOF file.
    pub fn write(&self) -> Vec<u8> {
        let mut strt = StringTable::new();
        // Names must be interned before the header is built, since the header
        // stores offsets rather than the names themselves.
        let area_names: Vec<u32> = self.areas.iter().map(|a| strt.add(&a.name)).collect();
        let sym_names: Vec<u32> = self.symbols.iter().map(|s| strt.add(&s.name)).collect();
        let sym_areas: Vec<u32> = self
            .symbols
            .iter()
            .map(|s| match &s.area {
                Some(n) => strt.add(n),
                None => 0,
            })
            .collect();

        // OBJ_HEAD
        let mut head = Vec::new();
        push_u32(&mut head, OBJECT_FILE_TYPE);
        push_u32(&mut head, AOF_VERSION);
        push_u32(&mut head, self.areas.len() as u32);
        push_u32(&mut head, self.symbols.len() as u32);
        let (entry_area, entry_offset) = self.entry.unwrap_or((0, 0));
        push_u32(&mut head, entry_area);
        push_u32(&mut head, entry_offset);
        for (a, name_off) in self.areas.iter().zip(&area_names) {
            push_u32(&mut head, *name_off);
            push_u32(&mut head, a.attributes | (a.alignment as u32));
            push_u32(&mut head, a.padded_len());
            push_u32(&mut head, a.relocs.len() as u32);
            push_u32(&mut head, a.base);
        }

        // OBJ_AREA: each area's bytes, then its relocations, both word-aligned.
        let mut areas = Vec::new();
        for a in &self.areas {
            if a.attributes & area_attr::ZERO_INIT == 0 {
                areas.extend_from_slice(&a.data);
                pad_to_word(&mut areas);
            }
            for r in &a.relocs {
                push_u32(&mut areas, r.offset);
                push_u32(&mut areas, r.flags());
            }
        }

        // OBJ_SYMT
        let mut symt = Vec::new();
        for (i, s) in self.symbols.iter().enumerate() {
            push_u32(&mut symt, sym_names[i]);
            push_u32(&mut symt, s.attributes);
            push_u32(&mut symt, s.value);
            push_u32(&mut symt, sym_areas[i]);
        }

        let mut idfn = self.identification.clone().into_bytes();
        idfn.push(0);

        let strt = strt.finish();

        let chunks: Vec<(&str, Vec<u8>)> = vec![
            ("OBJ_HEAD", head),
            ("OBJ_AREA", areas),
            ("OBJ_IDFN", idfn),
            ("OBJ_SYMT", symt),
            ("OBJ_STRT", strt),
        ];

        // Header is three words plus four per entry, and every chunk starts on
        // a word boundary.
        let header_len = (3 + 4 * MAX_CHUNKS as usize) * 4;
        let mut out = Vec::with_capacity(header_len);
        push_u32(&mut out, CHUNK_FILE_ID);
        push_u32(&mut out, MAX_CHUNKS);
        push_u32(&mut out, chunks.len() as u32);

        let mut offset = header_len as u32;
        for (name, body) in &chunks {
            let mut id = [0u8; 8];
            id[..name.len()].copy_from_slice(name.as_bytes());
            out.extend_from_slice(&id);
            push_u32(&mut out, offset);
            push_u32(&mut out, body.len() as u32);
            offset += ((body.len() as u32) + 3) & !3;
        }
        // Unused entries: the spec marks these with a zero file offset.
        for _ in chunks.len()..MAX_CHUNKS as usize {
            out.extend_from_slice(&[0u8; 8]);
            push_u32(&mut out, 0);
            push_u32(&mut out, 0);
        }

        for (_, body) in &chunks {
            out.extend_from_slice(body);
            pad_to_word(&mut out);
        }
        out
    }
}

// ----------------------------------------------------------------- identity

impl Object {
    /// What a relocation refers to, by name rather than by position.
    pub fn name_of(&self, by: RelocBy) -> String {
        match by {
            RelocBy::Symbol(i) => self
                .symbols
                .get(i as usize)
                .map(|s| format!("symbol {}", s.name))
                .unwrap_or_else(|| format!("symbol #{i}")),
            RelocBy::Area(i) => self
                .areas
                .get(i as usize)
                .map(|a| format!("area {}", a.name))
                .unwrap_or_else(|| format!("area #{i}")),
        }
    }
}

/// A hash of everything about an object that a compiler is responsible for.
///
/// Two assemblers given the same source should produce the same bytes, the
/// same areas with the same attributes, the same symbols and the same
/// relocations. This folds exactly that into one number. So a corpus of a
/// thousand units can be checked against a reference with a thousand
/// comparisons rather than a thousand diffs. A diff is needed only where
/// the numbers disagree.
///
/// It deliberately leaves out everything a *producer* is responsible for:
/// the identification string naming the tool and its version, the order the
/// chunks were written in, and the padding between them. Those differ
/// between any two assemblers, and say nothing about whether the code is
/// the same.
///
/// Nor does it use a relocation's symbol index. An index is a position in
/// the file's own symbol table. So two objects that agree in every
/// particular still carry different numbers whenever their tables are in a
/// different order. And they are: ObjAsm lists its imports before the
/// mapping symbols, though nothing requires that. What is hashed instead is
/// the name the relocation refers to.
///
/// The hash is FNV-1a. It only has to detect a difference; it does not have
/// to resist anyone trying to manufacture a collision.
pub fn content_hash(o: &Object) -> u64 {
    let mut h: u64 = 0xcbf2_9ce4_8422_2325;
    let mut eat = |bytes: &[u8]| {
        for b in bytes {
            h ^= *b as u64;
            h = h.wrapping_mul(0x1000_0000_01b3);
        }
    };
    for a in &o.areas {
        eat(a.name.as_bytes());
        eat(&a.attributes.to_le_bytes());
        eat(&[a.alignment]);
        eat(&a.reserved.to_le_bytes());
        eat(&a.data);
        // Sorted, because two assemblers may record them in either order and
        // the linker does not care which.
        let mut rs: Vec<(u32, u32, String)> = a
            .relocs
            .iter()
            .map(|r| {
                // The flag word without the 24-bit index, which is a position
                // rather than a fact about the code.
                let shape = r.flags() & !0x00FF_FFFF;
                (r.offset, shape, o.name_of(r.by))
            })
            .collect();
        rs.sort_unstable();
        for (off, shape, name) in rs {
            eat(&off.to_le_bytes());
            eat(&shape.to_le_bytes());
            eat(name.as_bytes());
        }
    }
    let mut syms: Vec<(&str, u32, u32, &str)> = o
        .symbols
        .iter()
        .map(|s| {
            (
                s.name.as_str(),
                s.attributes,
                s.value,
                s.area.as_deref().unwrap_or(""),
            )
        })
        .collect();
    syms.sort_unstable();
    for (n, attr, v, area) in syms {
        eat(n.as_bytes());
        eat(&attr.to_le_bytes());
        eat(&v.to_le_bytes());
        eat(area.as_bytes());
    }
    h
}

// ----------------------------------------------------------------- reading

/// The chunks in a chunk file, each as its name and its bytes. Unused
/// entries, which have a zero offset, are skipped.
pub fn chunks(file: &[u8]) -> Result<Vec<(String, Vec<u8>)>, String> {
    if file.len() < 12 {
        return Err("too short for a chunk file header".into());
    }
    let word = |i: usize| -> u32 {
        u32::from_le_bytes([file[i], file[i + 1], file[i + 2], file[i + 3]])
    };
    if word(0) != CHUNK_FILE_ID {
        return Err(format!("not a chunk file: id {:08X}", word(0)));
    }
    let max = word(4) as usize;
    let mut out = Vec::new();
    for i in 0..max {
        let e = 12 + i * 16;
        if e + 16 > file.len() {
            break;
        }
        let off = word(e + 8) as usize;
        let size = word(e + 12) as usize;
        if off == 0 {
            continue;
        }
        let name = String::from_utf8_lossy(&file[e..e + 8])
            .trim_end_matches('\0')
            .to_string();
        if off + size > file.len() {
            return Err(format!("chunk {name} runs past the end of the file"));
        }
        out.push((name, file[off..off + size].to_vec()));
    }
    Ok(out)
}

/// Decode the flag word of a relocation directive.
impl Reloc {
    fn from_words(offset: u32, flags: u32) -> Reloc {
        let field = match (flags >> 24) & 3 {
            0 => FieldType::Byte,
            1 => FieldType::HalfWord,
            2 => FieldType::Word,
            _ => FieldType::Instruction,
        };
        let sid = flags & 0x00FF_FFFF;
        Reloc {
            offset,
            by: if flags & (1 << 27) != 0 {
                RelocBy::Symbol(sid)
            } else {
                RelocBy::Area(sid)
            },
            field,
            pc_relative: flags & (1 << 26) != 0,
            based: flags & (1 << 28) != 0,
            max_instructions: if field == FieldType::Instruction {
                ((flags >> 29) & 3) as u8
            } else {
                0
            },
        }
    }
}

/// Read a NUL-terminated name at `off` in the string table.
fn strt_name(strt: &[u8], off: u32) -> String {
    let off = off as usize;
    if off == 0 || off >= strt.len() {
        return String::new();
    }
    let end = strt[off..]
        .iter()
        .position(|b| *b == 0)
        .map(|n| off + n)
        .unwrap_or(strt.len());
    // Names are Latin-1 in the sources; the ones that matter are ASCII.
    strt[off..end].iter().map(|b| *b as char).collect()
}

/// Read an AOF file back into an `Object`.
///
/// Symmetrical with `write`, and the basis for checking what we produce
/// against what ObjAsm produces for the same source.
pub fn read(file: &[u8]) -> Result<Object, String> {
    let chunks = chunks(file)?;
    let get = |n: &str| chunks.iter().find(|(name, _)| name == n).map(|(_, b)| b);
    let head = get("OBJ_HEAD").ok_or("no OBJ_HEAD chunk")?;
    let strt = get("OBJ_STRT").cloned().unwrap_or_default();
    let empty = Vec::new();
    let area_bytes = get("OBJ_AREA").unwrap_or(&empty);
    let symt = get("OBJ_SYMT").unwrap_or(&empty);

    let w = |b: &[u8], i: usize| -> u32 {
        if i + 4 > b.len() {
            0
        } else {
            u32::from_le_bytes([b[i], b[i + 1], b[i + 2], b[i + 3]])
        }
    };
    if w(head, 0) != OBJECT_FILE_TYPE {
        return Err(format!("not a relocatable object: type {:08X}", w(head, 0)));
    }
    let n_areas = w(head, 8) as usize;
    let n_syms = w(head, 12) as usize;
    let entry_area = w(head, 16);
    let entry_offset = w(head, 20);

    let mut areas = Vec::with_capacity(n_areas);
    let mut pos = 0usize;
    for i in 0..n_areas {
        let e = 24 + i * 20;
        let name = strt_name(&strt, w(head, e));
        let attr_word = w(head, e + 4);
        let size = w(head, e + 8) as usize;
        let n_relocs = w(head, e + 12) as usize;
        let base = w(head, e + 16);
        let attributes = attr_word & !0xFF;
        let zero_init = attributes & area_attr::ZERO_INIT != 0;
        let mut a = Area {
            name,
            attributes,
            alignment: (attr_word & 0xFF) as u8,
            data: Vec::new(),
            reserved: if zero_init { size as u32 } else { 0 },
            relocs: Vec::with_capacity(n_relocs),
            base,
        };
        if !zero_init {
            if pos + size > area_bytes.len() {
                return Err(format!("area {} runs past OBJ_AREA", a.name));
            }
            a.data = area_bytes[pos..pos + size].to_vec();
            pos += size;
        }
        for _ in 0..n_relocs {
            a.relocs
                .push(Reloc::from_words(w(area_bytes, pos), w(area_bytes, pos + 4)));
            pos += 8;
        }
        areas.push(a);
    }

    let mut symbols = Vec::with_capacity(n_syms);
    for i in 0..n_syms {
        let e = i * 16;
        let area_off = w(symt, e + 12);
        symbols.push(Symbol {
            name: strt_name(&strt, w(symt, e)),
            attributes: w(symt, e + 4),
            value: w(symt, e + 8),
            area: if area_off == 0 {
                None
            } else {
                Some(strt_name(&strt, area_off))
            },
        });
    }

    Ok(Object {
        areas,
        symbols,
        entry: if entry_area == 0 {
            None
        } else {
            Some((entry_area, entry_offset))
        },
        identification: get("OBJ_IDFN")
            .map(|b| {
                b.iter()
                    .take_while(|c| **c != 0)
                    .map(|c| *c as char)
                    .collect()
            })
            .unwrap_or_default(),
    })
}

#[cfg(test)]
mod tests {
    use super::*;

    fn word(b: &[u8], i: usize) -> u32 {
        u32::from_le_bytes([b[i], b[i + 1], b[i + 2], b[i + 3]])
    }

    fn simple() -> Object {
        let mut code = Area::new("C$$code", area_attr::CODE | area_attr::READ_ONLY);
        code.data = vec![0x01, 0x00, 0xA0, 0xE3]; // MOV r0, #1
        Object {
            areas: vec![code],
            symbols: vec![Symbol {
                name: "start".into(),
                attributes: sym_attr::DEFINED | sym_attr::GLOBAL,
                value: 0,
                area: Some("C$$code".into()),
            }],
            entry: Some((1, 0)),
            identification: "rosasm".into(),
        }
    }

    #[test]
    fn the_file_announces_itself_as_a_chunk_file() {
        let f = simple().write();
        assert_eq!(word(&f, 0), CHUNK_FILE_ID);
        assert_eq!(word(&f, 4), MAX_CHUNKS);
        assert_eq!(word(&f, 8), 5, "five chunks written");
    }

    #[test]
    fn all_five_chunks_are_present_and_word_aligned() {
        let f = simple().write();
        let cs = chunks(&f).unwrap();
        let names: Vec<&str> = cs.iter().map(|(n, _)| n.as_str()).collect();
        assert_eq!(
            names,
            ["OBJ_HEAD", "OBJ_AREA", "OBJ_IDFN", "OBJ_SYMT", "OBJ_STRT"]
        );
        for i in 0..5 {
            let off = word(&f, 12 + i * 16 + 8);
            assert_eq!(off % 4, 0, "chunk {i} must start on a word boundary");
        }
    }

    #[test]
    fn the_header_carries_type_version_and_counts() {
        let f = simple().write();
        let head = &chunks(&f).unwrap()[0].1;
        assert_eq!(word(head, 0), OBJECT_FILE_TYPE);
        assert_eq!(word(head, 4), AOF_VERSION);
        assert_eq!(word(head, 8), 1, "one area");
        assert_eq!(word(head, 12), 1, "one symbol");
        assert_eq!(word(head, 16), 1, "entry area index is 1-origin");
    }

    #[test]
    fn an_area_header_is_five_words_with_attributes_and_alignment() {
        let f = simple().write();
        let head = &chunks(&f).unwrap()[0].1;
        let attrs = word(head, 24 + 4);
        assert_eq!(attrs & 0xFF, 2, "alignment sits in the low byte");
        assert!(attrs & area_attr::CODE != 0);
        assert!(attrs & area_attr::READ_ONLY != 0);
        assert_eq!(word(head, 24 + 8), 4, "area size, rounded up to a word");
        assert_eq!(word(head, 24 + 12), 0, "no relocations");
    }

    #[test]
    fn names_are_offsets_into_the_string_table() {
        let f = simple().write();
        let cs = chunks(&f).unwrap();
        let head = &cs[0].1;
        let strt = &cs[4].1;
        let off = word(head, 24) as usize;
        assert!(off >= 4, "no valid offset is below the length word");
        let end = strt[off..].iter().position(|b| *b == 0).unwrap();
        assert_eq!(&strt[off..off + end], b"C$$code");
        // The table's first word is its own length, including itself.
        assert_eq!(word(strt, 0) as usize, strt.len());
    }

    #[test]
    fn a_zero_initialised_area_carries_no_bytes() {
        let mut bss = Area::new("bss", area_attr::ZERO_INIT);
        bss.data = vec![0; 1024];
        let o = Object {
            areas: vec![bss],
            ..Default::default()
        };
        let f = o.write();
        let cs = chunks(&f).unwrap();
        assert!(cs[1].1.is_empty(), "OBJ_AREA holds nothing for a zero-init area");
        // ...but the header still declares the size.
        assert_eq!(word(&cs[0].1, 24 + 8), 1024);
    }

    #[test]
    fn relocation_flags_follow_the_bit_layout() {
        let r = Reloc {
            offset: 0,
            by: RelocBy::Symbol(7),
            field: FieldType::Instruction,
            pc_relative: true,
            based: false,
            max_instructions: 1,
        };
        let f = r.flags();
        assert_eq!(f >> 31, 1, "bit 31 shall be 1");
        assert_eq!((f >> 29) & 0x3, 1, "II: at most one instruction");
        assert_eq!((f >> 28) & 1, 0, "B clear");
        assert_eq!((f >> 27) & 1, 1, "A set: relocate by a symbol");
        assert_eq!((f >> 26) & 1, 1, "R set: PC-relative");
        assert_eq!((f >> 24) & 0x3, 3, "FT: instruction");
        assert_eq!(f & 0x00FF_FFFF, 7, "SID");
    }

    #[test]
    fn an_area_relocation_clears_the_a_bit() {
        let r = Reloc {
            offset: 4,
            by: RelocBy::Area(2),
            field: FieldType::Word,
            pc_relative: false,
            based: false,
            max_instructions: 0,
        };
        let f = r.flags();
        assert_eq!((f >> 27) & 1, 0);
        assert_eq!(f & 0x00FF_FFFF, 2);
        assert_eq!((f >> 24) & 0x3, 2, "FT: word");
    }

    #[test]
    fn relocations_follow_their_area_two_words_each() {
        let mut a = Area::new("code", area_attr::CODE);
        a.data = vec![0; 8];
        a.relocs = vec![Reloc {
            offset: 4,
            by: RelocBy::Symbol(3),
            field: FieldType::Instruction,
            pc_relative: true,
            based: false,
            max_instructions: 0,
        }];
        let o = Object { areas: vec![a], ..Default::default() };
        let f = o.write();
        let area_chunk = &chunks(&f).unwrap()[1].1;
        assert_eq!(area_chunk.len(), 8 + 8, "contents then one 2-word directive");
        assert_eq!(word(area_chunk, 8), 4, "the directive's offset");
    }

    #[test]
    fn symbols_are_four_words_each() {
        let f = simple().write();
        let symt = &chunks(&f).unwrap()[3].1;
        assert_eq!(symt.len(), 16);
        assert_eq!(word(symt, 4), sym_attr::DEFINED | sym_attr::GLOBAL);
        assert_eq!(word(symt, 8), 0, "value: offset within its area");
    }

    #[test]
    fn repeated_names_are_interned_once() {
        let o = Object {
            areas: vec![Area::new("same", area_attr::CODE)],
            symbols: vec![
                Symbol { name: "same".into(), attributes: sym_attr::DEFINED, value: 0,
                         area: Some("same".into()) },
            ],
            ..Default::default()
        };
        let f = o.write();
        let cs = chunks(&f).unwrap();
        let strt = &cs[4].1;
        // "same\0" once, after the length word.
        assert_eq!(strt.len(), 4 + 5);
    }

    #[test]
    fn the_identification_chunk_is_nul_terminated() {
        let f = simple().write();
        let idfn = &chunks(&f).unwrap()[2].1;
        assert_eq!(idfn.last(), Some(&0));
        assert!(idfn.starts_with(b"rosasm"));
    }

    #[test]
    fn a_foreign_file_is_rejected_rather_than_misread() {
        assert!(chunks(&[0u8; 32]).is_err());
        assert!(chunks(&[0u8; 4]).is_err());
    }
}

#[cfg(test)]
mod attr_mapping_tests {
    use super::*;
    use crate::layout::parse_area;

    fn map(operands: &str) -> u32 {
        from_objasm_area(&parse_area(operands).unwrap().1)
    }

    #[test]
    fn a_code_area_is_code_readonly_and_apcs32() {
        let v = map("C$$code, CODE, READONLY");
        assert!(v & area_attr::CODE != 0);
        assert!(v & area_attr::READ_ONLY != 0);
        assert!(v & area_attr::APCS_32 != 0, "this target is 32-bit APCS");
    }

    #[test]
    fn a_plain_data_area_is_neither_code_nor_readonly() {
        let v = map("C$$data, DATA");
        assert_eq!(v & area_attr::CODE, 0);
        assert_eq!(v & area_attr::READ_ONLY, 0);
        assert_eq!(v & area_attr::APCS_32, 0, "APCS bits are code-only");
    }

    #[test]
    fn noinit_becomes_zero_initialised_and_drops_read_only() {
        // AOF forbids the combination; the directive's NOINIT wins.
        let v = map("bss, DATA, NOINIT, READONLY");
        assert!(v & area_attr::ZERO_INIT != 0);
        assert_eq!(v & area_attr::READ_ONLY, 0, "incompatible with zero-init");
    }

    #[test]
    fn the_remaining_attributes_map_across() {
        assert!(map("a, CODE, PIC") & area_attr::POSITION_INDEPENDENT != 0);
        assert!(map("a, CODE, REENTRANT") & area_attr::REENTRANT != 0);
        assert!(map("a, CODE, INTERWORK") & area_attr::INTERWORKING != 0);
        assert!(map("a, ABS") & area_attr::ABSOLUTE != 0);
        assert!(map("a, DATA, BASED r9") & area_attr::BASED != 0);
    }
}

#[cfg(test)]
mod roundtrip_tests {
    use super::*;

    fn built() -> Object {
        let mut code = Area::new("Demo$$Code", area_attr::CODE | area_attr::READ_ONLY | area_attr::APCS_32);
        code.data = vec![0x01, 0x00, 0xA0, 0xE3, 0x0E, 0xF0, 0xA0, 0xE1];
        code.relocs.push(Reloc {
            offset: 0,
            by: RelocBy::Symbol(2),
            field: FieldType::Instruction,
            pc_relative: true,
            based: false,
            max_instructions: 1,
        });
        let mut data = Area::new("Demo$$Data", 0);
        data.data = vec![0x78, 0x56, 0x34, 0x12];
        let mut bss = Area::new("Demo$$Bss", area_attr::ZERO_INIT);
        bss.reserved = 64;
        Object {
            areas: vec![code, data, bss],
            symbols: vec![
                Symbol {
                    name: "Demo_Start".into(),
                    attributes: sym_attr::DEFINED | sym_attr::GLOBAL,
                    value: 0,
                    area: Some("Demo$$Code".into()),
                },
                Symbol {
                    name: "OtherThing".into(),
                    attributes: sym_attr::GLOBAL,
                    value: 0,
                    area: None,
                },
            ],
            entry: Some((1, 0)),
            identification: "rosasm test".into(),
        }
    }

    #[test]
    fn an_object_survives_a_round_trip() {
        let o = built();
        let back = read(&o.write()).expect("our own output must read back");
        assert_eq!(back.areas.len(), 3);
        assert_eq!(back.identification, "rosasm test");
        assert_eq!(back.entry, Some((1, 0)));
        for (a, b) in o.areas.iter().zip(&back.areas) {
            assert_eq!(a.name, b.name);
            assert_eq!(a.attributes, b.attributes, "{}", a.name);
            assert_eq!(a.alignment, b.alignment);
            assert_eq!(a.data, b.data, "{}", a.name);
        }
        for (a, b) in o.symbols.iter().zip(&back.symbols) {
            assert_eq!((&a.name, a.attributes, a.value, &a.area), (&b.name, b.attributes, b.value, &b.area));
        }
    }

    #[test]
    fn a_zero_init_area_carries_no_bytes_but_declares_its_size() {
        let back = read(&built().write()).unwrap();
        let bss = back.areas.iter().find(|a| a.name == "Demo$$Bss").unwrap();
        assert!(bss.data.is_empty(), "nothing is stored for a NOINIT area");
        assert_eq!(bss.reserved, 64, "but the linker is told to reserve it");
    }

    #[test]
    fn a_relocation_survives_a_round_trip() {
        let back = read(&built().write()).unwrap();
        let r = &back.areas[0].relocs[0];
        assert_eq!(r.offset, 0);
        assert_eq!(r.by, RelocBy::Symbol(2));
        assert_eq!(r.field, FieldType::Instruction);
        assert!(r.pc_relative);
        assert!(!r.based);
        assert_eq!(r.max_instructions, 1);
    }

    #[test]
    fn a_file_that_is_not_a_chunk_file_is_refused() {
        assert!(read(b"not an object at all").is_err());
    }
}

#[cfg(test)]
mod hash_tests {
    use super::*;

    fn object() -> Object {
        let mut code = Area::new("C$$code", area_attr::CODE | area_attr::READ_ONLY);
        code.data = vec![0x01, 0x00, 0xA0, 0xE3, 0x0E, 0xF0, 0xA0, 0xE1];
        code.relocs.push(Reloc {
            offset: 0,
            by: RelocBy::Symbol(0),
            field: FieldType::Instruction,
            pc_relative: true,
            based: false,
            max_instructions: 1,
        });
        Object {
            areas: vec![code],
            symbols: vec![Symbol {
                name: "start".into(),
                attributes: sym_attr::DEFINED | sym_attr::GLOBAL,
                value: 0,
                area: Some("C$$code".into()),
            }],
            entry: None,
            identification: "rosasm".into(),
        }
    }

    #[test]
    fn the_same_content_hashes_the_same() {
        assert_eq!(content_hash(&object()), content_hash(&object()));
    }

    #[test]
    fn the_producer_is_not_part_of_the_content() {
        // Two assemblers name themselves differently and that is not a
        // difference in the code they made.
        let mut other = object();
        other.identification = "ObjAsm 4.08".into();
        assert_eq!(content_hash(&object()), content_hash(&other));
    }

    #[test]
    fn a_single_changed_byte_shows() {
        let mut other = object();
        other.areas[0].data[0] ^= 1;
        assert_ne!(content_hash(&object()), content_hash(&other));
    }

    #[test]
    fn relocations_hash_the_same_in_either_order() {
        let mut a = object();
        let mut b = object();
        let extra = Reloc {
            offset: 4,
            by: RelocBy::Area(0),
            field: FieldType::Word,
            pc_relative: false,
            based: false,
            max_instructions: 0,
        };
        a.areas[0].relocs.push(extra.clone());
        b.areas[0].relocs.insert(0, extra);
        assert_eq!(content_hash(&a), content_hash(&b));
    }

    #[test]
    fn a_moved_symbol_shows() {
        let mut other = object();
        other.symbols[0].value = 4;
        assert_ne!(content_hash(&object()), content_hash(&other));
    }

    #[test]
    fn an_area_attribute_is_part_of_it() {
        let mut other = object();
        other.areas[0].attributes |= area_attr::REENTRANT;
        assert_ne!(content_hash(&object()), content_hash(&other));
    }
}

#[cfg(test)]
mod hash_ordering_tests {
    use super::*;

    /// Two files that agree in every particular, but whose symbol tables are
    /// in different orders. ObjAsm and this assembler produce such pairs.
    fn pair() -> (Object, Object) {
        let sym = |n: &str, a: u32| Symbol {
            name: n.into(),
            attributes: a,
            value: 0,
            area: Some("c".into()),
        };
        let reloc = |sid: u32| Reloc {
            offset: 0,
            by: RelocBy::Symbol(sid),
            field: FieldType::Word,
            pc_relative: false,
            based: false,
            max_instructions: 0,
        };
        let mut a = Area::new("c", area_attr::CODE);
        a.data = vec![0, 0, 0, 0];
        let mut b = a.clone();
        // Both relocate by `wanted`; it sits at a different index in each.
        a.relocs.push(reloc(0));
        b.relocs.push(reloc(1));
        (
            Object {
                areas: vec![a],
                symbols: vec![sym("wanted", sym_attr::GLOBAL), sym("other", sym_attr::DEFINED)],
                entry: None,
                identification: "ours".into(),
            },
            Object {
                areas: vec![b],
                symbols: vec![sym("other", sym_attr::DEFINED), sym("wanted", sym_attr::GLOBAL)],
                entry: None,
                identification: "ObjAsm".into(),
            },
        )
    }

    #[test]
    fn a_relocation_is_identified_by_name_not_by_index() {
        let (a, b) = pair();
        assert_eq!(content_hash(&a), content_hash(&b));
    }

    #[test]
    fn relocating_by_a_different_symbol_still_shows() {
        let (a, mut b) = pair();
        b.areas[0].relocs[0].by = RelocBy::Symbol(0); // now `other`
        assert_ne!(content_hash(&a), content_hash(&b));
    }
}
