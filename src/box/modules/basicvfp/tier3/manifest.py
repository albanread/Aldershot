"""manifest.py -- the substitutions that build gen/rom_basicvfp_t3.c.

Each entry hands part of the interpreter to tier 3 hand code; the rest
of the file stays the lift, byte for byte, so any behavioural
difference belongs to the hand code.

CASE_SUBS: FACTOR dispatch case number -> hand function suffix, as the
lift's switch spells them (`case N: goto LABEL;`).  Unit 0 is the
experiment of basic-harness/hand/, ported; unit 1 is the
transcendentals (units/factor_trans.c).  The case numbers are the
lift's own, extracted from its switch -- regenerate them when the lift
is regenerated (`grep -o "case [0-9]*: goto LABEL;"`).
"""

CASE_SUBS = {
    # Group A (units/dispat_assign.c): the assignment family -- 16
    # statements plus the whole-array machinery.  LETST is the
    # identifier-start letter range (80 cases); the LVALUE keywords own
    # two tokens each (function form 143-147, statement form 207-211);
    # SWAP is in the escape-token second switch.  The box-build lift
    # spells the LETST/LETSTNOTCACHE cases with a ROS_POLL prefix
    # (substitute.py handles both shapes).
    'LETST': (list(range(37, 42)) + list(range(43, 58)) + [59, 60, 62]
              + list(range(65, 91)) + list(range(92, 123))),
    'LETSTNOTCACHE': [33, 36, 63, 124],
    'ASSIGNAT': 64,
    'LEXT': 162,
    'LLEFTD': 192, 'LMIDD': 193, 'LRIGHTD': 194,
    'LPTR': [143, 207], 'LPAGE': [144, 208], 'LTIME': [145, 209],
    'LLOMEM': [146, 210], 'LHIMEM': [147, 211],
    'LET': 233, 'DIM': 222, 'LOCAL': 234, 'SWAP': 20988,

    # Group B (units/dispat_control.c): HELD after first gating --
    # 56 Rosetta diffs, two signatures: "Call to &FFFFFFF0" (a hand
    # statement's marker R14 reaching an indirect transfer -- suspects
    # GOTO/GOSUB/RUN/CHAIN, whose lift callees GOFACT/POP/POPA/LOADER/
    # RUNNER return THROUGH R14 rather than by checked return) and
    # type/arithmetic errors early in output (suspects IF/WHILE/CASE
    # scanners).  Bisect: enable the 20 CASE_SUBS entries (below,
    # commented) a few at a time, gate, split.
    'ELSE2': 204,
    'NEXT': 237,
    'FOR': 227,
    'REPEAT': 245,
    'UNTIL': 253,
    'ENDWH': 206,
    'WHILE': 21544,
    'STOP': 250,
    'RUN': 249,
    'GOTO': 229,
    'GOSUB': 228,
    'RETURN': 248,
    'END': 224,
    'ENDPR': 225,
    'TRACE': 252,
    'QUIT': 28456,
    'CHAIN': 215,
    'IF': 231,
    'ON': 238,
    'CASE': 15528,

    # Group D (units/dispat_sys.c): the system statements -- 16 of
    # them.  ORGIN/DOMOUSE/SYS/INSTALLBAD/LIBRARY/OVERLAY ride the
    # escape-token second switch (case numbers > 164).  OTHER owns two
    # tokens (127 WHEN and 201 OTHERWISE share one body).  TWOSTMT
    # (the second switch's trunk) stays lift by design: a hand trunk
    # could not reach the still-lift Group C targets.
    'DOSTAR': 42, 'FNRET': 61, 'OTHER': [127, 201],
    'LERROR': 133, 'BBPUT': 213, 'CALL': 214, 'ENVEL': 226,
    'REPORT': 246, 'WIDTH': 254, 'OSCL': 255,
    'ORGIN': 25820, 'DOMOUSE': 25152, 'SYS': 27632,
    'INSTALLBAD': 61076, 'LIBRARY': 27288, 'OVERLAY': 26436,

    # Group C (units/dispat_io.c): the I/O + graphics statements -- 28
    # dispatch cases over 29 labels (CURSOFF's body lives in
    # dispat_control.c, brought to the one-argument CASE_SUBS shape).
    # LINEST is LINE INPUT (its LINE half); the sound/graphics five ride
    # the escape-token second switch.  CURSON has no case (the lift's
    # single reference is a branch) and stays called as a function.
    'LINEST': 134, 'CURSOFF': 135, 'SOUND': 212, 'CLEAR': 216,
    'CLG': 218, 'CLS': 219, 'DRAW': 223, 'GCOL': 230, 'INPUT': 232,
    'MODES': 235, 'MOVE': 236, 'VDU': 239, 'PLOT': 240, 'PRINT': 241,
    'READ': 243, 'RESTORE': 247, 'COLOUR': 251,
    'BEATS': 20684, 'VOICES': 20696, 'VOICE': 20724, 'STEREO': 20784,
    'TEMPO': 20772, 'ELLIPSE': 23756, 'CIRCLE': 23000, 'FILL': 24116,
    'PSET': 25908, 'RECT': 25976, 'DOTINT': 26228, 'WAIT': 26372,

    # unit-factor-core (units/factor_core.c): FACTOR's own constant
    # cases.  TSTN is the digit list (cases 46, 48-57); UNMINS is the
    # unary minus; HEXIN/BININ the & and % literal readers.
    'TSTN': [46] + list(range(48, 58)), 'HEXIN': 38, 'BININ': 37,
    'UNMINS': 45,

    # unit 0: the string and simple numeric built-ins (Factor.s)
    'ABS': 148, 'SGN': 180, 'INT': 168, 'SQR': 182,
    'LEN': 169, 'ASC': 151, 'INSTR': 167,
    'CHRD': 189, 'LEFTD': 192, 'MIDD': 193, 'RIGHTD': 194, 'STRND': 196,
    # unit 1: the transcendentals (Factor.s, fp.s, fp2.s)
    'SIN': 181, 'COS': 155, 'TAN': 183, 'ASN': 152, 'ACS': 149,
    'ATN': 153, 'LN': 170, 'LOG': 171, 'EXP': 161, 'DEG': 157, 'RAD': 178,
}

