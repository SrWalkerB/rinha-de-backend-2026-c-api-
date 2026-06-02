# Build: prepare (build-time index), api, lb, verify.
# AVX2 target (Mac Mini Late 2014 = Haswell, has AVX2). Override CFLAGS to disable.
CC      ?= gcc
CFLAGS  ?= -O3 -std=c11 -march=x86-64-v2 -mavx2 -mfma -flto -Wall -Wextra
LDLIBS  ?=

COMMON  := src/common/vec.c src/common/packed.c src/common/knn.c src/common/reqparse.c

.PHONY: all clean
all: prepare api lb verify

prepare: src/prepare/prepare.c $(COMMON)
	$(CC) $(CFLAGS) -o $@ $^ -lz -lm

api: src/api/api.c $(COMMON)
	$(CC) $(CFLAGS) -o $@ $^ -lm

verify: src/verify/verify.c $(COMMON)
	$(CC) $(CFLAGS) -o $@ $^ -lm

bench: src/verify/bench.c $(COMMON)
	$(CC) $(CFLAGS) -o $@ $^ -lm

lb: src/lb/lb.c
	$(CC) $(CFLAGS) -o $@ $^

clean:
	rm -f prepare api lb verify packed.bin
