# C test suite for libsnobol4 core

# Pike scan feature flag (must match core build)
if(ENABLE_PIKE_SCAN)
    add_compile_definitions(SNOBOL_PIKE_SCAN=1)
endif()

# Test executable
add_executable(snobol4_tests
    test_runner.c
    test_vm.c
    test_tables.c
    test_array.c
    test_dynamic_patterns.c
    test_table_ops.c
    test_template_ops.c
    test_control_flow.c
    test_backtracking.c
    test_stress_backtracking.c
    test_catastrophic.c
    test_search_meta_cache.c
    test_search.c
    test_search_ex_api.c
    test_search_vm_capture.c
    test_capture_registers.c
    test_search_anchored.c
    test_pike_scan.c
    test_lexer.c
    test_parser.c
    test_ast.c
    # Built-in functions test suites
    test_pattern_breakx.c
    test_pattern_bal.c
    test_pattern_fence.c
    test_pattern_pos_tab.c
    test_pattern_abort_fail_succeed.c
    test_string_size.c
    test_string_transform.c
    test_string_ops.c
    test_string_char.c
    test_string_case.c
    test_comparison.c
    test_comparison_numeric.c
    test_builtin_dispatch.c
    test_search_runtime.c
    test_fusion.c
    test_fusion_tier.c
    test_compact_choice.c
    test_vm_trail.c
    test_choice_arena.c
    test_choice_shortcircuit.c
    test_search_automaton.c
    test_search_simd.c
    test_search_alt_literals.c
    test_automaton_skip.c
    test_arena.c
    # New: Unicode fold, case-insensitive patterns, API version
    test_unicode_fold.c
    test_pattern_case.c
    test_api_version.c
    test_api_match.c
    test_api_literal_match.c
    test_api_search_next.c
    test_builder_compile.c
    test_reusable_match.c
    test_reuse_search.c
    test_break_grammar.c
    test_compiler.c
    test_property_based.c
    test_source_parity.c
    test_search_prefilter.c
    test_search_batch.c
    test_search_batch_ex.c
    test_search_oracle.c
)

# Link against core library
target_link_libraries(snobol4_tests PRIVATE snobol4)

# Use C23 standard (MSVC uses /std:c17 set below in the warning-flags block)
if(NOT MSVC)
    target_compile_features(snobol4_tests PRIVATE c_std_23)
endif()

# Enable profiling for tests
target_compile_definitions(snobol4_tests PRIVATE SNOBOL_PROFILE=1)

# Propagate sanitizer flags to test binaries
if(SNOBOL_SANITIZE)
    target_compile_options(snobol4_tests PRIVATE
        -fsanitize=address,undefined
        -fno-omit-frame-pointer
    )
    target_link_options(snobol4_tests PUBLIC
        -fsanitize=address,undefined
    )
endif()

if(MSVC)
    target_compile_options(snobol4_tests PRIVATE
        /W4
        /wd4100  # unreferenced formal parameter
        /wd4101  # unreferenced local variable
        /wd4189  # local variable initialized but not referenced
        /wd4244  # conversion (possible loss of data)
        /wd4267  # conversion from 'size_t' to smaller type
        /wd4310  # cast truncates constant value
        /wd4333  # '>>': right shift by too large amount
        /wd4389  # '==': signed/unsigned mismatch
        # Use C17 so nullptr is not a keyword (C23), allowing snobol_internal.h
        # compat #define nullptr NULL to work on MSVC 2022+.
        /std:c17
    )
    # Suppress MSVC safe-CRT deprecation warnings in test sources
    target_compile_definitions(snobol4_tests PRIVATE _CRT_SECURE_NO_WARNINGS)
else ()
    target_compile_options(snobol4_tests PRIVATE
            -Wall -Wextra
            -Wno-unused-variable
            -Wno-unused-but-set-variable
            -Wno-unused-function
            -Wno-sign-conversion
    )
    if(CMAKE_C_COMPILER_ID MATCHES "Clang")
        target_compile_options(snobol4_tests PRIVATE
            -Wno-implicit-int-conversion
            -Wno-shorten-64-to-32
        )
    endif()
endif()

# Register with CTest
add_test(
    NAME snobol4_c_tests
    COMMAND snobol4_tests
)

