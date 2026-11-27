file(
    GLOB_RECURSE XCOMPAT_SOURCES
    CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp"
)

add_executable(xcompat-cli ${XCOMPAT_SOURCES})

set_target_properties(xcompat-cli
    PROPERTIES
        CXX_STANDARD 23
        CXX_STANDARD_REQUIRED YES
        CXX_EXTENSIONS NO
)

target_compile_definitions(xcompat-cli
    PRIVATE
        NOMINMAX
        WIN32_LEAN_AND_MEAN
)

target_include_directories(xcompat-cli
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

target_link_libraries(xcompat-cli
    xcompat
)
