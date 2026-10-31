\ bench.fth -- the BOX language comparison in Forth (*F66, the Arm box):
\ fib(30) by recursion, a sieve of the primes below 2,000,000, and 200,000
\ string keys put in a hash table and read back.  F66 has no hash table, so
\ it is made of cells: open addressing, a cell for each slot's key (the
\ address of a counted string, 0 for none) and one for its value.
\ OS_ReadMonotonicTime counts centiseconds, so each test repeats until half
\ a second has gone; the best of three, in milliseconds.
2000000 constant n
200000 constant keys
524288 constant cap
: ticks ( -- cs ) 0 swi" OS_ReadMonotonicTime" sys r0 ;

: fib ( n -- f ) dup 2 < 0= if dup 1- recurse swap 2 - recurse + then ;

create flags n 1+ allot
: sieve ( -- count )
  flags n 1+ 0 fill  0
  n 1+ 2 do
    flags i + c@ 0= if
      1+  i 1415 < if  n 1+ i i * do 1 flags i + c! j +loop  then
    then
  loop ;

variable ktab  cap cells allocate throw ktab !
variable vtab  cap cells allocate throw vtab !
variable pool  keys 8 * allocate throw pool !   \ the keys, 8 bytes each
variable ptop
create kbuf 16 allot

\ "k" and the digits of i, a counted string at the end of kbuf
: key ( i -- c-addr )
  kbuf 16 + swap
  begin 10 /mod swap 48 + rot 1- tuck c! swap dup 0= until drop
  1- 107 over c!
  kbuf 16 + over - over 1- c! 1- ;

\ FNV-1a, 32 bits
: hash ( addr u -- h )
  2166136261 -rot over + swap do i c@ xor 16777619 * 4294967295 and loop ;

\ the key's slot: the one it is in, or the empty one where it goes
: slot ( c-addr -- i )
  dup count hash cap 1- and
  begin ktab @ over cells + @ dup
    if 2 pick count rot count compare 0<> else drop 0 then
  while 1+ cap 1- and repeat nip ;

: dict ( -- sum )
  ktab @ cap cells 0 fill  pool @ ptop !
  keys 1+ 1 do
    i key dup slot
    ktab @ over cells + @ 0= if
      over dup c@ 1+ ptop @ swap move
      ptop @ ktab @ 2 pick cells + !  8 ptop +!
    then
    nip i 2* swap cells vtab @ + !
  loop
  0 keys 1+ 1 do i key slot cells vtab @ + @ + loop ;

: work ( w -- r ) dup 0= if drop 30 fib else 1 = if sieve else dict then then ;

\ u's digits, with no space after them (. adds one)
: .n ( u -- ) 10 /mod ?dup if recurse then 48 + emit ;

variable t0  variable runs  variable e  variable r  variable best
\ test w, repeated until half a second has gone: e centiseconds, runs times
: timed ( w -- )
  ticks t0 !  0 runs !
  begin dup work r !  1 runs +!  ticks t0 @ - dup e !  49 > until drop ;
\ the best of three, in hundredths of a millisecond
: bench ( w c-addr u -- )
  1000000000 best !
  3 0 do 2 pick timed  e @ 1000 * runs @ /  best @ min best ! loop
  ." forth " type space
  best @ 100 /mod .n 46 emit dup 10 < if 48 emit then .n
  space r @ .n cr drop ;

: main  0 s" fib" bench  1 s" sieve" bench  2 s" dict" bench ;
main
