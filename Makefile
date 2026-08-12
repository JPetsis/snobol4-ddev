# libsnobol4 Makefile
# Simple wrapper around CMake build system
#
# Usage:
#   make          - Configure and build
#   make test     - Run tests
#   make clean    - Clean build artifacts
#   make help     - Show all targets

.PHONY: all build test clean distclean install uninstall help \
        build-debug build-release build-asan build-pgo-gen build-pgo-use pgo-train \
        test-verbose test-valgrind test-valgrind-report test-asan bench bench-c docs man install-man format format-php lint warnings \
        gen-unicode-fold

# Default build type
BUILD_TYPE ?= Release
CMAKE_EXTRA_FLAGS ?=

# Detect if we're in DDEV
IN_DDEV := $(shell [ -n "$$IS_DDEV_PROJECT" ] && echo 1 || echo 0)

# DDEV wrapper
ifneq ($(IN_DDEV),1)
    DDEV := $(shell command -v ddev 2> /dev/null)
    ifneq ($(DDEV),)
        # DDEV available but not in project - wrap commands
        DDEV_EXEC = ddev exec
    endif
endif

# CMake command
CMAKE = cmake
CMAKE_BUILD = $(CMAKE) --build build
CMAKE_TEST = ctest --test-dir build

# Default target
all: build

# Configure and build
# Note: LTO is pinned to ON — it is the canonical Release configuration and
# the perf baseline is captured from this build. An explicit -D also beats a
# stale SNOBOL_LTO=OFF cached from older Makefiles; override with
# CMAKE_EXTRA_FLAGS=-DSNOBOL_LTO=OFF if you really want a no-LTO build.
build:
	@echo "==> Configuring libsnobol4 ($(BUILD_TYPE))..."
	$(CMAKE) -B build \
		-DCMAKE_BUILD_TYPE=$(BUILD_TYPE) \
		-DBUILD_TESTS=ON \
		-DBUILD_PHP=OFF \
		-DSNOBOL_LTO=ON \
		-DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
		$(CMAKE_EXTRA_FLAGS)
	@echo "==> Building libsnobol4..."
	$(CMAKE_BUILD)
	@echo "==> Build complete!"
	@echo "==> compile_commands.json generated for IDE support"

# Build with different configurations
build-debug:
	@$(MAKE) build BUILD_TYPE=Debug

build-release:
	@$(MAKE) build BUILD_TYPE=Release

build-asan:
	@echo "==> Configuring build with AddressSanitizer + UBSan..."
	$(CMAKE) -B build-asan \
		-DCMAKE_BUILD_TYPE=Debug \
		-DBUILD_TESTS=ON \
		-DBUILD_PHP=OFF \
		-DSNOBOL_SANITIZE=ON \
		-DCMAKE_EXPORT_COMPILE_COMMANDS=ON
	@echo "==> Building with sanitizers..."
	$(CMAKE) --build build-asan

# Profile-Guided Optimization (Priority 6.9)
# Workflow: build-pgo-gen -> pgo-train -> build-pgo-use
PGO_DIR ?= build-pgo

build-pgo-gen:
	@echo "==> Configuring PGO instrumentation build (Release + -fprofile-generate)..."
	$(CMAKE) -B build-pgo-gen \
		-DCMAKE_BUILD_TYPE=Release \
		-DBUILD_TESTS=ON \
		-DBUILD_PHP=OFF \
		-DBUILD_BENCH_C=ON \
		-DSNOBOL_PGO_GEN=ON \
		-DCMAKE_EXPORT_COMPILE_COMMANDS=ON
	@echo "==> Building PGO instrumented probe..."
	$(CMAKE) --build build-pgo-gen --target snobol4_probe
	@echo "==> PGO instrumentation build complete. Next: make pgo-train"

