cmake_minimum_required(VERSION 3.20)

project(ensemble_fabric_consumer LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# This project deliberately consumes Ensemble Fabric only through the installed
# package: no source tree path, no add_subdirectory, no vendored copy.
find_package(ensemble_fabric 1.0 REQUIRED CONFIG)

add_executable(ensemble_consumer main.cpp)
target_link_libraries(ensemble_consumer PRIVATE ensemble_fabric::ensemble_fabric)

if(MSVC)
  target_compile_options(ensemble_consumer PRIVATE /W4 /WX /permissive- /utf-8)
else()
  target_compile_options(ensemble_consumer PRIVATE -Wall -Wextra -Wpedantic -Werror)
endif()

enable_testing()
add_test(NAME ensemble_consumer COMMAND ensemble_consumer)
