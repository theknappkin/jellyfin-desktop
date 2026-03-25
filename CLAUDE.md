# Project Notes

## Constraints
- **No hand-rolled JSON** — never manually construct or parse JSON with string concatenation, manual escaping, or homebrew parsers. Always use a proper JSON library or API (e.g. CEF's `CefParseJSON`/`CefWriteJSON`, or a vendored library if CEF isn't available in that context).
- **No artificial heartbeats/polling** - event-driven architecture only. Never use timeouts as a workaround for proper event integration. No arbitrary timeout-based bailouts in shutdown paths either — fix the root cause instead.
- **No debouncing** — debouncing is a hack that masks the real problem. If events fire too many times, fix the event dispatch (e.g. use a separate event queue like standalone mpv does), don't paper over it with timers or flags.
- **No texture stretching during resize** - CEF content must always render at 1:1 pixel mapping. Never scale/stretch textures to fill the viewport. Gaps from stale texture sizes are acceptable; stretching is not.
- **No hand-rolled math that libraries already provide** — never reimplement luminance scaling, color space conversions, or protocol value encoding that libplacebo or mpv already compute. Delegate to mpv or libplacebo code rather than duplicating logic. If you're finding yourself duplicating logic, there's likely something from mpv/libplacebo that could be used instead. Rounding errors from DIY math are unacceptable.
- **HDR Wayland protocol values must match standalone mpv** — the `wp_color_management` / `wp_image_description` protocol messages our app sends (luminances, mastering metadata, CLL, primaries) must be identical to what standalone mpv produces for the same content on the same display, as verified by `WAYLAND_DEBUG=1` comparison. Use `dev/hdr_test.cpp` + `dev/hdr_compare.sh` to validate. No "close enough" — exact match.

## Build
```
cmake --build build
```

## Architecture
- CEF (Chromium Embedded Framework) for web UI
- mpv via libmpv for video playback
- Vulkan rendering with libplacebo (gpu-next backend)
- Wayland subsurface for video layer

## mpv Integration
- Custom libmpv gpu-next path in `third_party/mpv/video/out/gpu_next/`
- `video.c` - main rendering, uses `map_scaler()` for proper filtering
- `context.c` - Vulkan FBO wrapping for libmpv
- `libmpv_gpu_next.c` - render backend glue
- **Never call sync mpv API (`mpv_get_property`, etc.) from event callbacks** - causes deadlock during video init. Use property observation or async variants instead.

## mpv Event Flow
mpv is the authoritative source of playback state. All state (position, speed, pause, seeking, etc.) flows from mpv property observations outward to the JS UI and OS media sessions. The JS UI and MPRIS/macOS media sessions are consumers — they never determine playback state, they only reflect what mpv reports. This means things like rate changes, seek completion, and position updates come from mpv, not from JS round-trips or manual bookkeeping.

## Debugging
- For mpv (third_party/mpv) and jellyfin-web: investigate source code directly before suggesting debug logs that require manual user action
