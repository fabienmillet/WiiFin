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
#include <stdio.h>

using namespace LibDraw;

static const u64 SLIDE_MS = 5000;   /* a slideshow's photo stays this long */

static u64 nowMs() { return ticks_to_millisecs(gettime()); }

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
    showPhoto(at);
    state = State::PhotoReady;
}

void LibraryView::showPhoto(int i)
{
    const int n = (int)photos.size();
    if (n == 0) return;
    photoSel = (i % n + n) % n;
    std::string bytes;
    runWithLoading([&]() {
        client.getPhotoBytes(serverUrl, auth, photos[photoSel].id, 640, 480, bytes);
    });
    if (photoTex) { GRRLIB_FreeTexture(photoTex); photoTex = nullptr; }
    if (!bytes.empty()) photoTex = loadJPEGTexture((const u8*)bytes.data(), (u32)bytes.size());
    photoShownAt = nowMs();
}

bool LibraryView::updatePhoto(ir_t& ir, bool aPressed)
{
    (void)ir;
    if (Input::isBackPressed()) {
        if (photoTex) { GRRLIB_FreeTexture(photoTex); photoTex = nullptr; }
        photos.clear();
        slideshow = false;
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
    if (photoTex && photoTex->w > 0 && photoTex->h > 0) {
        /* the whole photo, as large as the screen allows */
        const float W = Ui::screenWidth(), H = 450;
        float sc = W / photoTex->w;
        if (photoTex->h * sc > H) sc = H / photoTex->h;
        const float w = photoTex->w * sc, h = photoTex->h * sc;
        GRRLIB_DrawImg(320 - w / 2, H / 2 - h / 2, photoTex, 0, sc, sc, 0xFFFFFFFF);
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