# Bisect hold: labels listed here fall back to the lift (their dispatch
# cases are dropped from the substitution).  Empty when all is landed.
HELD = ()
for _h in HELD:
    del CASE_SUBS[_h]

# Row 8, stage 1 -- the scaffold drop's conservative leg: lift
# functions deleted from the twin outright.  Every name here is
# PROVABLY unreferenced with the swap trunks still alive (the row-8
# survey, 6 Oct 2026: a comment/string-aware reachability walk over the
# staged twin, rooted at the module's data-driven entries -- the header
# start word &FC100138 = MODULEMAIN, the service entry &FC1000E8 =
# Basic_Services, the *BASICVFP command handler &FC1000CC = Basic_Code
# -- plus basicvfp_register and every basicvfp_ name the hand units so
# much as declare).  They are the FUNCTION_SUBS thunks whose every lift
# caller is itself substituted now (the hand units call the hand
# functions directly), plus CQCCLEAR (MATCH's reset, inside the hand
# machine since 2b).  substitute.py deletes each definition, its
# registration-table line and its forward declaration, and asserts the
# name then appears NOWHERE in the twin -- a reference from code the
# survey believed dead would fail the build, not the tests.
PRUNE = (
    # stage 1 (dead with the trunks whole): the FUNCTION_SUBS thunks
    # whose every lift caller is itself substituted now (the hand units
    # call the hand functions directly), plus CQCCLEAR (MATCH's reset,
    # inside the hand machine since 2b)
    'AUMATCH', 'BIPLIN', 'BIQUER', 'CQCCLEAR', 'LEXTABADR', 'LOOKFL',
    'LOOKP1', 'LOOKP1A', 'SETVAR', 'SPUSHX', 'TOKENADDR',
    # stage 2b (dead once the lift dispatcher shrank to its live
    # regions): thunks whose only callers were the deleted statement
    # bodies -- the survey re-run on the shrunk twin listed exactly
    # these 22
    'AEDONES', 'CHAN', 'DONES', 'EVMATCH', 'FLOATZ', 'FPULL', 'FPUSH',
    'GOTLTCREATE', 'GTARGS', 'INLINE', 'LOOKUP', 'LVNOTCACHE',
    'PULLTYPE', 'PURGECACHE', 'PUSHTYPE', 'RNUL', 'RNULX', 'SETVAL',
    'SPULL', 'SPUSHLARGE', 'STORE', 'VARBYT',
)

