/* roscmem.c -- raw memory for Paige over the C library's heap.
 *
 * Paige's memory_ref is a pointer to a four-byte *cell* holding the
 * mem_rec's address: resizing moves the block, never the cell, so refs
 * held in the engine's master list stay good.  This is the Mac Handle
 * bargain, which is what the engine's platform branches assume, with an
 * 8-byte header (the cell pointer, the size) ahead of the mem_rec. */
#include <stddef.h>
#include <stdint.h>

#include <stdlib.h>

struct rosc_hdr {
    void *cell;                 /* the ref: a pointer to the 4-byte cell */
    unsigned size;              /* the payload size, not counting the header */
};

#define ROSC_HDR (sizeof(struct rosc_hdr))

void *rosc_block_of(void *ref);

void *rosc_alloc(long size)
{
    struct rosc_hdr *h;
    void **cell;
    if (size <= 0)
        size = 1;
    h = malloc((size_t)size + ROSC_HDR);
    if (!h)
        return NULL;
    cell = malloc(sizeof *cell);
    if (!cell) {
        free(h);
        return NULL;
    }
    h->cell = cell;
    h->size = (unsigned)size;
    *cell = (char *)h + ROSC_HDR;       /* the mem_rec */
    return cell;                        /* the memory_ref */
}

void *rosc_alloc_clear(long size)
{
    unsigned char *p;
    long i;
    void *ref = rosc_alloc(size);
    if (!ref)
        return NULL;
    p = rosc_block_of(ref);
    for (i = 0; i < size; i++)
        p[i] = 0;
    return ref;
}

/* the ref's mem_rec, as *(Handle) is on the Mac */
void *rosc_block_of(void *ref)
{
    void **cell = ref;
    if (!ref)
        return NULL;
    return *cell;
}

long rosc_memsize(void *ref)
{
    struct rosc_hdr *h;
    if (!ref)
        return 0;
    h = (struct rosc_hdr *)((char *)*(void **)ref - ROSC_HDR);
    if (!h)
        return 0;
    return (long)h->size;
}

/* resize: the block may move; the cell does not.  Returns the new
 * mem_rec pointer, NULL (with *errp set) when it could not. */
void *rosc_resize_err(void *ref, long size, void *errp)
{
    struct rosc_hdr *h, *nh;
    void **cell = ref;
    if (!ref || size <= 0)
        size = 1;
    h = (struct rosc_hdr *)((char *)*cell - ROSC_HDR);
    if (!h)
        return NULL;
    nh = realloc(h, (size_t)size + ROSC_HDR);
    if (!nh) {
        if (errp)
            *(unsigned short *)errp = 1;    /* NO_MEMORY_ERR */
        return NULL;
    }
    nh->size = (unsigned)size;
    *cell = (char *)nh + ROSC_HDR;
    return (char *)nh + ROSC_HDR;
}

void rosc_free(void *ref)
{
    void **cell = ref;
    if (!ref)
        return;
    if (*cell)
        free((char *)*cell - ROSC_HDR);
    free(cell);
}

/* a payload pointer (the mem_rec) back to its ref */
void *rosc_ref_of(void *block)
{
    struct rosc_hdr *h = (struct rosc_hdr *)((char *)block - ROSC_HDR);
    return h->cell;
}
