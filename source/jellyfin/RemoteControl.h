#pragma once
#include <string>
#include <vector>

/* -----------------------------------------------------------------------
 * Remote — the server's remote control (the dashboard's play / pause /
 * stop buttons and "Send message", the Jellyfin apps' controls).
 *
 * Jellyfin sends its commands through a WebSocket only (/socket): a thread
 * keeps one open beside the HTTP connection, on a socket of its own, and
 * reconnects when it drops.  JellyfinClient::postCapabilities tells the
 * server what WiiFin obeys (without it the dashboard shows no buttons).
 *
 * Playback commands wait in a queue for the player (setPlayerActive); in
 * the menus they are dropped.  Messages show over any screen (pump, from
 * the render hook).
 * ----------------------------------------------------------------------- */
namespace Remote {
    enum class Cmd {
        None,
        Pause, Unpause, PlayPause, Stop,
        Seek,          /* ticks: the position              */
        Next, Prev,    /* next / previous episode or track */
        FastForward, Rewind,
        VolumeUp, VolumeDown,
        SetVolume,     /* value: 0-100                     */
        Mute, Unmute, ToggleMute,
        AudioTrack,    /* value: Jellyfin stream index     */
        SubtitleTrack, /* value: Jellyfin stream index, -1 none */
    };
    struct Command {
        Cmd       cmd   = Cmd::None;
        long long ticks = 0;
        int       value = 0;
    };

    /* the profile's server (as saved: http(s)://host[:port][/path]) */
    void start(const std::string& serverUrl, const std::string& token, bool verifyTls);
    void stop();
    bool connected();

    /* "Play on..." (the apps' cast menu, the dashboard): items to play
     * now, the first one at startIndex from startTicks.  Waits (30 s at
     * most) for the library screen to start it; a player stops for it.
     * "Play next" / "Add to queue" (next = true / false) go to the player's
     * queue (takeQueue); with nothing playing they play now. */
    struct PlayRequest {
        std::vector<std::string> ids;
        int       startIndex = 0;
        long long startTicks = 0;
        bool      queue = false;   /* PlayNext / PlayLast */
        bool      next  = false;   /* PlayNext            */
    };
    bool hasPlay();
    bool takePlay(PlayRequest& r);
    bool takeQueue(PlayRequest& r);   /* a player: items for its queue */

    void setPlayerActive(bool on);
    bool poll(Command& c);      /* the player: next command, oldest first */
    const char* name(Cmd c);    /* for the log */

    /* every frame: messages to the screen, commands dropped in the menus */
    void pump();
}
