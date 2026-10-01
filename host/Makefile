# Host build of the runtime for tests (Linux/macOS).
CC ?= cc
CFLAGS ?= -O2 -g -std=c11 -Wall -Wextra -Wno-unused-parameter
RT = ../runtime
RT_SRC = $(RT)/mem.c $(RT)/cpu_interp.c $(RT)/vic.c $(RT)/cia.c $(RT)/sid.c \
         $(RT)/kernal.c $(RT)/basic_rnd.c $(RT)/machine.c $(RT)/palette.c
GEN ?= ../build/gen

all: ljrun_interp

ljrun_interp: ljrun.c $(RT_SRC) $(RT)/recomp_stub.c $(wildcard $(RT)/*.h) $(RT)/cpu_interp_gen.inc
	$(CC) $(CFLAGS) -o $@ ljrun.c $(RT_SRC) $(RT)/recomp_stub.c -lm

clean:
	rm -f ljrun_interp ljrun_game
