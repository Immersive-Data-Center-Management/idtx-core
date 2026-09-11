/**
 * @file UsdImagingThumbnailGenerator.cpp
 * @brief Implementation of the Hydra-based thumbnail generator.
 *
 * The whole translation unit is compiled out unless the project is built with
 * imaging support (@c IDTX_ENABLE_IMAGING). This keeps the default build free
 * of any imaging / OpenGL dependency while still being picked up by the
 * source glob in SConstruct.
 */
#include "thumbnails/UsdImagingThumbnailGenerator.h"

#if defined(IDTX_ENABLE_IMAGING)

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <exception>
#include <string>
#include <system_error>

#include <pxr/pxr.h>
#include <pxr/base/gf/bbox3d.h>
#include <pxr/base/gf/camera.h>
#include <pxr/base/gf/matrix4d.h>
#include <pxr/base/gf/range3d.h>
#include <pxr/base/gf/vec3d.h>
#include <pxr/base/tf/token.h>
#include <pxr/usd/usd/editContext.h>
#include <pxr/usd/usd/prim.h>
#include <pxr/usd/usd/primRange.h>
#include <pxr/usd/usd/stage.h>
#include <pxr/usd/usd/timeCode.h>
#include <pxr/usd/usdGeom/bboxCache.h>
#include <pxr/usd/usdGeom/camera.h>
#include <pxr/usd/usdGeom/metrics.h>
#include <pxr/usd/usdGeom/tokens.h>
#include <pxr/usd/usdLux/distantLight.h>
#include <pxr/usd/usdLux/domeLight.h>
#include <pxr/usdImaging/usdAppUtils/frameRecorder.h>

#if defined(IDTX_USE_OSMESA)
#include <dlfcn.h>
#include <vector>
#elif defined(__linux__)
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <vector>
#endif

#include <idtx/utils/Logger.h>

namespace idtx
{
namespace thumbnails
{

namespace {
IDTX_LOG_CATEGORY("UsdImagingThumbnailGenerator")

void SetEnvIfUnset(const char* name, const char* value) noexcept
{
#if defined(_WIN32)
    if (std::getenv(name) == nullptr)
    {
        _putenv_s(name, value);
    }
#else
    if (std::getenv(name) == nullptr)
    {
        ::setenv(name, value, 0);
    }
#endif
}

#if defined(IDTX_USE_OSMESA)
// RAII owner of an OSMesa off-screen context loaded dynamically.
// mesa-osmesa on Wolfi ships the .so but no dev header, so the API is reached
// via dlopen/dlsym. If libOSMesa.so fails to load (e.g. because its transitive
// libgallium → libLLVM dep requires a newer glibc than the runtime provides),
// dlopen returns NULL, m_error_ is set, and Generate() falls back to the grey
// placeholder — the binary itself is not affected.
class OsMesaContext
{
public:
    explicit OsMesaContext(std::uint32_t size)
        : m_buffer_(static_cast<std::size_t>(size) * size * 4u, 0)
    {
        for (const char* name : {"libOSMesa.so.8", "libOSMesa.so"})
        {
            m_lib_ = ::dlopen(name, RTLD_NOW | RTLD_LOCAL);
            if (m_lib_) break;
        }
        if (!m_lib_) { m_error_ = ::dlerror(); return; }

        auto pfn_create  = reinterpret_cast<PFNCreate>(
            ::dlsym(m_lib_, "OSMesaCreateContextExt"));
        auto pfn_current = reinterpret_cast<PFNMakeCurrent>(
            ::dlsym(m_lib_, "OSMesaMakeCurrent"));
        m_destroy_ = reinterpret_cast<PFNDestroy>(
            ::dlsym(m_lib_, "OSMesaDestroyContext"));

        if (!pfn_create || !pfn_current || !m_destroy_)
        {
            m_error_ = "OSMesa entry points missing in libOSMesa.so";
            return;
        }

        // 0x1908 = GL_RGBA (OSMESA_RGBA),  0x1401 = GL_UNSIGNED_BYTE
        m_ctx_ = pfn_create(0x1908u, 24, 8, 0, nullptr);
        if (!m_ctx_) { m_error_ = "OSMesaCreateContextExt failed"; return; }

        m_ok_ = static_cast<bool>(
            pfn_current(m_ctx_, m_buffer_.data(), 0x1401u,
                        static_cast<int>(size), static_cast<int>(size)));
        if (!m_ok_) m_error_ = "OSMesaMakeCurrent failed";
    }

