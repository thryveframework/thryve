# Builds the C and C++ examples with gcc and g++: access_log_c, projects_c, access_log_cpp and
# projects_cpp, in build/. It needs GNU Make and a POSIX shell (MSYS2's, on Windows).
#
# Against the library built from this repository, which it builds first; from examples/c_cpp:
#   make                      all the examples
#   make c                    the C examples
#   make cpp                  the C++ examples
#   make BUILD_TYPE=release   the same, against a release build of the library
#   make clean                removes them
#
# Against the C library from a release (thryve_ffi-<version>-<platform>), unzipped:
#   make THRYVE_FFI_DIR=<the unzipped folder>
#
# On Windows, gcc links the DLL itself, and the DLL is copied next to the examples, where Windows
# finds it. Run them from anywhere: they find the sample log and the configuration in
# examples/data/ through EXAMPLES_DIR, set below.

CARGO ?= cargo
CC = gcc
CXX = g++
BUILD_TYPE ?= debug
THRYVE_FFI_DIR ?=
OUT = build

ifeq ($(THRYVE_FFI_DIR),)
    # This repository's build: the headers build.rs writes, and the workspace's target folder.
    HEADER_DIR = ../../crates/thryve_ffi/out
    TARGET_DIR ?= ../../target
    LIB_DIR = $(TARGET_DIR)/$(BUILD_TYPE)
    BIN_DIR = $(LIB_DIR)
else
    # A release: the headers in include/; the library in lib/, and on Windows the DLL in bin/.
    HEADER_DIR = $(THRYVE_FFI_DIR)/include
    LIB_DIR = $(THRYVE_FFI_DIR)/lib
    BIN_DIR = $(THRYVE_FFI_DIR)/bin
endif

ifeq ($(BUILD_TYPE),release)
    CARGO_FLAGS = --release
    OPT_FLAGS = -O2
endif

# The examples folder, written the way the compiler reads it: under MSYS2, as a Windows path.
EXAMPLES_DIR := $(shell cygpath -m '$(abspath ..)' 2>/dev/null || echo '$(abspath ..)')

ifeq ($(OS),Windows_NT)
    EXE = .exe
    DLL = $(BIN_DIR)/thryve_ffi.dll
    LINK = $(DLL)
else
    EXE =
    # The library's folder is recorded in each example, where the loader finds it.
    LINK = -L$(LIB_DIR) -lthryve_ffi -Wl,-rpath,$(abspath $(LIB_DIR))
endif

FLAGS = -Wall -Wextra $(OPT_FLAGS) -I$(HEADER_DIR) -DEXAMPLES_DIR='"$(EXAMPLES_DIR)"'

# Each program is its main file, the projects it runs, and what every program shares.
C_SHARED = c/common.c c/console.c
CPP_SHARED = cpp/common.cpp cpp/console.cpp
PROGRAMS = access_log_c projects_c access_log_cpp projects_cpp

.PHONY: all c cpp library clean help
all: c cpp

# This repository's build of the library, and its headers. A release's needs no build.
library:
ifeq ($(THRYVE_FFI_DIR),)
	$(CARGO) build $(CARGO_FLAGS) --package thryve_ffi
endif

c: library
	mkdir -p $(OUT)
	$(CC) -std=c11 $(FLAGS) -o $(OUT)/access_log_c$(EXE) c/access_log.c c/log_report.c $(C_SHARED) $(LINK)
	$(CC) -std=c11 $(FLAGS) -o $(OUT)/projects_c$(EXE) c/projects.c c/log_report.c c/primes.c $(C_SHARED) $(LINK)
ifeq ($(OS),Windows_NT)
	cp $(DLL) $(OUT)/
endif

cpp: library
	mkdir -p $(OUT)
	$(CXX) -std=c++17 $(FLAGS) -o $(OUT)/access_log_cpp$(EXE) cpp/access_log.cpp cpp/log_report.cpp $(CPP_SHARED) $(LINK)
	$(CXX) -std=c++17 $(FLAGS) -o $(OUT)/projects_cpp$(EXE) cpp/projects.cpp cpp/log_report.cpp cpp/primes.cpp $(CPP_SHARED) $(LINK)
ifeq ($(OS),Windows_NT)
	cp $(DLL) $(OUT)/
endif

clean:
	rm -f $(foreach program,$(PROGRAMS),$(OUT)/$(program)$(EXE))
ifeq ($(OS),Windows_NT)
	rm -f $(OUT)/thryve_ffi.dll
endif

help:
	@echo "Thryve's C and C++ examples"
	@echo ""
	@echo "  make             all the examples, in $(OUT)/"
	@echo "  make c           the C examples"
	@echo "  make cpp         the C++ examples"
	@echo "  make clean       removes them"
	@echo ""
	@echo "Options:"
	@echo "  BUILD_TYPE=release       a release build of this repository's library"
	@echo "  TARGET_DIR=<path>        Cargo's target folder (default: ../../target)"
	@echo "  THRYVE_FFI_DIR=<folder>  the C library from a release, unzipped"
