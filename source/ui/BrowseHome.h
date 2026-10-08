#pragma once
#include <grrlib.h>
#include <wiiuse/wpad.h>
#include <ogc/lwp.h>
#include <ogc/mutex.h>
#include <string>
#include <vector>
#include "../jellyfin/JellyfinClient.h"

/* -----------------------------------------------------------------------
 * BrowseHome — home screen of the Flix theme.
 *
 * Rows of categories (continue watching, next up, latest and top rated
 * per library, favourites), each a horizontal carousel of posters, like the
 * video channels of the Wii era.  The focused row sits on a lighter band
 * with an info panel under the selected poster; the next row peeks in
 * below.  Up/Down slide between rows, Left/Right scroll the carousel.
 *
 * Posters are fetched by a background thread so scrolling never blocks.
 * The thread uses the shared JellyfinClient connection: stopLoader() must
 * be called before the owner makes any other request (LibraryView does it
 * whenever it leaves the home state).
 * ----------------------------------------------------------------------- */
class BrowseHome {
public:
    enum class Action { None, Open, Search, Browse, Back, Profiles };
    /* Home: what to watch now (resume, next up, latest, top rated, favourites).
     * Catalog: the "browse" page (libraries, every title A-Z, genres). */
    enum class Mode { Home, Catalog };

    BrowseHome(JellyfinClient& client, const std::string& serverUrl, const JellyfinAuth& auth,
               Mode mode = Mode::Home);
    ~BrowseHome();

    /* Fetch the rows (blocking network; run it inside a loading screen). */
    void build(const std::vector<JellyfinLibrary>& libs);
    bool built() const { return isBuilt; }
    bool empty() const { return rows.empty(); }
    /* Forget the rows so the next visit rebuilds them (e.g. after playback). */
    void invalidate();

    void startLoader();
    void stopLoader();
    bool loaderRunning() const { return loaderThread != LWP_THREAD_NULL; }

    Action update(const ir_t& ir, bool& irMode);
    void   render(const ir_t& ir);

    /* Item under the selection (valid after Action::Open). */
    const JellyfinItem* selectedItem() const;
    /* Changes whenever the highlight moves (LibraryView's sounds) */
    unsigned focusKey() const {
        unsigned k = (unsigned)(rowSel + 1) * 7919u + (unsigned)(headerFocus + 2) * 104729u;
        if (rowSel >= 0 && rowSel < (int)rows.size()) k += (unsigned)rows[rowSel].sel * 2654435761u;
        return k;
    }
    void setUserName(const std::string& n) { userName = n; }

private:
    struct Row {
        std::string                 title;
        bool                        square = false;   /* album art */
        bool                        wide   = false;   /* 16:9: the libraries' own pictures */
        std::vector<JellyfinItem>   items;
        std::vector<GRRLIB_texImg*> tex;
        std::vector<unsigned char>  st;               /* TileState */
        int                         sel    = 0;
        float                       scroll = 0.0f;   /* animated, follows `first` */
        int                         first  = 0;      /* first visible tile         */
    };
    enum TileState : unsigned char { TILE_NONE, TILE_LOADING, TILE_READY, TILE_FAILED };

    JellyfinClient&     client;
    Mode                mode;
    std::string         serverUrl;
    JellyfinAuth        auth;

    std::string         userName;
    std::vector<Row>    rows;
    bool                isBuilt  = false;
    int                 rowSel   = 0;
    float               rowAnim  = 0.0f;
    int                 headerFocus = -1;   /* -1 rows, 0 search, 1 browse, 2 profile */
    int                 hoverHeader = -1;
    int                 hoverArrow  = 0;    /* -1 left, +1 right: pointer on a side arrow */
    unsigned long long  arrowSince  = 0;    /* dwell-to-scroll timing (ms)                */
    unsigned long long  arrowLast   = 0;

    lwp_t               loaderThread = LWP_THREAD_NULL;
    mutex_t             texLock      = LWP_MUTEX_NULL;
    volatile bool       stopReq      = false;

    void buildHome(const std::vector<JellyfinLibrary>& libs);
    void buildCatalog(const std::vector<JellyfinLibrary>& libs);
    void freeTextures(Row& r);
    void tileWindow(const Row& r, int& first, int& last) const;
    void evictFarRows();
    bool pickNext(int& r, int& i);
    void loaderLoop();
    static void* loaderMain(void* self);

    float tileW(const Row& r) const;
    float stride(const Row& r) const;
    int   fullyVisible(const Row& r) const;
    float targetScroll(const Row& r) const;
    void  scrollRow(Row& r, int delta);      /* pointer: move the view, not the selection */
    void  followSelection(Row& r);           /* d-pad: keep the selection in view          */

    void drawRow(int r, float offY, float focus);
    void drawInfoPanel(float k);
    void drawHeader();
};
