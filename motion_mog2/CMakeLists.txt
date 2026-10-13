cmake_minimum_required(VERSION 3.14)
project(motion_mog2 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(OpenCV 4.5 REQUIRED)
find_package(PkgConfig REQUIRED)

pkg_check_modules(LIBAVCODEC REQUIRED IMPORTED_TARGET libavcodec)
pkg_check_modules(LIBAVFORMAT REQUIRED IMPORTED_TARGET libavformat)
pkg_check_modules(LIBAVUTIL REQUIRED IMPORTED_TARGET libavutil)

add_executable(
        motion_mog2
        src/main.cpp
        src/VideoStreamReader.cpp
        src/Mog2MotionDetector.cpp
        src/EncodedVideoBuffer.cpp
        src/PreEventConfig.cpp
        src/AvPacket.cpp
        src/AvCodecParameters.cpp
        src/AvBitstreamFilter.cpp
        src/VideoStreamParameters.cpp
        src/FfmpegRawVideoSource.cpp
)

target_include_directories(
        motion_mog2
        PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(
        motion_mog2
        PRIVATE
        ${OpenCV_LIBS}
        PkgConfig::LIBAVCODEC
        PkgConfig::LIBAVFORMAT
        PkgConfig::LIBAVUTIL
)

include(CTest)

if(BUILD_TESTING)
    add_executable(
            encoded_video_buffer_tests
            tests/encoded_video_buffer_tests.cpp
            src/EncodedVideoBuffer.cpp
            src/AvPacket.cpp
            src/AvCodecParameters.cpp
            src/VideoStreamParameters.cpp
    )

    target_include_directories(
            encoded_video_buffer_tests
            PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include
    )

    target_link_libraries(
            encoded_video_buffer_tests
            PRIVATE PkgConfig::LIBAVCODEC
    )

    add_test(NAME encoded_video_buffer_tests COMMAND encoded_video_buffer_tests)

    add_executable(
            av_packet_tests
            tests/av_packet_tests.cpp
            src/AvPacket.cpp
    )

    target_include_directories(
            av_packet_tests
            PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include
    )

    target_link_libraries(
            av_packet_tests
            PRIVATE PkgConfig::LIBAVCODEC
    )

    add_test(NAME av_packet_tests COMMAND av_packet_tests)

    add_executable(
            av_codec_parameters_tests
            tests/av_codec_parameters_tests.cpp
            src/AvCodecParameters.cpp
    )

    target_include_directories(
            av_codec_parameters_tests
            PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include
    )

    target_link_libraries(
            av_codec_parameters_tests
            PRIVATE
            PkgConfig::LIBAVCODEC
            PkgConfig::LIBAVUTIL
    )

    add_test(NAME av_codec_parameters_tests COMMAND av_codec_parameters_tests)

    add_executable(
            video_stream_parameters_tests
            tests/video_stream_parameters_tests.cpp
            src/VideoStreamParameters.cpp
            src/AvCodecParameters.cpp
    )

    target_include_directories(
            video_stream_parameters_tests
            PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include
    )

    target_link_libraries(
            video_stream_parameters_tests
            PRIVATE PkgConfig::LIBAVCODEC
    )

    add_test(NAME video_stream_parameters_tests COMMAND video_stream_parameters_tests)

    add_executable(
            encoded_video_packets_tests
            tests/encoded_video_packets_tests.cpp
            src/AvPacket.cpp
            src/AvCodecParameters.cpp
            src/VideoStreamParameters.cpp
    )

    target_include_directories(
            encoded_video_packets_tests
            PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include
    )

    target_link_libraries(
            encoded_video_packets_tests
            PRIVATE PkgConfig::LIBAVCODEC
    )

    add_test(NAME encoded_video_packets_tests COMMAND encoded_video_packets_tests)

    add_executable(
            ffmpeg_video_stream_parameters_tests
            tests/ffmpeg_video_stream_parameters_tests.cpp
            src/AvBitstreamFilter.cpp
            src/FfmpegRawVideoSource.cpp
            src/AvPacket.cpp
            src/AvCodecParameters.cpp
            src/VideoStreamParameters.cpp
    )

    target_include_directories(
            ffmpeg_video_stream_parameters_tests
            PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include
    )

    target_link_libraries(
            ffmpeg_video_stream_parameters_tests
            PRIVATE
            ${OpenCV_LIBS}
            PkgConfig::LIBAVCODEC
            PkgConfig::LIBAVFORMAT
            PkgConfig::LIBAVUTIL
    )

    add_executable(
            ffmpeg_encoded_video_packets_tests
            tests/ffmpeg_encoded_video_packets_tests.cpp
            src/AvBitstreamFilter.cpp
            src/FfmpegRawVideoSource.cpp
            src/AvPacket.cpp
            src/AvCodecParameters.cpp
            src/VideoStreamParameters.cpp
    )

    target_include_directories(
            ffmpeg_encoded_video_packets_tests
            PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include
    )

    target_link_libraries(
            ffmpeg_encoded_video_packets_tests
            PRIVATE
            ${OpenCV_LIBS}
            PkgConfig::LIBAVCODEC
            PkgConfig::LIBAVFORMAT
            PkgConfig::LIBAVUTIL
    )

    add_executable(
            video_stream_reader_tests
            tests/video_stream_reader_tests.cpp
            src/AvBitstreamFilter.cpp
            src/VideoStreamReader.cpp
            src/FfmpegRawVideoSource.cpp
            src/AvPacket.cpp
            src/AvCodecParameters.cpp
            src/VideoStreamParameters.cpp
    )

    target_include_directories(
            video_stream_reader_tests
            PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include
    )

    target_link_libraries(
            video_stream_reader_tests
            PRIVATE
            ${OpenCV_LIBS}
            PkgConfig::LIBAVCODEC
            PkgConfig::LIBAVFORMAT
            PkgConfig::LIBAVUTIL
    )

    add_executable(
            opencv_nvdec_file_tests
            tests/opencv_nvdec_file_tests.cpp
    )

    target_include_directories(
            opencv_nvdec_file_tests
            PRIVATE ${OpenCV_INCLUDE_DIRS}
    )

    target_link_libraries(
            opencv_nvdec_file_tests
            PRIVATE ${OpenCV_LIBS}
    )
endif()
