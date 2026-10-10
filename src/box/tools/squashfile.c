/* squashfile.c -- squashfile IN OUT TYPE: a file squashed as RISC OS's
 * build squashes resources (its squash tool), for the ROM's ResourceFS.
 *
 * Built for this machine with Squash's own compressor, c/cssr from the
 * RISC OS sources (Programmer/Squash): 12-bit LZW, in compress's format.
 * The file is Squash's: "SQSH", the length unsquashed, the original's load
 * and exec addresses -- file type TYPE, dated 0, as tools/mkresources.py
 * dates the ROM's files -- a reserved word, then the squashed data.  The
 * Desktop unsquashes its banner's sprites from it (Squash_Decompress). */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "cssr.h"

static void word(FILE *f, uint32_t w)
{
    for (int i = 0; i < 4; i++)
        fputc((int)(w >> 8 * i & 0xFF), f);
}

int main(int argc, char **argv)
{
    if (argc != 4) {
        fprintf(stderr, "usage: squashfile IN OUT TYPE\n");
        return 2;
    }
    FILE *f = fopen(argv[1], "rb");
    if (!f) {
        perror(argv[1]);
        return 1;
    }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    rewind(f);
    unsigned char *in = malloc((size_t)n + 1);
    if (!in || fread(in, 1, (size_t)n, f) != (size_t)n) {
        fprintf(stderr, "squashfile: cannot read %s\n", argv[1]);
        return 1;
    }
    fclose(f);

    unsigned int size = (unsigned int)compress_maximum_output_size((size_t)n);
    char *out = malloc(size);
    compress_state *ws = calloc(1, sizeof *ws);
    if (!out || !ws)
        return 1;
    ws->starting = 1;
    unsigned char *p = in;
    if (compress_store_store(&p, (unsigned int)n, out, &size, ws, 0) != output_ok) {
        fprintf(stderr, "squashfile: %s did not squash\n", argv[1]);
        return 1;
    }

    f = fopen(argv[2], "wb");
    if (!f) {
        perror(argv[2]);
        return 1;
    }
    fputs("SQSH", f);
    word(f, (uint32_t)n);
    word(f, 0xFFF00000u | (uint32_t)strtoul(argv[3], NULL, 16) << 8);
    word(f, 0);
    word(f, 0);
    fwrite(out, 1, size, f);
    return fclose(f) != 0;
}
