/* App settings and saved profiles: sd:/apps/WiiFin/wiifin.cfg */
#include "App.h"
#include "../ui/Ui.h"
#include "Utils.h"
#include "Lang.h"
#include "SoundFX.h"
#include "MusicSession.h"
#include "../player/VideoSurface.h"
#include "../player/vo_wiifin.h"
#include "../player/WiiPlayer.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* wiifin.cfg is never left half-written (#46): saveSettings writes
 * wiifin.cfg.tmp, keeps the previous file as wiifin.cfg.bak, then renames
 * the new one into place.  loadSettings reads the first of the three that
 * is whole, so a power cut or a crash during a save loses nothing. */
namespace {

/* A file saveSettings finished: it starts with wiifin_cfg=2 and ends with
 * end=1.  One from before this format (no wiifin_cfg line) counts when it
 * holds a setting at all. */
bool cfgWhole(const std::string& path) {
    FILE* f = fopen(path.c_str(), "r");
    if (!f) return false;
    char line[512];
    bool first = true, framed = false, any = false, ended = false;
    while (fgets(line, sizeof(line), f)) {
        if (first) framed = strncmp(line, "wiifin_cfg=", 11) == 0;
        first = false;
        if (strchr(line, '=')) any = true;
        ended = strncmp(line, "end=1", 5) == 0;
    }
    fclose(f);
    return framed ? ended : any;
}

} // namespace

