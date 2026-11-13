/**
 * @file llvrbackend_openxr.cpp
 * @brief SLVR: OpenXR implementation of LLVRBackend (OpenGL via XR_KHR_opengl_enable, Windows/WGL).
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * SLVR, based on the P373R VR Mod. LGPL 2.1, see LICENSE.
 * $/LicenseInfo$
 */

#include "llviewerprecompiledheaders.h"

#include "llvrbackend.h"

#include "llgl.h"
#include "llviewercontrol.h"
#include "llwin32headers.h"

#define XR_USE_PLATFORM_WIN32
#define XR_USE_GRAPHICS_API_OPENGL
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <vector>

// shortcut: Windows/WGL only and a single session (file-static state); add other
// platform bindings here if SLVR is ever built for Linux or macOS.
namespace
{
    struct EyeTarget
    {
        XrSwapchain swapchain = XR_NULL_HANDLE;
        int32_t width = 0;
        int32_t height = 0;
        std::vector<XrSwapchainImageOpenGLKHR> images;
    };

    XrInstance sInstance = XR_NULL_HANDLE;
    XrSystemId sSystemId = XR_NULL_SYSTEM_ID;
    XrSession sSession = XR_NULL_HANDLE;
    XrSpace sSpace = XR_NULL_HANDLE;
    XrEnvironmentBlendMode sBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
    EyeTarget sTargets[2];
    GLuint sFBO = 0;

    bool sAlive = false;      // false after the runtime ends or loses the session
    bool sRunning = false;    // between xrBeginSession and xrEndSession
    bool sFrameBegun = false; // xrBeginFrame was called and xrEndFrame is owed
    bool sEyeDone[2] = { false, false };
    XrTime sDisplayTime = 0;
    XrView sViews[2];
    XrCompositionLayerProjectionView sLayerViews[2];

    // Statistics for the periodic log line.
    using Clock = std::chrono::steady_clock;
    Clock::time_point sStatsStart;
    U32 sStatsFrames = 0;
    U32 sStatsStereoFrames = 0;
    double sStatsWaitMs = 0.0;
    U32 sErrorCount = 0;

    const F32 STATS_INTERVAL_SECONDS = 5.f;

    std::string resultName(XrResult result)
    {
        char name[XR_MAX_RESULT_STRING_SIZE] = "";
        if (sInstance == XR_NULL_HANDLE || XR_FAILED(xrResultToString(sInstance, result, name)))
        {
            return llformat("XrResult %d", (S32)result);
        }
        return llformat("%s (%d)", name, (S32)result);
    }

    // Every OpenXR call goes through here: failures are always logged with the call name
    // and the XrResult. Per-frame failures are rate limited so they cannot flood the log.
    bool xrOk(XrResult result, const char* call)
    {
        if (XR_SUCCEEDED(result))
        {
            return true;
        }
        if (result == XR_ERROR_SESSION_LOST || result == XR_ERROR_INSTANCE_LOST)
        {
            sAlive = false;
        }
        ++sErrorCount;
        if (sErrorCount <= 50 || sErrorCount % 500 == 0)
        {
            LL_WARNS("VR") << call << " failed: " << resultName(result)
                           << " (error #" << sErrorCount << ")" << LL_ENDL;
        }
        return false;
    }

    const char* sessionStateName(XrSessionState state)
    {
        static const char* const NAMES[] = { "UNKNOWN", "IDLE", "READY", "SYNCHRONIZED", "VISIBLE",
                                             "FOCUSED", "STOPPING", "LOSS_PENDING", "EXITING" };
        return (state >= 0 && state < (S32)LL_ARRAY_SIZE(NAMES)) ? NAMES[state] : "?";
    }

