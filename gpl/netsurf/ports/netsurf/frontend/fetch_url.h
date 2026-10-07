/* fetch_url.h -- NetSurf's http: and https: through RISC OS's URL_Fetcher
 * (design 22, section 11).  ROSGD's own; MIT licence. */
#ifndef ROSGD_FETCH_URL_H
#define ROSGD_FETCH_URL_H

#include "utils/errors.h"

/* Register the fetcher for http: and https:, after netsurf_init */
nserror fetch_url_register(void);

#endif
