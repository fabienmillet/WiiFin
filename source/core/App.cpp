#include "App.h"
#include "AppInternal.h"
#include "ExitZone.h"
#include "Text.h"
#include "../ui/Ui.h"
#include "Utils.h"
#include "Input.h"
#include "MusicBGM.h"
#include "SoundFX.h"
#include "../ui/ConnectView.h"
#include "../ui/MusicPlayerView.h"
#include "../ui/ProfileView.h"
#include "../ui/SettingsView.h"
#include "../ui/LibraryView.h"
#include "../jellyfin/JellyfinClient.h"

#include <grrlib.h>
#include <wiiuse/wpad.h>
#include <fat.h>
#include <sdcard/wiisd_io.h>
#include <errno.h>
#include <stdio.h>
#include <gccore.h>
#include <sys/iosupport.h>

#include <unistd.h>
#include <sys/stat.h>
#include <string>
#include "Log.h"
#include "../version.h"
#include <ogc/ios.h>
#include <ogc/lwp_watchdog.h>

/* Ring spinner PNG embedded asset (music player transition frames) */
extern unsigned char data_ring_png[];
extern unsigned int  data_ring_png_len;

volatile bool g_app_powerOff = false;
volatile bool g_app_reset    = false;
volatile bool g_restartApp = false;
static void onPower() { g_app_powerOff = true; }
static void onReset(u32, void*) { g_app_reset = true; }

/* Full-screen error card: title, two lines of explanation.  With retry, A
 * retries (returns true) and B goes back; without, A or B closes it. */
bool showErrorScreen(const char* title, const std::string& line1,
                     const std::string& line2, bool retry, ir_t& ir)
{
    const Ui::Palette& p = Ui::pal();
    for (;;) {
        Input::update();
        Input::readIR(ir);
        if (g_app_powerOff || g_app_reset) return false;
        if (Input::isBackPressed() || (!retry && Input::isAJustPressed())) return false;
        if (retry && Input::isAJustPressed()) { SoundFX::play(SoundFX::FX::Start); return true; }
        Ui::background(false);
        Ui::card(80, 150, 480, 130, 18, 0.0f);
        Ui::circle(114, 182, 13, p.danger);
        Ui::textCentered(114, 172, "!", 18, 0xFFFFFFFF);
        Ui::text(138, 170, title, 20, p.danger);
        Ui::text(100, 210, line1.c_str(), 15, p.text);
        Ui::text(100, 236, line2.c_str(), 13, p.textDim);
        Ui::textRight(546, 258, "WiiFin v" WIIFIN_VERSION, 11, p.textDim);   /* for bug reports */
        if (retry) {
            const Ui::Hint l[] = { { "A", "Retry" } };
            const Ui::Hint r[] = { { "B", "Back" } };
            Ui::bottomBar(l, 1, r, 1);
        } else {
            const Ui::Hint r[] = { { "A", "OK" } };
            Ui::bottomBar(nullptr, 0, r, 1);
        }
        GRRLIB_Render();
    }
}

extern unsigned char data_logo_wiifin_png[];
extern unsigned int data_logo_wiifin_png_len;
extern unsigned char data_button_start_png[];
extern unsigned int data_button_start_png_len;
extern unsigned char data_cursors_PointerP1_64_png[];
extern unsigned int data_cursors_PointerP1_64_png_len;
extern unsigned char data_cursors_HandOpenP1_64_png[];
extern unsigned int data_cursors_HandOpenP1_64_png_len;
extern unsigned char data_cursors_HandClosedP1_64_png[];
extern unsigned int data_cursors_HandClosedP1_64_png_len;
extern unsigned char data_wii_font_ttf[];
extern unsigned int data_wii_font_ttf_len;
extern unsigned char data_jp_font_ttf[];
extern unsigned int data_jp_font_ttf_len;

