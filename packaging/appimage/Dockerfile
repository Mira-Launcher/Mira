# Builds the Mira AppImage on Ubuntu 22.04 (glibc 2.35), so the libraries it bundles run on
# 22.04 and anything newer. See cmake/AppImageContainer.cmake.
FROM ubuntu:22.04

ARG QT_VERSION=6.12.0
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
      libxcb1-dev libxcb-xfixes0 libxcb-sync1 libxcb-shm0 libxcb-util1 libxrender1 libsm6 libice6 libcurl4-openssl-dev rpm \
      libpulse0 libxrandr2 libwayland-client0 libwayland-cursor0 libwayland-egl1 \
 && add-apt-repository -y ppa:ubuntu-toolchain-r/test \
 && apt-get update && apt-get install -y --no-install-recommends gcc-13 g++-13 \
 && rm -rf /var/lib/apt/lists/*

# Qt 6.12 from Qt's own builds (22.04's Qt is 6.2), which need glibc 2.34; its FFmpeg loads
# OpenSSL 3 for https video. It needs CMake 3.25 (22.04's is 3.22), from Kitware's pip package.
RUN pip3 install --no-cache-dir aqtinstall cmake==3.31.6 \
 && aqt install-qt linux desktop ${QT_VERSION} linux_gcc_64 -m qtmultimedia --outputdir /opt/Qt

# Compiler cache: its directory is mounted from the host (cmake/AppImageContainer.cmake), so a rebuilt
# container, a changed Dockerfile or a clean build directory still reuses every object file it has seen.
RUN apt-get update && apt-get install -y --no-install-recommends ccache && rm -rf /var/lib/apt/lists/*
ENV CCACHE_DIR=/tmp/ccache CCACHE_COMPILERCHECK=content CCACHE_SLOPPINESS=pch_defines,time_macros,include_file_mtime,include_file_ctime \
    CCACHE_MAXSIZE=5G

ENV CC=gcc-13 CXX=g++-13 CMAKE_PREFIX_PATH=${QT_ROOT} PATH=${QT_ROOT}/bin:${PATH}

# Qt reads AVIF, HEIF and JPEG XL only through KDE's kimageformats plugins, which Qt's own builds
# lack. Sites serve covers in these formats, often under a .png name. 22.04 has libavif and libheif
# but no libjxl, so that one is built too, on the system's brotli (its own copy would be bundled
# beside the system one). The plugins go into Qt's own plugin folder.
ARG KF_VERSION=6.10.0
ARG LIBJXL_VERSION=0.11.1
RUN apt-get update \
 && apt-get install -y --no-install-recommends libavif-dev libheif-dev libbrotli-dev \
 && rm -rf /var/lib/apt/lists/* \
 && git clone --depth 1 --branch v${LIBJXL_VERSION} --recursive --shallow-submodules \
      https://github.com/libjxl/libjxl /tmp/libjxl \
 && cmake -S /tmp/libjxl -B /tmp/libjxl/build -G Ninja -DCMAKE_BUILD_TYPE=Release \
      -DBUILD_TESTING=OFF -DJPEGXL_ENABLE_TOOLS=OFF -DJPEGXL_ENABLE_DOXYGEN=OFF \
      -DJPEGXL_ENABLE_MANPAGES=OFF -DJPEGXL_ENABLE_BENCHMARK=OFF -DJPEGXL_ENABLE_EXAMPLES=OFF \
      -DJPEGXL_ENABLE_JNI=OFF -DJPEGXL_ENABLE_SJPEG=OFF -DJPEGXL_ENABLE_OPENEXR=OFF \
      -DJPEGXL_ENABLE_PLUGINS=OFF -DJPEGXL_ENABLE_JPEGLI=OFF -DJPEGXL_FORCE_SYSTEM_BROTLI=ON \
 && cmake --build /tmp/libjxl/build && cmake --install /tmp/libjxl/build && ldconfig \
 && git clone --depth 1 --branch v${KF_VERSION} https://invent.kde.org/frameworks/extra-cmake-modules.git /tmp/ecm \
 && cmake -S /tmp/ecm -B /tmp/ecm/build -G Ninja -DBUILD_TESTING=OFF -DBUILD_HTML_DOCS=OFF \
      -DBUILD_MAN_DOCS=OFF -DBUILD_QTHELP_DOCS=OFF \
 && cmake --install /tmp/ecm/build \
 && git clone --depth 1 --branch v${KF_VERSION} https://invent.kde.org/frameworks/kimageformats.git /tmp/kimageformats \
 && cmake -S /tmp/kimageformats -B /tmp/kimageformats/build -G Ninja -DCMAKE_BUILD_TYPE=Release \
      -DBUILD_TESTING=OFF -DKIMAGEFORMATS_HEIF=ON -DKIMAGEFORMATS_JXL=ON \
      -DKDE_INSTALL_USE_QT_SYS_PATHS=ON \
 && cmake --build /tmp/kimageformats/build && cmake --install /tmp/kimageformats/build \
 && ls ${QT_ROOT}/plugins/imageformats/kimg_avif.so ${QT_ROOT}/plugins/imageformats/kimg_heif.so \
      ${QT_ROOT}/plugins/imageformats/kimg_jxl.so \
 && rm -rf /tmp/libjxl /tmp/ecm /tmp/kimageformats
