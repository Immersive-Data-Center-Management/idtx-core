# Multi-stage build for IDTX-Core server
# Build environment with all dependencies
#
# wolfi-base pinned digest (pulled: 2026-07-10)
FROM cgr.dev/chainguard/wolfi-base@sha256:02dab76bd852a70556b5b2002195c8a5fdab77d323c433bf6642aab080489795 AS builder

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
RUN scons target=release

# Verify the build output exists
RUN ls -la /idtx-core/bin/ && \
    test -f '/idtx-core/bin/idtx-core' && \
    echo "Build successful!"

# wolfi-base pinned digest (pulled: 2026-07-10)
FROM cgr.dev/chainguard/wolfi-base@sha256:02dab76bd852a70556b5b2002195c8a5fdab77d323c433bf6642aab080489795 AS libs

# Install runtime dependencies
RUN apk update && apk add --no-cache libtbb

# Runtime environment
# glibc-dynamic pinned digest (pulled: 2026-07-10)
FROM cgr.dev/chainguard/glibc-dynamic@sha256:7ff79e2caef2b8a137ddaf9940fb790e91148482092363760d6661e4591fd54c AS runtime

# Set working directory
WORKDIR /app

# Copy built application and dependencies from builder
COPY --from=builder /idtx-core/bin /app/

# Copy library dependencies from libs stage
COPY --from=libs /usr/lib/ /usr/lib/

# Create volume mount points for persistent data
VOLUME ["/app/uploads", "/app/data"]

# Expose the server port
EXPOSE 8080

# Set environment variables for USD
ENV PXR_PLUGINPATH_NAME=/app/plugin/usd
ENV LD_LIBRARY_PATH=/app

# Enable USD debug logging
# ENV TF_DEBUG=PLUG_REGISTRATION,USD_STAGE_OPEN

# Start the server
CMD ["/app/idtx-core"]
