cmake_minimum_required(VERSION 3.0)

project(ReStreamer)

if(DEFINED ENV{SNAPCRAFT_BUILD_ENVIRONMENT})
    add_definitions(-DSNAPCRAFT_BUILD=1)
    set(SNAPCRAFT_BUILD YES)
endif()

add_subdirectory(WebRTSP)

find_package(Threads REQUIRED)
find_package(PkgConfig REQUIRED)

pkg_search_module(SPDLOG REQUIRED spdlog)
pkg_search_module(LIBCONFIG REQUIRED libconfig)

file(GLOB SOURCES RELATIVE ${CMAKE_CURRENT_SOURCE_DIR}
    *.cpp
    *.h
    *.conf.sample
    README.md
    *.cmake)

file(GLOB_RECURSE SNAP RELATIVE ${CMAKE_CURRENT_SOURCE_DIR}
    snap/*)

add_executable(${PROJECT_NAME} ${SOURCES} ${SNAP})
target_include_directories(${PROJECT_NAME} PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/WebRTSP
    ${CMAKE_CURRENT_SOURCE_DIR}/WebRTSP/RtcStreaming/deps
    ${LIBCONFIG_INCLUDE_DIRS}
    ${SPDLOG_INCLUDE_DIRS})
target_link_libraries(${PROJECT_NAME}
    ${LIBCONFIG_LIBRARIES}
    ${SPDLOG_LDFLAGS}
    Http
    RtspParser
    Signalling
    RtcStreaming)

if(SNAPCRAFT_BUILD)
    install(TARGETS ${PROJECT_NAME} DESTINATION bin)
    install(FILES ${CMAKE_CURRENT_SOURCE_DIR}/restreamer.conf.sample DESTINATION etc)
endif()
