//! bench.rs -- the language comparison in Rust: fib(30), a sieve of the
//! primes below 2,000,000, and 200,000 string keys in a HashMap; the best
//! of three, in milliseconds.  Built as a static Linux program and run in
//! the box by *RunBox, as the box's Rust engines are.
use std::collections::HashMap;
use std::time::Instant;

fn fib(n: u32) -> u64 {
    if n < 2 { n as u64 } else { fib(n - 1) + fib(n - 2) }
}

fn sieve() -> u64 {
    let n = 2_000_000usize;
    let mut s = vec![false; n + 1];
    let mut c = 0;
    for i in 2..=n {
        if !s[i] {
            c += 1;
            let mut j = i * i;
            while j <= n {
                s[j] = true;
                j += i;
            }
        }
    }
    c
}

fn dict() -> u64 {
    let mut d: HashMap<String, u64> = HashMap::new();
    for i in 1..=200_000u64 {
        d.insert(format!("k{i}"), i * 2);
    }
    let mut s = 0;
    for i in 1..=200_000u64 {
        s += d[&format!("k{i}")];
    }
    s
}

fn best(name: &str, f: fn() -> u64) {
    let mut b = f64::MAX;
    let mut r = 0;
    for _ in 0..3 {
        let t = Instant::now();
        r = std::hint::black_box(f());
        b = b.min(t.elapsed().as_secs_f64() * 1000.0);
    }
    println!("rust {name} {b:.1} {r}");
}

fn main() {
    best("fib", || fib(std::hint::black_box(30)));
    best("sieve", sieve);
    best("dict", dict);
}