    ~OsMesaContext()
    {
        if (m_ctx_ && m_destroy_) m_destroy_(m_ctx_);
        if (m_lib_)               ::dlclose(m_lib_);
    }

    OsMesaContext(const OsMesaContext&)            = delete;
    OsMesaContext& operator=(const OsMesaContext&) = delete;

    bool        valid() const noexcept { return m_ok_; }
    const char* error() const noexcept { return m_error_; }

private:
    // fn(format, depthBits, stencilBits, accumBits, sharelist) → ctx
    using PFNCreate      = void* (*)(unsigned, int, int, int, void*);
    // fn(ctx, buffer, type, width, height) → GLboolean
    using PFNMakeCurrent = unsigned char (*)(void*, void*, unsigned, int, int);
    using PFNDestroy     = void (*)(void*);

    void*                     m_lib_     = nullptr;
    void*                     m_ctx_     = nullptr;
    bool                      m_ok_      = false;
    PFNDestroy                m_destroy_ = nullptr;
    const char*               m_error_   = nullptr;
    std::vector<std::uint8_t> m_buffer_;
};
#endif // IDTX_USE_OSMESA

#if !defined(IDTX_USE_OSMESA) && defined(__linux__)
// RAII owner of a headless EGL surfaceless PBuffer context. Creates the context
// and makes it current on construction so HgiGL's version check (which requires
// a current context) succeeds on the calling thread. Destroyed on scope exit.
struct EglContext
{
    EGLDisplay  display = EGL_NO_DISPLAY;
    EGLSurface  surface = EGL_NO_SURFACE;
    EGLContext  context = EGL_NO_CONTEXT;
    const char* error   = nullptr; // null = success

