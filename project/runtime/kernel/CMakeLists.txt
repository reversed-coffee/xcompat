file(
    GLOB_RECURSE XCOMPAT_SOURCES
    CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp"
)

add_library(xcompat-kernel SHARED ${XCOMPAT_SOURCES})

set_target_properties(xcompat-kernel
    PROPERTIES
        OUTPUT_NAME "xcptkrn"
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>"
        CXX_STANDARD 23
        CXX_STANDARD_REQUIRED YES
        CXX_EXTENSIONS NO
)

target_compile_definitions(xcompat-kernel
    PRIVATE
        NOMINMAX
        WIN32_LEAN_AND_MEAN
        XCOMPAT_KRN
)

target_include_directories(xcompat-kernel
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

target_link_libraries(xcompat-kernel
    Substrant.Hydra
)
