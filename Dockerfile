# Multi-stage build for IDTX-Core server
# Build environment with all dependencies
#
# Build args:
#   IMAGING=0|1  When 1, OpenUSD is built with Hydra imaging and the server is
#                compiled with the real UsdImagingThumbnailGenerator (software
#                OpenGL via Mesa llvmpipe). Default 0 keeps the slim build.
#
# wolfi-base pinned digest (pulled: 2026-08-05)
FROM cgr.dev/chainguard/wolfi-base@sha256:ca263a0360cca48e8fe3f86c8af61c6d5b85e484809fe187440a4206a50efc06 AS builder

ARG IMAGING=1

# Install build dependencies.
# NOTE: python-3.13 is pinned to 3.13.15-r3. The current -r4 revision in the
# Wolfi repo is linked against GLIBC_2.44, but the newest glibc available in
# this repo index only provides up to GLIBC_2.43, so python (and therefore
# scons, which imports math) fails with
# "libm.so.6: version `GLIBC_2.44' not found". -r3 is the last revision that
# only needs <= GLIBC_2.43. Remove the pin once Wolfi ships glibc >= 2.44.
#
# NOTE: perl is pinned to 5.44.0-r0 for the same GLIBC reason. The current
# -r1 revision links POSIX.so against GLIBC_2.44, so loading POSIX fails with
# "libm.so.6: version `GLIBC_2.44' not found". This breaks IPC::Cmd (which
# uses POSIX::_exit / POSIX::WNOHANG), which in turn makes vcpkg's openssl
# build abort with "Perl cannot find IPC::Cmd". -r0 still needs only
# <= GLIBC_2.43. Remove the pin once Wolfi ships glibc >= 2.44.
RUN apk update && apk add --no-cache \
    bash \
    git \
    python-3.13=3.13.15-r3 \
    python3 \
    scons \
    perl=5.44.0-r0 \
    cmake \
    make \
    wget \
    curl \
    gnutar \
    zip \
    unzip \
    pkgconf \
    gcc \
    glibc-dev \
    linux-headers

# Imaging builds additionally need Mesa OpenGL/EGL headers and libs so
# OpenUSD's Hydra (Storm/HgiGL) can compile and link against software GL.
# These are *build-time* dependencies only; the actual software-GL runtime is
# provided by the Ubuntu runtime stage (see below), not by Wolfi's Mesa.
#
# Headless rendering uses the EGL surfaceless platform (EglContext in
# UsdImagingThumbnailGenerator.cpp). mesa-dev / libglvnd-dev supply EGL/egl.h
# and EGL/eglext.h at compile time; the EGL/GL symbols are resolved at runtime
# against Ubuntu's libglvnd + Mesa.
#
# NOTE: libx11-dev and libglvnd-dev are required at *configure* time.
# OpenUSD's pxr/imaging/garch/CMakeLists.txt selects its GL platform-context
# and debug-window sources from the ${GARCH_GLPLATFORMCONTEXT}.h variable,
# which on Linux is only set in the X11/GLX branch. Without X11,
# find_package(X11) fails, those vars stay empty, and CMake aborts with
# "Cannot find source file: .../garch/.h". libx11-dev provides X11/Xlib.h;
# libglvnd-dev provides the GLX headers (GL/glx.h) and libGL.so.
RUN if [ "$IMAGING" = "1" ]; then \
        apk add --no-cache mesa mesa-dev mesa-gl \
            libx11-dev libglvnd-dev ; \
    fi


# Copy the source code and shared tools
COPY . /idtx-core

# Set working directory
WORKDIR /idtx-core

# Set cc and cxx to gcc.
# NOTE: We intentionally use gcc/g++ rather than clang here. The current Wolfi
# clang/LLVM build is linked against GLIBC_2.44 (libLLVM.so needs it), but the
# glibc available in this repo index only provides up to GLIBC_2.43, so any
# invocation of clang/clang++ fails with
# "libm.so.6: version `GLIBC_2.44' not found" and cannot even run. This breaks
# vcpkg's compiler detection (it honours CC/CXX) and the main build. gcc/g++ in
# this image only need <= GLIBC_2.43 and work correctly. This mirrors the
# python-3.13 GLIBC pin above; revisit once Wolfi ships glibc >= 2.44.
ENV CC=gcc
ENV CXX=g++

