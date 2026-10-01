# Native library of the Android app: runtime + recompiled game + front end.
cmake_minimum_required(VERSION 3.18)
project(lazyjones C)

get_filename_component(LJ_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../../../.." ABSOLUTE)
set(LJ_GEN_DIR "" CACHE PATH "Directory with lj_recomp.c and lj_game_data.c from tools/ljrecomp")

set(RUNTIME
    ${LJ_ROOT}/runtime/mem.c
    ${LJ_ROOT}/runtime/cpu_interp.c
    ${LJ_ROOT}/runtime/vic.c
    ${LJ_ROOT}/runtime/cia.c
    ${LJ_ROOT}/runtime/sid.c
    ${LJ_ROOT}/runtime/kernal.c
    ${LJ_ROOT}/runtime/basic_rnd.c
    ${LJ_ROOT}/runtime/machine.c
    ${LJ_ROOT}/runtime/palette.c
    ${LJ_ROOT}/frontend/fe.c)

if(LJ_GEN_DIR)
    message(STATUS "Lazy Jones: building with the recompiled game from ${LJ_GEN_DIR}")
    set(GAME ${LJ_GEN_DIR}/lj_recomp.c ${LJ_GEN_DIR}/lj_game_data.c ${LJ_ROOT}/runtime/lj_game.c)
else()
    message(WARNING "Lazy Jones: no game sources, building the app without the game")
    set(GAME ${LJ_ROOT}/runtime/recomp_stub.c ${LJ_ROOT}/runtime/nogame.c)
endif()

add_library(lazyjones SHARED main.c audio.c ${RUNTIME} ${GAME})
target_include_directories(lazyjones PRIVATE ${LJ_ROOT}/runtime ${LJ_ROOT}/frontend ${CMAKE_CURRENT_SOURCE_DIR})
set_target_properties(lazyjones PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON)
target_compile_options(lazyjones PRIVATE -O2 -Wall -Wextra -Wno-unused-parameter
                       -fvisibility=hidden -ffunction-sections -fdata-sections)
target_link_options(lazyjones PRIVATE -u ANativeActivity_onCreate -Wl,--gc-sections
                    -Wl,-z,max-page-size=16384 -Wl,--build-id=sha1)
target_link_libraries(lazyjones android log EGL GLESv2 m dl)
