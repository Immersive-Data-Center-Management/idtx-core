# Multi-stage build for IDTX-Core server
# Build environment with all dependencies
#
# wolfi-base pinned digest (pulled: 2026-08-05)
FROM cgr.dev/chainguard/wolfi-base@sha256:ca263a0360cca48e8fe3f86c8af61c6d5b85e484809fe187440a4206a50efc06 AS builder

# Install build dependencies
RUN apk update && apk add --no-cache \
    bash \
    git \
    python3 \
    scons \
    perl \
    clang \
    cmake \
    make \
    wget \
    curl \
    gnutar \
    zip \
    unzip \
    pkgconf


# Copy the source code and shared tools
COPY . /idtx-core

# Set working directory
WORKDIR /idtx-core

# Set cc and cxx to clang
ENV CC=clang
ENV CXX=clang++

# Build using SCons, this will handle installation and compilation of all dependencies
RUN scons target=release -j1

# Verify the build output exists
RUN ls -la /idtx-core/bin/ && \
    test -f '/idtx-core/bin/idtx-core' && \
    echo "Build successful!"

# wolfi-base pinned digest (pulled: 2026-08-05)
FROM cgr.dev/chainguard/wolfi-base@sha256:ca263a0360cca48e8fe3f86c8af61c6d5b85e484809fe187440a4206a50efc06 AS libs

# Install runtime dependencies
RUN apk update && apk add --no-cache libtbb

# Stage the runtime filesystem layout in an image that HAS a shell
FROM cgr.dev/chainguard/wolfi-base@sha256:ca263a0360cca48e8fe3f86c8af61c6d5b85e484809fe187440a4206a50efc06 AS rootfs
# Create uploads and data folder
RUN mkdir -p /out/app/uploads /out/app/data
# Copy built application and dependencies from builder
COPY --from=builder /idtx-core/bin /out/app/
# Copy library dependencies from libs stage
COPY --from=libs /usr/lib/ /out/usr/lib/

# Runtime environment
# glibc-dynamic pinned digest (pulled: 2026-08-07)
FROM cgr.dev/chainguard/glibc-dynamic:latest AS runtime

# Set working directory
WORKDIR /app

# Bring in the pre-staged filesystem with correct ownership in one shot
COPY --from=rootfs --chown=65532:65532 /out/ /

# Create volume mount points for persistent data
VOLUME ["/app/uploads", "/app/data"]

# Expose the server port
EXPOSE 8080

# Set environment variables for USD
ENV PXR_PLUGINPATH_NAME=/app/plugin/usd
ENV LD_LIBRARY_PATH=/app
# Enable USD debug logging
# ENV TF_DEBUG=PLUG_REGISTRATION,USD_STAGE_OPEN

# Set environment for default upload path
ENV IDTX_UPLOADS_ROOT=/app/uploads

# Start the server
USER 65532:65532
CMD ["/app/idtx-core"]