void App::loadSettings() {
    /* the clock as the console's region writes it, unless wiifin.cfg says */
    Ui::setClock12h(CONF_GetRegion() == CONF_REGION_US);
    Lang::choose(-1);   /* the Wii's language, unless wiifin.cfg says */
    startView = currentView();
    if (settingsPath.empty()) return;
    /* the saved file, else the one before it, else a save that stopped
     * before its rename */
    std::string from;
    for (const std::string& p : { settingsPath, settingsPath + ".bak", settingsPath + ".tmp" })
        if (cfgWhole(p)) { from = p; break; }
    if (from.empty()) {
        SYS_Report("[Settings] none saved yet in %s\n", settingsPath.c_str());
        return;
    }
    if (from != settingsPath)
        SYS_Report("[Settings] %s is empty or cut short: read %s instead\n",
                   settingsPath.c_str(), from.c_str());
    FILE* f = fopen(from.c_str(), "r");
    if (!f) return;

    profiles.clear();
    int profileCount = 0;
    SavedProfile legacyProfile;
    bool hasLegacy = false;

    char line[512];
    while (fgets(line, sizeof(line), f)) {
        char* nl = strchr(line, '\n'); if (nl) *nl = '\0';
        char* eq = strchr(line, '=');
        if (!eq) continue;
        *eq = '\0';
        const char* key = line;
        const char* val = eq + 1;

        if (strcmp(key, "ssl_verify") == 0) {
            jellyfinClient.sslVerify = atoi(val) != 0;
        } else if (strcmp(key, "music_enabled") == 0) {
            musicEnabled = atoi(val) != 0;
        } else if (strcmp(key, "music_autoplay") == 0) {
            MusicSession::setAutoplay(atoi(val) != 0);
        } else if (strcmp(key, "ui_sounds") == 0) {
            SoundFX::setEnabled(atoi(val) != 0);
        } else if (strcmp(key, "test_freeze_at") == 0) {
            g_wiifin_test_freeze = (float)atof(val);   /* tests */
        } else if (strcmp(key, "test_onboarding") == 0) {
            testOnboarding = atoi(val) != 0;   /* tests */
        } else if (strcmp(key, "log_sounds") == 0) {
            SoundFX::setLogging(atoi(val) != 0);   /* tests */
        } else if (strcmp(key, "ui_theme") == 0) {
            {
                int t = atoi(val);
                Ui::setTheme(t >= 0 && t < Ui::THEME_COUNT ? (Ui::Theme)t : Ui::Theme::Dark);
                /* settings saved before home_layout existed: Flix meant rows
                 * (home_layout, written after ui_theme, overrides this) */
                if (Ui::chosenTheme() == Ui::Theme::Flix) Ui::setHomeLayout(Ui::HomeLayout::Rows);
            }
        } else if (strcmp(key, "christmas") == 0) {
            Ui::setFestive(atoi(val) != 0);
        } else if (strcmp(key, "test_christmas") == 0) {
            Ui::forceFestive(atoi(val) != 0);   /* tests */
        } else if (strcmp(key, "smooth_motion") == 0) {
            g_wiifin_smooth_motion = atoi(val) != 0;
        } else if (strcmp(key, "disc_light") == 0) {
            g_wiifin_disc_light = atoi(val) != 0;
        } else if (strcmp(key, "video_zoom") == 0) {
            VideoSurface::setZoom(atoi(val) == 1 ? VideoSurface::Zoom::Fill : VideoSurface::Zoom::Fit);
        } else if (strcmp(key, "library_view") == 0) {
            int v = atoi(val);
            if (v >= 0 && v < Ui::LIBRARY_STYLE_COUNT) Ui::setLibraryStyle((Ui::LibraryStyle)v);
        } else if (strcmp(key, "home_layout") == 0) {
            Ui::setHomeLayout(atoi(val) == 1 ? Ui::HomeLayout::Rows : Ui::HomeLayout::Grid);
        } else if (strcmp(key, "language") == 0) {
            Lang::chooseKey(val);
        } else if (strcmp(key, "clock_12h") == 0) {
            Ui::setClock12h(atoi(val) != 0);
        } else if (strcmp(key, "solid_colors") == 0) {
            Ui::setSolidColors(atoi(val) != 0);
        } else if (strcmp(key, "direct_play") == 0) {
            jellyfinClient.directPlay = atoi(val) != 0;
        } else if (strcmp(key, "device_id") == 0) {
            JellyfinClient::setDeviceId(val);   /* tests: several Dolphins, one id each */
        } else if (strcmp(key, "screen_area_asked") == 0) {
            screenAreaAsked = atoi(val) != 0;
        } else if (strcmp(key, "safe_area") == 0) {
            int l = 0, t = 0, r = 0, b = 0;
            if (sscanf(val, "%d,%d,%d,%d", &l, &t, &r, &b) == 4) Ui::setSafeArea(l, t, r, b);
        } else if (strcmp(key, "video_quality") == 0) {
            int q = atoi(val);
            if (q >= 0 && q < JellyfinClient::VIDEO_QUALITY_COUNT) jellyfinClient.videoQuality = q;
        } else if (strcmp(key, "profile_count") == 0) {
            profileCount = atoi(val);
            if (profileCount > 0 && profileCount <= 32) profiles.resize((size_t)profileCount);
        } else if (strncmp(key, "profile.", 8) == 0) {
            /* profile.N.field=value */
            int idx = atoi(key + 8);
            if (idx < 0 || idx >= (int)profiles.size()) continue;
            const char* dot = strchr(key + 8, '.');
            if (!dot) continue;
            const char* field = dot + 1;
            if (strcmp(field, "server_url")   == 0) {
                profiles[idx].serverUrl = val;
                while (profiles[idx].serverUrl.size() > 1 && profiles[idx].serverUrl.back() == '/')
                    profiles[idx].serverUrl.pop_back();
            }
            if (strcmp(field, "username")      == 0) profiles[idx].username    = val;
            if (strcmp(field, "server_name")   == 0) profiles[idx].serverName  = val;
            if (strcmp(field, "user_id")       == 0) profiles[idx].userId      = val;
            if (strcmp(field, "access_token")  == 0) profiles[idx].accessToken = val;
            if (strcmp(field, "theme")         == 0) profiles[idx].theme        = atoi(val);
            if (strcmp(field, "home_layout")   == 0) profiles[idx].homeLayout   = atoi(val);
            if (strcmp(field, "library_view")  == 0) profiles[idx].libraryStyle = atoi(val);
            if (strcmp(field, "solid_colors")  == 0) profiles[idx].solidColors  = atoi(val);
            if (strcmp(field, "sign_in")       == 0) profiles[idx].signIn       = atoi(val);
            if (strcmp(field, "pass_salt")     == 0) profiles[idx].passSalt     = val;
            if (strcmp(field, "pass_hash")     == 0) profiles[idx].passHash     = val;
        } else {
            /* Legacy single-profile keys — migrate on first load */
            if (strcmp(key, "server_url")   == 0) {
                legacyProfile.serverUrl = val;
                while (legacyProfile.serverUrl.size() > 1 && legacyProfile.serverUrl.back() == '/')
                    legacyProfile.serverUrl.pop_back();
                hasLegacy = true;
            }
            if (strcmp(key, "username")      == 0) { legacyProfile.username    = val; }
            if (strcmp(key, "user_id")       == 0) { legacyProfile.userId      = val; }
            if (strcmp(key, "access_token")  == 0) { legacyProfile.accessToken = val; }
            if (strcmp(key, "server_name")   == 0) { legacyProfile.serverName  = val; }
        }
    }
    fclose(f);

    /* Migrate legacy single-profile format (no profile_count key present) */
    if (hasLegacy && profiles.empty() && !legacyProfile.accessToken.empty())
        profiles.push_back(legacyProfile);
    startView = currentView();   /* the menus' look: ui_theme, solid_colors... */
    SYS_Report("[Lang] %s (chosen: %s)\n", Lang::code(Lang::current()), Lang::choiceKey());
}

