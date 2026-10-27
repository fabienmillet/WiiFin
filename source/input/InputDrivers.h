/* The device drivers taken from libwiiinput (github.com/quatric/libwiiinput)
 * that Input calls into, and what they share. */
#ifndef WIIFIN_INPUTDRIVERS_H
#define WIIFIN_INPUTDRIVERS_H

#include <gctypes.h>
#include "Tablet.h"

#ifdef __cplusplus
extern "C" {
#endif

/* WpadQueue.c: room for a fourth Wii Remote and the Balance Board on
 * libogc 3.1 (linked with --wrap=wiiuse_init_cmd_queue) */
void indrv_wpadqueue_link(void);

/* UDraw.c: the uDraw GameTablet, for the TB_* calls in Tablet.c */
bool indrv_udraw_pop(tb_sample *out);
bool indrv_udraw_state(tb_sample *out);
bool indrv_udraw_info(tb_info *info);
void indrv_udraw_extent(s32 *w, s32 *h);

#ifdef __cplusplus
}
#endif

#endif
