FROM ubuntu:24.04

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && \
    apt-get install -y --no-install-recommends \
        bash \
        bc \
        bison \
        build-essential \
        ca-certificates \
        clang \
        cpio \
        dwarves \
        elfutils \
        file \
        flex \
        gcc-aarch64-linux-gnu \
        git \
        gzip \
        libdw-dev \
        libc6-dev-arm64-cross \
        libelf-dev \
        lld \
        libssl-dev \
        python3 \
        qemu-system-arm \
        socat \
        tar \
        unzip \
        llvm \
        xz-utils \
        zip \
        zstd && \
    rm -rf /var/lib/apt/lists/*
