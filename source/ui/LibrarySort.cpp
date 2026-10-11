/* LibraryView: sort and filter a film or series library (2 in its list or
 * grid): by name, year, rating or date added, ascending or descending; one
 * genre; all, not watched, watched or favourites.  The server sorts and filters (the
 * query), the list reloads. */
#include "LibraryView.h"
#include "LibraryDraw.h"
#include "Ui.h"
#include "../input/Input.h"
#include "../core/Lang.h"
#include <stdio.h>

using namespace LibDraw;

static const char* const SORTS[] = { N_("Name"), N_("Year"), N_("Rating"), N_("Date added") };
static const char* const SHOWS[] = { N_("All"), N_("Not watched"), N_("Watched"), N_("Favorites") };

bool LibraryView::sortable() const
{
    if (inItemsDrilldown || inBoxSetDrilldown) return false;
    if (!posterMode) return true;
    return currentLibType == "movies" ? movieTab == 0 : tvTab == 0;
}

std::string LibraryView::sortQuery() const
{
    static const char* const BY[] = { "SortName", "ProductionYear,SortName",
                                      "CommunityRating,SortName", "DateCreated,SortName" };
    std::string q;
    if (listSort.sort != 0 || listSort.desc)   /* name, A to Z: the server's default */
        q = std::string("&SortBy=") + BY[listSort.sort] + "&SortOrder=" +
            (listSort.desc ? "Descending" : "Ascending");
    if (listSort.genre >= 0 && listSort.genre < (int)sortGenres.size())
        q += "&GenreIds=" + sortGenres[listSort.genre].id;
    if (listSort.show == 1) q += "&IsPlayed=false";
    if (listSort.show == 2) q += "&IsPlayed=true";
    if (listSort.show == 3) q += "&IsFavorite=true";
    /* the filters apply to the titles themselves, not to the library's
     * folders: a recursive search for that type (none matched otherwise) */
    if (!q.empty())
        q += currentLibType == "movies" ? "&Recursive=true&IncludeItemTypes=Movie"
                                        : "&Recursive=true&IncludeItemTypes=Series";
    return q;
}

std::string LibraryView::sortSummary() const
{
    std::string s;
    auto add = [&](const std::string& part) { s += (s.empty() ? "" : "  \xc2\xb7  ") + part; };
    if (listSort.sort) add(tr(SORTS[listSort.sort]));
    if (listSort.desc != (listSort.sort != 0))   /* not the usual way round */
        add(listSort.desc ? tr("Descending") : tr("Ascending"));
    if (listSort.genre >= 0 && listSort.genre < (int)sortGenres.size()) add(sortGenres[listSort.genre].name);
    if (listSort.show) add(tr(SHOWS[listSort.show]));
    return s;
}

void LibraryView::openSortPanel()
{
    if (listSortLib != currentLibId) {   /* another library: its own genres, no filter */
        listSort    = ListSort();
        listSortLib = currentLibId;
        sortGenres.clear();
        runWithLoading([&]() {
            client.getItemsByQuery(serverUrl, auth,
                                   "/Genres?ParentId=" + currentLibId + "&UserId=" + auth.userId +
                                   "&SortBy=SortName", sortGenres);
        });
    }
    sortEdit  = listSort;
    sortRow   = 0;
    sortPanel = true;
}

void LibraryView::updateSortPanel()
{
    if (Input::isBackPressed()) { sortPanel = false; return; }
    if (Input::isUpPressed()   && sortRow > 0) sortRow--;
    if (Input::isDownPressed() && sortRow < 3) sortRow++;
    int d = Input::isRightPressed() ? 1 : (Input::isLeftPressed() ? -1 : 0);
    if (d) {
        if (sortRow == 0) {   /* each sort starts the usual way round */
            sortEdit.sort = (sortEdit.sort + d + 4) % 4;
            sortEdit.desc = sortEdit.sort != 0;
        }
        if (sortRow == 1) sortEdit.desc = !sortEdit.desc;
        if (sortRow == 3) sortEdit.show = (sortEdit.show + d + 4) % 4;
        if (sortRow == 2) {   /* -1 (every genre) .. the last one */
            int n = (int)sortGenres.size() + 1;
            sortEdit.genre = ((sortEdit.genre + 1 + d + n) % n) - 1;
        }
    }
    if (Input::isAJustPressed()) {
        sortPanel = false;
        listSort  = sortEdit;
        SYS_Report("[Library] sort %s, genre %d, show %s, %s\n", SORTS[listSort.sort], listSort.genre,
                   SHOWS[listSort.show], listSort.desc ? "descending" : "ascending");
        if (posterMode) freePosters();
        itemPage = 0;
        state = State::ItemsInit;
    }
}

void LibraryView::renderSortPanel()
{
    const Ui::Palette& p = Ui::pal();
    const float W = 380, H = 256, X = 320 - W / 2, Y = 97;
    Ui::roundRect(Ui::screenLeft(), 0, Ui::screenWidth(), 480, 0, 0x00000080);
    Ui::shadow(X, Y + 4, W, H, 16, 10.0f, p.shadow);
    Ui::roundRect(X, Y, W, H, 16, p.cardTop, p.cardBottom);
    Ui::roundBorder(X, Y, W, H, 16, 1.5f, p.cardBorder);
    Ui::textCentered(320, Y + 14, tr("Sort & filter"), 19, p.text);

    const char* labels[4] = { N_("Sort by"), N_("Order"), N_("Genre"), N_("Show") };
    std::string values[4] = {
        tr(SORTS[sortEdit.sort]),
        sortEdit.desc ? tr("Descending") : tr("Ascending"),
        sortEdit.genre >= 0 && sortEdit.genre < (int)sortGenres.size()
            ? fitText(font, sortGenres[sortEdit.genre].name, 16, 180) : std::string(tr("Every genre")),
        tr(SHOWS[sortEdit.show]),
    };
    for (int i = 0; i < 4; ++i) {
        float ry = Y + 50 + i * 42;
        bool on = i == sortRow;
        Ui::card(X + 16, ry, W - 32, 34, 10, on ? Ui::pulse() : 0.0f);
        Ui::text(X + 30, ry + 8, tr(labels[i]), 15, p.textDim);
        std::string v = std::string(on ? "\xe2\x80\xb9  " : "") + values[i] + (on ? "  \xe2\x80\xba" : "");
        Ui::textRight(X + W - 30, ry + 7, v.c_str(), 16, on ? p.text : p.textDim);
    }
    /* TRANSLATORS: A and B are the Wii Remote's buttons */
    Ui::textCentered(320, Y + H - 28, tr("A  apply      B  cancel"), 13, p.textDim);
}
