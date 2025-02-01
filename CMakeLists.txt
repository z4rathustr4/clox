cmake_minimum_required(VERSION 3.10)

# Project name and version
project(clox VERSION 1.0 LANGUAGES C)

# Set compilation flags
set(CMAKE_C_STANDARD 99)
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wall -Wextra -Werror -Wno-unused-parameter -Wno-unused-function")

# Define source files
file(GLOB SRC_FILES src/*.c)

# Include directories
include_directories(include)

# Create the executable
add_executable(clox ${SRC_FILES})
