# mpv gpu-next HDR Rendering on Wayland

Reference documentation for how standalone mpv's gpu-next VO handles HDR rendering,
how our custom libmpv gpu-next path adapts it, and where the gaps are.

---

## 1. Standalone mpv: Wayland Color Management

### 1.1 Data Structures

**`vo_wayland_state` color management fields** (`wayland_common.h:92-111`):

```c
struct wp_color_manager_v1 *color_manager;
struct wp_color_management_surface_v1 *color_surface;
struct wp_color_management_surface_feedback_v1 *color_surface_feedback;
struct mp_image_params target_params;
bool supports_parametric;
bool supports_display_primaries;
int primaries_map[PL_COLOR_PRIM_COUNT];
int transfer_map[PL_COLOR_TRC_COUNT];
void *icc_file;
uint32_t icc_size;
struct pl_color_space preferred_csp;         // <-- KEY: compositor's preferred color space
```

**Temporary accumulator** (`wayland_common.c:289-297`):

```c
struct vo_wayland_preferred_description_info {
    struct vo_wayland_state *wl;
    struct pl_color_space csp;     // Populated field-by-field from protocol callbacks
    float min_luma, max_luma, ref_luma;
    void *icc_file;
    uint32_t icc_size;
};
```

### 1.2 Initialization

In `vo_wayland_init()` (`wayland_common.c:4220-4323`):

1. Default `preferred_csp` set to sRGB/BT.709
2. Color manager bound from registry with capability listener
3. Surface feedback created on `wl->callback_surface`
4. `get_compositor_preferred_description(wl)` called once (async)

### 1.3 Capability Discovery

Color manager listener (`wayland_common.c:2016-2101`):

- `supported_feature()` — registers `supports_parametric` and `supports_display_primaries`
- `supported_tf_named()` — maps Wayland TF enums to `pl_color_transfer` (sRGB, PQ, HLG, BT.1886, etc.)
- `supported_primaries_named()` — maps Wayland prim enums to `pl_color_primaries` (BT.709, BT.2020, P3, etc.)

### 1.4 Preferred Description Flow (Async)

**`preferred_changed` callback** (`wayland_common.c:2283-2301`):

```c
static void preferred_changed2(void *data, ...) {
    struct vo_wayland_state *wl = data;
    get_compositor_preferred_description(wl);  // re-request, non-blocking
}
```

**`get_compositor_preferred_description()`** (`wayland_common.c:3095-3111`):

```c
static void get_compositor_preferred_description(struct vo_wayland_state *wl) {
    // Allocate temporary accumulator
    struct vo_wayland_preferred_description_info *wd = talloc_zero(...);
    wd->wl = wl;

    // Request preferred description from compositor
    struct wp_image_description_v1 *desc =
        wp_color_management_surface_feedback_v1_get_preferred(wl->color_surface_feedback);
    struct wp_image_description_info_v1 *info =
        wp_image_description_v1_get_information(desc);

    // Attach listener — returns immediately, events arrive in main loop
    wp_image_description_info_v1_add_listener(info, &image_description_info_listener, wd);
    wp_image_description_v1_destroy(desc);
}
```

This is **async** — the listener fires later in the main event loop. No blocking dispatch.

### 1.5 Image Description Info Listener

All 10 callbacks (`wayland_common.c:2185-2281`):

| Callback | What it populates | Line |
|----------|-------------------|------|
| `info_icc_file` | ICC profile via mmap'd fd | 2185 |
| `info_primaries_named` | `wd->csp.primaries` | 2204 |
| `info_tf_named` | `wd->csp.transfer` | 2216 |
| `info_luminances` | `wd->min_luma`, `max_luma`, `ref_luma` | 2223 |
| `info_target_primaries` | `wd->csp.hdr.prim.{red,green,blue,white}.{x,y}` | 2232 |
| `info_target_luminance` | `wd->csp.hdr.{min,max}_luma` | 2247 |
| `info_target_max_cll` | `wd->csp.hdr.max_cll` | 2255 |
| `info_target_max_fall` | `wd->csp.hdr.max_fall` | 2261 |
| `info_done` | **Finalizes**: luminance scaling, stores `wl->preferred_csp` | 2136 |

### 1.6 info_done: Luminance Scaling

`wayland_common.c:2136-2183` — the critical finalization callback:

