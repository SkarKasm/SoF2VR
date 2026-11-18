#include "openxr_core.h"
#include "proxy_dll.h"
#include <GL/glext.h>
#include <stdio.h>
#include <string.h>

// OpenXR function pointers (loaded dynamically for 32-bit compatibility)
static PFN_xrCreateInstance pfn_xrCreateInstance = NULL;
static PFN_xrGetInstanceProcAddr pfn_xrGetInstanceProcAddr = NULL;
static PFN_xrDestroyInstance pfn_xrDestroyInstance = NULL;
static PFN_xrGetSystem pfn_xrGetSystem = NULL;
static PFN_xrCreateSession pfn_xrCreateSession = NULL;
static PFN_xrDestroySession pfn_xrDestroySession = NULL;
static PFN_xrBeginSession pfn_xrBeginSession = NULL;
static PFN_xrEndSession pfn_xrEndSession = NULL;
static PFN_xrWaitFrame pfn_xrWaitFrame = NULL;
static PFN_xrBeginFrame pfn_xrBeginFrame = NULL;
static PFN_xrEndFrame pfn_xrEndFrame = NULL;
static PFN_xrLocateViews pfn_xrLocateViews = NULL;
static PFN_xrCreateReferenceSpace pfn_xrCreateReferenceSpace = NULL;
static PFN_xrCreateSwapchain pfn_xrCreateSwapchain = NULL;
static PFN_xrDestroySwapchain pfn_xrDestroySwapchain = NULL;
static PFN_xrEnumerateSwapchainImages pfn_xrEnumerateSwapchainImages = NULL;
PFN_xrAcquireSwapchainImage pfn_xrAcquireSwapchainImage = NULL;
PFN_xrWaitSwapchainImage pfn_xrWaitSwapchainImage = NULL;
PFN_xrReleaseSwapchainImage pfn_xrReleaseSwapchainImage = NULL;
static PFN_xrEnumerateViewConfigurationViews pfn_xrEnumerateViewConfigurationViews = NULL;
PFN_xrStringToPath pfn_xrStringToPath = NULL;
PFN_xrCreateActionSet pfn_xrCreateActionSet = NULL;
PFN_xrDestroyActionSet pfn_xrDestroyActionSet = NULL;
PFN_xrCreateAction pfn_xrCreateAction = NULL;
static PFN_xrDestroyAction pfn_xrDestroyAction = NULL;
PFN_xrSuggestInteractionProfileBindings pfn_xrSuggestInteractionProfileBindings = NULL;
PFN_xrAttachSessionActionSets pfn_xrAttachSessionActionSets = NULL;
PFN_xrGetActionStateBoolean pfn_xrGetActionStateBoolean = NULL;
static PFN_xrGetActionStateFloat pfn_xrGetActionStateFloat = NULL;
PFN_xrGetActionStateVector2f pfn_xrGetActionStateVector2f = NULL;
PFN_xrGetActionStatePose pfn_xrGetActionStatePose = NULL;
PFN_xrSyncActions pfn_xrSyncActions = NULL;
PFN_xrCreateActionSpace pfn_xrCreateActionSpace = NULL;
PFN_xrLocateSpace pfn_xrLocateSpace = NULL;
PFN_xrDestroySpace pfn_xrDestroySpace = NULL;
static PFN_xrApplyHapticFeedback pfn_xrApplyHapticFeedback = NULL;
static PFN_xrPollEvent pfn_xrPollEvent = NULL;

