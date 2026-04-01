// Resize test harness — full composited stack (CEF + video subsurface).
// Starts video immediately, composites a transparent CEF browser on top,
// scripts fullscreen enter/exit, and logs dimension state at each layer.
//
// Usage: ./build/resize-test [video-url]

#include <SDL3/SDL.h>
#include <GLES3/gl3.h>
#include <unistd.h>
#include <chrono>
#include <thread>
#include <filesystem>

#include "include/cef_app.h"
#include "include/cef_client.h"
#include "include/cef_render_handler.h"

#include "cef/cef_app.h"
#include "context/egl_context.h"
#include "context/opengl_frame_context.h"
#include "compositor/opengl_compositor.h"
#include "player/video_stack.h"
#include "player/video_render_controller.h"
#include "player/mpv/mpv_player.h"
#include "player/video_renderer.h"
#include "logging.h"

static const char* DEFAULT_URL =
    ""; // Replace with your Jellyfin video URL

// Minimal CEF client — transparent browser composited on top of video
class TestClient : public CefClient, public CefRenderHandler {
public:
    TestClient(int w, int h, OpenGLCompositor* compositor)
        : width_(w), height_(h), compositor_(compositor) {}

    CefRefPtr<CefRenderHandler> GetRenderHandler() override { return this; }

    void GetViewRect(CefRefPtr<CefBrowser>, CefRect& rect) override {
        rect.Set(0, 0, width_, height_);
    }

    void OnPaint(CefRefPtr<CefBrowser>, PaintElementType,
                 const RectList&, const void* buffer,
                 int width, int height) override {
        if (compositor_)
            compositor_->updateOverlayPartial(buffer, width, height);
    }

    void OnAcceleratedPaint(CefRefPtr<CefBrowser>, PaintElementType type,
                            const RectList&, const CefAcceleratedPaintInfo& info) override {
        if (!compositor_ || type != PET_VIEW || info.plane_count == 0) return;
        int w = info.extra.coded_size.width;
        int h = info.extra.coded_size.height;
        if (w > 0 && h > 0) {
            int fd = dup(info.planes[0].fd);
            if (fd >= 0)
                compositor_->queueDmabuf(fd, info.planes[0].stride, info.modifier, w, h);
        }
    }

    void resize(int w, int h) {
        width_ = w;
        height_ = h;
        if (auto browser = browser_) {
            browser->GetHost()->WasResized();
        }
    }

    void setBrowser(CefRefPtr<CefBrowser> b) { browser_ = b; }
    CefRefPtr<CefBrowser> browser() { return browser_; }

private:
    int width_, height_;
    OpenGLCompositor* compositor_;
    CefRefPtr<CefBrowser> browser_;

    IMPLEMENT_REFCOUNTING(TestClient);
};

static void logSDL(const char* label, SDL_Window* window) {
    int lw, lh, pw, ph;
    SDL_GetWindowSize(window, &lw, &lh);
    SDL_GetWindowSizeInPixels(window, &pw, &ph);
    float scale = SDL_GetWindowDisplayScale(window);
    LOG_INFO(LOG_MAIN, "[%s] SDL logical=%dx%d physical=%dx%d scale=%.2f",
             label, lw, lh, pw, ph, scale);
}

