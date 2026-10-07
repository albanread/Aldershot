# Makefile for NetSurf's native RISC OS front end on ROSGD (TARGET=rosgd):
# design 22, section 11.  ROSGD's own; MIT licence.  Built by
# ports/netsurf/build.sh, which copies this directory to frontends/rosgd.

CFLAGS += -std=c99 -Dnsrosgd \
	  -D_POSIX_C_SOURCE=200809L -D_XOPEN_SOURCE=700 -D_BSD_SOURCE \
	  -D_DEFAULT_SOURCE -D_NETBSD_SOURCE

LDFLAGS += -lm

# The Messages file, split from NetSurf's with the RISC OS entries
MESSAGES_FILTER=ro
MESSAGES_TARGET=$(FRONTEND_RESOURCES_DIR)

S_FRONTEND := main.c window.c plot.c download.c fetch_url.c

SOURCES = $(S_COMMON) $(S_IMAGE) $(S_BROWSER) $(S_FRONTEND)
EXETARGET := nsrosgd

install-rosgd:

package-rosgd:
