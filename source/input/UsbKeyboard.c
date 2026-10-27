/* USB keyboard, from libwiiinput (github.com/quatric/libwiiinput): a
 * thread keeps it connected and scanned (hot-plug safe), the frame loop
 * takes a snapshot of the keys once a frame.  See UsbKeyboard.h. */
#include <string.h>
#include <unistd.h>
#include <ogc/lwp.h>
#include <ogc/mutex.h>
#include <wiiuse/wpad.h>
#include <wiikeyboard/usbkeyboard.h>
#include "UsbKeyboard.h"

#define KEY_BYTES (256 / 8)
#define QUEUE_LEN 64

#define BIT_GET(a, k) ((a)[(k) >> 3] & (1 << ((k) & 7)))
#define BIT_SET(a, k) ((a)[(k) >> 3] |= (1 << ((k) & 7)))
#define BIT_CLR(a, k) ((a)[(k) >> 3] &= ~(1 << ((k) & 7)))

/* Written by the scan thread (via the event callback) under kbd_lock. */
static u8 live[KEY_BYTES];
static u8 pressed_acc[KEY_BYTES];
static u8 released_acc[KEY_BYTES];
static kb_key queue[QUEUE_LEN];
static int q_head, q_tail;
static bool caps;

/* Snapshot published by KB_Update(). */
static u8 held[KEY_BYTES];
static u8 pressed[KEY_BYTES];

static u32 key_map[256];

static mutex_t kbd_lock = LWP_MUTEX_NULL;
static lwp_t kbd_thread_hndl = LWP_THREAD_NULL;
static volatile bool kbd_thread_should_run;
static bool kbd_initialized;

