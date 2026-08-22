cmake_minimum_required(VERSION 3.18)
project(decode-client C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# 编译选项
add_compile_options(-Wall -Wextra -Wpedantic -O2)

# ---- 查找依赖 ----

# FFmpeg (libavcodec, libavformat, libavutil)
find_package(PkgConfig REQUIRED)
pkg_check_modules(AVCODEC  REQUIRED libavcodec)
pkg_check_modules(AVFORMAT REQUIRED libavformat)
pkg_check_modules(AVUTIL   REQUIRED libavutil)

# EGL
pkg_check_modules(EGL REQUIRED egl)

# OpenGL ES 3.x
pkg_check_modules(GLESV2 REQUIRED glesv2)

# X11
find_package(X11 REQUIRED)

# ---- 源文件 ----
set(SOURCES
    src/main.c
    src/demuxer.c
    src/comm.c
    src/renderer.c
)

# ---- 构建目标 ----
add_executable(decode-client ${SOURCES})

target_include_directories(decode-client PRIVATE
    ${AVCODEC_INCLUDE_DIRS}
    ${AVFORMAT_INCLUDE_DIRS}
    ${AVUTIL_INCLUDE_DIRS}
    ${EGL_INCLUDE_DIRS}
    ${GLESV2_INCLUDE_DIRS}
    ${X11_INCLUDE_DIR}
)

target_link_libraries(decode-client PRIVATE
    ${AVCODEC_LIBRARIES}
    ${AVFORMAT_LIBRARIES}
    ${AVUTIL_LIBRARIES}
    ${EGL_LIBRARIES}
    ${GLESV2_LIBRARIES}
    X11::X11
    m  # math library
)

target_link_directories(decode-client PRIVATE
    ${AVCODEC_LIBRARY_DIRS}
    ${AVFORMAT_LIBRARY_DIRS}
    ${AVUTIL_LIBRARY_DIRS}
    ${EGL_LIBRARY_DIRS}
    ${GLESV2_LIBRARY_DIRS}
)

# 安装
install(TARGETS decode-client DESTINATION bin)
