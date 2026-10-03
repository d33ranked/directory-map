cmake_minimum_required(VERSION 3.15)
project(DirTracker)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Disable Raylib examples and games to speed up the build process
set(BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(BUILD_GAMES OFF CACHE BOOL "" FORCE)

# Automatically fetch and configure Raylib
include(FetchContent)
FetchContent_Declare(
    raylib
    GIT_REPOSITORY https://github.com/raysan5/raylib.git
    GIT_TAG        5.0 # You can change this to a specific version or "master"
)
FetchContent_MakeAvailable(raylib)

# Create the executable and link Raylib
add_executable(DirTracker main.cpp)
target_link_libraries(DirTracker PRIVATE raylib)