```c
static void info_done(void *data, struct wp_image_description_info_v1 *info) {
    struct vo_wayland_preferred_description_info *wd = data;
    wp_image_description_info_v1_destroy(info);

    if (!wd->icc_file) {
        // Scale Wayland's reference-relative luminance to libplacebo's absolute scale
        float a = wd->min_luma;
        float b = (PL_COLOR_SDR_WHITE - PL_COLOR_HDR_BLACK) / (wd->ref_luma - a);

        wd->csp.hdr.min_luma = (wd->csp.hdr.min_luma - a) * b + PL_COLOR_HDR_BLACK;
        wd->csp.hdr.max_luma = (wd->csp.hdr.max_luma - a) * b + PL_COLOR_HDR_BLACK;
        if (wd->csp.hdr.max_cll != 0)
            wd->csp.hdr.max_cll = (wd->csp.hdr.max_cll - a) * b + PL_COLOR_HDR_BLACK;
        if (wd->csp.hdr.max_fall != 0)
            wd->csp.hdr.max_fall = (wd->csp.hdr.max_fall - a) * b + PL_COLOR_HDR_BLACK;

        wd->csp.hdr.min_luma = MPMAX(wd->csp.hdr.min_luma, 0.0);

        // Snap near-SDR values
        if (fabsf(wd->csp.hdr.max_luma - PL_COLOR_SDR_WHITE) < 1e-2f) {
            wd->csp.hdr.max_luma = PL_COLOR_SDR_WHITE;
            // clamp cll/fall to max_luma
        }

        // Store as preferred color space
        wl->preferred_csp = wd->csp;

        // If HDR headroom exists but TRC isn't HDR, upgrade to PQ
        if (wd->csp.hdr.max_luma != PL_COLOR_SDR_WHITE &&
            !pl_color_transfer_is_hdr(wd->csp.transfer))
            wl->preferred_csp.transfer = PL_COLOR_TRC_PQ;
    } else {
        // ICC profile path
        wl->icc_file = wd->icc_file;
        wl->icc_size = wd->icc_size;
        wl->pending_vo_events |= VO_EVENT_ICC_PROFILE_CHANGED;
    }
    talloc_free(wd);
}
```

### 1.7 Public API

```c
struct pl_color_space vo_wayland_preferred_csp(struct vo *vo) {
    return wl->preferred_csp;  // Just read the cached value
}
```

Called per-frame by rendering contexts. No dispatch, no blocking.

### 1.8 Sending Color to Compositor

**`vo_wayland_handle_color()`** (`wayland_common.c:4182-4195`) — called from `swap_buffers`:

```c
void vo_wayland_handle_color(struct vo_wayland_state *wl) {
    struct mp_image_params target_params = vo_get_target_params(wl->vo);
    pl_color_space_infer(&target_params.color);
    if (/* unchanged */) return;
    wl->target_params = target_params;
    set_color_management(wl);
    set_color_representation(wl);
}
```

**`set_color_management()`** (`wayland_common.c:3471-3535`):
- Creates parametric image description from `wl->target_params.color`
- Sends primaries, transfer, mastering display primaries, luminance, CLL, FALL
- Validates HDR metadata against protocol constraints

**`set_color_representation()`** (`wayland_common.c:3537-3578`):
- Sends coefficients, range, alpha mode, chroma location

---

## 2. vo_gpu_next.c: Rendering Integration

### 2.1 Swapchain Color Space Hint

In `draw_frame()` (`vo_gpu_next.c:1089-1234`):

1. Get `target_csp` from swapchain (via `sw->fns->target_csp`)
2. Infer missing fields, strip HDR metadata if SDR
3. Build `hint` from source video color space + target capabilities
4. Apply user overrides (`target_prim`, `target_trc`, `target_peak`)
5. Call `pl_swapchain_colorspace_hint(p->sw, &hint)`

### 2.2 Target Parameters Publication

After render (`vo_gpu_next.c:1416-1425`):

```c
p->target_params = (struct mp_image_params){
    .color = pass_colorspace ? hint : target.color,
    .repr = target.repr,
    .rotate = target.rotation,
};
vo->target_params = &p->target_params;
```

This is read by `vo_wayland_handle_color()` on the next cycle to inform the compositor.

---

## 3. Custom libmpv gpu-next Path

### 3.1 Architecture

```
Host Application
    ↓
mpv_render_context_render()
    ↓
libmpv_gpu_next.c::render()        — dispatcher
    ├── swapchain path → pl_video_render_to_swapchain()
    └── FBO path       → pl_video_render()
                              ↓
                        render_to_target()   — shared rendering
```

### 3.2 Display Profile Struct

