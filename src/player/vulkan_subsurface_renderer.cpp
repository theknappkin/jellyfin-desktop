#include "vulkan_subsurface_renderer.h"
#include "mpv/mpv_player.h"
#include <mpv/render.h>
#include <mpv/render_vk.h>
#ifdef __APPLE__
#include "platform/macos_layer.h"
#elif defined(_WIN32)
#include "platform/windows_video_layer.h"
#else
#include "platform/video_surface.h"
#endif

VulkanSubsurfaceRenderer::VulkanSubsurfaceRenderer(MpvPlayer* player, VideoSurface* surface)
    : player_(player), surface_(surface) {}

bool VulkanSubsurfaceRenderer::hasFrame() const {
    return player_->hasFrame();
}

bool VulkanSubsurfaceRenderer::render(int width, int height) {
    // Swapchain mode: mpv/libplacebo handles frame acquisition and presentation.
    int size[2] = { width, height };
    int flip_y = 0;
    mpv_render_param render_params[] = {
        {MPV_RENDER_PARAM_FLIP_Y, &flip_y},
        {MPV_RENDER_PARAM_VULKAN_SWAPCHAIN_SIZE, size},
        {MPV_RENDER_PARAM_INVALID, nullptr}
    };

    mpv_render_context_render(player_->renderContext(), render_params);
    player_->reportSwap();
    return true;
}

void VulkanSubsurfaceRenderer::setVisible(bool visible) {
    if (visible) {
        surface_->show();
    } else {
        surface_->hide();
    }
}

void VulkanSubsurfaceRenderer::resize(int width, int height) {
#if defined(__APPLE__) || defined(_WIN32)
    surface_->resize(width, height);
#else
    surface_->recreateSwapchain(width, height);
#endif
}

void VulkanSubsurfaceRenderer::setDestinationSize(int width, int height) {
    surface_->setDestinationSize(width, height);
}

void VulkanSubsurfaceRenderer::setColorspace() {
    surface_->setColorspace();
}

void VulkanSubsurfaceRenderer::cleanup() {
    surface_->cleanup();
}

float VulkanSubsurfaceRenderer::getClearAlpha(bool video_ready) const {
    return video_ready ? 0.0f : 1.0f;
}

bool VulkanSubsurfaceRenderer::isHdr() const {
    return surface_->isHdr();
}