// Global OpenXR context
static OpenXRContext g_openxr = {0};
static bool g_openxr_initialized = false;
static bool g_session_running = false;
static XrSessionState g_session_state = XR_SESSION_STATE_UNKNOWN;
static PFNGLGENFRAMEBUFFERSPROC pfn_glGenFramebuffers = NULL;
static PFNGLBINDFRAMEBUFFERPROC pfn_glBindFramebuffer = NULL;
static PFNGLFRAMEBUFFERTEXTURE2DPROC pfn_glFramebufferTexture2D = NULL;
static PFNGLGENRENDERBUFFERSPROC pfn_glGenRenderbuffers = NULL;
static PFNGLBINDRENDERBUFFERPROC pfn_glBindRenderbuffer = NULL;
static PFNGLRENDERBUFFERSTORAGEPROC pfn_glRenderbufferStorage = NULL;
static PFNGLFRAMEBUFFERRENDERBUFFERPROC pfn_glFramebufferRenderbuffer = NULL;
static PFNGLCHECKFRAMEBUFFERSTATUSPROC pfn_glCheckFramebufferStatus = NULL;
static PFNGLDELETEFRAMEBUFFERSPROC pfn_glDeleteFramebuffers = NULL;
static PFNGLDELETERENDERBUFFERSPROC pfn_glDeleteRenderbuffers = NULL;
void OpenXR_BindFramebuffer(GLuint framebuffer) {
    if (pfn_glBindFramebuffer) pfn_glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
}
// Load OpenXR loader functions
static bool LoadOpenXRFunctions(void) {
    HMODULE hLoader = LoadLibraryA("openxr_loader.dll");
    if (!hLoader) hLoader = LoadLibraryA("openxr_loader_x86.dll");
    if (!hLoader) {
        LOG_ERROR("Failed to load openxr_loader.dll");
        return false;
    }

    pfn_xrCreateInstance = (PFN_xrCreateInstance)GetProcAddress(hLoader, "xrCreateInstance");
    pfn_xrGetInstanceProcAddr = (PFN_xrGetInstanceProcAddr)GetProcAddress(hLoader, "xrGetInstanceProcAddr");
    pfn_xrDestroyInstance = (PFN_xrDestroyInstance)GetProcAddress(hLoader, "xrDestroyInstance");
    pfn_xrGetSystem = (PFN_xrGetSystem)GetProcAddress(hLoader, "xrGetSystem");
    pfn_xrCreateSession = (PFN_xrCreateSession)GetProcAddress(hLoader, "xrCreateSession");
    pfn_xrDestroySession = (PFN_xrDestroySession)GetProcAddress(hLoader, "xrDestroySession");
    pfn_xrBeginSession = (PFN_xrBeginSession)GetProcAddress(hLoader, "xrBeginSession");
    pfn_xrEndSession = (PFN_xrEndSession)GetProcAddress(hLoader, "xrEndSession");
    pfn_xrWaitFrame = (PFN_xrWaitFrame)GetProcAddress(hLoader, "xrWaitFrame");
    pfn_xrBeginFrame = (PFN_xrBeginFrame)GetProcAddress(hLoader, "xrBeginFrame");
    pfn_xrEndFrame = (PFN_xrEndFrame)GetProcAddress(hLoader, "xrEndFrame");
    pfn_xrLocateViews = (PFN_xrLocateViews)GetProcAddress(hLoader, "xrLocateViews");
    pfn_xrCreateReferenceSpace = (PFN_xrCreateReferenceSpace)GetProcAddress(hLoader, "xrCreateReferenceSpace");
    pfn_xrCreateActionSpace = (PFN_xrCreateActionSpace)GetProcAddress(hLoader, "xrCreateActionSpace");
    pfn_xrLocateSpace = (PFN_xrLocateSpace)GetProcAddress(hLoader, "xrLocateSpace");
    pfn_xrDestroySpace = (PFN_xrDestroySpace)GetProcAddress(hLoader, "xrDestroySpace");
    pfn_xrCreateSwapchain = (PFN_xrCreateSwapchain)GetProcAddress(hLoader, "xrCreateSwapchain");
    pfn_xrDestroySwapchain = (PFN_xrDestroySwapchain)GetProcAddress(hLoader, "xrDestroySwapchain");
    pfn_xrEnumerateSwapchainImages = (PFN_xrEnumerateSwapchainImages)GetProcAddress(hLoader, "xrEnumerateSwapchainImages");
    pfn_xrAcquireSwapchainImage = (PFN_xrAcquireSwapchainImage)GetProcAddress(hLoader, "xrAcquireSwapchainImage");
    pfn_xrWaitSwapchainImage = (PFN_xrWaitSwapchainImage)GetProcAddress(hLoader, "xrWaitSwapchainImage");
    pfn_xrReleaseSwapchainImage = (PFN_xrReleaseSwapchainImage)GetProcAddress(hLoader, "xrReleaseSwapchainImage");
    pfn_xrEnumerateViewConfigurationViews = (PFN_xrEnumerateViewConfigurationViews)GetProcAddress(hLoader, "xrEnumerateViewConfigurationViews");
    pfn_xrStringToPath = (PFN_xrStringToPath)GetProcAddress(hLoader, "xrStringToPath");
    pfn_xrCreateActionSet = (PFN_xrCreateActionSet)GetProcAddress(hLoader, "xrCreateActionSet");
    pfn_xrDestroyActionSet = (PFN_xrDestroyActionSet)GetProcAddress(hLoader, "xrDestroyActionSet");
    pfn_xrCreateAction = (PFN_xrCreateAction)GetProcAddress(hLoader, "xrCreateAction");
    pfn_xrDestroyAction = (PFN_xrDestroyAction)GetProcAddress(hLoader, "xrDestroyAction");
    pfn_xrSuggestInteractionProfileBindings = (PFN_xrSuggestInteractionProfileBindings)GetProcAddress(hLoader, "xrSuggestInteractionProfileBindings");
    pfn_xrAttachSessionActionSets = (PFN_xrAttachSessionActionSets)GetProcAddress(hLoader, "xrAttachSessionActionSets");
    pfn_xrGetActionStateBoolean = (PFN_xrGetActionStateBoolean)GetProcAddress(hLoader, "xrGetActionStateBoolean");
    pfn_xrGetActionStateFloat = (PFN_xrGetActionStateFloat)GetProcAddress(hLoader, "xrGetActionStateFloat");
    pfn_xrGetActionStateVector2f = (PFN_xrGetActionStateVector2f)GetProcAddress(hLoader, "xrGetActionStateVector2f");
    pfn_xrGetActionStatePose = (PFN_xrGetActionStatePose)GetProcAddress(hLoader, "xrGetActionStatePose");
    pfn_xrSyncActions = (PFN_xrSyncActions)GetProcAddress(hLoader, "xrSyncActions");
    pfn_xrApplyHapticFeedback = (PFN_xrApplyHapticFeedback)GetProcAddress(hLoader, "xrApplyHapticFeedback");
    pfn_xrPollEvent = (PFN_xrPollEvent)GetProcAddress(hLoader, "xrPollEvent");

    if (!pfn_xrCreateInstance || !pfn_xrGetInstanceProcAddr || !pfn_xrGetSystem ||
        !pfn_xrCreateSession || !pfn_xrPollEvent || !pfn_xrDestroyInstance ||
        !pfn_xrDestroySession || !pfn_xrBeginSession || !pfn_xrEndSession ||
        !pfn_xrWaitFrame || !pfn_xrBeginFrame || !pfn_xrEndFrame || !pfn_xrLocateViews ||
        !pfn_xrCreateReferenceSpace || !pfn_xrCreateSwapchain ||
        !pfn_xrDestroySwapchain || !pfn_xrEnumerateSwapchainImages ||
        !pfn_xrAcquireSwapchainImage || !pfn_xrWaitSwapchainImage ||
        !pfn_xrReleaseSwapchainImage || !pfn_xrEnumerateViewConfigurationViews) {
        LOG_ERROR("Critical OpenXR functions not found");
        return false;
    }
    LOG_INFO("OpenXR functions loaded successfully");
    return true;
}
// Initialize OpenXR instance and session
bool OpenXR_Init(HWND hwnd, HDC hdc, HGLRC hglrc) {
    if (g_openxr_initialized) return true;

    if (!LoadOpenXRFunctions()) return false;

    XrInstanceCreateInfo createInfo = {XR_TYPE_INSTANCE_CREATE_INFO};
    createInfo.applicationInfo.apiVersion = XR_CURRENT_API_VERSION;
    strcpy(createInfo.applicationInfo.applicationName, "SoF2 VR");
    createInfo.applicationInfo.applicationVersion = 1;
    strcpy(createInfo.applicationInfo.engineName, "SoF2 VR Proxy");
    createInfo.applicationInfo.engineVersion = 1;

    const char* extensions[] = {XR_KHR_OPENGL_ENABLE_EXTENSION_NAME};
    createInfo.enabledExtensionCount = 1;
    createInfo.enabledExtensionNames = (const char* const*)extensions;

    XrResult result = pfn_xrCreateInstance(&createInfo, &g_openxr.instance);
    if (XR_FAILED(result)) {
        LOG_ERROR("xrCreateInstance failed: %d", result);
        return false;
    }
    LOG_INFO("OpenXR instance created; Meta active runtime selected by the system loader");

    XrSystemGetInfo sysInfo = {XR_TYPE_SYSTEM_GET_INFO};
    sysInfo.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
    result = pfn_xrGetSystem(g_openxr.instance, &sysInfo, &g_openxr.systemId);
    if (XR_FAILED(result)) {
        LOG_ERROR("xrGetSystem failed: %d", result);
        return false;
    }
    LOG_INFO("OpenXR HMD system acquired: %llu", (unsigned long long)g_openxr.systemId);

    PFN_xrGetOpenGLGraphicsRequirementsKHR getGraphicsRequirements = NULL;
    result = pfn_xrGetInstanceProcAddr(g_openxr.instance,
        "xrGetOpenGLGraphicsRequirementsKHR",
        (PFN_xrVoidFunction*)&getGraphicsRequirements);
    if (XR_FAILED(result) || !getGraphicsRequirements) {
        LOG_ERROR("OpenXR runtime does not provide OpenGL graphics requirements (result %d)", result);
        return false;
    }
    XrGraphicsRequirementsOpenGLKHR graphicsRequirements = {
        XR_TYPE_GRAPHICS_REQUIREMENTS_OPENGL_KHR
    };
    result = getGraphicsRequirements(g_openxr.instance, g_openxr.systemId,
        &graphicsRequirements);
    if (XR_FAILED(result)) {
        LOG_ERROR("xrGetOpenGLGraphicsRequirementsKHR failed: %d", result);
        return false;
    }
    LOG_INFO("OpenGL graphics requirements: min=%d.%d max=%d.%d",
             XR_VERSION_MAJOR(graphicsRequirements.minApiVersionSupported),
             XR_VERSION_MINOR(graphicsRequirements.minApiVersionSupported),
             XR_VERSION_MAJOR(graphicsRequirements.maxApiVersionSupported),
             XR_VERSION_MINOR(graphicsRequirements.maxApiVersionSupported));

    uint32_t viewCount = 0;
    result = pfn_xrEnumerateViewConfigurationViews(g_openxr.instance, g_openxr.systemId,
        XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 0, &viewCount, NULL);
    if (XR_FAILED(result) || viewCount != 2) {
        LOG_ERROR("Stereo view configuration unavailable (result %d, views %u)", result, viewCount);
        return false;
    }
    g_openxr.viewConfigs = (XrViewConfigurationView*)calloc(viewCount, sizeof(XrViewConfigurationView));
    if (!g_openxr.viewConfigs) return false;
    for (uint32_t i = 0; i < viewCount; ++i)
        g_openxr.viewConfigs[i].type = XR_TYPE_VIEW_CONFIGURATION_VIEW;
    result = pfn_xrEnumerateViewConfigurationViews(g_openxr.instance, g_openxr.systemId,
        XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, viewCount, &viewCount, g_openxr.viewConfigs);
    if (XR_FAILED(result)) {
        LOG_ERROR("xrGetViewConfigurationViews failed: %d", result);
        return false;
    }
    g_openxr.viewCount = viewCount;
    LOG_INFO("View config: %dx%d per eye", g_openxr.viewConfigs[0].recommendedImageRectWidth,
             g_openxr.viewConfigs[0].recommendedImageRectHeight);

    XrGraphicsBindingOpenGLWin32KHR glBinding = {XR_TYPE_GRAPHICS_BINDING_OPENGL_WIN32_KHR};
    glBinding.hDC = hdc;
    glBinding.hGLRC = hglrc;

    XrSessionCreateInfo sessionInfo = {XR_TYPE_SESSION_CREATE_INFO};
    sessionInfo.systemId = g_openxr.systemId;
    sessionInfo.next = &glBinding;

    LOG_INFO("Calling xrCreateSession with HDC=%p HGLRC=%p", (void*)hdc, (void*)hglrc);
    result = pfn_xrCreateSession(g_openxr.instance, &sessionInfo, &g_openxr.session);
    if (XR_FAILED(result)) {
        LOG_ERROR("xrCreateSession failed: %d", result);
        return false;
    }
    LOG_INFO("xrCreateSession succeeded: %p", (void*)(ULONG_PTR)g_openxr.session);

    XrReferenceSpaceCreateInfo refSpaceInfo = {XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
    refSpaceInfo.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_STAGE;
    refSpaceInfo.poseInReferenceSpace = (XrPosef){{0,0,0,1}, {0,0,0}};
    result = pfn_xrCreateReferenceSpace(g_openxr.session, &refSpaceInfo, &g_openxr.stageSpace);
    if (XR_FAILED(result)) {
        LOG_WARN("Stage space creation failed, trying local space");
        refSpaceInfo.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
        result = pfn_xrCreateReferenceSpace(g_openxr.session, &refSpaceInfo, &g_openxr.stageSpace);
    }

    refSpaceInfo.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
    result = pfn_xrCreateReferenceSpace(g_openxr.session, &refSpaceInfo, &g_openxr.viewSpace);
    if (XR_FAILED(result)) {
        LOG_ERROR("View space creation failed: %d", result);
        return false;
    }
    LOG_INFO("OpenXR reference spaces created");

    if (g_openxr.viewCount == 2) {
        pfn_glGenFramebuffers = (PFNGLGENFRAMEBUFFERSPROC)wglGetProcAddress("glGenFramebuffers");
        pfn_glBindFramebuffer = (PFNGLBINDFRAMEBUFFERPROC)wglGetProcAddress("glBindFramebuffer");
        pfn_glFramebufferTexture2D = (PFNGLFRAMEBUFFERTEXTURE2DPROC)wglGetProcAddress("glFramebufferTexture2D");
        pfn_glGenRenderbuffers = (PFNGLGENRENDERBUFFERSPROC)wglGetProcAddress("glGenRenderbuffers");
        pfn_glBindRenderbuffer = (PFNGLBINDRENDERBUFFERPROC)wglGetProcAddress("glBindRenderbuffer");
        pfn_glRenderbufferStorage = (PFNGLRENDERBUFFERSTORAGEPROC)wglGetProcAddress("glRenderbufferStorage");
        pfn_glFramebufferRenderbuffer = (PFNGLFRAMEBUFFERRENDERBUFFERPROC)wglGetProcAddress("glFramebufferRenderbuffer");
        pfn_glCheckFramebufferStatus = (PFNGLCHECKFRAMEBUFFERSTATUSPROC)wglGetProcAddress("glCheckFramebufferStatus");
        pfn_glDeleteFramebuffers = (PFNGLDELETEFRAMEBUFFERSPROC)wglGetProcAddress("glDeleteFramebuffers");
        pfn_glDeleteRenderbuffers = (PFNGLDELETERENDERBUFFERSPROC)wglGetProcAddress("glDeleteRenderbuffers");
        if (!pfn_glGenFramebuffers || !pfn_glBindFramebuffer || !pfn_glFramebufferTexture2D ||
            !pfn_glGenRenderbuffers || !pfn_glBindRenderbuffer || !pfn_glRenderbufferStorage ||
            !pfn_glFramebufferRenderbuffer || !pfn_glCheckFramebufferStatus) {
            LOG_ERROR("OpenGL framebuffer functions are unavailable");
            return false;
        }
        for (int eye = 0; eye < 2; eye++) {
            XrSwapchainCreateInfo swapchainInfo = {XR_TYPE_SWAPCHAIN_CREATE_INFO};
            swapchainInfo.usageFlags = XR_SWAPCHAIN_USAGE_SAMPLED_BIT | XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT;
            swapchainInfo.format = GL_SRGB8_ALPHA8;
            swapchainInfo.sampleCount = 1;
            swapchainInfo.width = g_openxr.viewConfigs[eye].recommendedImageRectWidth;
            swapchainInfo.height = g_openxr.viewConfigs[eye].recommendedImageRectHeight;
            swapchainInfo.faceCount = 1;
            swapchainInfo.arraySize = 1;
            swapchainInfo.mipCount = 1;

            result = pfn_xrCreateSwapchain(g_openxr.session, &swapchainInfo, &g_openxr.swapchains[eye]);
            if (XR_FAILED(result)) {
                LOG_ERROR("xrCreateSwapchain failed for eye %d: %d", eye, result);
                return false;
            }
            LOG_INFO("Swapchain created for eye %d: %ux%u", eye, swapchainInfo.width, swapchainInfo.height);

            uint32_t imageCount = 0;
            pfn_xrEnumerateSwapchainImages(g_openxr.swapchains[eye], 0, &imageCount, NULL);
            g_openxr.swapchainImages[eye] = (XrSwapchainImageOpenGLKHR*)malloc(imageCount * sizeof(XrSwapchainImageOpenGLKHR));
            g_openxr.swapchainImageCount[eye] = imageCount;
            for (uint32_t i = 0; i < imageCount; i++) {
                g_openxr.swapchainImages[eye][i].type = XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_KHR;
            }
            pfn_xrEnumerateSwapchainImages(g_openxr.swapchains[eye], imageCount, &imageCount,
                (XrSwapchainImageBaseHeader*)g_openxr.swapchainImages[eye]);

            g_openxr.eyeFBOs[eye] = (GLuint*)calloc(imageCount, sizeof(GLuint));
            g_openxr.eyeDepthBuffers[eye] = (GLuint*)calloc(imageCount, sizeof(GLuint));
            for (uint32_t i = 0; i < imageCount; i++) {
                pfn_glGenFramebuffers(1, &g_openxr.eyeFBOs[eye][i]);
                pfn_glBindFramebuffer(GL_FRAMEBUFFER, g_openxr.eyeFBOs[eye][i]);
                pfn_glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                    g_openxr.swapchainImages[eye][i].image, 0);
                pfn_glGenRenderbuffers(1, &g_openxr.eyeDepthBuffers[eye][i]);
                pfn_glBindRenderbuffer(GL_RENDERBUFFER, g_openxr.eyeDepthBuffers[eye][i]);
                pfn_glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24,
                    g_openxr.viewConfigs[eye].recommendedImageRectWidth,
                    g_openxr.viewConfigs[eye].recommendedImageRectHeight);
                pfn_glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT,
                    GL_RENDERBUFFER, g_openxr.eyeDepthBuffers[eye][i]);
                if (pfn_glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
                    LOG_ERROR("OpenXR eye framebuffer %d image %u is incomplete", eye, i);
                    pfn_glBindRenderbuffer(GL_RENDERBUFFER, 0);
                    pfn_glBindFramebuffer(GL_FRAMEBUFFER, 0);
                    return false;
                }
                pfn_glBindRenderbuffer(GL_RENDERBUFFER, 0);
                pfn_glBindFramebuffer(GL_FRAMEBUFFER, 0);
            }
        }
    }

    g_openxr_initialized = true;
    LOG_INFO("OpenXR session created; waiting for runtime READY event");
    return true;
}

