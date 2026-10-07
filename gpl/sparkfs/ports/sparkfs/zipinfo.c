/*
 * CDDL HEADER START
 *
 * The contents of this file are subject to the terms of the
 * Common Development and Distribution License (the "Licence").
 * You may not use this file except in compliance with the Licence.
 *
 * You can obtain a copy of the licence at
 * RiscOS/Sources/FileSys/ImageFS/SparkFS/Codecs/SparkZip/LICENCE.
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
/* zipinfo.c -- SparkZip's s/info (FileSys/ImageFS/SparkFS/Codecs/SparkZip),
 * in C: the tables the codec gives SparkFS (SparkFS_Info, link->info),
 * word for word as the ObjAsm lays them out.
 *
 *   archivetypes:     4 (ZIP), file type &A91, flags 0; AnyLocal ListEnd
 *                     (the compression methods' list); AnyLocal ListEnd
 *                     (the codes'); "Zip" "new/zip", aligned; ListEnd
 *   compressiontypes: 0 Private "No compression" (2 NULs); &108 Private
 *                     "Deflate"; ListEnd
 *   codetypes:        0 Private "None" (4 NULs); ListEnd
 *   conversiontypes:  ListEnd
 *
 * In |C$$data| in the ObjAsm, so writable: SparkFS's *SparkFSMapFiletype
 * writes a new file type into archivetypes (link.c, imtypes).
 *
 * CDDL 1.0, as SparkZip's own; the original: Copyright 1992 David Pilling.
 * All rights reserved.  Use is subject to license terms.
 */
#define PRIVATE   2
#define ANYLOCAL  (-2)
#define LISTEND   (-1)

/* Strings in words, little-endian, as DCB lays them down */
#define W(a, b, c, d) ((int)((unsigned)(a) | (unsigned)(b) << 8 | (unsigned)(c) << 16 | (unsigned)(d) << 24))

int archivetypes[] = {
    0x00000004, 0xA91, 0,
    ANYLOCAL, LISTEND,
    ANYLOCAL, LISTEND,
    W('Z', 'i', 'p', 0), W('n', 'e', 'w', '/'), W('z', 'i', 'p', 0),
    LISTEND,
};

int compressiontypes[] = {
    0x00000000, PRIVATE,
    W('N', 'o', ' ', 'c'), W('o', 'm', 'p', 'r'), W('e', 's', 's', 'i'), W('o', 'n', 0, 0),
    0x00000108, PRIVATE,
    W('D', 'e', 'f', 'l'), W('a', 't', 'e', 0),
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
