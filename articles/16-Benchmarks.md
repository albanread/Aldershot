# Benchmarks: the Machines and the Languages

*30 October 2026; FasterBASIC, Modula-2, BCPL and Forth added 31 October*

**Note: this is not RISC OS.** BOX is a RISC OS window manager and
application environment running on a Linux kernel. [Is BOX Linux?](5-Linux.md) says what that means. The languages
below are BOX's, as BOX builds and runs them; their speeds on RISC OS
itself would be different.

## Introduction

BOX runs programs in many languages: C, BBC BASIC (interpreted, or
compiled), FasterBASIC, Modula-2, BCPL, Forth, Lua, Python, Tcl, Lisp,
Smalltalk and Mojo, with Rust used for library code and engines. It runs on Apple silicon Macs and on Intel
machines. We wanted to know how fast each language is on each machine,
so we wrote the same three small programs in every language and timed
them on the four machines that run the languages today.

This article covers:

- the machines;
- the languages, how BOX runs each, and what each is used for in BOX;
- the three tests, and how they were timed;
- the results, test by test;
- the machines compared;
- the languages compared;
- the faults the comparison found, and what we fixed;
- the limits of what three small programs can show.

The programs themselves are in [benchmarks](benchmarks/README.md).

---

## 1 The machines

| Machine | Processor | How BOX runs on it |
| --- | --- | --- |
| Mac Studio (2025) | Apple M4 Max | BOX for Apple silicon, as a virtual machine under QEMU, which uses the Mac's own hypervisor |
| Mac mini (2018) | Intel Core i7-8700B, 6 cores, 3.2 GHz | BOX installed on its own SSD, with macOS erased: [BOX on the 2018 Mac mini](13-MacMini.md) |
| MacBook Pro 16-inch (2019) | Intel Core i9-9980HK, 8 cores, 2.4 GHz | BOX installed on its own SSD: [BOX on the 2019 MacBook Pro](14-MacBookPro.md) |
| Mac Pro (2013) | Intel Xeon E5-1620 v2, 4 cores, 3.7 GHz | BOX installed on its own SSD: [BOX on the 2013 Mac Pro](15-MacPro.md) |

On the Apple silicon Mac, BOX is a virtual machine, but the hypervisor
lets its code run on the processor directly, at full speed. The three
Intel Macs run BOX as their only operating system. They are second-hand
machines, bought on eBay ([BOX on Second-Hand Intel Macs](12-Macs.md)).

All four ran the same day's build of BOX, and the programs ran one at a
time.

---

## 2 The languages

### How BOX runs them

| Language | What BOX uses | How the program runs |
| --- | --- | --- |
| C | clang, the box's `*CC` | compiled to machine code before it runs |
| Mojo | Mojo 1.1, the box's `*Mojo` | compiled to machine code before it runs |
| Rust | rustc, on the Mac that builds BOX | compiled before it runs, as a Linux program; see below |
| BBC BASIC, compiled | ROSBAS ([source](../src/ROSBAS/README.md)), the box's `*RosBas` | compiled to machine code before it runs |
| FasterBASIC | FasterBASIC's compiler, through LLVM, the box's `*FasterBASIC` | compiled to machine code in memory, then run |
| Modula-2 | a Modula-2 compiler (PIM 4 and ISO) through LLVM, the box's `*Modula2` | compiled to machine code in memory, then run |
| BCPL | a modern BCPL compiler through LLVM, the box's `*BCPL` | compiled to machine code in memory, then run |
| Forth | F66, a subroutine-threaded Forth with its own optimiser, the box's `*F66` | each definition compiled to machine code as it is read |
| Lua | LuaJIT | interpreted, then compiled to machine code as it runs |
| Smalltalk | MACVM, a Strongtalk-style virtual machine | interpreted, then compiled to machine code as it runs |
| Lisp | NCL, BOX's Common Lisp | compiled to machine code through LLVM as each definition is read |
| Python | CPython 3.14 | interpreted |
| Tcl | Tcl 9.1 | interpreted, from byte code |
| BBC BASIC | BOX's BASIC, RISC OS's interpreter translated into C | interpreted |

Each has a compiler or interpreter for both kinds of processor, A64
code on Apple silicon and x86-64 code on Intel, except Forth: F66 makes
only A64 code so far, so it runs on Apple silicon alone. FasterBASIC,
Modula-2, BCPL and compiled BASIC share one copy of LLVM inside BOX. Compiled BASIC keeps the
meaning of BBC BASIC exactly, so the same source gives the same answers
interpreted or compiled.

