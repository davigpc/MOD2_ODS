cmake_minimum_required(VERSION 3.20)
project(s4_media_evidence
    VERSION 0.1.0
    LANGUAGES CXX
)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Compiler flags
if(MSVC)
    add_compile_options(/W4 /permissive-)
else()
    add_compile_options(-Wall -Wextra -Wpedantic)
endif()

# Include directories
include_directories(include)

# Core Library Sources
file(GLOB_RECURSE S4_SOURCES CONFIGURE_DEPENDS
    "src/*.cpp"
)

add_library(s4_media_evidence_lib
    ${S4_SOURCES}
)

target_include_directories(s4_media_evidence_lib PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)

# Third-party dependencies (Find or FetchContent can be configured as needed)
find_package(PkgConfig QUIET)
find_package(SQLite3 QUIET)
find_package(OpenSSL QUIET) # For SHA256

if(SQLite3_FOUND)
    target_link_libraries(s4_media_evidence_lib PRIVATE SQLite::SQLite3)
endif()

if(OpenSSL_FOUND)
    target_link_libraries(s4_media_evidence_lib PRIVATE OpenSSL::Crypto)
endif()

# Main executable
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/src/main.cpp")
    add_executable(s4_media_evidence src/main.cpp)
    target_link_libraries(s4_media_evidence PRIVATE s4_media_evidence_lib)
endif()

# Testing
enable_testing()

add_executable(test_s4_unit tests/unit/test_domain_and_usecase.cpp)
target_link_libraries(test_s4_unit PRIVATE s4_media_evidence_lib)
add_test(NAME test_s4_unit COMMAND test_s4_unit)

