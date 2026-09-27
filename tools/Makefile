# tools/ - Phase 15 IO: the packed digit converter / checker (ecalc/packed_fmt.h); plain gcc, no ROCm
CC      ?= gcc
CFLAGS  ?= -O3 -march=native -Wall -fopenmp -pthread
all: unpack_digits wbench
unpack_digits: unpack_digits.c ../ecalc/packed_fmt.h
	$(CC) $(CFLAGS) -o $@ $<
wbench: wbench.c
	$(CC) $(CFLAGS) -o $@ $<
clean:
	rm -f unpack_digits wbench
