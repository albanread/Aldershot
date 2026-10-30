# Benchmarks: the Machines and the Languages

*30 October 2026*

**Note: this is not RISC OS.** BOX is a RISC OS window manager and
application environment running on a Linux kernel. [Is BOX Linux?](5-Linux.md) says what that means. The languages
below are BOX's, as BOX builds and runs them; their speeds on RISC OS
itself would be different.

## Introduction

BOX runs programs in several languages: C, BBC BASIC (interpreted, or
compiled), Lua, Python, Lisp and Smalltalk. It runs on Apple silicon
Macs and on Intel machines. We wanted to know how fast each language is
on each machine, so we wrote the same three small programs in every
language, and timed them on the four machines that run all of the
languages today.

This article covers:

- the machines;
- the languages, and how BOX runs each;
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

| Language | What BOX uses | How the program runs |
| --- | --- | --- |
| C | clang, the box's `*CC` | compiled to machine code before it runs |
| BBC BASIC | BOX's BASIC, RISC OS's interpreter translated into C | interpreted |
| BBC BASIC, compiled | ROSBAS ([source](../src/ROSBAS/README.md)), the box's `*RosBas` | compiled to machine code before it runs |
| Lua | LuaJIT | interpreted, then compiled to machine code as it runs |
| Python | CPython 3.14 | interpreted |
| Lisp | NCL, BOX's Common Lisp | compiled to machine code through LLVM as each definition is read |
| Smalltalk | MACVM, a Strongtalk-style virtual machine | interpreted, then compiled to machine code as it runs |

Each has a compiler or interpreter for both kinds of processor: A64
code on Apple silicon, x86-64 code on Intel. Compiled BASIC keeps the
meaning of BBC BASIC exactly, so the same source gives the same answers
interpreted or compiled.

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
table; Lua, Python, Lisp and Smalltalk use their built-in tables. BBC
BASIC has no hash table, so its program makes one from arrays, with a
hash function written in BASIC. The interpreted and compiled BASIC
results come from the same source file.

---

## 4 How we timed them

Each program times itself with its own language's clock, so the time
taken to start the language is not counted. Each test is run three times
and the best of the three is reported.

C, Lua and BASIC read clocks that count in hundredths of a second. To
time something shorter than that, these three repeat each test until at
least half a second has passed, and divide by the number of runs.

All times are in milliseconds (thousandths of a second). Smaller is
better. Every program gave the right answer on every machine.

---

## 5 The results

### fib(30)

| Language | Mac Studio (M4 Max) | Mac mini 2018 | MacBook Pro 2019 | Mac Pro 2013 |
| --- | ---: | ---: | ---: | ---: |
| C | 1.4 | 3.1 | 2.9 | 3.2 |
| Lua | 3.0 | 8.0 | 8.0 | 9.0 |
| Smalltalk | 3.2 | 7.7 | 7.4 | 9.9 |
| Lisp | 12.6 | 27.1 | 25.7 | 30.5 |
| BBC BASIC, compiled | 52 | 128 | 125 | 150 |
| Python | 63 | 141 | 148 | 171 |
| BBC BASIC, interpreted | 335 | 690 | 680 | 770 |

### Sieve of the primes below 2,000,000

| Language | Mac Studio (M4 Max) | Mac mini 2018 | MacBook Pro 2019 | Mac Pro 2013 |
| --- | ---: | ---: | ---: | ---: |
| C | 4.3 | 5.6 | 5.6 | 6.9 |
| Lua | 30 | 20 | 17 | 30 |
| BBC BASIC, compiled | 43 | 102 | 100 | 125 |
| Smalltalk | 55 | 134 | 131 | 170 |
| Lisp | 71 | 130 | 124 | 141 |
| Python | 94 | 213 | 191 | 282 |
| BBC BASIC, interpreted | 335 | 820 | 800 | 920 |

### 200,000 string keys

| Language | Mac Studio (M4 Max) | Mac mini 2018 | MacBook Pro 2019 | Mac Pro 2013 |
| --- | ---: | ---: | ---: | ---: |
| Lua | 11 | 39 | 35 | 52 |
| C | 24 | 46 | 45 | 58 |
| Python | 45 | 113 | 109 | 134 |
| Smalltalk | 88 | 219 | 203 | 280 |
| Lisp | 364 | 798 | 798 | 1038 |
| BBC BASIC, compiled | 375 | 850 | 820 | 980 |
| BBC BASIC, interpreted | 1950 | 4270 | 4220 | 5030 |

