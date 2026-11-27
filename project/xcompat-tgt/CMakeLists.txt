add_executable(xcompat-tgt main.cpp)

set_target_properties(xcompat-tgt
    PROPERTIES
        CXX_STANDARD 23
        CXX_STANDARD_REQUIRED YES
        CXX_EXTENSIONS NO
)

target_compile_definitions(xcompat-tgt
    PRIVATE
        NOMINMAX
        WIN32_LEAN_AND_MEAN
)
