cmake_minimum_required(VERSION 3.20)
project(clipjournal CXX RC)

set(VERSION 0.0.0 CACHE STRING "Version stamped into the exe (X.Y.Z); CI sets it from the git tag")
string(REPLACE "." "," VERSION_COMMA ${VERSION})
configure_file(clipjournal.rc.in clipjournal.rc @ONLY)

set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded")  # static CRT: one exe, no VC++ redistributable
add_executable(clipjournal WIN32 clipjournal.cpp ${CMAKE_CURRENT_BINARY_DIR}/clipjournal.rc)
target_compile_features(clipjournal PRIVATE cxx_std_20)
target_compile_options(clipjournal PRIVATE $<$<COMPILE_LANGUAGE:CXX>:/utf-8 /W4>)
