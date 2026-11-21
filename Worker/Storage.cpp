#include "Storage.h"
#include <stdlib.h>

// Oslobadja jedan niz ocitavanja (za obilazak pri stFree).
static bool freeArray(uint32_t key, void* value, void* ctx) {
    (void)key; (void)ctx;
    DynArray* a = (DynArray*)value;
    daFree(a);
    free(a);
    return true;
}

bool stInit(Storage* st) {
    st->readings = 0;
    return hmInit(&st->map, 0);
}

void stFree(Storage* st) {
    hmForEachRange(&st->map, 0, hmBucketCount(&st->map), freeArray, NULL);
    hmFree(&st->map);
    st->readings = 0;
}

bool stAdd(Storage* st, const Reading* r) {
    DynArray* a = (DynArray*)hmGet(&st->map, r->meterId);
    if (a == NULL) {
        a = (DynArray*)malloc(sizeof(DynArray));
        if (a == NULL) return false;
        if (!daInit(a, sizeof(ReadingItem), 0) || !hmPut(&st->map, r->meterId, a)) {
            daFree(a);
            free(a);
            return false;
        }
    }

    ReadingItem it;
    it.timestamp = r->timestamp;
    it.kwh = r->kwh;
    if (!daPush(a, &it)) return false;
    st->readings++;
    return true;
}

const DynArray* stGet(Storage* st, uint32_t meterId) {
    return (const DynArray*)hmGet(&st->map, meterId);
}

size_t stRemove(Storage* st, uint32_t meterId) {
    DynArray* a = (DynArray*)hmRemove(&st->map, meterId);
    if (a == NULL) return 0;
    size_t n = daCount(a);
    st->readings -= n;
    daFree(a);
    free(a);
    return n;
}

void stClear(Storage* st) {
    stFree(st);
    hmInit(&st->map, 0);
}

// Prosledjuje obilazak mape funkciji korisnika skladista.
typedef struct {
    StVisitFn fn;
    void*     ctx;
} ForEachCtx;

static bool forEachVisit(uint32_t key, void* value, void* ctx) {
    ForEachCtx* c = (ForEachCtx*)ctx;
    return c->fn(key, (const DynArray*)value, c->ctx);
}

void stForEach(Storage* st, StVisitFn fn, void* ctx) {
    ForEachCtx c = { fn, ctx };
    hmForEachRange(&st->map, 0, hmBucketCount(&st->map), forEachVisit, &c);
}

size_t stMeterCount(const Storage* st) {
    return hmCount(&st->map);
}

size_t stReadingCount(const Storage* st) {
    return st->readings;
}
