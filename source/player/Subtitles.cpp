#include "Subtitles.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <algorithm>

namespace {

/* "01:02:03,456" (or with a '.') -> seconds; -1 if it is not a time */
float parseTime(const char* s)
{
    int h, m, sec, ms;
    char sep;
    if (sscanf(s, "%d:%d:%d%c%d", &h, &m, &sec, &sep, &ms) != 5 || (sep != ',' && sep != '.'))
        return -1.0f;
    return h * 3600.0f + m * 60.0f + sec + ms / 1000.0f;
}

/* A code point as UTF-8 */
void putUtf8(std::string& out, unsigned cp)
{
    if (cp < 0x80)         out += (char)cp;
    else if (cp < 0x800)   { out += (char)(0xC0 | cp >> 6);  out += (char)(0x80 | (cp & 0x3F)); }
    else if (cp < 0x10000) { out += (char)(0xE0 | cp >> 12); out += (char)(0x80 | ((cp >> 6) & 0x3F));
                             out += (char)(0x80 | (cp & 0x3F)); }
    else                   { out += (char)(0xF0 | cp >> 18); out += (char)(0x80 | ((cp >> 12) & 0x3F));
                             out += (char)(0x80 | ((cp >> 6) & 0x3F)); out += (char)(0x80 | (cp & 0x3F)); }
}

/* An HTML entity at in[i] ('&'): its length, its text in out; 0 = none */
size_t entity(const std::string& in, size_t i, std::string& out)
{
    size_t e = in.find(';', i);
    if (e == std::string::npos || e - i > 9) return 0;
    std::string name = in.substr(i + 1, e - i - 1);
    static const struct { const char* name; unsigned cp; } NAMED[] = {
        { "amp", '&' }, { "lt", '<' }, { "gt", '>' }, { "quot", '"' }, { "apos", '\'' },
        { "nbsp", 0xA0 },
    };
    unsigned cp = 0;
    if (name.size() > 1 && name[0] == '#')
        cp = (unsigned)strtoul(name.c_str() + (name[1] == 'x' || name[1] == 'X' ? 2 : 1), nullptr,
                               name[1] == 'x' || name[1] == 'X' ? 16 : 10);
    else
        for (const auto& n : NAMED) if (name == n.name) cp = n.cp;
    if (!cp || cp > 0x10FFFF) return 0;
    putUtf8(out, cp);
    return e - i + 1;
}

/* Styling out: HTML-like tags (<i>, <font ...>) and ASS overrides ({\an8});
 * entities (&amp; &nbsp; &#39;...) and ASS's hard space (\h) as their
 * characters */
std::string cleanLine(const std::string& in)
{
    std::string out;
    for (size_t i = 0; i < in.size(); ++i) {
        char c = in[i];
        if (c == '&') {
            if (size_t n = entity(in, i, out)) { i += n - 1; continue; }
        }
        if (c == '\\' && i + 1 < in.size() && in[i + 1] == 'h') { out += ' '; ++i; continue; }
        if (c == '<' && in.find('>', i) != std::string::npos) { i = in.find('>', i); continue; }
        if (c == '{' && i + 1 < in.size() && in[i + 1] == '\\' && in.find('}', i) != std::string::npos) {
            i = in.find('}', i);
            continue;
        }
        if (c == '\\' && i + 1 < in.size() && (in[i + 1] == 'N' || in[i + 1] == 'n')) { out += '\n'; ++i; continue; }
        if (c != '\r') out += c;
    }
    /* trim */
    size_t a = out.find_first_not_of(" \t"), b = out.find_last_not_of(" \t");
    return a == std::string::npos ? "" : out.substr(a, b - a + 1);
}

} // namespace

bool Subtitles::load(const std::string& srt)
{
    clear();
    size_t p = 0;
    if (srt.compare(0, 3, "\xEF\xBB\xBF") == 0) p = 3;   /* UTF-8 BOM */
    Cue cue;
    bool inCue = false;
    while (p <= srt.size()) {
        size_t e = srt.find('\n', p);
        std::string line = srt.substr(p, e == std::string::npos ? std::string::npos : e - p);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        p = e == std::string::npos ? srt.size() + 1 : e + 1;

        size_t arrow = line.find("-->");
        if (arrow != std::string::npos) {
            if (inCue && !cue.text.empty()) cues.push_back(cue);
            cue = Cue();
            cue.from = parseTime(line.c_str());
            cue.to   = parseTime(line.c_str() + arrow + 3 + strspn(line.c_str() + arrow + 3, " "));
            inCue = cue.from >= 0.0f && cue.to > cue.from;
            continue;
        }
        if (!inCue) continue;
        if (line.empty()) {                  /* end of the cue */
            if (!cue.text.empty()) cues.push_back(cue);
            inCue = false;
            continue;
        }
        std::string t = cleanLine(line);
        if (!t.empty()) cue.text += (cue.text.empty() ? "" : "\n") + t;
    }
    if (inCue && !cue.text.empty()) cues.push_back(cue);
    std::stable_sort(cues.begin(), cues.end(), [](const Cue& a, const Cue& b) { return a.from < b.from; });
    return !cues.empty();
}

const Subtitles::Cue* Subtitles::at(float secs)
{
    if (cues.empty()) return nullptr;
    /* from the previous place when playback just moved on, else a search */
    if (last >= cues.size() || cues[last].from > secs) {
        size_t lo = 0, hi = cues.size();
        while (lo < hi) {
            size_t mid = (lo + hi) / 2;
            if (cues[mid].to <= secs) lo = mid + 1; else hi = mid;
        }
        last = lo;
    }
    while (last < cues.size() && cues[last].to <= secs) ++last;
    if (last < cues.size() && cues[last].from <= secs) return &cues[last];
    return nullptr;
}