    void pollEvents()
    {
        XrEventDataBuffer event{ XR_TYPE_EVENT_DATA_BUFFER };
        while (xrPollEvent(sInstance, &event) == XR_SUCCESS)
        {
            switch (event.type)
            {
            case XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED:
            {
                const XrSessionState state = reinterpret_cast<XrEventDataSessionStateChanged*>(&event)->state;
                LL_INFOS("VR") << "Session state: " << sessionStateName(state) << LL_ENDL;
                if (state == XR_SESSION_STATE_READY)
                {
                    XrSessionBeginInfo begin_info{ XR_TYPE_SESSION_BEGIN_INFO };
                    begin_info.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
                    sRunning = xrOk(xrBeginSession(sSession, &begin_info), "xrBeginSession");
                }
                else if (state == XR_SESSION_STATE_STOPPING)
                {
                    sRunning = false;
                    xrOk(xrEndSession(sSession), "xrEndSession");
                }
                else if (state == XR_SESSION_STATE_EXITING || state == XR_SESSION_STATE_LOSS_PENDING)
                {
                    sAlive = false;
                }
                break;
            }
            case XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING:
                LL_WARNS("VR") << "Runtime announced instance loss" << LL_ENDL;
                sAlive = false;
                break;
            default:
                LL_INFOS("VR") << "Event type " << (S32)event.type << LL_ENDL;
                break;
            }
            event = { XR_TYPE_EVENT_DATA_BUFFER };
        }
    }

    // The viewer's back buffer holds display-ready (gamma encoded) colour and is copied
    // unmodified, so an sRGB image is the format the compositor will read correctly.
    // A linear format (RGBA8/RGBA16F) would be gamma-corrected a second time: washed out.
    int64_t chooseFormat(const std::vector<int64_t>& offered)
    {
        const S32 forced = gSavedSettings.getS32("VRSwapchainFormat");
        const int64_t preferred[] = { forced, GL_SRGB8_ALPHA8, GL_RGBA8 };
        for (int64_t want : preferred)
        {
            if (want != 0 && std::find(offered.begin(), offered.end(), want) != offered.end())
            {
                return want;
            }
        }
        return offered.front();
    }

    bool createEyeTarget(EyeTarget& target, const XrViewConfigurationView& view, int64_t format)
    {
        XrSwapchainCreateInfo info{ XR_TYPE_SWAPCHAIN_CREATE_INFO };
        info.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
        info.format = format;
        info.sampleCount = 1;
        info.width = view.recommendedImageRectWidth;
        info.height = view.recommendedImageRectHeight;
        info.faceCount = 1;
        info.arraySize = 1;
        info.mipCount = 1;
        if (!xrOk(xrCreateSwapchain(sSession, &info, &target.swapchain), "xrCreateSwapchain"))
        {
            return false;
        }
        target.width = (int32_t)info.width;
        target.height = (int32_t)info.height;

        uint32_t count = 0;
        if (!xrOk(xrEnumerateSwapchainImages(target.swapchain, 0, &count, nullptr), "xrEnumerateSwapchainImages(count)")
            || count == 0)
        {
            return false;
        }
        target.images.assign(count, { XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_KHR });
        return xrOk(xrEnumerateSwapchainImages(target.swapchain, count, &count,
                                               reinterpret_cast<XrSwapchainImageBaseHeader*>(target.images.data())),
                    "xrEnumerateSwapchainImages");
    }

    void logStats()
    {
        const F32 seconds = std::chrono::duration<F32>(Clock::now() - sStatsStart).count();
        if (seconds < STATS_INTERVAL_SECONDS || sStatsFrames == 0)
        {
            return;
        }
        const XrPosef& pose = sViews[0].pose;
        const XrFovf& fov = sViews[0].fov;
        LL_INFOS("VR") << llformat("%.1f fps, %.1f ms/frame, %.1f ms in xrWaitFrame, %u of %u frames stereo",
                                   sStatsFrames / seconds, 1000.f * seconds / sStatsFrames,
                                   sStatsWaitMs / sStatsFrames, sStatsStereoFrames, sStatsFrames)
                       << llformat("; left eye pos (%.3f %.3f %.3f) rot (%.3f %.3f %.3f %.3f) fov deg L%.1f R%.1f U%.1f D%.1f",
                                   pose.position.x, pose.position.y, pose.position.z,
                                   pose.orientation.x, pose.orientation.y, pose.orientation.z, pose.orientation.w,
                                   fov.angleLeft * RAD_TO_DEG, fov.angleRight * RAD_TO_DEG,
                                   fov.angleUp * RAD_TO_DEG, fov.angleDown * RAD_TO_DEG)
                       << LL_ENDL;
        sStatsStart = Clock::now();
        sStatsFrames = sStatsStereoFrames = 0;
        sStatsWaitMs = 0.0;
    }
}