static const char plain[]   = "abcdefghijklmnopqrstuvwxyz1234567890\n\x1b\b\t -=[]\\#;'`,./";
static const char shifted[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ!@#$%^&*()\n\x1b\b\t _+{}|~:\"~<>?";

static u8 live_mods(void)
{
    u8 m = 0;
    if (BIT_GET(live, 0xE1) || BIT_GET(live, 0xE5)) m |= KB_MOD_SHIFT;
    if (BIT_GET(live, 0xE0) || BIT_GET(live, 0xE4)) m |= KB_MOD_CTRL;
    if (BIT_GET(live, 0xE2) || BIT_GET(live, 0xE6)) m |= KB_MOD_ALT;
    if (BIT_GET(live, 0xE3) || BIT_GET(live, 0xE7)) m |= KB_MOD_GUI;
    return m;
}

static char key_char(u8 code, bool shift)
{
    char c = 0;

    if (code >= 0x04 && code <= 0x38) {
        c = (shift ? shifted : plain)[code - 0x04];
        if (code <= 0x1D && caps)
            c = (c >= 'a' && c <= 'z') ? c - 32 : c + 32;
    } else if (code >= 0x59 && code <= 0x61) {
        c = '1' + (code - 0x59);
    } else {
        switch (code) {
        case 0x54: c = '/'; break;
        case 0x55: c = '*'; break;
        case 0x56: c = '-'; break;
        case 0x57: c = '+'; break;
        case 0x62: c = '0'; break;
        case 0x63: c = '.'; break;
        }
    }
    return (c >= 0x20 && c < 0x7F) ? c : 0;
}

/* Runs on the scan thread, inside USBKeyboard_Scan(). */
static void kb_event_locked(USBKeyboard_event event, bool *caps_changed)
{
    u8 k = event.keyCode;

    if (event.type == USBKEYBOARD_PRESSED) {
        bool fresh = !BIT_GET(live, k);
        BIT_SET(live, k);
        if (!fresh)
            return;
        BIT_SET(pressed_acc, k);

        if (k == 0x39) { /* Caps Lock */
            caps = !caps;
            *caps_changed = true;
            return;
        }
        if (k >= 0xE0 && k <= 0xE7)
            return;

        int next = (q_head + 1) % QUEUE_LEN;
        if (next != q_tail) {
            u8 mods = live_mods();
            queue[q_head].code = k;
            queue[q_head].mods = mods;
            queue[q_head].ch = (mods & KB_MOD_CTRL) ? 0 : key_char(k, mods & KB_MOD_SHIFT);
            q_head = next;
        }
    } else if (event.type == USBKEYBOARD_RELEASED) {
        if (BIT_GET(live, k))
            BIT_SET(released_acc, k);
        BIT_CLR(live, k);
    }
}

/* Every held key is released when the keyboard goes away. */
static void release_all_locked(void)
{
    for (int i = 0; i < 256; i++) {
        if (BIT_GET(live, i))
            BIT_SET(released_acc, i);
    }
    memset(live, 0, sizeof(live));
}

static volatile bool kbd_lost;

static void kb_event(USBKeyboard_event event)
{
    bool caps_changed = false;

    /* The disconnect event comes from an IPC callback, where a mutex cannot
     * be taken; the scan thread releases the keys. */
    if (event.type == USBKEYBOARD_DISCONNECTED) {
        kbd_lost = true;
        return;
    }

    LWP_MutexLock(kbd_lock);
    kb_event_locked(event, &caps_changed);
    LWP_MutexUnlock(kbd_lock);

    if (caps_changed)
        USBKeyboard_SetLed(USBKEYBOARD_LEDCAPS, caps);
}

static void *kbd_thread(void *arg)
{
    (void)arg;

    while (kbd_thread_should_run) {
        if (kbd_lost) {
            kbd_lost = false;
            LWP_MutexLock(kbd_lock);
            release_all_locked();
            LWP_MutexUnlock(kbd_lock);
        }

        if (!USBKeyboard_IsConnected()) {
            /* Looking for a keyboard walks the USB device list through IOS, so
             * only do it a few times a second. */
            if (USBKeyboard_Open(kb_event) <= 0) {
                usleep(250000);
                continue;
            }
            caps = false;
            for (int i = 0; i < 3; i++) {
                USBKeyboard_SetLed(i, 1);
                usleep(120000);
                USBKeyboard_SetLed(i, 0);
            }
        }

        /* Scan() blocks in IOS until the keyboard sends its next report, so it
         * must not run under kbd_lock: the frame loop takes that lock in
         * KB_Update() and KB_Pop(). kb_event() locks for each
         * event instead. */
        if (USBKeyboard_Scan() < 0)
            usleep(10000);
        else
            usleep(400);
    }

    return NULL;
}

static void map_clear(void)
{
    memset(key_map, 0, sizeof(key_map));
}

static void map_set(u8 keycode, u32 button)
{
    key_map[keycode] = button;
}

static void map_defaults(void)
{
    map_clear();
    map_set(0x52, WPAD_BUTTON_UP);
    map_set(0x51, WPAD_BUTTON_DOWN);
    map_set(0x50, WPAD_BUTTON_LEFT);
    map_set(0x4F, WPAD_BUTTON_RIGHT);
    map_set(0x28, WPAD_BUTTON_A);     /* Enter */
    map_set(0x58, WPAD_BUTTON_A);     /* Enter (numpad) */
    map_set(0x2A, WPAD_BUTTON_B);     /* Backspace */
    map_set(0x1B, WPAD_BUTTON_1);     /* X */
    map_set(0x1C, WPAD_BUTTON_2);     /* Y */
    map_set(0x4C, WPAD_BUTTON_MINUS); /* Delete */
    map_set(0x4B, WPAD_BUTTON_MINUS); /* Page Up */
    map_set(0x4E, WPAD_BUTTON_PLUS);  /* Page Down */
    map_set(0x29, WPAD_BUTTON_HOME);  /* Esc */
    map_set(0x4A, WPAD_BUTTON_HOME);  /* Home */
}

s32 KB_Init(void)
{
    if (kbd_initialized)
        return 0;

    s32 ret = USBKeyboard_Initialize();
    if (ret < 0)
        return ret;

    if (LWP_MutexInit(&kbd_lock, false) < 0) {
        USBKeyboard_Deinitialize();
        return -1;
    }

    memset(live, 0, sizeof(live));
    memset(pressed_acc, 0, sizeof(pressed_acc));
    memset(released_acc, 0, sizeof(released_acc));
    memset(held, 0, sizeof(held));
    memset(pressed, 0, sizeof(pressed));
    q_head = q_tail = 0;
    caps = false;
    map_defaults();

    kbd_thread_should_run = true;
    ret = LWP_CreateThread(&kbd_thread_hndl, kbd_thread, NULL, NULL, 0x4000, 0x7F);
    if (ret < 0) {
        kbd_thread_should_run = false;
        LWP_MutexDestroy(kbd_lock);
        kbd_lock = LWP_MUTEX_NULL;
        USBKeyboard_Deinitialize();
        return ret;
    }

    kbd_initialized = true;
    return 0;
}

void KB_Shutdown(void)
{
    if (!kbd_initialized)
        return;

    kbd_thread_should_run = false;
    if (kbd_thread_hndl != LWP_THREAD_NULL)
        LWP_JoinThread(kbd_thread_hndl, NULL);
    kbd_thread_hndl = LWP_THREAD_NULL;

    USBKeyboard_Close();
    USBKeyboard_Deinitialize();
    LWP_MutexDestroy(kbd_lock);
    kbd_lock = LWP_MUTEX_NULL;
    kbd_initialized = false;
}

void KB_Update(void)
{
    if (!kbd_initialized)
        return;

    LWP_MutexLock(kbd_lock);
    memcpy(held, live, sizeof(held));
    memcpy(pressed, pressed_acc, sizeof(pressed));
    memset(pressed_acc, 0, sizeof(pressed_acc));
    memset(released_acc, 0, sizeof(released_acc));
    LWP_MutexUnlock(kbd_lock);
}

bool KB_Connected(void)
{
    return kbd_initialized && USBKeyboard_IsConnected();
}

bool KB_Pop(kb_key *ev)
{
    bool ok = false;

    if (!kbd_initialized)
        return false;

    LWP_MutexLock(kbd_lock);
    if (q_tail != q_head) {
        if (ev)
            *ev = queue[q_tail];
        q_tail = (q_tail + 1) % QUEUE_LEN;
        ok = true;
    }
    LWP_MutexUnlock(kbd_lock);
    return ok;
}

static bool typing;

void KB_SetTyping(bool on) { typing = on; }

/* While typing, the keys that type (and Backspace, Enter) only go to
 * KB_Pop, and Esc is B: the arrows still move around. */
static bool types(int k)
{
    return (k >= 0x04 && k <= 0x38 && k != 0x29) || (k >= 0x54 && k <= 0x63);
}

static u32 pad_map(const u8 *keys)
{
    u32 out = 0;

    for (int i = 0; i < 256; i++) {
        if (!key_map[i] || !BIT_GET(keys, i))
            continue;
        if (typing && types(i))
            continue;
        out |= (typing && i == 0x29) ? WPAD_BUTTON_B : key_map[i];
    }
    return out;
}

u32 KB_PadHeld(void) { return pad_map(held); }
u32 KB_PadDown(void) { return pad_map(pressed); }
