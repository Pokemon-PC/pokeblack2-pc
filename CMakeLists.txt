cmake_minimum_required(VERSION 3.20)
project(pokeblack2_pc C)

# Black 2 or White 2 (same disassembly, different code and data)
set(PB2_VERSION black2 CACHE STRING "Game: black2 or white2")
set_property(CACHE PB2_VERSION PROPERTY STRINGS black2 white2)
# English (USA/Europe) or French: another ROM, so another address map and
# another exe/save (pokeblanche2 / pokenoire2)
set(PB2_LANG en CACHE STRING "Language of the ROM: en or fr")
set_property(CACHE PB2_LANG PROPERTY STRINGS en fr)
if(PB2_VERSION STREQUAL "white2" AND PB2_LANG STREQUAL "fr")
    set(PB2_RC_DEFAULT ${CMAKE_SOURCE_DIR}/build/rc_w2fr)
    set(PB2_EXE pokeblanche2)
elseif(PB2_VERSION STREQUAL "black2" AND PB2_LANG STREQUAL "fr")
    set(PB2_RC_DEFAULT ${CMAKE_SOURCE_DIR}/build/rc_b2fr)
    set(PB2_EXE pokenoire2)
elseif(PB2_VERSION STREQUAL "white2")
    set(PB2_RC_DEFAULT ${CMAKE_SOURCE_DIR}/build/rc_w2)
    set(PB2_EXE pokewhite2)
elseif(PB2_VERSION STREQUAL "black2")
    set(PB2_RC_DEFAULT ${CMAKE_SOURCE_DIR}/build/rc)
    set(PB2_EXE pokeblack2)
else()
    message(FATAL_ERROR "PB2_VERSION must be black2 or white2")
endif()

# Recompiled game code: python tools/recomp/recomp.py ../pokeblack2 build/rc
# (White 2: ... build/rc_w2 --version white2)
set(PB2_RC_DIR ${PB2_RC_DEFAULT} CACHE PATH "Recompiled C (tools/recomp/recomp.py output)")
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

add_executable(${PB2_EXE}
    src/runtime/main.c src/runtime/config.c src/runtime/mem.c src/runtime/rom.c src/runtime/cpu.c
    src/runtime/io.c src/runtime/bios.c src/runtime/card.c src/runtime/arm7.c
    src/runtime/dispatch.c src/runtime/thread.c src/runtime/watchdog.c src/runtime/hle.c
    src/runtime/heapwatch.c
    src/video/screen.c src/video/ppu.c src/video/vram.c src/video/gpu3d.c src/audio/snd.c)
target_include_directories(${PB2_EXE} PRIVATE src/runtime ${PB2_RC_DIR})
target_compile_options(${PB2_EXE} PRIVATE -O2 ${PB2_DEBUG_FLAGS} -Wall -Wno-unused-function -fno-strict-aliasing -fwrapv)
if(PB2_RELEASE)
    target_link_options(${PB2_EXE} PRIVATE -s -mwindows)
endif()
if(PB2_VERSION STREQUAL "white2")
    target_compile_definitions(${PB2_EXE} PRIVATE PB2_WHITE2)
endif()
if(PB2_LANG STREQUAL "fr")
    target_compile_definitions(${PB2_EXE} PRIVATE PB2_FRENCH)
endif()
target_link_libraries(${PB2_EXE} PRIVATE pb2_game pb2::SDL2 winmm)