**Rust is different from the others.** It is used in BOX only for library
code and engines, which run as Linux programs beside RISC OS. BOX's RISC
OS programs are not written in Rust, and Rust code is built on the Mac
that builds BOX. Its program here was built there too, and run in the box
with `*RunBox`, as the engines are. We include it because so much of BOX's
heavy work is done in it, and because it shows how fast that work can
be.

### What each is used for in BOX

| Language | Used for in BOX today |
| --- | --- |
| C | most of BOX itself: the operating system modules, translated from RISC OS's assembler or rewritten, the Window Manager, FileSwitch and HostFS, and Declaritive; the ported applications, among them PipeDream and NetSurf; and anyone's own programs, built with `*CC` |
| Mojo | the applications written for BOX: the front ends of !Scribe, !Trace, !Palette, !Ledger, !Folio, !Code, !Draft, !Present, !Publish, !FloorPlan, !Micropolis, !Tower and !Workflows, the Wimp side of each ([Apps for BOX](10-Apps.md)); Worker kernels, which run on the other cores; and anyone's own programs, built with `*Mojo` |
| Rust | library code and engines only: the document engines behind the applications above, which read and write .docx, .xlsx, PDF, SVG and images, and the engines of some of the games, such as !Tower's |
| BBC BASIC, interpreted | RISC OS's own language: existing BASIC programs, the examples on the disc, quick experiments, and BASIC programs' screens in desktop windows |
| BBC BASIC, compiled | turning a finished BASIC program into a faster application, from StrongED's Compile and run or with `*RosBas` |
| Lua | scripts, and small desktop utilities with Declaritive's Lua binding |
| Python | scripts, and small desktop programs with the `riscos` and `declaritive` modules; the newest of the languages |
| Tcl | automation: `*Tcl` scripts and !Workflows, which records and replays work in the desktop and drives the applications |
| Lisp | !Lisp, an editor with a live Lisp underneath, and Lisp programs with windows, a game pane and sound |
| Smalltalk | !Smalltalk, a Workspace and class Browser, and Smalltalk programs run like BASIC ones, by a double-click or `*Smalltalk` |
| FasterBASIC | !FasterBASIC, an editor in the style of QuickBASIC where F5 runs the program at once, and BASIC programs that call SWIs and open Declaritive windows |
| Modula-2 | !Modula2 and Modula-2 in !Code (colouring, Build and Run, errors, layout): programs made of modules, calling SWIs and opening Declaritive windows |
| BCPL | !BCPL and BCPL in !Code: BCPL programs with RISC OS's streams, SWIs and Declaritive windows |
| Forth | !F66, a console for trying things out, with the stack and the words on show, and Forth programs with SWIs and Declaritive windows; Apple silicon only so far |

---

## 3 The tests

| Test | What it does | Answer |
| --- | --- | --- |
| fib | works out the 30th Fibonacci number by the slowest, most obvious recursion: 1,664,079 calls | 832040 |
| sieve | counts the primes below 2,000,000 with the sieve of Eratosthenes, over an array of two million flags | 148933 |
| dict | puts 200,000 keys, `"k1"` to `"k200000"`, in a hash table with a number each, then looks each one up and adds the numbers | 40000200000 |

These are three common kinds of work. The fib test is mostly calling
procedures and adding small numbers. The sieve is loops and array
access. The dict test is building strings, hashing them and comparing
them.

Each program is written the way someone who uses that language would
write it, with no tricks. C uses an array of bytes and its own small hash
table; Mojo, Rust, Lua, Python, Tcl, Lisp, Smalltalk and FasterBASIC use
their own tables. BBC BASIC, Modula-2, BCPL and Forth have none, so their
programs make one from arrays, with a hash function written in the
language. The interpreted and
compiled BASIC results come from the same source file.

---

## 4 How we timed them

Each program times itself with its own language's clock, so the time
taken to start the language is not counted. Each test is run three times
and the best of the three is reported.

C, Mojo, Lua, BASIC, FasterBASIC, Modula-2, BCPL and Forth read clocks
that count in hundredths of a second.
To time something shorter than that, these repeat each test until at
least half a second has passed, and divide by the number of runs.

The first ten languages were timed on 30 October and the four newest on
31 October, each time with that day's build of BOX.

All times are in milliseconds (thousandths of a second). Smaller is
better. Every program gave the right answer on every machine. The
languages are listed fastest first, by their time on the Apple silicon
Mac.

---

## 5 The results

### fib(30)