# The DISPAT swap (units/dispat_swap.c + .h): the wholesale hand
# replacement of the statement dispatcher -- the case mechanism's
# successor.  ENABLE ON: substitute.py renames the lift's
# basicvfp_DISPAT to basicvfp_DISPAT_lift (body unchanged), puts a
# musttail thunk under the old name (so hand STMT, the lift's DC
# immediate-mode entry and the code-table registration at &FC100BEC
# all reach the hand dispatcher), and writes the compile-time wiring
# gen/dispat_swap_cfg.h that gates the dispatcher's per-group arms on
# the presence of each group's unit file -- coverage is always
# exactly what has landed, and an uncovered token tail-calls the lift
# dispatcher, so the gates hold with any amount of coverage (zero
# included).  DEFAULTED OFF until the integration gates it 7/7 +
# 163/163: with ENABLE off the twin is byte-identical to the
# pre-swap build and units/dispat_swap.c compiles to nothing.
# NOTE: the CASE_SUBS entries above STAY when the swap is on -- the
# lift's internal B DISPAT paths (THENLN's) never pass through the
# hand dispatcher and need the lift's own arms substituted.
DISPAT_SWAP = {
    'ENABLE': True,
    # Row 8, stage 2b: shrink the renamed lift dispatcher to its live
    # regions -- every landed statement's body region inside the trunk
    # is dead code now (the case arms tail-call the hand functions;
    # only the four residual goto arms -- ASS, TWOSTMT, CLOSE, PROC --
    # and the switches' own machinery remain, the structural minimum
    # the row-8 green light named).  substitute.py's shrink_dispat
    # recomputes region liveness on every run (goto targets from live
    # regions, conservative fallthrough) and asserts the surviving
    # label set equals KEEP_LABELS exactly -- a lift that regenerates
    # with different structure fails the build instead of pruning
    # something live.
    'SHRINK': {
        'ENABLE': True,
        'KEEP_LABELS': ('ASS', 'CLOSE', 'DISPAT', 'PROC', 'TWOSTMT'),
    },
    'GROUPS': {                       # cfg define -> units/ file whose
        'A': ('dispat_assign.c', 'BASICVFP_DISPAT_GROUP_A'),   # presence
        'B': ('dispat_control.c', 'BASICVFP_DISPAT_GROUP_B'),  # arms the
        'C': ('dispat_io.c', 'BASICVFP_DISPAT_GROUP_C'),       # group's
        'D': ('dispat_sys.c', 'BASICVFP_DISPAT_GROUP_D'),      # switch
    },
}

# The FACTOR swap (units/expr_factor_rest.c + .h): the evaluator
# trunk's own DISPAT-swap shape, ROW 8 FORM.  ENABLE ON: substitute.py
# puts a musttail thunk under the lift's basicvfp_FACTOR (every caller
# and the code-table registration reach the hand dispatcher) and
# DELETES the lift trunk wholesale -- unit-factor-resid closed the
# residual list (the hand dispatcher's default arm is FACERR, exactly
# the lift's own for an error character), so no path reaches the trunk
# and the green light's condition holds; the deletion asserts the name
# basicvfp_FACTOR_lift appears nowhere afterwards.
FACTOR_SWAP = {
    'ENABLE': True,
}