---

## 6 The machines compared

How many times longer each Intel Mac takes than the Apple silicon Mac,
taking the three tests together:

| Language | Mac mini 2018 | MacBook Pro 2019 | Mac Pro 2013 |
| --- | ---: | ---: | ---: |
| C | 1.8 | 1.7 | 2.1 |
| Lua | 1.8 | 1.7 | 2.4 |
| Smalltalk | 2.4 | 2.3 | 3.1 |
| Lisp | 2.0 | 2.0 | 2.4 |
| BBC BASIC, compiled | 2.4 | 2.3 | 2.8 |
| Python | 2.3 | 2.3 | 2.9 |
| BBC BASIC, interpreted | 2.2 | 2.2 | 2.5 |
| **All seven** | **2.1** | **2.0** | **2.6** |

The Apple silicon Mac is the fastest, by about two to one, even though
BOX runs there as a virtual machine. Its processor is six years newer
than the newest of the Intel ones.

The 2018 Mac mini and the 2019 MacBook Pro are within a few per cent of
each other. The MacBook Pro has more cores, but each program here uses
only one, and its single core is no faster than the mini's.

The 2013 Mac Pro takes about a quarter longer than the two later Intel
Macs. Its Ivy Bridge processor is five years older than theirs. All the
same, it runs every language and every test.

There is one exception to the pattern: Lua's sieve is quicker on the
two later Intel Macs than on the Apple silicon Mac. We think the reason
is that LuaJIT's compiler for x86-64 has had many more years of work
than its compiler for A64, but we have not looked into it.

---

## 7 The languages compared

How many times longer each language takes than C, on the Apple silicon
Mac:

| Language | fib | sieve | dict |
| --- | ---: | ---: | ---: |
| Lua | 2 | 7 | 0.5 |
| Smalltalk | 2 | 13 | 4 |
| Lisp | 9 | 16 | 15 |
| BBC BASIC, compiled | 37 | 10 | 16 |
| Python | 45 | 22 | 2 |
| BBC BASIC, interpreted | 240 | 78 | 83 |

**C is the fastest at calling procedures and running loops**, as it
should be, and is the yardstick for the rest.

**Lua and Smalltalk compile themselves to machine code while they run,
and it shows.** At calling procedures both are within two or three times
C's time, and about twenty times as fast as Python. LuaJIT's tables are
written in C and tuned hard, and beat our plain C hash table at the
dict test. Smalltalk's arrays and strings cost more than Lua's. It is
ahead of Lisp at both, and of BASIC at strings, but compiled BASIC is a
little quicker at the sieve.

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
takes forty-five to fifty-five times C's time at fib. But its dictionary
is written in C, so at the dict test it comes third, behind only LuaJIT
and C.

**The BBC BASIC interpreter** is the slowest. BOX's BASIC is RISC OS's
own interpreter, translated from ARM assembler into C ([Tiers of Translation](1-Tiers.md)), so that it behaves exactly as the original
does. That exactness costs time on every statement. For speed,
compile the program.

---

## 8 What we fixed on the way

Writing the comparison found three faults, which were fixed before the
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
  `strdup`. Both were fixed in the program.

---

## 9 Limits

- Three small programs say little about large ones. A program that
  spends its time drawing in the desktop, reading files or using the
  network will see much smaller differences between languages.
- The dict test compares tables written in C, for Lua and Python, with
  tables written in the language itself, for Lisp, Smalltalk and BASIC.
- The times vary by a few per cent from run to run. The C, Lua and BASIC
  times are averages over repeated runs, read from a clock that counts
  hundredths of a second.
- BOX also runs Tcl and Mojo; they are not in these tables. On the Apple
  silicon Mac, Tcl took 253 ms for fib, 270 ms for the sieve and 54 ms
  for the dict test.
- BOX runs on other machines too, among them an Intel N97 mini PC
  ([BOX on a Low-Cost Mini PC](9-MiniPC.md)) and Intel Macs under
  QEMU. They are not measured here yet.

---

## 10 The programs

The seven programs, one for each language (the two BBC BASIC results
share one), are in [benchmarks](benchmarks/README.md), with the commands
that run each one in BOX. Each prints one line for each test: the
language, the test, the time in milliseconds and the answer.
