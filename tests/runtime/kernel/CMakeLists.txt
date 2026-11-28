set_target_properties(xcompat-kernel PROPERTIES WINDOWS_EXPORT_ALL_SYMBOLS ON)

add_executable(xcompat-kernel-mmu-tests
    mmu_tests.cpp
)

set_target_properties(xcompat-kernel-mmu-tests
    PROPERTIES
        CXX_STANDARD 23
        CXX_STANDARD_REQUIRED YES
        CXX_EXTENSIONS NO
        RUNTIME_OUTPUT_DIRECTORY "${XCOMPAT_RUNTIME_DIRECTORY}"
)

target_compile_definitions(xcompat-kernel-mmu-tests
    PRIVATE
        NOMINMAX
        WIN32_LEAN_AND_MEAN
)

target_link_libraries(xcompat-kernel-mmu-tests
    PRIVATE
        xcompat-kernel
)

function(add_mmu_test name label)
    add_test(
        NAME "runtime.kernel.mmu.${name}"
        COMMAND xcompat-kernel-mmu-tests "${name}"
    )

    set_tests_properties("runtime.kernel.mmu.${name}"
        PROPERTIES
            LABELS "mmu;${label}"
            TIMEOUT 30
    )
endfunction()

add_mmu_test(physical-single regression)
add_mmu_test(physical-batch regression)
add_mmu_test(physical-count-semantics regression)
add_mmu_test(physical-repeated regression)
add_mmu_test(physical-double-free regression)
add_mmu_test(reservation-basic regression)
add_mmu_test(reservation-rounding regression)
add_mmu_test(reservation-explicit-conflict regression)
add_mmu_test(reservation-invalid regression)
add_mmu_test(mapping-single regression)
add_mmu_test(mapping-range regression)
add_mmu_test(mapping-validation regression)
add_mmu_test(aliasing-persistence regression)
add_mmu_test(physical-lifetime regression)
add_mmu_test(placeholder-subdivision regression)
add_mmu_test(placeholder-active-release regression)
add_mmu_test(bulk-contracts regression)

add_mmu_test(concurrent-physical stress)
add_mmu_test(concurrent-reservations stress)
add_mmu_test(concurrent-aliases stress)
add_mmu_test(large-batch stress)
add_mmu_test(repeated-cycles stress)
add_mmu_test(sparse-mappings stress)
