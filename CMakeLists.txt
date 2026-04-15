cmake_minimum_required(VERSION 3.22 FATAL_ERROR)
project(messenger_server VERSION 3.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wall -Wextra -O2")
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

find_package(PkgConfig REQUIRED)
find_package(Threads   REQUIRED)

pkg_check_modules(LIBSODIUM REQUIRED libsodium)
find_package(nlohmann_json 3.10 REQUIRED)
pkg_check_modules(HIREDIS hiredis)
find_package(OpenSSL QUIET)

# libpq
pkg_check_modules(LIBPQ libpq)
if(NOT LIBPQ_FOUND)
    find_library(LIBPQ_LIBRARIES NAMES pq)
    find_path(LIBPQ_INCLUDE_DIRS NAMES libpq-fe.h
              PATHS /usr/include/postgresql /usr/local/include/postgresql)
    if(LIBPQ_LIBRARIES)
        set(LIBPQ_FOUND TRUE)
    else()
        message(FATAL_ERROR "libpq not found. Run: apt-get install libpq-dev")
    endif()
endif()

# Crow (header-only, fetched automatically)
include(FetchContent)
FetchContent_Declare(crow
    GIT_REPOSITORY https://github.com/CrowCpp/Crow.git
    GIT_TAG        v1.2.0
)
FetchContent_MakeAvailable(crow)

# Binary
add_executable(messenger_server
    src/redis_client.cpp
    src/e2e_encryption.cpp
    src/websocket_connection.cpp
    src/messenger_server.cpp
    src/http_api.cpp
    src/main.cpp
)

target_include_directories(messenger_server PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${LIBSODIUM_INCLUDE_DIRS}
    ${LIBPQ_INCLUDE_DIRS}
)

target_link_libraries(messenger_server PRIVATE
    Threads::Threads
    nlohmann_json::nlohmann_json
    Crow::Crow
    ${LIBSODIUM_LIBRARIES}
    ${LIBPQ_LIBRARIES}
)

if(HIREDIS_FOUND)
    target_link_libraries(messenger_server PRIVATE ${HIREDIS_LIBRARIES})
    target_include_directories(messenger_server PRIVATE ${HIREDIS_INCLUDE_DIRS})
    target_compile_definitions(messenger_server PRIVATE HAVE_HIREDIS=1)
endif()

if(OPENSSL_FOUND)
    target_link_libraries(messenger_server PRIVATE OpenSSL::SSL OpenSSL::Crypto)
    target_compile_definitions(messenger_server PRIVATE HAVE_OPENSSL=1)
endif()

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

message(STATUS "[messenger] libpq=${LIBPQ_FOUND} hiredis=${HIREDIS_FOUND} openssl=${OPENSSL_FOUND}")
