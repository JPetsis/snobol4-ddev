# C microbenchmark suite — libsnobol4 core vs PCRE2
# Compile once, match many times to isolate engine performance.

find_package(PkgConfig QUIET)
if(PkgConfig_FOUND)
    pkg_check_modules(PCRE2 libpcre2-8)
endif()

if(NOT PCRE2_FOUND)
    find_path(PCRE2_INCLUDE_DIR pcre2.h)
    find_library(PCRE2_LIBRARY pcre2-8)
    if(PCRE2_INCLUDE_DIR AND PCRE2_LIBRARY)
        set(PCRE2_FOUND TRUE)
        set(PCRE2_INCLUDE_DIRS ${PCRE2_INCLUDE_DIR})
        set(PCRE2_LIBRARIES ${PCRE2_LIBRARY})
    endif()
endif()

if(NOT PCRE2_FOUND)
    message(WARNING "pcre2-8 not found; C benchmarks will compare snobol4 vs (none). Install pcre2 to enable comparison.")
endif()

# Benchmark executable
add_executable(snobol4_bench
    bench_literal.c
    bench_alternation.c
    bench_tokenization.c
    bench_substitution.c
    bench_complex_http.c
    bench_delimiter.c
    bench_runner.c
)

target_link_libraries(snobol4_bench PRIVATE snobol4)
target_compile_features(snobol4_bench PRIVATE c_std_23)

if(MSVC)
    target_compile_options(snobol4_bench PRIVATE /std:c17 /W4)
    target_compile_definitions(snobol4_bench PRIVATE _CRT_SECURE_NO_WARNINGS)
else()
    target_compile_options(snobol4_bench PRIVATE -Wall -Wextra -Wno-unused-variable)
endif()

if(PCRE2_FOUND)
    target_include_directories(snobol4_bench PRIVATE ${PCRE2_INCLUDE_DIRS})
    target_compile_definitions(snobol4_bench PRIVATE HAVE_PCRE2=1)
    target_link_libraries(snobol4_bench PRIVATE ${PCRE2_LIBRARIES})
endif()

# Diagnostic probe — per-scenario timing breakdown with optional PCRE2 comparison.
add_executable(snobol4_probe
    bench_probe.c
)
target_link_libraries(snobol4_probe PRIVATE snobol4)
target_compile_features(snobol4_probe PRIVATE c_std_23)

# The baseline guard (PROBE_BASELINE=1) compares against a baseline captured
# with the same build configuration — no-LTO codegen is systematically
# slower, so cross-config comparisons read as mass "regressions".  The probe
# selects search_perf_baseline(_nolto).json based on this define.
if(SNOBOL_LTO)
    target_compile_definitions(snobol4_probe PRIVATE SNOBOL_LTO_BUILD=1)
endif()

if(PCRE2_FOUND)
    target_include_directories(snobol4_probe PRIVATE ${PCRE2_INCLUDE_DIRS})
    target_compile_definitions(snobol4_probe PRIVATE HAVE_PCRE2=1)
    target_link_libraries(snobol4_probe PRIVATE ${PCRE2_LIBRARIES})
endif()
if(MSVC)
    target_compile_options(snobol4_probe PRIVATE /std:c17 /W4)
    target_compile_definitions(snobol4_probe PRIVATE _CRT_SECURE_NO_WARNINGS)
else()
    target_compile_options(snobol4_probe PRIVATE -Wall -Wextra -Wno-unused-variable)
endif()

# Build only when bench/c is explicitly added (controlled by top-level option)
