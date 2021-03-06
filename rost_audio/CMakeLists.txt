cmake_minimum_required(VERSION 2.4.6)
include($ENV{ROS_ROOT}/core/rosbuild/rosbuild.cmake)

# Set the build type.  Options are:
#  Coverage       : w/ debug symbols, w/o optimization, w/ code-coverage
#  Debug          : w/ debug symbols, w/o optimization
#  Release        : w/o debug symbols, w/ optimization
#  RelWithDebInfo : w/ debug symbols, w/ optimization
#  MinSizeRel     : w/o debug symbols, w/ optimization, stripped binaries
#set(ROS_BUILD_TYPE RelWithDebInfo)

rosbuild_init()

#set the default path for built executables to the "bin" directory
set(EXECUTABLE_OUTPUT_PATH ${PROJECT_SOURCE_DIR}/bin)
#set the default path for built libraries to the "lib" directory
set(LIBRARY_OUTPUT_PATH ${PROJECT_SOURCE_DIR}/lib)

#uncomment if you have defined messages
rosbuild_genmsg()
#uncomment if you have defined services
#rosbuild_gensrv()

#common commands for building c++ executables and libraries
#rosbuild_add_library(${PROJECT_NAME} src/example.cpp)
#target_link_libraries(${PROJECT_NAME} another_library)
#rosbuild_add_boost_directories()
#rosbuild_link_boost(${PROJECT_NAME} thread)
#rosbuild_add_executable(example examples/example.cpp)
#target_link_libraries(example ${PROJECT_NAME})

rosbuild_add_boost_directories()

find_package(OpenCV REQUIRED )

find_package(PkgConfig)
pkg_check_modules(GST gstreamer-0.10)
include_directories(${GST_INCLUDE_DIRS})

include_directories(${PROJECT_SOURCE_DIR}/libmfcc)
rosbuild_add_library(mfcc libmfcc/libmfcc.c)

rosbuild_add_executable(audio_words src/mfcc_wav.cpp)
target_link_libraries(audio_words fftw3 sndfile mfcc)

rosbuild_add_executable(gen_vocab src/gen_vocab.cpp)
rosbuild_link_boost(gen_vocab program_options)
target_link_libraries(gen_vocab ${OpenCV_LIBS} fftw3 sndfile mfcc)

rosbuild_add_executable(audio_words_cli src/mfcc_wav_cli.cpp)
target_link_libraries(audio_words_cli fftw3 sndfile mfcc)
rosbuild_link_boost(audio_words_cli program_options)
