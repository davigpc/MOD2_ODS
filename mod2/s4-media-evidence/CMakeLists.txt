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
include_directories(include third_party/httplib)

# Third-party dependencies
find_package(PkgConfig QUIET)
find_package(SQLite3 REQUIRED)
find_package(OpenSSL QUIET) # For SHA256
find_package(Threads REQUIRED)

find_library(UUID_LIBRARY NAMES uuid REQUIRED)

# O nome do alvo importado do SQLite difere entre versões do CMake
# (SQLite::SQLite3 nas mais antigas, SQLite3::SQLite3 nas mais novas).
if(TARGET SQLite3::SQLite3)
    set(SQLITE_TARGET SQLite3::SQLite3)
else()
    set(SQLITE_TARGET SQLite::SQLite3)
endif()

# Core Library Sources
file(GLOB_RECURSE S4_SOURCES CONFIGURE_DEPENDS
    "src/*.cpp"
)
list(REMOVE_ITEM S4_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/src/main.cpp")

add_library(s4_media_evidence_lib
    ${S4_SOURCES}
)

target_include_directories(s4_media_evidence_lib PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/third_party/httplib>
    $<INSTALL_INTERFACE:include>
)

target_link_libraries(s4_media_evidence_lib PRIVATE
    ${SQLITE_TARGET}
    ${UUID_LIBRARY}
    Threads::Threads
)

if(OpenSSL_FOUND)
    target_link_libraries(s4_media_evidence_lib PRIVATE OpenSSL::Crypto)
endif()

# --- S4.1 Ring Buffer -------------------------------------------------------
# shm_open/shm_unlink vivem em librt nas glibc antigas (JetPack incluso).
if(UNIX AND NOT APPLE)
    target_link_libraries(s4_media_evidence_lib PUBLIC rt)
endif()

# Captura ao vivo e opcional: sem GStreamer o componente compila e os testes
# rodam com as fontes sintetica e gravada.
option(ODS_S4_WITH_GSTREAMER "Compila o adaptador de captura GStreamer (S4.1)" ON)
if(ODS_S4_WITH_GSTREAMER AND PkgConfig_FOUND)
    pkg_check_modules(GST QUIET gstreamer-1.0 gstreamer-app-1.0)
    if(GST_FOUND)
        target_compile_definitions(s4_media_evidence_lib PUBLIC ODS_S4_WITH_GSTREAMER)
        target_include_directories(s4_media_evidence_lib PUBLIC ${GST_INCLUDE_DIRS})
        target_link_libraries(s4_media_evidence_lib PUBLIC ${GST_LIBRARIES})
        message(STATUS "GStreamer encontrado: captura ao vivo do S4.1 habilitada")
    else()
        message(STATUS "GStreamer ausente: S4.1 usa apenas fontes sintetica e gravada")
    endif()
endif()

# Main executable
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/src/main.cpp")
    add_executable(s4_media_evidence src/main.cpp)
    target_link_libraries(s4_media_evidence PRIVATE s4_media_evidence_lib)
endif()

# Testing
enable_testing()

function(s4_add_test name source)
    add_executable(${name} ${source})
    target_include_directories(${name} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(${name} PRIVATE s4_media_evidence_lib)
    add_test(NAME ${name} COMMAND ${name})
endfunction()

s4_add_test(test_s4_unit tests/unit/test_domain_and_usecase.cpp)
s4_add_test(test_s4_sha256 tests/unit/test_sha256_hasher.cpp)
s4_add_test(test_s4_sqlite_repository tests/integration/test_sqlite_repository.cpp)
s4_add_test(test_s4_extract_clip tests/integration/test_extract_clip.cpp)
s4_add_test(test_s4_http_server tests/integration/test_http_server.cpp)
s4_add_test(test_s4_http_post_event tests/integration/test_http_post_event.cpp)
s4_add_test(test_s4_ring_buffer tests/unit/test_ring_buffer.cpp)
s4_add_test(test_s4_ring_buffer_runtime tests/integration/test_ring_buffer_runtime.cpp)