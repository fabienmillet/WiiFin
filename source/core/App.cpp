#include "App.h"
#include "AppInternal.h"
#include "ExitZone.h"
#include "Text.h"
#include "Lang.h"
#include "../ui/Ui.h"
#include "Utils.h"
#include "Input.h"
#include "MusicBGM.h"
#include "BackgroundMusic.h"
#include "SoundFX.h"
#include "../ui/ConnectView.h"
#include "../ui/MusicPlayerView.h"
#include "../ui/ProfileView.h"
#include "../ui/SettingsView.h"
#include "../ui/LibraryView.h"
#include "../jellyfin/JellyfinClient.h"
#include "../jellyfin/RemoteControl.h"

#include <grrlib.h>
#include <wiiuse/wpad.h>
#include <fat.h>
#include <sdcard/wiisd_io.h>
#include <errno.h>
#include <ft2build.h>
#include FT_FREETYPE_H
#include <algorithm>
#include <dirent.h>
#include <stdio.h>
#include <gccore.h>
#include <ogc/wiilaunch.h>
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

/* The power button turns the Wii off even when WiiFin is stuck: a request
 * that never returns (the loading spinner waits for it), a thread that
 * won't stop on the way out.  The way out normally takes a moment (the
 * position sent to the server...); 5 s after the button, this does it. */
static u8 s_powerWatchStack[8192] ATTRIBUTE_ALIGN(32);
static void* powerWatch(void*)
{
    while (!g_app_powerOff) usleep(100 * 1000);
    SYS_Report("[WiiFin] power button\n");
    for (int i = 0; i < 50; i++) usleep(100 * 1000);
    SYS_Report("[WiiFin] still on 5 s after the power button: off now\n");
    SYS_ResetSystem(SYS_POWEROFF, 0, 0);
    return nullptr;
}

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
            const Ui::Hint l[] = { { "A", N_("Retry") } };
            const Ui::Hint r[] = { { "B", N_("Back") } };
            Ui::bottomBar(l, 1, r, 1);
        } else {
            const Ui::Hint r[] = { { "A", N_("OK") } };
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
    {   /* above the main thread: it runs even if that one spins */
        lwp_t t;
        LWP_CreateThread(&t, powerWatch, nullptr, s_powerWatchStack, sizeof(s_powerWatchStack), 100);
    }

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
    Text::setFallback(jpFont);   /* Japanese titles, subtitles... in any text */
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

    if (!settingsPath.empty()) {
        Log::open(settingsPath.substr(0, settingsPath.rfind('/') + 1));
        loadSdFonts(settingsPath.substr(0, settingsPath.rfind('/') + 1));
    }
    SYS_Report("[WiiFin] v%s, IOS%d v%d, %s, MEM1 %u KB / MEM2 %u KB free, settings %s\n",
               WIIFIN_VERSION, (int)IOS_GetVersion(), (int)IOS_GetRevision(),
               WiiUtils::widescreen ? "16:9" : "4:3",
               (unsigned)(SYS_GetArena1Size() / 1024), (unsigned)(SYS_GetArena2Size() / 1024),
               settingsPath.empty() ? "(none)" : settingsPath.c_str());
    /* One DeviceId per console (a hash of its id: the log is public);
     * wiifin.cfg's device_id, read by loadSettings, overrides it */
    {
        u32 id = 0;
        if (ES_GetDeviceID(&id) >= 0 && id) {
            u32 h = 2166136261u;
            for (int i = 0; i < 4; ++i) h = (h ^ ((id >> (i * 8)) & 0xFF)) * 16777619u;
            char buf[24];
            snprintf(buf, sizeof(buf), "wiifin-%08x", (unsigned)h);
            JellyfinClient::setDeviceId(buf);
        }
    }
    loadSettings();
    /* DHCP takes a few seconds: get it going while the menus show */
    jellyfinClient.startNetwork();
    /* the SD card's sounds/ folder, next to wiifin.cfg, may replace the
     * built-in sounds and music */
    {
        std::string dir = settingsPath.empty() ? "" : settingsPath.substr(0, settingsPath.rfind('/') + 1);
        MusicBGM::loadCustom(dir.c_str());
        MusicBGM::init(musicEnabled);
        SoundFX::init(dir.c_str());
    }
}

/* Fonts in <dir>fonts/ (.ttf, .otf, .ttc), by name: fallbacks for what the
 * built-in fonts lack, Chinese and Korean for one (the Homebrew Channel
 * package brings a Noto Sans CJK subset).  Read whole into memory, MEM2 by
 * then (FreeType keeps reading them). */
