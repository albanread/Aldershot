//! The ObjAsm symbol table.
//!
//! ObjAsm has three variable types, and they do not mix. An arithmetic
//! variable declared by `GBLA` can only ever be assigned by `SETA`. The type
//! is fixed when the variable is declared, so assigning a value of another
//! type is an error. The value is not converted.
//!
//! Two namespaces live here:
//!
//! * **variables** — `GBLA`/`GBLL`/`GBLS` (global) and `LCLA`/`LCLL`/`LCLS`
//!   (scoped to one macro expansion). These are the assembly-time variables the
//!   macro engine reads and writes.
//! * **absolutes** — symbols defined by `*` / `EQU`. These are a separate
//!   namespace. A constant named `Foo` and a string variable named `Foo` can
//!   exist together, and the corpus does exactly that.
//!
//! Arithmetic values are **32-bit unsigned**. The assembler manual is explicit:
//! "arithmetic values are unsigned, so the value of `0>-1` is `{FALSE}`". All
//! arithmetic wraps at 32 bits and all comparisons are unsigned.

use std::collections::HashMap;

#[derive(Debug, Clone, PartialEq)]
pub enum Value {
    Arith(u32),
    Logical(bool),
    Str(String),
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Type {
    Arith,
    Logical,
    Str,
}

impl Value {
    pub fn ty(&self) -> Type {
        match self {
            Value::Arith(_) => Type::Arith,
            Value::Logical(_) => Type::Logical,
            Value::Str(_) => Type::Str,
        }
    }
}

impl Type {
    /// The value a freshly declared variable takes.
    pub fn default_value(self) -> Value {
        match self {
            Type::Arith => Value::Arith(0),
            Type::Logical => Value::Logical(false),
            Type::Str => Value::Str(String::new()),
        }
    }

    pub fn name(self) -> &'static str {
        match self {
            Type::Arith => "arithmetic",
            Type::Logical => "logical",
            Type::Str => "string",
        }
    }
}

#[derive(Debug, PartialEq)]
pub enum SymError {
    /// Assigning a value of the wrong type to a declared variable.
    TypeMismatch { name: String, declared: Type, got: Type },
    /// `SETA`/`SETL`/`SETS` on something never declared.
    NotDeclared(String),
    /// `LCL*` outside any macro expansion.
    NoFrame(String),
}

impl std::fmt::Display for SymError {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        match self {
            SymError::TypeMismatch { name, declared, got } => write!(
                f,
                "'{name}' is {} and cannot be assigned a {} value",
                declared.name(),
                got.name()
            ),
            SymError::NotDeclared(n) => write!(f, "'{n}' has not been declared"),
            SymError::NoFrame(n) => {
                write!(f, "'{n}' declared local outside a macro expansion")
            }
        }
    }
}

/// ObjAsm identifiers are case-sensitive, so no folding here. (Directives and
/// mnemonics are not; that is handled where they are recognised.)
#[derive(Default)]
pub struct SymTab {
    globals: HashMap<String, Value>,
    /// One frame per active macro expansion. Innermost last.
    frames: Vec<HashMap<String, Value>>,
    absolutes: HashMap<String, u32>,
}

impl SymTab {
    pub fn new() -> Self {
        Self::default()
    }

    // ---- variables -------------------------------------------------------

    /// `GBLA`/`GBLL`/`GBLS`. Declaring an existing global again **resets**
    /// it to the default value. That is why a header can be read by `GET`
    /// more than once with the same result, and the corpus relies on this
    /// heavily.
    pub fn declare_global(&mut self, name: &str, ty: Type) {
        self.globals.insert(name.to_string(), ty.default_value());
    }