pgo-train:
	@echo "==> Training PGO profile (running snobol4_probe)..."
	@test -f build-pgo-gen/bench/c/snobol4_probe || \
		(echo "PGO instrumented binary not found. Run 'make build-pgo-gen' first." && exit 1)
	@mkdir -p $(PGO_DIR)
	cd build-pgo-gen && ./bench/c/snobol4_probe
	@echo "==> Merging profile data with llvm-profdata..."
	@if command -v llvm-profdata >/dev/null 2>&1; then \
		llvm-profdata merge -output=$(PGO_DIR)/profdata build-pgo-gen/default_*.profraw 2>/dev/null || \
		llvm-profdata merge -output=$(PGO_DIR)/profdata build-pgo-gen/**/default_*.profraw 2>/dev/null || true; \
	elif command -v xcrun >/dev/null 2>&1; then \
		xcrun llvm-profdata merge -output=$(PGO_DIR)/profdata build-pgo-gen/default_*.profraw 2>/dev/null || true; \
	else \
		echo "llvm-profdata not found; relying on GCC-style .gcda next to objects."; \
	fi
	@echo "==> Profile written to $(PGO_DIR)/profdata. Next: make build-pgo-use"

build-pgo-use:
	@test -f $(PGO_DIR)/profdata || \
		(echo "Merged profile not found. Run 'make pgo-train' first." && exit 1)
	@echo "==> Configuring PGO optimized build (-fprofile-use)..."
	$(CMAKE) -B build-pgo-use \
		-DCMAKE_BUILD_TYPE=Release \
		-DBUILD_TESTS=ON \
		-DBUILD_PHP=OFF \
		-DSNOBOL_PGO_USE=ON \
		-DSNOBOL_PGO_PROFILE=$(CURDIR)/$(PGO_DIR)/profdata \
		-DCMAKE_EXPORT_COMPILE_COMMANDS=ON
	@echo "==> Building PGO optimized library..."
	$(CMAKE) --build build-pgo-use
	@echo "==> PGO optimized build complete!"

# Run tests
test:
	@echo "==> Running C tests..."
	@if [ -f build/tests/c/snobol4_tests ]; then \
		./build/tests/c/snobol4_tests; \
	elif [ -d build ]; then \
		echo "Test binary not found. Run 'make build' first."; \
		exit 1; \
	else \
		echo "Build directory not found. Run 'make build' first."; \
		exit 1; \
	fi

test-verbose:
	@echo "==> Running tests via CTest (verbose)..."
	@if [ -d build ]; then \
		$(CMAKE_TEST) --verbose --output-on-failure; \
	else \
		echo "Build directory not found. Run 'make build' first."; \
		exit 1; \
	fi

# Run tests under Valgrind (delegates to CMake test-valgrind target)
test-valgrind:
	@echo "==> Running tests under Valgrind (via CMake target)..."
	@if [ -d build ]; then \
		$(CMAKE_BUILD) --target test-valgrind; \
	else \
		echo "Build directory not found. Run 'make build' first."; \
		exit 1; \
	fi

# Run tests under Valgrind and save XML report to build/valgrind-report.xml
test-valgrind-report:
	@echo "==> Running tests under Valgrind (XML report → build/valgrind-report.xml)..."
	@if [ -d build ]; then \
		$(CMAKE_BUILD) --target test-valgrind-report; \
		echo "==> Report saved to build/valgrind-report.xml"; \
	else \
		echo "Build directory not found. Run 'make build' first."; \
		exit 1; \
	fi

# Run tests with AddressSanitizer + UBSan (delegates to CMake test-asan target)
test-asan:
	@echo "==> Running ASan/UBSan tests (via CMake target)..."
	@if [ -d build-asan ]; then \
		$(CMAKE) --build build-asan --target test-asan; \
	else \
		echo "ASan build directory not found. Run 'make build-asan' first."; \
		exit 1; \
	fi

# Clean targets
# Removes all CMake build trees (build/, build-asan/, build-win/, cmake-build-*/)
# and transient benchmark result files.
clean:
	@echo "==> Cleaning all build directories..."
	rm -rf build*/ cmake-build-*/
	@echo "==> Cleaning generated version header..."
	rm -f core/include/snobol/version.h
	@echo "==> Cleaning benchmark results..."
	find bench -maxdepth 1 -type f -name 'results_*.json' ! -name 'results_example.json' ! -name 'results_builtin.json' ! -name 'results_layered_search.json' -delete 2>/dev/null || true
	@echo "==> Cleaning coverage artifacts..."
	find . -maxdepth 3 \( -name '*.info' -o -name '*.gcda' -o -name '*.gcno' \) -delete 2>/dev/null || true
	@echo "==> Cleaning clangd index cache..."
	rm -rf .cache/
	@echo "==> Clean complete!"