RUN scons target=release vcpkg_only=1
RUN if [ "$IMAGING" = "1" ]; then \
        scons target=release imaging=1 openusd_only=1 ; \
    else \
        scons target=release openusd_only=1; \
    fi

# Build using SCons, this will handle installation and compilation of all dependencies.
# Pass imaging=1 (without osmesa=1) so IDTX_USE_OSMESA is NOT defined and the
# EGL surfaceless path (EglContext) is compiled. The Ubuntu runtime stage
# provides the Mesa/EGL software-GL stack the binary dlopens at run time.
RUN if [ "$IMAGING" = "1" ]; then \
        scons target=release imaging=1 -j1 ; \
    else \
        scons target=release -j1 ; \
    fi

# Verify the build output exists
RUN ls -la /idtx-core/bin/ && \
    test -f '/idtx-core/bin/idtx-core' && \
    echo "Build successful!"

# ---------------------------------------------------------------------------
# Runtime stage
# ---------------------------------------------------------------------------
# The builder above runs on Wolfi (needed for the pinned toolchain), but the
# runtime uses Ubuntu 26.04 because it ships glibc >= 2.44. Every Mesa software
# GL backend on the current Wolfi repo (mesa-egl/llvmpipe via libLLVM-22,
# mesa-osmesa/libgallium via libLLVM-19) is linked against GLIBC_2.44, which
# Wolfi's glibc (2.43 max) cannot satisfy, so headless rendering is impossible
# on a Wolfi runtime. Ubuntu's native Mesa is self-consistent with its own
# glibc, so the EGL surfaceless path works.
#
# The idtx-core binary and libusd_ms.so are built on Wolfi against glibc 2.43;
# glibc is forward compatible, so they load on Ubuntu's newer glibc. Only the
# application's own build products are copied across — NO Wolfi shared
# libraries are copied. Overlaying one distro's Mesa/glibc-linked .so onto
# another distro's glibc is exactly what broke earlier attempts. All GL / X11 /
# TBB / C++ runtime libraries come from Ubuntu's apt so they stay consistent
# with Ubuntu's own glibc and Mesa vendor ICD manifest.
FROM ubuntu:26.04 AS runtime

ARG IMAGING=1

WORKDIR /app

