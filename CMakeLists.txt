cmake_minimum_required(VERSION 3.20)
project(Project4Variant5 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Час вимірюємо лише в оптимізованій збірці.
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

find_package(Threads REQUIRED)

add_executable(generator src/generator.cpp)
add_executable(benchmark src/main.cpp)

foreach(target generator benchmark)
    target_link_libraries(${target} PRIVATE Threads::Threads)
    if(MINGW)
        target_link_options(${target} PRIVATE -static)
    endif()
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /utf-8)
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic)
    endif()
endforeach()
