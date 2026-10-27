/* SPDX-FileCopyrightText: 2026 RiftWii contributors */
/* SPDX-License-Identifier: GPL-3.0-or-later */
/* The GameCube controller adapter for Wii U (WUP-028), from libwiiinput
 * (which took it from RiftWii, github.com/KakarottoCake/Riftwii).
 *
 * Nintendo's adapter and the third-party ones that copy it, in either mode
 * (Wii U / PC).  It is looked for every two seconds, so it can be plugged
 * in at any time; Input reads its four ports like the console's own
 * GameCube ports.  Sticks centre on where they rest when a controller is
 * plugged in; X+Y+Start held for three seconds takes the centre again, as
 * on the console.
 */
#ifndef WIIFIN_GCADAPTER_H
#define WIIFIN_GCADAPTER_H

#include <gctypes.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    u16  buttons;                  /* PAD_BUTTON_* / PAD_TRIGGER_* */
    s8   stick_x, stick_y;         /* from the centre, y up, about -100..100 */
    s8   substick_x, substick_y;
    u8   trigger_l, trigger_r;     /* 0..255 */
    bool wireless;                 /* a WaveBird receiver: no rumble */
} gca_pad;

/* USB must be up (USB_Initialize); enabled false leaves the adapter alone. */
void indrv_gcadapter_init(bool enabled);
void indrv_gcadapter_shutdown(void);
/* Once a frame: looks for the adapter, or reads it. */
void indrv_gcadapter_update(void);

/* True while the adapter is plugged in and reporting. */
bool GCA_Connected(void);
/* Port 0-3 as of the last update; false when nothing is plugged into it. */
bool GCA_Pad(int port, gca_pad *out);
/* Rumble needs the adapter's power (grey) cable plugged in. */
void GCA_Rumble(int port, bool on);
/* Where the adapter's steps and errors go. */
void GCA_SetLog(int (*print)(const char *fmt, ...));

#ifdef __cplusplus
}
#endif

#endif