| Language | Mac Studio (M4 Max) | Mac mini 2018 | MacBook Pro 2019 | Mac Pro 2013 |
| --- | ---: | ---: | ---: | ---: |
| BCPL | 1.4 | 2.7 | 2.5 | 3.0 |
| FasterBASIC | 1.4 | 2.6 | 2.4 | 3.0 |
| C | 1.4 | 3.1 | 2.9 | 3.2 |
| Rust | 1.5 | 6.5 | 2.3 | 2.4 |
| Mojo | 1.5 | 2.8 | 2.6 | 3.0 |
| Modula-2 | 1.7 | 2.5 | 2.2 | 2.8 |
| Forth | 2.5 | -- | -- | -- |
| Lua | 3.0 | 8.0 | 8.0 | 9.0 |
| Smalltalk | 3.2 | 7.7 | 7.4 | 9.9 |
| Lisp | 12.8 | 27.1 | 25.7 | 30.5 |
| BBC BASIC, compiled | 52 | 128 | 125 | 150 |
| Python | 62.8 | 141 | 148 | 170 |
| Tcl | 259 | 570 | 549 | 732 |
| BBC BASIC, interpreted | 330 | 690 | 680 | 770 |

### Sieve of the primes below 2,000,000

| Language | Mac Studio (M4 Max) | Mac mini 2018 | MacBook Pro 2019 | Mac Pro 2013 |
| --- | ---: | ---: | ---: | ---: |
| BCPL | 4.1 | 6.4 | 5.9 | 7.0 |
| C | 4.2 | 5.6 | 5.6 | 6.9 |
| Modula-2 | 4.3 | 6.2 | 5.9 | 7.0 |
| Mojo | 4.5 | 15.2 | 14.7 | 19.2 |
| Rust | 4.6 | 7.5 | 7.1 | 8.4 |
| Forth | 6.3 | -- | -- | -- |
| FasterBASIC | 7.1 | 12.5 | 8.9 | 21.7 |
| Lua | 26 | 20 | 17 | 30 |
| BBC BASIC, compiled | 43.3 | 102 | 100 | 125 |
| Smalltalk | 55 | 134 | 130 | 170 |
| Lisp | 69.3 | 130 | 124 | 141 |
| Python | 90.4 | 213 | 190 | 282 |
| Tcl | 282 | 626 | 572 | 868 |
| BBC BASIC, interpreted | 335 | 820 | 800 | 920 |

### 200,000 string keys

| Language | Mac Studio (M4 Max) | Mac mini 2018 | MacBook Pro 2019 | Mac Pro 2013 |
| --- | ---: | ---: | ---: | ---: |
| Lua | 11 | 39 | 35 | 52 |
| Forth | 12.8 | -- | -- | -- |
| Mojo | 17.6 | 34 | 30.6 | 35.3 |
| C | 21.7 | 46.4 | 45 | 57.8 |
| Modula-2 | 22.7 | 61.1 | 56.7 | 57.8 |
| Rust | 28.4 | 76.4 | 78.9 | 90.9 |
| BCPL | 40.8 | 114 | 100 | 150 |
| Python | 46.2 | 113 | 109 | 134 |
| Tcl | 55.3 | 171 | 159 | 197 |
| Smalltalk | 77.8 | 219 | 203 | 280 |
| Lisp | 347 | 798 | 798 | 1038 |
| BBC BASIC, compiled | 370 | 850 | 820 | 980 |
| FasterBASIC | 465 | 610 | 570 | 750 |
| BBC BASIC, interpreted | 1940 | 4270 | 4220 | 5030 |

A dash marks a time that cannot be taken: our Forth is written in Arm
assembly language and so runs only on the Arm box.

---

## 6 The machines compared

How many times longer each Intel Mac takes than the Apple silicon Mac,
taking the three tests together:

| Language | Mac mini 2018 | MacBook Pro 2019 | Mac Pro 2013 |
| --- | ---: | ---: | ---: |
| Rust | 2.7 | 1.9 | 2.1 |
| C | 1.8 | 1.8 | 2.2 |
| Mojo | 2.3 | 2.1 | 2.6 |
| Lua | 1.9 | 1.8 | 2.5 |
| Smalltalk | 2.5 | 2.4 | 3.3 |
| Lisp | 2.1 | 2.0 | 2.4 |
| BBC BASIC, compiled | 2.4 | 2.3 | 2.8 |
| Python | 2.3 | 2.3 | 2.9 |
| Tcl | 2.5 | 2.3 | 3.1 |
| FasterBASIC | 1.6 | 1.4 | 2.2 |
| Modula-2 | 1.8 | 1.7 | 1.9 |
| BCPL | 2.1 | 1.9 | 2.4 |
| Forth | -- | -- | -- |
| BBC BASIC, interpreted | 2.2 | 2.2 | 2.6 |
| **All** | **2.2** | **2.0** | **2.5** |

