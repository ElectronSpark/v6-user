# xv6-os user-essentials sub-repo — standalone CMake project.
#
# Builds the xv6-native userland: the static `userlib` (ulib + printf +
# umalloc + fsutil + generated usys.S), and ~60 small programs linked
# against it. Each program lives in programs/<name>/<name>.c.
#
# Standalone usage (requires a cross toolchain on PATH):
#   cmake -S . -B build \
#       -DARCH=riscv \
#       -DCMAKE_C_COMPILER=$XV6_TOOLCHAIN_BIN/riscv64-xv6-linux-musl-gcc \
#       -DCMAKE_INSTALL_PREFIX=$XV6_SYSROOT
#   cmake --build build -j --target install
#
# Programs install as /bin/_<name> in the sysroot (matches xv6 convention).
#
# Umbrella usage: invoked by xv6-os/cmake/BuildUser.cmake which sets
# the cross compiler from the built toolchain.
#
# NOTE: musl-libc-linked programs (a separate set in xv6-tmp using
# add_musl_program()) are deferred — they require the real custom
# musl-aware toolchain. See ../MIGRATION.md.

cmake_minimum_required(VERSION 3.10)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_BINARY_DIR)
    message(FATAL_ERROR "In-source builds are not allowed.")
endif()

# ARCH selection (matches kernel sub-repo naming: riscv | x86_64).
if(NOT DEFINED ARCH)
    if(DEFINED ENV{ARCH})
        set(ARCH "$ENV{ARCH}" CACHE STRING "Target architecture (riscv or x86_64)")
    else()
        set(ARCH "riscv" CACHE STRING "Target architecture (riscv or x86_64)")
    endif()
endif()
if(NOT ARCH MATCHES "^(riscv|x86_64)$")
    message(FATAL_ERROR "Invalid ARCH: ${ARCH}. Must be 'riscv' or 'x86_64'.")
endif()

# OPT_LEVEL passthrough (defaults to -O0 like xv6-tmp).
if(NOT DEFINED OPT_LEVEL)
    if(DEFINED ENV{OPT})
        set(OPT_LEVEL "$ENV{OPT}" CACHE STRING "Optimization level")
    else()
        set(OPT_LEVEL "0" CACHE STRING "Optimization level")
    endif()
endif()

# LAB selection — the upstream user CMakeLists conditionally adds extra
# programs per lab (e.g. lazytests for LAB=lazy, attack/secret for
# LAB=mmap). Default to "util" matching the kernel sub-repo.
if(NOT DEFINED ENV{LAB} OR "$ENV{LAB}" STREQUAL "")
    set(ENV{LAB} "util")
endif()
set(LAB_NAME "$ENV{LAB}")
string(TOUPPER "${LAB_NAME}" LABUPPER)

# Toolchain detection mirrors the kernel sub-repo (TOOLPREFIX env or
# auto-discover). Skip linker test (bare-metal toolchain has no libc).
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)

if(DEFINED ENV{TOOLPREFIX} AND NOT "$ENV{TOOLPREFIX}" STREQUAL "")
    set(CMAKE_C_COMPILER   $ENV{TOOLPREFIX}gcc CACHE STRING "C compiler" FORCE)
    set(CMAKE_ASM_COMPILER $ENV{TOOLPREFIX}gcc CACHE STRING "ASM compiler" FORCE)
    set(CMAKE_OBJCOPY      $ENV{TOOLPREFIX}objcopy)
    set(CMAKE_OBJDUMP      $ENV{TOOLPREFIX}objdump)
elseif(NOT DEFINED CMAKE_C_COMPILER)
    if(ARCH STREQUAL "riscv")
        find_program(_user_gcc riscv64-xv6-linux-musl-gcc)
        if(NOT _user_gcc)
            find_program(_user_gcc riscv64-unknown-elf-gcc)
        endif()
    else()
        find_program(_user_gcc x86_64-xv6-linux-musl-gcc)
        if(NOT _user_gcc)
            find_program(_user_gcc x86_64-elf-gcc)
        endif()
        if(NOT _user_gcc)
            find_program(_user_gcc x86_64-linux-gnu-gcc)
        endif()
    endif()
    if(_user_gcc)
        set(CMAKE_C_COMPILER ${_user_gcc} CACHE STRING "C compiler" FORCE)
        set(CMAKE_ASM_COMPILER ${_user_gcc} CACHE STRING "ASM compiler" FORCE)
    else()
        message(FATAL_ERROR "No cross compiler found for ARCH=${ARCH}.")
    endif()
