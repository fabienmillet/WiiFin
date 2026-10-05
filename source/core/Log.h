#pragma once
#include <string>

/* -----------------------------------------------------------------------
 * Log — wiifin.log next to the settings file, for bug reports from real
 * Wiis (no USB Gecko there).
 *
 * Every SYS_Report() line also lands in the file (the linker wraps
 * SYS_Report, see Log.cpp), so the existing [Net] / [WiiPlayer] / [HTTP]
 * diagnostics need no extra calls.  A low-priority thread writes the lines
 * to the card every 250 ms (never the caller: SD writes would stall the
 * player), so a crash loses at most that much; the previous session is kept
 * as wiifin.prev.log (the one that crashed, when the user relaunches).
 *
 * Users post this file publicly: credentials, the user id, IPv4 addresses
 * and the server's name (addPrivate) are masked on the way in.
 * ----------------------------------------------------------------------- */
namespace Log {
    /* Call first thing in main(). */
    void init();
    /* dir ends with '/'; lines reported before this are written first. */
    void open(const std::string& dir);
    /* Before leaving the app: writes what is pending, closes the file. */
    void close();
    /* Replace this name (the server's host) by "<server>" from now on. */
    void addPrivate(const std::string& name);
    void addPrivateUrl(const std::string& url);    /* its host */
}