# Uninstall previously installed files using CMake's install manifest.
# Removes headers, library, CMake config files, and libsnobol4.pc.
# Set INSTALL_PREFIX to match the prefix used during 'make install' (default: /usr/local).
INSTALL_PREFIX ?= /usr/local
uninstall:
	@echo "==> Uninstalling libsnobol4 from $(INSTALL_PREFIX)..."
	@if [ -f build/install_manifest.txt ]; then \
		echo "Using build/install_manifest.txt..."; \
		xargs rm -f < build/install_manifest.txt; \
		rm -f build/install_manifest.txt; \
	else \
		echo "No install manifest found in build/. Removing known paths manually..."; \
		rm -rf $(INSTALL_PREFIX)/include/snobol; \
		rm -f  $(INSTALL_PREFIX)/lib/libsnobol4.a; \
		rm -rf $(INSTALL_PREFIX)/lib/cmake/libsnobol4; \
		rm -f  $(INSTALL_PREFIX)/lib/pkgconfig/libsnobol4.pc; \
		rm -f  $(INSTALL_PREFIX)/bin/example_basic; \
		rm -f  $(INSTALL_PREFIX)/bin/example_captures; \
	fi
	@echo "==> Uninstall complete!"

distclean: clean uninstall
	@echo "==> Removing generated config files..."
	rm -f snobol4-core/config.h.in~
	rm -f snobol4-core/configure~
	@echo "==> Deep clean complete!"

# Install (generates build/install_manifest.txt used by 'make uninstall')
install:
	@echo "==> Installing libsnobol4 to $(INSTALL_PREFIX)..."
	$(CMAKE) --install build --prefix $(INSTALL_PREFIX)
	@echo "==> Install complete! Run 'make uninstall' to reverse."

# Benchmark (requires PHP extension)
bench:
	@echo "==> Running benchmark suite..."
	@if command -v php >/dev/null 2>&1; then \
		for bench in bench/tokenize.php bench/replace.php bench/dates.php bench/backtracking.php; do \
			if [ -f "$$bench" ]; then \
				echo "Running $$bench..."; \
				php "$$bench"; \
			fi; \
		done; \
	else \
		echo "PHP not found. Benchmarks require PHP extension."; \
		echo "Run 'make build-php' to build PHP binding first."; \
	fi

# C microbenchmark suite (compile libsnobol4 vs PCRE2 at C level)
bench-c:
	@echo "==> Building and running C microbenchmarks..."
	$(CMAKE) -B build \
		-DCMAKE_BUILD_TYPE=$(BUILD_TYPE) \
		-DBUILD_TESTS=ON \
		-DBUILD_PHP=OFF \
		-DBUILD_BENCH_C=ON \
		$(CMAKE_EXTRA_FLAGS)
	$(CMAKE_BUILD) --target snobol4_bench
	@echo ""
	./build/bench/c/snobol4_bench

# Head-to-head comparison benchmarks (requires PHP + snobol extension)
# Generates bench/BENCHMARKS.md with results.
bench-comparison:
	@echo "==> Running PCRE2 comparison benchmarks..."
	@if command -v php >/dev/null 2>&1; then \
		php bench/compare_pcre2.php; \
		echo "==> Results written to bench/BENCHMARKS.md"; \
	else \
		echo "PHP not found. Benchmarks require PHP extension."; \
		echo "Run 'make build-php' to build PHP binding first."; \
	fi

# Documentation
docs:
	@echo "==> Generating documentation..."
	@if command -v doxygen >/dev/null 2>&1; then \
		if [ -f Doxyfile ]; then \
			doxygen Doxyfile; \
			echo "Documentation generated in html/"; \
		else \
			echo "Doxyfile not found. Creating basic documentation..."; \
		fi; \
	else \
		echo "Doxygen not installed. Install with: brew install doxygen (macOS) or apt install doxygen (Linux)"; \
	fi

# Man pages (section 3) generated by Doxygen into docs/api/man/
man: docs
	@echo "==> Man pages generated in docs/api/man/ (man3)"
	@if [ -d docs/api/man ]; then ls docs/api/man | head -5; echo "..."; fi