int main(int argc, char* argv[]) {
    // CEF subprocess check — must be absolutely first, before any other init
    CefMainArgs main_args(argc, argv);
    CefRefPtr<App> app(new App());
    int exit_code = CefExecuteProcess(main_args, app, nullptr);
    if (exit_code >= 0)
        return exit_code;

    const char* video_url = DEFAULT_URL;
    for (int i = 1; i < argc; i++) {
        if (argv[i][0] != '-') {
            video_url = argv[i];
            break;
        }
    }

    initLogging(SDL_LOG_PRIORITY_DEBUG);
    LOG_INFO(LOG_MAIN, "=== RESIZE TEST HARNESS (CEF + video) ===");

    // SDL + window
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

    // EGL context
    EGLContext_ egl;
    if (!egl.init(window)) {
        LOG_ERROR(LOG_MAIN, "EGL init failed");
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }

    OpenGLFrameContext frameContext(&egl);
    frameContext.beginFrame(0.0f, 1.0f);
    frameContext.endFrame();

    int physical_w, physical_h, logical_w, logical_h;
    SDL_GetWindowSizeInPixels(window, &physical_w, &physical_h);
    SDL_GetWindowSize(window, &logical_w, &logical_h);

    // CEF compositor — renders browser texture on top of video
    OpenGLCompositor compositor;
    if (!compositor.init(&egl, physical_w, physical_h)) {
        LOG_ERROR(LOG_MAIN, "Compositor init failed");
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }

    // CEF init
    CefSettings settings;
    settings.no_sandbox = true;
    settings.windowless_rendering_enabled = true;
    settings.external_message_pump = true;

    // Use same resource paths as the main app
    std::filesystem::path exe_path = std::filesystem::canonical("/proc/self/exe").parent_path();
#ifdef CEF_RESOURCES_DIR
    CefString(&settings.resources_dir_path).FromString(CEF_RESOURCES_DIR);
    CefString(&settings.locales_dir_path).FromString(CEF_RESOURCES_DIR "/locales");
#else
    CefString(&settings.resources_dir_path).FromString(exe_path.string());
    CefString(&settings.locales_dir_path).FromString((exe_path / "locales").string());
#endif
    // Subprocess must use this same binary
    CefString(&settings.browser_subprocess_path).FromString(
        std::filesystem::canonical("/proc/self/exe").string());

    // Use temp cache to avoid interfering with running app
    std::filesystem::path cache_path = "/tmp/jellyfin-resize-test";
    std::filesystem::create_directories(cache_path);
    CefString(&settings.root_cache_path).FromString(cache_path.string());
    CefString(&settings.cache_path).FromString((cache_path / "cache").string());

    if (!CefInitialize(main_args, settings, app, nullptr)) {
        LOG_ERROR(LOG_CEF, "CefInitialize failed");
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }
    LOG_INFO(LOG_CEF, "CEF initialized");

    // Create transparent browser
    CefRefPtr<TestClient> client(new TestClient(physical_w, physical_h, &compositor));

    CefWindowInfo window_info;
    window_info.SetAsWindowless(0);
    bool use_dmabuf = egl.supportsDmaBufImport();
    window_info.shared_texture_enabled = use_dmabuf;

    CefBrowserSettings browser_settings;
    browser_settings.background_color = 0;  // transparent
    browser_settings.windowless_frame_rate = 60;

    // Load a transparent page — CEF layer is visible but see-through
    CefBrowserHost::CreateBrowser(window_info, client,
        "data:text/html,<body style='background:transparent'></body>",
        browser_settings, nullptr, nullptr);

    // Video stack
    VideoStack videoStack = VideoStack::create(window, logical_w, logical_h,
                                               &egl, "auto-safe", {});
    if (!videoStack.player || !videoStack.renderer) {
        LOG_ERROR(LOG_MAIN, "VideoStack creation failed");
        CefShutdown();
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

    LOG_INFO(LOG_MAIN, "Loading video: %s", video_url);
    if (!mpv->loadFile(video_url, 0.0)) {
        LOG_ERROR(LOG_MAIN, "loadFile failed");
        videoController.stop();
        CefShutdown();
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

    // Test phases: windowed → fullscreen → restored
    enum class Phase { WINDOWED, PRE_FS, FULLSCREEN, PRE_RESTORE, RESTORED, DONE };
    Phase phase = Phase::WINDOWED;
    auto start = std::chrono::steady_clock::now();
    auto phase_start = start;
    bool running = true;

    while (running) {
        auto now = std::chrono::steady_clock::now();
        double phase_sec = std::chrono::duration<double>(now - phase_start).count();
        double total_sec = std::chrono::duration<double>(now - start).count();

        // Phase transitions
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
            LOG_INFO(LOG_MAIN, "=== EXITING FULLSCREEN ===");
            logSDL("BEFORE-RESTORE", window);
            SDL_SetWindowFullscreen(window, false);
            phase = Phase::PRE_RESTORE;
            phase_start = now;
        } else if (phase == Phase::PRE_RESTORE && phase_sec > 1.0) {
            phase = Phase::RESTORED;
            phase_start = now;
            LOG_INFO(LOG_MAIN, "=== RESTORE SETTLED ===");
            logSDL("AFTER-RESTORE", window);
        } else if (phase == Phase::RESTORED && phase_sec > 2.0) {
            phase = Phase::DONE;
            logSDL("FINAL", window);
        }

        if (total_sec > 12.0 || phase == Phase::DONE)
            running = false;

        // Process events
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
                compositor.resize(npw, nph);
                client->resize(npw, nph);
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

        // Pump CEF work
        App::DoWork();

        // Render frame: video behind, CEF compositor on top
        egl.makeCurrentMain();
        int vp_w, vp_h;
        SDL_GetWindowSizeInPixels(window, &vp_w, &vp_h);
        glViewport(0, 0, vp_w, vp_h);

        frameContext.beginFrame(0.0f, videoController.getClearAlpha());
        videoController.render(vp_w, vp_h);

        // Import CEF dmabuf if pending, then composite
        compositor.importQueuedDmabuf();
        if (compositor.hasValidOverlay())
            compositor.composite(vp_w, vp_h, 1.0f);

        frameContext.endFrame();

        mpv->processEvents();

        std::this_thread::sleep_for(std::chrono::milliseconds(16));
    }

    LOG_INFO(LOG_MAIN, "=== RESIZE TEST COMPLETE ===");

    // Cleanup
    videoController.setActive(false);
    videoController.stop();
    mpv->stop();
    mpv->cleanup();
    renderer->cleanup();
    VideoStack::cleanupStatics();
    compositor.cleanup();

    if (auto browser = client->browser())
        browser->GetHost()->CloseBrowser(true);
    // Give CEF a moment to process the close
    for (int i = 0; i < 10; i++) {
        App::DoWork();
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    CefShutdown();

    egl.cleanup();
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}
