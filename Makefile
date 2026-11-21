# libexecinfo - BSD backtrace library

# Build configuration
CC ?= cc
AR ?= ar
PYTHON ?= python3
STRIP ?= strip
INSTALL ?= install

# Version information
VERSION_MAJOR = 1
VERSION_MINOR = 1
VERSION_PATCH = 0
VERSION = $(VERSION_MAJOR).$(VERSION_MINOR).$(VERSION_PATCH)
SONAME = libexecinfo.so.$(VERSION_MAJOR)

# Installation paths
DESTDIR ?=
PREFIX ?= /usr/local
LIBDIR ?= $(PREFIX)/lib
INCLUDEDIR ?= $(PREFIX)/include
PKGCONFIGDIR ?= $(LIBDIR)/pkgconfig

# Compiler and linker flags
CPPFLAGS ?=
CFLAGS ?= -O2 -g
LDFLAGS ?=

# Security and modern compiler flags
ifeq ($(DEBUG), 1)
    SECURITY_CFLAGS = -fstack-protector-strong \
                      -fPIC -Wall -Wextra -Wformat=2 -Wformat-security \
                      -Wnull-dereference -Wstack-protector \
                      -fsanitize=address \
                      -fno-omit-frame-pointer

    ifneq ($(CC),clang)
        SECURITY_CFLAGS += -Wtrampolines
    endif
    SECURITY_LDFLAGS = -fsanitize=address
else
    SECURITY_CFLAGS = -fstack-protector-strong \
                      -fPIC -Wall -Wextra -Wformat=2 -Wformat-security \
                      -Wnull-dereference -Wstack-protector \
                      -fno-omit-frame-pointer

    # Only add -Wtrampolines if supported (GCC specific)
    ifneq ($(CC),clang)
        SECURITY_CFLAGS += -Wtrampolines
    endif
    # Only add _FORTIFY_SOURCE if not already defined (e.g. by makepkg)
    ifeq ($(findstring _FORTIFY_SOURCE,$(CFLAGS) $(CPPFLAGS)),)
        SECURITY_CFLAGS += -D_FORTIFY_SOURCE=2
    endif
    SECURITY_LDFLAGS =
endif

# Standard compliance
STD_CFLAGS = -std=gnu11

# Debug vs Release
DEBUG ?= 0

# Final flags
EXECINFO_CFLAGS = $(CPPFLAGS) $(CFLAGS) $(STD_CFLAGS) $(SECURITY_CFLAGS) \
                  $(BUILD_CFLAGS) $(ARCH_CFLAGS) -Wno-frame-address -c
EXECINFO_LDFLAGS = $(LDFLAGS) $(BUILD_LDFLAGS)

# Source files
SOURCES = execinfo.c stacktraverse.c
OBJECTS = $(SOURCES:.c=.o)
SHARED_OBJECTS = $(SOURCES:.c=.So)

# --- Build-flag change detection -------------------------------------------
# `make` only compares file mtimes; it has no idea that DEBUG=1 vs. DEBUG=0
# (or any other CFLAGS change) means the *same* execinfo.c needs different
# object code. If you build once with DEBUG=1 (ASan-instrumented objects)
# and then run e.g. `make test` without DEBUG=1 and without an intervening
# `make clean`, execinfo.c/stacktraverse.c are unchanged, so make reuses the
# stale ASan objects in libexecinfo.a -- but links the new target without
# -fsanitize=address, producing "undefined reference to __asan_*".
#
# Fix: record the flags actually used for the last build in a stamp file
# and make the object files depend on it. The FORCE prerequisite makes the
# stamp's recipe run on every invocation, but the recipe only rewrites (and
# therefore only bumps the mtime of) the file when its content actually
# differs -- so changing DEBUG correctly forces a rebuild, and repeated
# `make`/`make test` calls with the *same* flags still do nothing.
BUILD_SIG := $(EXECINFO_CFLAGS) $(SECURITY_LDFLAGS)

.PHONY: FORCE
FORCE:

.build_sig: FORCE
	@echo '$(BUILD_SIG)' | cmp -s - $@ 2>/dev/null || echo '$(BUILD_SIG)' > $@

$(OBJECTS) $(SHARED_OBJECTS): .build_sig

# Make's implicit "default goal = first target in the file" rule means
# inserting FORCE/.build_sig above would otherwise silently make FORCE
# the default goal for a bare `make` invocation. Pin it explicitly so
# the default goal doesn't depend on where rules happen to be ordered.
.DEFAULT_GOAL := all

# Generated files
GENERATED_FILES = stacktraverse.c

# Targets
STATIC_LIB = libexecinfo.a
SHARED_LIB = libexecinfo.so.$(VERSION)
# Named test_runner, not "test": a file literally named "test" would
# collide with the PHONY target below of the same name, which is what
# silently turned `make test` into a no-op in the previous Makefile
# (make saw the file "test" already existed and considered it up to
# date, so the recipe -- and the test binary -- never ran).
TEST_BINARY = test_runner

.PHONY: all static dynamic test test-dynamic clean install install-static install-dynamic \
        install-headers install-pkgconfig uninstall help generate

# Default target
all: static dynamic

# Generate source files
generate: $(GENERATED_FILES)

stacktraverse.c: gen.py
	$(PYTHON) gen.py --max-depth 128 --output $@

# Static library
static: $(STATIC_LIB)

$(STATIC_LIB): $(OBJECTS)
	$(AR) rcs $@ $^

# Dynamic library
dynamic: $(SHARED_LIB)

