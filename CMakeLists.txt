cmake_minimum_required(VERSION 3.0)

set(CMAKE_CXX_STANDARD 14)

project(ReStreamer)

option(CAMERA_STREAMER "Build as camera streamer app" OFF)
option(V4L2_STREAMER "Build as app to stream from V4L2 device" OFF)
option(ONVIF_SUPPORT "ONVIF support" ON)

if(DEFINED ENV{SNAPCRAFT_PROJECT_NAME})
    add_definitions(-DSNAPCRAFT_BUILD=1)
    set(SNAPCRAFT_BUILD YES)
endif()

if(CAMERA_STREAMER)
    add_definitions(-DBUILD_AS_CAMERA_STREAMER=1)
    set(ONVIF_SUPPORT OFF CACHE BOOL "ONVIF not used by camera streamer" FORCE)
elseif(V4L2_STREAMER)
    add_definitions(-DBUILD_AS_V4L2_STREAMER=1)
    set(ONVIF_SUPPORT OFF CACHE BOOL "ONVIF not used by V4L2 streamer" FORCE)
endif()

if(ONVIF_SUPPORT)
    add_definitions(-DONVIF_SUPPORT=1)
endif()

add_subdirectory(WebRTSP)

find_package(Threads REQUIRED)
find_package(PkgConfig REQUIRED)

pkg_search_module(LIBCONFIG REQUIRED libconfig)

file(GLOB SOURCES RELATIVE ${CMAKE_CURRENT_SOURCE_DIR}
    *.cpp
    *.h
    *.conf.sample
    README.md
    BUILD.md
    *.sh
    *.cmake)

file(GLOB_RECURSE SNAP RELATIVE ${CMAKE_CURRENT_SOURCE_DIR}
    snap/*)

file(GLOB_RECURSE WWW RELATIVE ${CMAKE_CURRENT_SOURCE_DIR}
    www/[^.]*)

add_executable(${PROJECT_NAME} ${SOURCES} ${SNAP} ${WWW})
target_include_directories(${PROJECT_NAME} PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/WebRTSP
    ${LIBCONFIG_INCLUDE_DIRS}
    ${SPDLOG_INCLUDE_DIRS})
target_link_libraries(${PROJECT_NAME}
    ${LIBCONFIG_LIBRARIES}
    ${SPDLOG_LDFLAGS}
    Http
    RtspParser
    Client
    Signalling
    RtStreaming)
if(CAMERA_STREAMER)
    set_target_properties(${PROJECT_NAME} PROPERTIES OUTPUT_NAME "CameraStreamer")
elseif(V4L2_STREAMER)
    set_target_properties(${PROJECT_NAME} PROPERTIES OUTPUT_NAME "V4L2Streamer")
endif()

if(SNAPCRAFT_BUILD)
    install(TARGETS ${PROJECT_NAME} DESTINATION bin)
    if(CAMERA_STREAMER)
        install(FILES ${CMAKE_CURRENT_SOURCE_DIR}/camera-streamer.conf.sample DESTINATION etc)
    elseif(V4L2_STREAMER)
        install(FILES ${CMAKE_CURRENT_SOURCE_DIR}/v4l2-streamer.conf.sample DESTINATION etc)
    else()
        install(FILES ${CMAKE_CURRENT_SOURCE_DIR}/restreamer.conf.sample DESTINATION etc)
    endif()
    install(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/www
        DESTINATION /opt/$ENV{SNAPCRAFT_PROJECT_NAME}
        FILES_MATCHING
        PATTERN "www/3rd_party/*.js"
        PATTERN "www/3rd_party/*.css"
        PATTERN "www/WebRTSP/*.mjs"
        PATTERN "www/css/*.css"
        PATTERN "www/index.html"
        PATTERN "www/favicon.ico")
endif()
