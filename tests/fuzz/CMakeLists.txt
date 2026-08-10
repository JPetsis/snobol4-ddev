# libFuzzer fuzz harnesses for libsnobol4
# Built only with Clang and SNOBOL_FUZZ=ON

add_executable(fuzz_compiler fuzz_compiler.c)
target_link_libraries(fuzz_compiler PRIVATE snobol4)
target_compile_options(fuzz_compiler PRIVATE -fsanitize=fuzzer)
target_link_options(fuzz_compiler PRIVATE -fsanitize=fuzzer)

add_executable(fuzz_vm fuzz_vm.c)
target_link_libraries(fuzz_vm PRIVATE snobol4)
target_compile_options(fuzz_vm PRIVATE -fsanitize=fuzzer)
target_link_options(fuzz_vm PRIVATE -fsanitize=fuzzer)
add_executable(fuzz_oracle fuzz_oracle.c)
target_link_libraries(fuzz_oracle PRIVATE snobol4)
target_compile_options(fuzz_oracle PRIVATE -fsanitize=fuzzer)
target_link_options(fuzz_oracle PRIVATE -fsanitize=fuzzer)
