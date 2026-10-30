# the language comparison: fib(30), a sieve below 2,000,000, 200,000 string keys
import time
def fib(n):
    return n if n < 2 else fib(n - 1) + fib(n - 2)
def sieve():
    n = 2000000
    s = [True] * (n + 1)
    c = 0
    for i in range(2, n + 1):
        if s[i]:
            c += 1
            for j in range(i * i, n + 1, i):
                s[j] = False
    return c
def dict_():
    n = 200000
    d = {}
    for i in range(1, n + 1):
        d["k" + str(i)] = i * 2
    s = 0
    for i in range(1, n + 1):
        s += d["k" + str(i)]
    return s
def best(name, f):
    b = None
    for _ in range(3):
        t = time.perf_counter()
        r = f()
        ms = (time.perf_counter() - t) * 1000
        b = ms if b is None else min(b, ms)
    print("python %s %.1f %s" % (name, b, r))
best("fib", lambda: fib(30))
best("sieve", sieve)
best("dict", dict_)
