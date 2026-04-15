cmake_minimum_required(VERSION 3.22 FATAL_ERROR)

project(
    messenger_server
    VERSION 2.0.0
    DESCRIPTION "Premium Telegram-like Messenger Backend (C++20)"
    LANGUAGES CXX
)

# C++ Standard Configuration
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wall -Wextra -Wpedantic -O3 -march=native")

# Output directories
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)

# ============================================================================
# DEPENDENCIES
# ============================================================================

# Find required packages
find_package(PkgConfig REQUIRED)

# Threads
find_package(Threads REQUIRED)

# libsodium (E2EE encryption)
pkg_check_modules(LIBSODIUM REQUIRED libsodium)
set(SODIUM_LIBRARIES ${LIBSODIUM_LIBRARIES})
set(SODIUM_INCLUDE_DIRS ${LIBSODIUM_INCLUDE_DIRS})

# nlohmann_json (JSON parsing)
find_package(nlohmann_json 3.10 REQUIRED)

# Redis (libhiredis)
pkg_check_modules(HIREDIS hiredis)
if(NOT HIREDIS_FOUND)
    message(WARNING "hiredis not found via pkg-config, will try find_package")
    find_package(hiredis QUIET)
endif()

# OpenSSL (for HTTPS/TLS support)
find_package(OpenSSL QUIET)

# CPR (HTTP Client Library - optional)
find_package(CPR QUIET)

# Drogon Framework (optional - for advanced WebSocket handling)
find_package(Drogon QUIET)

# ============================================================================
# HEADER FILES
# ============================================================================

set(HEADER_FILES
    include/redis_client.hpp
    include/e2e_encryption.hpp
    include/websocket_connection.hpp
    include/messenger_server.hpp
)

# ============================================================================
# SOURCE FILES
# ============================================================================

set(SOURCE_FILES
    src/redis_client.cpp
    src/e2e_encryption.cpp
    src/websocket_connection.cpp
    src/messenger_server.cpp
    src/main.cpp
)

# ============================================================================
# MAIN TARGET: messenger_server executable
# ============================================================================

add_executable(messenger_server ${SOURCE_FILES} ${HEADER_FILES})

# Include directories
target_include_directories(messenger_server PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${SODIUM_INCLUDE_DIRS}
)

# Link libraries
target_link_libraries(messenger_server PRIVATE
    Threads::Threads
    nlohmann_json::nlohmann_json
    ${SODIUM_LIBRARIES}
)

# Link Redis if available
if(HIREDIS_FOUND)
    target_link_libraries(messenger_server PRIVATE ${HIREDIS_LIBRARIES})
    target_include_directories(messenger_server PRIVATE ${HIREDIS_INCLUDE_DIRS})
    target_compile_definitions(messenger_server PRIVATE HAVE_HIREDIS=1)
else()
    message(WARNING "hiredis not found - will use simplified Redis client")
endif()

# Link OpenSSL if available
if(OPENSSL_FOUND)
    target_link_libraries(messenger_server PRIVATE OpenSSL::SSL OpenSSL::Crypto)
    target_compile_definitions(messenger_server PRIVATE HAVE_OPENSSL=1)
endif()

# Link CPR if available
if(CPR_FOUND)
    target_link_libraries(messenger_server PRIVATE CPR::CPR)
    target_compile_definitions(messenger_server PRIVATE HAVE_CPR=1)
endif()

# Link Drogon if available
if(Drogon_FOUND)
    target_link_libraries(messenger_server PRIVATE Drogon::Drogon)
    target_compile_definitions(messenger_server PRIVATE HAVE_DROGON=1)
else()
    message(STATUS "Drogon not found - using custom WebSocket implementation")
endif()

# Set default C++ flags if not already set
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

message(STATUS "Build type: ${CMAKE_BUILD_TYPE}")
message(STATUS "C++ Standard: ${CMAKE_CXX_STANDARD}")
message(STATUS "Compiler: ${CMAKE_CXX_COMPILER}")

# ============================================================================
# TESTING (Optional)
# ============================================================================

enable_testing()
find_package(GTest QUIET)

if(GTEST_FOUND)
    message(STATUS "GoogleTest found - unit tests enabled")
    
    file(GLOB TEST_SOURCES "tests/*.cpp")
    add_executable(messenger-tests ${TEST_SOURCES})
    target_link_libraries(messenger-tests PRIVATE
        GTest::GTest
        GTest::Main
        nlohmann_json::nlohmann_json
        ${SODIUM_LIBRARIES}
    )
    target_include_directories(messenger-tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${SODIUM_INCLUDE_DIRS}
    )
    add_test(NAME MessengerTests COMMAND messenger-tests)
else()
    message(STATUS "GoogleTest not found - tests skipped")
endif()

# ============================================================================
# INSTALLATION
# ============================================================================

install(TARGETS messenger_server
    RUNTIME DESTINATION bin
    LIBRARY DESTINATION lib
    ARCHIVE DESTINATION lib
)

install(DIRECTORY include/ DESTINATION include/messenger)

# ============================================================================
# DOCUMENTATION
# ============================================================================

# Doxygen (optional)
find_package(Doxygen)
if(DOXYGEN_FOUND)
    set(DOXYGEN_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/docs)
    set(DOXYGEN_INPUT_FILTER "")
    
    add_custom_target(docs
        COMMAND ${DOXYGEN_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/Doxyfile
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
        COMMENT "Generating API documentation with Doxygen"
    )
endif()

# ============================================================================
# SUMMARY
# ============================================================================

message(STATUS "")
message(STATUS "============================================")
message(STATUS "  Messenger Server Build Configuration")
message(STATUS "============================================")
message(STATUS "Project: ${PROJECT_NAME} v${PROJECT_VERSION}")
message(STATUS "Source dir: ${CMAKE_CURRENT_SOURCE_DIR}")
message(STATUS "Build dir: ${CMAKE_BINARY_DIR}")
message(STATUS "")
message(STATUS "Dependencies:")
message(STATUS "  - libsodium: ${LIBSODIUM_FOUND}")
message(STATUS "  - nlohmann_json: YES")
message(STATUS "  - hiredis: ${HIREDIS_FOUND}")
message(STATUS "  - OpenSSL: ${OPENSSL_FOUND}")
message(STATUS "  - CPR: ${CPR_FOUND}")
message(STATUS "  - Drogon: ${Drogon_FOUND}")
message(STATUS "  - GoogleTest: ${GTEST_FOUND}")
message(STATUS "")
message(STATUS "Targets:")
message(STATUS "  - messenger_server: ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/messenger_server")
if(GTEST_FOUND)
    message(STATUS "  - messenger-tests: ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/messenger-tests")
endif()
message(STATUS "")
message(STATUS "To build:")
message(STATUS "  mkdir build && cd build")
message(STATUS "  cmake ..")
message(STATUS "  make -j$(nproc)")
message(STATUS "  ./bin/messenger_server")
message(STATUS "============================================")
message(STATUS "")
