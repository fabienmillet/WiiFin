#include "JpegTexture.h"
#include <stdio.h>
#include <string.h>
#include <malloc.h>
#include <setjmp.h>
#include <jpeglib.h>

// libjpeg error manager with setjmp so corrupt/bad JPEG data never reaches
// the default error_exit which calls exit() and causes an invalid write crash.
struct JpegErrMgr {
    struct jpeg_error_mgr pub;
    jmp_buf               buf;
};
static void jpegErrExit(j_common_ptr cinfo) {
    longjmp(((JpegErrMgr*)cinfo->err)->buf, 1);
}
// Suppress all libjpeg diagnostic output to prevent the default
// output_message → fprintf(stderr,...) path, which triggers setvbuf → malloc
// from the worker thread and corrupts the heap allocator state.
static void jpegNoOp(j_common_ptr) {}

GRRLIB_texImg* loadJPEGTexture(const u8* data, u32 size) {
    // Reject immediately if data doesn't start with the JPEG SOI marker.
    if (size < 3 || data[0] != 0xFF || data[1] != 0xD8 || data[2] != 0xFF)
        return nullptr;

    struct jpeg_decompress_struct cinfo __attribute__((aligned(32)));
    JpegErrMgr jerr __attribute__((aligned(32)));
    // Strip buffer: 4 scanlines at a time — avoids a large w*h*3 intermediate
    // allocation and the associated heap pressure / fragmentation.
    unsigned char* strip = nullptr;
    GRRLIB_texImg* tex   = nullptr;

    // err MUST be set before jpeg_create_decompress
    cinfo.err = jpeg_std_error(&jerr.pub);
    jerr.pub.error_exit    = jpegErrExit;
    jerr.pub.output_message = jpegNoOp; // suppress fprintf → setvbuf → malloc

    // Any libjpeg error longjmps here; clean up and return nullptr
    if (setjmp(jerr.buf)) {
        jpeg_destroy_decompress(&cinfo);
        free(strip);
        if (tex) { free(tex->data); free(tex); }
        return nullptr;
    }

    jpeg_create_decompress(&cinfo);
    cinfo.progress = nullptr;
    jpeg_mem_src(&cinfo, data, size);
    jpeg_read_header(&cinfo, TRUE);
    // Always request RGB output regardless of source color space.
    cinfo.out_color_space = JCS_RGB;
    // Speed over exactness: the images are small and already resized by the
    // server, so the integer DCT and plain chroma upsampling are not visible.
    cinfo.dct_method          = JDCT_IFAST;
    cinfo.do_fancy_upsampling = FALSE;
    jpeg_start_decompress(&cinfo);

    u32 w  = cinfo.output_width;
    u32 h  = cinfo.output_height;
    u32 nc = (u32)cinfo.output_components; // 3 for JCS_RGB
    if (w == 0 || h == 0 || w > 2048 || h > 2048 || nc != 3) {
        jpeg_abort_decompress(&cinfo);
        jpeg_destroy_decompress(&cinfo);
        return nullptr;
    }

    // Allocate the GX texture buffer first (32-byte aligned, correct tile size)
    tex = (GRRLIB_texImg*)calloc(1, sizeof(GRRLIB_texImg));
    if (!tex) {
        jpeg_abort_decompress(&cinfo);
        jpeg_destroy_decompress(&cinfo);
        return nullptr;
    }
    u32 bufsize = GX_GetTexBufferSize(w, h, GX_TF_RGBA8, 0, 0);
    tex->data = memalign(32, bufsize);
    if (!tex->data) {
        free(tex); tex = nullptr;
        jpeg_abort_decompress(&cinfo);
        jpeg_destroy_decompress(&cinfo);
        return nullptr;
    }

    // Decode in 4-scanline strips and write directly to GX RGBA8 tile layout.
    // Peak extra allocation: w*4*3 bytes (≤2880 B for 240-wide posters) vs the
    // old w*h*3 approach (up to 244 KB) that caused heap fragmentation crashes.
    strip = (unsigned char*)malloc(w * 4 * nc);
    if (!strip) {
        free(tex->data); free(tex); tex = nullptr;
        jpeg_abort_decompress(&cinfo);
        jpeg_destroy_decompress(&cinfo);
        return nullptr;
    }

    u8* tileData = (u8*)tex->data;
    for (u32 by = 0; by < h; by += 4) {
        int nrows = (int)(h - by);
        if (nrows > 4) nrows = 4;

        // Point each row-pointer into the strip buffer
        JSAMPROW rp[4];
        for (int i = 0; i < 4; i++)
            rp[i] = strip + (u32)i * w * nc;

        // Read nrows scanlines (libjpeg may deliver them one at a time)
        int done = 0;
        while (done < nrows && cinfo.output_scanline < h)
            done += (int)jpeg_read_scanlines(&cinfo, rp + done, (JDIMENSION)(nrows - done));

        // Convert strip to GX RGBA8 tile format (in-place, tile-by-tile)
        for (u32 bx = 0; bx < w; bx += 4) {
            // AR sub-block (alpha + red for all 16 texels in this 4×4 tile)
            for (u8 r = 0; r < 4; r++) {
                for (u8 c = 0; c < 4; c++) {
                    u32 sx = bx + c;
                    u8  red = (sx < w && r < (u8)nrows)
                              ? strip[((u32)r * w + sx) * nc] : 0;
                    *tileData++ = 0xFF; // alpha
                    *tileData++ = red;
                }
            }
            // GB sub-block (green + blue for same 16 texels)
            for (u8 r = 0; r < 4; r++) {
                for (u8 c = 0; c < 4; c++) {
                    u32 sx = bx + c;
                    u8 g = 0, b = 0;
                    if (sx < w && r < (u8)nrows) {
                        g = strip[((u32)r * w + sx) * nc + 1];
                        b = strip[((u32)r * w + sx) * nc + 2];
                    }
                    *tileData++ = g;
                    *tileData++ = b;
                }
            }
        }
    }

    free(strip); strip = nullptr;
    jpeg_finish_decompress(&cinfo);
    jpeg_destroy_decompress(&cinfo);

    tex->w      = w;
    tex->h      = h;
    tex->format = GX_TF_RGBA8;
    GRRLIB_SetHandle(tex, 0, 0);
    GRRLIB_FlushTex(tex);
    return tex;
}
