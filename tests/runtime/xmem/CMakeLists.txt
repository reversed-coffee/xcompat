add_executable(xcompat-xmem-tests
    xmem_tests.cpp
)

set_target_properties(xcompat-xmem-tests
    PROPERTIES
        CXX_STANDARD 23
        CXX_STANDARD_REQUIRED YES
        CXX_EXTENSIONS NO
        RUNTIME_OUTPUT_DIRECTORY "${XCOMPAT_RUNTIME_DIRECTORY}"
)

target_compile_definitions(xcompat-xmem-tests
    PRIVATE
        NOMINMAX
        WIN32_LEAN_AND_MEAN
)

target_link_libraries(xcompat-xmem-tests
    PRIVATE
        xcompat-xmem
        xcompat-kernel
)

function(add_xmem_test name)
    add_test(
        NAME "runtime.xmem.${name}"
        COMMAND xcompat-xmem-tests "${name}"
    )

    set_tests_properties("runtime.xmem.${name}"
        PROPERTIES
            LABELS "xmem;regression"
            TIMEOUT 30
    )
endfunction()

add_xmem_test(default-allocation)
add_xmem_test(physical-mapping)
add_xmem_test(scatter-mapping)
add_xmem_test(virtual-and-statistics)
add_xmem_test(invalid-parameters)
