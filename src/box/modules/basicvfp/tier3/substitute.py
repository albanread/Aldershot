#!/usr/bin/env python3
"""substitute.py -- build the tier 3 twin of the machine translation.

Applies manifest.py to basic-harness/gen/rom_basicvfp.c, writing
basic-harness/gen/rom_basicvfp_t3.c: the lift with the tier 3 units'
code substituted at their labels, the evaluators the units call
exported, and the units' header included.

A dispatch case `case N: goto LABEL;` becomes a call that hands in the
live return address (r14, exactly what `STR R14,[SP,#-4]!` would have
pushed) and returns once the hand function has set R15, as
`LDR PC,[SP],#4` would have.  The case stores the caller's dirty
locals first (R10, R11, R13) -- the original's BL prologue did that
implicitly, and the hand code reads its state in R[].

Optionally (manifest.DISPAT_SWAP, defaulted OFF) the statement
dispatcher is swapped wholesale: the lift's basicvfp_DISPAT is renamed
basicvfp_DISPAT_lift and a musttail thunk under the old name hands the
token switch to units/dispat_swap.c's hand dispatcher, which covers
every token with a landed hand statement and tail-calls the renamed
lift for the rest -- so the swap is safe at any coverage, zero
included.  The swap's compile-time wiring is written to
gen/dispat_swap_cfg.h on every run (units/dispat_swap.c reads it
through the gen include path).

Every tier-copied occurrence of a pattern is replaced.  A pattern that
matches nothing is an error, never a silent pass: a missed tier-copy
would pass tests until a program reached the unpatched copy.
"""
import glob
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import manifest

ROOT = os.environ.get('BASIC_T3_ROOT',
                      os.path.join(HERE, '..', 'basic-harness'))
SRC = os.path.join(ROOT, 'gen', 'rom_basicvfp.c')
DST = os.path.join(ROOT, 'gen', 'rom_basicvfp_t3.c')


def replace_function(src, name):
    """Rewrite the lift's definition of basicvfp_<name> as a thunk that
    calls basicvfp_hand_<name> with the same parameters -- the whole-
    function substitution, for units that are not dispatch cases.
    Returns (src, 1); exits if the definition is not found.  The
    definition may already be public (the box-build lift declares EXPR
    so; an exported name's thunk is public too)."""
    pat = r'\n(?:static )?void basicvfp_%s\(([^)]*)\)\n\{' % name
    ms = list(re.finditer(pat, src))
    if len(ms) != 1:
        sys.exit(f'substitute.py: function definition of {name} found '
                 f'{len(ms)} times (want exactly once)')
    m = ms[0]
    params = m.group(1)
    args = []
    for p in params.split(','):
        p = p.strip()
        args.append('s' if p.startswith('struct ros_cpu')
                    else p.split()[-1].lstrip('*'))
    # the body's extent on the comment/string-blanked text: the lift's
    # ARM comments carry braces ({R0-R12}), and a raw counter can
    # mis-span (Review, 6 Oct; the row-8 survey's lesson)
    stripped = strip_comments_strings(src)
    i = src.index('{', m.start())
    depth = 0
    for j in range(i, len(stripped)):
        if stripped[j] == '{':
            depth += 1
        elif stripped[j] == '}':
            depth -= 1
            if depth == 0:
                break
    else:
        sys.exit(f'substitute.py: unbalanced body: {name}')
    # a name that is also exported must stay callable from outside
    # the translation unit's static linkage: its thunk loses static
    kind = 'void' if name in getattr(manifest, 'EXPORTS', ()) else 'static void'
    thunk = ('\n%s basicvfp_%s(%s)\n{\n    basicvfp_hand_%s(%s);\n}'
             % (kind, name, params, name, ', '.join(args)))
    return src[:m.start() + 1] + thunk + src[j + 1:]


def strip_comments_strings(src):
    """src with comments and string/char literals blanked (newlines
    kept), so brace and paren matching on the result is faithful to C
    structure -- the lift's ARM-source comments contain braces
    ({R0-R12}), and a naive counter mis-spans functions."""
    out = list(src)
    i, n = 0, len(src)
    while i < n:
        c = src[i]
        if c == '/' and i + 1 < n and src[i + 1] == '*':
            j = src.find('*/', i + 2)
            j = n if j < 0 else j + 2
        elif c == '/' and i + 1 < n and src[i + 1] == '/':
            j = src.find('\n', i)
            j = n if j < 0 else j
        elif c in '"\'':
            q = c
            j = i + 1
            while j < n:
                if src[j] == '\\':
                    j += 2
                    continue
                if src[j] == q:
                    break
                j += 1
            j = min(j + 1, n)
        else:
            i += 1
            continue
        for k in range(i, j):
            if out[k] != '\n':
                out[k] = ' '
        i = j
    return ''.join(out)


