10 REM bench -- the language comparison in BBC BASIC: fib(30), a sieve of
20 REM the primes below 2,000,000, and 200,000 string keys in a hash table
30 REM made of arrays (BASIC has none of its own), open addressing.  TIME counts
40 REM centiseconds, so each test is repeated until half a second has gone;
50 REM the best of three, in milliseconds.  Run as it is by BASIC, or compiled
60 REM by *RosBas.
70 N%=2000000: DIM S% N%+1
80 C%=524288: DIM K$(C%-1), V(C%-1), G%(C%-1): gen%=0
90 PROCbest("fib",1): PROCbest("sieve",2): PROCbest("dict",3)
100 END
110 :
120 DEF PROCbest(name$,w%)
130 LOCAL b,k%,t%,c%,e%,r
140 b=1E30
150 FOR k%=1 TO 3
160   t%=TIME: c%=0
170   REPEAT r=FNwork(w%): c%+=1: e%=TIME-t%: UNTIL e%>=50
180   IF e%*10/c%<b THEN b=e%*10/c%
190 NEXT
200 @%=&2010A: PRINT "basic ";name$;" ";b;
210 @%=&20012: PRINT " ";r
220 ENDPROC
230 :
240 DEF FNwork(w%)
250 IF w%=1 THEN =FNfib(30)
260 IF w%=2 THEN =FNsieve
270 =FNdict
280 :
290 DEF FNfib(n%) IF n%<2 THEN =n% ELSE =FNfib(n%-1)+FNfib(n%-2)
300 :
310 DEF FNsieve
320 LOCAL i%,j%,c%
330 FOR i%=2 TO N%: S%?i%=0: NEXT
340 c%=0
350 FOR i%=2 TO N%
360   IF S%?i%=0 THEN
370     c%+=1
380     IF i%<=1414 THEN FOR j%=i%*i% TO N% STEP i%: S%?j%=1: NEXT
390   ENDIF
400 NEXT
410 =c%
420 :
430 DEF FNhash(k$)
440 LOCAL h%,j%
450 h%=0
460 FOR j%=1 TO LEN k$: h%=(h%*31+ASC MID$(k$,j%,1)) AND &7FFFFF: NEXT
470 =h% AND (C%-1)
480 :
490 DEF FNdict
500 LOCAL i%,h%,s,k$
510 gen%+=1
520 FOR i%=1 TO 200000
530   k$="k"+STR$i%: h%=FNhash(k$)
540   WHILE G%(h%)=gen% AND K$(h%)<>k$: h%=(h%+1) AND (C%-1): ENDWHILE
550   G%(h%)=gen%: K$(h%)=k$: V(h%)=i%*2
560 NEXT
570 s=0
580 FOR i%=1 TO 200000
590   k$="k"+STR$i%: h%=FNhash(k$)
600   WHILE NOT (G%(h%)=gen% AND K$(h%)=k$): h%=(h%+1) AND (C%-1): ENDWHILE
610   s+=V(h%)
620 NEXT
630 =s
