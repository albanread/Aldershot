"""capps_patches.py -- the patch series over staged RISC OS C (tools/capps.py).

Each patch is (staged path, the text as RISC OS has it, the text for clang,
why), or with REGEX: (staged path, REGEX, pattern, replacement, how many
times it must match, why).  The old text must occur exactly once (a pattern
exactly as often as given), so a change upstream stops the stage rather than
slipping past it.  Paths are under OUT/pre/src, after unixifying
(c/name -> name.c).

What Norcroft's dialect needs everywhere (unsigned plain bit-fields, word-
aligned structures, the library's statics thread-local) is not here: the
source pass (tools/capps_pass.py) does it from clang's AST.  These are the
component patches: the few places whose text must change.
"""
import os
import re

REGEX = "regex"

MONTHS = "Jan Feb Mar Apr May Jun Jul Aug Sep Oct Nov Dec".split()


def c_date(riscos):
    """RISC_OSLib's VersionNum date ("22 Aug 2026") as __DATE__ spells a
    date ("Aug 22 2026", a day below 10 space-padded): the library banner's
    form in every Norcroft build."""
    with open(os.path.join(riscos, "Sources", "Lib", "RISC_OSLib", "VersionNum"), "rb") as f:
        m = re.search(rb'#define\s+Module_Date\s+"(\d+) (\w{3}) (\d{4})"', f.read())
    if not m or m.group(2).decode() not in MONTHS:
        raise SystemExit("capps: RISC_OSLib's VersionNum has no Module_Date \"dd Mmm yyyy\"")
    return "%s %2d %s" % (m.group(2).decode(), int(m.group(1)), m.group(3).decode())