def delete_function(src, full):
    """Delete ONE definition of basicvfp_<full> (the body-found one),
    its forward declaration if present, and its registration-table
    line if present; banner comments above the definition go too.
    Every step is asserted; returns (src, lines_deleted)."""
    stripped = strip_comments_strings(src)
    defnm = None
    for m in re.finditer(r'\n(?:static )?void %s\(' % re.escape(full),
                         stripped):
        i = stripped.index('(', m.start())
        depth = 0
        for j in range(i, len(stripped)):
            if stripped[j] == '(':
                depth += 1
            elif stripped[j] == ')':
                depth -= 1
                if depth == 0:
                    break
        k = j + 1
        while k < len(stripped) and stripped[k] in ' \t\n':
            k += 1
        if k < len(stripped) and stripped[k] == '{':
            defnm = (m, k)
            break
    if defnm is None:
        sys.exit('substitute.py: delete_function: definition not found: %s'
                 % full)
    m, k = defnm
    depth = 0
    for q in range(k, len(stripped)):
        if stripped[q] == '{':
            depth += 1
        elif stripped[q] == '}':
            depth -= 1
            if depth == 0:
                break
    # start: the line the definition begins on, walked back over a
    # /* ---- ... */ banner that mentions the name (and blank lines)
    start = m.start() + 1
    while src[:start].rstrip().endswith('*/'):
        b = src.rfind('/*', 0, start)
        if b < 0 or full not in src[b:start]:
            break
        start = b
        while src[:start].endswith('\n\n'):
            start -= 1
    # end: past the definition and the blank lines after it
    end = q + 1
    while end < len(src) and src[end] == '\n':
        end += 1
    deleted = src.count('\n', start, end)
    src = src[:start] + src[end:]
    # the forward declaration, if the file carries one
    decl = re.search(r'\n(?:static )?void %s\(([^)]*)\);\n'
                     % re.escape(full), src)
    if decl:
        src = src[:decl.start() + 1] + src[decl.end():]
    # the registration-table line
    table = re.compile(
        r'\n    \{ 0x[0-9A-Fa-f]+u, %s, "basicvfp:[^"]*" \},'
        % re.escape(full))
    ms = table.findall(src)
    if len(ms) > 1:
        sys.exit('substitute.py: delete_function: %s registered more than '
                 'once' % full)
    if ms:
        src = table.sub('', src, count=1)
    return src, deleted


def _last_statement(text):
    """The last top-level statement of a stripped region: statements
    end at a depth-0 ';' or at the '}' closing a depth-1 block."""
    t = text.strip()
    if not t:
        return ''
    cuts = [0]
    depth = 0
    for i, c in enumerate(t):
        if c in '{(':
            depth += 1
        elif c in '})':
            depth -= 1
            if depth == 0 and c == '}':
                cuts.append(i + 1)
        elif c == ';' and depth == 0:
            cuts.append(i + 1)
    if cuts[-1] >= len(t):
        last = t[cuts[-2]:] if len(cuts) >= 2 else t
    else:
        last = t[cuts[-1]:]
    # a leading label is not part of the statement
    last = re.sub(r'^(?:L_[A-Za-z0-9_]+|[A-Za-z_][A-Za-z0-9_]*)'
                  r'(?::\s*)+', '', last).strip()
    return last