    /// `LCLA`/`LCLL`/`LCLS`. The variable belongs to the innermost macro
    /// expansion. While that frame lasts, it hides a global of the same name.
    pub fn declare_local(&mut self, name: &str, ty: Type) -> Result<(), SymError> {
        match self.frames.last_mut() {
            Some(frame) => {
                frame.insert(name.to_string(), ty.default_value());
                Ok(())
            }
            None => Err(SymError::NoFrame(name.to_string())),
        }
    }

    /// `SETA`/`SETL`/`SETS`. Assigns to the innermost binding of `name`.
    pub fn set(&mut self, name: &str, value: Value) -> Result<(), SymError> {
        let got = value.ty();
        for frame in self.frames.iter_mut().rev() {
            if let Some(slot) = frame.get_mut(name) {
                let declared = slot.ty();
                if declared != got {
                    return Err(SymError::TypeMismatch {
                        name: name.to_string(),
                        declared,
                        got,
                    });
                }
                *slot = value;
                return Ok(());
            }
        }
        match self.globals.get_mut(name) {
            Some(slot) => {
                let declared = slot.ty();
                if declared != got {
                    return Err(SymError::TypeMismatch {
                        name: name.to_string(),
                        declared,
                        got,
                    });
                }
                *slot = value;
                Ok(())
            }
            None => Err(SymError::NotDeclared(name.to_string())),
        }
    }

    pub fn get(&self, name: &str) -> Option<&Value> {
        for frame in self.frames.iter().rev() {
            if let Some(v) = frame.get(name) {
                return Some(v);
            }
        }
        self.globals.get(name)
    }

    // ---- absolutes (`*` / EQU) ------------------------------------------

    pub fn define_absolute(&mut self, name: &str, value: u32) {
        self.absolutes.insert(name.to_string(), value);
    }

    pub fn absolute(&self, name: &str) -> Option<u32> {
        self.absolutes.get(name).copied()
    }

    /// Every absolute: the source's numeric constants, by name.
    pub fn absolutes(&self) -> &HashMap<String, u32> {
        &self.absolutes
    }

    /// Backing for `:DEF:`, which is true for either namespace.
    pub fn is_defined(&self, name: &str) -> bool {
        self.get(name).is_some() || self.absolutes.contains_key(name)
    }

    /// The assembly-time variables, for a caller that needs to put them back.
    pub fn variables(&self) -> HashMap<String, Value> {
        self.globals.clone()
    }

    /// Replace the assembly-time variables wholesale.
    ///
    /// A second pass runs every `GBLA`/`GBLL`/`GBLS` again, so it must not
    /// inherit the first pass's declarations. Otherwise a header guarded by
    /// `[ :LNOT: :DEF: Included_Hdr_Foo ]` would find the guard already set
    /// and skip itself, and its macro definitions would be lost with it.
    /// Absolutes are left alone, because resolving them is what the second
    /// pass is for.
    pub fn set_variables(&mut self, v: HashMap<String, Value>) {
        self.globals = v;
    }

    // ---- macro frames ----------------------------------------------------

    pub fn push_frame(&mut self) {
        self.frames.push(HashMap::new());
    }

    pub fn pop_frame(&mut self) {
        self.frames.pop();
    }

