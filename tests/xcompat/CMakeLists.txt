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

function(configure_no_crt target entry)
    set_property(TARGET ${target} PROPERTY MSVC_RUNTIME_CHECKS "")
    target_compile_options(${target} PRIVATE /GS- $<$<COMPILE_LANGUAGE:CXX>:/EHs-c->)
    target_link_options(${target} PRIVATE /NODEFAULTLIB "/ENTRY:${entry}" /SUBSYSTEM:CONSOLE
        /DYNAMICBASE /FIXED:NO /INCREMENTAL:NO)
endfunction()

add_library(xcompat-fixture-provider SHARED fixtures/provider.c fixtures/provider.def)
configure_fixture(xcompat-fixture-provider provider)
configure_no_crt(xcompat-fixture-provider DllMain)
target_link_options(xcompat-fixture-provider PRIVATE /BASE:0x0000000180100000)

add_library(xcompat-fixture-delay-provider SHARED fixtures/provider.c fixtures/delay_provider.def)
configure_fixture(xcompat-fixture-delay-provider delayprovider)
configure_no_crt(xcompat-fixture-delay-provider DllMain)
target_link_options(xcompat-fixture-delay-provider PRIVATE /BASE:0x0000000180200000)

add_library(xcompat-fixture-forward SHARED fixtures/forward.c)
configure_fixture(xcompat-fixture-forward forward)
set_property(TARGET xcompat-fixture-forward PROPERTY MSVC_RUNTIME_CHECKS "")
target_compile_options(xcompat-fixture-forward PRIVATE /GS-)
target_link_options(xcompat-fixture-forward PRIVATE /NOENTRY /NODEFAULTLIB /DYNAMICBASE /FIXED:NO /BASE:0x0000000180300000)

add_library(xcompat-fixture-forward-cycle-a SHARED fixtures/forward_cycle_a.c)
configure_fixture(xcompat-fixture-forward-cycle-a forward_cycle_a)
set_property(TARGET xcompat-fixture-forward-cycle-a PROPERTY MSVC_RUNTIME_CHECKS "")
target_compile_options(xcompat-fixture-forward-cycle-a PRIVATE /GS-)
target_link_options(xcompat-fixture-forward-cycle-a PRIVATE /NOENTRY /NODEFAULTLIB /DYNAMICBASE /FIXED:NO /BASE:0x0000000180400000)

add_library(xcompat-fixture-forward-cycle-b SHARED fixtures/forward_cycle_b.c)
configure_fixture(xcompat-fixture-forward-cycle-b forward_cycle_b)
set_property(TARGET xcompat-fixture-forward-cycle-b PROPERTY MSVC_RUNTIME_CHECKS "")
target_compile_options(xcompat-fixture-forward-cycle-b PRIVATE /GS-)
target_link_options(xcompat-fixture-forward-cycle-b PRIVATE /NOENTRY /NODEFAULTLIB /DYNAMICBASE /FIXED:NO /BASE:0x0000000180500000)

add_library(xcompat-fixture-leaf SHARED fixtures/provider.c fixtures/leaf.def)
configure_fixture(xcompat-fixture-leaf leaf)
configure_no_crt(xcompat-fixture-leaf DllMain)
target_link_options(xcompat-fixture-leaf PRIVATE /BASE:0x0000000180600000)

add_library(xcompat-fixture-missing-export-forward SHARED fixtures/missing_export_forward.c)
configure_fixture(xcompat-fixture-missing-export-forward missing_export_forward)
set_property(TARGET xcompat-fixture-missing-export-forward PROPERTY MSVC_RUNTIME_CHECKS "")
target_compile_options(xcompat-fixture-missing-export-forward PRIVATE /GS-)
target_link_options(xcompat-fixture-missing-export-forward PRIVATE /NOENTRY /NODEFAULTLIB /DYNAMICBASE /FIXED:NO /BASE:0x0000000180700000)

add_library(xcompat-fixture-cxx SHARED fixtures/cxx_fixture.cpp fixtures/cxx_fixture.def)
configure_fixture(xcompat-fixture-cxx cxx_fixture)
set_target_properties(xcompat-fixture-cxx PROPERTIES
    MSVC_RUNTIME_LIBRARY "MultiThreaded"
    MSVC_RUNTIME_CHECKS ""
)
target_link_options(xcompat-fixture-cxx PRIVATE /BASE:0x0000000180800000 /INCREMENTAL:NO)

add_executable(xcompat-fixture-entry fixtures/entry_root.cpp)
configure_fixture(xcompat-fixture-entry entry_root)
configure_no_crt(xcompat-fixture-entry entry)
target_link_libraries(xcompat-fixture-entry PRIVATE phnt::phnt)

add_executable(xcompat-fixture-binding fixtures/binding_root.c)
configure_fixture(xcompat-fixture-binding binding_root)
configure_no_crt(xcompat-fixture-binding entry)
target_link_libraries(xcompat-fixture-binding PRIVATE
    xcompat-fixture-provider xcompat-fixture-forward xcompat-fixture-delay-provider delayimp kernel32
)
target_link_options(xcompat-fixture-binding PRIVATE /DELAYLOAD:delayprovider.dll)

