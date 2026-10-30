"bench.st -- the language comparison in Smalltalk: fib(30), a sieve of
 the primes below 2,000,000, and 200,000 string keys in a Dictionary; the
 best of three, in milliseconds, in the Transcript.  *Smalltalk bench OUT
 writes them to the file OUT (a Linux name) as well."
Object subclass: Bench [
    Bench class >> fib: n [ n < 2 ifTrue: [ ^n ]. ^(self fib: n - 1) + (self fib: n - 2) ]

    Bench class >> sieve [
        | n s c |
        n := 2000000.
        s := ByteArray new: n.
        c := 0.
        2 to: n do: [ :i |
            (s at: i) = 0 ifTrue: [
                c := c + 1.
                i * i to: n by: i do: [ :j | s at: j put: 1 ] ] ].
        ^c
    ]

    Bench class >> dict [
        | n d s |
        n := 200000.
        d := Dictionary new.
        1 to: n do: [ :i | d at: 'k' , i printString put: i * 2 ].
        s := 0.
        1 to: n do: [ :i | s := s + (d at: 'k' , i printString) ].
        ^s
    ]

    Bench class >> best: name of: aBlock on: out [
        | b r |
        b := nil.
        3 timesRepeat: [ | t ms |
            t := Smalltalk microsecondClock.
            r := aBlock value.
            ms := Smalltalk microsecondClock - t.
            (b isNil or: [ ms < b ]) ifTrue: [ b := ms ] ].
        out nextPutAll: 'smalltalk ' , name , ' ' , (b // 1000) printString , '.' , (b \\ 1000 // 100) printString , ' ' , r printString; nextPut: Character nl
    ]

    Bench class >> start: args [
        | out |
        out := WriteStream on: (String new: 200).
        self best: 'fib' of: [ self fib: 30 ] on: out.
        self best: 'sieve' of: [ self sieve ] on: out.
        self best: 'dict' of: [ self dict ] on: out.
        Transcript show: out contents.
        args isEmpty ifFalse: [
            Box query: 'write' , (String with: Character tab) , (args at: 1) , (String with: Character tab) , out contents ]
    ]
]
