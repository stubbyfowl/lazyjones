# Host build of the runtime for tests (Linux/macOS).
CC ?= cc
CFLAGS ?= -O2 -g -std=c11 -Wall -Wextra -Wno-unused-parameter
RT = ../runtime
RT_SRC = $(RT)/mem.c $(RT)/cpu_interp.c $(RT)/vic.c $(RT)/cia.c $(RT)/sid.c \
         $(RT)/kernal.c $(RT)/basic_rnd.c $(RT)/machine.c $(RT)/palette.c
GEN ?= ../build/gen

all: ljrun_interp ljexplore

# recompiled game: needs build/gen from tools/ljrecomp build
ljrun_game: ljrun.c $(RT_SRC) $(RT)/trace.c $(RT)/lj_game.c $(GEN)/lj_recomp.c $(GEN)/lj_game_data.c $(wildcard $(RT)/*.h)
	$(CC) $(CFLAGS) -I$(RT) -o $@ ljrun.c $(RT_SRC) $(RT)/trace.c $(RT)/lj_game.c $(GEN)/lj_recomp.c $(GEN)/lj_game_data.c -lm

ljrun_interp: ljrun.c $(RT_SRC) $(RT)/recomp_stub.c $(wildcard $(RT)/*.h) $(RT)/cpu_interp_gen.inc
	$(CC) $(CFLAGS) -o $@ ljrun.c $(RT_SRC) $(RT)/trace.c $(RT)/recomp_stub.c -lm

ljexplore: ljexplore.c $(RT_SRC) $(RT)/trace.c $(RT)/recomp_stub.c $(wildcard $(RT)/*.h) $(RT)/cpu_interp_gen.inc
	$(CC) $(CFLAGS) -DLJ_TRACE -o $@ ljexplore.c $(RT_SRC) $(RT)/trace.c $(RT)/recomp_stub.c -lm

clean:
	rm -f ljrun_interp ljrun_game ljexplore