ViewPrefs App::currentView() const {
    ViewPrefs v;
    v.theme        = (int)Ui::chosenTheme();
    v.homeLayout   = (int)Ui::homeLayout();
    v.libraryStyle = (int)Ui::libraryStyle();
    v.solidColors  = Ui::solidColors() ? 1 : 0;
    return v;
}

void App::applyView(const ViewPrefs& v) {
    Ui::setTheme(v.theme >= 0 && v.theme < Ui::THEME_COUNT ? (Ui::Theme)v.theme : Ui::Theme::Dark);
    Ui::setHomeLayout(v.homeLayout == 1 ? Ui::HomeLayout::Rows : Ui::HomeLayout::Grid);
    Ui::setLibraryStyle(v.libraryStyle >= 0 && v.libraryStyle < Ui::LIBRARY_STYLE_COUNT
                        ? (Ui::LibraryStyle)v.libraryStyle : Ui::LibraryStyle::Posters);
    Ui::setSolidColors(v.solidColors == 1);
}

ViewPrefs App::viewOf(const SavedProfile& p) const {
    ViewPrefs v = startView;
    if (p.theme >= 0)        v.theme        = p.theme;
    if (p.homeLayout >= 0)   v.homeLayout   = p.homeLayout;
    if (p.libraryStyle >= 0) v.libraryStyle = p.libraryStyle;
    if (p.solidColors >= 0)  v.solidColors  = p.solidColors;
    return v;
}

void App::keepView(SavedProfile& p) {
    const ViewPrefs v = currentView();
    p.theme = v.theme; p.homeLayout = v.homeLayout; p.libraryStyle = v.libraryStyle; p.solidColors = v.solidColors;
}

static std::string sanitizeConfigValue(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s)
        if (c != '\n' && c != '\r') out += c;
    return out;
}

void App::logSettings() {
    int l, t, r, b;
    Ui::safeArea(l, t, r, b);
    SYS_Report("[Settings] ssl %d, music %d, sounds %d, quality %s, direct %d, smooth %d, "
               "theme %s, christmas %d, home %s, view %d, light %d, area %d,%d,%d,%d, solid %d, clock %s, language %s\n",
               jellyfinClient.sslVerify, musicEnabled, SoundFX::enabled(),
               JellyfinClient::videoQualityName(jellyfinClient.videoQuality), jellyfinClient.directPlay,
               g_wiifin_smooth_motion, Ui::themeName(Ui::chosenTheme()), Ui::festive(),
               Ui::homeLayout() == Ui::HomeLayout::Rows ? "rows" : "grid", (int)Ui::libraryStyle(),
               (int)g_wiifin_disc_light, l, t, r, b, Ui::solidColors(), Ui::clock12h() ? "12h" : "24h",
               Lang::code(Lang::current()));
}