void App::loadSdFonts(const std::string& dir)
{
    struct Langs { bool hangul = false, han = false; ~Langs() { Lang::setFonts(hangul, han); } } langs;
    std::string fdir = dir + "fonts";
    DIR* d = opendir(fdir.c_str());
    if (!d) return;
    std::vector<std::string> names;
    while (struct dirent* e = readdir(d)) {
        std::string n = e->d_name;
        size_t dot = n.rfind('.');
        std::string ext = dot == std::string::npos ? "" : n.substr(dot);
        for (char& c : ext) c = (char)tolower((unsigned char)c);
        if (ext == ".ttf" || ext == ".otf" || ext == ".ttc") names.push_back(n);
    }
    closedir(d);
    std::sort(names.begin(), names.end());
    for (const std::string& n : names) {
        std::string path = fdir + "/" + n;
        FILE* f = fopen(path.c_str(), "rb");
        if (!f) continue;
        fseek(f, 0, SEEK_END);
        long size = ftell(f);
        fseek(f, 0, SEEK_SET);
        void* buf = size > 0 && size <= (24L << 20) ? malloc((size_t)size) : nullptr;
        bool ok = buf && fread(buf, 1, (size_t)size, f) == (size_t)size;
        fclose(f);
        GRRLIB_ttfFont* font = ok ? GRRLIB_LoadTTF((const u8*)buf, (s32)size) : nullptr;
        if (!font) {
            free(buf);
            SYS_Report("[Text] fonts/%s: cannot be read (%ld bytes)\n", n.c_str(), size);
            continue;
        }
        sdFonts.push_back(font);
        sdFontData.push_back(buf);
        /* Korean and Chinese: the languages it lets WiiFin speak */
        if (FT_Get_Char_Index((FT_Face)font->face, 0xD55C)) langs.hangul = true;   /* 한 */
        if (FT_Get_Char_Index((FT_Face)font->face, 0x4E2D)) langs.han    = true;   /* 中 */
        Text::addFallback(font);
        SYS_Report("[Text] fonts/%s: %ld KB at %p, %ld glyphs\n", n.c_str(), size / 1024, buf,
                   (long)((FT_Face)font->face)->num_glyphs);
    }
}

/* First launch: CRT TVs cut the edges of the picture (overscan) and their
 * owners rarely find Settings > Screen Area by themselves.  Offered once. */
