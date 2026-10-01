cmake_minimum_required(VERSION 3.25)
project(randy-vk LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded")   # static CRT: no extra runtime DLL next to the game

# Drop-in randy31.dll. Exports not implemented here forward to the original, renamed randy31_orig.dll.
add_library(randy31 SHARED proxy/dllmain.cpp proxy/trace.cpp proxy/randy31.def)
set_target_properties(randy31 PROPERTIES OUTPUT_NAME randy31 PREFIX "")

# Wine-run check that every proxy export resolves to the original's address.
add_executable(forward_check tests/forward_check.cpp)
