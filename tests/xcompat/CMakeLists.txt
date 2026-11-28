set(XCOMPAT_FIXTURE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/fixtures")
file(MAKE_DIRECTORY "${XCOMPAT_FIXTURE_DIRECTORY}" "${XCOMPAT_FIXTURE_DIRECTORY}/unsearched")

function(configure_fixture target output_name)
    set_target_properties(${target}
        PROPERTIES
            OUTPUT_NAME "${output_name}"
            RUNTIME_OUTPUT_DIRECTORY "${XCOMPAT_FIXTURE_DIRECTORY}"
            LIBRARY_OUTPUT_DIRECTORY "${XCOMPAT_FIXTURE_DIRECTORY}"
            ARCHIVE_OUTPUT_DIRECTORY "${XCOMPAT_FIXTURE_DIRECTORY}"
            CXX_STANDARD 23
            CXX_STANDARD_REQUIRED YES
            CXX_EXTENSIONS NO
    )
endfunction()

function(configure_no_crt_executable target entry)
    set_property(TARGET ${target} PROPERTY MSVC_RUNTIME_CHECKS "")
    target_compile_options(${target} PRIVATE /GS- $<$<COMPILE_LANGUAGE:CXX>:/EHs-c->)
    target_link_options(${target} PRIVATE /NODEFAULTLIB "/ENTRY:${entry}" /SUBSYSTEM:CONSOLE
        /DYNAMICBASE /FIXED:NO /INCREMENTAL:NO)
endfunction()

function(configure_no_entry_library target base)
    set_property(TARGET ${target} PROPERTY MSVC_RUNTIME_CHECKS "")
    target_compile_options(${target} PRIVATE /GS-)
    target_link_options(${target} PRIVATE /NOENTRY /NODEFAULTLIB /DYNAMICBASE /FIXED:NO
        "/BASE:${base}")
endfunction()

add_library(xcompat-fixture-provider SHARED fixtures/provider.c fixtures/provider.def)
configure_fixture(xcompat-fixture-provider provider)
configure_no_entry_library(xcompat-fixture-provider 0x0000000180100000)

add_library(xcompat-fixture-delay-provider SHARED fixtures/provider.c fixtures/delay_provider.def)
configure_fixture(xcompat-fixture-delay-provider delayprovider)
configure_no_entry_library(xcompat-fixture-delay-provider 0x0000000180200000)

add_library(xcompat-fixture-forward SHARED fixtures/forward.c)
configure_fixture(xcompat-fixture-forward forward)
configure_no_entry_library(xcompat-fixture-forward 0x0000000180300000)

add_library(xcompat-fixture-forward-cycle-a SHARED fixtures/forward_cycle_a.c)
configure_fixture(xcompat-fixture-forward-cycle-a forward_cycle_a)
configure_no_entry_library(xcompat-fixture-forward-cycle-a 0x0000000180400000)

add_library(xcompat-fixture-forward-cycle-b SHARED fixtures/forward_cycle_b.c)
configure_fixture(xcompat-fixture-forward-cycle-b forward_cycle_b)
configure_no_entry_library(xcompat-fixture-forward-cycle-b 0x0000000180500000)

add_library(xcompat-fixture-leaf SHARED fixtures/provider.c fixtures/leaf.def)
configure_fixture(xcompat-fixture-leaf leaf)
configure_no_entry_library(xcompat-fixture-leaf 0x0000000180600000)

add_library(xcompat-fixture-missing-export-forward SHARED fixtures/missing_export_forward.c)
configure_fixture(xcompat-fixture-missing-export-forward missing_export_forward)
configure_no_entry_library(xcompat-fixture-missing-export-forward 0x0000000180700000)

add_library(xcompat-fixture-absent-provider SHARED fixtures/provider.c fixtures/absent_provider.def)
set_target_properties(xcompat-fixture-absent-provider PROPERTIES
    OUTPUT_NAME absent_provider
    RUNTIME_OUTPUT_DIRECTORY "${XCOMPAT_FIXTURE_DIRECTORY}/unsearched"
    LIBRARY_OUTPUT_DIRECTORY "${XCOMPAT_FIXTURE_DIRECTORY}/unsearched"
    ARCHIVE_OUTPUT_DIRECTORY "${XCOMPAT_FIXTURE_DIRECTORY}/unsearched"
)
configure_no_entry_library(xcompat-fixture-absent-provider 0x0000000180800000)

add_executable(xcompat-fixture-entry fixtures/entry_root.cpp)
configure_fixture(xcompat-fixture-entry entry_root)
configure_no_crt_executable(xcompat-fixture-entry entry)
target_link_libraries(xcompat-fixture-entry PRIVATE phnt::phnt)

add_executable(xcompat-fixture-binding fixtures/binding_root.c)
configure_fixture(xcompat-fixture-binding binding_root)
configure_no_crt_executable(xcompat-fixture-binding entry)
target_link_libraries(xcompat-fixture-binding PRIVATE
    xcompat-fixture-provider xcompat-fixture-forward xcompat-fixture-delay-provider delayimp kernel32
)
target_link_options(xcompat-fixture-binding PRIVATE /DELAYLOAD:delayprovider.dll)

add_executable(xcompat-fixture-stack fixtures/stack_root.c)
configure_fixture(xcompat-fixture-stack stack_root)
configure_no_crt_executable(xcompat-fixture-stack entry)
target_link_libraries(xcompat-fixture-stack PRIVATE phnt::phnt kernel32)
target_link_options(xcompat-fixture-stack PRIVATE /STACK:2097152,131072)

add_executable(xcompat-fixture-missing-provider fixtures/missing_provider_root.c)
configure_fixture(xcompat-fixture-missing-provider missing_provider_root)
configure_no_crt_executable(xcompat-fixture-missing-provider entry)
target_link_libraries(xcompat-fixture-missing-provider PRIVATE xcompat-fixture-absent-provider)

add_executable(xcompat-fixture-forward-cycle-root fixtures/forward_cycle_root.c)
configure_fixture(xcompat-fixture-forward-cycle-root forward_cycle_root)
configure_no_crt_executable(xcompat-fixture-forward-cycle-root entry)
target_link_libraries(xcompat-fixture-forward-cycle-root PRIVATE xcompat-fixture-forward-cycle-a)

add_executable(xcompat-fixture-missing-export-root fixtures/missing_export_root.c)
configure_fixture(xcompat-fixture-missing-export-root missing_export_root)
configure_no_crt_executable(xcompat-fixture-missing-export-root entry)
target_link_libraries(xcompat-fixture-missing-export-root PRIVATE xcompat-fixture-missing-export-forward)

add_executable(xcompat-guest-tests guest_tests.cpp)
set_target_properties(xcompat-guest-tests PROPERTIES
    CXX_STANDARD 23
    CXX_STANDARD_REQUIRED YES
    CXX_EXTENSIONS NO
    RUNTIME_OUTPUT_DIRECTORY "${XCOMPAT_RUNTIME_DIRECTORY}"
)
target_compile_definitions(xcompat-guest-tests PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
target_link_libraries(xcompat-guest-tests PRIVATE xcompat)
add_dependencies(xcompat-guest-tests
    xcompat-fixture-entry xcompat-fixture-binding xcompat-fixture-stack
    xcompat-fixture-missing-provider xcompat-fixture-forward-cycle-root
    xcompat-fixture-missing-export-root xcompat-fixture-forward-cycle-b
    xcompat-fixture-leaf
)

function(add_xcompat_case name case_name fixture labels)
    add_test(NAME "xcompat.${name}"
        COMMAND xcompat-guest-tests "${case_name}" "${fixture}")
    set_tests_properties("xcompat.${name}" PROPERTIES
        LABELS "${labels}"
        TIMEOUT 30
        ENVIRONMENT "XCOMPAT_GUEST_TEST=present"
    )
endfunction()

add_xcompat_case(loader.paths paths "${XCOMPAT_FIXTURE_DIRECTORY}" loader)
add_xcompat_case(loader.binding binding "$<TARGET_FILE:xcompat-fixture-binding>" loader)
add_xcompat_case(loader.unresolved unresolved "${XCOMPAT_FIXTURE_DIRECTORY}" loader)
add_xcompat_case(guest.entry entry "$<TARGET_FILE:xcompat-fixture-entry>" guest)
add_xcompat_case(guest.stack stack "$<TARGET_FILE:xcompat-fixture-stack>" guest)

set(entry_arguments "arg with spaces" "quote\"inside" "trailing\\")
add_test(NAME xcompat.cli.entry COMMAND $<TARGET_FILE:xcompat-cli>
    $<TARGET_FILE:xcompat-fixture-entry> ${entry_arguments})
set_tests_properties(xcompat.cli.entry PROPERTIES
    LABELS guest
    TIMEOUT 30
    ENVIRONMENT "XCOMPAT_GUEST_TEST=present"
)
