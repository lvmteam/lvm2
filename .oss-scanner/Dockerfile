# OSS Scanner build image for LVM2 userspace (tools, libdm, daemons).
# Network is available during this build only; the scanner runs offline afterward.
FROM debian:bookworm
ENV DEBIAN_FRONTEND=noninteractive \
    CC=clang \
    CXX=clang++ \
    CFLAGS="-g -fno-omit-frame-pointer" \
    CXXFLAGS="-g -fno-omit-frame-pointer"

RUN apt-get update && apt-get install -y --no-install-recommends \
      ca-certificates \
      autoconf automake libtool \
      build-essential clang \
      pkg-config \
      libaio-dev libblkid-dev libudev-dev \
      libselinux1-dev libsepol-dev \
      libncurses-dev libreadline-dev \
      libsystemd-dev libdbus-1-dev \
      python3 \
    && rm -rf /var/lib/apt/lists/*

COPY . /src
WORKDIR /src

RUN ./configure \
      --enable-udev_sync \
      --enable-cmdlib --enable-dmeventd \
      --enable-pkgconfig --enable-readline \
      --enable-dbus-service --enable-notify-dbus \
      --enable-lvmpolld --enable-dmfilemapd \
      --with-thin=internal --with-cache=internal \
      --with-vdo=internal --with-writecache=internal \
      --with-integrity=internal \
    && make -j"$(nproc)"

RUN make run-unit-test
