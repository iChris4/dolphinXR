FROM ubuntu:26.04

RUN apt-get update \
  && apt-get install -y \
    build-essential \
    clang \
    cmake \
    pkg-config \
    libgl1-mesa-dev \
    libx11-dev \
    libxrandr-dev \
    libxi-dev \
    libegl1-mesa-dev \
    libavcodec-dev \
    libavformat-dev \
    libavutil-dev \
    libswresample-dev \
    libswscale-dev \
    libudev-dev \
    libevdev-dev \
    libsdl3-dev \
    libfmt-dev \
    glslang-dev \
    glslang-tools \
    libpugixml-dev \
    libenet-dev \
    libxxhash-dev \
    libbz2-dev \
    liblzma-dev \
    libzstd-dev \
    zlib1g-dev \
    libminizip-ng-dev \
    liblzo2-dev \
    liblz4-dev \
    libspng-dev \
    libcubeb-dev \
    libusb-1.0-0-dev \
    libsfml-dev \
    libminiupnpc-dev \
    libcurl4-openssl-dev \
    libhidapi-dev \
    libsystemd-dev \
    libgtest-dev \
    libasound2-dev \
    libpulse-dev \
    llvm-dev \
    libbluetooth-dev \
    qt6-base-dev \
    qt6-base-private-dev \
    qt6-svg-dev \
    gettext \
    libvulkan-dev \
    vulkan-tools

RUN mkdir -p /app/dolphinxr
WORKDIR /app/dolphinxr

# Folders except Externals
COPY .tx CMake Data docs Flatpak Installer Languages LICENSES Source Tools ./
# TopLevel files
COPY .dockerignore .editorconfig .git-blame-ignore-revs .gitattributes .gitignore .gitmodules .mailmap AndroidSetup.md BuildMacOSUniversalBinary.py CMakeLists.txt CMakeSettings.json CODE_OF_CONDUCT.md Contributing.md COPYING Dockerfile Readme.md ./
# Externals
COPY Externals Externals

RUN mkdir Build \
  && cd Build \
  && cmake .. -DENABLE_VR=ON -DENABLE_VULKAN=ON -DLINUX_LOCAL_DEV=true -DUSE_SYSTEM_MBEDTLS=OFF -DUSE_SYSTEM_LIBMGBA=OFF \
  && cd .. \
  && cmake --build Build --target dolphin-emu -j $(nproc)
