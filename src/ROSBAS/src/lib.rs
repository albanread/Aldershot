//! ROSBAS: a compiler for BBC BASIC V programs.
//!
//! The language is defined by `spec/` and its conformance tests in
//! `tests/spec/`. That language is RISC OS 5.30's BASICVFP, with the
//! differences that the specification lists.
//!
//! The crate reads tokenised programs, loads text programs as BASIC's
//! TEXTLOAD does, and lists them. It also compiles a program to C: it
//! analyses control flow (flow.rs), lowers to an IR (lower.rs, ir.rs) and
//! emits C (cgen.rs). driver.rs runs those steps in order.

pub mod program;
pub mod textload;
pub mod tokens;
pub mod ast;
pub mod parse;
pub mod flow;
pub mod ir;
pub mod lower;
pub mod cgen;
pub mod driver;