static void OpenXR_PollEvents(void) {
    XrEventDataBuffer event = {XR_TYPE_EVENT_DATA_BUFFER};
    XrResult result;

    if (!pfn_xrPollEvent || !g_openxr.instance) return;
    while ((result = pfn_xrPollEvent(g_openxr.instance, &event)) != XR_EVENT_UNAVAILABLE) {
        if (XR_FAILED(result)) {
            LOG_WARN("xrPollEvent failed: %d", result);
            break;
        }
        if (event.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED) {
            XrEventDataSessionStateChanged* changed = (XrEventDataSessionStateChanged*)&event;
            if (changed->session != g_openxr.session) continue;
            g_session_state = changed->state;
            LOG_INFO("OpenXR session state: %d", (int)g_session_state);

            if (g_session_state == XR_SESSION_STATE_READY && !g_session_running) {
                XrSessionBeginInfo beginInfo = {XR_TYPE_SESSION_BEGIN_INFO};
                beginInfo.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
                result = pfn_xrBeginSession(g_openxr.session, &beginInfo);
                if (XR_FAILED(result)) {
                    LOG_ERROR("xrBeginSession failed: %d", result);
                } else {
                    g_session_running = true;
                    LOG_INFO("OpenXR session started");
                }
            } else if (g_session_state == XR_SESSION_STATE_STOPPING && g_session_running) {
                pfn_xrEndSession(g_openxr.session);
                g_session_running = false;
            }
        }
        event = (XrEventDataBuffer){XR_TYPE_EVENT_DATA_BUFFER};
    }
}