#define logo_wiifin_png      data_logo_wiifin_png
#define logo_wiifin_png_len  data_logo_wiifin_png_len
#define button_start_png      data_button_start_png
#define button_start_png_len  data_button_start_png_len
#define wii_font_ttf      data_wii_font_ttf
#define wii_font_ttf_len  data_wii_font_ttf_len
#define jp_font_ttf       data_jp_font_ttf
#define jp_font_ttf_len   data_jp_font_ttf_len

// --- Button layout constants ---
// btnTex is 512x128 (power-of-2 required by GX/GRRLIB).
// Display at 280x70 (4:1 ratio preserved, sx=sy=0.547).
static const int BX        = 170;
static const int BW        = 300;
static const int BH        = 58;
static const int BY_START  = 168;
static const int B_SPACING = 74;

void App::init(const char* argv0) {
    SYS_SetPowerCallback(onPower);
    SYS_SetResetCallback(onReset);

    SYS_Report("[WiiFin] init start\n");

    {
        VIDEO_Init();
        SYS_Report("[WiiFin] VIDEO_Init done\n");
        GXRModeObj* m = VIDEO_GetPreferredMode(NULL);
        /* TV standard (0 NTSC, 1 PAL 50 Hz, 2 MPAL, 5 PAL 60 Hz) and sizes */
        SYS_Report("[WiiFin] video mode: tv=%u fb=%ux%u efb=%u vi=%ux%u%s\n",
                   (unsigned)(m->viTVMode >> 2), (unsigned)m->fbWidth, (unsigned)m->xfbHeight,
                   (unsigned)m->efbHeight, (unsigned)m->viWidth, (unsigned)m->viHeight,
                   (m->viTVMode & 3) == VI_PROGRESSIVE ? " progressive" : "");
        void* xfb = MEM_K0_TO_K1(SYS_AllocateFramebuffer(m));
        VIDEO_ClearFrameBuffer(m, xfb, 0x00800080); // YCbCr black — prevents green garbage frame
        VIDEO_Configure(m);
        VIDEO_SetNextFramebuffer(xfb);
        VIDEO_SetBlack(false);  // unblank so Dolphin's renderer connects to VI output
        VIDEO_Flush();
        SYS_Report("[WiiFin] VIDEO_Flush done, waiting 50ms\n");
        usleep(50000);
    }

    {
        static u8 s_pre_fifo[256 * 1024] DEAD_AT_EXIT ATTRIBUTE_ALIGN(32);
        SYS_Report("[WiiFin] GX_Init start\n");
        GX_Init(s_pre_fifo, sizeof(s_pre_fifo));
        SYS_Report("[WiiFin] GX_AbortFrame start\n");
        GX_AbortFrame();
        GX_Flush();
        SYS_Report("[WiiFin] GX pre-init done\n");
    }

    SYS_Report("[WiiFin] GRRLIB_Init start\n");
    GRRLIB_Init();
    SYS_Report("[WiiFin] GRRLIB_Init done\n");

    // Clear both framebuffers to black immediately to avoid green garbage frame
    GRRLIB_FillScreen(0x000000FF);
    GRRLIB_Render();
    GRRLIB_FillScreen(0x000000FF);
    GRRLIB_Render();

    WiiUtils::detectAspect();
    Ui::initScreen(WiiUtils::widescreen);

    // Load textures and fonts from embedded data (no file I/O, always fast)
    logoTex   = GRRLIB_LoadTexture(logo_wiifin_png);
    btnTex    = GRRLIB_LoadTexture(button_start_png);
    cursorPointerTex    = GRRLIB_LoadTexture(data_cursors_PointerP1_64_png);
    font   = GRRLIB_LoadTTF(wii_font_ttf, wii_font_ttf_len);
    jpFont = GRRLIB_LoadTTF(jp_font_ttf, jp_font_ttf_len);
    Ui::setFont(font);
    ringTex = GRRLIB_LoadTexture(data_ring_png);

    // Show a splash frame immediately so the user sees something during init.
    if (logoTex) {
        Ui::background(false);
        float ls = 0.60f;
        int lw = (int)(logoTex->w * ls);
        GRRLIB_DrawImg((640 - lw) / 2, (480 - (int)(logoTex->h * ls)) / 2,
                       logoTex, 0, ls, ls, 0xFFFFFFFF);
        GRRLIB_Render();
    } else {
        // Logo failed to load — show a plain coloured screen so we know init reached this point
        GRRLIB_FillScreen(0x1E3A5FFF);  // steel-blue diagnostic fallback
        GRRLIB_Render();
    }

    // Now init filesystem and input (may take a moment on first call)
    fatInitDefault();
    // fatMountSimple was removed: calling it after fatInitDefault() overwrites
    // the devoptab entry for "sd" with a stub that has open_r=NULL → errno=88.
    // fatInitDefault() alone correctly registers "sd" with a working FAT driver.

    WPAD_Init();
    WPAD_SetDataFormat(WPAD_CHAN_0, WPAD_FMT_BTNS_ACC_IR);
    WPAD_SetVRes(WPAD_CHAN_0, 640, 480);

    // Try argv0, all known prefixes, and NO-prefix (default libfat device)
    if (argv0 && argv0[0]) {
        std::string p(argv0);
        size_t slash = p.rfind('/');
        if (slash != std::string::npos)
            argvPath = p.substr(0, slash + 1) + "wiifin.cfg";
    }
    const char* probes[] = {
        argvPath.empty() ? nullptr : argvPath.c_str(),
        "/apps/WiiFin/wiifin.cfg",        // no prefix = default device
        "sd:/apps/WiiFin/wiifin.cfg",
        "fat:/apps/WiiFin/wiifin.cfg",
        "fat0:/apps/WiiFin/wiifin.cfg",
        "fat1:/apps/WiiFin/wiifin.cfg",
        "usb:/apps/WiiFin/wiifin.cfg",
    };
    // mkdirp: create each component of `dir` (e.g. "sd:/apps/WiiFin") only
    // if the device supports mkdir_r.  mkdir() only creates one level, so we
    // walk forward from the first '/' after the device prefix.
    auto mkdirp = [](const std::string& dir) {
        const char* colon = strchr(dir.c_str(), ':');
        if (!colon) return;
        std::string devname(dir.c_str(), colon - dir.c_str());
        bool devOk = false;
        for (int j = 0; j < STD_MAX; j++) {
            if (!devoptab_list[j] || !devoptab_list[j]->name) continue;
            if (devname == devoptab_list[j]->name && devoptab_list[j]->mkdir_r) {
                devOk = true; break;
            }
        }
        if (!devOk) return;
        // Walk each path component and mkdir incrementally
        std::string cur;
        const char* p = dir.c_str();
        while (*p) {
            const char* slash = strchr(p + 1, '/');
            if (slash) {
                cur.assign(dir.c_str(), slash);
                mkdir(cur.c_str(), 0777); /* ignore errors (EEXIST ok) */
                p = slash;
            } else {
                mkdir(dir.c_str(), 0777);
                break;
            }
        }
    };

    settingsPath = "";
    for (int i = 0; i < 7; i++) {
        if (!probes[i]) continue;
        // Create parent directory tree before probing
        {
            std::string p(probes[i]);
            size_t slash = p.rfind('/');
            if (slash != std::string::npos && slash > 0)
                mkdirp(p.substr(0, slash));
        }
        errno = 0;
        FILE* f = fopen(probes[i], "a");
        if (f) { fclose(f); settingsPath = probes[i]; break; }
    }

    if (!settingsPath.empty())
        Log::open(settingsPath.substr(0, settingsPath.rfind('/') + 1));
    SYS_Report("[WiiFin] v%s, IOS%d v%d, %s, MEM1 %u KB / MEM2 %u KB free, settings %s\n",
               WIIFIN_VERSION, (int)IOS_GetVersion(), (int)IOS_GetRevision(),
               WiiUtils::widescreen ? "16:9" : "4:3",
               (unsigned)(SYS_GetArena1Size() / 1024), (unsigned)(SYS_GetArena2Size() / 1024),
               settingsPath.empty() ? "(none)" : settingsPath.c_str());
    loadSettings();
    /* DHCP takes a few seconds: get it going while the menus show */
    jellyfinClient.startNetwork();
    MusicBGM::init(musicEnabled);
    SoundFX::init();
}

