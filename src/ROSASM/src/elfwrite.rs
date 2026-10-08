//! ELF32 little-endian ARM relocatable objects.
//!
//! `aof.rs` writes AOF, which is what the RISC OS linker reads. Not every
//! linker reads it. roscc, the linker that builds modules from clang's
//! output, reads only ELF. So a source assembled here can reach roscc only
//! through this writer.
//!
//! This is not a conversion from AOF. The assembler already holds every
//! relocation in ELF terms. The encoder is clang, and `elfread` reads
//! clang's relocations back with their ELF type and addend unchanged.
//! `aof.rs` is where they are turned into AOF's flag word and AOF's way of
//! storing the addend. So the two writers work side by side from one
//! object. This one is the shorter path, because it keeps the type and the
//! addend the encoder gave.
//!
//! What ELF needs that AOF does not:
//!
//!   - a null section at index 0, and a null symbol at index 0;
//!   - every local symbol before every global one, with the index of the
//!     first global recorded in the symbol table's `sh_info`;
//!   - a section symbol for each section, because ELF writes a relocation by
//!     an area's base as one against that section's symbol;
//!   - relocations in their own `SHT_REL` section, naming their target in
//!     `sh_info`.

use crate::aof::{area_attr, sym_attr, Area, Symbol};

// Section header types and flags.
const SHT_PROGBITS: u32 = 1;
const SHT_SYMTAB: u32 = 2;
const SHT_STRTAB: u32 = 3;
const SHT_NOBITS: u32 = 8;
const SHT_REL: u32 = 9;
const SHF_WRITE: u32 = 0x1;
const SHF_ALLOC: u32 = 0x2;
const SHF_EXECINSTR: u32 = 0x4;

// Symbol binding and type, as packed into `st_info`.
const STB_LOCAL: u8 = 0;
const STB_GLOBAL: u8 = 1;
const STT_NOTYPE: u8 = 0;
const STT_SECTION: u8 = 3;
const SHN_ABS: u16 = 0xfff1;

const EM_ARM: u16 = 40;
const ET_REL: u16 = 1;
const EHDR_SIZE: u32 = 52;
const SHDR_SIZE: u32 = 40;
const SYM_SIZE: u32 = 16;

/// What a relocation is resolved against.
#[derive(Debug, Clone, Copy, PartialEq)]
pub enum Target {
    /// The base of the area with this index. In ELF this is a relocation
    /// against that section's own symbol, with the offset within the section
    /// carried as the addend.
    Section(usize),
    /// The symbol with this index in the assembler's symbol table.
    Symbol(u32),
}

/// One relocation, in the terms the encoder gave.
#[derive(Debug, Clone)]
pub struct Rel {
    /// The area the subject field is in.
    pub area: usize,
    /// Byte offset of the subject field within that area.
    pub offset: u32,
    pub target: Target,
    /// The ELF relocation type, e.g. `R_ARM_CALL`. The addend is not here.
    /// These are `SHT_REL` relocations, so it stays in the field itself.
    pub kind: u8,
}

/// A section as it will be written, and where it came from.
struct Sec {
    name: String,
    sh_type: u32,
    flags: u32,
    align: u32,
    data: Vec<u8>,
    /// `sh_link` and `sh_info`. Only the symbol and relocation sections use
    /// them.
    link: u32,
    info: u32,
    entsize: u32,
}

#[derive(Default)]
struct Strings {
    bytes: Vec<u8>,
}

impl Strings {
    fn new() -> Self {
        // Offset 0 is the empty string, which anything without a name uses.
        Strings { bytes: vec![0] }
    }

    fn add(&mut self, s: &str) -> u32 {
        if s.is_empty() {
            return 0;
        }
        let off = self.bytes.len() as u32;
        self.bytes.extend_from_slice(s.as_bytes());
        self.bytes.push(0);
        off
    }
}

fn push_u16(v: &mut Vec<u8>, x: u16) {
    v.extend_from_slice(&x.to_le_bytes());
}

fn push_u32(v: &mut Vec<u8>, x: u32) {
    v.extend_from_slice(&x.to_le_bytes());
}

/// The name ObjAsm gives a module's header area.
///
/// `Link -rmf` requires this name, so a module's source cannot call the
/// area anything else. ELF tools have their own name for the same thing.
const OBJASM_MODULE_HEADER: &str = "!!!Module$$Header";

/// The section roscc looks for when it links a module. The header has to be
/// at offset 0, and roscc finds it by this name.
const ELF_MODULE_HEADER: &str = ".module";