    pub fn depth(&self) -> usize {
        self.frames.len()
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn globals_declare_with_type_defaults() {
        let mut s = SymTab::new();
        s.declare_global("a", Type::Arith);
        s.declare_global("l", Type::Logical);
        s.declare_global("t", Type::Str);
        assert_eq!(s.get("a"), Some(&Value::Arith(0)));
        assert_eq!(s.get("l"), Some(&Value::Logical(false)));
        assert_eq!(s.get("t"), Some(&Value::Str(String::new())));
    }

    #[test]
    fn assignment_must_match_declared_type() {
        let mut s = SymTab::new();
        s.declare_global("n", Type::Arith);
        assert!(s.set("n", Value::Arith(42)).is_ok());
        let err = s.set("n", Value::Str("x".into())).unwrap_err();
        assert_eq!(
            err,
            SymError::TypeMismatch {
                name: "n".into(),
                declared: Type::Arith,
                got: Type::Str
            }
        );
    }

    #[test]
    fn assigning_an_undeclared_variable_is_an_error() {
        let mut s = SymTab::new();
        assert_eq!(
            s.set("nope", Value::Arith(1)).unwrap_err(),
            SymError::NotDeclared("nope".into())
        );
    }

    #[test]
    fn redeclaring_a_global_resets_it() {
        // Repeated GET of a header must be idempotent.
        let mut s = SymTab::new();
        s.declare_global("v", Type::Arith);
        s.set("v", Value::Arith(99)).unwrap();
        s.declare_global("v", Type::Arith);
        assert_eq!(s.get("v"), Some(&Value::Arith(0)));
    }

    #[test]
    fn variables_can_be_restored_without_touching_absolutes() {
        let mut t = SymTab::new();
        t.declare_global("Predefined", Type::Logical);
        let before = t.variables();
        // What a pass would add.
        t.declare_global("Included_Hdr_Foo", Type::Logical);
        t.define_absolute("SomeLabel", 0x100);
        assert!(t.is_defined("Included_Hdr_Foo"));

        t.set_variables(before);
        assert!(!t.is_defined("Included_Hdr_Foo"), "the guard must be gone");
        assert!(t.is_defined("Predefined"), "the caller's variables stay");
        assert_eq!(t.absolute("SomeLabel"), Some(0x100), "absolutes survive");
    }

    #[test]
    fn locals_shadow_globals_and_pop_cleanly() {
        let mut s = SymTab::new();
        s.declare_global("x", Type::Arith);
        s.set("x", Value::Arith(1)).unwrap();

        s.push_frame();
        s.declare_local("x", Type::Arith).unwrap();
        s.set("x", Value::Arith(2)).unwrap();
        assert_eq!(s.get("x"), Some(&Value::Arith(2)));

        s.pop_frame();
        assert_eq!(s.get("x"), Some(&Value::Arith(1)), "global must resurface");
    }

    #[test]
    fn nested_frames_resolve_innermost_first() {
        let mut s = SymTab::new();
        s.push_frame();
        s.declare_local("v", Type::Arith).unwrap();
        s.set("v", Value::Arith(1)).unwrap();
        s.push_frame();
        s.declare_local("v", Type::Arith).unwrap();
        s.set("v", Value::Arith(2)).unwrap();
        assert_eq!(s.get("v"), Some(&Value::Arith(2)));
        s.pop_frame();
        assert_eq!(s.get("v"), Some(&Value::Arith(1)));
    }

    #[test]
    fn setting_through_a_frame_hits_the_global_when_not_shadowed() {
        let mut s = SymTab::new();
        s.declare_global("g", Type::Arith);
        s.push_frame();
        s.set("g", Value::Arith(7)).unwrap();
        s.pop_frame();
        assert_eq!(s.get("g"), Some(&Value::Arith(7)));
    }

    #[test]
    fn local_outside_a_macro_is_an_error() {
        let mut s = SymTab::new();
        assert_eq!(
            s.declare_local("x", Type::Arith).unwrap_err(),
            SymError::NoFrame("x".into())
        );
    }

    #[test]
    fn absolutes_are_a_separate_namespace() {
        let mut s = SymTab::new();
        s.declare_global("Foo", Type::Str);
        s.define_absolute("Foo", 0x1000);
        assert_eq!(s.get("Foo"), Some(&Value::Str(String::new())));
        assert_eq!(s.absolute("Foo"), Some(0x1000));
    }

    #[test]
    fn is_defined_covers_both_namespaces() {
        let mut s = SymTab::new();
        assert!(!s.is_defined("a"));
        s.declare_global("a", Type::Arith);
        assert!(s.is_defined("a"));
        assert!(!s.is_defined("b"));
        s.define_absolute("b", 1);
        assert!(s.is_defined("b"));
    }
}
