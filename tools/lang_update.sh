#!/bin/bash
# lang_update.sh [code]: lang/wiifin.pot from the strings the source marks
# (tr, N_), then each lang/<code>.po brought up to date with it: new
# strings empty, changed ones marked fuzzy, gone ones kept at the end as
# #~ lines.  With a code, lang/<code>.po is made first when there is none.
# How to translate: lang/README.md.
set -e
cd "$(dirname "$0")/.."
mkdir -p lang
xgettext --from-code=UTF-8 --language=C++ --keyword=tr --keyword=N_ --add-comments=TRANSLATORS: \
    --package-name=WiiFin --msgid-bugs-address=https://github.com/fabienmillet/WiiFin/issues \
    --sort-by-file --no-wrap -o lang/wiifin.pot $(git ls-files --cached --others --exclude-standard 'source/*.cpp' 'source/*.h')
if [ -n "$1" ] && [ ! -f "lang/$1.po" ]; then
    msginit --no-translator --no-wrap -l "$1.UTF-8" -i lang/wiifin.pot -o "lang/$1.po"
fi
for po in lang/*.po; do
    [ -f "$po" ] || continue
    msgmerge -q -U --backup=none --no-wrap "$po" lang/wiifin.pot
    printf '%s: ' "$po"; msgfmt -c --statistics -o /dev/null "$po"
done