def _terminates(text, depth=0):
    """True when control cannot leave the end of this stripped text
    (fall into whatever follows).  Conservative: anything not provably
    terminating counts as falling through."""
    last = _last_statement(text)
    if not last:
        return False
    if last.startswith('goto '):
        return True
    if last.startswith('return'):
        return True
    if last in ('break;', 'continue;'):
        return True
    if 'ROS_TAIL_CALL(' in last and not last.startswith(('if', 'switch')):
        return True                     # musttail: a returned call
    # (an `if (c) { ... ROS_TAIL_CALL(f) }` with no else FALLS THROUGH
    # when c is false: it goes to the if/else rule below -- Review, 6 Oct)
    if last.startswith('switch') and 'break' not in last:
        arms = re.findall(r'(case\b|default:)', last)
        if arms and arms[-1] == 'default:' \
                and re.search(r'default:\s*ros_fault\(', last):
            return True
    if last.startswith('if') and depth < 8:
        # if (...) A else B -- both branches must terminate
        i = last.index('(')
        d = 0
        for j in range(i, len(last)):
            if last[j] == '(':
                d += 1
            elif last[j] == ')':
                d -= 1
                if d == 0:
                    break
        rest = last[j + 1:].strip()
        d = 0
        then_end = None
        for k, c in enumerate(rest):
            if c in '{(':
                d += 1
            elif c in '})':
                d -= 1
                if d == 0:
                    then_end = k + 1
                    break
            elif c == ';' and d == 0:
                then_end = k + 1
                break
        if then_end is None:
            return False
        then = rest[:then_end]
        tail = rest[then_end:].strip()
        if not tail.startswith('else'):
            return False
        els = tail[4:].strip()
        return _terminates(then.strip('{} '), depth + 1) and \
            _terminates(els.strip('{} '), depth + 1)
    return False


def shrink_dispat(src, expect_labels):
    """Row 8, stage 2b: shrink the renamed lift dispatcher to its live
    regions -- the entry, the token switch, the TWOSTMT trunk and the
    residual statement bodies (ASS, CLOSE, PROC; TWOSTMT itself).  A
    label's region is LIVE when a case arm or a goto from a live
    region targets it, or when a live region falls through into it
    (the fallthrough test is conservative: only goto/return/break/
    continue/musttail, a default-faulting breakless switch, or an
    if/else whose branches both terminate, count as stopping).  The
    surviving label set is asserted against expect_labels -- a lift
    that regenerates with different structure fails the build instead
    of pruning something live.  Returns (src, lines_deleted)."""
    stripped = strip_comments_strings(src)
    defnm = None
    for m in re.finditer(r'\nvoid basicvfp_DISPAT_lift\(', stripped):
        i = stripped.index('(', m.start())
        d = 0
        for j in range(i, len(stripped)):
            if stripped[j] == '(':
                d += 1
            elif stripped[j] == ')':
                d -= 1
                if d == 0:
                    break
        k = j + 1
        while k < len(stripped) and stripped[k] in ' \t\n':
            k += 1
        if stripped[k] == '{':
            defnm = (m, k)
            break
    if defnm is None:
        sys.exit('substitute.py: shrink: basicvfp_DISPAT_lift not found')
    m, k = defnm
    depth = 0
    for q in range(k, len(stripped)):
        if stripped[q] == '{':
            depth += 1
        elif stripped[q] == '}':
            depth -= 1
            if depth == 0:
                break
    a = stripped.count('\n', 0, m.start()) + 1
    b = stripped.count('\n', 0, q) + 1
    # the prologue begins after the body's opening brace (the '{' at k
    # sits on the line after the signature)
    a = stripped.count('\n', 0, k) + 2

    # regions: prologue then label..next-label
    labre = re.compile(r'^(L_[A-Za-z0-9_]+|[A-Z][A-Z0-9_]*):', re.M)
    labels = []
    for mm in labre.finditer(stripped, m.start(), q):
        labels.append((stripped.count('\n', 0, mm.start()) + 1, mm.group(1)))

    bounds = [a] + [ln for ln, _ in labels] + [b + 1]
    regions = [(a, (labels[0][0] - 1) if labels else b - 1, None)]
    for idx, (ln, nm) in enumerate(labels):
        if idx + 1 < len(labels):
            end = labels[idx + 1][0] - 1
        else:
            end = b - 1          # the closing brace's line always stays
        regions.append((ln, end, nm))

    bylabel = {nm: i for i, (s, e, nm) in enumerate(regions) if nm}

    live = {0}
    casegoto = re.compile(r'case\s+\d+:\s*(?:\{[^}]*\})?\s*goto\s+(\w+);')
    for mm in casegoto.finditer(stripped, m.start(), q):
        i = bylabel.get(mm.group(1))
        if i is None:
            sys.exit('substitute.py: shrink: case goto target is not a '
                     'top-level label: %s' % mm.group(1))
        live.add(i)

    changed = True
    while changed:
        changed = False
        for i, (s, e, nm) in enumerate(regions):
            if i not in live:
                continue
            txt = "\n".join(stripped.split('\n')[s - 1:e])
            for g in re.findall(r'\bgoto\s+([A-Za-z_][A-Za-z0-9_]*)', txt):
                t = bylabel.get(g)
                if t is not None and t not in live:
                    live.add(t)
                    changed = True
            if not _terminates(txt) and i + 1 < len(regions) \
                    and (i + 1) not in live:
                live.add(i + 1)
                changed = True

    live_labels = sorted(regions[i][2] for i in live if regions[i][2])
    if live_labels != sorted(expect_labels):
        sys.exit('substitute.py: shrink: the trunk\'s surviving labels '
                 'changed: %s (expected %s) -- the lift regenerated; '
                 're-survey before pruning' % (live_labels,
                                               sorted(expect_labels)))
    dead = [i for i in range(len(regions)) if i not in live]
    deleted = 0
    lines_src = src.split('\n')
    for i in sorted(dead, reverse=True):
        s, e, nm = regions[i]
        deleted += e - s + 1
        del lines_src[s - 1:e]
    return "\n".join(lines_src), deleted


