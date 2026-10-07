/*
 * CDDL HEADER START
 *
 * The contents of this file are subject to the terms of the
 * Common Development and Distribution License (the "Licence").
 * You may not use this file except in compliance with the Licence.
 *
 * You can obtain a copy of the licence at
 * RiscOS/Sources/FileSys/ImageFS/SparkFS/Codecs/SparkSpark/LICENCE.
 * See the Licence for the specific language governing permissions
 * and limitations under the Licence.
 *
 * When distributing Covered Code, include this CDDL HEADER in each
 * file and include the Licence file. If applicable, add the
 * following below this CDDL HEADER, with the fields enclosed by
 * brackets "[]" replaced with your own identifying information:
 * Portions Copyright [yyyy] [name of copyright owner]
 *
 * CDDL HEADER END
 *
 * Copyright 1992 David Pilling.  All rights reserved.
 * Use is subject to license terms.
 */
/* sparkinfo.c -- SparkSpark's s/info (FileSys/ImageFS/SparkFS/Codecs/SparkSpark),
 * in C: the tables the codec gives SparkFS (SparkFS_Info, link->info),
 * word for word as the ObjAsm lays them out.
 *
 *   archivetypes:     1 Spark file (&DDC), 2 Spark dir (&2000, APP: an
 *                     application directory, any public or local methods and codes),
 *                     3 PK arc (&DDC; methods 0, &C, 1, 6; code Garble),
 *                     8 ArcFS (&DDC, read only); each its methods, codes,
 *                     name and new-archive name, aligned; ListEnd
 *   compressiontypes: 0 "No compression", &C Squeeze, 1-5 Crunch 12-16,
 *                     6 Squash, 7-11 Compress 12-16 (Crunch 13-16 and
 *                     Compress 13-15 read only, Private+ReadOnly); ListEnd
 *   codetypes:        0 None, 1 Garble, 2 DES; ListEnd
 *   conversiontypes:  pairs (from, to) of archive types: Spark file to
 *                     Spark dir and back, ArcFS to Spark dir, ...; ListEnd
 *
 * Writable, as the ObjAsm's |C$$data| (SparkFS writes a mapped file type
 * into archivetypes: *SparkFSMapFiletype).
 *
 * CDDL 1.0, as SparkSpark's own; the original: Copyright 1992 David Pilling.
 * All rights reserved.  Use is subject to license terms.
 */
#define READONLY   1
#define PRIVATE    2
#define ANYPUBLIC  (-3)
#define ANYLOCAL   (-2)
#define LISTEND    (-1)
#define GARBLE     1
#define SPARKFILE  1
#define SPARKDIR   2
#define PKARC      3
#define ARCFS      8

/* Strings in words, little-endian, as DCB lays them down */
#define W(a, b, c, d) ((int)((unsigned)(a) | (unsigned)(b) << 8 | (unsigned)(c) << 16 | (unsigned)(d) << 24))

int archivetypes[] = {
    0x00000001,
    0xDDC,
    0,
    ANYLOCAL,
    LISTEND,
    ANYLOCAL,
    LISTEND,
    W('S', 'p', 'a', 'r'), W('k', ' ', 'f', 'i'), W('l', 'e', 0, 'n'), W('e', 'w', '/', 'a'), W('r', 'c', 0, 0),
    0x00000002,
    0x2000,
    0,
    ANYPUBLIC,
    ANYLOCAL,
    LISTEND,
    ANYPUBLIC,
    ANYLOCAL,
    LISTEND,
    W('S', 'p', 'a', 'r'), W('k', ' ', 'd', 'i'), W('r', 0, '!', 'n'), W('e', 'w', '/', 'a'), W('r', 'c', 0, 0),
    0x00000003,
    0xDDC,
    0,
    0x00000000,
    0x0000000C,
    0x00000001,
    0x00000006,
    LISTEND,
    GARBLE,
    LISTEND,
    W('P', 'K', ' ', 'a'), W('r', 'c', 0, 'n'), W('e', 'w', '/', 'a'), W('r', 'c', 0, 0),
    0x00000008,
    0xDDC,
    READONLY,
    ANYLOCAL,
    LISTEND,
    ANYLOCAL,
    LISTEND,
    W('A', 'r', 'c', 'F'), W('S', 0, 'n', 'e'), W('w', '/', 'a', 'r'), W('c', 0, 0, 0),
    LISTEND,
};

int compressiontypes[] = {
    0x00000000,
    PRIVATE,
    W('N', 'o', ' ', 'c'), W('o', 'm', 'p', 'r'), W('e', 's', 's', 'i'), W('o', 'n', 0, 0),
    0x0000000C,
    PRIVATE,
    W('S', 'q', 'u', 'e'), W('e', 'z', 'e', 0),
    0x00000001,
    PRIVATE,
    W('C', 'r', 'u', 'n'), W('c', 'h', ' ', '1'), W('2', 0, 0, 0),
    0x00000002,
    PRIVATE + READONLY,
    W('C', 'r', 'u', 'n'), W('c', 'h', ' ', '1'), W('3', 0, 0, 0),
    0x00000003,
    PRIVATE + READONLY,
    W('C', 'r', 'u', 'n'), W('c', 'h', ' ', '1'), W('4', 0, 0, 0),
    0x00000004,
    PRIVATE + READONLY,
    W('C', 'r', 'u', 'n'), W('c', 'h', ' ', '1'), W('5', 0, 0, 0),
    0x00000005,
    PRIVATE + READONLY,
    W('C', 'r', 'u', 'n'), W('c', 'h', ' ', '1'), W('6', 0, 0, 0),
    0x00000006,
    PRIVATE,
    W('S', 'q', 'u', 'a'), W('s', 'h', 0, 0),
    0x00000007,
    PRIVATE,
    W('C', 'o', 'm', 'p'), W('r', 'e', 's', 's'), W(' ', '1', '2', 0),
    0x00000008,
    PRIVATE + READONLY,
    W('C', 'o', 'm', 'p'), W('r', 'e', 's', 's'), W(' ', '1', '3', 0),
    0x00000009,
    PRIVATE + READONLY,
    W('C', 'o', 'm', 'p'), W('r', 'e', 's', 's'), W(' ', '1', '4', 0),
    0x0000000A,
    PRIVATE + READONLY,
    W('C', 'o', 'm', 'p'), W('r', 'e', 's', 's'), W(' ', '1', '5', 0),
    0x0000000B,
    PRIVATE,
    W('C', 'o', 'm', 'p'), W('r', 'e', 's', 's'), W(' ', '1', '6', 0),
    LISTEND,
};

int codetypes[] = {
    0x00000000,
    PRIVATE,
    W('N', 'o', 'n', 'e'), W(0, 0, 0, 0),
    0x00000001,
    PRIVATE,
    W('G', 'a', 'r', 'b'), W('l', 'e', 0, 0),
    0x00000002,
    PRIVATE,
    W('D', 'E', 'S', 0),
    LISTEND,
};

int conversiontypes[] = {
    SPARKFILE,
    SPARKDIR,
    ARCFS,
    SPARKDIR,
    SPARKDIR,
    SPARKFILE,
    SPARKDIR,
    PKARC,
    ARCFS,
    SPARKFILE,
    ARCFS,
    PKARC,
    LISTEND,
};
