//! The ObjAsm vocabulary, as measured from the RISC OS 5.31 corpus.
//!
//! This is the acceptance checklist for the front end. Every directive listed
//! here occurs in the sources we must assemble, and nothing outside this list
//! does. Counts are occurrences across 1,448 files (702,349 lines). They are
//! recorded so that the work can be done in order of actual use, not in the
//! order the manual lists things.

/// A directive we must implement, with its measured frequency in the corpus.
pub struct Directive {
    pub name: &'static str,
    pub count: u32,
    pub group: Group,
}

#[derive(Debug, Clone, Copy, PartialEq)]
pub enum Group {
    /// Storage and constants — the bulk of the bytes.
    Data,
    /// Macro definition and expansion.
    Macro,
    /// Conditional assembly and loops.
    Control,
    /// Symbol declaration and assignment.
    Symbol,
    /// Section and layout control.
    Layout,
    /// Linkage: what leaves and enters the object.
    Linkage,
    /// Listing and diagnostics.
    Meta,
}
use Group::*;

/// Keyword directives. The surveyed ones come first, highest frequency
/// first; the ones added later follow them.
pub const DIRECTIVES: &[Directive] = &[
    Directive { name: "DCD",     count: 8950, group: Data },
    Directive { name: "DCB",     count: 7537, group: Data },
    Directive { name: "ASSERT",  count: 3989, group: Meta },
    Directive { name: "ROUT",    count: 3493, group: Layout },
    Directive { name: "GET",     count: 3223, group: Control },
    Directive { name: "SETS",    count: 2638, group: Symbol },
    Directive { name: "ALIGN",   count: 2263, group: Layout },
    Directive { name: "GBLS",    count: 1919, group: Symbol },
    Directive { name: "MACRO",   count: 1514, group: Macro },
    Directive { name: "MEND",    count: 1514, group: Macro },
    Directive { name: "END",     count: 1228, group: Layout },
    Directive { name: "SETL",    count: 1221, group: Symbol },
    Directive { name: "GBLL",    count: 1153, group: Symbol },
    Directive { name: "EXPORT",  count:  990, group: Linkage },
    Directive { name: "RN",      count:  864, group: Symbol },
    Directive { name: "SETA",    count:  718, group: Symbol },
    Directive { name: "LTORG",   count:  545, group: Layout },
    Directive { name: "IMPORT",  count:  434, group: Linkage },
    Directive { name: "DCW",     count:  412, group: Data },
    Directive { name: "AREA",    count:  400, group: Layout },
    Directive { name: "OPT",     count:  294, group: Meta },
    Directive { name: "GBLA",    count:  238, group: Symbol },
    Directive { name: "SUBT",    count:  205, group: Meta },
    Directive { name: "LCLS",    count:  139, group: Symbol },
    Directive { name: "LCLA",    count:  109, group: Symbol },
    Directive { name: "TTL",     count:   89, group: Meta },
    Directive { name: "WHILE",   count:   85, group: Control },
    Directive { name: "WEND",    count:   85, group: Control },
    Directive { name: "CN",      count:   60, group: Symbol },
    Directive { name: "FN",      count:   36, group: Symbol },
    Directive { name: "ENTRY",   count:   35, group: Linkage },
    Directive { name: "DCQ",     count:   20, group: Data },
    Directive { name: "ARM",     count:   20, group: Layout },
    Directive { name: "KEEP",    count:   17, group: Linkage },
    Directive { name: "DCFD",    count:   15, group: Data },
    Directive { name: "MEXIT",   count:   12, group: Macro },
    Directive { name: "LCLL",    count:   10, group: Symbol },
    Directive { name: "EXTERN",  count:    9, group: Linkage },
    Directive { name: "DCFS",    count:    8, group: Data },
    Directive { name: "CODE32",  count:    7, group: Layout },
    Directive { name: "SPACE",   count:    5, group: Data },
    Directive { name: "DATA",    count:    4, group: Layout },
    Directive { name: "INCLUDE", count:    2, group: Control },
    // Counted by hand: ResourceFS's `ResourceFile` macro, Messages, and
    // RISC_OSLib's SWI-name tables.
    Directive { name: "BIN",     count:    4, group: Data },
    Directive { name: "INCBIN",  count:    0, group: Data },
    // Keyword spellings of symbolic forms. The corpus harness found these.
    // The original survey missed them because it counted only the
    // punctuation spellings.
    Directive { name: "EQU",     count: 1822, group: Symbol },
    Directive { name: "IF",      count:   32, group: Control },
    Directive { name: "ENDIF",   count:   32, group: Control },
    Directive { name: "ELSE",    count:   11, group: Control },
];

