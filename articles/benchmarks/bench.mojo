"""Bench: the language comparison in Mojo: fib(30), a sieve of the primes
below 2,000,000, and 200,000 string keys in a Dict.  The clock counts
centiseconds, so each test is repeated until half a second has gone; the
best of three, in milliseconds.  Int is 32 bits in the box: the dict sum
is an Int64."""

from riscos import system
from std.collections import Dict


def fib(n: Int) -> Int:
    if n < 2:
        return n
    return fib(n - 1) + fib(n - 2)


def sieve() -> Int64:
    comptime N = 2000000
    var s = List[UInt8]()
    s.resize(N + 1, 0)
    var c = 0
    for i in range(2, N + 1):
        if s[i] == 0:
            c += 1
            if i <= 1414:
                var j = i * i
                while j <= N:
                    s[j] = 1
                    j += i
    return Int64(c)


def dict() raises -> Int64:
    var d = Dict[String, Int64]()
    for i in range(1, 200001):
        d["k" + String(i)] = Int64(i) * 2
    var s: Int64 = 0
    for i in range(1, 200001):
        s += d["k" + String(i)]
    return s


def work(w: Int) raises -> Int64:
    if w == 0:
        return Int64(fib(30))
    if w == 1:
        return sieve()
    return dict()


def main() raises:
    var names = List[String]()
    names.append("fib")
    names.append("sieve")
    names.append("dict")
    for w in range(3):
        var best: Float64 = 1.0e30
        var r: Int64 = 0
        for _ in range(3):
            var t = system.monotonic_time()
            var runs = 0
            while True:
                r = work(w)
                runs += 1
                if system.monotonic_time() - t >= 50:
                    break
            var ms = Float64(system.monotonic_time() - t) * 10.0 / Float64(runs)
            if ms < best:
                best = ms
        print("mojo", names[w], Float64(Int(best * 10.0 + 0.5)) / 10.0, r)
