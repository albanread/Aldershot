//! Just enough ELF32 to get code and relocations back out of the encoder.
//!
//! We give UAL text to LLVM's integrated assembler, and it gives back an
//! ELF object. Nothing downstream uses that object as it is: the output is
//! written afresh, as AOF or by `elfwrite`. So this reads only what has to
//! be carried across: the contents of the allocatable sections, the symbol
//! table, and the REL relocations.

#[derive(Debug, Clone)]
pub struct Section {
    pub name: String,
    pub sh_type: u32,
    pub flags: u32,
    pub addr: u32,
    pub data: Vec<u8>,
    /// For a relocation section, the section it applies to.
    pub info: u32,
    pub entsize: u32,
}

#[derive(Debug, Clone)]
pub struct Symbol {
    pub name: String,
    pub value: u32,
    pub size: u32,
    pub info: u8,
    pub shndx: u16,
}

impl Symbol {
    pub fn is_global(&self) -> bool {
        self.info >> 4 == 1
    }
    pub fn is_defined(&self) -> bool {
        self.shndx != 0
    }
}

#[derive(Debug, Clone)]
pub struct Rel {
    /// Section index the relocation applies to.
    pub section: u32,
    pub offset: u32,
    /// Index into the symbol table.
    pub sym: u32,
    pub kind: u8,
}

#[derive(Debug, Default)]
pub struct Object {
    pub sections: Vec<Section>,
    pub symbols: Vec<Symbol>,
    pub rels: Vec<Rel>,
}

pub const SHT_PROGBITS: u32 = 1;
pub const SHT_NOBITS: u32 = 8;
pub const SHF_ALLOC: u32 = 0x2;
pub const SHF_EXECINSTR: u32 = 0x4;
pub const SHF_WRITE: u32 = 0x1;

fn u16le(b: &[u8], i: usize) -> u16 {
    u16::from_le_bytes([b[i], b[i + 1]])
}
fn u32le(b: &[u8], i: usize) -> u32 {
    u32::from_le_bytes([b[i], b[i + 1], b[i + 2], b[i + 3]])
}

fn cstr(b: &[u8], off: usize) -> String {
    let end = b[off..].iter().position(|c| *c == 0).unwrap_or(0);
    String::from_utf8_lossy(&b[off..off + end]).to_string()
}

/// Parse an ELF32 little-endian relocatable object.
pub fn parse(f: &[u8]) -> Result<Object, String> {
    if f.len() < 52 || &f[..4] != b"\x7fELF" {
        return Err("not an ELF file".into());
    }
    if f[4] != 1 || f[5] != 1 {
        return Err("expected 32-bit little-endian ELF".into());
    }
    let shoff = u32le(f, 32) as usize;
    let shentsize = u16le(f, 46) as usize;
    let shnum = u16le(f, 48) as usize;
    let shstrndx = u16le(f, 50) as usize;

    let raw: Vec<&[u8]> = (0..shnum)
        .map(|i| &f[shoff + i * shentsize..shoff + (i + 1) * shentsize])
        .collect();

    let shstr_off = u32le(raw[shstrndx], 16) as usize;
    let shstr_size = u32le(raw[shstrndx], 20) as usize;
    let shstr = &f[shstr_off..shstr_off + shstr_size];

    let mut o = Object::default();
    for r in &raw {
        let name = cstr(shstr, u32le(r, 0) as usize);
        let sh_type = u32le(r, 4);
        let flags = u32le(r, 8);
        let addr = u32le(r, 12);
        let off = u32le(r, 16) as usize;
        let size = u32le(r, 20) as usize;
        let info = u32le(r, 28);
        let entsize = u32le(r, 36);
        // NOBITS occupies no space in the file.
        let data = if sh_type == SHT_NOBITS || off + size > f.len() {
            Vec::new()
        } else {
            f[off..off + size].to_vec()
        };
        o.sections.push(Section { name, sh_type, flags, addr, data, info, entsize });
    }

    // Symbols, and the relocations that name them.
    for (i, s) in o.sections.clone().iter().enumerate() {
        match s.sh_type {
            2 => {
                // SHT_SYMTAB. Its link field gives the string table.
                let strtab_idx = u32le(raw[i], 24) as usize;
                let strtab = &o.sections[strtab_idx].data;
                for e in s.data.as_chunks::<16>().0 {
                    o.symbols.push(Symbol {
                        name: cstr(strtab, u32le(e, 0) as usize),
                        value: u32le(e, 4),
                        size: u32le(e, 8),
                        info: e[12],
                        shndx: u16le(e, 14),
                    });
                }
            }
            9 => {
                // SHT_REL. Each entry is an offset and an info word.
                for e in s.data.as_chunks::<8>().0 {
                    let info = u32le(e, 4);
                    o.rels.push(Rel {
                        section: s.info,
                        offset: u32le(e, 0),
                        sym: info >> 8,
                        kind: (info & 0xFF) as u8,
                    });
                }
            }
            _ => {}
        }
    }
    Ok(o)
}

impl Object {
    /// The sections that carry an image: code and data, not metadata.
    pub fn allocatable(&self) -> impl Iterator<Item = (usize, &Section)> {
        self.sections
            .iter()
            .enumerate()
            .filter(|(_, s)| s.flags & SHF_ALLOC != 0 && !s.name.is_empty())
    }

    pub fn section_named(&self, name: &str) -> Option<&Section> {
        self.sections.iter().find(|s| s.name == name)
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn a_non_elf_file_is_refused() {
        assert!(parse(&[0u8; 64]).is_err());
        assert!(parse(b"short").is_err());
    }

    #[test]
    fn a_64_bit_object_is_refused_rather_than_misread() {
        let mut f = vec![0u8; 64];
        f[..4].copy_from_slice(b"\x7fELF");
        f[4] = 2; // ELFCLASS64
        f[5] = 1;
        assert!(parse(&f).is_err());
    }

    #[test]
    fn symbol_binding_is_read_from_the_info_field() {
        let g = Symbol { name: "g".into(), value: 0, size: 0, info: 0x10, shndx: 1 };
        let l = Symbol { name: "l".into(), value: 0, size: 0, info: 0x00, shndx: 1 };
        let u = Symbol { name: "u".into(), value: 0, size: 0, info: 0x10, shndx: 0 };
        assert!(g.is_global() && g.is_defined());
        assert!(!l.is_global());
        assert!(!u.is_defined(), "shndx 0 is undefined");
    }
}
