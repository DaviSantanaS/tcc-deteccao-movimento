cmake_minimum_required(VERSION 3.14)
project(MotionGPU_NoDisplay LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(OpenCV 4.5 REQUIRED)

add_executable(MotionGPU_NoDisplay src/main.cpp)
target_link_libraries(MotionGPU_NoDisplay PRIVATE ${OpenCV_LIBS})
