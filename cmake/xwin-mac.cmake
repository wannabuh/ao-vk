# Local toolchain on macOS: as xwin-linux.cmake (clang-cl + lld-link against `xwin splat`'s MSVC CRT and Windows SDK in
# ~/.xwin), with Homebrew's LLVM and lld, which aren't on the PATH. The macOS Vulkan SDK has no Vulkan Memory Allocator:
# VMA_INCLUDE_DIR names a directory with vma/vk_mem_alloc.h. docs/mac.md.
include("${CMAKE_CURRENT_LIST_DIR}/xwin-linux.cmake")

set(LLVM_BIN "/opt/homebrew/opt/llvm@22/bin" CACHE PATH "Homebrew LLVM: clang-cl, llvm-rc, llvm-mt, llvm-lib")
set(LLD_BIN "/opt/homebrew/opt/lld@22/bin" CACHE PATH "Homebrew lld: lld-link")
set(VMA_INCLUDE_DIR "" CACHE PATH "Directory holding vma/vk_mem_alloc.h")

set(CMAKE_C_COMPILER   "${LLVM_BIN}/clang-cl")
set(CMAKE_CXX_COMPILER "${LLVM_BIN}/clang-cl")
set(CMAKE_LINKER       "${LLD_BIN}/lld-link")
set(CMAKE_RC_COMPILER  "${LLVM_BIN}/llvm-rc")
set(CMAKE_MT           "${LLVM_BIN}/llvm-mt")
set(CMAKE_AR           "${LLVM_BIN}/llvm-lib")

if(VMA_INCLUDE_DIR)
    string(APPEND CMAKE_C_FLAGS_INIT   " /imsvc${VMA_INCLUDE_DIR}")
    string(APPEND CMAKE_CXX_FLAGS_INIT " /imsvc${VMA_INCLUDE_DIR}")
endif()
