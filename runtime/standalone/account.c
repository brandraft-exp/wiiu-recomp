/* Local account subset. Cemu nn_act.cpp slot/persistent-ID getters. */
#include "rt.h"
static void one(ppc_ctx *c){c->r[3]=1;}
static void zero(ppc_ctx *c){c->r[3]=0;}
static void occupied(ppc_ctx *c){c->r[3]=c->r[3]==1;}
static void persistent(ppc_ctx *c){c->r[3]=0x80000001;}
hle_fn hle_account_lookup(const char *name){
#define E(n,f) if(!strcmp(name,n))return f
    E("Initialize__Q2_2nn3actFv",zero);E("Finalize__Q2_2nn3actFv",zero);
    E("GetSlotNo__Q2_2nn3actFv",one);E("GetNumOfAccounts__Q2_2nn3actFv",one);
    E("IsSlotOccupied__Q2_2nn3actFUc",occupied);E("GetPersistentId__Q2_2nn3actFv",persistent);
    E("HasNfsAccount__Q2_2nn3actFv",zero);
#undef E
    return NULL;
}
