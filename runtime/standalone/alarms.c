/* Cemu coreinit_Alarm.cpp: OSCreateAlarm, __OSInitiateAlarm, alarm_update.
 * Callbacks run on a dedicated serialized guest service thread, never on a
 * host signal handler or while another guest thread executes. */
#include "rt.h"
#include <stdlib.h>
typedef struct alarm_record {
    uint32_t address,handler;
    uint64_t deadline,period;
    int active;
    struct alarm_record *next;
} alarm_record;
static alarm_record *alarms;
static int started;
static alarm_record *get(uint32_t a){
    for(alarm_record *p=alarms;p;p=p->next)if(p->address==a)return p;
    alarm_record *p=calloc(1,sizeof *p);if(!p)rt_fatal(NULL,"allocating alarm");
    p->address=a;p->next=alarms;alarms=p;return p;
}
static void update(ppc_ctx *c){
    uint64_t now=ppc_timebase(c);
    for(alarm_record *p=alarms;p;p=p->next){
        if(!p->active||p->deadline>now)continue;
        uint32_t handler=p->handler;
        if(p->period){p->deadline+=((now-p->deadline)/p->period+1)*p->period;st64(p->address+0x18,p->deadline);}
        else p->active=0;
        hle_thread_lookup("OSGetCurrentThread")(c);uint32_t context=c->r[3];
        c->r[3]=p->address;c->r[4]=context;ppc_call_indirect(c,handler);
    }
}
static void create(ppc_ctx *c){get(c->r[3])->active=0;memset(g_mem+c->r[3],0,0x58);st32(c->r[3],0x614c724d);}
static void create_ex(ppc_ctx *c){create(c);st32(c->r[3]+4,c->r[4]);}
static void cancel(ppc_ctx *c){alarm_record *p=get(c->r[3]);c->r[3]=p->active;p->active=0;}
static void userdata(ppc_ctx *c){st32(c->r[3]+0x38,c->r[4]);}
static void get_userdata(ppc_ctx *c){c->r[3]=ld32(c->r[3]+0x38);}
static void set(ppc_ctx *c,int periodic){
    uint32_t address=c->r[3],handler=c->r[periodic?9:7];
    uint64_t time=(uint64_t)c->r[5]<<32|c->r[6],period=periodic?((uint64_t)c->r[7]<<32|c->r[8]):0;
    if(!handler||(periodic&&!period))return;
    uint64_t now=ppc_timebase(c),deadline=time;
    if(!periodic)deadline=now+time;
    else if(deadline<=now)deadline+=((now-deadline)/period+1)*period;
    alarm_record *p=get(address);p->handler=handler;p->deadline=deadline;p->period=period;p->active=1;
    st32(address+12,handler);st64(address+0x18,deadline);st64(address+0x28,period);st64(address+0x30,periodic?time:0);
    if(!started){started=1;hle_service_start(c,update,1000000);}
}
static void single(ppc_ctx *c){set(c,0);}
static void periodic(ppc_ctx *c){set(c,1);}
hle_fn hle_alarm_lookup(const char *name){
#define E(n,f) if(!strcmp(name,n))return f
    E("OSCreateAlarm",create);E("OSCreateAlarmEx",create_ex);E("OSCancelAlarm",cancel);
    E("OSSetAlarmUserData",userdata);E("OSGetAlarmUserData",get_userdata);
    E("OSSetAlarm",single);E("OSSetPeriodicAlarm",periodic);
#undef E
    return NULL;
}
