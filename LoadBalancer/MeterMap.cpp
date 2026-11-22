#include "MeterMap.h"
#include "Workers.h"
#include "HashMap.h"
#include <stdio.h>
#include <stdlib.h>

// Jedno brojilo u mapi.
typedef struct {
    uint32_t   owner;  // WR koji sada cuva ocitavanja
    uint32_t   home;   // kucni WR = meterId % N
    MeterState state;
} MeterEntry;

static HashMap          s_map;                      // meterId -> MeterEntry*
static CRITICAL_SECTION s_cs;                       // stiti s_map i s_meterCount
static uint32_t         s_meterCount[MAX_WORKERS];  // broj brojila po WR-u (za izbor najmanje opterecenog)
static bool             s_ready = false;

// Merenje najduzeg drzanja kriticne sekcije (QueryPerformanceCounter).
static LARGE_INTEGER    s_freq;
static LARGE_INTEGER    s_lockedAt;
static LONGLONG         s_maxHoldTicks = 0;

// Zakljuca mapu i zapamti trenutak zakljucavanja.
static void mapLock(void) {
    EnterCriticalSection(&s_cs);
    QueryPerformanceCounter(&s_lockedAt);
}

// Izracuna koliko je mapa bila zakljucana, zapamti najduze, pa otkljuca.
static void mapUnlock(void) {
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    LONGLONG held = now.QuadPart - s_lockedAt.QuadPart;
    if (held > s_maxHoldTicks) s_maxHoldTicks = held;
    LeaveCriticalSection(&s_cs);
}

// Aktivni WR sa najmanje brojila (pri jednakom broju, manji id). -1 ako nema aktivnog.
// Poziva se pod kriticnom sekcijom mape.
static int leastLoadedWorker(void) {
    int best = -1;
    for (uint32_t i = 0; i < g_workerCount; i++) {
        if (workerState(i) != SLOT_ACTIVE) continue;
        if (best < 0 || s_meterCount[i] < s_meterCount[best]) best = (int)i;
    }
    return best;
}

// Vlasnik za brojilo bez vlasnika: kucni WR ako je aktivan, inace najmanje opterecen.
// Poziva se pod kriticnom sekcijom mape. -1 ako nema aktivnog WR-a.
static int pickOwner(uint32_t home) {
    if (workerState(home) == SLOT_ACTIVE) return (int)home;
    return leastLoadedWorker();
}

// Upise vlasnika brojila i azurira brojace. Poziva se pod kriticnom sekcijom mape.
static void setOwner(MeterEntry* e, uint32_t owner) {
    s_meterCount[e->owner]--;
    e->owner = owner;
    e->state = owner == e->home ? METER_HOME : METER_BORROWED;
    s_meterCount[owner]++;
}

bool meterMapInit(void) {
    if (!hmInit(&s_map, 1024)) return false;
    InitializeCriticalSection(&s_cs);
    QueryPerformanceFrequency(&s_freq);
    for (int i = 0; i < MAX_WORKERS; i++) s_meterCount[i] = 0;
    s_maxHoldTicks = 0;
    s_ready = true;
    return true;
}

// Oslobadja jedan zapis (za obilazak pri meterMapFree).
static bool freeEntry(uint32_t key, void* value, void* ctx) {
    (void)key; (void)ctx;
    free(value);
    return true;
}

void meterMapFree(void) {
    if (!s_ready) return;
    printf("[LB] Mapa brojila: %zu brojila, najduze drzanje sekcije %.3f ms\n",
        hmCount(&s_map), meterMapMaxHoldMs());
    for (uint32_t i = 0; i < g_workerCount; i++)
        if (s_meterCount[i] > 0) printf("[LB]   WR %u: %u brojila\n", i, s_meterCount[i]);

    hmForEachRange(&s_map, 0, hmBucketCount(&s_map), freeEntry, NULL);
    hmFree(&s_map);
    DeleteCriticalSection(&s_cs);
    s_ready = false;
}

RouteResult meterRoute(uint32_t meterId, LbMsg* msg) {
    uint32_t lbId = msg->hdr.requestId;
    RouteResult result = ROUTE_NO_WORKER;

    mapLock();
    MeterEntry* e = (MeterEntry*)hmGet(&s_map, meterId);

    // Novo brojilo: kucni WR, ili najmanje optereceni ako kucni nije tu.
    if (e == NULL) {
        int owner = pickOwner(meterId % g_workerCount);
        if (owner < 0) { mapUnlock(); return ROUTE_NO_WORKER; }

        e = (MeterEntry*)malloc(sizeof(MeterEntry));
        if (e == NULL || !hmPut(&s_map, meterId, e)) {
            free(e);
            mapUnlock();
            return ROUTE_NO_WORKER;   // nema memorije: korisnik dobije ERROR
        }
        e->home = meterId % g_workerCount;
        e->owner = (uint32_t)owner;
        e->state = e->owner == e->home ? METER_HOME : METER_BORROWED;
        s_meterCount[owner]++;
    }

    // TODO (seoba): METER_MOVING i METER_RETURNING -> poruka u parking, ROUTE_PARKED.

    // Najvise dva pokusaja: ako vlasnik bas tad nestane (pad WR-a), izabere se novi.
    for (int attempt = 0; attempt < 2; attempt++) {
        SlotState st = workerState(e->owner);
        if (st != SLOT_ACTIVE && st != SLOT_LEAVING) {
            int owner = pickOwner(e->home);
            if (owner < 0) break;
            setOwner(e, (uint32_t)owner);
        }

        // Upis u zapis na cekanju PRE stavljanja u red (vidi Dogovor.h).
        pendingSetWorker(lbId, e->owner);
        if (workerEnqueue(e->owner, msg)) {
            result = ROUTE_SENT;
            break;
        }
    }
    mapUnlock();
    return result;
}

// Obilazak pri padu WR-a: izbaci brojila ciji je vlasnik ctx.
static bool dropIfOwner(uint32_t key, void* value, void* ctx) {
    MeterEntry* e = (MeterEntry*)value;
    if (e->owner == *(uint32_t*)ctx) {
        hmRemove(&s_map, key);   // dozvoljeno za trenutni kljuc (HashMap.h)
        free(e);
    }
    return true;
}

void meterMapDropWorker(uint32_t workerId) {
    mapLock();
    size_t before = hmCount(&s_map);
    hmForEachRange(&s_map, 0, hmBucketCount(&s_map), dropIfOwner, &workerId);
    size_t dropped = before - hmCount(&s_map);
    s_meterCount[workerId] = 0;
    mapUnlock();
    printf("[LB] WR %u je pao: %zu njegovih brojila izbaceno iz mape (podaci izgubljeni).\n",
        workerId, dropped);
}

double meterMapMaxHoldMs(void) {
    if (s_freq.QuadPart == 0) return 0.0;
    return (double)s_maxHoldTicks * 1000.0 / (double)s_freq.QuadPart;
}
