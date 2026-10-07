/*
 * CDDL HEADER START
 *
 * The contents of this file are subject to the terms of the
 * Common Development and Distribution License (the "Licence").
 * You may not use this file except in compliance with the Licence.
 *
 * You can obtain a copy of the licence at
 * RiscOS/Sources/FileSys/ImageFS/SparkFS/Codecs/SparkTar/LICENCE.
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
/* tarinfo.c -- SparkTar's s/info (FileSys/ImageFS/SparkFS/Codecs/SparkTar),
 * in C: the tables the codec gives SparkFS (SparkFS_Info, link->info),
 * word for word as the ObjAsm lays them out.
 *
 *   archivetypes:     7 (TAR), file type &C46, flags 0; AnyLocal ListEnd;
 *                     AnyLocal ListEnd; "Tar" "new/tar", aligned; ListEnd
 *   compressiontypes: &200 Private "Unix"; &201 Private "Arctar"; &203
 *                     Private "Comma"; &204 Private "FLtar" (each 8
 *                     bytes); ListEnd
 *   codetypes:        0 Private "None"; ListEnd
 *   conversiontypes:  ListEnd
 *
 * Writable, as the ObjAsm's |C$$data| (SparkFS writes a mapped file type
 * into archivetypes).
 *
 * CDDL 1.0, as SparkTar's own; the original: Copyright 1992 David Pilling.
 * All rights reserved.  Use is subject to license terms.
 */
#define PRIVATE   2
#define ANYLOCAL  (-2)
#define LISTEND   (-1)

#define W(a, b, c, d) ((int)((unsigned)(a) | (unsigned)(b) << 8 | (unsigned)(c) << 16 | (unsigned)(d) << 24))

int archivetypes[] = {
    0x00000007, 0xC46, 0,
    ANYLOCAL, LISTEND,
    ANYLOCAL, LISTEND,
    W('T', 'a', 'r', 0), W('n', 'e', 'w', '/'), W('t', 'a', 'r', 0),
    LISTEND,
};

int compressiontypes[] = {
    0x00000200, PRIVATE, W('U', 'n', 'i', 'x'), W(0, 0, 0, 0),
    0x00000201, PRIVATE, W('A', 'r', 'c', 't'), W('a', 'r', 0, 0),
    0x00000203, PRIVATE, W('C', 'o', 'm', 'm'), W('a', 0, 0, 0),
    0x00000204, PRIVATE, W('F', 'L', 't', 'a'), W('r', 0, 0, 0),
    LISTEND,
};

int codetypes[] = {
    0x00000000, PRIVATE,
    W('N', 'o', 'n', 'e'), W(0, 0, 0, 0),
    LISTEND,
};

int conversiontypes[] = {
    LISTEND,
};
