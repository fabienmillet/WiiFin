/* What every frame shows on top, whatever the screen: linked with
 * --wrap=GRRLIB_Render (Makefile), each GRRLIB_Render comes here first.
 * The server's messages (Remote) and the test build's frame counters
 * (tools/test/tour.sh sets g_wiifin_before_render) go there. */
#include <grrlib.h>
#include "../jellyfin/RemoteControl.h"
#include "../ui/Ui.h"
#include "Text.h"

extern "C" {
void (*g_wiifin_before_render)(void) = nullptr;
void __real_GRRLIB_Render(void);

void __wrap_GRRLIB_Render(void)
{
    Remote::pump();
    Ui::drawNotice();
    if (g_wiifin_before_render) g_wiifin_before_render();
    __real_GRRLIB_Render();
    Text::endFrame();
}
}