void App::offerScreenCalibration(ir_t& ir) {
    int l, t, r, b;
    Ui::safeArea(l, t, r, b);
    if (screenAreaAsked || l || t || r || b) { screenAreaAsked = true; return; }
    screenAreaAsked = true;
    const Ui::Palette& p = Ui::pal();
    auto drawPointer = [&]() {
        if (ir.valid && cursorPointerTex) {
            orient_t orient; WPAD_Orientation(WPAD_CHAN_0, &orient);
            GRRLIB_DrawImg((int)ir.x - 20, (int)ir.y - 4, cursorPointerTex, orient.roll, 1, 1, 0xFFFFFFFF);
        }
    };
    const int BY = 256, BW2 = 140, BH2 = 40;
    const int bx[2] = { 170, 330 };
    int sel = 0;   /* 0 = Adjust, 1 = Not now */
    for (;;) {
        Input::update();
        Input::readIR(ir);
        if (g_app_powerOff || g_app_reset) return;
        int was = sel;
        if (Input::isLeftPressed())  sel = 0;
        if (Input::isRightPressed()) sel = 1;
        if (ir.valid)
            for (int i = 0; i < 2; ++i)
                if (ir.x >= bx[i] && ir.x <= bx[i] + BW2 && ir.y >= BY && ir.y <= BY + BH2) sel = i;
        if (sel != was) SoundFX::play(SoundFX::FX::Select);
        if (Input::isBackPressed() || (Input::isAJustPressed() && sel == 1)) {
            SoundFX::play(SoundFX::FX::Back);
            break;
        }
        if (Input::isAJustPressed()) {
            SoundFX::play(SoundFX::FX::Start);
            SettingsView sv(btnTex, font, jellyfinClient, musicEnabled);
            sv.startCalibration();
            while (sv.isCalibrating() && !g_app_powerOff && !g_app_reset) {
                Input::update();
                Input::readIR(ir);
                sv.update(ir);
                sv.render(ir);
                drawPointer();
                GRRLIB_Render();
            }
            break;
        }
        Ui::background(false);
        Ui::card(70, 120, 500, 190, 18, 0.0f);
        Ui::textCentered(320, 140, tr("Does the whole picture fit your TV?"), 22, p.text);
        Ui::textCentered(320, 182, tr("Older TVs (CRT) often cut the edges of the picture."), 15, p.textDim);
        /* TRANSLATORS: one sentence on two lines, the second one naming the menu */
        Ui::textCentered(320, 204, tr("You can shrink it to fit now, or later in"), 15, p.textDim);
        Ui::textCentered(320, 226, tr("Settings > Screen Area."), 15, p.textDim);
        Ui::button(bx[0], BY, BW2, BH2, N_("Adjust"), 18, sel == 0 ? Ui::pulse() : 0.0f);
        Ui::button(bx[1], BY, BW2, BH2, N_("Not now"), 18, sel == 1 ? Ui::pulse() : 0.0f);
        const Ui::Hint l2[] = { { "A", N_("Select") } };
        const Ui::Hint r2[] = { { "B", N_("Not now") } };
        Ui::bottomBar(l2, 1, r2, 1);
        drawPointer();
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
        N_("Connect To Jellyfin"),
        N_("Settings"),
        N_("Exit")
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
                Ui::textCentered(320, 274, tr("Connecting to the network..."), 18, p.textDim);
                if (slow)
                    Ui::textCentered(320, 300, tr("This is taking long: the Wii's network may need a restart."), 13, p.textDim);
                const Ui::Hint r[] = { { "B", N_("Back") } };
                Ui::bottomBar(nullptr, 0, r, 1);
                GRRLIB_Render();
            }
            if (jellyfinClient.takeNetworkResult()) return true;
            if (!showErrorScreen(tr("No network connection"),
                                 tr("Check the Wii's Internet settings, then try again."),
                                 jellyfinClient.lastError(), true, ir)) {
                if (g_app_powerOff || g_app_reset) running = false;
                return false;
            }
        }
    };

    /* the profile menu's WiiFin Menu: past the profiles, to this menu */
    bool toMainMenu = false;

    /* ---- Helper: launch LibraryView for a saved profile ---- */
    auto runLibraryWithProfile = [&](SavedProfile& p) {
        /* its own look while it is open; WiiFin's again after (and a
         * profile signing in with its password keeps no token) */
        applyView(viewOf(p));
        struct Leave {
            App& app; SavedProfile& p;
            ~Leave() { app.applyView(app.startView); if (p.signIn == SavedProfile::Password) p.accessToken.clear(); }
        } leave{ *this, p };
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
        /* the server's remote control, while this profile is open */
        jellyfinClient.postCapabilities(p.serverUrl, auth);
        Remote::start(p.serverUrl, auth.accessToken, jellyfinClient.sslVerify);
        struct RemoteOff { ~RemoteOff() { Remote::stop(); } } remoteOff;
        for (;;) {
            while (true) {
                Input::update();
                Input::readIR(ir);
                if (g_app_powerOff || g_app_reset) { running = false; break; }
                if (Input::isHomePressed() && showHomeOverlay()) { running = false; break; }
                if (lv.update(ir)) break;
                if (lv.render(ir)) GRRLIB_Render();
                else               VIDEO_WaitVSync();   /* the previous picture stays */
            }
            if (!running) return;
            if (lv.pendingPlayIsMusic) {
                lv.pendingPlayIsMusic = false;
                SoundFX::play(SoundFX::FX::Play);
                MusicPlayerView mpv(font, jellyfinClient, auth, p.serverUrl);
                mpv.setCursorTex(cursorPointerTex);
                mpv.setTracks(lv.pendingMusicTracks, lv.pendingMusicTrackIdx);
                mpv.setShuffleStart(lv.pendingMusicShuffle);
                lv.pendingMusicShuffle = false;
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
            } else if (lv.exitTo == LibraryView::Exit::NowPlaying) {
                /* the music that went on while browsing, in its player again */
                lv.exitTo = LibraryView::Exit::Profiles;
                std::vector<MusicTrack> tracks;
                std::vector<int> order;
                int pos = 0, repeat = 0;
                float secs = 0.0f;
                BackgroundMusic::state(tracks, order, pos, secs, repeat);
                if (tracks.empty()) continue;
                SYS_Report("[WiiFin] now playing: song %d of %d at %.0f s\n", pos + 1, (int)order.size(),
                           (double)secs);
                MusicPlayerView mpv(font, jellyfinClient, auth, p.serverUrl);
                mpv.setCursorTex(cursorPointerTex);
                mpv.setTracks(tracks, 0);
                mpv.setQueue(order, pos, secs, repeat);
                bool wantsExit = mpv.run();
                if (g_app_powerOff || g_app_reset) { running = false; return; }
                SoundFX::play(SoundFX::FX::Back);
                if (wantsExit) { running = false; return; }
                lv.onPlaybackFinished("");
            } else if (lv.exitTo == LibraryView::Exit::Settings) {
                /* Settings from the profile menu: signed in still */
                lv.exitTo = LibraryView::Exit::Profiles;
                runSettings(&p);
                if (!running) return;
                lv.onSettingsClosed();
            } else {
                /* the profile closes: its music with it, the menu music back */
                BackgroundMusic::stop(true);
                if (lv.exitTo == LibraryView::Exit::WiiFinMenu) {
                    toMainMenu = true;
                    SYS_Report("[WiiFin] back to the menu\n");
                } else {
                    SYS_Report("[WiiFin] back to the profiles\n");
                }
                break; // user navigated back (B from libraries grid) — no play requested
            }
        }
    };

    /* ---- Helper: open ConnectView, on success add/update profile + library ---- */
    auto runConnect = [&](const SavedProfile* again = nullptr) {
        ConnectView cv(btnTex, cursorPointerTex, font, jellyfinClient);
        if (again) cv.setFields(again->serverUrl, again->username);   /* its password, each time */
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
            int found = -1;
            if (!p.userId.empty()) {
                for (int i = 0; i < (int)profiles.size(); ++i) {
                    SavedProfile& existing = profiles[i];
                    if (!existing.userId.empty() && existing.userId == p.userId) {
                        /* the same account signed in again: its own settings stay */
                        existing.serverUrl = p.serverUrl; existing.username = p.username;
                        existing.serverName = p.serverName; existing.accessToken = p.accessToken;
                        found = i; break;
                    }
                }
            }
            if (found < 0) {
                profiles.push_back(p);
                found = (int)profiles.size() - 1;
                runOnboarding(profiles[found]);   /* sign-in, home screen, library view */
                if (g_app_powerOff || g_app_reset) { running = false; return; }
            }
            saveSettings();
            runLibraryWithProfile(profiles[found]);
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
            if (res == ProfileResult::AddNew) {
                runConnect();
                if (toMainMenu) { toMainMenu = false; return; }
                continue;
            }
            if (res == ProfileResult::Selected) {
                SavedProfile& sp = profiles[pv.selectedIdx];
                if (testOnboarding) {   /* tests: asked as a new profile */
                    testOnboarding = false;
                    runOnboarding(sp);
                    saveSettings();
                }
                if (sp.signIn == SavedProfile::Passcode && !askPasscode(sp)) continue;
                if (sp.signIn == SavedProfile::Password) {
                    runConnect(&sp);   /* signs it in, then opens it */
                } else {
                    runLibraryWithProfile(sp);
                }
                if (toMainMenu) { toMainMenu = false; return; }
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
        irMode = ir.valid;   /* no pointer on the screen: the D-pad drives, A acts on the highlighted item */
        if (Input::isUpPressed())   { selectedIndex = (selectedIndex - 1 + MENU_COUNT) % MENU_COUNT; irMode = false; }
        if (Input::isDownPressed()) { selectedIndex = (selectedIndex + 1) % MENU_COUNT; irMode = false; }
        if (Input::isUpPressed() || Input::isDownPressed()) SoundFX::play(SoundFX::FX::Move);

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
                case 1:
                    runSettings(nullptr);   /* WiiFin's own look, and the system */
                    break;
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
            static const Ui::Hint left[]  = { { "A", N_("Select") } };
            static const Ui::Hint right[] = { { "HOME", N_("Menu") } };
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
    BackgroundMusic::stop(false);
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
    Text::setFallback(nullptr);
    GRRLIB_FreeTTF(font);
    GRRLIB_FreeTTF(jpFont);
    for (GRRLIB_ttfFont* f : sdFonts) GRRLIB_FreeTTF(f);
    for (void* d : sdFontData) free(d);
    GRRLIB_Exit();
    ConnectView::shutdownUsbKeyboard();
    WPAD_Shutdown();
    if (g_app_powerOff)        SYS_ResetSystem(SYS_POWEROFF,    0, 0);
    else if (g_restartApp) {
        /* the HOME menu's Reset: the channel (WAD) launched again by the
         * system, which exit() didn't do (it went to the Wii Menu); from
         * the Homebrew Channel, back to it */
        u64 tid = 0;
        if (ES_GetTitleID(&tid) >= 0 && tid == 0x000100015749464EULL) {   /* 00010001-WIFN */
            SYS_Report("[WiiFin] reset: the channel again\n");
            WII_Initialize();
            WII_LaunchTitle(tid);
        }
        exit(0);
    }
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


