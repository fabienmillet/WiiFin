/* The interface's language.  The source is in English; lang/<code>.po
 * (gettext) translate it, compiled into the DOL (tools/lang_table.sh).
 *
 *   tr("Settings")    the string in the language chosen, or as it is
 *   N_("Settings")    marks a string kept for later (a table, a hint):
 *                     the code that shows it calls tr() on it
 *
 * Only what the interface says goes through tr(): never the log
 * (SYS_Report, the tests read it), the server's queries, nor the names
 * coming from the server. */
#pragma once

#include <string>

#define N_(s) s

const char* tr(const char* en);

namespace Lang {

/* The languages there are: 0 is English (the source), then one per
 * lang/<code>.po, in the order of their codes. */
int         count();
const char* code(int i);        /* "en", "fr"... */
const char* name(int i);        /* in that language: "English", "Français"... */

/* The choice: -1 follows the Wii's language (Wii Settings), else one of
 * the languages above. */
void        choose(int i);
int         choice();
int         current();          /* the language in use */
/* the SD card's fonts/ has Korean (Hangul) or Chinese characters: without,
 * those languages would draw nothing, so they are left out (the Wii's
 * language among them: English then; a choice of one waits for the font) */
void        setFonts(bool hangul, bool han);
/* the next language Settings offers after i (-1 the Wii's): the ones the
 * fonts can draw */
int         next(int i);
/* wiifin.cfg's language=: "wii" or a code */
const char* choiceKey();
void        chooseKey(const char* key);

/* a running time: "1h 3min", "45min"; "" under a minute */
std::string runTime(int minutes);

} // namespace Lang
