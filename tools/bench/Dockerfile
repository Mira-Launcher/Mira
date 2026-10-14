# The benchmark container: CI's toolchain (Ubuntu 24.04, GCC 14, Qt 6.12.0) plus the profilers, Xvfb for the
# GUI, and Pillow for the fixture's generated art. Runs with no network, so nothing measured can reach the web.
FROM ubuntu:24.04

ARG QT_VERSION=6.12.0
ENV DEBIAN_FRONTEND=noninteractive \
    QT_ROOT=/opt/Qt/${QT_VERSION}/gcc_64

RUN apt-get update && apt-get install -y --no-install-recommends \
      ca-certificates git cmake ninja-build ccache pkg-config gcc-14 g++-14 python3 python3-pip python3-pil \
      sqlite3 curl unzip cabextract p7zip-full libcurl4-openssl-dev libgl1-mesa-dev libegl1-mesa-dev \
      libxkbcommon-dev libxkbcommon-x11-0 libxcb-cursor0 libxcb-icccm4 libxcb-image0 libxcb-keysyms1 \
      libxcb-randr0 libxcb-render-util0 libxcb-shape0 libxcb-xinerama0 libxcb-xkb1 libpulse0 libdbus-1-3 \
      libfontconfig1 libglib2.0-0 \
      valgrind heaptrack xvfb xdotool imagemagick dbus \
 && rm -rf /var/lib/apt/lists/*

RUN pip3 install --no-cache-dir --break-system-packages aqtinstall \
 && aqt install-qt linux desktop ${QT_VERSION} linux_gcc_64 -m qtmultimedia --outputdir /opt/Qt

ENV CC=gcc-14 CXX=g++-14 CMAKE_PREFIX_PATH=${QT_ROOT} PATH=${QT_ROOT}/bin:${PATH} \
    CCACHE_DIR=/ccache CCACHE_MAXSIZE=5G \
    CCACHE_SLOPPINESS=pch_defines,time_macros,include_file_mtime,include_file_ctime