# Lift functions the hand units call, made non-static in the twin.
# (VFPException_SQRT is not here: the lifter merged it into its
# callers as a goto, so there is no function to export.)
EXPORTS = ('basicvfp_EXPR', 'basicvfp_FACTOR', 'basicvfp_VFPException',
           'basicvfp_ERSYNT', 'basicvfp_ERTYPESTR', 'basicvfp_CHOUT',
           'basicvfp_DONEXT', 'basicvfp_MISTAK', 'basicvfp_CREALP',
           'basicvfp_LVBLNK', 'basicvfp_EXPRRECUR', 'basicvfp_ZDIVOR',
           'basicvfp_ERLONG', 'basicvfp_FC102830', 'basicvfp_EXPRNEXT',
           'basicvfp_EXPRHARD', 'basicvfp_DISPAT', 'basicvfp_CLRSTK',
           'basicvfp_DOEXCEPTION', 'basicvfp_ARLOOKCACHE',
           'basicvfp_ERARRY', 'basicvfp_VARIND', 'basicvfp_STOREA',
           'basicvfp_MSGXLATE', 'basicvfp_MSGERR', 'basicvfp_ESCAPE',
           'basicvfp_CTALLY', 'basicvfp_FC1006C0',
           'basicvfp_FNGOA', 'basicvfp_FNTRC',
           'basicvfp_EXPR', 'basicvfp_ERRQ1', 'basicvfp_RETSTK',
           'basicvfp_AEEXPR', 'basicvfp_AEEXDN', 'basicvfp_EXPRDN',
           'basicvfp_CRAELV', 'basicvfp_MUNGLE', 'basicvfp_GOFACT',
           'basicvfp_FNDLNO', 'basicvfp_ONSKIP', 'basicvfp_POP',
           'basicvfp_POPA', 'basicvfp_RUNNER', 'basicvfp_LOADER',
           'basicvfp_ENDER', 'basicvfp_ENDTRC', 'basicvfp_STORER0MISAL',
           'basicvfp_ERTYPEINT', 'basicvfp_NOLINE', 'basicvfp_ERDEEPNEST',
           'basicvfp_FNFIND', 'basicvfp_FNDEFLIST', 'basicvfp_FNINSTANT',
           'basicvfp_FNMISS', 'basicvfp_VARSTR',
           'basicvfp_OSFILELOADSTRACC',  # FC102830: the lifter's own name for the operator jump table -- regenerate the manifest's exports when the lift is regenerated
           # Group A (dispat_assign): the assignment bodies' callees.
           # INITIALISERAM and READNUM have merged-register signatures
           # (value/pointer parameters), not the plain (s) form --
           # substitute.py's de-static pass matches any parameter list.
           'basicvfp_STOREANINT', 'basicvfp_VARNOTNUM',
           'basicvfp_STSTOR', 'basicvfp_STSTORE',
           'basicvfp_INITIALISERAM', 'basicvfp_ERARRZ',
           'basicvfp_ERTYPENUM', 'basicvfp_FACERR', 'basicvfp_ERCOMM',
           'basicvfp_MISSEQ', 'basicvfp_ERRSUB', 'basicvfp_ARRAYINTDIV',
           'basicvfp_EQAEEX', 'basicvfp_AECHAN', 'basicvfp_MSGPRNXXX',
           'basicvfp_POPLOCALAR', 'basicvfp_READNUM', 'basicvfp_ERBRA',
           # Group D (dispat_sys): the system statements' callees.
           # OSCLIREGS, CRUNCHCHK and CRUNCHROUTINE carry
           # merged-register signatures -- the widened de-static pass
           # matches any parameter list.
           'basicvfp_INTEXA', 'basicvfp_INTEXC', 'basicvfp_WRITEG',
           'basicvfp_NLINE', 'basicvfp_GETARRAYSIZE1',
           'basicvfp_OSFILEINFOSTRACC', 'basicvfp_LIBSUB',
           'basicvfp_PUTBACKHAND', 'basicvfp_CALLARMROUT',
           'basicvfp_ERTYPESTRINGARRAY',
           'basicvfp_OSCLIREGS', 'basicvfp_CRUNCHCHK',
           'basicvfp_CRUNCHROUTINE',
           # Group C (dispat_io): the I/O + graphics statements' callees.
           # CHECKFILL and ZEROX carry merged-register signatures -- the
           # widened de-static pass matches any parameter list.  FCONFP
           # is the LIFT's own (factor_const's hand FCONFP is HELD, so
           # this export calls the lift body, deliberately).
           'basicvfp_CHANNL', 'basicvfp_CHECKFILL', 'basicvfp_DATAIT',
           'basicvfp_DATAST', 'basicvfp_DOPLOT', 'basicvfp_FCONFP',
           'basicvfp_FCONVERT2', 'basicvfp_PRINTS', 'basicvfp_PRSPEC',
           'basicvfp_PRSPEL', 'basicvfp_SPCSWC', 'basicvfp_VALSTR',
           'basicvfp_ZEROX',
           # unit-factor-core: the constant readers' callees.  FREAD is
           # the box-build lift's plain (s) wrapper form.
           'basicvfp_VALCMP', 'basicvfp_FREAD',
           # unit-clrstk: the immediate-mode loop's callees (the
           # report's list; FNDLNO/FREAD/MSGPRNXXX et al already ride
           # the earlier groups).
           'basicvfp_FROMAT', 'basicvfp_GETTWO', 'basicvfp_RENUM1',
           'basicvfp_RENUM2', 'basicvfp_NPRN', 'basicvfp_POSITE',
           'basicvfp_CHOUTNOCOUNT', 'basicvfp_TWINBG',
           'basicvfp_LISTLINE', 'basicvfp_HELPPRN',
           'basicvfp_MSGPRNSSX', 'basicvfp_MSGPRNCCC',
           'basicvfp_INSRT', 'basicvfp_OSFILELOAD',
           'basicvfp_LOADFILEFINAL', 'basicvfp_REMOVE',
           'basicvfp_FC10EF60',
           # unit-modmain: the module start entry's callees (FROMAT,
           # LOADFILEFINAL, FSASET already ride clrstk's list).
           'basicvfp_RDCOMCHER', 'basicvfp_RDCOMCH', 'basicvfp_RDHEX',
           'basicvfp_ENTRYUNK', 'basicvfp_ENTRYHELP',
           'basicvfp_BADIPHEX', 'basicvfp_LOADFILEINCORE',
           # unit-listline: the renderer's callees (NPRN/POSITE/
           # CHOUTNOCOUNT already ride clrstk's list).
           'basicvfp_SPCOUT',
           # unit-expr-factor-rest: the evaluator trunk's callees.
           # The operator ladder's thunks ride FUNCTION_SUBS -- the
           # export only makes the thunk public for the hand dispatch.
           'basicvfp_EXPRADD', 'basicvfp_EXPRSUB', 'basicvfp_EXPRMUL',
           'basicvfp_EXPRDIV', 'basicvfp_EXPRPOW', 'basicvfp_EXPROR',
           'basicvfp_EXPREOR', 'basicvfp_EXPRAND', 'basicvfp_EXPRLT',
           'basicvfp_EXPREQ', 'basicvfp_EXPRGT', 'basicvfp_EXPRINTDIV',
           'basicvfp_EXPRMOD',
           'basicvfp_ERRSB2', 'basicvfp_ERVARAR',
           # unit-factor-resid: the residual built-ins' callees.
           # DORANDOM carries a merged-register signature -- the
           # widened de-static pass matches any parameter list.
           # FCONFP is already exported (Group C's entry, the lift's
           # own body -- factor_const's hand FCONFP stays held).
           'basicvfp_DORANDOM', 'basicvfp_FRNDAA',
           'basicvfp_READARRAYFACTOR', 'basicvfp_READARRAYFACTOR1',
           'basicvfp_GETARRAYSIZE')

