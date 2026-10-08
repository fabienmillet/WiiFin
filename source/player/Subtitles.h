#pragma once

#include <string>
#include <vector>

/* -----------------------------------------------------------------------
 * Subtitles — a text track (SRT, as Jellyfin converts any text subtitles
 * to), drawn by WiiFin over the picture instead of burned in by the
 * server: the video can then play as it is (direct play), and the server
 * has no picture to re-encode.
 * ----------------------------------------------------------------------- */
class Subtitles {
public:
    struct Cue {
        float from = 0.0f, to = 0.0f;   /* seconds into the item */
        std::string text;               /* lines separated by '\n', tags removed */
    };

    /* Replaces the track with an SRT file's cues; false when none parsed. */
    bool load(const std::string& srt);
    void clear() { cues.clear(); last = 0; }
    bool empty() const { return cues.empty(); }

    /* The cue shown at secs, nullptr between cues. */
    const Cue* at(float secs);

private:
    std::vector<Cue> cues;
    size_t last = 0;   /* where the previous lookup ended: playback moves on */
};
