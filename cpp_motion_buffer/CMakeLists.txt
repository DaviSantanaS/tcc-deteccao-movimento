cmake_minimum_required(VERSION 3.14)
project(Motion_Buffer LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(OpenCV 4.5 REQUIRED)

# Detector (já existente)
add_executable(motion_buffer_detector src/main.cpp)
target_link_libraries(motion_buffer_detector PRIVATE ${OpenCV_LIBS})

# Segmentador (wrapper C++ que chama FFmpeg)
add_executable(segment_recorder src/segment_recorder.cpp)

# Clipper (C++ que lê eventos e concatena com FFmpeg)
add_executable(motion_buffer_clipper src/motion_buffer_clipper.cpp)
