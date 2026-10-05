/* App settings and saved profiles: sd:/apps/WiiFin/wiifin.cfg */
#include "App.h"
#include "../ui/Ui.h"
#include "Utils.h"
#include "../player/VideoSurface.h"
#include "../player/vo_wiifin.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void App::loadSettings() {
    if (settingsPath.empty()) return;
    FILE* f = fopen(settingsPath.c_str(), "r");
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
        } else if (strcmp(key, "ui_theme") == 0) {
            {
                int t = atoi(val);
                Ui::setTheme(t >= 0 && t < Ui::THEME_COUNT ? (Ui::Theme)t : Ui::Theme::Dark);
                /* settings saved before home_layout existed: Flix meant rows
                 * (home_layout, written after ui_theme, overrides this) */
                if (Ui::theme() == Ui::Theme::Flix) Ui::setHomeLayout(Ui::HomeLayout::Rows);
            }
        } else if (strcmp(key, "smooth_motion") == 0) {
            g_wiifin_smooth_motion = atoi(val) != 0;
        } else if (strcmp(key, "video_zoom") == 0) {
            VideoSurface::setZoom(atoi(val) == 1 ? VideoSurface::Zoom::Fill : VideoSurface::Zoom::Fit);
        } else if (strcmp(key, "library_view") == 0) {
            int v = atoi(val);
            if (v >= 0 && v < Ui::LIBRARY_STYLE_COUNT) Ui::setLibraryStyle((Ui::LibraryStyle)v);
        } else if (strcmp(key, "home_layout") == 0) {
            Ui::setHomeLayout(atoi(val) == 1 ? Ui::HomeLayout::Rows : Ui::HomeLayout::Grid);
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
}

static std::string sanitizeConfigValue(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s)
        if (c != '\n' && c != '\r') out += c;
    return out;
}

void App::saveSettings() {
    if (settingsPath.empty()) return;
    FILE* f = fopen(settingsPath.c_str(), "w");
    if (!f) return;
    fprintf(f, "ssl_verify=%d\n",      jellyfinClient.sslVerify ? 1 : 0);
    fprintf(f, "music_enabled=%d\n",    musicEnabled ? 1 : 0);
    fprintf(f, "video_quality=%d\n",    jellyfinClient.videoQuality);
    fprintf(f, "ui_theme=%d\n",         (int)Ui::theme());
    fprintf(f, "home_layout=%d\n",      (int)Ui::homeLayout());
    fprintf(f, "library_view=%d\n",     (int)Ui::libraryStyle());
    fprintf(f, "smooth_motion=%d\n",    g_wiifin_smooth_motion ? 1 : 0);
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
        fprintf(f, "profile.%d.access_token=%s\n", i, sanitizeConfigValue(p.accessToken).c_str());
    }
    fflush(f);
    fclose(f);
}
