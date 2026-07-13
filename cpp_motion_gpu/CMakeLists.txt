cmake_minimum_required(VERSION 3.14)
project(motion_gpu LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(OpenCV 4.5 REQUIRED)

add_executable(motion_gpu src/main.cpp)
target_link_libraries(motion_gpu PRIVATE ${OpenCV_LIBS})