def prune_functions(src, names):
    """Row 8: delete the provably-unreferenced lift functions (see
    manifest.PRUNE for the evidence).  Every deletion is asserted;
    afterwards no pruned name may appear anywhere in the twin -- a
    reference from code the survey believed dead would fail the build,
    not the tests.  Returns (src, lines_deleted)."""
    deleted = 0
    for name in names:
        full = 'basicvfp_' + name
        # the name must be defined exactly once (delete_function takes
        # the first body-found match; a second would mean the lift
        # changed shape)
        stripped = strip_comments_strings(src)
        ndefs = 0
        for mm in re.finditer(r'\n(?:static )?void %s\('
                              % re.escape(full), stripped):
            kk = stripped.index(')', mm.start()) + 1
            while kk < len(stripped) and stripped[kk] in ' \t\n':
                kk += 1
            if kk < len(stripped) and stripped[kk] == '{':
                ndefs += 1
        if ndefs != 1:
            sys.exit('substitute.py: prune: %s not defined exactly once'
                     ' (%d definitions)' % (full, ndefs))
        src, n = delete_function(src, full)
        deleted += n
    for name in names:
        full = 'basicvfp_' + name
        if re.search(r'\b%s\b' % re.escape(full), src):
            sys.exit('substitute.py: prune: %s still referenced after '
                     'pruning -- the survey was wrong, STOP' % full)
    return src, deleted


def swap_dispat(src):
    """The DISPAT swap (units/dispat_swap.c): rename the lift's
    statement dispatcher basicvfp_DISPAT -> basicvfp_DISPAT_lift (its
    body, all 11.8k lines, stays in the twin unchanged) and put a
    guaranteed-tail-call thunk under the old name, so every caller --
    hand STMT, the lift's DC immediate-mode entry, the code table's
    &FC100BEC registration -- reaches the hand dispatcher, which
    tail-calls the landed hand statement for a covered token and the
    renamed lift for everything else.  The lift's own switch (with its
    CASE_SUBS arms) stays live: the lift's internal B DISPAT paths
    (THENLN's) branch straight back into it and never pass the thunk.
    Returns the rewritten source; exits if either anchor is not found
    exactly once."""
    fwd = 'static void basicvfp_DISPAT(struct ros_cpu *s);'
    thunk = ('void basicvfp_DISPAT_lift(struct ros_cpu *s);\n'
             '/* ---- tier 3 DISPAT swap (units/dispat_swap.c) ----------\n'
             ' * The hand dispatcher owns the statement token switch: a\n'
             ' * covered token tail-calls its hand statement directly;\n'
             ' * anything else tail-calls the lift\'s own dispatcher\n'
             ' * below, renamed basicvfp_DISPAT_lift and otherwise\n'
             ' * unchanged.  With no statements covered this is the\n'
             ' * lift\'s behaviour unchanged. */\n'
             'void basicvfp_DISPAT(struct ros_cpu *s)\n'
             '{\n'
             '    ROS_TAIL_CALL(basicvfp_hand_DISPAT);\n'
             '}')
    if src.count(fwd) != 1:
        sys.exit('substitute.py: DISPAT swap: the lift\'s forward '
                 'declaration was not found exactly once')
    src = src.replace(fwd, thunk)
    defn = 'static void basicvfp_DISPAT(struct ros_cpu *s)\n{'
    if src.count(defn) != 1:
        sys.exit('substitute.py: DISPAT swap: the lift\'s definition '
                 'was not found exactly once')
    return src.replace(defn,
                       'void basicvfp_DISPAT_lift(struct ros_cpu *s)\n{')


