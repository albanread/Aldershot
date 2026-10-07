/*
 * CDDL HEADER START
 *
 * The contents of this file are subject to the terms of the
 * Common Development and Distribution License (the "Licence").
 * You may not use this file except in compliance with the Licence.
 *
 * You can obtain a copy of the licence at
 * RiscOS/Sources/FileSys/ImageFS/SparkFS/Codecs/SparkCPIO/LICENCE.
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
/* zooinfo.c -- SparkZoo's s/info (FileSys/ImageFS/SparkFS/Codecs/SparkZoo),
 * in C: the tables the codec gives SparkFS (SparkFS_Info, link->info),
 * word for word as the ObjAsm lays them out.
 *
 *   archivetypes:     5 (Zoo), file type &DDC, read only; AnyLocal ListEnd;
 *                     AnyLocal ListEnd; "Zoo" "new/zoo", aligned; ListEnd
 *   compressiontypes: ListEnd
 *   codetypes:        0 Private "None" (4 NULs); ListEnd
 *   conversiontypes:  ListEnd
 *
 * Writable, as the ObjAsm's |C$$data| (SparkFS writes a mapped file type
 * into archivetypes: *SparkFSMapFiletype).
 *
 * CDDL 1.0, as SparkZoo's own; the original: Copyright 1992 David Pilling.
 * All rights reserved.  Use is subject to license terms.
 */
#define READONLY   1
#define PRIVATE    2
#define ANYPUBLIC  (-3)
#define ANYLOCAL   (-2)
#define LISTEND    (-1)

/* Strings in words, little-endian, as DCB lays them down */
#define W(a, b, c, d) ((int)((unsigned)(a) | (unsigned)(b) << 8 | (unsigned)(c) << 16 | (unsigned)(d) << 24))

int archivetypes[] = {
    0x00000005,
    0xDDC,
    READONLY,
    ANYLOCAL,
    LISTEND,
    ANYLOCAL,
    LISTEND,
    W('Z', 'o', 'o', 0), W('n', 'e', 'w', '/'), W('z', 'o', 'o', 0),
    LISTEND,
};

int compressiontypes[] = {
    LISTEND,
};

int codetypes[] = {
    0x00000000,
    PRIVATE,
    W('N', 'o', 'n', 'e'), W(0, 0, 0, 0),
    LISTEND,
};

int conversiontypes[] = {
    LISTEND,
};
