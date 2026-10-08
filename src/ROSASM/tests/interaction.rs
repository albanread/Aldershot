//! Cross-stage tests, driven through the public API.
//!
//! These test the points where one stage hands work to another, rather than
//! any one stage alone: macro expansion feeding the evaluator, the
//! environment feeding conditionals, and layout feeding both. The unit tests
//! in each module cover the stages themselves.

use std::collections::HashMap;

use rosasm::expand::{Expander, MapResolver};

fn run_env(pds: &[&str], src: &str) -> Vec<String> {
    let lines: Vec<String> = src.lines().map(|s| s.to_string()).collect();
    let r = MapResolver(HashMap::new());
    let mut e = Expander::new(&r);
    e.set_target_builtins();
    for pd in pds {
        e.predefine(pd).expect("predefine should parse");
    }
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

fn run(src: &str) -> Vec<String> {
    run_env(&[], src)
}

/// The 32-bit words a source emits, in order, read from the bytes that data
/// directives evaluate into. The source text itself is left as written, as
/// in ObjAsm's own listing.
fn words(src: &str) -> Vec<u32> {
    let lines: Vec<String> = src.lines().map(|s| s.to_string()).collect();
    let r = MapResolver(HashMap::new());
    let mut e = Expander::new(&r);
    e.set_target_builtins();
    e.run("test", lines)
        .expect("expansion")
        .into_iter()
        .filter(|l| !l.listing_only)
        .flat_map(|l| {
            l.bytes
                .chunks(4)
                .filter(|c| c.len() == 4)
                .map(|c| u32::from_le_bytes([c[0], c[1], c[2], c[3]]))
                .collect::<Vec<_>>()
        })
        .collect()
}

// ---------------------------------------------------------------- environment

#[test]
fn predefines_drive_conditionals_the_way_pd_does() {
    let src = "        [ Machine = \"RPi\"\n        DCB 1\n        |\n        DCB 2\n        ]\n";
    assert_eq!(run_env(&["Machine SETS \"RPi\""], src), vec!["DCB 1"]);
    assert_eq!(run_env(&["Machine SETS \"IOMD\""], src), vec!["DCB 2"]);
}

#[test]
fn target_builtins_are_readable() {
    assert_eq!(words("        DCD {CONFIG}
"), vec![32]);
}

#[test]
fn a_predefine_must_use_a_set_directive() {
    let r = MapResolver(HashMap::new());
    let mut e = Expander::new(&r);
    assert!(e.predefine("Foo = 1").is_err());
    assert!(e.predefine("Foo SETA 1").is_ok());
}

#[test]
fn register_names_resolve_to_their_numbers() {
    // The manual notes ObjAsm "still permits register names in expressions
    // (they are automatically converted to the register number)".
    assert_eq!(words("        DCD R3, sp, lr, pc
"), vec![3, 13, 14, 15]);
}

#[test]
fn rn_names_a_register_and_the_name_is_then_usable() {
    let src = "Rx      RN      R7\n        DCD Rx\n        ASSERT Rx <> R8\n";
    assert_eq!(words(src), vec![7]);
}

// -------------------------------------------------- macro/evaluator seams

#[test]
fn a_macro_parameter_can_supply_an_operator() {
    let src = concat!(
        "        MACRO\n",
        "        Combine $op\n",
        "        DCD 12 $op 10\n",
        "        MEND\n",
        "        Combine :AND:\n",
        "        Combine :OR:\n"
    );
    assert_eq!(words(src), vec![8, 14]);
}

#[test]
fn a_macro_parameter_can_supply_a_whole_opcode() {
    // Substitution is textual and happens before the line is parsed, so a
    // parameter may expand to the opcode field itself.
    let src = concat!(
        "        MACRO\n",
        "        Emit $what\n",
        "        $what 1\n",
        "        MEND\n",
        "        Emit DCD\n",
        "        Emit DCB\n"
    );
    assert_eq!(run(src), vec!["DCD 1", "DCB 1"]);
}

#[test]
fn string_variables_build_symbol_names() {
    // The pattern behind TerritoryNum_$Territory in the corpus.
    let src = concat!(
        "        GBLS    T\n",
        "T       SETS    \"Two\"\n",
        "Num_Two *       22\n",
        "        DCD     Num_$T\n"
    );
    assert_eq!(words(src), vec![22]);
}

#[test]
fn nested_macros_each_get_their_own_locals() {
    let src = concat!(
        "        MACRO\n",
        "        Inner\n",
        "        LCLA v\n",
        "v       SETA 2\n",
        "        DCD $v\n",
        "        MEND\n",
        "        MACRO\n",
        "        Outer\n",
        "        LCLA v\n",
        "v       SETA 1\n",
        "        Inner\n",
        "        DCD $v\n",
        "        MEND\n",
        "        Outer\n"
    );
    // The inner macro's locals must not overwrite the outer macro's.
    assert_eq!(words(src), vec![2, 1]);
}

#[test]
fn while_inside_a_macro_uses_the_macros_own_counter() {
    let src = concat!(
        "        MACRO\n",
        "        Table $n\n",
        "        LCLA i\n",
        "i       SETA 0\n",
        "        WHILE i < $n\n",
        "        DCD $i\n",
        "i       SETA i + 1\n",
        "        WEND\n",
        "        MEND\n",
        "        Table 3\n"
    );
    assert_eq!(words(src), vec![0, 1, 2]);
}

#[test]
fn a_macro_may_open_a_conditional_its_caller_closes() {
    // The manual: "The IF construction can be used inside macro expansions as
    // easily as it is used in the main program." A conditional need not nest
    // inside an expansion. It may open inside a macro and close after it.
    let src = concat!(
        "        MACRO\n",
        "        OpenIf\n",
        "        [ {TRUE}\n",
        "        MEND\n",
        "        OpenIf\n",
        "        DCB 1\n",
        "        ]\n"
    );
    assert_eq!(run(src), vec!["DCB 1"]);
}

#[test]
fn equ_inside_a_macro_is_visible_afterwards() {
    let src = concat!(
        "        MACRO\n",
        "        Def $name, $val\n",
        "$name   *       $val\n",
        "        MEND\n",
        "        Def Thing, 9\n",
        "        DCD Thing\n"
    );
    assert_eq!(words(src), vec![9]);
}

// ------------------------------------------- evaluator corners via expansion

#[test]
fn arithmetic_substitution_round_trips_through_hex() {
    // `$n` renders as eight hex digits and `&` reads them back. Any other
    // rendering would silently change the constant.
    let src = "        GBLA n\nn       SETA &ABCD\n        DCD &$n\n";
    assert_eq!(words(src), vec![0xABCD]);
}

#[test]
fn a_string_variable_can_carry_a_whole_expression() {
    let src = concat!(
        "        GBLS    E\n",
        "E       SETS    \"1 :SHL: 8\"\n",
        "        DCD     $E\n"
    );
    assert_eq!(words(src), vec![256]);
}

#[test]
fn logical_substitution_is_t_or_f() {
    let src = concat!(
        "        GBLL    L\n",
        "L       SETL    {TRUE}\n",
        "        [ \"$L\" = \"T\"\n",
        "        DCB 1\n",
        "        ]\n"
    );
    assert_eq!(run(src), vec!["DCB 1"]);
}

#[test]
fn division_by_zero_in_a_skipped_branch_is_harmless() {
    // Lines in a skipped branch are only scanned, not evaluated, so the
    // division is never done.
    assert!(run("        [ {FALSE}\n        DCD 1 / 0\n        ]\n").is_empty());
}

#[test]
fn deeply_nested_conditionals_close_correctly() {
    let mut src = String::new();
    for _ in 0..50 {
        src.push_str("        [ {TRUE}\n");
    }
    src.push_str("        DCB 1\n");
    for _ in 0..50 {
        src.push_str("        ]\n");
    }
    assert_eq!(run(&src), vec!["DCB 1"]);
}

#[test]
fn recursive_macro_invocation_terminates_on_its_guard() {
    let src = concat!(
        "        GBLA    d\n",
        "d       SETA    0\n",
        "        MACRO\n",
        "        Recurse\n",
        "d       SETA    d + 1\n",
        "        DCB     1\n",
        "        [ d < 4\n",
        "        Recurse\n",
        "        ]\n",
        "        MEND\n",
        "        Recurse\n"
    );
    assert_eq!(run(src).len(), 4);
}

#[test]
fn a_get_chain_deeper_than_the_limit_is_reported_not_hung() {
    // A header that includes itself must terminate with a diagnostic.
    let mut m = HashMap::new();
    m.insert("loop".to_string(), vec!["        GET loop".to_string()]);
    let r = MapResolver(m);
    let mut e = Expander::new(&r);
    let err = e
        .run("test", vec!["        GET loop".to_string()])
        .expect_err("an include cycle must be an error");
    assert!(err.msg.contains("nested too deeply"), "{err}");
}

// ------------------------------------------- rules checked against ObjAsm
//
// These were checked against ObjAsm 4.08 running under the headless RPCEmu
// (tools/roshell.py). Where the manual was ambiguous, ObjAsm's behaviour was
// taken as the rule.

#[test]
fn quotes_round_a_macro_argument_are_stripped() {
    // ObjAsm: for `Probe $s`, both `Probe abc` and `Probe "abc"` print
    // arg=[abc]. The quotes protect commas at the call site and are removed.
    let src = concat!(
        "        MACRO\n",
        "        Probe $s\n",
        "        DCB \"[$s]\"\n",
        "        MEND\n",
        "        Probe abc\n",
        "        Probe \"abc\"\n",
        "        Probe\n"
    );
    assert_eq!(run(src), vec!["DCB \"[abc]\"", "DCB \"[abc]\"", "DCB \"[]\""]);
}

#[test]
fn a_quoted_argument_keeps_its_commas_as_one_argument() {
    // ObjAsm: `Comma "x,y", z` gives a=[x,y] b=[z].
    let src = concat!(
        "        MACRO\n",
        "        Comma $a, $b\n",
        "        DCB \"[$a][$b]\"\n",
        "        MEND\n",
        "        Comma \"x,y\", z\n"
    );
    assert_eq!(run(src), vec!["DCB \"[x,y][z]\""]);
}

#[test]
fn the_corpus_conditional_pattern_works() {
    // `[ "$str" <> ""` appears throughout the corpus and only works because
    // the argument's own quotes have been stripped.
    let src = concat!(
        "        MACRO\n",
        "        TLINE $str\n",
        "      [ \"$str\" <> \"\"\n",
        "        DCB 1\n",
        "      |\n",
        "        DCB 2\n",
        "      ]\n",
        "        MEND\n",
        "        TLINE \"hello\"\n",
        "        TLINE\n"
    );
    assert_eq!(run(src), vec!["DCB 1", "DCB 2"]);
}

// ------------------------------------------------- two-pass resolution
//
// ObjAsm resolves symbols over two passes, so a definition may follow its
// first use. `Hdr:HighFSI` relies on this: it sets `OSFind_OpenIn * open_read`
// at line 299 and defines `open_read` at line 310.

#[test]
fn an_equ_may_refer_to_a_symbol_defined_later() {
    let src = concat!(
        "First   *       Later\n",
        "Later   *       7\n",
        "        DCD     First\n"
    );
    assert_eq!(words(src), vec![7]);
}

#[test]
fn forward_references_may_chain() {
    let src = concat!(
        "A       *       B\n",
        "B       *       C\n",
        "C       *       3\n",
        "        DCD     A, B, C\n"
    );
    assert_eq!(words(src), vec![3, 3, 3]);
}

#[test]
fn a_genuinely_undefined_symbol_is_still_an_error() {
    let lines: Vec<String> = "X       *       NeverDefined\n"
        .lines()
        .map(|s| s.to_string())
        .collect();
    let r = MapResolver(HashMap::new());
    let mut e = Expander::new(&r);
    let err = e.run("test", lines).expect_err("must not resolve");
    assert!(err.msg.contains("cannot be resolved"), "{err}");
}

#[test]
fn an_assertion_may_refer_to_a_symbol_defined_later() {
    // The manual makes an assertion a second-pass diagnostic, so it may name
    // a symbol the file defines further down.
    let src = concat!(
        "        ASSERT  Size = 8\n",
        "Size    *       8\n",
        "        DCD     Size\n"
    );
    assert_eq!(words(src), vec![8]);
}

#[test]
fn a_failing_assertion_still_reports_after_settling() {
    let lines: Vec<String> = "        ASSERT Size = 9\nSize    *       8\n"
        .lines()
        .map(|s| s.to_string())
        .collect();
    let r = MapResolver(HashMap::new());
    let mut e = Expander::new(&r);
    let err = e.run("test", lines).expect_err("must fail");
    assert!(err.msg.contains("assertion failed"), "{err}");
}

// ---------------------------------------------------------------- FPA layout

/// Where a label lands after FPA compares, with and without conversion to
/// VFP. As FPA, every instruction is one word. Converted, a compare is two
/// words (VCMP, then VMRS). Counting two words in both cases once moved every
/// later label, and with it every ADR, DCD and literal load that named one.
fn label_after_compares(to_vfp: bool) -> u32 {
    let src = "        AREA    Code, CODE\n        CMF     f0, #0\n        CNFE    f1, f2\nAfter   MOV     r0, r0\n";
    let lines: Vec<String> = src.lines().map(|s| s.to_string()).collect();
    let r = MapResolver(HashMap::new());
    let mut e = Expander::new(&r);
    e.set_target_builtins();
    e.set_fpa_to_vfp(to_vfp);
    e.run("test", lines).expect("expansion");
    e.label_defs()["After"].1
}

#[test]
fn an_fpa_compare_is_one_word_unless_converted() {
    assert_eq!(label_after_compares(false), 8);
    assert_eq!(label_after_compares(true), 16);
}