The Apple silicon Mac is the fastest, by about two to one, even though
BOX runs there as a virtual machine. Its processor is six years newer
than the newest of the Intel ones.

The 2018 Mac mini and the 2019 MacBook Pro are close. The MacBook Pro has
more cores, but each program here uses only one. The one large
difference is Rust's fib, which takes 6.5 ms on the mini every time,
against 2.3 ms on the MacBook Pro and 2.4 ms on the 2013 Mac Pro; the
mini's other Rust times match the MacBook's. So it is something about the
mini's processor, the i7-8700B, and that one program's code. We have not
found out what.

The 2013 Mac Pro takes about a fifth longer than the two later Intel
Macs. Its Ivy Bridge processor is five years older than theirs. All the
same, it runs every language and every test.

Lua's sieve is the exception to the pattern: it is quicker on the two
later Intel Macs than on the Apple silicon Mac. We think the reason is
that LuaJIT's compiler for x86-64 has had many more years of work than
its compiler for A64, but we have not looked into it.

FasterBASIC loses the least on the Intel Macs, because most of its time
is spent in the dict test, where it is only a third slower on the 2018
Mac mini than on the Apple silicon Mac. Modula-2 is the steadiest on the
2013 Mac Pro, which takes no longer than the two later Intel Macs at its
dict test. Forth has no Intel times; it runs on Apple silicon only.

---

## 7 The languages compared

How many times longer each language takes than C, on the Apple silicon
Mac:

| Language | fib | sieve | dict |
| --- | ---: | ---: | ---: |
| Mojo | 1.1 | 1.1 | 0.8 |
| Modula-2 | 1.2 | 1.0 | 1.0 |
| Rust | 1.1 | 1.1 | 1.3 |
| Forth | 1.8 | 1.5 | 0.6 |
| BCPL | 1.0 | 1.0 | 1.9 |
| Lua | 2.1 | 6.2 | 0.5 |
| FasterBASIC | 1.0 | 1.7 | 21 |
| Smalltalk | 2.3 | 13 | 3.6 |
| Python | 45 | 22 | 2.1 |
| Lisp | 9.1 | 16 | 16 |
| BBC BASIC, compiled | 37 | 10 | 17 |
| Tcl | 185 | 67 | 2.5 |
| BBC BASIC, interpreted | 236 | 80 | 89 |

**Mojo, Rust and C are the fast three.** All are compiled to machine code
before they run, through the same kind of optimising compiler, LLVM; on
the Apple silicon Mac they are within a tenth of one another at fib and
the sieve. Mojo is the quickest of the three at the dict test, since its
`Dict` is a well-made table and its strings are cheap; Rust's standard
`HashMap` spends time on a hash function chosen to resist attack rather
than for speed. On the Intel Macs, Mojo falls behind C and Rust at the
sieve. Of the languages BOX's RISC OS programs are written in, Mojo is as
fast as C on Apple silicon, and close to it on Intel apart from the
sieve.

**Modula-2, BCPL and FasterBASIC are as quick as C at calls.** All three
are compiled through LLVM, as C is, into machine code in memory just
before the program runs, and at fib all three are within a fifth of C.
Modula-2 and BCPL match C at the sieve as well, and Modula-2 matches it
at the dict test, with a table made from arrays. BCPL takes about twice
C's time at the dict test: its table starts small and is doubled as it
fills, and each key is a vector of its own from the heap. FasterBASIC is
a little behind at the sieve. Its dict test takes twenty times C's,
since every key is a new string value, made, hashed, stored in its
`HASHMAP` and freed again by its run-time library, which does far more
work for each short string than C's program does.

**Forth** compiles each definition to machine code as it is read, with
its own optimiser rather than LLVM. It is within twice C's time at fib
and the sieve, and quicker than C at the dict test, second only to Lua:
its program keeps the table in two fixed arrays, large enough from the
start, and copies the keys into one block of memory, so nothing is
allocated for each key.

**Lua and Smalltalk compile themselves to machine code while they run,
and it shows.** At calling procedures both are within two or three times
C's time, and about twenty times as fast as Python. LuaJIT's tables are
written in C and tuned hard, and are the quickest of all at the dict
test. Smalltalk's arrays and strings cost more than Lua's. It is ahead of
Lisp at both, and of BASIC at strings, but compiled BASIC is a little
quicker at the sieve.

