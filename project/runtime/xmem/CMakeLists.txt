file(
    GLOB_RECURSE XCOMPAT_SOURCES
    CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp"
)

add_library(xcompat-xmem SHARED ${XCOMPAT_SOURCES})

set_target_properties(xcompat-xmem
    PROPERTIES
        OUTPUT_NAME "xmem"
        CXX_STANDARD 23
        CXX_STANDARD_REQUIRED YES
        CXX_EXTENSIONS NO
)

target_compile_definitions(xcompat-xmem
    PRIVATE
        NOMINMAX
        WIN32_LEAN_AND_MEAN
        XMEM_LIBRARY
)

target_include_directories(xcompat-xmem
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

target_link_libraries(xcompat-xmem)
