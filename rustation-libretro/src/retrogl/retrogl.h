// PlayStation OpenGL 3.3 renderer playing nice with libretro
#ifndef RETROGL_H
#define RETROGL_H

#include "libretro.h"
#include <glsm/glsmsym.h>

#include "../renderer/GlRenderer.h"

extern unsigned int VRAM_WIDTH_PIXELS;
extern unsigned int VRAM_HEIGHT;

const size_t VRAM_PIXELS = (size_t) VRAM_WIDTH_PIXELS * (size_t) VRAM_HEIGHT;

class RetroGl {
public:
    /* 
    Rust's enums members can contain data. To emulate that,
    I'll use a helper struct to save the data.  
    */
    GlStateData state_data;
    GlState state;
    VideoClock video_clock;

    // new(video_clock: VideoClock)
    RetroGl(VideoClock video_clock);
    ~RetroGl();

    void context_reset();
    GlRenderer* gl_renderer();
    void context_destroy();
    void prepare_render();
    void finalize_frame();
    void refresh_variables();
    retro_system_av_info get_system_av_info();
};

/// State machine dealing with OpenGL context
/// destruction/reconstruction
enum class GlState {
    // OpenGL context is ready
    Valid,
    /// OpenGL context has been destroyed (or is not created yet)
    Invalid
};

struct GlStateData {
    GlRenderer* r;
    DrawConfig c;
};

struct DrawConfig {
    uint16_t display_top_left[2];
    uint16_t display_resolution[2];
    bool display_24bpp;
    int16_t draw_offset[2];
    uint16_t draw_area_top_left[2];
    uint16_t draw_area_dimensions[2];
    uint16_t vram[VRAM_PIXELS];
};

#endif
