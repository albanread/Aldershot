/* harness.c -- what ROSGD adds to NetSurf's monkey front end for the box's
 * checks (ports/netsurf/boxcheck.py).  ROSGD's own; MIT licence.
 *
 * Linked into nsmonkey beside monkey's own sources, which are unchanged,
 * and set up before main:
 *   - http: and https: through URL_Fetcher (frontend/fetch_url.c), the
 *     fetcher the native front end registers;
 *   - a command, SLEEP ms, which runs NetSurf's scheduler for that long --
 *     fetches move on in it -- so a script can wait for a page, where
 *     monkey itself takes its next command at once.
 */
#include <stdint.h>
#include <stdlib.h>
#include <time.h>

#include <nsutils/time.h>

#include "utils/errors.h"
#include "monkey/dispatch.h"
#include "monkey/schedule.h"
#include "rosgd/fetch_url.h"

static void sleep_command(int argc, char **argv)
{
    uint64_t now = 0, end;
    nsu_getmonotonic_ms(&now);
    end = now + (argc > 1 ? (uint64_t)strtoul(argv[1], NULL, 10) : 1000);
    while (now < end) {
        int next = monkey_schedule_run();
        uint64_t wait = end - now;
        if (next >= 0 && (uint64_t)next < wait)
            wait = (uint64_t)next;
        if (wait > 20)
            wait = 20;
        struct timespec ts = { 0, (long)wait * 1000000L };
        nanosleep(&ts, NULL);
        nsu_getmonotonic_ms(&now);
    }
}

__attribute__((constructor)) static void harness(void)
{
    if (fetch_url_register() != NSERROR_OK || monkey_register_handler("SLEEP", sleep_command) != NSERROR_OK)
        abort();
}