endif()

project(xv6-${ARCH}-user LANGUAGES C ASM)

find_package(Perl REQUIRED)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Default install prefix → expect umbrella to override with the sysroot.
if(CMAKE_INSTALL_PREFIX_INITIALIZED_TO_DEFAULT)
    set(CMAKE_INSTALL_PREFIX "${CMAKE_BINARY_DIR}/sysroot" CACHE PATH "" FORCE)
endif()

message(STATUS "")
message(STATUS "========================================")
message(STATUS "  xv6 user (essentials) configuration")
message(STATUS "----------------------------------------")
message(STATUS "  ARCH:         ${ARCH}")
message(STATUS "  Optimization: -O${OPT_LEVEL}")
message(STATUS "  Lab:          ${LAB_NAME}")
message(STATUS "  Sysroot:      ${CMAKE_INSTALL_PREFIX}")
message(STATUS "========================================")
message(STATUS "")

include(${CMAKE_SOURCE_DIR}/cmake/AddXv6Program.cmake)

# ==============================================================================
# Architecture-specific config (mirrors xv6-tmp/user/CMakeLists.txt prelude).
# ==============================================================================
set(USER_LIB_DIR ${CMAKE_SOURCE_DIR}/lib)

if(ARCH STREQUAL "x86_64")
    set(USER_ARCH_CFLAGS "")
    set(USER_LINKER_SCRIPT ${USER_LIB_DIR}/x86_64/user.ld)
    set(USER_USYS_PL       ${USER_LIB_DIR}/x86_64/usys_x86_64.pl)
else()
    set(USER_ARCH_CFLAGS "-march=rv64gc;-mabi=lp64d;-mcmodel=medany")
    set(USER_LINKER_SCRIPT ${USER_LIB_DIR}/user.ld)
    set(USER_USYS_PL       ${USER_LIB_DIR}/usys.pl)
endif()

add_compile_options(-Wall -O${OPT_LEVEL} -fno-omit-frame-pointer
    -ggdb -gdwarf-2 -fno-common -nostdlib
    -fno-builtin-strncpy -fno-builtin-strncmp -fno-builtin-strlen
    -fno-builtin-memset -fno-builtin-memmove -fno-builtin-memcmp
    -fno-builtin-memcpy -fno-pie -no-pie -nostdinc -ffreestanding
    -DSOL_${LABUPPER} -DLAB_${LABUPPER}
    -I${USER_LIB_DIR})
if(NOT ARCH STREQUAL "x86_64")
    add_compile_options(-march=rv64gc -mabi=lp64d -mcmodel=medany)
endif()

# ==============================================================================
# usys.S — generated from usys.pl (perl script).
# ==============================================================================
set(USYS_S ${CMAKE_CURRENT_BINARY_DIR}/usys.S)
add_custom_command(
    OUTPUT  ${USYS_S}
    COMMAND ${PERL_EXECUTABLE} ${USER_USYS_PL} > ${USYS_S}
    DEPENDS ${USER_USYS_PL}
    COMMENT "Generating usys.S from ${USER_USYS_PL}")
set_source_files_properties(${USYS_S} PROPERTIES GENERATED TRUE LANGUAGE C)

# ==============================================================================
# userlib — static lib linked into every xv6-native program.
# ==============================================================================
add_library(userlib STATIC
    ${USER_LIB_DIR}/ulib.c
    ${USER_LIB_DIR}/printf.c
    ${USER_LIB_DIR}/umalloc.c
    ${USER_LIB_DIR}/fsutil.c
    ${USYS_S})
target_include_directories(userlib PUBLIC ${USER_LIB_DIR})

# ==============================================================================
# Programs — one subdir per program; auto-discovered.
# ==============================================================================
file(GLOB _prog_dirs RELATIVE ${CMAKE_SOURCE_DIR}/programs
     ${CMAKE_SOURCE_DIR}/programs/*)
set(XV6_USER_PROGRAMS "")
foreach(_p ${_prog_dirs})
    if(IS_DIRECTORY ${CMAKE_SOURCE_DIR}/programs/${_p})
        if(EXISTS ${CMAKE_SOURCE_DIR}/programs/${_p}/${_p}.c)
            xv6_user_program(${_p})
            list(APPEND XV6_USER_PROGRAMS _${_p})
        endif()
    endif()
endforeach()

list(LENGTH XV6_USER_PROGRAMS _n)
message(STATUS "xv6 user programs to build: ${_n}")
add_custom_target(user_programs ALL DEPENDS ${XV6_USER_PROGRAMS})