    explicit EglContext(std::uint32_t size)
    {
        // Prefer explicit platform selection via eglGetPlatformDisplayEXT so
        // the platform is unambiguous across the libglvnd dispatch layer that
        // Wolfi's Mesa uses.
#ifndef EGL_PLATFORM_SURFACELESS_MESA
#define EGL_PLATFORM_SURFACELESS_MESA 0x31DD
#endif
#ifndef EGL_PLATFORM_DEVICE_EXT
#define EGL_PLATFORM_DEVICE_EXT 0x313F
#endif
        auto pfn_get_platform = reinterpret_cast<PFNEGLGETPLATFORMDISPLAYEXTPROC>(
            eglGetProcAddress("eglGetPlatformDisplayEXT"));
        if (pfn_get_platform)
        {
            // Strategy 1: EGL_PLATFORM_SURFACELESS_MESA — a fully headless
            // off-screen context with no display server, GPU, or DRI device
            // nodes. This is the path used in the container (Mesa llvmpipe).
            display = pfn_get_platform(EGL_PLATFORM_SURFACELESS_MESA,
                                       EGL_DEFAULT_DISPLAY, nullptr);

            // Strategy 2: EGL_PLATFORM_DEVICE_EXT — enumerate a software EGL
            // device. Fallback for Mesa builds without the surfaceless
            // platform compiled in.
            if (display == EGL_NO_DISPLAY)
            {
                auto pfn_query_devices =
                    reinterpret_cast<PFNEGLQUERYDEVICESEXTPROC>(
                        eglGetProcAddress("eglQueryDevicesEXT"));
                if (pfn_query_devices)
                {
                    EGLint num_devices = 0;
                    pfn_query_devices(0, nullptr, &num_devices);
                    if (num_devices > 0)
                    {
                        std::vector<EGLDeviceEXT> devs(
                            static_cast<std::size_t>(num_devices));
                        pfn_query_devices(num_devices, devs.data(), &num_devices);
                        for (EGLint i = 0;
                             i < num_devices && display == EGL_NO_DISPLAY; ++i)
                        {
                            display = pfn_get_platform(
                                EGL_PLATFORM_DEVICE_EXT,
                                devs[static_cast<std::size_t>(i)],
                                nullptr);
                        }
                    }
                }
            }

            // Strategy 3: plain eglGetDisplay — last-resort default platform
            // (honours DISPLAY if an X server happens to be available).
            if (display == EGL_NO_DISPLAY)
                display = eglGetDisplay(EGL_DEFAULT_DISPLAY);

            if (display == EGL_NO_DISPLAY)
            {
                error = "no headless EGL display (surfaceless, device platform,"
                        " and default all failed)";
                return;
            }
        }
        else
        {
            // eglGetProcAddress returned NULL: libglvnd found no EGL vendor,
            // which on Wolfi means the 50_mesa.json ICD manifest is missing
            // from /usr/share/glvnd/egl_vendor.d/.
            display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
            if (display == EGL_NO_DISPLAY)
            {
                error = "no EGL vendor found (missing libglvnd ICD manifest"
                        " /usr/share/glvnd/egl_vendor.d/50_mesa.json?)";
                return;
            }
        }
        if (!eglInitialize(display, nullptr, nullptr))
            { error = "eglInitialize failed"; return; }
        if (!eglBindAPI(EGL_OPENGL_API))
            { error = "eglBindAPI(EGL_OPENGL_API) failed"; return; }

        const EGLint cfg_attribs[] = {
            EGL_SURFACE_TYPE,    EGL_PBUFFER_BIT,
            EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT,
            EGL_DEPTH_SIZE,      24,
            EGL_STENCIL_SIZE,    8,
            EGL_NONE,
        };
        EGLConfig config;
        EGLint    n_configs = 0;
        if (!eglChooseConfig(display, cfg_attribs, &config, 1, &n_configs)
            || n_configs == 0)
            { error = "eglChooseConfig found no EGL_OPENGL_BIT config"; return; }

        const EGLint surf_attribs[] = {
            EGL_WIDTH,  static_cast<EGLint>(size),
            EGL_HEIGHT, static_cast<EGLint>(size),
            EGL_NONE,
        };
        surface = eglCreatePbufferSurface(display, config, surf_attribs);
        if (surface == EGL_NO_SURFACE)
            { error = "eglCreatePbufferSurface failed"; return; }

        const EGLint ctx_attribs[] = {
            EGL_CONTEXT_MAJOR_VERSION,       4,
            EGL_CONTEXT_MINOR_VERSION,       5,
            EGL_CONTEXT_OPENGL_PROFILE_MASK, EGL_CONTEXT_OPENGL_COMPATIBILITY_PROFILE_BIT,
            EGL_NONE,
        };
        context = eglCreateContext(display, config, EGL_NO_CONTEXT, ctx_attribs);
        if (context == EGL_NO_CONTEXT)
            { error = "eglCreateContext(GL 4.5 compat) failed"; return; }

        if (!eglMakeCurrent(display, surface, surface, context))
            { error = "eglMakeCurrent failed"; return; }
    }

    ~EglContext()
    {
        if (display != EGL_NO_DISPLAY)
        {
            eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
            if (context != EGL_NO_CONTEXT) eglDestroyContext(display, context);
            if (surface != EGL_NO_SURFACE) eglDestroySurface(display, surface);
            eglTerminate(display);
        }
    }

    EglContext(const EglContext&)            = delete;
    EglContext& operator=(const EglContext&) = delete;