/// An ELF section name for an ObjAsm area.
///
/// Area names contain characters that ELF allows but tools read badly. So a
/// plain area name gets a prefix (`.text.` or `.data.`). This keeps ObjAsm's
/// name visible and makes it a proper section name. Two kinds of name are
/// not plain:
///
///   - `!!!Module$$Header` becomes `.module`. The two toolchains spell the
///     same thing differently, and each requires its own spelling. So one
///     source can build both ways only if the writer translates it here.
///   - A name that already starts with a dot is left exactly as it is. So a
///     source that names `.module` itself still gets `.module`.
fn section_name(area: &Area) -> String {
    if area.name == OBJASM_MODULE_HEADER {
        return ELF_MODULE_HEADER.to_string();
    }
    if area.name.starts_with('.') {
        return area.name.clone();
    }
    let code = area.attributes & area_attr::CODE != 0;
    format!("{}{}", if code { ".text." } else { ".data." }, area.name)
}

/// The ELF object for these areas, symbols and relocations.
///
/// `symbols` is in the order that `Target::Symbol` indexes. This function
/// maps that order onto ELF's own, which puts the locals first.
pub fn write(areas: &[Area], symbols: &[Symbol], rels: &[Rel]) -> Vec<u8> {
    let mut shstr = Strings::new();
    let mut str_ = Strings::new();

    // Section 0 is the null section, and the areas follow it. So an area's
    // section index is its own index plus one.
    let area_shndx = |ai: usize| (ai + 1) as u16;

    let mut secs: Vec<Sec> = Vec::new();
    for a in areas {
        let code = a.attributes & area_attr::CODE != 0;
        let zero = a.attributes & area_attr::ZERO_INIT != 0;
        let read_only = a.attributes & area_attr::READ_ONLY != 0;
        let mut flags = SHF_ALLOC;
        if code {
            flags |= SHF_EXECINSTR;
        }
        if !read_only {
            flags |= SHF_WRITE;
        }
        // A ZERO_INIT area has no bytes in the file. Its size is what it
        // reserves.
        let data = if zero {
            vec![0u8; a.reserved.max(a.data.len() as u32) as usize]
        } else {
            a.data.clone()
        };
        secs.push(Sec {
            name: section_name(a),
            sh_type: if zero { SHT_NOBITS } else { SHT_PROGBITS },
            flags,
            align: 1u32 << a.alignment.clamp(2, 16),
            data,
            link: 0,
            info: 0,
            entsize: 0,
        });
    }

    // ---- symbols ---------------------------------------------------------
    //
    // ELF order is the null symbol, then every local, then every global. A
    // section symbol is local. There is one for each area, whether or not a
    // relocation names it, so that any relocation by an area's base has a
    // symbol to name.
    let mut elf_syms: Vec<Vec<u8>> = Vec::new();
    let sym_entry = |name: u32, value: u32, info: u8, shndx: u16| {
        let mut e = Vec::with_capacity(SYM_SIZE as usize);
        push_u32(&mut e, name);
        push_u32(&mut e, value);
        push_u32(&mut e, 0); // st_size: not tracked
        e.push(info);
        e.push(0); // st_other
        push_u16(&mut e, shndx);
        e
    };
    elf_syms.push(sym_entry(0, 0, 0, 0)); // the null symbol

    // Section symbols, in area order, so area `ai` is ELF symbol `ai + 1`.
    for (ai, _) in areas.iter().enumerate() {
        elf_syms.push(sym_entry(
            0,
            0,
            (STB_LOCAL << 4) | STT_SECTION,
            area_shndx(ai),
        ));
    }
    let section_sym = |ai: usize| (ai + 1) as u32;

    // The assembler's own symbols, locals first. `where_` maps an index in
    // `symbols` onto the ELF symbol index it ended up at.
    let mut where_: Vec<u32> = vec![0; symbols.len()];
    let area_index = |name: &Option<String>| -> Option<usize> {
        let n = name.as_ref()?;
        areas.iter().position(|a| &a.name == n)
    };
    let describe = |s: &Symbol| -> (u32, u16) {
        // (value, section index)
        if s.attributes & sym_attr::ABSOLUTE != 0 {
            (s.value, SHN_ABS)
        } else if s.attributes & sym_attr::DEFINED != 0 {
            match area_index(&s.area) {
                Some(ai) => (s.value, area_shndx(ai)),
                None => (s.value, SHN_ABS),
            }
        } else {
            // An external reference: no value, no section.
            (0, 0)
        }
    };
    for global_pass in [false, true] {
        for (i, s) in symbols.iter().enumerate() {
            let is_global = s.attributes & sym_attr::GLOBAL != 0;
            if is_global != global_pass {
                continue;
            }
            if !global_pass {
                // A local that is not defined here is left out. It has
                // nothing to tell the linker.
                if s.attributes & sym_attr::DEFINED == 0 {
                    continue;
                }
            }
            let (value, shndx) = describe(s);
            let bind = if is_global { STB_GLOBAL } else { STB_LOCAL };
            where_[i] = elf_syms.len() as u32;
            let name = str_.add(&s.name);
            elf_syms.push(sym_entry(name, value, (bind << 4) | STT_NOTYPE, shndx));
        }
        if !global_pass {
            // Everything written so far is local. ELF needs to be told where
            // the globals start, and this is the point where that is known.
            secs.push(Sec {
                name: String::from(".symtab"),
                sh_type: SHT_SYMTAB,
                flags: 0,
                align: 4,
                data: Vec::new(), // filled once every symbol is in
                link: 0,          // the string table, patched below
                info: elf_syms.len() as u32,
                entsize: SYM_SIZE,
            });
        }
    }
    let symtab_at = secs.len() - 1;
    secs[symtab_at].data = elf_syms.concat();

    // ---- relocations -----------------------------------------------------
    //
    // One SHT_REL section for each area that has relocations. Its sh_info
    // gives the area, and its sh_link gives the symbol table.
    let mut rel_secs: Vec<Sec> = Vec::new();
    for (ai, a) in areas.iter().enumerate() {
        let mine: Vec<&Rel> = rels.iter().filter(|r| r.area == ai).collect();
        if mine.is_empty() {
            continue;
        }
        let mut data = Vec::with_capacity(mine.len() * 8);
        for r in mine {
            let sym = match r.target {
                Target::Section(t) => section_sym(t),
                Target::Symbol(i) => *where_.get(i as usize).unwrap_or(&0),
            };
            push_u32(&mut data, r.offset);
            push_u32(&mut data, (sym << 8) | r.kind as u32);
        }
        rel_secs.push(Sec {
            name: format!(".rel{}", section_name(a)),
            sh_type: SHT_REL,
            flags: 0,
            align: 4,
            data,
            link: symtab_at as u32, // set again below, with the other links
            info: area_shndx(ai) as u32,
            entsize: 8,
        });
    }
    secs.extend(rel_secs);

    // The string tables go last, so every index above is already settled.
    let strtab_at = secs.len();
    secs.push(Sec {
        name: String::from(".strtab"),
        sh_type: SHT_STRTAB,
        flags: 0,
        align: 1,
        data: std::mem::take(&mut str_.bytes),
        link: 0,
        info: 0,
        entsize: 0,
    });
    secs[symtab_at].link = strtab_at as u32;
    for s in secs.iter_mut() {
        if s.sh_type == SHT_REL {
            s.link = symtab_at as u32;
        }
    }
    let shstrtab_at = secs.len();
    secs.push(Sec {
        name: String::from(".shstrtab"),
        sh_type: SHT_STRTAB,
        flags: 0,
        align: 1,
        data: Vec::new(), // filled once every section name is in it
        link: 0,
        info: 0,
        entsize: 0,
    });

    // Section 0, the null section, is not in `secs`. It is written directly.
    let name_offsets: Vec<u32> = secs.iter().map(|s| shstr.add(&s.name)).collect();
    secs[shstrtab_at].data = std::mem::take(&mut shstr.bytes);

    // ---- layout ----------------------------------------------------------
    //
    // The header, then every section's contents, then the section header
    // table. Only the contents need aligning. A NOBITS section takes no
    // space in the file.
    let mut offsets: Vec<u32> = Vec::with_capacity(secs.len());
    let mut cursor = EHDR_SIZE;
    for s in &secs {
        if s.sh_type == SHT_NOBITS {
            offsets.push(cursor);
            continue;
        }
        let align = s.align.max(1);
        cursor = cursor.div_ceil(align) * align;
        offsets.push(cursor);
        cursor += s.data.len() as u32;
    }
    cursor = cursor.div_ceil(4) * 4;
    let shoff = cursor;

    let mut out: Vec<u8> = Vec::with_capacity((shoff + (secs.len() as u32 + 1) * SHDR_SIZE) as usize);
    out.extend_from_slice(&[0x7f, b'E', b'L', b'F']);
    out.push(1); // ELFCLASS32
    out.push(1); // ELFDATA2LSB
    out.push(1); // EV_CURRENT
    out.push(0); // ELFOSABI_NONE
    out.extend_from_slice(&[0; 8]); // EI_ABIVERSION and padding
    push_u16(&mut out, ET_REL);
    push_u16(&mut out, EM_ARM);
    push_u32(&mut out, 1); // e_version
    push_u32(&mut out, 0); // e_entry: a relocatable object has none
    push_u32(&mut out, 0); // e_phoff
    push_u32(&mut out, shoff);
    // EF_ARM_EABI_VER5, with no float-ABI flag.
    push_u32(&mut out, 0x0500_0000);
    push_u16(&mut out, EHDR_SIZE as u16);
    push_u16(&mut out, 0); // e_phentsize
    push_u16(&mut out, 0); // e_phnum
    push_u16(&mut out, SHDR_SIZE as u16);
    push_u16(&mut out, secs.len() as u16 + 1); // + the null section
    push_u16(&mut out, shstrtab_at as u16 + 1);

    for (i, s) in secs.iter().enumerate() {
        if s.sh_type == SHT_NOBITS {
            continue;
        }
        while out.len() < offsets[i] as usize {
            out.push(0);
        }
        out.extend_from_slice(&s.data);
    }
    while out.len() < shoff as usize {
        out.push(0);
    }

    // The null section header, then one per section.
    out.extend_from_slice(&[0; SHDR_SIZE as usize]);
    for (i, s) in secs.iter().enumerate() {
        push_u32(&mut out, name_offsets[i]);
        push_u32(&mut out, s.sh_type);
        push_u32(&mut out, s.flags);
        push_u32(&mut out, 0); // sh_addr: assigned by the linker
        push_u32(&mut out, offsets[i]);
        push_u32(&mut out, s.data.len() as u32);
        // A section index in sh_link must count the null section too, so
        // one is added here. sh_info is written as it stands. A relocation
        // section's sh_info is already the true index of the section it
        // applies to, and the symbol table's is a count.
        push_u32(&mut out, if s.link != 0 { s.link + 1 } else { 0 });
        push_u32(&mut out, s.info);
        push_u32(&mut out, s.align.max(1));
        push_u32(&mut out, s.entsize);
    }
    out
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::aof::{area_attr, sym_attr, Area, Symbol};

    fn rd_u16(d: &[u8], o: usize) -> u16 {
        u16::from_le_bytes([d[o], d[o + 1]])
    }
    fn rd_u32(d: &[u8], o: usize) -> u32 {
        u32::from_le_bytes([d[o], d[o + 1], d[o + 2], d[o + 3]])
    }

    fn one_code_area() -> Area {
        let mut a = Area::new(
            "!!!Module$$Header",
            area_attr::CODE | area_attr::READ_ONLY | area_attr::APCS_32,
        );
        // BL to somewhere else, with the encoder's addend of -8 in place.
        a.data = vec![0xfe, 0xff, 0xff, 0xeb];
        a
    }

    #[test]
    fn writes_an_elf32_arm_relocatable_header() {
        let out = write(&[one_code_area()], &[], &[]);
        assert_eq!(&out[0..4], b"\x7fELF");
        assert_eq!(out[4], 1, "ELFCLASS32");
        assert_eq!(out[5], 1, "little-endian");
        assert_eq!(rd_u16(&out, 16), ET_REL);
        assert_eq!(rd_u16(&out, 18), EM_ARM, "EM_ARM");
        // The null section is counted, and every section has a header.
        assert!(rd_u16(&out, 0x30) >= 4);
    }

    #[test]
    fn the_objasm_module_header_becomes_the_elf_one() {
        // The name Link -rmf insists on and the name roscc looks for are
        // the same section. One source can build with either toolchain.
        assert_eq!(section_name(&one_code_area()), ".module");

        let mut dotted = Area::new(".module", area_attr::CODE | area_attr::READ_ONLY);
        dotted.data = vec![0; 4];
        assert_eq!(section_name(&dotted), ".module");

        let mut plain = Area::new("C$$code", area_attr::CODE | area_attr::READ_ONLY);
        plain.data = vec![0; 4];
        assert_eq!(section_name(&plain), ".text.C$$code");
        let data = Area::new("C$$data", area_attr::READ_ONLY);
        assert_eq!(section_name(&data), ".data.C$$data");
    }

    #[test]
    fn locals_come_before_globals_and_sh_info_says_where() {
        let area = one_code_area();
        let symbols = vec![
            Symbol {
                name: "an_export".into(),
                attributes: sym_attr::DEFINED | sym_attr::GLOBAL,
                value: 0,
                area: Some("!!!Module$$Header".into()),
            },
            Symbol {
                name: "a_local".into(),
                attributes: sym_attr::DEFINED,
                value: 4,
                area: Some("!!!Module$$Header".into()),
            },
            Symbol {
                name: "an_import".into(),
                attributes: sym_attr::GLOBAL,
                value: 0,
                area: None,
            },
        ];
        let out = write(&[area], &symbols, &[]);

        // Find .symtab through the section headers.
        let shoff = rd_u32(&out, 0x20) as usize;
        let shnum = rd_u16(&out, 0x30) as usize;
        let mut symtab = None;
        for i in 0..shnum {
            let b = shoff + i * SHDR_SIZE as usize;
            if rd_u32(&out, b + 4) == SHT_SYMTAB {
                symtab = Some((rd_u32(&out, b + 16), rd_u32(&out, b + 20), rd_u32(&out, b + 28)));
            }
        }
        let (off, size, first_global) = symtab.expect("a symbol table");
        let count = size / SYM_SIZE;
        // null + one section symbol + the local = 3 before the globals.
        assert_eq!(first_global, 3, "null, section symbol, then the local");
        assert_eq!(count, 5, "and both globals after them");

        for i in 0..count {
            let b = (off + i * SYM_SIZE) as usize;
            let bind = out[b + 12] >> 4;
            assert_eq!(
                bind > STB_LOCAL,
                i >= first_global,
                "symbol {i} is on the wrong side of sh_info"
            );
        }
    }

    #[test]
    fn a_relocation_names_its_area_and_keeps_the_encoders_type() {
        let area = one_code_area();
        let symbols = vec![Symbol {
            name: "hostfs_init".into(),
            attributes: sym_attr::GLOBAL,
            value: 0,
            area: None,
        }];
        let rels = vec![Rel {
            area: 0,
            offset: 0,
            target: Target::Symbol(0),
            kind: crate::reloc::elf_type::R_ARM_CALL,
        }];
        let out = write(&[area], &symbols, &rels);

        let shoff = rd_u32(&out, 0x20) as usize;
        let shnum = rd_u16(&out, 0x30) as usize;
        let mut rel = None;
        for i in 0..shnum {
            let b = shoff + i * SHDR_SIZE as usize;
            if rd_u32(&out, b + 4) == SHT_REL {
                rel = Some((rd_u32(&out, b + 16), rd_u32(&out, b + 20), rd_u32(&out, b + 28)));
            }
        }
        let (off, size, info) = rel.expect("a relocation section");
        assert_eq!(size, 8, "one relocation");
        assert_eq!(info, 1, "against section 1, the only area");
        let r_offset = rd_u32(&out, off as usize);
        let r_info = rd_u32(&out, off as usize + 4);
        assert_eq!(r_offset, 0);
        assert_eq!(
            (r_info & 0xff) as u8,
            crate::reloc::elf_type::R_ARM_CALL,
            "the encoder's own type, not one derived here"
        );
        // The addend stays in the instruction. SHT_REL carries no addend,
        // so the BL in section 1 still holds the encoder's -8.
        let code = rd_u32(&out, shoff + SHDR_SIZE as usize + 16) as usize;
        assert_eq!(rd_u32(&out, code), 0xebff_fffe);
    }

    #[test]
    fn a_relocation_by_area_base_names_that_sections_symbol() {
        let mut second = Area::new("C$$code", area_attr::CODE | area_attr::READ_ONLY);
        second.data = vec![0; 4];
        let rels = vec![Rel {
            area: 0,
            offset: 0,
            target: Target::Section(1),
            kind: crate::reloc::elf_type::R_ARM_CALL,
        }];
        let out = write(&[one_code_area(), second], &[], &rels);

        let shoff = rd_u32(&out, 0x20) as usize;
        let shnum = rd_u16(&out, 0x30) as usize;
        let mut rel_off = None;
        for i in 0..shnum {
            let b = shoff + i * SHDR_SIZE as usize;
            if rd_u32(&out, b + 4) == SHT_REL {
                rel_off = Some(rd_u32(&out, b + 16));
            }
        }
        let r_info = rd_u32(&out, rel_off.expect("a relocation section") as usize + 4);
        // Section symbols follow the null symbol, in area order. So the
        // second area's is symbol 2.
        assert_eq!(r_info >> 8, 2, "the second area's section symbol");
    }
}
