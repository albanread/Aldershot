# The benchmark programs

The programs timed in [Benchmarks: the Machines and the Languages](../16-Benchmarks.md):
the same three tests in each language. They are BOX's own work, under the
MIT licence ([LICENSES](../../LICENSES/README.md)).

| File | Language | How it is run in BOX |
| --- | --- | --- |
| bench.c | C | `*CC bench/c -O2 -o benchcc`, then `*benchcc` |
| bench.bas | BBC BASIC | interpreted: `*Basic -quit bench/bas`; compiled: `*RosBas bench/bas`, then `*bench` |
| bench.lua | Lua | `*LuaJIT bench/lua` |
| bench.py | Python | `*Python3 bench/py` |
| bench.lisp | Lisp | `*RunBox <Lisp$Dir>.Engine-<processor> --load <the file's Linux name>` |
| bench.st | Smalltalk | `*Smalltalk bench/st <a Linux file name for the results>`, from the desktop or with `*WimpTask` |

Each prints one line for each test: the language, the test, the time in
milliseconds and the answer. The answers are 832040 for fib, 148933 for
the sieve and 40000200000 for the dict test.

Each program times itself, with its language's own clock, and reports
the best of three runs. C, Lua and BASIC read clocks that count hundredths
of a second, so they repeat each test until half a second has passed and
divide.

The BASIC file has line numbers and is plain text; both the interpreter
and the compiler read it as it is.
