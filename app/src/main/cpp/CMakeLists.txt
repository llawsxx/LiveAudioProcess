cmake_minimum_required(VERSION 3.22.1)
project(pulseforge_audio C)

add_library(pulseforge_audio SHARED native_audio.c convolution_reverb.c)
target_compile_options(pulseforge_audio PRIVATE -O3 -ffast-math -Wall -Wextra)
target_link_libraries(pulseforge_audio log m aaudio)
