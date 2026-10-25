# tools/ - Phase 15 IO: the packed digit converter / checker (ecalc/packed_fmt.h); plain gcc, no ROCm
# Phase 16 A: gcc unless CC is given (make's built-in `cc` is the Cray compiler wrapper on aac7, which adds the loaded modules' -l flags)
ifeq ($(origin CC),default)
CC       = gcc
endif
CFLAGS  ?= -O3 -march=native -Wall -fopenmp -pthread
all: unpack_digits wbench
unpack_digits: unpack_digits.c ../ecalc/packed_fmt.h
	$(CC) $(CFLAGS) -o $@ $<
wbench: wbench.c
	$(CC) $(CFLAGS) -o $@ $<
clean:
	rm -f unpack_digits wbench
