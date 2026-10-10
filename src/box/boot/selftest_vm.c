/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Alban Read */
/* selftest_vm.c checks that the memory calls behave as they do on Linux,
 * since the box's own code and its programs rely on that behaviour. The
 * test runs wherever the box runs: on Linux, and on the HAL, which has its
 * own vm.c.
 *
 * Setting pages to PROT_NONE with mprotect must keep their memory. When the
 * caller sets them back to read and write, the pages must still hold what
 * they held before, and mincore must report them resident throughout. (In
 * #200 the HAL freed the pages, and a fresh zeroed page came back in their
 * place.) */
#include <stdint.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include "selftest.h"

#define check ros_check

void ros_selftest_vm(void)
{
    const size_t page = (size_t)sysconf(_SC_PAGESIZE), n = 4;   /* 16 KB pages when hosted on a Mac */
    uint8_t *m = mmap(NULL, n * page, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (m == MAP_FAILED) {
        check(0, "mprotect PROT_NONE keeps the memory", "mmap failed");
        return;
    }
    for (size_t i = 0; i < n * page; i++)
        m[i] = (uint8_t)(i * 7 + 3);
    int none = mprotect(m + page, 2 * page, PROT_NONE);
    unsigned char vec[4] = { 0 };
    int mc = mincore(m, n * page, vec);
    int back = mprotect(m + page, 2 * page, PROT_READ | PROT_WRITE);
    size_t bad = 0;
    for (size_t i = 0; i < n * page; i++)
        bad += m[i] != (uint8_t)(i * 7 + 3);
    /* Also check a page that goes to PROT_NONE and back a second time, with a write in between. */
    m[page] = 0xA5;
    int again = mprotect(m + page, page, PROT_NONE) || mprotect(m + page, page, PROT_READ);
    int kept = m[page] == 0xA5;
    munmap(m, n * page);
    check(!none && !back && !again && !bad && kept && !mc && (vec[1] & 1) && (vec[2] & 1),
          "mprotect PROT_NONE keeps the memory: back to read and write, the pages are as they were; "
          "mincore has them resident (#200)",
          "mprotect %d %d %d, %zu bytes changed, kept %d, mincore %d %d%d%d%d", none, back, again, bad, kept,
          mc, vec[0] & 1, vec[1] & 1, vec[2] & 1, vec[3] & 1);
}