def swap_factor(src):
    """The FACTOR swap (units/expr_factor_rest.c), row 8 form: the lift's
    evaluator trunk is DELETED (unit-factor-resid landed every residual
    built-in, so the hand dispatcher's default arm is FACERR and no path
    reaches the trunk -- the green light), and a guaranteed-tail-call
    thunk under the old name hands the character switch to the hand
    dispatcher.  Every caller and the code-table registration reach the
    thunk.  Deletion is asserted, and the name basicvfp_FACTOR_lift must
    appear nowhere afterwards.  Returns (src, lines_deleted)."""
    fwd = 'static void basicvfp_FACTOR(struct ros_cpu *s);'
    thunk = ('/* ---- tier 3 FACTOR swap (units/expr_factor_rest.c) ---\n'
             ' * The hand dispatcher owns FACTOR\'s character switch: every\n'
             ' * case the interpreter can reach is hand (the constants,\n'
             ' * the built-ins, the transcendentals, the trunk bodies of\n'
             ' * row 5 and unit-factor-resid), and the default arm is\n'
             ' * FACERR, exactly the lift\'s own for an error character.\n'
             ' * The lift trunk itself is GONE (row 8\'s scaffold drop):\n'
             ' * nothing referenced it any more. */\n'
             'void basicvfp_FACTOR(struct ros_cpu *s)\n'
             '{\n'
             '    ROS_TAIL_CALL(basicvfp_hand_FACTOR);\n'
             '}')
    if src.count(fwd) != 1:
        sys.exit('substitute.py: FACTOR swap: the lift\'s forward '
                 'declaration was not found exactly once')
    src = src.replace(fwd, thunk)
    defn = 'static void basicvfp_FACTOR(struct ros_cpu *s)\n{'
    if src.count(defn) != 1:
        sys.exit('substitute.py: FACTOR swap: the lift\'s definition '
                 'was not found exactly once')
    src = src.replace(defn, 'static void basicvfp_FACTOR_lift('
                            'struct ros_cpu *s)\n{')
    src, deleted = delete_function(src, 'basicvfp_FACTOR_lift')
    if re.search(r'\bbasicvfp_FACTOR_lift\b', src):
        sys.exit('substitute.py: FACTOR swap: basicvfp_FACTOR_lift still '
                 'referenced after the drop -- the green light was wrong, '
                 'STOP')
    return src, deleted


def write_swap_cfg(path, enabled, groups):
    """Write gen/dispat_swap_cfg.h -- the DISPAT swap's compile-time
    wiring, included by units/dispat_swap.c through the shared gen
    include path (the box build's -Ibuild/gen, the hosted -Igen).
    Rewritten on EVERY run, next to the twin, so a stale configuration
    can never outlive the build that wrote it: with the swap disabled
    the file defines nothing and the unit compiles to nothing."""
    lines = ['/* dispat_swap_cfg.h -- generated by substitute.py; do not',
             ' * edit.  The DISPAT swap\'s compile-time wiring, read by',
             ' * units/dispat_swap.c.  Rewritten on every run. */', '']
    if not enabled:
        lines += ['/* The swap is disabled (manifest.DISPAT_SWAP): the',
                  ' * twin keeps the lift\'s own basicvfp_DISPAT and',
                  ' * units/dispat_swap.c compiles to nothing. */']
    else:
        lines += ['#define BASICVFP_DISPAT_SWAP 1']
        for key in sorted(groups):
            fname, define, present = groups[key]
            lines.append('#define %s %d%s'
                         % (define, 1 if present else 0,
                            '  /* %s: %s */'
                            % (fname,
                               'present, arms live' if present else
                               'ABSENT, its tokens fall to the lift')))
    open(path, 'w').write('\n'.join(lines) + '\n')


