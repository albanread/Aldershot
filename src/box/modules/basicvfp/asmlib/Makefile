CC = cc
# The frozen corpora: clones of the MRASM and WRASM projects, in zcode-work
# beside the toolchain (RISCOSDEV/zcode-work/basic-asm/oracles)
ORACLES ?= ../../../../../zcode-work/basic-asm/oracles
CFLAGS = -std=c11 -O1 -g -Wall -Wextra -Wno-unused-parameter

all: asmtest

# asma64_tab.c is generated, and kept here: gen_a64_tab.py (basic-asm's
# tools, beside ORACLES) writes it into basic-asm's asmlib from the MRASM
# corpus; tab_immval.py finishes it -- the immediates its entries cannot
# vary, and asma64_table's own fixes.  To regenerate:
#asma64_tab.c: $(ORACLES)/../tools/gen_a64_tab.py $(ORACLES)/MRASM/corpus/aarch64.tsv tab_immval.py
#	python3 $(ORACLES)/../tools/gen_a64_tab.py
#	cp $(ORACLES)/../asmlib/asma64_tab.c asma64_tab.c
#	python3 tab_immval.py $(ORACLES)/../tools $(ORACLES)/MRASM/corpus/aarch64.tsv asma64_tab.c

asmtest: asmtest.c asma64.c asma64_mv.c asma64_tab.c asmx64.c asmeval.c asm.h asma64.h asma64_int.h
	$(CC) $(CFLAGS) -o asmtest asmtest.c asma64.c asma64_mv.c asma64_tab.c asmx64.c asmeval.c

# Every golden: the corpora (LLVM MC's, frozen), and goldens/ -- the
# PC-relative forms with BASIC's absolute targets and the instructions the
# corpora leave out, from LLVM MC too (goldens/gen_pcrel.py, gen_common.py),
# and what the encoders must refuse (refuse_*.tsv, by hand).  Each prints
# its tally; make fails if any line does.
asmtest-all: asmtest
	@fail=0; \
	for f in $(ORACLES)/MRASM/corpus/aarch64.tsv $(ORACLES)/WRASM/corpus/x86_64.tsv goldens/*.tsv; do \
	    ./asmtest $$f > asmtest.out; s=$$?; grep -v "^FAIL" asmtest.out | tail -1; \
	    grep "^FAIL" asmtest.out; [ $$s = 0 ] || fail=1; \
	done; rm -f asmtest.out; exit $$fail

test64: asmtest
	./asmtest $(ORACLES)/MRASM/corpus/aarch64.tsv
	./asmtest goldens/pcrel_a64.tsv
	./asmtest goldens/common_a64.tsv
	./asmtest goldens/refuse_a64.tsv

testx64: asmtest
	./asmtest $(ORACLES)/WRASM/corpus/x86_64.tsv
	./asmtest goldens/pcrel_x86_64.tsv
	./asmtest goldens/common_x86_64.tsv
	./asmtest goldens/refuse_x86_64.tsv

clean:
	rm -f asmtest asmtest.out

.PHONY: all asmtest-all test64 testx64 clean