bool LLVRBackend::isInitialized() const
{
    return sSession != XR_NULL_HANDLE;
}

bool LLVRBackend::isAlive() const
{
    return sAlive;
}

bool LLVRBackend::init(std::string& error)
{
    // Any failure below leaves a partly built session behind; shutdown() releases it.
    auto fail = [&](const std::string& reason)
    {
        error = reason;
        LL_WARNS("VR") << "OpenXR init failed: " << reason << LL_ENDL;
        shutdown();
        return false;
    };

    sErrorCount = 0;

    // --- Instance ---
    uint32_t ext_count = 0;
    if (!xrOk(xrEnumerateInstanceExtensionProperties(nullptr, 0, &ext_count, nullptr),
              "xrEnumerateInstanceExtensionProperties(count)"))
    {
        return fail("No OpenXR runtime found. Set an active OpenXR runtime (e.g. VDXR in Virtual Desktop Streamer).");
    }
    std::vector<XrExtensionProperties> extensions(ext_count, { XR_TYPE_EXTENSION_PROPERTIES });
    if (!xrOk(xrEnumerateInstanceExtensionProperties(nullptr, ext_count, &ext_count, extensions.data()),
              "xrEnumerateInstanceExtensionProperties"))
    {
        return fail("Could not list the OpenXR runtime's extensions.");
    }
    bool has_opengl = false;
    std::string ext_list;
    for (const XrExtensionProperties& ext : extensions)
    {
        ext_list += std::string(" ") + ext.extensionName;
        has_opengl = has_opengl || strcmp(ext.extensionName, XR_KHR_OPENGL_ENABLE_EXTENSION_NAME) == 0;
    }
    LL_INFOS("VR") << "Runtime extensions:" << ext_list << LL_ENDL;
    if (!has_opengl)
    {
        return fail("The active OpenXR runtime does not support OpenGL applications (XR_KHR_opengl_enable).");
    }

    const char* const enabled_extensions[] = { XR_KHR_OPENGL_ENABLE_EXTENSION_NAME };
    XrInstanceCreateInfo instance_info{ XR_TYPE_INSTANCE_CREATE_INFO };
    snprintf(instance_info.applicationInfo.applicationName, XR_MAX_APPLICATION_NAME_SIZE, "SLVR");
    instance_info.applicationInfo.applicationVersion = 1;
    instance_info.applicationInfo.apiVersion = XR_MAKE_VERSION(1, 0, 0);
    instance_info.enabledExtensionCount = 1;
    instance_info.enabledExtensionNames = enabled_extensions;
    if (!xrOk(xrCreateInstance(&instance_info, &sInstance), "xrCreateInstance"))
    {
        return fail("Could not connect to the OpenXR runtime (xrCreateInstance).");
    }

    XrInstanceProperties instance_props{ XR_TYPE_INSTANCE_PROPERTIES };
    if (xrOk(xrGetInstanceProperties(sInstance, &instance_props), "xrGetInstanceProperties"))
    {
        LL_INFOS("VR") << "Runtime: " << instance_props.runtimeName << " "
                       << XR_VERSION_MAJOR(instance_props.runtimeVersion) << "."
                       << XR_VERSION_MINOR(instance_props.runtimeVersion) << "."
                       << XR_VERSION_PATCH(instance_props.runtimeVersion) << LL_ENDL;
    }

    // --- System (the headset) ---
    XrSystemGetInfo system_info{ XR_TYPE_SYSTEM_GET_INFO };
    system_info.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
    if (!xrOk(xrGetSystem(sInstance, &system_info, &sSystemId), "xrGetSystem"))
    {
        return fail("No headset found. Connect the headset (start Virtual Desktop) and try again.");
    }
    XrSystemProperties system_props{ XR_TYPE_SYSTEM_PROPERTIES };
    if (xrOk(xrGetSystemProperties(sInstance, sSystemId, &system_props), "xrGetSystemProperties"))
    {
        LL_INFOS("VR") << "System: " << system_props.systemName << ", max swapchain "
                       << system_props.graphicsProperties.maxSwapchainImageWidth << "x"
                       << system_props.graphicsProperties.maxSwapchainImageHeight << LL_ENDL;
    }

    // --- Graphics requirements: the runtime requires this call before xrCreateSession ---
    PFN_xrGetOpenGLGraphicsRequirementsKHR get_requirements = nullptr;
    if (!xrOk(xrGetInstanceProcAddr(sInstance, "xrGetOpenGLGraphicsRequirementsKHR",
                                    reinterpret_cast<PFN_xrVoidFunction*>(&get_requirements)),
              "xrGetInstanceProcAddr(xrGetOpenGLGraphicsRequirementsKHR)")
        || !get_requirements)
    {
        return fail("The OpenXR runtime did not provide its OpenGL entry point.");
    }
    XrGraphicsRequirementsOpenGLKHR requirements{ XR_TYPE_GRAPHICS_REQUIREMENTS_OPENGL_KHR };
    if (!xrOk(get_requirements(sInstance, sSystemId, &requirements), "xrGetOpenGLGraphicsRequirementsKHR"))
    {
        return fail("Could not query the runtime's OpenGL requirements.");
    }
    GLint gl_major = 0, gl_minor = 0;
    glGetIntegerv(GL_MAJOR_VERSION, &gl_major);
    glGetIntegerv(GL_MINOR_VERSION, &gl_minor);
    LL_INFOS("VR") << "OpenGL context " << gl_major << "." << gl_minor
                   << " (" << (const char*)glGetString(GL_RENDERER) << "); runtime requires "
                   << XR_VERSION_MAJOR(requirements.minApiVersionSupported) << "."
                   << XR_VERSION_MINOR(requirements.minApiVersionSupported) << " to "
                   << XR_VERSION_MAJOR(requirements.maxApiVersionSupported) << "."
                   << XR_VERSION_MINOR(requirements.maxApiVersionSupported) << LL_ENDL;
    if (XR_MAKE_VERSION(gl_major, gl_minor, 0) < requirements.minApiVersionSupported)
    {
        LL_WARNS("VR") << "OpenGL context is older than the runtime's minimum; session creation may fail" << LL_ENDL;
    }

    // --- Session on the viewer's own GL context ---
    XrGraphicsBindingOpenGLWin32KHR binding{ XR_TYPE_GRAPHICS_BINDING_OPENGL_WIN32_KHR };
    binding.hDC = wglGetCurrentDC();
    binding.hGLRC = wglGetCurrentContext();
    if (!binding.hDC || !binding.hGLRC)
    {
        return fail("No current OpenGL context on the render thread.");
    }
    XrSessionCreateInfo session_info{ XR_TYPE_SESSION_CREATE_INFO };
    session_info.next = &binding;
    session_info.systemId = sSystemId;
    const XrResult session_result = xrCreateSession(sInstance, &session_info, &sSession);
    if (!xrOk(session_result, "xrCreateSession"))
    {
        sSession = XR_NULL_HANDLE;
        return fail(session_result == XR_ERROR_GRAPHICS_DEVICE_INVALID
                    ? "The viewer is rendering on a different GPU than the headset uses. Run SLVR on the same GPU as the VR runtime."
                    : "Could not create the VR session (" + resultName(session_result) + ").");
    }
    sAlive = true;

    // shortcut: seated/LOCAL space only; add STAGE (room scale, floor level) with a setting when needed.
    XrReferenceSpaceCreateInfo space_info{ XR_TYPE_REFERENCE_SPACE_CREATE_INFO };
    space_info.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
    space_info.poseInReferenceSpace.orientation.w = 1.f;
    if (!xrOk(xrCreateReferenceSpace(sSession, &space_info, &sSpace), "xrCreateReferenceSpace(LOCAL)"))
    {
        return fail("Could not create the tracking space.");
    }

    uint32_t blend_count = 0;
    XrEnvironmentBlendMode blend_modes[8];
    if (xrOk(xrEnumerateEnvironmentBlendModes(sInstance, sSystemId, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,
                                              LL_ARRAY_SIZE(blend_modes), &blend_count, blend_modes),
             "xrEnumerateEnvironmentBlendModes")
        && blend_count > 0)
    {
        sBlendMode = blend_modes[0];
    }

    // --- Per-eye images ---
    uint32_t view_count = 0;
    XrViewConfigurationView config_views[2] = { { XR_TYPE_VIEW_CONFIGURATION_VIEW }, { XR_TYPE_VIEW_CONFIGURATION_VIEW } };
    if (!xrOk(xrEnumerateViewConfigurationViews(sInstance, sSystemId, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,
                                                2, &view_count, config_views),
              "xrEnumerateViewConfigurationViews")
        || view_count != 2)
    {
        return fail("The headset does not offer a stereo view configuration.");
    }

    uint32_t format_count = 0;
    if (!xrOk(xrEnumerateSwapchainFormats(sSession, 0, &format_count, nullptr), "xrEnumerateSwapchainFormats(count)")
        || format_count == 0)
    {
        return fail("The runtime offers no swapchain formats.");
    }
    std::vector<int64_t> formats(format_count);
    if (!xrOk(xrEnumerateSwapchainFormats(sSession, format_count, &format_count, formats.data()),
              "xrEnumerateSwapchainFormats"))
    {
        return fail("Could not list the runtime's swapchain formats.");
    }
    const int64_t format = chooseFormat(formats);
    std::string format_list;
    for (int64_t offered : formats)
    {
        format_list += llformat(" 0x%04X", (U32)offered);
    }
    LL_INFOS("VR") << "Swapchain formats offered:" << format_list
                   << llformat("; using 0x%04X", (U32)format)
                   << (format == GL_SRGB8_ALPHA8 ? " (GL_SRGB8_ALPHA8)" : format == GL_RGBA8 ? " (GL_RGBA8)" : "")
                   << llformat("; blend mode %d", (S32)sBlendMode) << LL_ENDL;

    for (S32 eye = 0; eye < 2; ++eye)
    {
        LL_INFOS("VR") << "Eye " << eye << " image: recommended "
                       << config_views[eye].recommendedImageRectWidth << "x" << config_views[eye].recommendedImageRectHeight
                       << ", max " << config_views[eye].maxImageRectWidth << "x" << config_views[eye].maxImageRectHeight
                       << ", recommended samples " << config_views[eye].recommendedSwapchainSampleCount << LL_ENDL;
        if (!createEyeTarget(sTargets[eye], config_views[eye], format))
        {
            return fail("Could not create the eye images (swapchain).");
        }
        LL_INFOS("VR") << "Eye " << eye << " swapchain: " << sTargets[eye].images.size() << " images" << LL_ENDL;
    }

    glGenFramebuffers(1, &sFBO);

    sStatsStart = Clock::now();
    sStatsFrames = sStatsStereoFrames = 0;
    sStatsWaitMs = 0.0;
    LL_INFOS("VR") << "OpenXR session created" << LL_ENDL;
    return true;
}

