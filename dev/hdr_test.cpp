// HDR test harness — full video stack with transparent CEF-less window.
// Runs the identical Wayland surface hierarchy, dmabuf path, and color
// management protocol as the real app. Use with WAYLAND_DEBUG=1 to
// capture protocol messages for comparison with standalone mpv.
//
// Usage: WAYLAND_DEBUG=1 ./build/hdr-test <video-url> 2>app_wl.log

#include <SDL3/SDL.h>
#include <GLES3/gl3.h>
#include <chrono>
#include <thread>

#include "context/egl_context.h"
#include "player/video_stack.h"
#include "player/video_render_controller.h"
#include "player/mpv/mpv_player.h"
#include "player/video_renderer.h"
#include "logging.h"

static constexpr int TEST_DURATION_SEC = 5;

static const char* HDR_TEST_URL =
    ""; // Replace with your Jellyfin video URL

int main(int argc, char* argv[]) {
    (void)argc; (void)argv;
    const char* url = HDR_TEST_URL;

    initLogging(SDL_LOG_PRIORITY_DEBUG);
    LOG_INFO(LOG_MAIN, "HDR test harness starting");

    // SDL + window
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        LOG_ERROR(LOG_MAIN, "SDL_Init failed: %s", SDL_GetError());
        return 1;
    }

    SDL_Window* window = SDL_CreateWindow(
        "HDR Test", 1280, 720,
        SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY);
    if (!window) {
        LOG_ERROR(LOG_MAIN, "SDL_CreateWindow failed: %s", SDL_GetError());
        SDL_Quit();
        return 1;
    }

    // EGL context (needed for the compositor GL surface)
    EGLContext_ egl;
    if (!egl.init(window)) {
        LOG_ERROR(LOG_MAIN, "EGL init failed");
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }

    // Clear to transparent — the video subsurface renders behind this
    glClearColor(0, 0, 0, 0);
    glClear(GL_COLOR_BUFFER_BIT);
    egl.swapBuffers();

    int physical_w, physical_h;
    SDL_GetWindowSizeInPixels(window, &physical_w, &physical_h);
    int logical_w, logical_h;
    SDL_GetWindowSize(window, &logical_w, &logical_h);

    LOG_INFO(LOG_MAIN, "Window: %dx%d physical, %dx%d logical",
             physical_w, physical_h, logical_w, logical_h);

    // Video stack — identical path to the real app
    VideoStack videoStack = VideoStack::create(window, logical_w, logical_h,
                                               &egl, "auto-safe", {});
    if (!videoStack.player || !videoStack.renderer) {
        LOG_ERROR(LOG_MAIN, "VideoStack creation failed");
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }
    MpvPlayer* mpv = videoStack.player.get();
    VideoRenderer* renderer = videoStack.renderer.get();

    // Video render controller — threaded, same as real app
    VideoRenderController videoController;
    videoController.startThreaded(renderer);
    mpv->setRedrawCallback([&videoController]() {
        videoController.notify();
    });

    // Load and play
    LOG_INFO(LOG_MAIN, "Loading: %s", url);
    if (!mpv->loadFile(url, 0.0)) {
        LOG_ERROR(LOG_MAIN, "loadFile failed");
        videoController.stop();
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }
    renderer->setVisible(true);
    videoController.setActive(true);
    renderer->setDestinationSize(logical_w, logical_h);
    videoController.requestResize(physical_w, physical_h);
    videoController.render(physical_w, physical_h);

    // Event loop — pump SDL events + render transparent surface
    auto start = std::chrono::steady_clock::now();
    bool running = true;
    while (running) {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_EVENT_QUIT)
                running = false;
        }

        // Transparent clear on the main EGL surface (CEF would render here)
        egl.makeCurrentMain();
        glClearColor(0, 0, 0, 0);
        glClear(GL_COLOR_BUFFER_BIT);
        egl.swapBuffers();

        // Process mpv events
        mpv->processEvents();

        // Update render dimensions
        videoController.render(physical_w, physical_h);

        // Exit after TEST_DURATION_SEC seconds
        auto elapsed = std::chrono::steady_clock::now() - start;
        if (elapsed > std::chrono::seconds(TEST_DURATION_SEC))
            running = false;

        std::this_thread::sleep_for(std::chrono::milliseconds(16));
    }

    LOG_INFO(LOG_MAIN, "HDR test complete, cleaning up");

    videoController.setActive(false);
    videoController.stop();
    mpv->stop();
    mpv->cleanup();
    renderer->cleanup();
    VideoStack::cleanupStatics();
    egl.cleanup();
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}