void App::saveSettings() {
    if (settingsPath.empty()) return;
    const std::string tmp = settingsPath + ".tmp", bak = settingsPath + ".bak";
    FILE* f = fopen(tmp.c_str(), "w");
    if (!f) {
        SYS_Report("[Settings] cannot write %s: %s\n", tmp.c_str(), strerror(errno));
        return;
    }
    fprintf(f, "wiifin_cfg=2\n");
    fprintf(f, "ssl_verify=%d\n",      jellyfinClient.sslVerify ? 1 : 0);
    fprintf(f, "music_enabled=%d\n",    musicEnabled ? 1 : 0);
    fprintf(f, "ui_sounds=%d\n",        SoundFX::enabled() ? 1 : 0);
    fprintf(f, "music_autoplay=%d\n",   MusicSession::autoplay() ? 1 : 0);
    fprintf(f, "video_quality=%d\n",    jellyfinClient.videoQuality);
    fprintf(f, "direct_play=%d\n",      jellyfinClient.directPlay ? 1 : 0);
    /* WiiFin's own look: not the one of the profile open */
    fprintf(f, "ui_theme=%d\n",         startView.theme);
    fprintf(f, "home_layout=%d\n",      startView.homeLayout);
    fprintf(f, "library_view=%d\n",     startView.libraryStyle);
    fprintf(f, "solid_colors=%d\n",     startView.solidColors);
    fprintf(f, "clock_12h=%d\n",        Ui::clock12h() ? 1 : 0);
    fprintf(f, "language=%s\n",         Lang::choiceKey());
    fprintf(f, "smooth_motion=%d\n",    g_wiifin_smooth_motion ? 1 : 0);
    fprintf(f, "christmas=%d\n",        Ui::festive() ? 1 : 0);
    fprintf(f, "disc_light=%d\n",     g_wiifin_disc_light ? 1 : 0);
    fprintf(f, "video_zoom=%d\n",       (int)VideoSurface::zoom());
    {
        int l, t, r, b;
        Ui::safeArea(l, t, r, b);
        fprintf(f, "safe_area=%d,%d,%d,%d\n", l, t, r, b);
    }
    fprintf(f, "screen_area_asked=%d\n", screenAreaAsked ? 1 : 0);
    fprintf(f, "profile_count=%d\n",   (int)profiles.size());
    for (int i = 0; i < (int)profiles.size(); i++) {
        const SavedProfile& p = profiles[i];
        fprintf(f, "profile.%d.server_url=%s\n",  i, sanitizeConfigValue(p.serverUrl).c_str());
        fprintf(f, "profile.%d.username=%s\n",     i, sanitizeConfigValue(p.username).c_str());
        fprintf(f, "profile.%d.server_name=%s\n",  i, sanitizeConfigValue(p.serverName).c_str());
        fprintf(f, "profile.%d.user_id=%s\n",      i, sanitizeConfigValue(p.userId).c_str());
        /* the password each time: no token kept */
        fprintf(f, "profile.%d.access_token=%s\n", i,
                p.signIn == SavedProfile::Password ? "" : sanitizeConfigValue(p.accessToken).c_str());
        if (p.theme >= 0)        fprintf(f, "profile.%d.theme=%d\n",        i, p.theme);
        if (p.homeLayout >= 0)   fprintf(f, "profile.%d.home_layout=%d\n",  i, p.homeLayout);
        if (p.libraryStyle >= 0) fprintf(f, "profile.%d.library_view=%d\n", i, p.libraryStyle);
        if (p.solidColors >= 0)  fprintf(f, "profile.%d.solid_colors=%d\n", i, p.solidColors);
        fprintf(f, "profile.%d.sign_in=%d\n", i, p.signIn);
        if (p.signIn == SavedProfile::Passcode) {
            fprintf(f, "profile.%d.pass_salt=%s\n", i, sanitizeConfigValue(p.passSalt).c_str());
            fprintf(f, "profile.%d.pass_hash=%s\n", i, sanitizeConfigValue(p.passHash).c_str());
        }
    }
    fprintf(f, "end=1\n");
    /* all of it on the card before it replaces the saved file */
    bool ok = fflush(f) == 0 && !ferror(f);
    if (ok && fsync(fileno(f)) != 0) ok = false;
    int err = errno;
    if (fclose(f) != 0) { ok = false; err = errno; }
    if (!ok) {
        SYS_Report("[Settings] writing %s failed (%s): the saved settings are kept\n",
                   tmp.c_str(), strerror(err));
        remove(tmp.c_str());
        return;
    }
    /* the saved file becomes the backup, the new one takes its place
     * (FAT's rename does not replace an existing file) */
    if (cfgWhole(settingsPath)) {
        remove(bak.c_str());
        if (rename(settingsPath.c_str(), bak.c_str()) != 0)
            SYS_Report("[Settings] no backup of %s: %s\n", settingsPath.c_str(), strerror(errno));
    }
    remove(settingsPath.c_str());
    if (rename(tmp.c_str(), settingsPath.c_str()) != 0)
        SYS_Report("[Settings] cannot rename %s: %s (it is read from there)\n", tmp.c_str(), strerror(errno));
}