# Runtime shared-library dependencies.
#   libstdc++6 / libgcc-s1 : C++ runtime for the gcc-built binary. Ubuntu's is
#                            >= the Wolfi build's, so GLIBCXX symbols resolve.
#   libtbb12               : oneTBB (USD built with --onetbb) runtime.
# OpenSSL and zlib are linked statically from vcpkg, so no libssl/zlib package
# is needed here. If a runtime "libssl.so.3 not found" ever appears, add
# libssl3t64 + zlib1g.
#
# Imaging additionally needs the headless software-GL stack:
#   libegl1 / libgl1 / libglx0 / libglvnd0 : libglvnd GL/EGL dispatch layer.
#   libegl-mesa0 / libgl1-mesa-dri         : Mesa EGL vendor + llvmpipe/swrast
#                                            Gallium drivers, and the glvnd ICD
#                                            manifest under
#                                            /usr/share/glvnd/egl_vendor.d.
#   libx11-6                               : garch was configured via the
#                                            X11/GLX path so libusd_ms.so links
#                                            libX11 at load time.
RUN apt-get update \
    && apt-get install -y --no-install-recommends \
        ca-certificates libstdc++6 libgcc-s1 libtbb12 \
    && if [ "$IMAGING" = "1" ]; then \
        apt-get install -y --no-install-recommends \
            libegl1 libgl1 libglx0 libglvnd0 \
            libegl-mesa0 libgl1-mesa-dri libx11-6 ; \
    fi \
    && rm -rf /var/lib/apt/lists/*

# Create data dirs owned by the non-root runtime user, then copy the built
# application (idtx-core, libusd_ms.so, libosd*.so, USD plugin config) from the
# Wolfi builder. The plugin config lands at /app/plugin/usd (see
# PXR_PLUGINPATH_NAME below) via the SConstruct post-build copy.
RUN mkdir -p /app/uploads /app/data && chown -R 65532:65532 /app
COPY --from=builder --chown=65532:65532 /idtx-core/bin/ /app/

# The binary is built with Wolfi's gcc 16 (libstdc++.so.6.0.36, exporting
# GLIBCXX_3.4.35), which is NEWER than Ubuntu 26.04's libstdc++6. Bundle the
# Wolfi C++ runtime into /app so the newer GLIBCXX/GCC symbols resolve —
# LD_LIBRARY_PATH=/app (set below) puts these ahead of Ubuntu's /usr/lib. A
# newer libstdc++ is a strict superset, so Ubuntu's own Mesa/GL libraries that
# were built against the older libstdc++ keep working against this one. Both
# link glibc 2.43 and load fine on Ubuntu's newer glibc (forward compatible).
COPY --from=builder /usr/lib/libstdc++.so.6* /app/
COPY --from=builder /usr/lib/libgcc_s.so.1 /app/

# Create volume mount points for persistent data
VOLUME ["/app/uploads", "/app/data"]

# Expose the server port
EXPOSE 8080

# Set environment variables for USD
ENV PXR_PLUGINPATH_NAME=/app/plugin/usd
ENV LD_LIBRARY_PATH=/app

# --- Software OpenGL (imaging thumbnail rendering) ------------------------
# Headless rendering uses the EGL surfaceless platform (EglContext in
# UsdImagingThumbnailGenerator.cpp): no display server, GPU, or DRI device
# nodes required. Mesa's llvmpipe provides a full GL 4.5 software rasteriser.
# EnsureSoftwareGlEnv() also sets these at runtime if unset; they are declared
# here explicitly for clarity and for any child processes.
ENV LIBGL_ALWAYS_SOFTWARE=1
ENV GALLIUM_DRIVER=llvmpipe
ENV MESA_GL_VERSION_OVERRIDE=4.5
ENV MESA_GLSL_VERSION_OVERRIDE=450
# --------------------------------------------------------------------------

# Set environment for default upload path
ENV IDTX_UPLOADS_ROOT=/app/uploads

# --- Security / anti-abuse safety net (in-process rate limiting) ---------
# These are a per-replica safety net that complements the primary throttling
# expected at the Kubernetes ingress / cloud load balancer. All values are
# optional; the application ships with conservative built-in defaults.
#
#   SERVER_TIMEOUT_SECONDS        Idle connection timeout (Slowloris guard).
#   RL_GLOBAL_MAX_REQUESTS        Per-IP request budget per RL_GLOBAL_WINDOW_SECONDS.
#   RL_GLOBAL_WINDOW_SECONDS      Window length for the global budget.
#   RL_LOGIN_MAX_REQUESTS         Per-IP login request budget per RL_LOGIN_WINDOW_SECONDS.
#   RL_LOGIN_WINDOW_SECONDS       Window length for the login budget.
#   RL_LOGIN_MAX_FAILURES         Failed logins before a source is locked out.
#   RL_LOGIN_FAILURE_WINDOW_SECONDS  Window in which failures accumulate.
#   RL_LOGIN_LOCKOUT_SECONDS      Lockout duration once the failure threshold trips.
#   RL_LOGIN_MAX_BODY_BYTES       Max accepted body size for /api/v1/auth/login.
#   RL_GLOBAL_MAX_BODY_BYTES      Max accepted body size for other endpoints.
#   RL_TRUST_FORWARDED_FOR        "true" (default) to honour X-Forwarded-For.
# --------------------------------------------------------------------------

# Start the server
USER 65532:65532
CMD ["/app/idtx-core"]
