cmake_minimum_required(VERSION 3.10)
project(llvmcov2html)

set(CMAKE_CXX_STANDARD 17)

set (LLVM_VERSION "12")
find_package(LLVM ${LLVM_VERSION} REQUIRED CONFIG)
include_directories(SYSTEM ${LLVM_INCLUDE_DIRS})
add_definitions(${LLVM_DEFINITIONS})

set(llvm_libs LLVM)

add_executable(llvmcov2html
   main.cpp)
target_link_libraries(llvmcov2html ${llvm_libs})