void LLVRBackend::shutdown()
{
    if (sInstance == XR_NULL_HANDLE)
    {
        return;
    }
    // Children are destroyed with their parent, but destroy in order so failures are logged.
    for (EyeTarget& target : sTargets)
    {
        if (target.swapchain != XR_NULL_HANDLE)
        {
            xrOk(xrDestroySwapchain(target.swapchain), "xrDestroySwapchain");
        }
        target = EyeTarget();
    }
    if (sFBO)
    {
        glDeleteFramebuffers(1, &sFBO);
        sFBO = 0;
    }
    if (sSpace != XR_NULL_HANDLE)
    {
        xrOk(xrDestroySpace(sSpace), "xrDestroySpace");
        sSpace = XR_NULL_HANDLE;
    }
    if (sSession != XR_NULL_HANDLE)
    {
        xrOk(xrDestroySession(sSession), "xrDestroySession");
        sSession = XR_NULL_HANDLE;
    }
    xrDestroyInstance(sInstance);
    sInstance = XR_NULL_HANDLE;
    sSystemId = XR_NULL_SYSTEM_ID;
    sAlive = sRunning = sFrameBegun = false;
    LL_INFOS("VR") << "OpenXR shut down" << LL_ENDL;
}

bool LLVRBackend::beginFrame(LLVREye eyes[2])
{
    if (!isInitialized())
    {
        return false;
    }
    if (sFrameBegun)
    {
        endFrame(); // the caller abandoned the previous frame; the runtime still needs its xrEndFrame
    }
    pollEvents();
    if (!sAlive || !sRunning)
    {
        return false;
    }

    XrFrameState frame_state{ XR_TYPE_FRAME_STATE };
    const Clock::time_point wait_start = Clock::now();
    if (!xrOk(xrWaitFrame(sSession, nullptr, &frame_state), "xrWaitFrame"))
    {
        return false;
    }
    sStatsWaitMs += std::chrono::duration<double, std::milli>(Clock::now() - wait_start).count();
    if (!xrOk(xrBeginFrame(sSession, nullptr), "xrBeginFrame"))
    {
        return false;
    }
    sFrameBegun = true;
    sDisplayTime = frame_state.predictedDisplayTime;
    sEyeDone[0] = sEyeDone[1] = false;
    if (!frame_state.shouldRender)
    {
        return false; // endFrame() will submit no layers
    }

    XrViewLocateInfo locate_info{ XR_TYPE_VIEW_LOCATE_INFO };
    locate_info.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
    locate_info.displayTime = sDisplayTime;
    locate_info.space = sSpace;
    XrViewState view_state{ XR_TYPE_VIEW_STATE };
    uint32_t view_count = 0;
    sViews[0] = sViews[1] = { XR_TYPE_VIEW };
    if (!xrOk(xrLocateViews(sSession, &locate_info, &view_state, 2, &view_count, sViews), "xrLocateViews")
        || view_count != 2)
    {
        return false;
    }
    const XrViewStateFlags needed = XR_VIEW_STATE_POSITION_VALID_BIT | XR_VIEW_STATE_ORIENTATION_VALID_BIT;
    if ((view_state.viewStateFlags & needed) != needed)
    {
        LL_WARNS_ONCE("VR") << "Head pose not valid (tracking lost?); frame skipped" << LL_ENDL;
        return false;
    }

    for (S32 i = 0; i < 2; ++i)
    {
        const XrPosef& pose = sViews[i].pose;
        const XrFovf& fov = sViews[i].fov;
        eyes[i] = { { pose.position.x, pose.position.y, pose.position.z },
                    { pose.orientation.x, pose.orientation.y, pose.orientation.z, pose.orientation.w },
                    { tanf(fov.angleLeft), tanf(fov.angleRight), tanf(fov.angleUp), tanf(fov.angleDown) } };
    }
    return true;
}

