/* Software keyboard initialization follows Cemu swkbd.cpp::SwkbdCreate and
 * SwkbdCalcSubThread*. Its delay is required by MH3U's initialization workers.
 * Text/state/receiver behavior follows Cemu SwkbdAppearInputForm,
 * SwkbdAppearKeyboard, SwkbdSetReceiver and swkbd_inputStringChanged. */
#include "rt.h"
#include <stdio.h>
static unsigned font_jobs, predict_jobs;
static int created, active, decided, cancelled, allow_ok=1;
static uint32_t string_address, receiver[6];
static uint16_t text[4096];
static unsigned length,limit=4095;
/* devkitPro/wut include/nn/swkbd/swkbd_cpp.h: KeyboardCondition is two
 * words: selected language and character menu. Native input uses defaults. */
static void condition(ppc_ctx *c){st32(c->r[3],0);st32(c->r[3]+4,0);c->r[3]=0;}
static void no_op(ppc_ctx *c){c->r[3]=0;}
static void set_receiver(ppc_ctx *c){for(unsigned i=0;i<6;i++)receiver[i]=ld32(c->r[3]+4*i);c->r[3]=0;}
static void begin(uint32_t initial,unsigned max){
    if(!string_address)string_address=hle_sys_alloc(8192);
    limit=max<4096?max:4095;length=0;
    while(initial&&length<limit&&(text[length]=ld16(initial+2*length)))length++;
    text[length]=0;active=1;decided=cancelled=0;allow_ok=1;
    for(unsigned i=0;i<=length;i++)st16(string_address+2*i,text[i]);
    hle_text_begin(text,limit);
    fprintf(stderr,"SWKBD: text entry opened (limit %u); Enter confirms, Esc cancels\n",limit);
}
static void appear_form(ppc_ctx *c){uint32_t a=c->r[3];begin(ld32(a+0xc8),ld32(a+0xd0));c->r[3]=1;}
static void appear_keyboard(ppc_ctx *c){
    uint32_t a=c->r[3];for(unsigned i=0;i<6;i++)receiver[i]=ld32(a+0xa8+4*i);
    unsigned max=receiver[2]>0?receiver[2]-1:4095;
    if((int32_t)receiver[3]>0&&receiver[3]<max)max=receiver[3];
    begin(receiver[1],max);c->r[3]=1;
}
static void disappear(ppc_ctx *c){active=0;hle_text_end();c->r[3]=1;}
static void get_string(ppc_ctx *c){if(!string_address)string_address=hle_sys_alloc(8192);c->r[3]=string_address;}
static void ok(ppc_ctx *c){c->r[3]=decided;}
static void cancel(ppc_ctx *c){c->r[3]=cancelled;}
static void enable_ok(ppc_ctx *c){allow_ok=c->r[3]!=0;hle_text_allow_confirm(allow_ok);c->r[3]=0;}
static void is_target(ppc_ctx *c){c->r[3]=active&&c->r[3]==receiver[0];}

