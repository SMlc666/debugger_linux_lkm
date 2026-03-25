FROM ubuntu:24.04

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && \
    apt-get install -y --no-install-recommends \
        bash \
        bc \
        bison \
        build-essential \
        ca-certificates \
        cpio \
        file \
        flex \
        gcc-aarch64-linux-gnu \
        git \
        gzip \
        libc6-dev-arm64-cross \
        libelf-dev \
        libssl-dev \
        qemu-system-arm \
        socat \
        tar \
        unzip \
        xz-utils \
        zip \
        zstd && \
    rm -rf /var/lib/apt/lists/*
