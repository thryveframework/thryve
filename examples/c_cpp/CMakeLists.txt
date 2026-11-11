# Builds the C and C++ examples against Thryve's C library: access_log_c, projects_c,
# access_log_cpp and projects_cpp.
#
# Against the library built from this repository, from examples/c_cpp:
#   cargo build -p thryve_ffi
#   cmake -S . -B build
#   cmake --build build
#
# Against the C library from a release (thryve_ffi-<version>-<platform>), unzipped:
#   cmake -S . -B build -DTHRYVE_FFI_DIR=<the unzipped folder>
#   cmake --build build
#
# Without the DLL, with Thryve built into each program (MSVC, this repository's library only):
#   cmake -S . -B build-static -DTHRYVE_FFI_STATIC=ON
#   cmake --build build-static
#
# The programs land in build/ (build/Debug/ with Visual Studio), next to a copy of the DLL on
# Windows. Run them from anywhere: they find the sample log and the configuration in
# examples/data/ through EXAMPLES_DIR, set below.
#
# For a release build of this repository's library, build it with `cargo build --release -p
# thryve_ffi`, and configure with -DCMAKE_BUILD_TYPE=Release (single-config generators such as
# Ninja or Make).
#
# What the cargo build produces, in the workspace's target/debug/ or target/release/:
#   Windows, MSVC toolchain:  thryve_ffi.dll and its import library, thryve_ffi.dll.lib
#   Windows, GNU toolchain:   thryve_ffi.dll and its import library, libthryve_ffi.dll.a
#   Linux / macOS:            libthryve_ffi.so / libthryve_ffi.dylib
# and the static library, thryve_ffi.lib (MSVC) or libthryve_ffi.a, which also needs Rust's
# system libraries to link. The examples use the shared library, unless THRYVE_FFI_STATIC is on.
# The headers, thryve.h and thryve_ffi.h, are written by build.rs to crates/thryve_ffi/out/.

