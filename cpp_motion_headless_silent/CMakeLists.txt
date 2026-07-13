cmake_minimum_required(VERSION 3.14)
project(motion_headless_silent LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(OpenCV 4.5 REQUIRED)

add_executable(motion_headless_silent src/main.cpp)
target_link_libraries(motion_headless_silent PRIVATE ${OpenCV_LIBS})
