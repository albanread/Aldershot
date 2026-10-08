//! `rosasm` — an ObjAsm-compatible assembler for RISC OS 5, and a compiler
//! from the code it assembles to C (`emitc`, with `a32`, `lift` and
//! `structure`).
//!
//! As an assembler, this crate does everything above the mnemonic: the macro language,
//! conditional assembly, the symbol table with its three variable types,
//! directives and layout. LLVM encodes the instructions. The crate lowers
//! each one to UAL for it, reads LLVM's object and relocations back, and
//! writes the AOF or ELF object itself.
//!
//! The target is the Raspberry Pi 4 (Cortex-A72, ARMv8-A in AArch32,
//! NEON/VFPv4). It has no 26-bit modes and no FPA hardware.
//!
//! README.md says what the crate is for; STATUS.md says where it stands.

pub mod a32;
pub mod aof;
pub mod elfread;
pub mod elfwrite;
pub mod emitc;
pub mod expand;
pub mod fpa;
pub mod expr;
pub mod layout;
pub mod lift;
pub mod legalize;
pub mod lex;
pub mod listing;
pub mod lower;
pub mod reloc;
pub mod source;
pub mod structure;
pub mod symtab;
pub mod vocab;
pub mod vfp;
