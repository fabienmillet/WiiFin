# Translating WiiFin

WiiFin's interface is written in English; each `<code>.po` file here
translates it into one language. They are standard gettext files: open
one in [Poedit](https://poedit.net/) (or any text editor), fill in the
translations, and send the file back (a pull request, or attached to an
issue or on the Discord).

WiiFin shows the Wii's own language (Wii Settings) when there is a file
for it, English otherwise; **Settings > Language** picks another.

## Starting a language

```sh
tools/lang_update.sh de      # makes lang/de.po from lang/wiifin.pot
```

Without the tools, copy `wiifin.pot` to `<code>.po` (`de`, `es`, `it`,
`nl`, `pt_BR`, `ja`, `ko`, `zh_CN`...) and set `Language: <code>` in its
header.

## Rules of thumb

- Keep `%d`, `%s`, `%.1f` and the like, in the same order: the build
  refuses a translation where they differ (they would crash the Wii).
- The screen is small (640 px wide, 14 px text in the bottom bar): keep
  translations about as short as the English. A `TRANSLATORS:` comment
  above a string says where it is shown when that matters.
- An empty translation shows the English: a file can be partly done.
- Button names (A, B, 1, 2, -/+, HOME) are drawn as the buttons: they
  aren't in the files.

## For developers

Mark a string the interface shows with `tr("...")`, or `N_("...")`
where it is stored and translated by the code that draws it (see
`source/core/Lang.h`). Never translate the log. After adding strings:

```sh
tools/lang_update.sh         # refreshes wiifin.pot and every .po
```

The build compiles each `.po` into the DOL (`tools/lang_table.sh`).