// Per-frame OpenXR update
bool OpenXR_FrameBegin(void) {
    if (!g_openxr_initialized) return false;
    OpenXR_PollEvents();
    if (!g_session_running || (g_session_state != XR_SESSION_STATE_SYNCHRONIZED &&
        g_session_state != XR_SESSION_STATE_VISIBLE && g_session_state != XR_SESSION_STATE_FOCUSED)) return false;

    XrFrameWaitInfo waitInfo = {XR_TYPE_FRAME_WAIT_INFO};
    XrFrameState frameState = {XR_TYPE_FRAME_STATE};
    XrResult result = pfn_xrWaitFrame(g_openxr.session, &waitInfo, &frameState);
    if (XR_FAILED(result)) return false;

    g_openxr.predictedDisplayTime = frameState.predictedDisplayTime;
    g_openxr.shouldRender = frameState.shouldRender;

    XrFrameBeginInfo beginInfo = {XR_TYPE_FRAME_BEGIN_INFO};
    result = pfn_xrBeginFrame(g_openxr.session, &beginInfo);
    return XR_SUCCEEDED(result);
}

// Locate views (head pose)
bool OpenXR_LocateViews(void) {
    if (!g_openxr_initialized || !g_session_running) return false;
    g_openxr.viewsLocated = false;

    XrViewLocateInfo locateInfo = {XR_TYPE_VIEW_LOCATE_INFO};
    locateInfo.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
    locateInfo.displayTime = g_openxr.predictedDisplayTime;
    locateInfo.space = g_openxr.stageSpace;

    XrViewState viewState = {XR_TYPE_VIEW_STATE};
    uint32_t viewCount = 0;
    XrView views[2] = {{XR_TYPE_VIEW}, {XR_TYPE_VIEW}};

    XrResult result = pfn_xrLocateViews(g_openxr.session, &locateInfo, &viewState, 2, &viewCount, views);
    if (XR_FAILED(result) || viewCount != 2) return false;

    g_openxr.views[0] = views[0];
    g_openxr.views[1] = views[1];
    g_openxr.viewsLocated = true;
    return true;
}

