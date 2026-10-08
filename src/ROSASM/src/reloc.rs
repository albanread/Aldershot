//! Turning the encoder's fixups into AOF relocation directives.
//!
//! LLVM and the RISC OS linker disagree about what a relocatable field holds
//! before it is relocated. So the addend has to be rewritten as the object is
//! translated.
//!
//! ELF on ARM uses `REL` relocations, which keep the addend in the field
//! itself. For a branch, LLVM stores −8. The ELF linker's rule is
//! `S + A − P`, and the −8 cancels the pipeline offset (pc reads 8 ahead),
//! so the branch lands on `S`.
//!
//! AOF's rule for a PC-relative field, from `docs/DDE/CodeStds/AOF`, is
//!
//! ```text
//!     subject_field += relocation_value − base_of_area_containing(field)
//! ```
//!
//! This is measured from the base of the *area*, not from the field. Take a
//! branch at offset `off` in its area, to a symbol whose value is `S`:
//!
//! ```text
//!     target = area_base + off + 8 + final_field
//!            = area_base + off + 8 + stored + (S − area_base)
//! ```
//!
//! For `target == S`, we need `stored = −off − 8`. That is, the stored addend
//! must also count back to the area's base, which makes it exactly
//! `llvm_addend − off`.
//!
//! A branch to a label in the *same* area needs no relocation at all, because
//! the distance between two points in one area cannot change.

/// The ARM ELF relocation types that hand-written assembler gives rise to.
pub mod elf_type {
    pub const R_ARM_PC24: u8 = 1;
    pub const R_ARM_ABS32: u8 = 2;
    pub const R_ARM_REL32: u8 = 3;
    pub const R_ARM_CALL: u8 = 28;
    pub const R_ARM_JUMP24: u8 = 29;
    /// A single data transfer's 12-bit offset from `pc`: `LDR r0, Label`.
    pub const R_ARM_LDR_PC_G0: u8 = 4;
    /// Emitted for `bx` on targets before v5. The linker may ignore it.
    pub const R_ARM_V4BX: u8 = 40;
}

/// Does this relocation apply to a branch's 24-bit offset field?
pub fn is_branch(kind: u8) -> bool {
    matches!(
        kind,
        elf_type::R_ARM_PC24 | elf_type::R_ARM_CALL | elf_type::R_ARM_JUMP24
    )
}

/// Does this relocation apply to a data transfer's 12-bit offset field?
pub fn is_ldr_literal(kind: u8) -> bool {
    kind == elf_type::R_ARM_LDR_PC_G0
}

/// The byte addend a single data transfer carries.
///
/// The offset is twelve unsigned bits, with a separate add/subtract bit, so
/// the reach is ±4095. That is much less than a branch's reach, which is why
/// a distant literal needs a literal pool rather than a longer offset.
pub fn ldr_addend(insn: u32) -> i32 {
    let imm = (insn & 0xFFF) as i32;
    // Bit 23 is the U bit: set to add, clear to subtract.
    if insn & (1 << 23) != 0 {
        imm
    } else {
        -imm
    }
}

/// Replace a data transfer's offset, setting the add/subtract bit to match.
pub fn set_ldr_addend(insn: u32, bytes: i32) -> Option<u32> {
    if !(-4095..=4095).contains(&bytes) {
        return None;
    }
    let cleared = insn & !(0xFFF | (1 << 23));
    Some(if bytes >= 0 {
        cleared | (1 << 23) | bytes as u32
    } else {
        cleared | (-bytes) as u32
    })
}

/// The byte addend a branch instruction carries, sign-extended.
///
/// The field is a signed count of words, so the range is ±32MB.
pub fn branch_addend(insn: u32) -> i32 {
    let raw = insn & 0x00FF_FFFF;
    // Sign-extend from 24 bits, then scale to bytes.
    let signed = ((raw as i32) << 8) >> 8;
    signed * 4
}

/// Replace a branch instruction's addend, keeping condition and opcode.
///
/// Returns `None` if the offset does not fit. This is the linker's overflow
/// error, caught earlier.
pub fn set_branch_addend(insn: u32, bytes: i32) -> Option<u32> {
    if bytes % 4 != 0 {
        return None;
    }
    let words = bytes / 4;
    if !(-(1 << 23)..(1 << 23)).contains(&words) {
        return None;
    }
    Some((insn & 0xFF00_0000) | ((words as u32) & 0x00FF_FFFF))
}

/// What a fixup at `offset` in an area should store, given the encoder's
/// addend and the target's offset within *its* area.
///
/// `target_in_area` is the label's offset when we know where the target is.
/// It is zero for an imported symbol, whose whole value comes from the
/// linker.
pub fn pc_relative_addend(llvm_addend: i32, offset_in_area: u32, target_in_area: u32) -> i32 {
    llvm_addend + target_in_area as i32 - offset_in_area as i32
}

/// The addend for a reference we resolve ourselves, within one area.
///
/// No relocation is emitted. `pc` reads as the instruction's address plus
/// eight, and both points move together, so the distance is the same wherever
/// the area is placed. The arithmetic is the same for a branch and for a
/// literal load. Only the field the result goes in differs.
pub fn local_pc_addend(offset_in_area: u32, target_in_area: u32) -> i32 {
    target_in_area as i32 - offset_in_area as i32 - 8
}

