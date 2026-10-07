# SparkFS in the ROM (#107), included by the Makefile after capps.mk (whose
# CAPPS and CAPPS_RT these rules read).
#
# SparkFS's filing system and its ten codecs are ROM modules: each an x32 or
# A64X32 C module image, as the ColourPicker's is (tools/mkromx32.py: the
# runtime copies it to the RMA and starts it from its header, runtime/rom.c),
# in a 256K slot of its own above the ColourPicker's, started after it
# (boot/rom_contents.c).  !SparkFS -- SparkFSApp, SparkFS's desktop front end
# -- is Resources:$.Apps.!SparkFS, as RISC OS's ROM holds its applications:
# build.sh's rom/, which the Makefile's resources.c rule takes as one of
# mkresources.py's roots (and names itself: this file is read after that
# rule).  ports/sparkfs/README.md has the rest.
#
# build.sh builds it all, in a few seconds: it runs again when the port,
# third_party's SparkFS sources, the staged C or the runtime objects change.
#
#   make [ARCH=aarch64] sparkfs   the modules and rom/Apps/!SparkFS alone

THIRD_PARTY  ?= ../../third_party
SPARKFS_SRC  := $(THIRD_PARTY)/SparkFS
SPARKFS_W    := $(B)/ports/sparkfs
# The ROM's files' datestamp, so a build makes the same ROM as the last
# (#55): SOURCE_DATE_EPOCH, else the port's last commit
SPARKFS_EPOCH ?= $(or $(SOURCE_DATE_EPOCH),$(shell git log -1 --format=%ct -- ports/sparkfs 2>/dev/null),0)

$(SPARKFS_W)/.made: ports/sparkfs/build.sh ports/sparkfs/sparkfs.mk \
                    $(shell find ports/sparkfs -type f -not -path 'ports/sparkfs/test/*') \
                    $(shell find $(SPARKFS_SRC) -type f 2>/dev/null) \
                    $(CAPPS)/inc/C/kernel.h $(ROSCC) $(CAPPS_RT)/modclib_$(ABI).o $(CAPPS_RT)/crt0_$(ABI).o
	ARCH=$(ARCH) THIRD_PARTY=$(abspath $(THIRD_PARTY)) SPARKFS_EPOCH=$(SPARKFS_EPOCH) sh ports/sparkfs/build.sh
	touch $@

# Each module's image as a ROM unit: the Makefile's SPARKFS_ROM gives its
# file, its unit's name and its slot
$(GEN)/sparkfs-rom.made: $(SPARKFS_W)/.made tools/mkromx32.py
	@mkdir -p $(GEN)
	@for m in $(SPARKFS_ROM); do set -- $$(echo $$m | tr : ' '); \
	    echo "mkromx32.py $$2 $$3 $(SPARKFS_W)/modules/$$1,ffa"; \
	    $(PYTHON) tools/mkromx32.py $$2 $$3 $(SPARKFS_W)/modules/$$1,ffa $(GEN)/rom_$$2.c \
	        $(GEN)/rom_$$2.h $(SPARKFS_SLOT) || exit 1; \
	done
	touch $@
$(SPARKFS_ROMC) $(SPARKFS_ROMH): $(GEN)/sparkfs-rom.made ;

sparkfs: $(SPARKFS_W)/.made
.PHONY: sparkfs
