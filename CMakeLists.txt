cmake_minimum_required(VERSION 3.20)
project(mbp-soundloop C)

set(CMAKE_C_STANDARD 11)

find_package(PkgConfig REQUIRED)
pkg_check_modules(PORTAUDIO REQUIRED portaudio-2.0)

add_executable(mbp-soundloop main.c audio.c power.c)

target_compile_options(mbp-soundloop PRIVATE -O2 -Wall -Wextra ${PORTAUDIO_CFLAGS_OTHER})
target_include_directories(mbp-soundloop PRIVATE ${PORTAUDIO_INCLUDE_DIRS})
target_link_libraries(mbp-soundloop
    ${PORTAUDIO_LIBRARIES}
    "-framework IOKit"
    "-framework CoreFoundation"
    m
)
