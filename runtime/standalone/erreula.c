/* Error-dialog lifecycle follows Cemu erreula.cpp::ErrEulaInstance. Dialog
 * selection is never fabricated; presentation/input are supplied by the UI. */
#include <stdio.h>
#include "rt.h"
static int created, state, home_visible;
static uint32_t region, language;
static uint64_t changed;
static int32_t result = -1;
#define RET(v) (c->r[3] = (uint32_t)(v))
static void create(ppc_ctx *c) {
    created = 1; state = 0; result = -1; region = c->r[4]; language = c->r[5];
}
static void destroy(ppc_ctx *c) { (void)c; created = 0; state = 0; result = -1; }
static void get_state(ppc_ctx *c) { RET(state); }
static void get_result(ppc_ctx *c) { RET(result); }
static void result_type(ppc_ctx *c) { RET(result == -1 ? 0 : result < 10 ? 1 : result >= 9999 ? 2 : result == 40 ? 4 : 3); }
static void selected(ppc_ctx *c) { RET(result != -1); }
static void left(ppc_ctx *c) { RET(created && result == 0); }
static void right(ppc_ctx *c) { RET(created && result == 1); }
static void appear(ppc_ctx *c) {
    if (!created) return;
    state = 1; result = -1; changed = ppc_timebase(c);
    fprintf(stderr, "ErrEula dialog: type=%u code=%u region=%u language=%u\n",
            ld32(c->r[3]), ld32(c->r[3] + 16), region, language);
}
static void disappear(ppc_ctx *c) { if (state == 2) { state = 3; changed = ppc_timebase(c); } }
static void calc(ppc_ctx *c) {
    if ((state == 1 || state == 3) && ppc_timebase(c) - changed > 62156250ull * 80 / 1000)
        state = state == 1 ? 2 : 0;
}
static void change_lang(ppc_ctx *c) { language = c->r[3]; }
static void home_appear(ppc_ctx *c) { (void)c; home_visible = 1; }
static void home_disappear(ppc_ctx *c) { (void)c; home_visible = 0; }
static void home_state(ppc_ctx *c) { RET(home_visible); }
hle_fn hle_erreula_lookup(const char *name) {
    static const struct { const char *prefix; hle_fn fn; } exports[] = {
        {"ErrEulaCreate__", create}, {"ErrEulaDestroy__", destroy},
        {"ErrEulaGetStateErrorViewer__", get_state}, {"ErrEulaGetResultCode__", get_result},
        {"ErrEulaGetResultType__", result_type}, {"ErrEulaIsDecideSelectButtonError__", selected},
        {"ErrEulaIsDecideSelectLeftButtonError__", left}, {"ErrEulaIsDecideSelectRightButtonError__", right},
        {"ErrEulaAppearError__", appear}, {"ErrEulaDisappearError__", disappear},
        {"ErrEulaCalc__", calc}, {"ErrEulaChangeLang__", change_lang},
        {"ErrEulaAppearHomeNixSign__", home_appear}, {"ErrEulaDisappearHomeNixSign__", home_disappear},
        {"ErrEulaIsAppearHomeNixSign__", home_state},
    };
    for (unsigned i = 0; i < sizeof exports / sizeof *exports; i++)
        if (!strncmp(name, exports[i].prefix, strlen(exports[i].prefix))) return exports[i].fn;
    return NULL;
}