foreach(name constructor tls exception)
    add_executable(xcompat-fixture-${name} "fixtures/${name}_root.c")
    configure_fixture(xcompat-fixture-${name} "${name}_root")
    configure_no_crt(xcompat-fixture-${name} entry)
    target_link_libraries(xcompat-fixture-${name} PRIVATE xcompat-fixture-cxx)
endforeach()

add_executable(xcompat-fixture-thread fixtures/thread_root.c)
configure_fixture(xcompat-fixture-thread thread_root)
configure_no_crt(xcompat-fixture-thread entry)
target_link_libraries(xcompat-fixture-thread PRIVATE xcompat-fixture-cxx kernel32)
target_link_options(xcompat-fixture-thread PRIVATE /STACK:1048576,65536)

add_executable(xcompat-fixture-stack fixtures/stack_root.c)
configure_fixture(xcompat-fixture-stack stack_root)
configure_no_crt(xcompat-fixture-stack entry)
target_link_libraries(xcompat-fixture-stack PRIVATE kernel32)
target_link_libraries(xcompat-fixture-stack PRIVATE phnt::phnt)
target_link_options(xcompat-fixture-stack PRIVATE /STACK:2097152,131072)

add_executable(xcompat-fixture-xmem fixtures/xmem_root.cpp)
configure_fixture(xcompat-fixture-xmem xmem_root)
configure_no_crt(xcompat-fixture-xmem entry)
target_link_libraries(xcompat-fixture-xmem PRIVATE xcompat-xmem)

add_executable(xcompat-fixture-kernel fixtures/kernel_root.cpp)
configure_fixture(xcompat-fixture-kernel kernel_root)
configure_no_crt(xcompat-fixture-kernel entry)
target_link_libraries(xcompat-fixture-kernel PRIVATE xcompat-kernel)

set(cycle_import_directory "${CMAKE_CURRENT_BINARY_DIR}/cycle-imports")
file(MAKE_DIRECTORY "${cycle_import_directory}")
add_custom_command(
    OUTPUT "${cycle_import_directory}/cycle_a.lib" "${cycle_import_directory}/cycle_b.lib"
    COMMAND "${CMAKE_AR}" /DEF:${CMAKE_CURRENT_SOURCE_DIR}/fixtures/cycle_a.def /OUT:${cycle_import_directory}/cycle_a.lib /MACHINE:X64
    COMMAND "${CMAKE_AR}" /DEF:${CMAKE_CURRENT_SOURCE_DIR}/fixtures/cycle_b.def /OUT:${cycle_import_directory}/cycle_b.lib /MACHINE:X64
    DEPENDS fixtures/cycle_a.def fixtures/cycle_b.def
    VERBATIM
)
add_custom_target(xcompat-cycle-imports DEPENDS
    "${cycle_import_directory}/cycle_a.lib" "${cycle_import_directory}/cycle_b.lib")

add_library(xcompat-fixture-recorder SHARED fixtures/recorder.c fixtures/recorder.def)
configure_fixture(xcompat-fixture-recorder recorder)
configure_no_crt(xcompat-fixture-recorder DllMain)
target_link_options(xcompat-fixture-recorder PRIVATE /BASE:0x0000000180900000)

add_library(xcompat-fixture-cycle-a SHARED fixtures/cycle_a.c fixtures/cycle_a.def)
configure_fixture(xcompat-fixture-cycle-a cycle_a)
configure_no_crt(xcompat-fixture-cycle-a DllMain)
add_dependencies(xcompat-fixture-cycle-a xcompat-cycle-imports)
target_link_options(xcompat-fixture-cycle-a PRIVATE /BASE:0x0000000180A00000)
target_link_libraries(xcompat-fixture-cycle-a PRIVATE
    "${cycle_import_directory}/cycle_b.lib" xcompat-fixture-recorder)

add_library(xcompat-fixture-cycle-b SHARED fixtures/cycle_b.c fixtures/cycle_b.def)
configure_fixture(xcompat-fixture-cycle-b cycle_b)
configure_no_crt(xcompat-fixture-cycle-b DllMain)
add_dependencies(xcompat-fixture-cycle-b xcompat-cycle-imports)
target_link_options(xcompat-fixture-cycle-b PRIVATE /BASE:0x0000000180B00000)
target_link_libraries(xcompat-fixture-cycle-b PRIVATE
    "${cycle_import_directory}/cycle_a.lib" xcompat-fixture-recorder)

add_executable(xcompat-fixture-cycle-root fixtures/cycle_root.c)
configure_fixture(xcompat-fixture-cycle-root cycle_root)
configure_no_crt(xcompat-fixture-cycle-root entry)
target_link_libraries(xcompat-fixture-cycle-root PRIVATE
    "${cycle_import_directory}/cycle_a.lib" xcompat-fixture-recorder)
add_dependencies(xcompat-fixture-cycle-root xcompat-cycle-imports)