```c
typedef struct mpv_display_profile {
    float max_luma;       // Display peak luminance (nits)
    float min_luma;       // Display black level (nits)
    float ref_luma;       // Display reference white (nits)
    void *swapchain_out;  // Set by context.c → platform can re-hint
} mpv_display_profile;
```

### 3.3 Context Initialization

`context.c::libmpv_gpu_next_init_vk()` (lines 467-606):

1. Import Vulkan device into libplacebo
2. If `MPV_RENDER_PARAM_VULKAN_SURFACE` provided → create swapchain
3. If `MPV_RENDER_PARAM_DISPLAY_PROFILE` provided:
   - Store live pointer: `ctx->display_profile = dp`
   - Write swapchain back: `dp->swapchain_out = (void*)p->swapchain`
   - Initial HDR hint with luminance scaling:
     ```c
     float a = dp->min_luma;
     float b = (PL_COLOR_SDR_WHITE - PL_COLOR_HDR_BLACK) / (dp->ref_luma - a);
     hdr_hint.hdr.max_luma = (dp->max_luma - a) * b + PL_COLOR_HDR_BLACK;
     pl_swapchain_colorspace_hint(p->swapchain, &hdr_hint);
     ```

### 3.4 FBO Render Path

`video.c::pl_video_render()` (lines 595-646):

```c
void pl_video_render(struct pl_video *p, struct vo_frame *frame, pl_tex target_tex,
                     const mpv_display_profile *display_profile)
{
    // 1. Start with options-based target colorspace
    struct pl_color_space target_color = pl_color_space_srgb;
    if (opts->target_trc || opts->target_prim) {
        target_color = { opts->target_prim, opts->target_trc };
        // + target_peak, target_min_luma, target_max_cll, target_max_fall
    }

    // 2. Override with live display profile (per-frame)
    if (display_profile && display_profile->max_luma > 0 && display_profile->ref_luma > 0) {
        float a = display_profile->min_luma;
        float b = (PL_COLOR_SDR_WHITE - PL_COLOR_HDR_BLACK) / (display_profile->ref_luma - a);
        target_color.hdr.min_luma = (display_profile->min_luma - a) * b + PL_COLOR_HDR_BLACK;
        target_color.hdr.max_luma = (display_profile->max_luma - a) * b + PL_COLOR_HDR_BLACK;
        target_color.hdr.max_cll = target_color.hdr.max_luma;
        target_color.hdr.max_fall = target_color.hdr.max_luma;
    }

    // 3. Build target frame and render
    struct pl_frame target = { .color = target_color, .repr = pl_color_repr_rgb };
    render_to_target(p, frame, &target);
}
```

### 3.5 Swapchain Render Path

`video.c::pl_video_render_to_swapchain()` (lines 648-681):

```c
void pl_video_render_to_swapchain(struct pl_video *p, struct vo_frame *frame,
                                  const struct pl_swapchain_frame *sw_frame,
                                  const mpv_display_profile *display_profile)
{
    struct pl_frame target;
    pl_frame_from_swapchain(&target, sw_frame);  // Get color from swapchain

    // Override with display profile (same formula as FBO path)
    if (display_profile && display_profile->max_luma > 0 && display_profile->ref_luma > 0) {
        // ... same luminance scaling ...
    }

    render_to_target(p, frame, &target);
}
```

### 3.6 Shared Render: render_to_target()

`video.c:490-593`:

1. Set target crop (handle flipped FBO)
2. Set output levels to FULL
3. Convert target sRGB → gamma 2.2 (IEC 61966-2-1)
4. SDR gamma adjustment: if target is gamma-based and source has compatible TRC (BT.1886/gamma22/sRGB), use source TRC directly to avoid roundtrip
5. Push frame to queue → `map_frame()` uploads to GPU
6. Update queue at target PTS
7. Apply crops, render OSD overlays
8. Configure scalers, peak detection, dithering
9. `ra_next_render_image_mix()` — final GPU render

### 3.7 Source Frame Colorspace: map_frame()

`video.c:115-162`:

1. `mp_image_params_guess_csp()` — infer missing metadata
2. `ra_upload_mp_image()` — CPU→GPU upload
3. Apply guessed colorspace to frame
4. Convert source sRGB → gamma 2.2
5. Attach ICC profile, chroma location, film grain

---

## 4. Our Wayland Integration

### 4.1 What We Capture

From the compositor's preferred description, we only capture **3 fields**:

```cpp
.luminances = [](void* d, ..., uint32_t min, uint32_t max, uint32_t ref) {
    c->min_lum = min; c->max_lum = max; c->ref_lum = ref;
},
```

