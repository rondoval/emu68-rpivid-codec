# The host programs (developer-guide.md section 4.3): the backend's stream layer
# compiled by the host compiler, with host_compat/ standing in for the Amiga headers.
#   make -C tests/host            builds them into tests/host/out/
#   make -C tests/host check      runs them, the clips of tests/fixtures/ through stream_check,
#                                 clip720 without its first picture (no_irap_start.py), and
#                                 clip720_slices with two pictures' slices disagreeing (mixed_slices.py)
# test_bits, test_nal, test_ps, test_dpb, test_rps and test_record share host_check.h; test_bits and test_ps write units with bit_writer.h.
TOP := $(abspath ..)/..
OUT := out
CC ?= cc
CFLAGS ?= -O2 -g -Wall -Wextra -Wconversion -Wsign-conversion -Wshadow -Wmissing-prototypes -Wstrict-prototypes
CPPFLAGS := -I$(TOP)/include -I$(TOP)/src/stream -Ihost_compat

PROGRAMS := $(OUT)/test_bits $(OUT)/test_nal $(OUT)/test_ps $(OUT)/test_dpb $(OUT)/test_rps $(OUT)/test_record $(OUT)/stream_check
STREAM := $(TOP)/src/stream/bits.c $(TOP)/src/stream/nal.c $(TOP)/src/stream/ps.c $(TOP)/src/stream/slice.c \
          $(TOP)/src/stream/rps.c $(TOP)/src/stream/dpb.c $(TOP)/src/stream/record.c $(TOP)/src/stream/stream.c

all: $(PROGRAMS)

$(OUT):
	mkdir -p $(OUT)

$(OUT)/test_bits: test_bits.c bit_writer.h host_check.h $(TOP)/src/stream/bits.c $(TOP)/src/stream/bits.h | $(OUT)
	$(CC) $(CFLAGS) $(CPPFLAGS) -o $@ test_bits.c $(TOP)/src/stream/bits.c

$(OUT)/test_nal: test_nal.c host_check.h $(TOP)/src/stream/nal.c $(TOP)/src/stream/nal.h | $(OUT)
	$(CC) $(CFLAGS) $(CPPFLAGS) -o $@ test_nal.c $(TOP)/src/stream/nal.c

$(OUT)/test_ps: test_ps.c bit_writer.h host_check.h $(TOP)/src/stream/ps.c $(TOP)/src/stream/slice.c $(TOP)/src/stream/bits.c \
                $(TOP)/src/stream/*.h | $(OUT)
	$(CC) $(CFLAGS) $(CPPFLAGS) -o $@ test_ps.c $(TOP)/src/stream/ps.c $(TOP)/src/stream/slice.c $(TOP)/src/stream/bits.c

$(OUT)/test_dpb: test_dpb.c host_check.h $(TOP)/src/stream/dpb.c $(TOP)/src/stream/*.h | $(OUT)
	$(CC) $(CFLAGS) $(CPPFLAGS) -o $@ test_dpb.c $(TOP)/src/stream/dpb.c

$(OUT)/test_rps: test_rps.c host_check.h $(TOP)/src/stream/rps.c $(TOP)/src/stream/dpb.c $(TOP)/src/stream/*.h | $(OUT)
	$(CC) $(CFLAGS) $(CPPFLAGS) -o $@ test_rps.c $(TOP)/src/stream/rps.c $(TOP)/src/stream/dpb.c

$(OUT)/test_record: test_record.c host_check.h $(TOP)/src/stream/record.c $(TOP)/src/stream/rps.c $(TOP)/src/stream/dpb.c \
                    $(TOP)/src/stream/*.h | $(OUT)
	$(CC) $(CFLAGS) $(CPPFLAGS) -o $@ test_record.c $(TOP)/src/stream/record.c $(TOP)/src/stream/rps.c $(TOP)/src/stream/dpb.c

$(OUT)/stream_check: stream_check.c $(STREAM) $(TOP)/src/stream/*.h | $(OUT)
	$(CC) $(CFLAGS) $(CPPFLAGS) -o $@ stream_check.c $(STREAM)

check: all
	$(OUT)/test_bits
	$(OUT)/test_nal
	$(OUT)/test_ps
	$(OUT)/test_dpb
	$(OUT)/test_rps
	$(OUT)/test_record
	for c in clip1080 clip720 clip4k clip1080_10 clip4k_10; do \
		$(OUT)/stream_check --frames $$(grep -c '^0,' ../fixtures/$$c.framemd5) ../fixtures/$$c.h265 || exit 1; done
	python3 no_irap_start.py ../fixtures/clip720.h265 $(OUT)/no_irap_start.h265
	$(OUT)/stream_check --frames $$(expr $$(grep -c '^0,' ../fixtures/clip720.framemd5) - 1) $(OUT)/no_irap_start.h265
	python3 mixed_slices.py ../fixtures/clip720_slices.h265 $(OUT)/mixed_slices.h265
	$(OUT)/stream_check --frames $$(expr $$(grep -vc '^#' ../fixtures/clip720_slices.colmd5) - 2) --dropped 2 $(OUT)/mixed_slices.h265

clean:
	rm -rf $(OUT)

.PHONY: all check clean
