cmake_minimum_required(VERSION 3.20)
project(clipjournal CXX)

set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded")  # static CRT: one exe, no VC++ redistributable
add_executable(clipjournal WIN32 clipjournal.cpp)
target_compile_features(clipjournal PRIVATE cxx_std_20)
target_compile_options(clipjournal PRIVATE /utf-8 /W4)