/// The addend for a branch we resolve ourselves, within one area.
pub fn local_branch_addend(offset_in_area: u32, target_in_area: u32) -> i32 {
    local_pc_addend(offset_in_area, target_in_area)
}

#[cfg(test)]
mod tests {
    use super::*;

    /// `bl` with LLVM's addend for an undefined symbol: EB FF FF FE.
    const BL_MINUS_8: u32 = 0xEBFF_FFFE;

    #[test]
    fn an_undefined_branch_carries_minus_eight() {
        assert_eq!(branch_addend(BL_MINUS_8), -8);
    }

    #[test]
    fn a_forward_addend_reads_back() {
        let insn = set_branch_addend(0xEB00_0000, 0x1000).unwrap();
        assert_eq!(branch_addend(insn), 0x1000);
        // The condition and opcode bits survive.
        assert_eq!(insn & 0xFF00_0000, 0xEB00_0000);
    }

    #[test]
    fn a_backward_addend_reads_back() {
        let insn = set_branch_addend(0x1A00_0000, -0x2000).unwrap();
        assert_eq!(branch_addend(insn), -0x2000);
        assert_eq!(insn & 0xFF00_0000, 0x1A00_0000, "BNE stays BNE");
    }

    #[test]
    fn an_out_of_range_or_misaligned_offset_is_refused() {
        assert_eq!(set_branch_addend(0xEB00_0000, 2), None, "not a word offset");
        assert_eq!(set_branch_addend(0xEB00_0000, 1 << 25), None, "beyond ±32MB");
        assert!(set_branch_addend(0xEB00_0000, (1 << 25) - 4).is_some());
    }

    #[test]
    fn a_local_branch_lands_on_its_target() {
        // A branch at &10 to a label at &40: pc reads &18, so the offset is &28.
        assert_eq!(local_branch_addend(0x10, 0x40), 0x28);
        // Backwards.
        assert_eq!(local_branch_addend(0x40, 0x10), -0x38);
        // A branch to itself is the classic -8.
        assert_eq!(local_branch_addend(0x40, 0x40), -8);
    }

    #[test]
    fn an_imported_branch_counts_back_to_the_area_base() {
        // The stored addend is -off-8. Adding (S - area_base) then lands on
        // S, once the pc's 8-byte offset is allowed for.
        assert_eq!(pc_relative_addend(-8, 0x20, 0), -0x28);
        assert_eq!(pc_relative_addend(-8, 0, 0), -8);
    }

    #[test]
    fn a_cross_area_branch_adds_the_target_offset() {
        // Same as an import, plus where the label sits inside its own area.
        assert_eq!(pc_relative_addend(-8, 0x20, 0x100), 0x100 - 0x20 - 8);
    }

    #[test]
    fn the_aof_rule_reproduces_the_target() {
        // Work the linker's arithmetic through, for a branch at &20 in an area
        // linked at &8000, to a symbol that ends up at &9000.
        let (area_base, off, s) = (0x8000i64, 0x20i64, 0x9000i64);
        let stored = pc_relative_addend(-8, off as u32, 0) as i64;
        let final_field = stored + (s - area_base);
        let target = area_base + off + 8 + final_field;
        assert_eq!(target, s);
    }

    #[test]
    fn a_literal_load_addend_reads_back_either_way() {
        // LDR r0, [pc, #8]
        let insn = 0xE59F_0008;
        assert_eq!(ldr_addend(insn), 8);
        let back = set_ldr_addend(insn, -16).unwrap();
        assert_eq!(ldr_addend(back), -16);
        assert_eq!(back & 0xF000, 0x0000, "the register field is untouched");
        // And back again.
        assert_eq!(ldr_addend(set_ldr_addend(back, 4).unwrap()), 4);
    }

    #[test]
    fn a_literal_beyond_the_twelve_bit_reach_is_refused() {
        assert_eq!(set_ldr_addend(0xE59F_0000, 4096), None);
        assert_eq!(set_ldr_addend(0xE59F_0000, -4096), None);
        assert!(set_ldr_addend(0xE59F_0000, 4095).is_some());
    }

    #[test]
    fn a_local_literal_load_lands_on_its_target() {
        // A load at &10 reading a word at &40: pc is &18, so the offset is &28.
        assert_eq!(local_pc_addend(0x10, 0x40), 0x28);
        assert_eq!(local_pc_addend(0x40, 0x10), -0x38);
    }

    #[test]
    fn only_branch_relocations_touch_the_offset_field() {
        assert!(is_branch(elf_type::R_ARM_CALL));
        assert!(is_branch(elf_type::R_ARM_JUMP24));
        assert!(is_branch(elf_type::R_ARM_PC24));
        assert!(!is_branch(elf_type::R_ARM_ABS32));
        assert!(!is_branch(elf_type::R_ARM_V4BX));
        assert!(!is_branch(elf_type::R_ARM_LDR_PC_G0));
        assert!(is_ldr_literal(elf_type::R_ARM_LDR_PC_G0));
        assert!(!is_ldr_literal(elf_type::R_ARM_CALL));
    }
}
