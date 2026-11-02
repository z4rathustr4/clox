cmake_minimum_required(VERSION 3.10)

# avoid fucking cmake fucking up the ass my Makefile
get_filename_component(SRC_REAL "${CMAKE_SOURCE_DIR}" REALPATH)
get_filename_component(BIN_REAL "${CMAKE_BINARY_DIR}" REALPATH)
if(SRC_REAL STREQUAL BIN_REAL)
  message(FATAL_ERROR
		"CMake is retarded. Please use a separate directory for building\n"
		"To build with retarded CMake, please run:\n"
    "  cmake -S . -B build && cmake --build build\n"
		"thank you for your patience :)")
endif()

project(clox VERSION 1.0 LANGUAGES C)

# flags
set(CMAKE_C_STANDARD 17)
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wall -Wextra -Werror -Wno-unused-parameter -Wno-unused-function")

# source files
file(GLOB SRC_FILES src/*.c)

# headers
include_directories(include)

# executable
add_executable(clox ${SRC_FILES})