static void sleep_ticks(ppc_ctx *c, uint32_t ticks) {
    uint32_t saved[8]; memcpy(saved, &c->r[3], sizeof saved);
    c->r[3] = 0; c->r[4] = ticks;
    hle_thread_lookup("OSSleepTicks")(c);
    memcpy(&c->r[3], saved, sizeof saved);
}
static void create(ppc_ctx *c) {
    created = 1;
    sleep_ticks(c, 62156250);
    font_jobs = predict_jobs = 3;
}
static void destroy(ppc_ctx *c) { (void)c; created = active = 0; font_jobs = predict_jobs = 0; hle_text_end(); }
static void state(ppc_ctx *c) { c->r[3] = active?2:0; }
static void need_font(ppc_ctx *c) { c->r[3] = created && font_jobs; }
static void need_predict(ppc_ctx *c) { c->r[3] = created && predict_jobs; }
static void font(ppc_ctx *c) { if (font_jobs) { --font_jobs; sleep_ticks(c, 62156250 / 200); } }
static void predict(ppc_ctx *c) { if (predict_jobs) { --predict_jobs; sleep_ticks(c, 62156250 / 200); } }
static void calc(ppc_ctx *c) {
    if(!active){c->r[3]=0;return;}
    uint16_t updated[4096];unsigned n;int result=hle_text_poll(updated,&n);
    int changed=n!=length||memcmp(updated,text,(n+1)*2);
    if(changed){
        length=n;memcpy(text,updated,(n+1)*2);
        for(unsigned i=0;i<=n;i++)st16(string_address+2*i,text[i]);
        if(receiver[1]&&receiver[2]){
            unsigned count=n<receiver[2]-1?n:receiver[2]-1;
            for(unsigned i=0;i<count;i++)st16(receiver[1]+2*i,text[i]);
            st16(receiver[1]+2*count,0);
        }
        if(receiver[0]){
            uint32_t table=ld32(receiver[0]),fn=table?ld32(table+12):0;
            if(fn){
                ppc_ctx saved=*c;c->r[1]-=32;st32(c->r[1],saved.r[1]);
                st32(c->r[1]+8,0);st32(c->r[1]+12,0);
                c->r[3]=receiver[0];c->r[4]=c->r[1]+8;ppc_call_indirect(c,fn);*c=saved;
            }
        }
    }
    if(result==1&&allow_ok)decided=1;
    if(result==2)cancelled=1;
    c->r[3]=0;
}
/* Cemu swkbd.cpp::SwkbdGetDrawStringInfo: no composing/selected substring. */
static void draw_string_info(ppc_ctx *c) {
    uint32_t a=c->r[3];
    for(unsigned i=0;i<6;i++)st32(a+i*4,UINT32_MAX);
    st8(a+24,0);c->r[3]=0;
}
/* Cemu SwkbdInitLearnDic: persistent dictionary header and empty indices. */
static void init_dictionary(ppc_ctx *c) {
    uint32_t a = c->r[3];
    if (!created || !a) { c->r[3] = 0; return; }
    memset(g_mem + a, 0, 0xa460);
    const uint32_t header[18] = {0x4e4a4443, 0x30000, 0x80020000, 0xa418,
        0x2c, 0x6e, 0x6e, 0xfec, 0, 0, 1000, 0x20, 0, 0, 0, 0x48, 0x81a, 0x8cec};
    for (unsigned i = 0; i < 18; i++) st32(a + i * 4, header[i]);
    st32(a + 0xa45c, 0x4e4a4443); c->r[3] = 1;
}
hle_fn hle_swkbd_lookup(const char *name) {
#define PREFIX(n, f) if (!strncmp(name, n "__", sizeof(n "__") - 1)) return f
    PREFIX("SwkbdAppearInputForm", appear_form);PREFIX("SwkbdAppearKeyboard", appear_keyboard);
    PREFIX("SwkbdDisappearInputForm", disappear);PREFIX("SwkbdDisappearKeyboard", disappear);
    PREFIX("SwkbdGetKeyboardCondition", condition);
    PREFIX("SwkbdGetInputFormString", get_string);PREFIX("SwkbdSetReceiver", set_receiver);
    PREFIX("SwkbdIsDecideOkButton", ok);PREFIX("SwkbdIsDecideCancelButton", cancel);
    PREFIX("SwkbdSetEnableOkButton", enable_ok);PREFIX("SwkbdIsKeyboardTarget", is_target);
    PREFIX("SwkbdDrawTV", no_op);PREFIX("SwkbdDrawDRC", no_op);
    PREFIX("SwkbdConfirmUnfixAll", no_op);PREFIX("SwkbdSetCursorPos", no_op);
    PREFIX("SwkbdSetSelectFrom", no_op);PREFIX("SwkbdSetUserControllerEventObj", no_op);
    PREFIX("SwkbdSetUserSoundObj", no_op);PREFIX("SwkbdSetControllerRemo", no_op);
    PREFIX("SwkbdIsCoveredWithSubWindow", no_op);PREFIX("SwkbdIsSelectCursorActive", no_op);
    PREFIX("SwkbdInactivateSelectCursor", no_op);
    PREFIX("SwkbdInitLearnDic", init_dictionary);
    PREFIX("SwkbdGetDrawStringInfo", draw_string_info);
    PREFIX("SwkbdCreate", create); PREFIX("SwkbdDestroy", destroy);
    PREFIX("SwkbdGetStateKeyboard", state); PREFIX("SwkbdGetStateInputForm", state);
    PREFIX("SwkbdIsNeedCalcSubThreadFont", need_font); PREFIX("SwkbdIsNeedCalcSubThreadPredict", need_predict);
    PREFIX("SwkbdCalcSubThreadFont", font); PREFIX("SwkbdCalcSubThreadPredict", predict); PREFIX("SwkbdCalc", calc);
    return NULL;
#undef PREFIX
}
