/* LibraryView: a photo library's pictures, one at a time on the whole
 * screen: Left / Right the folder's others, A a slideshow, B back to the
 * list. */
#include "LibraryView.h"
#include "LibraryDraw.h"
#include "Ui.h"
#include "JpegTexture.h"
#include "../input/Input.h"
#include "../core/Lang.h"
#include "../core/SoundFX.h"
#include <ogc/lwp_watchdog.h>
#include <ogc/mutex.h>
#include <stdio.h>
#include <unistd.h>

using namespace LibDraw;

static const u64 SLIDE_MS = 5000;   /* a slideshow's photo stays this long */

static u64 nowMs() { return ticks_to_millisecs(gettime()); }
static const u64 FADE_MS = 450;     /* one photo into the next */

/* The next photo (the one Right or the slideshow shows next, or Left's)
 * fetched and decoded meanwhile on a thread of its own: it then comes at
 * once, no loading screen; the client's requests take their turn */
namespace {
struct Prefetch {
    lwp_t   thread = LWP_THREAD_NULL;
    mutex_t lock = LWP_MUTEX_NULL;
    bool    quit = false, busy = false;
    std::string want, have;           /* the photo asked for, the one ready */
    GRRLIB_texImg* tex = nullptr;     /* have's */
    JellyfinClient* client = nullptr;
    std::string url;
    JellyfinAuth auth;
};
Prefetch s_pre;
u8 s_preStack[32 * 1024] __attribute__((aligned(32)));

void* prefetchMain(void*)
{
    for (;;) {
        LWP_MutexLock(s_pre.lock);
        if (s_pre.quit) { LWP_MutexUnlock(s_pre.lock); break; }
        const std::string id = s_pre.want != s_pre.have ? s_pre.want : std::string();
        s_pre.busy = !id.empty();
        LWP_MutexUnlock(s_pre.lock);
        if (id.empty()) { usleep(20 * 1000); continue; }
        std::string bytes;
        s_pre.client->getPhotoBytes(s_pre.url, s_pre.auth, id, 640, 480, bytes);
        GRRLIB_texImg* t = bytes.empty() ? nullptr : loadJPEGTexture((const u8*)bytes.data(), (u32)bytes.size());
        LWP_MutexLock(s_pre.lock);
        if (s_pre.tex) GRRLIB_FreeTexture(s_pre.tex);
        s_pre.tex = t;
        s_pre.have = id;
        s_pre.busy = false;
        LWP_MutexUnlock(s_pre.lock);
    }
    return nullptr;
}

void prefetchStart(JellyfinClient& c, const std::string& url, const JellyfinAuth& a)
{
    if (s_pre.thread != LWP_THREAD_NULL) return;
    if (s_pre.lock == LWP_MUTEX_NULL) LWP_MutexInit(&s_pre.lock, false);
    s_pre.client = &c;
    s_pre.url = url;
    s_pre.auth = a;
    s_pre.quit = false;
    s_pre.want.clear();
    s_pre.have.clear();
    LWP_CreateThread(&s_pre.thread, prefetchMain, nullptr, s_preStack, sizeof(s_preStack), 40);
}

void prefetchStop()
{
    if (s_pre.thread == LWP_THREAD_NULL) return;
    LWP_MutexLock(s_pre.lock);
    s_pre.quit = true;
    LWP_MutexUnlock(s_pre.lock);
    LWP_JoinThread(s_pre.thread, nullptr);
    s_pre.thread = LWP_THREAD_NULL;
    if (s_pre.tex) { GRRLIB_FreeTexture(s_pre.tex); s_pre.tex = nullptr; }
    s_pre.want.clear();
    s_pre.have.clear();
}

void prefetchAsk(const std::string& id)
{
    LWP_MutexLock(s_pre.lock);
    s_pre.want = id;
    LWP_MutexUnlock(s_pre.lock);
}

/* that photo, if it is ready (or comes within wait ms); null otherwise */
GRRLIB_texImg* prefetchTake(const std::string& id, u64 wait)
{
    const u64 until = nowMs() + wait;
    for (;;) {
        LWP_MutexLock(s_pre.lock);
        if (s_pre.have == id && s_pre.tex) {
            GRRLIB_texImg* t = s_pre.tex;
            s_pre.tex = nullptr;
            s_pre.have.clear();
            LWP_MutexUnlock(s_pre.lock);
            return t;
        }
        const bool coming = s_pre.want == id && (s_pre.busy || s_pre.have != id);
        LWP_MutexUnlock(s_pre.lock);
        if (!coming || nowMs() >= until) return nullptr;
        usleep(10 * 1000);
    }
}
} // namespace

/* the folder's photos (not its folders or videos), from the one opened: the
 * list's own query (a type filter makes Jellyfin look in the subfolders too) */
