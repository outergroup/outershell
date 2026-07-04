FROM debian:bookworm-slim

RUN apt-get update \
    && apt-get install -y --no-install-recommends \
        build-essential \
        crossbuild-essential-amd64 \
        crossbuild-essential-arm64 \
        ca-certificates \
    && rm -rf /var/lib/apt/lists/*
