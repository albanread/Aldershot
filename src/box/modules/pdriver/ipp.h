/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Alban Read */

/* ipp.h -- the Internet Printing Protocol, enough of it to print
 * (modules/pdriver/ipp.c).
 */
#ifndef ROSGD_IPP_H
#define ROSGD_IPP_H

#include <stddef.h>
#include <stdint.h>

#include "rosgd/error.h"
#include "pdriver.h"

#define IPP_ERR (PDRIVER_ERRBASE + 15)

/* The tags RFC 8010 gives the things this uses */
#define IPP_TAG_OPERATION   0x01u
#define IPP_TAG_JOB         0x02u
#define IPP_TAG_END         0x03u
#define IPP_TAG_INTEGER     0x21u
#define IPP_TAG_ENUM        0x23u
#define IPP_TAG_RESOLUTION  0x32u
#define IPP_TAG_TEXT        0x41u
#define IPP_TAG_NAME        0x42u
#define IPP_TAG_KEYWORD     0x44u
#define IPP_TAG_URI         0x45u
#define IPP_TAG_CHARSET     0x47u
#define IPP_TAG_LANGUAGE    0x48u
#define IPP_TAG_MIMETYPE    0x49u

#define IPP_OP_PRINT_JOB                0x0002u
#define IPP_OP_GET_JOB_ATTRIBUTES       0x0009u
#define IPP_OP_GET_PRINTER_ATTRIBUTES   0x000Bu

/* What a printer takes.  IPP Everywhere requires PWG Raster of every
 * printer and only recommends PDF. */
#define IPP_FMT_PDF   (1u << 0)
#define IPP_FMT_PWG   (1u << 1)
#define IPP_FMT_URF   (1u << 2)
#define IPP_FMT_JPEG  (1u << 3)

#define IPP_COLOUR_SRGB  (1u << 0)
#define IPP_COLOUR_SGRAY (1u << 1)

struct ipp_printer {
    uint32_t formats;
    uint32_t colour;
    uint32_t dpi;
    uint32_t state;                     /* 3 idle, 4 processing, 5 stopped */
    char model[64];
    char media[64];
};

os_error *ipp_ask(const char *uri, struct ipp_printer *out);
os_error *ipp_print(const char *uri, const char *job_name, const char *format,
                    const void *doc, size_t doclen, uint32_t copies, uint32_t *job_id);
const char *ipp_format_name(uint32_t format);

#endif
