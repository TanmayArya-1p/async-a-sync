cmake_minimum_required(VERSION 3.20)
project(FilAsyncPlugin)

find_package(LLVM REQUIRED CONFIG NO_MODULE)
message(STATUS "Found LLVM ${LLVM_PACKAGE_VERSION} in ${LLVM_DIR}")

add_library(FilAsync MODULE ../pass/FilAsync.cpp)
# FilAsync.h is included as llvm/Transforms/Instrumentation/FilAsync.h. Map
# that path onto pass/, ahead of any copy compiler/build.sh installed into the
# LLVM tree.
file(MAKE_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/include/llvm/Transforms/Instrumentation)
file(CREATE_LINK ${CMAKE_CURRENT_SOURCE_DIR}/../pass/FilAsync.h
  ${CMAKE_CURRENT_BINARY_DIR}/include/llvm/Transforms/Instrumentation/FilAsync.h
  SYMBOLIC)
target_include_directories(FilAsync BEFORE PRIVATE
  ${CMAKE_CURRENT_BINARY_DIR}/include
  )
target_include_directories(FilAsync PRIVATE ${LLVM_INCLUDE_DIRS})
target_compile_features(FilAsync PRIVATE cxx_std_17)
# The LLVM 20 tree this is loaded into was built with -fno-rtti -fno-exceptions;
# match it for ABI parity.
target_compile_options(FilAsync PRIVATE
  -fno-rtti
  -fno-exceptions
  )
# Do NOT link the (static) LLVM libraries into the plugin: it is loaded into
# `opt`, which statically links and exports the LLVM symbols it needs, so its
# LLVM references are left undefined here and resolved at load time.
target_link_options(FilAsync PRIVATE -Wl,--unresolved-symbols=ignore-all)