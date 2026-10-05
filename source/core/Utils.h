#pragma once
#include <ogc/conf.h>

// 16:9 detection.  The Wii frame is 640x480; a 16:9 TV stretches it by 4/3.
// Ui::initScreen() widens the drawing space to match, so everything keeps
// its proportions and nothing needs pre-squishing any more: wsScaleX() is 1
// and stays only for the call sites written against it.
namespace WiiUtils {
    extern bool widescreen;
    inline void detectAspect() {
        widescreen = (CONF_GetAspectRatio() == CONF_ASPECT_16_9);
    }
    // Horizontal scale to apply to drawn textures when in 16:9 mode
    inline float wsScaleX() { return 1.0f; }
}