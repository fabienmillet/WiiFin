/* USB keyboard, from libwiiinput (github.com/quatric/libwiiinput).
 *
 * A thread keeps the keyboard connected and scanned, so it can be plugged
 * in at any time.  Its keys work as a controller (Priiloader's layout):
 *
 *   arrows = D-pad, Enter = A, Backspace = B, X = 1, Y = 2,
 *   Delete / Page Up = -, Page Down = +, Esc / Home = HOME
 *
 * and every press comes out of KB_Pop with the character it types. */
#ifndef WIIFIN_USBKEYBOARD_H
#define WIIFIN_USBKEYBOARD_H

#include <gctypes.h>

#ifdef __cplusplus
extern "C" {
#endif

#define KB_MOD_SHIFT (1 << 0)
#define KB_MOD_CTRL  (1 << 1)
#define KB_MOD_ALT   (1 << 2)
#define KB_MOD_GUI   (1 << 3)

typedef struct {
    u8   code;   /* USB HID usage code (0x28 Enter, 0x2A Backspace) */
    char ch;     /* printable ASCII for the press (shift/caps aware), else 0 */
    u8   mods;   /* KB_MOD_* held at the time of the press */
} kb_key;

/* USB must be up (USB_Initialize). */
s32  KB_Init(void);
void KB_Shutdown(void);
/* Once a frame: what was pressed since the last one. */
void KB_Update(void);
bool KB_Connected(void);
/* The next key press (modifier keys alone are not reported); false when
 * there is none left.  Holding a key does not repeat. */
bool KB_Pop(kb_key *ev);

/* Text entry: the keys that type leave the controller layout. */
void KB_SetTyping(bool on);
/* The keyboard as Wii Remote buttons (WPAD_BUTTON_*). */
u32  KB_PadHeld(void);
u32  KB_PadDown(void);

#ifdef __cplusplus
}
#endif

#endif