**Lisp** is compiled too, and takes about four times Smalltalk's time
at calls. Its dict test is slow because its hashing, its comparison of
keys and its formatting of numbers are written in Lisp.

**Compiled BBC BASIC** is five to eight times as fast as the
interpreter, from the same source. It stays well behind C because it
keeps BBC BASIC's behaviour exactly: whole-number arithmetic is checked
for overflow, every variable is a global, since `LOCAL` works by dynamic
scope, and every statement records its line number and checks for
Escape.

**Python** interprets every statement and pays for every call, so it
takes about forty-five times C's time at fib. But its dictionary is
written in C, so it is quick at the dict test.

**Tcl** is slower still at calls and loops, nearly two hundred times C's
time at fib: every procedure call and every expression goes through
Tcl's interpreter, and a list element is changed through a command. Its
dictionaries, like Python's, are written in C, so it is quick at the
dict test, ahead of Smalltalk.

**The BBC BASIC interpreter** is the slowest at all three tests, a
little behind Tcl at fib and the sieve and far behind everything at the
dict test, where its hash table is written in BASIC. BOX's BASIC is RISC
OS's own interpreter, translated from ARM assembler into C ([Tiers of Translation](1-Tiers.md)), so that it behaves exactly as the original
does. That exactness costs time on every statement. For speed, compile
the program.

---

## 8 What we fixed on the way

Writing the comparison found these faults, which were fixed before the
times above were taken:

- **Lisp's hash tables never grew.** A table kept the 16 buckets it was
  made with, so a table of n entries took time in proportion to n for
  every lookup, and n squared to fill. The dict test took minutes, and
  once ended in a fault. Tables now double their buckets when they fill.
- **Smalltalk's strings were slow.** Joining two strings, copying part
  of one, and turning a number into text each went through several steps
  where one would do. Each is now a single built-in operation. A
  Smalltalk method that runs only a few times, such as a program's main
  one, could also keep running its loops in the interpreter for good;
  now they are compiled. Together these halved Smalltalk's dict time.
- **Our C program** assumed that `long` is 64 bits and that `strdup`
  exists. On the Intel version of BOX, `long` is 32 bits, so the sieve
  overflowed and gave a wrong answer; and BOX's C library has no
  `strdup`. Both were fixed in the program. Mojo's `Int` is 32 bits in
  the box for the same reason, so its program adds up the dict test's
  numbers in an `Int64`.
- **FasterBASIC's dict test seemed never to end.** Freeing a block of
  memory walked the whole list of free blocks first, to catch a block
  freed twice, so freeing n blocks in a row took time in proportion to n
  squared. The test finished, then took minutes to free its 200,000
  keys. A free block now carries a mark, and the list is walked only
  when the mark is there.
- **BCPL had four faults.** `VEC n` and `GETVEC(n)` gave n words rather
  than n+1, so writing `v!n` overwrote the next block; `FREEVEC` freed
  nothing, so a `GETVEC` in a loop used memory until there was none;
  every routine checked in with a garbage collector that BCPL does not
  have, which took fib from 18.5 ms to 1.4 ms once removed; and every call
  of a routine with a `VEC` made and cleared 64 KB of workspace, where
  1 KB does, which took the dict test from 227 ms to 39 ms.

---

## 9 Limits

- Three small programs say little about large ones. A program that
  spends its time drawing in the desktop, reading files or using the
  network will see much smaller differences between languages.
- The dict test compares tables written in C, for Lua, Python and Tcl,
  with tables written in the language itself, for Mojo, Rust, Lisp,
  Smalltalk, BASIC, Modula-2, BCPL and Forth. FasterBASIC's `HASHMAP`
  is part of its run-time library, which is written in Zig.
- Forth runs only on Apple silicon so far, so it has no Intel times.
- The times vary by a few per cent from run to run. The C, Mojo, Lua,
  BASIC, FasterBASIC, Modula-2, BCPL and Forth times are averages over repeated runs, read from a clock that
  counts hundredths of a second. The Apple silicon times are the best of
  two runs of all the programs.
- BOX runs on other machines too, among them an Intel N97 mini PC
  ([BOX on a Low-Cost Mini PC](9-MiniPC.md)) and Intel Macs under
  QEMU. They are not measured here yet.

---

## 10 The programs

The programs, one for each language (the two BBC BASIC results share
one), are in [benchmarks](benchmarks/README.md), with the commands that
run each one in BOX. Each prints one line for each test: the language,
the test, the time in milliseconds and the answer.