Everything else is ignored (empty lambdas):
`icc_file`, `primaries`, `primaries_named`, `tf_power`, `tf_named`,
`target_primaries`, `target_luminance`, `target_max_cll`, `target_max_fall`

### 4.2 Query Model: Synchronous Blocking

Our `queryDisplayProfile()` blocks on `wl_display_dispatch_queue()`:

```cpp
while (wl_display_dispatch_queue(wl_display_, queue) > 0)
    if (ic.done) break;
```

Standalone mpv's `get_compositor_preferred_description()` returns immediately —
the listener fires asynchronously in the main event loop.

### 4.3 Stale Flag + Polling

```
preferred_changed fires → display_profile_stale_ = true
next render frame → pollDisplayProfile()
  → dispatch feedback queue
  → if stale: queryDisplayProfile() (blocking)
  → drain feedback queue again (prevent storm)
  → clear stale
```

### 4.4 Two Render Paths

| | Dmabuf Path | Swapchain Path |
|---|---|---|
| **When** | `supports_parametric_` | Fallback |
| **Render context** | FBO (no swapchain) | Swapchain |
| **Presentation** | `wl_surface_attach` | libplacebo swapchain |
| **HDR signaling** | Explicit image description | Via swapchain format |
| **Target colorspace** | mpv options (`target-prim=bt.2020`, `target-trc=pq`) | From swapchain |
| **Display profile** | Passed via `MPV_RENDER_PARAM_DISPLAY_PROFILE` | Same |

### 4.5 HDR Signaling

`setHdrImageDescription()` — called once at init:
- Hardcoded PQ/BT.2020
- Luminances: 0-10000 nits, ref white = `PL_COLOR_SDR_WHITE`
- Never updated when compositor sends preferred_changed

---

## 5. Comparison: This Project vs Standalone mpv

| Aspect | This Project | Standalone mpv |
|--------|-------------|----------------|
| **Color info captured** | Luminance only (3 fields) | Full description (10 fields) |
| **Query model** | Sync blocking dispatch | Async listener |
| **preferred_changed** | Sets flag → poll next frame | Calls async re-request |
| **Per-frame cost** | Dispatch queue + conditional requery | Read cached `preferred_csp` |
| **Render paths** | 2 (dmabuf + swapchain) | 1 (swapchain) |
| **HDR signaling** | Explicit image description (static) | Via swapchain + `set_color_management()` |
| **Display profile struct** | `mpv_display_profile` (nits only) | `pl_color_space` (full) |
| **ICC profile** | Ignored | Parsed + cached |
| **Primaries** | Hardcoded BT.2020 | From compositor feedback |
| **TRC** | Hardcoded PQ | From compositor feedback |
| **Color representation** | Not implemented | Coefficients, range, alpha, chroma |
| **Target metadata** | Ignored | Parsed for tone mapping |
| **Sends to compositor** | Static PQ/BT.2020 desc | Dynamic per-frame desc from target_params |

---

## 6. Luminance Scaling Formula

Used identically in standalone mpv (`info_done`), our `queryDisplayProfile()`,
`context.c` (initial hint), and `video.c` (per-frame override):

```
a = min_luma                                          // display black level
b = (PL_COLOR_SDR_WHITE - PL_COLOR_HDR_BLACK) / (ref_luma - a)   // scale factor

scaled = (raw - a) * b + PL_COLOR_HDR_BLACK
```

Maps display's reference-relative luminance to libplacebo's absolute scale
where SDR white = `PL_COLOR_SDR_WHITE` (203 nits).

---

## 7. Complete Data Flow

### Standalone mpv

```
Compositor
  → preferred_changed event
  → get_compositor_preferred_description() [async, returns immediately]
  → info_* callbacks populate pl_color_space
  → info_done: scale luminance, store in wl->preferred_csp
  → vo_wayland_preferred_csp() [per-frame read, no dispatch]
  → vo_gpu_next.c: build hint from preferred_csp + source color
  → pl_swapchain_colorspace_hint()
  → render with display-aware tone mapping
  → vo_wayland_handle_color(): send target_params back to compositor
```

### This Project

```
Compositor
  → preferred_changed event (on feedback_queue_)
  → sets display_profile_stale_ = true
  → next render frame: pollDisplayProfile()
    → dispatch feedback_queue_
    → queryDisplayProfile() [sync blocking dispatch]
    → store 3 nits values in display_profile_
  → pl_video_render/pl_video_render_to_swapchain reads display_profile per-frame
  → luminance scaling applied to target frame
  → render with display-aware tone mapping
  → (no color sent back to compositor)
```