def check_hooks(lift, twin):
    """Every hook patch-basicasm.py applied to the lift must still be
    reached in the twin (modules/basicvfp/patch-basicasm.py: the cross
    assemblers' seven sites, and FNGOACACHE's native-stack test).  The
    patch runs BEFORE substitution, so a hook inside a body the twin
    replaces or deletes is lost silently -- the twin builds, the gates
    pass, and the x86-64 assembler mis-encodes a forward reference or a
    deep PROC takes a SIGSEGV.  So: every hook name the lift calls is
    looked for in the twin and in the units, and a lost one is an error.
    Derived from the lift, not from a list, so it holds for any lift."""
    # on the comment-blanked text: a name left in a comment (the
    # include's own "/* ros_stack_room: ... */") is not a hook kept
    lift = strip_comments_strings(lift)
    twin = strip_comments_strings(twin)
    names = set(re.findall(r'\b(basicasm_[a-z_]+|ros_stack_room)\b', lift))
    if not names:
        return 0                        # an unpatched lift: nothing to keep
    units = ''
    for u in sorted(glob.glob(os.path.join(HERE, 'units', '*.c'))):
        units += strip_comments_strings(open(u, encoding='utf-8').read())
    lost = sorted(n for n in names if n not in twin and n not in units)
    if lost:
        sys.exit('substitute.py: the twin loses patch-basicasm.py hooks '
                 'that the lift carries: ' + ', '.join(lost) +
                 ' -- the hand body that replaced the patched one must '
                 'carry the hook (STATUS.md, the port note)')
    return len(names)


