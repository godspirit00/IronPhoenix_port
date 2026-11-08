#pragma once
#include "../common.h"

struct VideoConfig {
    int window_width = 1600;
    int window_height = 900;
    bool fullscreen = false;
    bool vsync = true;
    float aspect = 16.0f / 9.0f;  // output aspect; > 1.5 also sets the Xbox widescreen flag (game renders anamorphic)
    bool stretch_ui = false;      // widescreen: false = 2D UI kept at 4:3 proportions (centered), true = game's original stretched UI
    int surface_scale = 3;        // internal resolution multiplier (3 = 1920x1440) for the GPU emulation
    int shot_every_ms = 0;        // debug: save a screenshot every N ms (0 = off)
};
extern VideoConfig g_video;

// Create the window and GL contexts, start the GPU emulation. Main thread only.
void video_init();
// Message pump + presentation; never returns.
void video_main_loop();