/* First launch: CRT TVs cut the edges of the picture (overscan) and their
 * owners rarely find Settings > Screen Area by themselves.  Offered once. */
void App::offerScreenCalibration(ir_t& ir) {
    int l, t, r, b;
    Ui::safeArea(l, t, r, b);
    if (screenAreaAsked || l || t || r || b) { screenAreaAsked = true; return; }
    screenAreaAsked = true;
    const Ui::Palette& p = Ui::pal();
    for (;;) {
        Input::update();
        Input::readIR(ir);
        if (g_app_powerOff || g_app_reset) return;
        if (Input::isBackPressed()) break;
        if (Input::isAJustPressed()) {
            SoundFX::play(SoundFX::FX::Start);
            SettingsView sv(btnTex, font, jellyfinClient, musicEnabled);
            sv.startCalibration();
            while (sv.isCalibrating() && !g_app_powerOff && !g_app_reset) {
                Input::update();
                Input::readIR(ir);
                sv.update(ir);
                sv.render(ir);
                GRRLIB_Render();
            }
            break;
        }
        Ui::background(false);
        Ui::card(70, 120, 500, 190, 18, 0.0f);
        Ui::textCentered(320, 140, "Does the whole picture fit your TV?", 22, p.text);
        Ui::textCentered(320, 182, "Older TVs (CRT) often cut the edges of the picture.", 15, p.textDim);
        Ui::textCentered(320, 204, "You can shrink it to fit now, or later in", 15, p.textDim);
        Ui::textCentered(320, 226, "Settings > Screen Area.", 15, p.textDim);
        Ui::button(170, 256, 140, 40, "Adjust", 18, Ui::pulse());
        Ui::button(330, 256, 140, 40, "Not now", 18, 0.0f);
        const Ui::Hint l2[] = { { "A", "Adjust" } };
        const Ui::Hint r2[] = { { "B", "Not now" } };
        Ui::bottomBar(l2, 1, r2, 1);
        GRRLIB_Render();
    }
    saveSettings();
}

