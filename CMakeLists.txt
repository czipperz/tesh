cmake_minimum_required(VERSION 3.3)

project(tesh)

# Force c++11.
set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(PROGRAM_NAME ${PROJECT_NAME})
set(LIBRARY_NAME ${PROJECT_NAME}-lib)
set(TEST_PROGRAM_NAME ${PROJECT_NAME}-test)

set(CMAKE_MODULE_PATH ${CMAKE_SOURCE_DIR}/cmake ${CMAKE_MODULE_PATH})
include(cmake/SetupSDL2.cmake)

# Build wrapper executable.
if(CMAKE_BUILD_TYPE MATCHES Debug)
    add_executable(${PROGRAM_NAME} main.cpp)
    target_compile_definitions(${PROGRAM_NAME} PUBLIC CONSOLE_MAIN)
else()
    add_executable(${PROGRAM_NAME} WIN32 main.cpp)
endif()
target_link_libraries(${PROGRAM_NAME} ${LIBRARY_NAME} cz tracy)

# Add test program.
if (TESH_BUILD_TESTS)
    file(GLOB_RECURSE TEST_SRCS tests/*.cpp)
    add_executable(${TEST_PROGRAM_NAME} ${TEST_SRCS})
    target_include_directories(${TEST_PROGRAM_NAME} PUBLIC src cz/test_base)
    target_link_libraries(${TEST_PROGRAM_NAME} ${LIBRARY_NAME} cz tracy)
    target_link_libraries(${TEST_PROGRAM_NAME} czt)
endif()

# Build library with all actual code.
file(GLOB_RECURSE SRCS src/*.cpp)
add_library(${LIBRARY_NAME} ${SRCS})

# Run GNU Global if it is available.
if (WIN32)
    add_custom_target(update_global
                      COMMAND ${CMAKE_SOURCE_DIR}/update-global.bat
                      WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
else()
    add_custom_target(update_global
                      COMMAND ${CMAKE_SOURCE_DIR}/update-global.sh
                      WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
endif()
add_dependencies(${PROJECT_NAME} update_global)

# Setup cz.
# TRACY_INCLUDE_DIRS allows cz to include the Tracy headers.
set(TRACY_INCLUDE_DIRS ${CMAKE_SOURCE_DIR}/tracy)
add_subdirectory(cz)
include_directories(cz/include)

# Setup Tracy.
add_library(tracy tracy/TracyClient.cpp)
include_directories(tracy)
if (TRACY_ENABLE)
    add_definitions(-DTRACY_ENABLE)

    if (NOT WIN32)
        target_link_libraries(${PROJECT_NAME} pthread dl z)
    endif()
endif()

# Enable High DPI
if (WIN32)
    target_link_libraries(${PROGRAM_NAME} Shcore.lib)
    if (TESH_BUILD_TESTS)
        target_link_libraries(${TEST_PROGRAM_NAME} Shcore.lib)
    endif()
endif()

# Link in Pseudo TTY library.
if (NOT WIN32)
    target_link_libraries(${PROGRAM_NAME} util)
    if (TESH_BUILD_TESTS)
        target_link_libraries(${TEST_PROGRAM_NAME} util)
    endif()
endif()

# Setup logo
if (WIN32)
    # Windows_RC.h does some Windows magic to set the app icon.
    target_sources(${PROGRAM_NAME} PUBLIC ${CMAKE_SOURCE_DIR}/res/Windows_RC.rc)

    # Build the icon from the png.
    add_custom_target(icon DEPENDS ${CMAKE_SOURCE_DIR}/res/logo.ico)
    add_custom_command(OUTPUT ${CMAKE_SOURCE_DIR}/res/logo.ico
                       COMMAND magick convert ${CMAKE_SOURCE_DIR}/res/logo.png -define icon:auto-resize=256,128,96,64,48,32,16 ${CMAKE_SOURCE_DIR}/res/logo.ico
                       DEPENDS ${CMAKE_SOURCE_DIR}/res/logo.png)
else()
    # Copy the icon to the output directory.
    add_custom_target(icon DEPENDS ${CMAKE_BINARY_DIR}/logo.png)
    add_custom_command(OUTPUT ${CMAKE_BINARY_DIR}/logo.png
                       COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CMAKE_SOURCE_DIR}/res/logo.png ${CMAKE_BINARY_DIR}
                       DEPENDS ${CMAKE_SOURCE_DIR}/res/logo.png)
endif()
add_dependencies(${PROGRAM_NAME} icon)

# Warnings.
if(MSVC)
    add_definitions(-D_CRT_SECURE_CPP_OVERLOAD_STANDARD_NAMES=1)
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} /utf-8 /wd4100 /wd26495 /wd26812")
elseif(CMAKE_COMPILER_IS_GNUCC OR CMAKE_COMPILER_IS_GNUCXX)
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wall -Wextra -Wpedantic -Werror=switch -Wno-unused-parameter -Wno-sign-compare")
endif()

# Define NDEBUG for release builds.
if(CMAKE_BUILD_TYPE STREQUAL "Release")
  add_definitions(-DNDEBUG)
endif()
