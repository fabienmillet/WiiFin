#pragma once
#include <string>

/* One saved Jellyfin account.
 * The password is NEVER stored — only the access token returned by the server. */
struct SavedProfile {
    std::string serverUrl;
    std::string username;    /* display name; empty when authenticated via Quick Connect */
    std::string serverName;  /* friendly name retrieved from the server */
    std::string userId;
    std::string accessToken; /* Jellyfin session token — not the password */

    /* its own look (View Settings), -1: WiiFin's (the start-up one) */
    int theme = -1, homeLayout = -1, libraryStyle = -1, solidColors = -1;

    /* how it is opened: at once (its token kept), after a passcode, or
     * with the password (or Quick Connect) each time: no token kept */
    enum SignIn { Auto = 0, Passcode = 1, Password = 2 };
    int signIn = Auto;
    std::string passSalt, passHash;   /* the passcode's SHA-256, never the code */
};

/* The look a profile has, or WiiFin's start-up one (the menus) */
struct ViewPrefs { int theme = 1, homeLayout = 0, libraryStyle = 0, solidColors = 0; };