bool LLVRBackend::submitEye(int eye, int x, int y, int width, int height)
{
    if (!sFrameBegun || eye < 0 || eye > 1 || width <= 0 || height <= 0)
    {
        return false;
    }
    EyeTarget& target = sTargets[eye];

    uint32_t index = 0;
    if (!xrOk(xrAcquireSwapchainImage(target.swapchain, nullptr, &index), "xrAcquireSwapchainImage"))
    {
        return false;
    }
    XrSwapchainImageWaitInfo wait_info{ XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO };
    wait_info.timeout = XR_INFINITE_DURATION;
    bool ok = xrOk(xrWaitSwapchainImage(target.swapchain, &wait_info), "xrWaitSwapchainImage");
    if (ok)
    {
        // Raw copy: with GL_FRAMEBUFFER_SRGB on, the blit would re-encode into an sRGB image.
        // The scissor test also clips blits, so both are switched off for the copy.
        const GLboolean srgb_was_on = glIsEnabled(GL_FRAMEBUFFER_SRGB);
        const GLboolean scissor_was_on = glIsEnabled(GL_SCISSOR_TEST);
        glDisable(GL_FRAMEBUFFER_SRGB);
        glDisable(GL_SCISSOR_TEST);

        glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
        glReadBuffer(GL_BACK);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, sFBO);
        glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, target.images[index].image, 0);
        const GLenum status = glCheckFramebufferStatus(GL_DRAW_FRAMEBUFFER);
        if (status == GL_FRAMEBUFFER_COMPLETE)
        {
            // shortcut: scales the window image to the eye image, so sharpness is limited by the
            // window size; render straight into the swapchain image when quality matters.
            glBlitFramebuffer(x, y, x + width, y + height, 0, 0, target.width, target.height,
                              GL_COLOR_BUFFER_BIT, GL_LINEAR);
        }
        else
        {
            ok = false;
            LL_WARNS_ONCE("VR") << llformat("Eye framebuffer incomplete: 0x%04X", status) << LL_ENDL;
        }
        glBindFramebuffer(GL_FRAMEBUFFER, 0);

        if (srgb_was_on) glEnable(GL_FRAMEBUFFER_SRGB);
        if (scissor_was_on) glEnable(GL_SCISSOR_TEST);
    }
    // Release even after a failed wait or copy, so the swapchain is not left acquired.
    ok = xrOk(xrReleaseSwapchainImage(target.swapchain, nullptr), "xrReleaseSwapchainImage") && ok;

    sLayerViews[eye] = { XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW };
    sLayerViews[eye].pose = sViews[eye].pose;
    sLayerViews[eye].fov = sViews[eye].fov;
    sLayerViews[eye].subImage.swapchain = target.swapchain;
    sLayerViews[eye].subImage.imageRect.extent = { target.width, target.height };
    sEyeDone[eye] = ok;
    return ok;
}

void LLVRBackend::endFrame()
{
    if (!sFrameBegun)
    {
        return;
    }
    sFrameBegun = false;

    XrCompositionLayerProjection layer{ XR_TYPE_COMPOSITION_LAYER_PROJECTION };
    layer.space = sSpace;
    layer.viewCount = 2;
    layer.views = sLayerViews;
    const XrCompositionLayerBaseHeader* const layers[] = { reinterpret_cast<XrCompositionLayerBaseHeader*>(&layer) };

    // Without both eyes (runtime said not to render, or a copy failed) submit an empty frame.
    const bool stereo = sEyeDone[0] && sEyeDone[1];
    XrFrameEndInfo end_info{ XR_TYPE_FRAME_END_INFO };
    end_info.displayTime = sDisplayTime;
    end_info.environmentBlendMode = sBlendMode;
    end_info.layerCount = stereo ? 1 : 0;
    end_info.layers = stereo ? layers : nullptr;
    xrOk(xrEndFrame(sSession, &end_info), "xrEndFrame");

    ++sStatsFrames;
    if (stereo)
    {
        ++sStatsStereoFrames;
    }
    logStats();
}
