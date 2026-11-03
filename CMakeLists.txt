cmake_minimum_required(VERSION 3.20)
project(NitpickHDR LANGUAGES CXX RC)

# Release builds pass -DNITPICK_VERSION=1.2.3 (taken from the git tag by CI)
set(NITPICK_VERSION "0.0.0-dev" CACHE STRING "Version string embedded in the exe")
if(NOT NITPICK_VERSION MATCHES "^([0-9]+)\\.([0-9]+)\\.([0-9]+)")
    message(FATAL_ERROR "NITPICK_VERSION must start with MAJOR.MINOR.PATCH, got '${NITPICK_VERSION}'")
endif()
set(NITPICK_VERSION_MAJOR ${CMAKE_MATCH_1})
set(NITPICK_VERSION_MINOR ${CMAKE_MATCH_2})
set(NITPICK_VERSION_PATCH ${CMAKE_MATCH_3})
configure_file(src/version.h.in "${CMAKE_CURRENT_BINARY_DIR}/generated/version.h" @ONLY)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

add_executable(NitpickHDR WIN32
    src/main.cpp
    src/hdr.cpp
    src/icons.cpp
    src/keyhook.cpp
    src/settings.cpp
    res/app.rc
)
target_include_directories(NitpickHDR PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated")
target_compile_definitions(NitpickHDR PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN
                           _WIN32_WINNT=0x0A00 WINVER=0x0A00)
target_link_libraries(NitpickHDR PRIVATE user32 gdi32 shell32 advapi32)

if(MSVC)
    target_compile_options(NitpickHDR PRIVATE $<$<COMPILE_LANGUAGE:CXX>:/W4 /utf-8>)
    # The manifest is embedded via app.rc
    target_link_options(NitpickHDR PRIVATE /MANIFEST:NO)
else()
    target_compile_options(NitpickHDR PRIVATE $<$<COMPILE_LANGUAGE:CXX>:-Wall -Wextra -Wno-missing-field-initializers>)
    target_link_options(NitpickHDR PRIVATE -municode -static)
endif()

install(TARGETS NitpickHDR RUNTIME DESTINATION .)
