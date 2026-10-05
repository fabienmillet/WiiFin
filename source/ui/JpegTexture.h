#pragma once
#include <grrlib.h>

/* Decode a JPEG into an RGBA8 texture; nullptr on bad data or no memory. */
GRRLIB_texImg* loadJPEGTexture(const u8* data, u32 size);
