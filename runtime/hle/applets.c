// System library applets Odyssey can start: on the Switch these calls launch an applet and wait for it to close;
// our am service has no applets, so they would wait forever. Answered here as if the player closed them at once.
#include <stdio.h>
#include <string.h>

#include "../hle.h"

// nn::web::ShowOfflineHtmlPage(OfflineHtmlPageReturnValue *out, const ShowOfflineHtmlPageArg &arg) -> nn::Result
// (an offline HTML page, e.g. a manual; StageScene polls rs::isExistRequestShowHtmlViewer). The return value starts
// with the exit reason: 1 = closed with the back button.
HLE_FUNC("_ZN2nn3web19ShowOfflineHtmlPageEPNS0_26OfflineHtmlPageReturnValueERKNS0_22ShowOfflineHtmlPageArgE", web_show_offline) {
    LOG("[applet] web: offline HTML page requested (lr %llx) - closed at once\n", (unsigned long long)c->x[30]);
    if (c->x[0]) *(uint64_t *)(uintptr_t)c->x[0] = 1;
    c->x[0] = 0;
}

// nn::err::ShowError(nn::Result): the error applet (a message box on the Switch). Logged; the game goes on.
HLE_FUNC("_ZN2nn3err9ShowErrorENS_6ResultE", err_show_error) {
    uint32_t r = (uint32_t)c->x[0];
    LOG("[applet] error applet: result %#x (module %u, description %u, lr %llx)\n", r, r & 0x1ff, (r >> 9) & 0x1fff,
        (unsigned long long)c->x[30]);
}
