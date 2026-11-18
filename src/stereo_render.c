#include "stereo_render.h"
#include "proxy_dll.h"
#include "openxr_core.h"
#include <GL/gl.h>
#include <GL/glext.h>
#include <stdio.h>
#include <math.h>
#include <string.h>

static void StereoRender_ComputeProjectionMatrix(int eye, float znear, float zfar);
static void StereoRender_ComputeViewMatrix(int eye);

static VRViewParams g_viewParams[2] = {0};
static float g_projMatrices[2][16] = {0};
static float g_viewMatrices[2][16] = {0};
static int g_windowWidth = 0;
static int g_windowHeight = 0;

void StereoRender_Init(int width, int height) {
    g_windowWidth = width;
    g_windowHeight = height;
    LOG_INFO("Stereo render initialized: %dx%d", width, height);
}

void StereoRender_Shutdown(void) {
    LOG_INFO("Stereo render shutdown");
}

void StereoRender_BeginFrame(void) {
    if (!OpenXR_ShouldRender()) return;
    
    for (int eye = 0; eye < 2; eye++) {
        const XrView* view = OpenXR_GetView(eye);
        if (!view) continue;
        
        g_viewParams[eye].pos[0] = view->pose.position.x;
        g_viewParams[eye].pos[1] = view->pose.position.y;
        g_viewParams[eye].pos[2] = view->pose.position.z;
        g_viewParams[eye].rot[0] = view->pose.orientation.x;
        g_viewParams[eye].rot[1] = view->pose.orientation.y;
        g_viewParams[eye].rot[2] = view->pose.orientation.z;
        g_viewParams[eye].rot[3] = view->pose.orientation.w;
        g_viewParams[eye].fov = view->fov;
        
        StereoRender_ComputeProjectionMatrix(eye, 0.05f, 1000.0f);
        StereoRender_ComputeViewMatrix(eye);
    }
}

void StereoRender_ComputeProjectionMatrix(int eye, float znear, float zfar) {
    const XrView* view = OpenXR_GetView(eye);
    if (!view) return;
    
    const XrFovf* fov = &view->fov;
    float tanLeft = tanf(fov->angleLeft);
    float tanRight = tanf(fov->angleRight);
    float tanUp = tanf(fov->angleUp);
    float tanDown = tanf(fov->angleDown);
    
    float tanWidth = tanRight - tanLeft;
    float tanHeight = tanUp - tanDown;
    
    float* proj = g_projMatrices[eye];
    memset(proj, 0, 16 * sizeof(float));
    
    proj[0] = 2.0f / tanWidth;
    proj[5] = 2.0f / tanHeight;
    proj[8] = (tanRight + tanLeft) / tanWidth;
    proj[9] = (tanUp + tanDown) / tanHeight;
    proj[10] = -(zfar + znear) / (zfar - znear);
    proj[11] = -1.0f;
    proj[14] = -(2.0f * zfar * znear) / (zfar - znear);
}

void StereoRender_ComputeViewMatrix(int eye) {
    const XrView* view = OpenXR_GetView(eye);
    if (!view) return;
    
    const XrPosef* pose = &view->pose;
    float x = pose->orientation.x;
    float y = pose->orientation.y;
    float z = pose->orientation.z;
    float w = pose->orientation.w;
    
    float xx = x * x;
    float yy = y * y;
    float zz = z * z;
    float xy = x * y;
    float xz = x * z;
    float yz = y * z;
    float wx = w * x;
    float wy = w * y;
    float wz = w * z;
    
    float* viewMat = g_viewMatrices[eye];
    viewMat[0] = 1 - 2 * (yy + zz);
    viewMat[1] = 2 * (xy - wz);
    viewMat[2] = 2 * (xz + wy);
    viewMat[3] = 0;
    
    viewMat[4] = 2 * (xy + wz);
    viewMat[5] = 1 - 2 * (xx + zz);
    viewMat[6] = 2 * (yz - wx);
    viewMat[7] = 0;
    
    viewMat[8] = 2 * (xz - wy);
    viewMat[9] = 2 * (yz + wx);
    viewMat[10] = 1 - 2 * (xx + yy);
    viewMat[11] = 0;
    
    viewMat[12] = -(viewMat[0] * pose->position.x + viewMat[4] * pose->position.y + viewMat[8] * pose->position.z);
    viewMat[13] = -(viewMat[1] * pose->position.x + viewMat[5] * pose->position.y + viewMat[9] * pose->position.z);
    viewMat[14] = -(viewMat[2] * pose->position.x + viewMat[6] * pose->position.y + viewMat[10] * pose->position.z);
    viewMat[15] = 1.0f;
}

void StereoRender_RenderEye(int eye, void (*renderCallback)(int eye, const float* proj, const float* view)) {
    if (!OpenXR_ShouldRender()) return;
    
    XrSwapchain swapchain = OpenXR_GetSwapchain(eye);
    if (swapchain == XR_NULL_HANDLE) return;
    
    uint32_t imageIndex = 0;
    XrSwapchainImageAcquireInfo acquireInfo = {XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
    XrResult result = pfn_xrAcquireSwapchainImage(swapchain, &acquireInfo, &imageIndex);
    if (XR_FAILED(result)) return;
    
    XrSwapchainImageWaitInfo waitInfo = {XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
    waitInfo.timeout = XR_INFINITE_DURATION;
    result = pfn_xrWaitSwapchainImage(swapchain, &waitInfo);
    if (XR_FAILED(result)) {
        pfn_xrReleaseSwapchainImage(swapchain, NULL);
        return;
    }
    
    GLuint fbo = OpenXR_GetEyeFBO(eye, imageIndex);
    if (fbo) {
        OpenXR_BindFramebuffer(fbo);
        glViewport(0, 0, OpenXR_GetEyeWidth(eye), OpenXR_GetEyeHeight(eye));
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        
        glMatrixMode(GL_PROJECTION);
        glLoadMatrixf(g_projMatrices[eye]);
        glMatrixMode(GL_MODELVIEW);
        glLoadMatrixf(g_viewMatrices[eye]);
        
        if (renderCallback) {
            renderCallback(eye, g_projMatrices[eye], g_viewMatrices[eye]);
        }
        
        OpenXR_BindFramebuffer(0);
    }
    
    XrSwapchainImageReleaseInfo releaseInfo = {XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
    pfn_xrReleaseSwapchainImage(swapchain, &releaseInfo);
}

void StereoRender_EndFrame(void) {
    OpenXR_FrameEnd();
}

const float* StereoRender_GetProjectionMatrix(int eye) {
    return g_projMatrices[eye];
}

const float* StereoRender_GetViewMatrix(int eye) {
    return g_viewMatrices[eye];
}

const VRViewParams* StereoRender_GetViewParams(int eye) {
    return &g_viewParams[eye];
}
