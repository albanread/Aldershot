# ROSBAS status

## The test corpus

ROSBAS is checked against two sets of BASIC programs.

**The conformance suite.** 627 small programs, one or more for each
section of the language specification, grouped by chapter:

| Chapter | Programs |
|---|---|
| 2 Source text | 75 |
| 3 Values | 29 |
| 4 Variables and arrays | 38 |
| 5 Expressions | 46 |
| 6 Strings | 30 |
| 7 Control | 81 |
| 8 Procedures and functions | 45 |
| 9 Errors | 55 |
| 10 Output | 46 |
| 11 Input and data | 37 |
| 12 Memory | 18 |
| 13 Files | 38 |
| 14 The operating system | 56 |
| 16 The inline assembler, CALL and USR | 15 |

Each program has the output that real RISC OS 5.30 printed when its own
BBC BASIC ran it. The compiled program must print exactly the same. A few
programs also have:

- a `.default` file: the output when one of the interpreter's quirks is
  left off, which is the compiler's default;
- a `.rejected` file: the program must be refused by the compiler, with
  the message given.

RISC OS 5.30 cannot assemble x86-64 or AArch64 code, so the assembler's
tests were recorded by the BOX's own BBC BASIC instead.

**The Rosetta corpus.** 163 complete programs: the BBC BASIC solutions
from Rosetta Code, collected and checked. Their output was also recorded
on RISC OS 5.30. This is
the integration test. A few programs cannot give the same output twice,
because they use random numbers or the clock, and are checked by rule
instead.

**The tokeniser** is also checked on its own. On the conformance
programs, the 163 Rosetta programs and 24 further cases,
`tokenise` gives the same bytes and line numbers as the interpreter's own
loader.

Neither corpus is in this repository yet.

## Where it stands

As of 28 September 2026:

- **On the host** (a Mac, with its C compiler): 640 of the 649
  conformance test runs pass. 162 of the 163 Rosetta programs pass. The
  one left reads the screen back, which the host cannot do.
- **On RISC OS 5.30**, as ARM applications: the same 640 conformance
  runs pass.
- **In the BOX**, on Intel and Apple silicon: the BOX's `*RosBas`
  command compiles and builds a program entirely inside the BOX.

Still to do:

- running the Rosetta corpus on RISC OS 5.30;
- ARM code in the inline assembler, and `CALL` and `USR` on ARM;
- one quirk that does not yet behave correctly when it is turned on.
