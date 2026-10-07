/* Interface/MimeMap.h for SparkFS's C: MimeMap's hdr/MimeMap
 * (Networking/MimeMap), as the DDE's Hdr2H would export it, with its SWI
 * (Global/SWIs.h's MimeMapperSWI_Base, &50B00), which the staged swis.h
 * does not name. */
#ifndef INTERFACE_MIMEMAP_H
#define INTERFACE_MIMEMAP_H
#ifndef MimeMap_Translate
#define MimeMap_Translate      0x50B00
#endif
#define MMM_TYPE_RISCOS        0        /* RISC OS file type passed as an int */
#define MMM_TYPE_RISCOS_STRING 1        /* RISC OS file type passed as a char* */
#define MMM_TYPE_MIME          2        /* MIME content type passed as a char* */
#define MMM_TYPE_DOT_EXTN      3        /* File extention as a char* */
#define MMM_TYPE_MAC           4        /* Apple type/creator passed as a char* */
#define MMM_TYPE_DOT_EXTNS     5        /* File extentions as a char** (output only) */
#endif