def main():
    src = open(SRC).read()

    cases = 0
    for label, cs in sorted(manifest.CASE_SUBS.items()):
        # A label's value is one case number or a LIST of them: LETST is
        # the identifier-start letter range (80 cases, one per token the
        # cruncher can start a variable name with), and the LVALUE
        # keywords own two tokens each (plain and statement forms).
        # Every number must match a case in one of the two shapes
        # below -- a missed one is an error, never a silent pass.
        for case in (cs if isinstance(cs, list) else [cs]):
            # The lift's dispatch has two shapes: a bare goto, and a poll
            # first (the lifter's checkpoint where the region needs one).
            # The poll is kept: it is behaviour, not decoration.
            # The transfer is a GUARANTEED tail call, the lift's own
            # ROS_TAIL_CALL contract: a hand statement never returns -- it
            # transfers to the next statement through STMT/NXT/DONEXT -- so
            # a plain call would leave DISPAT's frame on the C stack for
            # every statement executed, and a FOR/NEXT loop would exhaust
            # it.  musttail needs matching signatures, so the hand case
            # functions are one-argument and read their live return address
            # from R14, which the case stores first (exactly the r14 the
            # two-argument call used to hand in).
            # The case stores EVERY live local, not just the preamble's
            # dirty three (R10/R11/R13): the lift's own labels can branch
            # straight back into the dispatch (THENLN's BNE DISPAT, IF's
            # no-THEN exit) with locals the preamble never touched, and the
            # lift's statement body would read those LIVE locals -- the
            # hand body reads R[], which the case must therefore make
            # equal to them (on the normal STMT route every store is a
            # no-op: R[] is already in sync there).
            stores = ' '.join(f'R[{i}] = r{i};' for i in range(14))
            store = ('{ ' + stores + ' R[14] = r14; '
                     'ROS_TAIL_CALL(basicvfp_hand_%s); }' % label)
            # both shapes are replaced: a tier-copy in the other shape
            # must not survive because the first shape matched (Review,
            # 6 Oct -- the old loop stopped at the first shape found)
            found = 0
            for old, new in ((f'case {case}: goto {label};',
                              f'case {case}: ' + store),
                             (f'case {case}: {{ ROS_POLL(s, r13); goto {label}; }}',
                              f'case {case}: {{ ROS_POLL(s, r13); ' + store[1:])):
                n = src.count(old)
                if n:
                    src = src.replace(old, new)
                    assert src.count(old) == 0
                    found += n
            if found == 0:
                sys.exit(f'substitute.py: dispatch case not found: '
                         f'case {case}: goto {label}; (either form)')
            cases += found

    funcs = 0
    for name in getattr(manifest, 'FUNCTION_SUBS', ()):
        src = replace_function(src, name)
        funcs += 1

    # The DISPAT swap (units/dispat_swap.c): opt-in, defaulted OFF in
    # manifest.DISPAT_SWAP until the integration gates it.  The cfg
    # header is written below, next to the twin, so the two can never
    # disagree; a run that exits on a bad anchor writes neither.
    swap = getattr(manifest, 'DISPAT_SWAP', {})
    swap_on = bool(swap.get('ENABLE'))
    swap_groups = {}
    for key, (fname, define) in sorted(swap.get('GROUPS', {}).items()):
        swap_groups[key] = (fname, define,
                            os.path.isfile(os.path.join(HERE, 'units',
                                                        fname)))
    if swap_on:
        if 'DISPAT' in getattr(manifest, 'FUNCTION_SUBS', ()):
            sys.exit('substitute.py: DISPAT_SWAP and a FUNCTION_SUBS '
                     'entry for DISPAT are mutually exclusive')
        src = swap_dispat(src)
        # Row 8, stage 2b: shrink the renamed trunk to its live
        # regions (the residual arms and the TWOSTMT trunk).  The
        # surviving label set is asserted against the manifest's
        # expectation -- a regenerated lift fails the build rather
        # than pruning something live.
        shrink = swap.get('SHRINK')
        dispat_deleted = 0
        if shrink and shrink.get('ENABLE'):
            src, dispat_deleted = shrink_dispat(src, shrink['KEEP_LABELS'])

    # The FACTOR swap (units/expr_factor_rest.c): the evaluator trunk's
    # own DISPAT-swap shape -- row 8 form: the trunk is deleted
    # wholesale (unit-factor-resid closed the residual list; the hand
    # dispatcher's default arm is FACERR and nothing reaches the lift
    # trunk any more).
    factor_swap_on = bool(getattr(manifest, 'FACTOR_SWAP', {})
                          .get('ENABLE'))
    factor_deleted = 0
    if factor_swap_on:
        if 'FACTOR' in getattr(manifest, 'FUNCTION_SUBS', ()):
            sys.exit('substitute.py: FACTOR_SWAP and a FUNCTION_SUBS '
                     'entry for FACTOR are mutually exclusive')
        src, factor_deleted = swap_factor(src)

    # Row 8, stage 1: the scaffold drop's conservative leg -- lift
    # functions with no remaining caller anywhere (survey: 6 Oct 2026).
    # Runs after the swaps so names are in their final form; the final
    # assertion inside fails the build on any reference left behind.
    pruned_lines = 0
    prune_list = getattr(manifest, 'PRUNE', ())
    if prune_list:
        src, pruned_lines = prune_functions(src, prune_list)

    exports = 0
    # dict.fromkeys: EXPORTS may name a function twice (EXPR rides both
    # the evaluator and jump-table lists); de-static it once.
    for fn in dict.fromkeys(manifest.EXPORTS):
        # Most exports are (struct ros_cpu *s), but the lifter gave a
        # few merged-register signatures -- INITIALISERAM and READNUM
        # carry value/pointer parameters -- so match the name and its
        # opening paren whatever the parameter list (both the forward
        # declaration and the definition).  A name the lift already
        # declares public (EXPR in the box-build lift) is satisfied as
        # it stands; a name found NOWHERE is an error.
        src, n = re.subn(r'\bstatic void %s\(' % re.escape(fn),
                         'void %s(' % fn, src)
        if n == 0 and not re.search(r'\bvoid %s\(' % re.escape(fn), src):
            sys.exit(f'substitute.py: export not found to de-static: {fn}')
        exports += n

    anchor = '#include "rom_basicvfp.h"'
    if src.count(anchor) < 1:
        sys.exit('substitute.py: include anchor not found')
    includes = ''.join('\n#include "' + h + '"' for h in manifest.UNITS_HEADERS)
    src = src.replace(anchor, anchor + includes, 1)

    write_swap_cfg(os.path.join(ROOT, 'gen', 'dispat_swap_cfg.h'),
                   swap_on, swap_groups)
    hooks = check_hooks(open(SRC).read(), src)
    open(DST, 'w').write(src)
    swap_note = ('off' if not swap_on else
                 'on (groups ' + ','.join(k for k in sorted(swap_groups)
                                          if swap_groups[k][2]) + ')')
    print(f'{DST}: {cases} dispatch cases substituted '
          f'({len(manifest.CASE_SUBS)} hand functions), '
          f'{funcs} whole functions thunked, '
          f'{exports} exports, '
          f'row-8 prune {len(prune_list)} functions/{pruned_lines} lines'
          + (f' + FACTOR_lift {factor_deleted} lines' if factor_swap_on
             else '')
          + f', dispat swap {swap_note}'
          + f', {hooks} basicasm hooks kept')


if __name__ == '__main__':
    main()