def patches(riscos):
    return [
        # c/armsys:66 -- the library's banner carries the build date; a
        # reproducible build refuses __DATE__ (-Werror=date-time), so it
        # carries the component's date from VersionNum, in __DATE__'s form:
        # "Shared C Library vsn 623/32 [Aug 22 2026]", as Norcroft builds print.
        ("Lib/RISC_OSLib/armsys.c",
         '           " [" __DATE__ "]\\n";',
         '           " [" "%s" "]\\n";' % c_date(riscos),
         "reproducible: no __DATE__"),
        # Toolbox/Gadgets c/glib3:35 -- an array initialised by a scalar NULL,
        # which Norcroft accepts and C does not.
        ("Toolbox/Gadgets/glib3.c",
         "static TaskRec *all_tasks[8]=NULL;",
         "static TaskRec *all_tasks[8]={NULL};",
         "invalid C: scalar initialiser for an array"),
        # c/alloc:277 -- the heap's mutex.  The 32-bit build calls kernel/s
        # k_body's AcquireMutex: SWP (or LDREX/STREX) 0 into the word, and
        # XOS_UpCall 6 (sleep on it) while it was already 0.  The same as a
        # C11 atomic exchange; heapMutex is a library static, so each
        # client's own (the source pass makes it thread-local).
        ("Lib/RISC_OSLib/alloc.c",
         "extern void AcquireMutex(volatile int *p);\n\n"
         "#define ACQUIREMUTEX \\\n"
         "  do { \\\n"
         "    AcquireMutex(&heapMutex); \\\n"
         "  } while (0)\n",
         "/* ROSGD: k_body's AcquireMutex as a C11 atomic exchange (capps_patches) */\n"
         "#define ACQUIREMUTEX \\\n"
         "  do { \\\n"
         "    while (__atomic_exchange_n(&heapMutex, 0, __ATOMIC_ACQUIRE) == 0) \\\n"
         "      _swix(OS_UpCall, _INR(0,1), 6, &heapMutex); \\\n"
         "  } while (0)\n",
         "SWP: the heap mutex as C11 atomics"),
        ("Lib/RISC_OSLib/alloc.c",
         "#define RELEASEMUTEX { heapMutex = 1; }",
         "#define RELEASEMUTEX { __atomic_store_n(&heapMutex, 1, __ATOMIC_RELEASE); }",
         "SWP: the heap mutex as C11 atomics"),
        # c/scanf:101-797 -- vfscanf hands its va_list to rd_int, rd_real,
        # rd_string and rd_string_map by value, and each takes its argument
        # with va_arg.  With Norcroft's char *[1] (and x32's one-element
        # array) the callee advances the caller's list; where va_list is a
        # structure passed by value (AArch64) it would not.  So the list
        # goes by pointer: vfscanf copies it and passes &ap.
        ("Lib/RISC_OSLib/scanf.c", REGEX,
         r"(static long int rd_\w+\(FILE \*p, )va_list res,", r"\1va_list *res,", 5,
         "va_list by pointer to the conversions"),
        ("Lib/RISC_OSLib/scanf.c", REGEX,
         r"va_arg\(res, ", "va_arg(*res, ", 10,
         "va_list by pointer to the conversions"),
        ("Lib/RISC_OSLib/scanf.c", REGEX,
         r"va_arg\(argv, ", "va_arg(*argp, ", 6,
         "va_list by pointer to the conversions"),
        ("Lib/RISC_OSLib/scanf.c", REGEX,
         r"(rd_\w+\(p, )argv,", r"\1argp,", 9,
         "va_list by pointer to the conversions"),
        # c/fpprintf:589 and c/scanf:263 -- %a reads a double's words, and
        # scanf's hexadecimal conversion (cvthex) writes them, in the FPA's
        # order: the high word (sign, exponent) first.  x32's doubles are
        # IEEE little-endian, the high word second.  (Package L3a.)
        ("Lib/RISC_OSLib/fpprintf.c",
         "                    whi = ((unsigned *)lvd)[0];\n"
         "                    wlo = ((unsigned *)lvd)[1];\n",
         "                    /* ROSGD: x32's word order, the high word second (capps_patches) */\n"
         "                    whi = ((unsigned *)lvd)[1];\n"
         "                    wlo = ((unsigned *)lvd)[0];\n",
         "FPA word order: %a"),
        ("Lib/RISC_OSLib/scanf.c",
         "        return *(double *) a;\n",
         "    {   /* ROSGD: x32's word order, the high word second (capps_patches) */\n"
         "        union { double d; unsigned w[2]; } u;\n"
         "        u.w[0] = a[1];\n"
         "        u.w[1] = a[0];\n"
         "        return u.d;\n"
         "    }\n",
         "FPA word order: hexadecimal scanf"),
        # h/hostsys:156 -- fp_number, the union math.c's frexp, scalbln (and
        # so ldexp and scalbn), ilogb and modf read a double's fields
        # through, is laid out in the FPA's word order: the high word (sign,
        # exponent) first.  x32's doubles are IEEE little-endian, the high
        # word second: hostsys.h's other order.  (Package S2; its
        # _fp_normalize users, in fpprintf and scanf, are in their
        # !HOST_HAS_BCD_FLT branches, not built.)
        ("Lib/RISC_OSLib/hostsys.h",
         "#        undef OTHER_WORD_ORDER_FOR_FP_NUMBERS\n",
         "/* ROSGD (capps_patches): x32's word order, the high word second */\n"
         "#        define OTHER_WORD_ORDER_FOR_FP_NUMBERS 1\n",
         "FPA word order: fp_number"),
        # c/fpprintf:614 -- %a fills buff[0..14] (a leading 0, then 14 hex
        # digits) and fp_roundhex reads buff[len], and in a tie scans on for a
        # non-zero digit: with 13 digits, or a tie below 13, that is buff[15]
        # and past it, which nothing wrote -- the stack's garbage decides the
        # last digit, on 5.30 too (%a of 0x1.fffffffffffff can print
        # 0x2.0000000000000p+0).  A zero there ends the digits, so the
        # rounding is round to nearest even of what is there.  (Package L3a.)
        ("Lib/RISC_OSLib/fpprintf.c",
         "                    for (i=7; i<=14; i++)\n"
         "                        buff[i] = hextab[(wlo >> (56-i*4)) & 0xf];\n",
         "                    for (i=7; i<=14; i++)\n"
         "                        buff[i] = hextab[(wlo >> (56-i*4)) & 0xf];\n"
         "                    buff[15] = 0;   /* ROSGD: the digits end here (capps_patches) */\n",
         "%a: fp_roundhex reads past the digits"),
        # c/scanf:161 -- carefully_narrow gives ERANGE if narrowing a double
        # to float raised underflow.  MXCSR's UE is IEEE's tiny and inexact;
        # the FPA's (FPASC coresrc/s/rounding, UnderflowForReg, the trap
        # disabled) is tiny after rounding to 24 bits and denormalising
        # losing bits: "1.1754942e-38" narrows to 007FFFFF inexactly but
        # without that loss, and 5.30 sets no ERANGE.  So the FPA's test, on
        # the double's bits.  (Package L3a.)
        ("Lib/RISC_OSLib/scanf.c",
         "static float carefully_narrow(double l)\n{\n",
         "/* ROSGD (capps_patches): underflow as the FPA flags it narrowing l: l\n"
         " * rounded to 24 bits with the exponent unbounded is below FLT_MIN, and\n"
         " * is not a multiple of the least denormal, 2^-149 */\n"
         "static int fpa_underflows_single(double l)\n"
         "{   union { double d; unsigned long long u; } b;\n"
         "    unsigned long long m, r, rem;\n"
         "    int e, s;\n"
         "    b.d = l;\n"
         "    e = (int)(b.u >> 52) & 0x7FF;\n"
         "    m = b.u & 0xFFFFFFFFFFFFFull;\n"
         "    if (e == 0x7FF || (e == 0 && m == 0)) return 0;\n"
         "    if (e == 0)                           /* a double denormal: normalised */\n"
         "    {   while (!(m & 0x10000000000000ull)) m <<= 1, e--;\n"
         "        e++;\n"
         "    }\n"
         "    else m |= 0x10000000000000ull;        /* l = m x 2^(e-1075) */\n"
         "    r = m >> 29, rem = m & 0x1FFFFFFFull; /* 24 bits, to nearest even */\n"
         "    if (rem > 0x10000000ull || (rem == 0x10000000ull && (r & 1))) r++;\n"
         "    if (r == 0x1000000ull) r >>= 1, e++;\n"
         "    if (e >= 897) return 0;               /* r x 2^(e-1046) >= FLT_MIN */\n"
         "    s = 897 - e;                          /* the bits under 2^-149 */\n"
         "    return s > 24 || (r & ((1ull << s) - 1)) != 0;\n"
         "}\n\n"
         "static float carefully_narrow(double l)\n{\n",
         "FPA underflow: %f's narrowing"),
        ("Lib/RISC_OSLib/scanf.c",
         "    f = (float) l;\n"
         "    if (fetestexcept(FE_UNDERFLOW|FE_OVERFLOW))\n"
         "        errno = ERANGE;\n",
         "    f = (float) l;\n"
         "    if (fetestexcept(FE_OVERFLOW) || fpa_underflows_single(l))   /* ROSGD (capps_patches) */\n"
         "        errno = ERANGE;\n",
         "FPA underflow: %f's narrowing"),
        # c/scanf:400-420 and 189-204 -- the exponent's arithmetic, which
        # Norcroft's code wraps (the ARM's adds), and clang -O2 does not:
        # signed overflow is undefined, and clang folds "x < -999" as if it
        # could not happen, so "1e-2147483648" (x2 wraps to INT_MIN) gave
        # +HUGE_VAL, where 5.30 gives 0 with ERANGE.  So unsigned, as the
        # ARM computes it.  (Package L3a; -fwrapv in the flag table would
        # do it everywhere.)
        ("Lib/RISC_OSLib/scanf.c", REGEX,
         r"x2 = 10\*x2 \+ scanf_intofdigit\(ch\);",
         "x2 = (int)(10u*(unsigned)x2 + (unsigned)scanf_intofdigit(ch));   /* ROSGD: wraps (capps_patches) */", 2,
         "exponent arithmetic wraps, as Norcroft's"),
        ("Lib/RISC_OSLib/scanf.c", REGEX,
         r"if \(flag & NEGEXP\) x -= x2; else x \+= x2;",
         "if (flag & NEGEXP) x = (int)((unsigned)x - (unsigned)x2); else x = (int)((unsigned)x + (unsigned)x2);", 2,
         "exponent arithmetic wraps, as Norcroft's"),
        ("Lib/RISC_OSLib/scanf.c",
         "            a[2] = 0;\n"
         "            x -= 8;\n"
         "        }\n"
         "        while ((a[0] & 0xf00)==0)\n"
         "        {   a[0] = (a[0]<<4) | (a[1]>>28);\n"
         "            a[1] = (a[1]<<4) | (a[2]>>28);\n"
         "            a[2] = a[2]<<4;\n"
         "            x -= 1;\n"
         "        }\n"
         "        x += 18;",
         "            a[2] = 0;\n"
         "            x = (int)((unsigned)x - 8u);   /* ROSGD: wraps (capps_patches) */\n"
         "        }\n"
         "        while ((a[0] & 0xf00)==0)\n"
         "        {   a[0] = (a[0]<<4) | (a[1]>>28);\n"
         "            a[1] = (a[1]<<4) | (a[2]>>28);\n"
         "            a[2] = a[2]<<4;\n"
         "            x = (int)((unsigned)x - 1u);\n"
         "        }\n"
         "        x = (int)((unsigned)x + 18u);",
         "exponent arithmetic wraps, as Norcroft's"),
        ("Lib/RISC_OSLib/scanf.c",
         "maxx=1023, x += 20 + 64;\n",
         "maxx=1023, x = (int)((unsigned)x + 84u);   /* ROSGD: wraps (capps_patches) */\n",
         "exponent arithmetic wraps, as Norcroft's"),
        ("Lib/RISC_OSLib/scanf.c",
         "maxx=127, x += 23 + 64;\n",
         "maxx=127, x = (int)((unsigned)x + 87u);\n",
         "exponent arithmetic wraps, as Norcroft's"),
        ("Lib/RISC_OSLib/scanf.c",
         "        a[2] = 0;\n"
         "        x -= 32;\n"
         "    }\n\n"
         "    while ((a[0] & msb)==0)\n"
         "    {   a[0] = (a[0]<<1) | (a[1]>>31);\n"
         "        a[1] = (a[1]<<1) | (a[2]>>31);\n"
         "        a[2] = a[2]<<1;\n"
         "        x -= 1;\n"
         "    }\n",
         "        a[2] = 0;\n"
         "        x = (int)((unsigned)x - 32u);\n"
         "    }\n\n"
         "    while ((a[0] & msb)==0)\n"
         "    {   a[0] = (a[0]<<1) | (a[1]>>31);\n"
         "        a[1] = (a[1]<<1) | (a[2]>>31);\n"
         "        a[2] = a[2]<<1;\n"
         "        x = (int)((unsigned)x - 1u);\n"
         "    }\n",
         "exponent arithmetic wraps, as Norcroft's"),
        ("Lib/RISC_OSLib/scanf.c",
         "int vfscanf(FILE *p, const char *sfmt, va_list argv)\n{\n",
         "static int vfscanf_ap(FILE *p, const char *sfmt, va_list *argp);\n\n"
         "int vfscanf(FILE *p, const char *sfmt, va_list argv)\n"
         "{   /* ROSGD: the conversions share one list through a pointer (capps_patches) */\n"
         "    va_list ap;\n"
         "    int n;\n"
         "    va_copy(ap, argv);\n"
         "    n = vfscanf_ap(p, sfmt, &ap);\n"
         "    va_end(ap);\n"
         "    return n;\n"
         "}\n\n"
         "static int vfscanf_ap(FILE *p, const char *sfmt, va_list *argp)\n{\n",
         "va_list by pointer to the conversions"),
    ]


def filters(root, riscos):
    """{absolute staged path: bytes -> bytes} for capps.py's writer; root is
    the tree the sources are staged into (OUT/pre)."""
    by = {}
    for p in patches(riscos):
        by.setdefault(os.path.abspath(os.path.join(root, "src", p[0])), []).append(p)

    def make(items):
        def f(data):
            t = data.decode("latin-1")
            for p in items:
                if p[1] == REGEX:
                    rel, _, pat, repl, want, why = p
                    t, n = re.subn(pat, repl, t)
                    if n != want:
                        raise SystemExit("capps: patch for %s (%s): %r matches %d times, not %d"
                                         % (rel, why, pat, n, want))
                    continue
                rel, old, new, why = p
                n = t.count(old)
                if n != 1:
                    raise SystemExit("capps: patch for %s (%s): the text occurs %d times, not once" % (rel, why, n))
                t = t.replace(old, new)
            return t.encode("latin-1")
        return f
    return {p: make(items) for p, items in by.items()}
