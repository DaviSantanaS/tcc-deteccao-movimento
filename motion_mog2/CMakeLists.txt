cmake_minimum_required(VERSION 3.14)
project(motion_mog2 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(OpenCV 4.5 REQUIRED)

add_executable(
    motion_mog2
    src/main.cpp
    src/VideoStreamReader.cpp
    src/Mog2MotionDetector.cpp
    src/EncodedVideoBuffer.cpp
)

target_include_directories(
    motion_mog2
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(motion_mog2 PRIVATE ${OpenCV_LIBS})