// End frame and submit
void OpenXR_FrameEnd(void) {
    if (!g_openxr_initialized || !g_session_running) return;

    XrCompositionLayerProjection layer = {XR_TYPE_COMPOSITION_LAYER_PROJECTION};
    XrCompositionLayerProjectionView projectionViews[2] = {
        {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW},
        {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW}
    };
    layer.space = g_openxr.stageSpace;
    layer.viewCount = 2;
    layer.views = projectionViews;

    for (int eye = 0; eye < 2; eye++) {
        projectionViews[eye].type = XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW;
        projectionViews[eye].pose = g_openxr.views[eye].pose;
        projectionViews[eye].fov = g_openxr.views[eye].fov;
        projectionViews[eye].subImage.swapchain = g_openxr.swapchains[eye];
        projectionViews[eye].subImage.imageRect.offset = (XrOffset2Di){0, 0};
        projectionViews[eye].subImage.imageRect.extent = (XrExtent2Di){
            g_openxr.viewConfigs[eye].recommendedImageRectWidth,
            g_openxr.viewConfigs[eye].recommendedImageRectHeight
        };
    }

    XrFrameEndInfo endInfo = {XR_TYPE_FRAME_END_INFO};
    endInfo.displayTime = g_openxr.predictedDisplayTime;
    endInfo.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
    endInfo.layerCount = g_openxr.shouldRender && g_openxr.viewsLocated ? 1 : 0;
    endInfo.layers = endInfo.layerCount
        ? (const XrCompositionLayerBaseHeader* const*)&layer
        : NULL;

    pfn_xrEndFrame(g_openxr.session, &endInfo);
    g_openxr.viewsLocated = false;
}

