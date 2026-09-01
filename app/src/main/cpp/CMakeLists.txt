cmake_minimum_required(VERSION 3.22.1)
project(pulseforge_audio LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(LIBUSB_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/third_party/libusb)
set(LIBUAC_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/third_party/libuac)

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

add_library(pulseforge_audio SHARED native_audio.c convolution_reverb.c usb_host_bridge.cpp)
target_include_directories(pulseforge_audio PRIVATE ${LIBUAC_ROOT}/include ${LIBUSB_ROOT}/libusb)
target_compile_options(pulseforge_audio PRIVATE -O3 -ffast-math -Wall -Wextra)
target_link_libraries(pulseforge_audio log m aaudio uac libusb)
