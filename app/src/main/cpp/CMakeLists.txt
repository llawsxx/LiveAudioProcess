cmake_minimum_required(VERSION 3.22.1)
project(audioprocess LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(LIBUSB_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/third_party/libusb)
set(LIBUAC_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/third_party/libuac)
set(FDK_AAC_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/third_party/fdk-aac)
set(OPUS_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/third_party/opus)

set(BUILD_SHARED_LIBS OFF CACHE BOOL "Build static third-party libraries" FORCE)
set(BUILD_PROGRAMS OFF CACHE BOOL "Do not build FDK-AAC tools" FORCE)
set(FDK_AAC_INSTALL_CMAKE_CONFIG_MODULE OFF CACHE BOOL "Do not install FDK-AAC config" FORCE)
set(FDK_AAC_INSTALL_PKGCONFIG_MODULE OFF CACHE BOOL "Do not install FDK-AAC pkg-config" FORCE)
add_subdirectory(${FDK_AAC_ROOT} fdk-aac EXCLUDE_FROM_ALL)
set(OPUS_BUILD_SHARED_LIBRARY OFF CACHE BOOL "Build Opus as a static library" FORCE)
set(OPUS_BUILD_TESTING OFF CACHE BOOL "Do not build Opus tests" FORCE)
set(OPUS_BUILD_PROGRAMS OFF CACHE BOOL "Do not build Opus programs" FORCE)
set(OPUS_INSTALL_PKG_CONFIG_MODULE OFF CACHE BOOL "Do not install Opus pkg-config" FORCE)
set(OPUS_INSTALL_CMAKE_CONFIG_MODULE OFF CACHE BOOL "Do not install Opus CMake config" FORCE)
add_subdirectory(${OPUS_ROOT} opus EXCLUDE_FROM_ALL)

add_library(libusb STATIC
        ${LIBUSB_ROOT}/libusb/core.c
        ${LIBUSB_ROOT}/libusb/descriptor.c
        ${LIBUSB_ROOT}/libusb/hotplug.c
        ${LIBUSB_ROOT}/libusb/io.c
        ${LIBUSB_ROOT}/libusb/sync.c
        ${LIBUSB_ROOT}/libusb/strerror.c
        ${LIBUSB_ROOT}/libusb/os/linux_usbfs.c
        ${LIBUSB_ROOT}/libusb/os/events_posix.c
        ${LIBUSB_ROOT}/libusb/os/threads_posix.c
        ${LIBUSB_ROOT}/libusb/os/linux_netlink.c)
target_include_directories(libusb PUBLIC ${LIBUSB_ROOT}/libusb PRIVATE ${LIBUSB_ROOT})
target_compile_options(libusb PRIVATE -fvisibility=hidden -pthread)
target_link_libraries(libusb log)

add_library(uac STATIC
        ${LIBUAC_ROOT}/src/uac_context.cpp
        ${LIBUAC_ROOT}/src/uac_device.cpp
        ${LIBUAC_ROOT}/src/uac_parser.cpp
        ${LIBUAC_ROOT}/src/uac_streaming.cpp
        ${LIBUAC_ROOT}/src/uac_exceptions.cpp)
target_include_directories(uac PRIVATE ${LIBUAC_ROOT}/src ${LIBUAC_ROOT}/include ${LIBUSB_ROOT}/libusb ${LIBUSB_ROOT})
target_link_libraries(uac PRIVATE libusb)
target_compile_options(uac PRIVATE -fno-rtti -pthread)
target_compile_definitions(uac PRIVATE UAC_ENABLE_LOGGING)

add_library(audioprocess SHARED native_audio.c convolution_reverb.c test_music.c usb_host_bridge.cpp wifi_aac_bridge.cpp wifi_opus_bridge.cpp)
target_include_directories(audioprocess PRIVATE ${LIBUAC_ROOT}/include ${LIBUSB_ROOT}/libusb)
target_compile_options(audioprocess PRIVATE -O3 -ffast-math -Wall -Wextra)
target_link_libraries(audioprocess log m dl aaudio uac libusb fdk-aac opus)