// Shutdown OpenXR
void OpenXR_Shutdown(void) {
    if (g_session_running) {
        pfn_xrEndSession(g_openxr.session);
        g_session_running = false;
    }
    for (int eye = 0; eye < 2; ++eye) {
        uint32_t count = g_openxr.swapchainImageCount[eye];
        if (g_openxr.eyeFBOs[eye] && pfn_glDeleteFramebuffers)
            pfn_glDeleteFramebuffers(count, g_openxr.eyeFBOs[eye]);
        if (g_openxr.eyeDepthBuffers[eye] && pfn_glDeleteRenderbuffers)
            pfn_glDeleteRenderbuffers(count, g_openxr.eyeDepthBuffers[eye]);
        free(g_openxr.eyeFBOs[eye]);
        free(g_openxr.eyeDepthBuffers[eye]);
        free(g_openxr.swapchainImages[eye]);
        g_openxr.eyeFBOs[eye] = NULL;
        g_openxr.eyeDepthBuffers[eye] = NULL;
        g_openxr.swapchainImages[eye] = NULL;
        if (g_openxr.swapchains[eye] && pfn_xrDestroySwapchain)
            pfn_xrDestroySwapchain(g_openxr.swapchains[eye]);
        g_openxr.swapchains[eye] = XR_NULL_HANDLE;
    }
    free(g_openxr.viewConfigs);
    g_openxr.viewConfigs = NULL;
    g_openxr.viewCount = 0;
    if (g_openxr.session) {
        pfn_xrDestroySession(g_openxr.session);
        g_openxr.session = XR_NULL_HANDLE;
    }
    if (g_openxr.instance) {
        pfn_xrDestroyInstance(g_openxr.instance);
        g_openxr.instance = XR_NULL_HANDLE;
    }
    g_session_state = XR_SESSION_STATE_UNKNOWN;
    g_openxr.shouldRender = false;
    g_openxr_initialized = false;
    LOG_INFO("OpenXR shutdown complete");
}