# Whole-function substitutions: the lift's definition of each becomes
# a thunk calling basicvfp_hand_<name> with the same signature.
# Units 2a (scan helpers) and 2b (the tokenizer's three entries; the
# machine itself is one hand function).
FUNCTION_SUBS = (
    'SPACES', 'AESPAC', 'DONE', 'DONES', 'AEDONE', 'AEDONES',
    'SPTSTN', 'SPGETN', 'OSSTRI',
    'WORDCQ', 'NUMBCP', 'NUMBCQ', 'CONSTI',
    'MATCH', 'EVMATCH', 'AUMATCH',
    'TOKOUT', 'TOKENADDR', 'LEXTABADR',
    'Basic_Code',
    'FLOATY', 'FLOATZ', 'FLOATQ', 'IFLT',
    'INTEGY', 'INTEGZ', 'INTEGB', 'SFIX',
    'PUSHTYPE', 'SPUSH', 'SPUSHX', 'SPUSHLARGE', 'FPUSH',
    'PULLTYPE', 'SPULL', 'FPULL',
    'LOOKUP', 'LOOKFL', 'LOOKP1', 'LOOKP1A',
    'GOTLTCREATE', 'CREATE', 'AELV',
    'COMPR',
    'DIVOP',
    'EXPRADD', 'EXPRSUB',
    'EXPRMUL', 'EXPRDIV', 'EXPRPOW',
    'EXPROR', 'EXPREOR', 'EXPRAND',
    'EXPRLT', 'EXPRLTOREQ', 'EXPREQ',
    'EXPRGT', 'EXPRGTOREQ',
    'EXPRRSHIFT', 'EXPRRSHIFTLOGICAL',
    'EXPRINTDIV', 'EXPRMOD',
    'STMT', 'CRLINE',
    'LVCONT', 'LVNOTCACHE',
    # LVBLNK the lifter merged; hand_AELV calls it directly
    'BIPLIN', 'BIQUER',
    'STORE', 'SETVAL', 'SETVAR', 'DATA', 'NXT', 'DONEXT', 'DONXTS',
    'FLUSHCACHE', 'PURGECACHE', 'MSG',
    'SETFSA', 'INLINE', 'TITLE', 'ORDERR',
    'FN', 'FNBODY', 'GTARGS', 'FNGOACACHE',
    'FNFIND', 'FNDEFLIST',
    # unit-clrstk: the immediate-mode loop + the seventeen *-commands
    # (Basic.s:619-707 + Command.s) as one whole-function unit.
    'CLRSTK',
    # unit-modmain: the module start entry (Basic.s:23-612) -- the
    # VFP context negotiation, MAIN's workspace, the CALLEDNAME
    # machine.
    'MODULEMAIN',
    # unit-listline: the LIST/printer line renderer (Command.s:608-659)
    'LISTLINE',
    # 3b: Basic_Services, the service call entry (ModHead.s:257; the
    # lift inlined MOVEMEMORY into it -- Service_Memory's relocation)
    'Basic_Services',
    # unit-expr-factor-rest: the evaluator's entry and trunk machinery
    # (Expr.s) with FACTOR's dispatch and its core read paths
    # (Factor.s), plus the Lexical.s evaluator entries the statement
    # bodies share.  FACTOR itself is the FACTOR_SWAP, not a sub.
    'EXPR', 'EXPRRECUR', 'EXPRNEXT', 'EXPRHARD', 'AEEXPR',
    'AECHAN', 'CHAN', 'CHANNL',
    'ARLOOKCACHE',
    'QSTR', 'VARIND', 'VARBYT', 'VARNOTNUM', 'VARSTR',
    'BRA', 'VALSTR', 'VAL0', 'RNUL', 'RNULX', 'DATAST',
    'INTEXA', 'INTEXC', 'EQAEEX', 'AEEXDN', 'EXPRDN',
    # 'FREAD', 'FCONFP': HELD -- the rig's box-build lift has FREAD
    # as a plain (s) wrapper; the harness lift has the 12-param form
    # the unit was written against.  Adapt units/factor_const.c to
    # the box-build lift's bodies before wiring.
    # EXPRNEQUAL and EXPRLSHIFT the lifter merged into jump-table
    # fragments (no function definitions to thunk); they stay lift.
)