# Install man pages (man3) next to the library
install-man: man
	@echo "==> Installing man pages to $(INSTALL_PREFIX)/share/man/man3/"
	@if [ -d docs/api/man/man3 ]; then \
		mkdir -p "$(INSTALL_PREFIX)/share/man/man3"; \
		cp docs/api/man/man3/*.3 "$(INSTALL_PREFIX)/share/man/man3/"; \
		echo "Man pages installed."; \
	else \
		echo "No man pages found in docs/api/man/man3. Run 'make docs' first."; \
	fi

# Code formatting
format:
	@echo "==> Formatting code..."
	@if command -v clang-format >/dev/null 2>&1; then \
		find core/include core/src tests/c \( -name '*.c' -o -name '*.h' \) | xargs clang-format -i; \
		echo "Code formatted!"; \
	else \
		echo "clang-format not found. Install with: brew install clang-format (macOS) or apt install clang-format (Linux)"; \
	fi

# Format the PHP binding C sources (bindings/php/src)
format-php:
	@echo "==> Formatting PHP binding sources..."
	@if command -v clang-format >/dev/null 2>&1; then \
		find bindings/php/src \( -name '*.c' -o -name '*.h' \) | xargs clang-format -i; \
		echo "PHP binding formatted!"; \
	else \
		echo "clang-format not found. Install with: brew install clang-format (macOS) or apt install clang-format (Linux)"; \
	fi

# Linting
lint:
	@echo "==> Running linter..."
	@if command -v clang-tidy >/dev/null 2>&1; then \
		if [ -d build ]; then \
			LINT_EXTRA=""; \
			if command -v xcrun >/dev/null 2>&1; then \
				LINT_EXTRA="--extra-arg=-isysroot$$(xcrun --show-sdk-path)"; \
			fi; \
			find core/include core/src tests/c -name '*.c' | xargs clang-tidy -p build $$LINT_EXTRA; \
		else \
			echo "Build directory not found. Run 'make build' first."; \
		fi; \
	else \
		echo "clang-tidy not found. Options:"; \
		echo ""; \
		echo "  Option 1: Install LLVM toolchain with clang-tidy"; \
		echo "    macOS:  brew install llvm"; \
		echo "            (clang-tidy is included in LLVM)"; \
		echo "    Then ensure PATH includes LLVM bin:"; \
		echo "    export PATH=\"\$$PATH:/opt/homebrew/opt/llvm/bin\""; \
		echo ""; \
		echo "  Option 2: Use compiler warnings instead (no extra install)"; \
		echo "    make warnings"; \
		echo "    (Builds with -Wall -Wextra -Wpedantic -Werror)"; \
		echo ""; \
		echo "  Option 3: Run clang-tidy manually"; \
		echo "    clang-tidy core/src/*.c -- -Icore/include"; \
		echo "    (Uses compile_commands.json from build/)"; \
		exit 0; \
	fi

# Build with strict warnings (alternative to clang-tidy)
warnings:
	@echo "==> Building with strict warnings..."
	$(CMAKE) -B build \
		-DCMAKE_BUILD_TYPE=Debug \
		-DBUILD_TESTS=ON \
		-DBUILD_PHP=OFF \
		-DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
		-DCMAKE_C_FLAGS="-Wall -Wextra -Wpedantic"
	$(CMAKE_BUILD)
	@echo "==> Build with strict warnings complete!"
	@echo "Note: -Werror omitted to allow unused placeholders in active development"

# Generate Unicode fold tables from UCD
gen-unicode-fold:
	@echo "==> Generating Unicode fold tables..."
	cc -o /tmp/gen_unicode_fold dev/gen_unicode_fold.c
	/tmp/gen_unicode_fold > core/src/unicode_fold_data.c
	@echo "==> core/src/unicode_fold_data.c regenerated"
	@echo "==> Rebuild the project to pick up changes: make build"

# PHP binding build
build-php:
	@echo "==> Building with PHP binding..."
	$(CMAKE) -B build \
		-DCMAKE_BUILD_TYPE=$(BUILD_TYPE) \
		-DBUILD_TESTS=ON \
		-DBUILD_PHP=ON \
		$(CMAKE_EXTRA_FLAGS)
	$(CMAKE_BUILD)
	@echo "==> PHP binding build complete!"

# Help
help:
	@echo "libsnobol4 Build System"
	@echo "======================="
	@echo ""
	@echo "Build targets:"
	@echo "  all (default)  - Configure and build the project"
	@echo "  build          - Build with default configuration (Release)"
	@echo "  build-debug    - Build with Debug configuration"
	@echo "  build-release  - Build with Release configuration"
	@echo "  build-asan     - Build with AddressSanitizer+UBSan (SNOBOL_SANITIZE=ON)"
	@echo "  build-pgo-gen  - PGO instrumentation build (train with: make pgo-train)"
	@echo "  pgo-train      - Run probe to generate PGO profile (build-pgo/profdata)"
	@echo "  build-pgo-use  - PGO optimized build using the generated profile"
	@echo "  build-php      - Build with PHP binding (requires PHP dev headers)"
	@echo "  gen-unicode-fold - Regenerate BMP case-folding tables from UCD"
	@echo ""
	@echo "Test targets:"
	@echo "  test           - Run C test suite"
	@echo "  test-verbose   - Run tests with verbose output"
	@echo "  test-valgrind  - Run tests under Valgrind (terminal output)"
	@echo "  test-valgrind-report - Run tests under Valgrind (XML report → build/valgrind-report.xml)"
	@echo "  test-asan      - Run tests under AddressSanitizer+UBSan (requires make build-asan first)"
	@echo ""
	@echo "Clean targets:"
	@echo "  clean          - Remove all build directories (build/, build-asan/, cmake-build-*/, etc.) and transient benchmark artifacts"
	@echo "  uninstall      - Remove installed files (headers, lib, cmake config, libsnobol4.pc); uses build/install_manifest.txt or falls back to INSTALL_PREFIX paths"
	@echo "  distclean      - clean + uninstall + remove generated config files"
	@echo ""
	@echo "Other targets:"
	@echo "  install        - Install to INSTALL_PREFIX (default: /usr/local); requires sudo if prefix needs root"
	@echo "  uninstall      - Uninstall from INSTALL_PREFIX (default: /usr/local)"
	@echo "  bench          - Run benchmark suite (requires PHP)"
	@echo "  bench-c        - Build & run C microbenchmarks (core vs PCRE2)"
	@echo "  docs           - Generate documentation (requires Doxygen)"
	@echo "  man            - Generate man pages (section 3) via Doxygen"
	@echo "  install-man    - Install man pages to INSTALL_PREFIX/share/man/man3"
	@echo "  format         - Format core C code (requires clang-format)"
	@echo "  format-php     - Format the PHP binding C sources (requires clang-format)"
	@echo "  lint           - Run clang-tidy (requires LLVM toolchain)"
	@echo "  warnings       - Build with strict warnings (alternative to lint)"
	@echo "  help           - Show this help message"
	@echo ""
	@echo "Examples:"
	@echo "  make                    # Build the project"
	@echo "  make build-debug        # Debug build"
	@echo "  make test               # Run tests"
	@echo "  make test-verbose       # Verbose test output"
	@echo "  make test-valgrind      # Run tests under Valgrind (terminal output)"
	@echo "  make test-valgrind-report # Run Valgrind + save XML to build/valgrind-report.xml"
	@echo "  make clean              # Remove all build directories"
	@echo "  make build-php          # Build with PHP binding"
	@echo "  make install            # Install to /usr/local (default)"
	@echo "  make install INSTALL_PREFIX=~/local  # Install to custom prefix"
	@echo "  make uninstall          # Remove installed files"
	@echo ""
	@echo "Variables:"
	@echo "  BUILD_TYPE       - CMake build type (default: Release)"
	@echo "  CMAKE_EXTRA_FLAGS - Additional CMake flags"
	@echo "  INSTALL_PREFIX   - Installation prefix for install/uninstall (default: /usr/local)"
	@echo ""
	@echo "Examples with variables:"
	@echo "  make build BUILD_TYPE=Debug"
	@echo "  make build CMAKE_EXTRA_FLAGS='-DBUILD_PHP=ON'"