// Getters for stereo render
const XrView* OpenXR_GetView(int eye) {
    if (eye >= 0 && eye < 2) return &g_openxr.views[eye];
    return NULL;
}

XrSwapchain OpenXR_GetSwapchain(int eye) {
    if (eye >= 0 && eye < 2) return g_openxr.swapchains[eye];
    return XR_NULL_HANDLE;
}

uint32_t OpenXR_GetSwapchainImageCount(int eye) {
    if (eye >= 0 && eye < 2) return g_openxr.swapchainImageCount[eye];
    return 0;
}

int32_t OpenXR_GetEyeWidth(int eye) {
    if (eye >= 0 && eye < 2 && g_openxr.viewConfigs)
        return (int32_t)g_openxr.viewConfigs[eye].recommendedImageRectWidth;
    return 0;
}

int32_t OpenXR_GetEyeHeight(int eye) {
    if (eye >= 0 && eye < 2 && g_openxr.viewConfigs)
        return (int32_t)g_openxr.viewConfigs[eye].recommendedImageRectHeight;
    return 0;
}

GLuint OpenXR_GetEyeFBO(int eye, uint32_t index) {
    if (eye >= 0 && eye < 2 && index < g_openxr.swapchainImageCount[eye]) {
        return g_openxr.eyeFBOs[eye][index];
    }
    return 0;
}

XrSpace OpenXR_GetStageSpace(void) { return g_openxr.stageSpace; }
XrSpace OpenXR_GetViewSpace(void) { return g_openxr.viewSpace; }
XrSession OpenXR_GetSession(void) { return g_openxr.session; }
XrInstance OpenXR_GetInstance(void) { return g_openxr.instance; }
XrTime OpenXR_GetPredictedDisplayTime(void) { return g_openxr.predictedDisplayTime; }
bool OpenXR_ShouldRender(void) { return g_openxr.shouldRender; }
bool OpenXR_IsInitialized(void) { return g_openxr_initialized; }
