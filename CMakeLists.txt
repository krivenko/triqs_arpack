# Start configuration
cmake_minimum_required(VERSION 2.8.7)

# Version number
project(triqs_arpack CXX)

set(TRIQS_ARPACK_VERSION "0.5")
message(STATUS "Configuring triqs_arpack version ${TRIQS_ARPACK_VERSION}")

# Append triqs installed files to the cmake load path
list(APPEND CMAKE_MODULE_PATH ${CMAKE_SOURCE_DIR}/cmake ${TRIQS_PATH}/share/triqs/cmake)

# Load TRIQS, including all predefined variables from TRIQS installation
find_package(TRIQS REQUIRED)

if(NOT TRIQS_VERSION EQUAL 1.5)
    message(FATAL_ERROR "This extension requires the TRIQS library version 1.5 (got ${TRIQS_VERSION})")
endif()

# Get hash
triqs_get_git_hash(${CMAKE_SOURCE_DIR} "TRIQS_ARPACK")
if(${GIT_RESULT} EQUAL 0)
    message(STATUS "Hash: ${TRIQS_ARPACK_GIT_HASH}")
endif(${GIT_RESULT} EQUAL 0)

# We want to be installed in the TRIQS tree
set(CMAKE_INSTALL_PREFIX ${TRIQS_PATH})

# Install FindARPACK.cmake
install(FILES cmake/FindARPACK.cmake DESTINATION share/triqs/cmake)

option(Tests "Enable Tests" ON)

# Install C++ headers
add_subdirectory(c++)

if (${Tests})
    enable_testing()
    add_subdirectory(test)
endif()