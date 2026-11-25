cmake_minimum_required(VERSION 3.20)
project(pokeblack2_pc C)

# Recompiled game code: python tools/recomp/recomp.py ../pokeblack2 build/rc
set(PB2_RC_DIR ${CMAKE_SOURCE_DIR}/build/rc CACHE PATH "Recompiled C (tools/recomp/recomp.py output)")
file(GLOB PB2_RC_SOURCES CONFIGURE_DEPENDS ${PB2_RC_DIR}/*.c)
if(NOT PB2_RC_SOURCES)
    message(FATAL_ERROR "No recompiled code in ${PB2_RC_DIR}: run tools/recomp/recomp.py first")
endif()

list(APPEND CMAKE_MODULE_PATH ${CMAKE_SOURCE_DIR}/cmake)
include(Pb2SDL2)

# PB2_RELEASE=ON: the build given to players (tools/package.py): no debug
# information, stripped, no console window (messages: pokeblack2.log / box)
option(PB2_RELEASE "Player build: no debug info, stripped, no console" OFF)
if(PB2_RELEASE)
    set(PB2_DEBUG_FLAGS "")
else()
    set(PB2_DEBUG_FLAGS -g)
endif()

add_library(pb2_game STATIC ${PB2_RC_SOURCES})
target_include_directories(pb2_game PRIVATE src/runtime ${PB2_RC_DIR})
target_compile_options(pb2_game PRIVATE -O1 $<$<NOT:$<BOOL:${PB2_RELEASE}>>:-g1> -w -fno-strict-aliasing -fwrapv)

add_executable(pokeblack2
    src/runtime/main.c src/runtime/config.c src/runtime/mem.c src/runtime/rom.c src/runtime/cpu.c
    src/runtime/io.c src/runtime/bios.c src/runtime/card.c src/runtime/arm7.c
    src/runtime/dispatch.c src/runtime/thread.c src/runtime/watchdog.c src/runtime/hle.c
    src/video/screen.c src/video/ppu.c src/video/vram.c src/video/gpu3d.c src/audio/snd.c)
target_include_directories(pokeblack2 PRIVATE src/runtime ${PB2_RC_DIR})
target_compile_options(pokeblack2 PRIVATE -O2 ${PB2_DEBUG_FLAGS} -Wall -Wno-unused-function -fno-strict-aliasing -fwrapv)
if(PB2_RELEASE)
    target_link_options(pokeblack2 PRIVATE -s -mwindows)
endif()
target_link_libraries(pokeblack2 PRIVATE pb2_game pb2::SDL2 winmm)
