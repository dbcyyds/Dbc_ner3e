cmake_minimum_required(VERSION 3.22)
project(nr3e-task LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

add_library(nr3e SHARED
  src/main.cpp src/ue.cpp src/task.cpp src/ipc.cpp)
target_include_directories(nr3e PRIVATE include)
target_compile_options(nr3e PRIVATE -fvisibility=hidden -O2 -w -D_GNU_SOURCE)
target_link_libraries(nr3e PRIVATE log dl m)
set_target_properties(nr3e PROPERTIES
  OUTPUT_NAME nr3e
  LIBRARY_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/outputs/arm64-v8a)
