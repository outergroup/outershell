# Toolchain image for building the Linux backend from macOS via Docker
# Desktop. `./app build-backend` runs `go build` inside this image; the
# Dockerfile exists so the toolchain version is pinned in the project rather
# than in the build script.
#
# CGO is disabled in the build, so the output is a static binary that runs on
# any Linux distribution regardless of libc.
FROM golang:1.23-alpine
WORKDIR /src