    bool valid() const noexcept { return error == nullptr; }
};
#endif // !IDTX_USE_OSMESA && __linux__

// Ensure the process is configured to use the Mesa llvmpipe software
// rasteriser. Idempotent; only sets each var if it is not already present.
void EnsureSoftwareGlEnv() noexcept
{
    SetEnvIfUnset("LIBGL_ALWAYS_SOFTWARE", "1");
    SetEnvIfUnset("GALLIUM_DRIVER", "llvmpipe");
    // llvmpipe advertises a modest GL version by default; Storm wants 4.x.
    SetEnvIfUnset("MESA_GL_VERSION_OVERRIDE", "4.5");
    SetEnvIfUnset("MESA_GLSL_VERSION_OVERRIDE", "450");
}

// Return the first authored UsdGeomCamera in the stage, or an invalid prim.
pxr::UsdPrim FindAuthoredCamera(const pxr::UsdStageRefPtr& stage)
{
    for (const pxr::UsdPrim& prim : stage->Traverse())
    {
        if (prim.IsA<pxr::UsdGeomCamera>())
        {
            return prim;
        }
    }
    return pxr::UsdPrim();
}

// World-space bounding box of the imageable content (default+render purposes).
pxr::GfRange3d ComputeStageBounds(const pxr::UsdStageRefPtr& stage)
{
    const pxr::TfTokenVector purposes = {
        pxr::UsdGeomTokens->default_,
        pxr::UsdGeomTokens->render,
    };
    pxr::UsdGeomBBoxCache cache(pxr::UsdTimeCode::Default(), purposes, true);
    const pxr::GfBBox3d world = cache.ComputeWorldBound(stage->GetPseudoRoot());
    return world.ComputeAlignedRange();
}

// Author a synthetic perspective preview camera into the stage session layer,
// framing the bounds from 45 degrees above the content. Falls back to a unit
// sphere frame when the bounds are empty/degenerate.
pxr::UsdGeomCamera AuthorSyntheticCamera(const pxr::UsdStageRefPtr& stage,
                                         const pxr::GfRange3d& bounds)
{
    pxr::GfVec3d center(0.0);
    double       radius = 1.0;
    if (!bounds.IsEmpty())
    {
        center = bounds.GetMidpoint();
        radius = bounds.GetSize().GetLength() * 0.5;
        if (radius <= 0.0) radius = 1.0;
    }

    constexpr double kFovYDeg = 45.0;
    constexpr double kFit     = 1.15;
    const double     fovYRad  = kFovYDeg * (M_PI / 180.0);
    const double     dist     = (radius * kFit) / std::tan(fovYRad * 0.5);

    // Direction from target toward eye: 45 degrees of elevation above the
    // content, honouring the stage up-axis.
    const pxr::TfToken up = pxr::UsdGeomGetStageUpAxis(stage);
    pxr::GfVec3d       dir;
    pxr::GfVec3d       upVec;
    if (up == pxr::UsdGeomTokens->z)
    {
        dir   = pxr::GfVec3d(1.0, -1.0, 1.0).GetNormalized();
        upVec = pxr::GfVec3d(0.0, 0.0, 1.0);
    }
    else
    {
        dir   = pxr::GfVec3d(1.0, 1.0, 1.0).GetNormalized();
        upVec = pxr::GfVec3d(0.0, 1.0, 0.0);
    }

    const pxr::GfVec3d eye = center + dir * dist;

    pxr::GfCamera gf;
    gf.SetProjection(pxr::GfCamera::Perspective);
    gf.SetPerspectiveFromAspectRatioAndFieldOfView(
        1.0f, static_cast<float>(kFovYDeg), pxr::GfCamera::FOVVertical);
    const float nearC = static_cast<float>(std::max(0.001, dist - radius * 2.0));
    const float farC  = static_cast<float>(dist + radius * 2.0);
    gf.SetClippingRange(pxr::GfRange1f(nearC, farC));
    gf.SetFocusDistance(static_cast<float>(dist));

    pxr::GfMatrix4d view(1.0);
    view.SetLookAt(eye, center, upVec);
    gf.SetTransform(view.GetInverse());

    // Author into the session layer so the on-disk file is never modified.
    pxr::UsdEditContext editCtx(stage, stage->GetSessionLayer());
    pxr::UsdGeomCamera  cam =
        pxr::UsdGeomCamera::Define(stage, pxr::SdfPath("/__idtx_preview_cam"));
    cam.SetFromCamera(gf, pxr::UsdTimeCode::Default());

    // Add a distant headlight sharing the camera's transform so scenes with no
    // authored lights still render with visible geometry.
    pxr::UsdLuxDistantLight headlight = pxr::UsdLuxDistantLight::Define(
        stage, pxr::SdfPath("/__idtx_preview_headlight"));
    headlight.CreateIntensityAttr().Set(5000.0f);
    pxr::UsdGeomXformable(headlight).MakeMatrixXform().Set(
        view.GetInverse(), pxr::UsdTimeCode::Default());

    // Add a dome light for ambient/image-based fill. A single distant light
    // only lights surfaces whose normals face it, leaving everything else pure
    // black; the dome ensures curved and back-facing geometry (spheres,
    // cylinders, cones) still show their colour instead of rendering black.
    pxr::UsdLuxDomeLight dome = pxr::UsdLuxDomeLight::Define(
        stage, pxr::SdfPath("/__idtx_preview_dome"));
    dome.CreateIntensityAttr().Set(1000.0f);

    return cam;
}

} // namespace

bool UsdImagingThumbnailGenerator::Generate(const std::filesystem::path& usd_file,
                                            const std::filesystem::path& out_path,
                                            std::string& out_error)
{
    // Any failure path below delegates here so callers always get a thumbnail.
    auto fall_back = [&](const std::string& why) -> bool {
        IDTX_LOG(IDTX_WARN,
                 "Hydra render failed for '{}': {} - using placeholder",
                 usd_file.string(), why);
        std::string placeholder_err;
        if (!m_fallback_.Generate(usd_file, out_path, placeholder_err))
        {
            out_error = "render failed (" + why + ") and placeholder failed ("
                        + placeholder_err + ")";
            return false;
        }
        return true;
    };

    try
    {
        EnsureSoftwareGlEnv();

#if defined(IDTX_USE_OSMESA)
        OsMesaContext gl(m_size_);
        if (!gl.valid())
        {
            const char* e = gl.error();
            return fall_back(std::string("OSMesa: ") + (e ? e : "unknown error"));
        }
#elif defined(__linux__)
        EglContext egl_ctx(m_size_);
        if (!egl_ctx.valid())
        {
            return fall_back(std::string("EGL init: ") + egl_ctx.error);
        }
#endif

        pxr::UsdStageRefPtr stage = pxr::UsdStage::Open(usd_file.string());
        if (!stage)
        {
            return fall_back("stage could not be opened");
        }

        // Prefer an authored camera; otherwise synthesise one in the session
        // layer framed to the stage extent.
        pxr::UsdGeomCamera camera;
        if (const pxr::UsdPrim authored = FindAuthoredCamera(stage))
        {
            camera = pxr::UsdGeomCamera(authored);
        }
        else
        {
            camera = AuthorSyntheticCamera(stage, ComputeStageBounds(stage));
        }
        if (!camera)
        {
            return fall_back("no usable camera");
        }

        // Render with Storm to a temp file, then move into place atomically.
        // Preserve the original extension (e.g. ".png") so HioImage can pick the
        // right write plugin. concat(".tmp") would give "file.png.tmp" whose
        // extension is ".tmp" — no plugin handles that → write fails.
        const std::filesystem::path tmp_path =
            out_path.parent_path() /
            (out_path.stem().string() + ".tmp" + out_path.extension().string());

        pxr::UsdAppUtilsFrameRecorder recorder;
        recorder.SetRendererPlugin(pxr::TfToken("HdStormRendererPlugin"));
        recorder.SetImageWidth(m_size_);
        recorder.SetComplexity(1.0f);
        recorder.SetCameraLightEnabled(true);

        const bool recorded = recorder.Record(stage, camera,
                                               pxr::UsdTimeCode::Default(),
                                               tmp_path.string());

        std::error_code ec;
        if (!recorded || !std::filesystem::exists(tmp_path, ec)
            || std::filesystem::file_size(tmp_path, ec) == 0)
        {
            std::filesystem::remove(tmp_path, ec);
            return fall_back("frame recorder produced no image");
        }

        std::filesystem::rename(tmp_path, out_path, ec);
        if (ec)
        {
            // Fall back to a copy if rename across devices fails.
            std::filesystem::copy_file(
                tmp_path, out_path,
                std::filesystem::copy_options::overwrite_existing, ec);
            std::error_code ec2;
            std::filesystem::remove(tmp_path, ec2);
            if (ec)
            {
                return fall_back("could not move rendered image into place");
            }
        }

        IDTX_LOG(IDTX_INFO, "Rendered thumbnail for '{}'", usd_file.string());
        return true;
    }
    catch (const std::exception& e)
    {
        return fall_back(e.what());
    }
    catch (...)
    {
        return fall_back("unknown exception");
    }
}

} // namespace thumbnails
} // namespace idtx

#endif // IDTX_ENABLE_IMAGING
