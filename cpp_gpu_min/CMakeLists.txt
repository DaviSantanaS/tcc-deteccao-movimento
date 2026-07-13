cmake_minimum_required(VERSION 3.16)
project(minimal_motion_cuda CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(OpenCV 4.5 REQUIRED)

add_executable(minimal_motion src/main.cpp)
target_link_libraries(minimal_motion PRIVATE ${OpenCV_LIBS})
