MODULE bench;
(* bench.mod -- the BOX language comparison in Modula-2: fib(30) by recursion,
   a sieve of the primes below 2,000,000, and 200,000 string keys put in a
   hash table and read back.  The library has no hash table, so, as BBC
   BASIC's program does, it is made of arrays: open addressing, the keys
   made and compared by the ISO Strings and WholeStr modules.
   OS_ReadMonotonicTime counts centiseconds, so each test repeats until half
   a second has gone; the best of three, in milliseconds. *)
IMPORT SWI;
FROM STextIO IMPORT WriteString, WriteLn;
FROM SWholeIO IMPORT WriteInt;
FROM SRealIO IMPORT WriteFixed;
FROM Strings IMPORT Concat, Equal, Length;
FROM WholeStr IMPORT IntToStr;

CONST N = 2000000; KEYS = 200000; CAP = 524288;
TYPE Key = ARRAY [0..15] OF CHAR;
VAR sv: ARRAY [0..N] OF BOOLEAN;
    ks: ARRAY [0..CAP - 1] OF Key;      (* a slot's key *)
    vs: ARRAY [0..CAP - 1] OF INTEGER;  (* its value *)
    gs: ARRAY [0..CAP - 1] OF INTEGER;  (* the run that filled it *)
    gen: INTEGER;

PROCEDURE Fib(n: INTEGER): INTEGER;
BEGIN
  IF n < 2 THEN RETURN n ELSE RETURN Fib(n - 1) + Fib(n - 2) END
END Fib;

PROCEDURE Sieve(): INTEGER;
VAR i, j, c: INTEGER;
BEGIN
  FOR i := 2 TO N DO sv[i] := FALSE END;
  c := 0;
  FOR i := 2 TO N DO
    IF NOT sv[i] THEN
      INC(c);
      IF i <= 1414 THEN
        j := i * i;
        WHILE j <= N DO sv[j] := TRUE; INC(j, i) END
      END
    END
  END;
  RETURN c
END Sieve;

PROCEDURE Hash(k: ARRAY OF CHAR): INTEGER;
VAR h, j: INTEGER;
BEGIN
  h := 0;
  FOR j := 0 TO VAL(INTEGER, Length(k)) - 1 DO
    h := (h * 31 + VAL(INTEGER, ORD(k[j]))) MOD 800000H
  END;
  RETURN h MOD CAP
END Hash;

PROCEDURE Dict(): INTEGER;
VAR i, h, s: INTEGER; d: ARRAY [0..11] OF CHAR; k: Key;
BEGIN
  INC(gen);
  FOR i := 1 TO KEYS DO
    IntToStr(i, d); Concat("k", d, k);
    h := Hash(k);
    WHILE (gs[h] = gen) AND NOT Equal(ks[h], k) DO h := (h + 1) MOD CAP END;
    gs[h] := gen; ks[h] := k; vs[h] := i * 2
  END;
  s := 0;
  FOR i := 1 TO KEYS DO
    IntToStr(i, d); Concat("k", d, k);
    h := Hash(k);
    WHILE NOT ((gs[h] = gen) AND Equal(ks[h], k)) DO h := (h + 1) MOD CAP END;
    INC(s, vs[h])
  END;
  RETURN s
END Dict;

PROCEDURE Work(w: INTEGER): INTEGER;
BEGIN
  IF w = 1 THEN RETURN Fib(30) ELSIF w = 2 THEN RETURN Sieve() ELSE RETURN Dict() END
END Work;

PROCEDURE Now(): INTEGER;
VAR r: SWI.Regs;
BEGIN
  SWI.Call(42H, r);   (* OS_ReadMonotonicTime *)
  RETURN VAL(INTEGER, r[0])
END Now;

PROCEDURE Best(name: ARRAY OF CHAR; w: INTEGER);
VAR b, ms: REAL; k, t, c, e, r: INTEGER;
BEGIN
  b := 1.0E9;
  FOR k := 1 TO 3 DO
    t := Now(); c := 0;
    REPEAT r := Work(w); INC(c); e := Now() - t UNTIL e >= 50;
    ms := FLOAT(e) * 10.0 / FLOAT(c);
    IF ms < b THEN b := ms END
  END;
  WriteString("modula2 "); WriteString(name); WriteString(" ");
  WriteFixed(b, 2, 1); WriteString(" "); WriteInt(r, 1); WriteLn
END Best;

BEGIN
  gen := 0;
  Best("fib", 1);
  Best("sieve", 2);
  Best("dict", 3)
END bench.
