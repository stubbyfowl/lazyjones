# Host build of the runtime for tests (Linux/macOS).
CC ?= cc
CFLAGS ?= -O2 -g -std=c11 -ffp-contract=off -Wall -Wextra -Wno-unused-parameter
RT = ../runtime
RT_SRC = $(RT)/mem.c $(RT)/cpu_interp.c $(RT)/vic.c $(RT)/cia.c $(RT)/sid.c \
         $(RT)/kernal.c $(RT)/basic_rnd.c $(RT)/machine.c $(RT)/palette.c \
         $(RT)/lj_extras.c
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
	rm -f ljrun_interp ljrun_game ljexplore gltest fetest fetest_nogame ../tests/cpu_klaus ../tests/cpu_ss

# front end tests (tests/fe_test.c)
FE_TEST_DEPS = ../tests/fe_test.c ../frontend/fe.c $(RT_SRC) $(wildcard $(RT)/*.h) ../frontend/fe.h
fetest: $(FE_TEST_DEPS) $(RT)/lj_game.c $(GEN)/lj_recomp.c $(GEN)/lj_game_data.c
	$(CC) $(CFLAGS) -I$(RT) -I../frontend -o $@ ../tests/fe_test.c ../frontend/fe.c $(RT_SRC) $(RT)/lj_game.c $(GEN)/lj_recomp.c $(GEN)/lj_game_data.c -lm

fetest_nogame: $(FE_TEST_DEPS) $(RT)/recomp_stub.c $(RT)/nogame.c
	$(CC) $(CFLAGS) -DLJ_NOGAME -I$(RT) -I../frontend -o $@ ../tests/fe_test.c ../frontend/fe.c $(RT_SRC) $(RT)/recomp_stub.c $(RT)/nogame.c -lm

# CPU tests (see tests/): Klaus Dormann's functional test and SingleStepTests
../tests/cpu_klaus: ../tests/cpu_klaus.c $(RT_SRC) $(RT)/recomp_stub.c $(wildcard $(RT)/*.h) $(RT)/cpu_interp_gen.inc
	$(CC) $(CFLAGS) -o $@ ../tests/cpu_klaus.c $(RT_SRC) $(RT)/trace.c $(RT)/recomp_stub.c -lm

../tests/cpu_ss: ../tests/cpu_ss.c $(RT_SRC) $(RT)/recomp_stub.c $(wildcard $(RT)/*.h) $(RT)/cpu_interp_gen.inc
	$(CC) $(CFLAGS) -o $@ ../tests/cpu_ss.c $(RT_SRC) $(RT)/trace.c $(RT)/recomp_stub.c -lm

tests: ../tests/cpu_klaus ../tests/cpu_ss fetest_nogame

# drawing test with Mesa (needs libegl-dev, libgles-dev, libegl-mesa0, libgl1-mesa-dri)
APP = ../android/app/src/main/cpp
gltest: gltest.c $(APP)/glview.c $(APP)/glview.h ../frontend/fe.h
	$(CC) $(CFLAGS) -I$(APP) -I../frontend -o $@ gltest.c $(APP)/glview.c -lEGL -lGLESv2 -lm
