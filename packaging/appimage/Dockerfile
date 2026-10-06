# Builds the Mira AppImage on Ubuntu 22.04 (glibc 2.35), so the libraries it bundles run on
# 22.04 and anything newer. See cmake/AppImageContainer.cmake.
FROM ubuntu:22.04

ARG QT_VERSION=6.5.3
ENV DEBIAN_FRONTEND=noninteractive \
    APPIMAGE_EXTRACT_AND_RUN=1 \
    QT_ROOT=/opt/Qt/${QT_VERSION}/gcc_64

# GCC 13 for C++23 (22.04 ships 12). Qt's own Linux builds need these system libraries, and the
# AppImage bundles what they link against, so they are installed here and not on the user's machine.
RUN apt-get update && apt-get install -y --no-install-recommends \
      ca-certificates curl file git gnupg make ninja-build cmake pkg-config python3 python3-pip \
      software-properties-common xz-utils \
      libgl1-mesa-dev libegl1-mesa-dev libglib2.0-0 libdbus-1-3 libfontconfig1 libfreetype6 \
      libx11-xcb1 libxkbcommon0 libxkbcommon-x11-0 libxcb-cursor0 libxcb-icccm4 libxcb-image0 \
      libxcb-keysyms1 libxcb-randr0 libxcb-render-util0 libxcb-shape0 libxcb-xinerama0 libxcb-xkb1 \
      libxcb-xfixes0 libxcb-sync1 libxcb-shm0 libxcb-util1 libxrender1 libsm6 libice6 libcurl4-openssl-dev \
 && add-apt-repository -y ppa:ubuntu-toolchain-r/test \
 && apt-get update && apt-get install -y --no-install-recommends gcc-13 g++-13 \
 && rm -rf /var/lib/apt/lists/*

# Qt 6.5 or newer from Qt's own builds (22.04's Qt is 6.2).
RUN pip3 install --no-cache-dir aqtinstall \
 && aqt install-qt linux desktop ${QT_VERSION} gcc_64 --outputdir /opt/Qt

ENV CC=gcc-13 CXX=g++-13 CMAKE_PREFIX_PATH=${QT_ROOT} PATH=${QT_ROOT}/bin:${PATH}