void LibraryView::openPhotos(const std::string& id, State back)
{
    photos.clear();
    bool ok = false;
    std::string err;
    std::vector<JellyfinItem> all;
    runWithLoading([&]() {
        int total = 0;
        ok = client.getItems(serverUrl, auth, currentLibId, 0, 1000, all, total, "");
        if (!ok) err = client.lastError();
    });
    if (!ok) { errMsg = err; state = State::Error; return; }
    for (const JellyfinItem& it : all) if (it.type == "Photo") photos.push_back(it);
    int at = 0;
    for (int i = 0; i < (int)photos.size(); ++i) if (photos[i].id == id) at = i;
    if (photos.empty()) return;
    SYS_Report("[Photos] %d in the folder, from photo %d\n", (int)photos.size(), at + 1);
    photoBack = back;
    slideshow = false;
    prefetchStart(client, serverUrl, auth);
    showPhoto(at);
    state = State::PhotoReady;
}

void LibraryView::showPhoto(int i)
{
    const int n = (int)photos.size();
    if (n == 0) return;
    const int was = photoSel;
    photoSel = (i % n + n) % n;
    /* the one fetched meanwhile; still coming, a moment more; else (or too
     * long) fetched now, the loading screen meanwhile */
    GRRLIB_texImg* t = prefetchTake(photos[photoSel].id, 2000);
    if (!t) {
        std::string bytes;
        runWithLoading([&]() {
            client.getPhotoBytes(serverUrl, auth, photos[photoSel].id, 640, 480, bytes);
        });
        if (!bytes.empty()) t = loadJPEGTexture((const u8*)bytes.data(), (u32)bytes.size());
    }
    /* the last one fading out under it */
    if (photoPrevTex) GRRLIB_FreeTexture(photoPrevTex);
    photoPrevTex = photoTex;
    photoTex = t;
    photoShownAt = photoFadeAt = nowMs();
    /* the next the same way along (back: the one before) */
    const int dir = photoSel == (was + n - 1) % n && n > 2 ? -1 : 1;
    if (n > 1) prefetchAsk(photos[(photoSel + dir + n) % n].id);
}

void LibraryView::closePhotos()
{
    prefetchStop();
    if (photoTex) { GRRLIB_FreeTexture(photoTex); photoTex = nullptr; }
    if (photoPrevTex) { GRRLIB_FreeTexture(photoPrevTex); photoPrevTex = nullptr; }
    photos.clear();
    slideshow = false;
}

bool LibraryView::updatePhoto(ir_t& ir, bool aPressed)
{
    (void)ir;
    if (Input::isBackPressed()) {
        closePhotos();
        state = photoBack;
        return false;
    }
    if (Input::isRightPressed() || Input::isRPressed()) { SoundFX::play(SoundFX::FX::Move); showPhoto(photoSel + 1); }
    if (Input::isLeftPressed() || Input::isLPressed())  { SoundFX::play(SoundFX::FX::Move); showPhoto(photoSel - 1); }
    if (aPressed) {
        slideshow = !slideshow;
        photoShownAt = nowMs();
        SYS_Report("[Photos] slideshow %s\n", slideshow ? "on" : "off");
    }
    if (slideshow && photos.size() > 1 && nowMs() - photoShownAt > SLIDE_MS) showPhoto(photoSel + 1);
    return false;
}

void LibraryView::renderPhoto(ir_t& ir)
{
    (void)ir;
    const Ui::Palette& p = Ui::pal();
    Ui::roundRect(Ui::screenLeft(), 0, Ui::screenWidth(), 480, 0, 0x000000FF);
    /* the whole photo, as large as the screen allows; the last one fading
     * out under it */
    auto draw = [](GRRLIB_texImg* t, float a) {
        if (!t || t->w <= 0 || t->h <= 0 || a <= 0) return;
        const float W = Ui::screenWidth(), H = 450;
        float sc = W / t->w;
        if (t->h * sc > H) sc = H / t->h;
        const float w = t->w * sc, h = t->h * sc;
        GRRLIB_DrawImg(320 - w / 2, H / 2 - h / 2, t, 0, sc, sc, 0xFFFFFF00 | (u32)(255 * a));
    };
    float k = (float)(nowMs() - photoFadeAt) / FADE_MS;
    if (k > 1) k = 1;
    k = k * k * (3 - 2 * k);
    if (k < 1) draw(photoPrevTex, 1 - k);
    else if (photoPrevTex) { GRRLIB_FreeTexture(photoPrevTex); photoPrevTex = nullptr; }
    if (photoTex && photoTex->w > 0 && photoTex->h > 0) {
        draw(photoTex, k);
    } else {
        Ui::textCentered(320, 210, tr("This photo can't be shown"), 16, p.textDim);
    }
    char pos[96];
    /* TRANSLATORS: the photo's place in its folder */
    snprintf(pos, sizeof(pos), tr("%d of %d"), photoSel + 1, (int)photos.size());
    const std::string name = photos.empty() ? "" : filterDejaVu(photos[photoSel].name, 80);
    const Ui::Hint l[] = { { "LR", N_("Photos") }, { "A", slideshow ? N_("Stop") : N_("Slideshow") } };
    const Ui::Hint r[] = { { "", pos }, { "B", N_("Back") } };
    Ui::footer(l, 2, r, 2, name.c_str());
}