/// Symbolic directive forms. These occupy the opcode field but are punctuation,
/// and several are aliases for a keyword above.
pub struct Symbolic {
    pub sym: &'static str,
    pub count: u32,
    pub means: &'static str,
    pub group: Group,
}

pub const SYMBOLIC: &[Symbolic] = &[
    Symbolic { sym: "*", count: 19260, means: "EQU",          group: Symbol },
    Symbolic { sym: "[", count: 14978, means: "IF",           group: Control },
    Symbolic { sym: "]", count: 14776, means: "ENDIF",        group: Control },
    Symbolic { sym: "#", count: 13196, means: "field in MAP", group: Layout },
    Symbolic { sym: "|", count:  4454, means: "ELSE",         group: Control },
    Symbolic { sym: "^", count:  2721, means: "MAP",          group: Layout },
    Symbolic { sym: "!", count:   395, means: "assert+report",group: Meta },
    Symbolic { sym: "%", count:    48, means: "SPACE",        group: Data },
    // Classic ObjAsm aliases. They are rare or absent here, but cheap to
    // accept.
    Symbolic { sym: "=", count:     0, means: "DCB",          group: Data },
    Symbolic { sym: "&", count:     0, means: "DCD",          group: Data },
];

/// Expression operators, highest frequency first. Because of the ones that
/// work on strings, the evaluator must do more than integer arithmetic.
pub const OPERATORS: &[(&str, u32)] = &[
    (":SHL:", 4083),
    (":LNOT:", 1944),
    (":OR:", 1463),
    (":DEF:", 1292),
    (":INDEX:", 898),
    (":LAND:", 627),
    (":AND:", 476),
    (":LOR:", 427),
    (":CC:", 316),
    (":SHR:", 239),
    (":STR:", 193),
    (":MOD:", 115),
    (":LEN:", 110),
    (":NOT:", 96),
    (":RIGHT:", 66),
    (":EOR:", 60),
    (":BASE:", 53),
    (":LEFT:", 45),
    (":CHR:", 35),
    (":ROR:", 3),
    (":ROL:", 3),
];

pub fn is_directive(op: &str) -> bool {
    let up = op.to_ascii_uppercase();
    DIRECTIVES.iter().any(|d| d.name == up)
}

pub fn is_symbolic(op: &str) -> bool {
    SYMBOLIC.iter().any(|s| s.sym == op)
}

/// Total directive occurrences the corpus contains, for progress reporting.
pub fn total_weight() -> u32 {
    DIRECTIVES.iter().map(|d| d.count).sum::<u32>()
        + SYMBOLIC.iter().map(|s| s.count).sum::<u32>()
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn directive_lookup_is_case_insensitive() {
        assert!(is_directive("DCD"));
        assert!(is_directive("dcd"));
        assert!(is_directive("Dcd"));
        assert!(!is_directive("MOV"));
    }

    #[test]
    fn macro_and_mend_are_paired_in_the_corpus() {
        let m = DIRECTIVES.iter().find(|d| d.name == "MACRO").unwrap();
        let e = DIRECTIVES.iter().find(|d| d.name == "MEND").unwrap();
        assert_eq!(m.count, e.count, "every MACRO must have its MEND");
    }

    #[test]
    fn while_and_wend_are_paired() {
        let w = DIRECTIVES.iter().find(|d| d.name == "WHILE").unwrap();
        let e = DIRECTIVES.iter().find(|d| d.name == "WEND").unwrap();
        assert_eq!(w.count, e.count);
    }

    #[test]
    fn symbolic_forms_are_recognised() {
        for s in ["*", "[", "]", "#", "|", "^", "!", "%", "=", "&"] {
            assert!(is_symbolic(s), "{s} should be a symbolic directive");
        }
    }
}