$(SHARED_LIB): $(SHARED_OBJECTS)
	$(CC) -shared -Wl,-soname,$(SONAME) $(EXECINFO_LDFLAGS) -o $@ $^ -ldl
	ln -sf $@ $(SONAME)
	ln -sf $@ libexecinfo.so

# Object files
%.o: %.c
	$(CC) $(EXECINFO_CFLAGS) -o $@ $<

%.So: %.c
	$(CC) $(EXECINFO_CFLAGS) -fPIC -DPIC -o $@ $<

# Test program (static lib) -- PHONY `test` builds it and runs it,
# propagating the binary's exit code so a CI/makepkg check() actually
# fails when the tests fail.
#
# Linked against the .a file directly ($(STATIC_LIB), not -L. -lexecinfo):
# when both libexecinfo.a and libexecinfo.so exist in the same -L
# directory (true here, since `all` builds both), the linker resolves
# -lexecinfo to the .so by default, regardless of this rule's
# dependency on the static lib. That produced a binary that looked
# statically tested but was actually dynamically linked, and then
# failed at runtime because the freshly-built, not-yet-installed .so
# isn't on the loader's search path. Naming the .a file explicitly
# removes the ambiguity.
$(TEST_BINARY): test.c $(STATIC_LIB)
	$(CC) $(CFLAGS) $(STD_CFLAGS) $(SECURITY_CFLAGS) $(SECURITY_LDFLAGS) $(BUILD_LDFLAGS) -rdynamic -o $@ $< $(STATIC_LIB) -ldl

test: $(TEST_BINARY)
	./$(TEST_BINARY)

# Test using dynamic lib -- also actually runs now, with LD_LIBRARY_PATH
# set so the freshly-built (not-yet-installed) .so is found.
test-dynamic: test.c $(SHARED_LIB)
	$(CC) $(CFLAGS) $(STD_CFLAGS) $(BUILD_LDFLAGS) -rdynamic -o $(TEST_BINARY) $< -L. -lexecinfo -ldl
	LD_LIBRARY_PATH=. ./$(TEST_BINARY)

# Pkg-config file, generated from the libexecinfo.pc.in template via
# plain variable substitution. The previous version built this file
# with a sequence of `@echo ... >> $@` lines and tried to reference
# earlier-written values via $${prefix} etc. -- but each `@echo` runs
# in its own subshell, so $${prefix} expanded to the *shell's* (empty)
# variable, not the value written into the file a line earlier. The
# result was a .pc file with empty libdir/includedir/exec_prefix unless
# patched afterward (which is what the PKGBUILD's prepare() step was
# working around). Driving it from the template removes the need for
# that downstream patch entirely.
libexecinfo.pc: libexecinfo.pc.in
	sed -e 's|@PREFIX@|$(PREFIX)|g' \
	    -e 's|@LIBDIR@|$(LIBDIR)|g' \
	    -e 's|@INCLUDEDIR@|$(INCLUDEDIR)|g' \
	    -e 's|@VERSION@|$(VERSION)|g' \
	    $< > $@

# Install targets
install: install-dynamic install-static install-headers install-pkgconfig

install-static: $(STATIC_LIB)
	$(INSTALL) -D -m644 $< $(DESTDIR)$(LIBDIR)/$<

install-dynamic: $(SHARED_LIB)
	$(INSTALL) -D -m755 $< $(DESTDIR)$(LIBDIR)/$<
	ln -sf $< $(DESTDIR)$(LIBDIR)/$(SONAME)
	ln -sf $< $(DESTDIR)$(LIBDIR)/libexecinfo.so
	ldconfig -n $(DESTDIR)$(LIBDIR) 2>/dev/null || true

install-headers:
	$(INSTALL) -D -m644 execinfo.h $(DESTDIR)$(INCLUDEDIR)/execinfo.h
	$(INSTALL) -D -m644 stacktraverse.h $(DESTDIR)$(INCLUDEDIR)/stacktraverse.h

install-pkgconfig: libexecinfo.pc
	$(INSTALL) -D -m644 $< $(DESTDIR)$(PKGCONFIGDIR)/$<

# Uninstall
uninstall:
	rm -f $(DESTDIR)$(LIBDIR)/libexecinfo.*
	rm -f $(DESTDIR)$(INCLUDEDIR)/execinfo.h
	rm -f $(DESTDIR)$(INCLUDEDIR)/stacktraverse.h
	rm -f $(DESTDIR)$(PKGCONFIGDIR)/libexecinfo.pc

# Clean
clean:
	rm -f *.o *.So *.a *.so *.so.* $(TEST_BINARY) libexecinfo.pc .build_sig
	rm -f $(GENERATED_FILES)

# Help
help:
	@echo "libexecinfo build system"
	@echo ""
	@echo "Targets:"
	@echo "  all              - Build static and dynamic libraries (default)"
	@echo "  static           - Build static library only"
	@echo "  dynamic          - Build dynamic library only"
	@echo "  test             - Build AND RUN the test program (static lib)"
	@echo "  test-dynamic     - Build AND RUN the test program (dynamic lib)"
	@echo "  generate         - Generate stacktraverse.c"
	@echo "  install          - Install everything"
	@echo "  clean            - Clean build artifacts"
	@echo ""
	@echo "Variables:"
	@echo "  DEBUG=1          - Enable debug build with sanitizers"
	@echo "  PREFIX=/path     - Installation prefix (default: /usr/local)"
	@echo "  CC=compiler      - C compiler to use"
	@echo "  PYTHON=python    - Python interpreter for code generation"
