FROM ubuntu:26.04

RUN apt-get update \
  && apt-get install -y \
    build-essential \
    clang \
    cmake \
    qt6-base-dev \
    qt6-base-private-dev \
    qt6-svg-dev \
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
    gettext \
    libvulkan-dev \
    vulkan-tools
    ## fmt that comes with ubuntu 26 also seems incorrect.
    # libfmt-dev \

RUN mkdir -p /app/dolphinxr
WORKDIR /app/dolphinxr

COPY . .

RUN mkdir Build \
  && cd Build \
  && cmake .. -DENABLE_VR=ON -DENABLE_VULKAN=ON -DLINUX_LOCAL_DEV=true -DUSE_SYSTEM_MBEDTLS=OFF -DUSE_SYSTEM_LIBMGBA=OFF \
  && cd .. \
  && cmake --build Build --target dolphin-emu -j $(nproc)
