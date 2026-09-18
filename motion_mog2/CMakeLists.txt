cmake_minimum_required(VERSION 3.14)
project(motion_mog2 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(OpenCV 4.5 REQUIRED)
find_package(PkgConfig REQUIRED)

pkg_check_modules(
    FFMPEG
    REQUIRED
    IMPORTED_TARGET
    libavformat
    libavcodec
    libavutil
)

add_executable(
    motion_mog2
    src/main.cpp
    src/FfmpegRawVideoSource.cpp
    src/VideoStreamReader.cpp
    src/Mog2MotionDetector.cpp
    src/EncodedVideoBuffer.cpp
)

target_include_directories(
    motion_mog2
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(
    motion_mog2
    PRIVATE
    ${OpenCV_LIBS}
    PkgConfig::FFMPEG
)

include(CTest)

if(BUILD_TESTING)
    add_executable(
        av_packet_tests
        tests/av_packet_tests.cpp
        src/EncodedVideoBuffer.cpp
    )

    target_include_directories(
        av_packet_tests
        PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include
    )

    target_link_libraries(
        av_packet_tests
        PRIVATE
        PkgConfig::FFMPEG
    )

    add_test(NAME av_packet_tests COMMAND av_packet_tests)
endif()