void App::loop() {
    ir_t ir;
    int  selectedIndex = 0;
    bool irMode        = false;
    int  prevIrBtn     = -1;  /* last button index hovered via IR; -1 = none */
    const int MENU_COUNT = 3;
    float menuFocus[MENU_COUNT] = {};
    const std::string menuItems[] = {
        "Connect To Jellyfin",
        "Settings",
        "Exit"
    };

    /* Thin wrapper so existing call sites don't need to change. */
    auto showHomeOverlay = [&]() -> bool {
        return doShowHomeOverlay(font, btnTex, cursorPointerTex, musicEnabled);
    };

    /* ---- Helper: wait for the network (started at boot), offering a retry
     * when it failed.  Returns false when the user backs out. ---- */
    auto waitForNetwork = [&]() -> bool {
        const Ui::Palette& p = Ui::pal();
        for (;;) {
            jellyfinClient.startNetwork();
            /* B leaves: the attempt keeps going in the background (IOS can
             * take very long to answer) and the next try picks it up */
            u64 t0 = ticks_to_millisecs(gettime());
            while (jellyfinClient.networkBusy()) {
                Input::update();
                if (g_app_powerOff || g_app_reset) { running = false; return false; }
                if (Input::isBackPressed()) return false;
                bool slow = ticks_to_millisecs(gettime()) - t0 > 10000;
                Ui::background(false);
                Ui::spinner(ringTex, 320, 220);
                Ui::textCentered(320, 274, "Connecting to the network...", 18, p.textDim);
                if (slow)
                    Ui::textCentered(320, 300, "This is taking long: the Wii's network may need a restart.", 13, p.textDim);
                const Ui::Hint r[] = { { "B", "Back" } };
                Ui::bottomBar(nullptr, 0, r, 1);
                GRRLIB_Render();
            }
            if (jellyfinClient.takeNetworkResult()) return true;
            if (!showErrorScreen("No network connection",
                                 "Check the Wii's Internet settings, then try again.",
                                 jellyfinClient.lastError(), true, ir)) {
                if (g_app_powerOff || g_app_reset) running = false;
                return false;
            }
        }
    };

    /* ---- Helper: launch LibraryView for a saved profile ---- */
    auto runLibraryWithProfile = [&](const SavedProfile& p) {
        JellyfinAuth auth;
        auth.userId      = p.userId;
        auth.accessToken = p.accessToken;
        auth.serverName  = p.serverName;
        {
            /* server kind only: the log may be posted publicly */
            const std::string& u = p.serverUrl;
            size_t hs = u.find("://"); hs = hs == std::string::npos ? 0 : hs + 3;
            std::string host = u.substr(hs, u.find_first_of(":/", hs) - hs);
            bool ip = !host.empty() && host.find_first_not_of("0123456789.") == std::string::npos;
            Log::addPrivate(host);
            SYS_Report("[WiiFin] open profile: %s, %s host%s%s\n",
                       u.compare(0, 8, "https://") == 0 ? "https" : "http",
                       ip ? "IP" : "name", ip ? "" : " .", ip ? "" :
                       (host.rfind('.') == std::string::npos ? "(none)" : host.substr(host.rfind('.') + 1).c_str()));
        }
        if (!waitForNetwork()) return; /* DNS won't work without this */
        LibraryView lv(font, jpFont, cursorPointerTex, ringTex,
                       jellyfinClient, auth, p.serverUrl);
        lv.setUserName(p.username);
        for (;;) {
            while (true) {
                Input::update();
                Input::readIR(ir);
                if (g_app_powerOff || g_app_reset) { running = false; break; }
                if (Input::isHomePressed() && showHomeOverlay()) { running = false; break; }
                if (lv.update(ir)) break;
                lv.render(ir);
                GRRLIB_Render();
            }
            if (!running) return;
            if (lv.pendingPlayIsMusic) {
                lv.pendingPlayIsMusic = false;
                SoundFX::play(SoundFX::FX::Start);
                MusicPlayerView mpv(font, jellyfinClient, auth, p.serverUrl);
                mpv.setCursorTex(cursorPointerTex);
                mpv.setTracks(lv.pendingMusicTracks, lv.pendingMusicTrackIdx);
                bool wantsExit = mpv.run();
                if (g_app_powerOff || g_app_reset) { running = false; return; }
                SoundFX::play(SoundFX::FX::Back);
                if (wantsExit) { running = false; return; }
                lv.onPlaybackFinished("");
            } else if (!lv.pendingPlayUrl.empty()) {
                std::string lastItemId;
                bool wantsExit = runPlaySession(jellyfinClient, auth, p.serverUrl, lv,
                                                font, btnTex, cursorPointerTex, ringTex,
                                                lastItemId);
                if (wantsExit || g_app_powerOff || g_app_reset) { running = false; return; }
                saveSettings();   /* the zoom may have changed during playback */
                lv.onPlaybackFinished(lastItemId);
            } else {
                break; // user navigated back (B from libraries grid) — no play requested
            }
        }
    };

    /* ---- Helper: open ConnectView, on success add/update profile + library ---- */
    auto runConnect = [&]() {
        ConnectView cv(btnTex, cursorPointerTex, font, jellyfinClient);
        ConnectResult res = ConnectResult::None;
        while (res == ConnectResult::None && running) {
            Input::update();
            Input::readIR(ir);
            if (g_app_powerOff || g_app_reset) { running = false; break; }
            if (Input::isHomePressed() && showHomeOverlay()) { running = false; break; }
            res = cv.update(ir);
            cv.render(ir);
            GRRLIB_Render();
        }
        if (res == ConnectResult::Success) {
            SavedProfile p;
            p.serverUrl   = cv.serverUrl;
            p.username    = cv.username;
            p.serverName  = cv.auth.serverName;
            p.userId      = cv.auth.userId;
            p.accessToken = cv.auth.accessToken;
            /* Update existing profile if same userId (token refresh/re-auth), otherwise append.
             * Only deduplicate when both sides have a known userId — if userId is empty
             * (should never happen) always append to avoid silently overwriting profiles. */
            bool found = false;
            if (!p.userId.empty()) {
                for (auto& existing : profiles) {
                    if (!existing.userId.empty() && existing.userId == p.userId) {
                        existing = p; found = true; break;
                    }
                }
            }
            if (!found) profiles.push_back(p);
            saveSettings();
            runLibraryWithProfile(p);
        }
    };

    /* ---- Helper: profile picker loop ---- */
    auto runProfilePicker = [&]() {
        while (running) {
            if (profiles.empty()) {
                runConnect();
                return;
            }
            ProfileView pv(font, cursorPointerTex, profiles);
            ProfileResult res = ProfileResult::None;
            while (res == ProfileResult::None && running) {
                Input::update();
                Input::readIR(ir);
                if (g_app_powerOff || g_app_reset) { running = false; return; }
                if (Input::isHomePressed() && showHomeOverlay()) { running = false; return; }
                res = pv.update(ir);
                pv.render(ir);
                GRRLIB_Render();
            }
            if (!running || res == ProfileResult::Back) return;
            if (res == ProfileResult::DeleteOne) {
                int idx = pv.selectedIdx;
                if (idx >= 0 && idx < (int)profiles.size()) {
                    profiles.erase(profiles.begin() + idx);
                    saveSettings();
                }
                continue;
            }
            if (res == ProfileResult::AddNew) { runConnect(); continue; }
            if (res == ProfileResult::Selected) {
                runLibraryWithProfile(profiles[pv.selectedIdx]);
                continue; /* re-show picker on return */
            }
        }
    };

    SYS_Report("[WiiFin] ready\n");   /* tools/test/smoke.sh waits for this */
    offerScreenCalibration(ir);

    while (running) {
        Input::update();   // calls WPAD_ScanPads() internally
        Input::readIR(ir);

        // --- Input: D-pad navigation ---
        if (g_app_reset) running = false;
        else if (Input::isHomePressed() && showHomeOverlay()) running = false;
        if (g_app_powerOff) running = false;
        if (ir.valid) irMode = true;
        if (Input::isUpPressed())   { selectedIndex = (selectedIndex - 1 + MENU_COUNT) % MENU_COUNT; irMode = false; }
        if (Input::isDownPressed()) { selectedIndex = (selectedIndex + 1) % MENU_COUNT; irMode = false; }

        // --- IR hover updates selection (no action yet) ---
        bool irHovered = false;
        if (ir.valid) {
            for (int i = 0; i < MENU_COUNT; ++i) {
                int by = BY_START + i * B_SPACING;
                if (ir.x >= BX && ir.x <= BX + BW &&
                    ir.y >= by && ir.y <= by + BH) {
                    selectedIndex = i;
                    irHovered = true;
                    irMode = true;
                    break;
                }
            }
        }

        // --- Select sound: fire once when IR cursor first enters a button ---
        {
            int curIrBtn = irHovered ? selectedIndex : -1;
            if (curIrBtn >= 0 && curIrBtn != prevIrBtn)
                SoundFX::play(SoundFX::FX::Select);
            prevIrBtn = curIrBtn;
        }

        // --- Single action dispatch on A press ---
        if (Input::isAJustPressed() && (irHovered || (!ir.valid && !irMode))) {
            SoundFX::play(SoundFX::FX::Start);
            switch (selectedIndex) {
                case 0: {
                    runProfilePicker();
                    break;
                }
                case 1: {
                    // Launch Settings view
                    SettingsView sv(btnTex, font, jellyfinClient, musicEnabled);
                    while (true) {
                        Input::update();
                        Input::readIR(ir);
                        if (g_app_powerOff || g_app_reset) { running = false; break; }
                        if (Input::isHomePressed() && showHomeOverlay()) { running = false; break; }
                        if (sv.update(ir)) break;
                        sv.render(ir);
                        if (ir.valid && cursorPointerTex) {
                            orient_t orient; WPAD_Orientation(WPAD_CHAN_0, &orient);
                            GRRLIB_DrawImg((int)ir.x - 20, (int)ir.y - 4, cursorPointerTex, orient.roll, 1, 1, 0xFFFFFFFF);
                        }
                        GRRLIB_Render();
                    }
                    saveSettings();
                    MusicBGM::setEnabled(musicEnabled);
                    break;
                }
                case 2: running = false; break;
            }
        }

        // ===================== RENDER =====================
        Ui::background(false);

        if (logoTex) {
            float ls = 0.62f;
            int lw = (int)(logoTex->w * ls);
            GRRLIB_DrawImg((640 - lw) / 2, 40, logoTex, 0, ls, ls, 0xFFFFFFFF);
        }

        for (int i = 0; i < MENU_COUNT; ++i) {
            int by = BY_START + i * B_SPACING;
            bool hover = ir.valid && ir.x >= BX && ir.x <= BX + BW && ir.y >= by && ir.y <= by + BH;
            bool focus = ir.valid ? hover : (i == selectedIndex);
            menuFocus[i] = Ui::approach(menuFocus[i], focus ? 1.0f : 0.0f);
            float grow = 8.0f * menuFocus[i];
            Ui::button(BX - grow, by - grow * 0.25f, BW + grow * 2, BH + grow * 0.5f,
                       menuItems[i].c_str(), 22, menuFocus[i]);
        }

        {
            static const Ui::Hint left[]  = { { "A", "Select" } };
            static const Ui::Hint right[] = { { "HOME", "Menu" } };
            Ui::bottomBar(left, 1, right, 1);
        }

        // --- IR Cursor: rendered last ---
        if (ir.valid && cursorPointerTex) {
            orient_t orient;
            WPAD_Orientation(WPAD_CHAN_0, &orient);
            GRRLIB_DrawImg(
                (int)ir.x - 20,
                (int)ir.y - 4,
                cursorPointerTex, orient.roll, 1, 1, 0xFFFFFFFF);
        }

        GRRLIB_Render();
    }

    // --- Cleanup ---
    saveSettings();
    Log::close();        // its writer thread must not touch the card past here
    MusicBGM::pause();   // stop audio thread/ASND callbacks before tearing down GX
    // Blank the VI output before GX teardown to avoid purple/pink artefact frame
    VIDEO_SetBlack(true);
    VIDEO_Flush();
    VIDEO_WaitVSync();
    GRRLIB_FreeTexture(logoTex);
    GRRLIB_FreeTexture(btnTex);
    GRRLIB_FreeTexture(cursorPointerTex);
    GRRLIB_FreeTexture(ringTex);
    Text::clearCache();
    GRRLIB_FreeTTF(font);
    GRRLIB_FreeTTF(jpFont);
    GRRLIB_Exit();
    ConnectView::shutdownUsbKeyboard();
    WPAD_Shutdown();
    if (g_app_powerOff)        SYS_ResetSystem(SYS_POWEROFF,    0, 0);
    else if (g_restartApp) exit(0);  // HBC catches exit(0) and reloads the app
    else {
        /* Restore the NAND-loader stub bytes at their installed VA (0x80804000)
         * before returning to the System Menu.  The stub was zeroed at startup
         * by the DOL BSS initialiser (BSS spans 0x806f7e0c–0x80ae64dc, which
         * includes the stub zone) and may have been overwritten again by
         * MPlayer's stream-cache buffer during playback.  Without this restore
         * SYS_RETURNTOMENU results in a DSI exception in the return trampoline. */
        {
            extern char __stub_zone_start[], __stub_zone_end[];
            size_t sz = (size_t)(__stub_zone_end - __stub_zone_start);
            if (sz > 0) {
                memcpy((void*)0x80804000u, __stub_zone_start, sz);
                DCFlushRange((void*)0x80804000u, sz);
            }
        }
        SYS_ResetSystem(SYS_RETURNTOMENU, 0, 0);
    }
}

void App::run() {
    loop();
}


