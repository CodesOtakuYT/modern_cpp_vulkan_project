add_executable(source main.cpp)
target_compile_features(source PRIVATE cxx_std_23)
set_target_properties(source PROPERTIES CXX_EXTENSIONS off CXX_STANDARD_REQUIRED on)

if (MSVC)
    target_compile_options(source PRIVATE /W3 /sdl /external:anglebrackets /external:W2 /fsanitize=address /wd4068)
else ()
    target_compile_options(source PRIVATE -Wall -Wextra -Wpedantic -isystem)
endif ()