// Resize test harness — starts windowed, toggles fullscreen, logs all dimensions.
// Tests that the video layer resizes correctly when going fullscreen.
//
// Usage: ./build/resize-test [video-url]

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

static const char* DEFAULT_URL =
    "https://jellyfin.nullsum.net/Videos/61e35022113290eec74e625dcda4530c/stream.mkv"
    "?Static=true"
    "&mediaSourceId=61e35022113290eec74e625dcda4530c"
    "&deviceId=TW96aWxsYS81LjAgKFgxMTsgTGludXggeDg2XzY0KSBBcHBsZVdlYktpdC81MzcuMzYgKEtIVE1MLCBsaWtlIEdlY2tvKSBDaHJvbWUvMTQ1LjAuMC4wIFNhZmFyaS81MzcuMzZ8MTc3NDIzMDQ3OTM5OQ11"
    "&ApiKey=119982a9125e451bad9d31dfbfc13f1e"
    "&Tag=5bf4b4a92402f50418d19cd8cb2f2402";

static void logSDL(const char* label, SDL_Window* window) {
    int lw, lh, pw, ph;
    SDL_GetWindowSize(window, &lw, &lh);
    SDL_GetWindowSizeInPixels(window, &pw, &ph);
    float scale = SDL_GetWindowDisplayScale(window);
    LOG_INFO(LOG_MAIN, "[%s] SDL logical=%dx%d physical=%dx%d scale=%.2f",
             label, lw, lh, pw, ph, scale);
}

int main(int argc, char* argv[]) {
    const char* url = (argc > 1) ? argv[1] : DEFAULT_URL;

    initLogging(SDL_LOG_PRIORITY_DEBUG);
    LOG_INFO(LOG_MAIN, "=== RESIZE TEST HARNESS ===");

    if (!SDL_Init(SDL_INIT_VIDEO)) {
        LOG_ERROR(LOG_MAIN, "SDL_Init failed: %s", SDL_GetError());
        return 1;
    }

    SDL_Window* window = SDL_CreateWindow(
        "Resize Test", 960, 540,
        SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY);
    if (!window) {
        LOG_ERROR(LOG_MAIN, "SDL_CreateWindow failed: %s", SDL_GetError());
        SDL_Quit();
        return 1;
    }

    EGLContext_ egl;
    if (!egl.init(window)) {
        LOG_ERROR(LOG_MAIN, "EGL init failed");
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }

    glClearColor(0, 0, 0, 0);
    glClear(GL_COLOR_BUFFER_BIT);
    egl.swapBuffers();

    int physical_w, physical_h, logical_w, logical_h;
    SDL_GetWindowSizeInPixels(window, &physical_w, &physical_h);
    SDL_GetWindowSize(window, &logical_w, &logical_h);

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

    VideoRenderController videoController;
    videoController.startThreaded(renderer);
    mpv->setRedrawCallback([&videoController]() {
        videoController.notify();
    });

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

    logSDL("INIT", window);

    enum class Phase { WINDOWED, PRE_FS, FULLSCREEN, DONE };
    Phase phase = Phase::WINDOWED;
    auto start = std::chrono::steady_clock::now();
    auto phase_start = start;
    bool running = true;

    while (running) {
        auto now = std::chrono::steady_clock::now();
        double phase_sec = std::chrono::duration<double>(now - phase_start).count();
        double total_sec = std::chrono::duration<double>(now - start).count();

        if (phase == Phase::WINDOWED && phase_sec > 3.0) {
            LOG_INFO(LOG_MAIN, "=== ENTERING FULLSCREEN ===");
            logSDL("BEFORE-FS", window);
            SDL_SetWindowFullscreen(window, true);
            phase = Phase::PRE_FS;
            phase_start = now;
        } else if (phase == Phase::PRE_FS && phase_sec > 1.0) {
            phase = Phase::FULLSCREEN;
            phase_start = now;
            LOG_INFO(LOG_MAIN, "=== FULLSCREEN SETTLED ===");
            logSDL("AFTER-FS", window);
        } else if (phase == Phase::FULLSCREEN && phase_sec > 3.0) {
            phase = Phase::DONE;
            logSDL("FINAL", window);
        }

        if (total_sec > 9.0 || phase == Phase::DONE)
            running = false;

        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_EVENT_QUIT) {
                running = false;
            } else if (event.type == SDL_EVENT_WINDOW_RESIZED) {
                int nlw = event.window.data1;
                int nlh = event.window.data2;
                int npw, nph;
                SDL_GetWindowSizeInPixels(window, &npw, &nph);

                LOG_INFO(LOG_MAIN, "[RESIZE-EVENT] logical=%dx%d physical=%dx%d",
                         nlw, nlh, npw, nph);

                egl.resize(npw, nph);
                renderer->setDestinationSize(nlw, nlh);
                videoController.requestResize(npw, nph);

                physical_w = npw;
                physical_h = nph;
                logical_w = nlw;
                logical_h = nlh;
            } else if (event.type == SDL_EVENT_WINDOW_ENTER_FULLSCREEN) {
                LOG_INFO(LOG_MAIN, "[FS-ENTER]");
            } else if (event.type == SDL_EVENT_WINDOW_LEAVE_FULLSCREEN) {
                LOG_INFO(LOG_MAIN, "[FS-LEAVE]");
            }
        }

        egl.makeCurrentMain();
        int vp_w, vp_h;
        SDL_GetWindowSizeInPixels(window, &vp_w, &vp_h);
        glViewport(0, 0, vp_w, vp_h);
        glClearColor(0, 0, 0, 0);
        glClear(GL_COLOR_BUFFER_BIT);
        egl.swapBuffers();

        mpv->processEvents();
        videoController.render(vp_w, vp_h);

        std::this_thread::sleep_for(std::chrono::milliseconds(16));
    }

    LOG_INFO(LOG_MAIN, "=== RESIZE TEST COMPLETE ===");

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