add_library(xcompat-fixture-absent-provider SHARED fixtures/provider.c fixtures/absent_provider.def)
set_target_properties(xcompat-fixture-absent-provider PROPERTIES
    OUTPUT_NAME absent_provider
    RUNTIME_OUTPUT_DIRECTORY "${XCOMPAT_FIXTURE_DIRECTORY}/unsearched"
    LIBRARY_OUTPUT_DIRECTORY "${XCOMPAT_FIXTURE_DIRECTORY}/unsearched"
    ARCHIVE_OUTPUT_DIRECTORY "${XCOMPAT_FIXTURE_DIRECTORY}/unsearched"
)
configure_no_crt(xcompat-fixture-absent-provider DllMain)

add_executable(xcompat-fixture-missing-provider fixtures/missing_provider_root.c)
configure_fixture(xcompat-fixture-missing-provider missing_provider_root)
configure_no_crt(xcompat-fixture-missing-provider entry)
target_link_libraries(xcompat-fixture-missing-provider PRIVATE xcompat-fixture-absent-provider)

add_executable(xcompat-fixture-forward-cycle-root fixtures/forward_cycle_root.c)
configure_fixture(xcompat-fixture-forward-cycle-root forward_cycle_root)
configure_no_crt(xcompat-fixture-forward-cycle-root entry)
target_link_libraries(xcompat-fixture-forward-cycle-root PRIVATE xcompat-fixture-forward-cycle-a)

add_executable(xcompat-fixture-missing-export-root fixtures/missing_export_root.c)
configure_fixture(xcompat-fixture-missing-export-root missing_export_root)
configure_no_crt(xcompat-fixture-missing-export-root entry)
target_link_libraries(xcompat-fixture-missing-export-root PRIVATE xcompat-fixture-missing-export-forward)

add_executable(xcompat-guest-tests guest_tests.cpp)
set_target_properties(xcompat-guest-tests PROPERTIES
    CXX_STANDARD 23
    CXX_STANDARD_REQUIRED YES
    CXX_EXTENSIONS NO
    RUNTIME_OUTPUT_DIRECTORY "${XCOMPAT_RUNTIME_DIRECTORY}"
)
target_compile_definitions(xcompat-guest-tests PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
target_include_directories(xcompat-guest-tests PRIVATE "${PROJECT_SOURCE_DIR}/project/xcompat/src")
target_link_libraries(xcompat-guest-tests PRIVATE xcompat)
add_dependencies(xcompat-guest-tests
    xcompat-fixture-entry xcompat-fixture-binding xcompat-fixture-constructor
    xcompat-fixture-tls xcompat-fixture-exception xcompat-fixture-thread
    xcompat-fixture-stack xcompat-fixture-xmem xcompat-fixture-kernel
    xcompat-fixture-cycle-root xcompat-fixture-missing-provider
    xcompat-fixture-cycle-a xcompat-fixture-cycle-b
    xcompat-fixture-forward-cycle-root xcompat-fixture-missing-export-root
    xcompat-fixture-forward-cycle-b xcompat-fixture-leaf
    xcompat-kernel xcompat-xmem
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

add_xcompat_case(loader.parser parser "$<TARGET_FILE:xcompat-fixture-entry>" loader)
add_xcompat_case(loader.state state "$<TARGET_FILE:xcompat-fixture-entry>" loader)
add_xcompat_case(loader.binding binding "$<TARGET_FILE:xcompat-fixture-binding>" loader)
add_xcompat_case(loader.unresolved unresolved "${XCOMPAT_FIXTURE_DIRECTORY}" loader)
add_xcompat_case(loader.cycle cycle "$<TARGET_FILE:xcompat-fixture-cycle-root>" loader)
add_xcompat_case(guest.entry entry "$<TARGET_FILE:xcompat-fixture-entry>" guest)
add_xcompat_case(guest.import import "$<TARGET_FILE:xcompat-fixture-binding>" guest)
add_xcompat_case(guest.constructor constructor "$<TARGET_FILE:xcompat-fixture-constructor>" guest)
add_xcompat_case(guest.tls tls "$<TARGET_FILE:xcompat-fixture-tls>" guest)
add_xcompat_case(guest.exception exception "$<TARGET_FILE:xcompat-fixture-exception>" guest)
add_xcompat_case(guest.thread thread "$<TARGET_FILE:xcompat-fixture-thread>" guest)
add_xcompat_case(guest.stack stack "$<TARGET_FILE:xcompat-fixture-stack>" guest)
add_xcompat_case(guest.xmem xmem "$<TARGET_FILE:xcompat-fixture-xmem>" guest)
add_xcompat_case(guest.kernel-mmu kernel-mmu "$<TARGET_FILE:xcompat-fixture-kernel>" guest)

set(entry_arguments "arg with spaces" "quote\"inside" "trailing\\")
set_tests_properties(xcompat.guest.entry PROPERTIES
    ENVIRONMENT "XCOMPAT_GUEST_TEST=present"
)
add_test(NAME xcompat.cli.entry COMMAND $<TARGET_FILE:xcompat-cli>
    $<TARGET_FILE:xcompat-fixture-entry> ${entry_arguments})
set_tests_properties(xcompat.cli.entry PROPERTIES
    LABELS guest
    TIMEOUT 30
    ENVIRONMENT "XCOMPAT_GUEST_TEST=present"
)