cmake_minimum_required(VERSION 3.15)
project(thryve_examples LANGUAGES C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

# ============================================================
# Find Thryve's C library and its headers
# ============================================================

set(THRYVE_FFI_DIR "" CACHE PATH
    "The C library from a release, unzipped; empty for this repository's build")
option(THRYVE_FFI_STATIC
    "Build Thryve into the programs, with no DLL (MSVC, this repository's build only)" OFF)

# The static library is in a cargo build only (a release download holds the DLL), and only MSVC can
# link it: cargo builds it here with Rust's MSVC toolchain.
if(THRYVE_FFI_STATIC AND THRYVE_FFI_DIR)
    message(FATAL_ERROR "THRYVE_FFI_STATIC links the library built from this repository: a release "
                        "download has no static library. Leave THRYVE_FFI_DIR empty.")
endif()
if(THRYVE_FFI_STATIC AND NOT MSVC)
    message(FATAL_ERROR "THRYVE_FFI_STATIC needs MSVC: the static library comes from Rust's MSVC "
                        "toolchain, and only MSVC can link it. Leave it off to use the DLL.")
endif()

if(THRYVE_FFI_DIR)
    # A release: the headers in include/; the library in lib/, and on Windows the DLL in bin/.
    set(THRYVE_FFI_INCLUDE_DIR "${THRYVE_FFI_DIR}/include")
    set(THRYVE_FFI_LIB_DIR "${THRYVE_FFI_DIR}/lib")
    set(THRYVE_FFI_BIN_DIR "${THRYVE_FFI_DIR}/bin")
    set(THRYVE_FFI_MISSING "Check THRYVE_FFI_DIR: it should name the unzipped C library.")
else()
    set(THRYVE_FFI_TARGET_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../target"
        CACHE PATH "Cargo's target folder")
    if(CMAKE_BUILD_TYPE STREQUAL "Release")
        set(THRYVE_FFI_PROFILE "release")
    else()
        set(THRYVE_FFI_PROFILE "debug")
    endif()

    set(THRYVE_FFI_INCLUDE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../crates/thryve_ffi/out")
    set(THRYVE_FFI_LIB_DIR "${THRYVE_FFI_TARGET_DIR}/${THRYVE_FFI_PROFILE}")
    set(THRYVE_FFI_BIN_DIR "${THRYVE_FFI_LIB_DIR}")
    set(THRYVE_FFI_MISSING "Run 'cargo build -p thryve_ffi' first, or set THRYVE_FFI_DIR.")
endif()

if(NOT EXISTS "${THRYVE_FFI_INCLUDE_DIR}/thryve.h")
    message(FATAL_ERROR "thryve.h not found in ${THRYVE_FFI_INCLUDE_DIR}. ${THRYVE_FFI_MISSING}")
endif()

if(THRYVE_FFI_STATIC)
    set(THRYVE_FFI_STATIC_LIB "${THRYVE_FFI_LIB_DIR}/thryve_ffi.lib")
    if(NOT EXISTS "${THRYVE_FFI_STATIC_LIB}")
        message(FATAL_ERROR "${THRYVE_FFI_STATIC_LIB} not found. Run 'cargo build -p thryve_ffi' "
                            "first.")
    endif()

    add_library(thryve_ffi STATIC IMPORTED)
    set_target_properties(thryve_ffi PROPERTIES
        IMPORTED_LOCATION "${THRYVE_FFI_STATIC_LIB}"
        # The Windows libraries that Rust's code calls into. To print the list for your Rust:
        #   cargo rustc -p thryve_ffi --lib -- --print native-static-libs
        INTERFACE_LINK_LIBRARIES "kernel32;ntdll;userenv;ws2_32;dbghelp"
    )
elseif(WIN32)
    add_library(thryve_ffi SHARED IMPORTED)
    set(THRYVE_FFI_DLL "${THRYVE_FFI_BIN_DIR}/thryve_ffi.dll")

    # The DLL's import library: a release names it thryve_ffi.lib. In a cargo build, that name is
    # the static library: the MSVC toolchain names the import library thryve_ffi.dll.lib, the GNU
    # toolchain libthryve_ffi.dll.a.
    if(THRYVE_FFI_DIR)
        set(THRYVE_FFI_IMPORT_LIB "${THRYVE_FFI_LIB_DIR}/thryve_ffi.lib")
    elseif(EXISTS "${THRYVE_FFI_LIB_DIR}/thryve_ffi.dll.lib")
        set(THRYVE_FFI_IMPORT_LIB "${THRYVE_FFI_LIB_DIR}/thryve_ffi.dll.lib")
    else()
        set(THRYVE_FFI_IMPORT_LIB "${THRYVE_FFI_LIB_DIR}/libthryve_ffi.dll.a")
    endif()

    if(NOT EXISTS "${THRYVE_FFI_DLL}" OR NOT EXISTS "${THRYVE_FFI_IMPORT_LIB}")
        message(FATAL_ERROR "thryve_ffi.dll or its import library not found (looked for "
                            "${THRYVE_FFI_DLL} and ${THRYVE_FFI_IMPORT_LIB}). ${THRYVE_FFI_MISSING}")
    endif()

    set_target_properties(thryve_ffi PROPERTIES
        IMPORTED_LOCATION "${THRYVE_FFI_DLL}"
        IMPORTED_IMPLIB "${THRYVE_FFI_IMPORT_LIB}"
    )
elseif(APPLE)
    add_library(thryve_ffi SHARED IMPORTED)
    set_target_properties(thryve_ffi PROPERTIES
        IMPORTED_LOCATION "${THRYVE_FFI_LIB_DIR}/libthryve_ffi.dylib"
    )
else()
    add_library(thryve_ffi SHARED IMPORTED)
    set_target_properties(thryve_ffi PROPERTIES
        IMPORTED_LOCATION "${THRYVE_FFI_LIB_DIR}/libthryve_ffi.so"
    )
endif()

# Whatever links thryve_ffi also gets its headers.
set_target_properties(thryve_ffi PROPERTIES
    INTERFACE_INCLUDE_DIRECTORIES "${THRYVE_FFI_INCLUDE_DIR}"
)

# ============================================================
# The examples: the C ones include thryve.h, the C++ ones the extern "C" wrapper thryve_ffi.h
# ============================================================

# The examples folder, where every example finds the sample log and the configuration.
get_filename_component(EXAMPLES_DIR "${CMAKE_CURRENT_SOURCE_DIR}/.." ABSOLUTE)

# Each program is its main file, the projects it runs, and what every program shares.
set(C_SHARED c/common.c c/console.c)
add_executable(access_log_c c/access_log.c c/log_report.c ${C_SHARED})
add_executable(projects_c c/projects.c c/log_report.c c/primes.c ${C_SHARED})

set(CPP_SHARED cpp/common.cpp cpp/console.cpp)
add_executable(access_log_cpp cpp/access_log.cpp cpp/log_report.cpp ${CPP_SHARED})
add_executable(projects_cpp cpp/projects.cpp cpp/log_report.cpp cpp/primes.cpp ${CPP_SHARED})

foreach(example access_log_c projects_c access_log_cpp projects_cpp)
    target_link_libraries(${example} PRIVATE thryve_ffi)
    target_compile_definitions(${example} PRIVATE EXAMPLES_DIR="${EXAMPLES_DIR}")

    if(MSVC)
        # fopen and strerror are standard C; MSVC would rather they were its own _s versions.
        target_compile_definitions(${example} PRIVATE _CRT_SECURE_NO_WARNINGS)
        target_compile_options(${example} PRIVATE /W4)
    else()
        target_compile_options(${example} PRIVATE -Wall -Wextra)
    endif()

    if(THRYVE_FFI_STATIC)
        # Rust's code uses the release C runtime, also in a debug build. Each program uses it too,
        # or MSVC would link two runtimes and warn (LNK4098).
        set_target_properties(${example} PROPERTIES MSVC_RUNTIME_LIBRARY "MultiThreadedDLL")
    elseif(WIN32)
        # On Windows, a program finds the DLL next to it.
        add_custom_command(TARGET ${example} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${THRYVE_FFI_DLL}"
                $<TARGET_FILE_DIR:${example}>
            COMMENT "Copying thryve_ffi.dll next to ${example}"
        )
    endif()
endforeach()