# The hand units' headers, included by the twin after its own.
UNITS_HEADERS = ('basicvfp_hand.h', 'factor_trans.h', 'lexical_scan.h',
                 'lexical_match.h', 'lexical_list.h',
                 'modhead_shell.h', 'basic_conv.h', 'basic_stack.h',
                 'basic_lookup.h', 'expr_compr.h', 'expr_divop.h',
                 'expr_addsub.h', 'expr_muldivpow.h', 'expr_bool.h',
                 'basic_stmt.h', 'expr_lv.h', 'basic_organs.h',
                 'funct_call.h', 'funct_args.h', 'funct_ret.h',
                 'funct_find.h', 'factor_const.h', 'dispat_control.h',
                 'dispat_assign.h', 'dispat_sys.h', 'dispat_io.h',
                 'factor_core.h', 'clrstk.h', 'modmain.h', 'listline.h',
                 'basic_services.h',
                 'dispat_swap.h',
                 'expr_factor_rest.h',
                 'factor_resid.h')

# The hand translation units, compiled alongside the twin.
UNITS = ('units/basicvfp_hand.c', 'units/factor_trans.c',
         'units/lexical_scan.c', 'units/lexical_match.c',
         'units/lexical_list.c', 'units/modhead_shell.c',
         'units/basic_conv.c', 'units/funct_find.c', 'units/factor_const.c')
    # NOTE: the rig (vm-verify.sh) appends units/*.c by glob -- this
    # tuple is documentation; keep it in step when adding units.
