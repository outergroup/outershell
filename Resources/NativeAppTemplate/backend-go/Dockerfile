# Toolchain image for building the Linux backend from macOS with Apple's
# `container` tool. `./app build-backend` runs `go build` inside this image;
# the Dockerfile pins the toolchain version in the project.
#
# CGO is disabled in the build, so the output is a static binary that runs on
# any Linux distribution regardless of libc.
FROM docker.io/library/golang:1.23-alpine
WORKDIR /src
