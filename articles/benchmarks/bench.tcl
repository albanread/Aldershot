# bench.tcl -- the language comparison in Tcl: fib(30), a sieve below 2,000,000, 200,000 string keys in a dict; the best of three, in milliseconds
proc fib {n} {
    if {$n < 2} { return $n }
    return [expr {[fib [expr {$n - 1}]] + [fib [expr {$n - 2}]]}]
}
proc sieve {} {
    set n 2000000
    set s [lrepeat [expr {$n + 1}] 1]
    set c 0
    for {set i 2} {$i <= $n} {incr i} {
        if {[lindex $s $i]} {
            incr c
            for {set j [expr {$i * $i}]} {$j <= $n} {incr j $i} { lset s $j 0 }
        }
    }
    return $c
}
proc dict_ {} {
    set n 200000
    set d [dict create]
    for {set i 1} {$i <= $n} {incr i} { dict set d k$i [expr {$i * 2}] }
    set s 0
    for {set i 1} {$i <= $n} {incr i} { incr s [dict get $d k$i] }
    return $s
}
proc best {name script} {
    set b {}
    for {set k 0} {$k < 3} {incr k} {
        set t [clock microseconds]
        set r [uplevel #0 $script]
        set ms [expr {([clock microseconds] - $t) / 1000.0}]
        if {$b eq {} || $ms < $b} { set b $ms }
    }
    puts "tcl $name [format %.1f $b] $r"
}
best fib {fib 30}
best sieve {sieve}
best dict {dict_}
