#include "Pending.h"
#include "HashMap.h"
#include "DynArray.h"
#include <stdio.h>
#include <stdlib.h>

#define NO_WORKER 0xFFFFFFFFu   // zahtev jos nije poslat nijednom WR-u

// Jedan zahtev na cekanju.
typedef struct {
    Session* session;        // drzi jednu referencu
    uint32_t userRequestId;  // korisnikov originalni requestId
    uint32_t worker;         // kom WR-u je poslat, ili NO_WORKER
} PendingEntry;

static HashMap          s_map;   // lbId -> PendingEntry*
static CRITICAL_SECTION s_cs;
static bool             s_ready = false;

bool pendingInit(void) {
    if (!hmInit(&s_map, 1024)) return false;
    InitializeCriticalSection(&s_cs);
    s_ready = true;
    return true;
}

// Pusta sesiju i oslobadja zapis (za obilazak pri pendingFree).
static bool freeEntry(uint32_t key, void* value, void* ctx) {
    (void)key; (void)ctx;
    PendingEntry* e = (PendingEntry*)value;
    sessionRelease(e->session);
    free(e);
    return true;
}

void pendingFree(void) {
    if (!s_ready) return;
    hmForEachRange(&s_map, 0, hmBucketCount(&s_map), freeEntry, NULL);
    hmFree(&s_map);
    DeleteCriticalSection(&s_cs);
    s_ready = false;
}

bool pendingAdd(uint32_t lbId, Session* s, uint32_t userRequestId) {
    PendingEntry* e = (PendingEntry*)malloc(sizeof(PendingEntry));
    if (e == NULL) return false;
    e->session = s;
    e->userRequestId = userRequestId;
    e->worker = NO_WORKER;

    EnterCriticalSection(&s_cs);
    bool ok = hmPut(&s_map, lbId, e);
    LeaveCriticalSection(&s_cs);
    if (!ok) free(e);
    return ok;
}

size_t pendingCount(void) {
    EnterCriticalSection(&s_cs);
    size_t n = hmCount(&s_map);
    LeaveCriticalSection(&s_cs);
    return n;
}

// Izvadi zapis iz mape (ili NULL ako ga nema).
static PendingEntry* takeEntry(uint32_t lbId) {
    EnterCriticalSection(&s_cs);
    PendingEntry* e = (PendingEntry*)hmRemove(&s_map, lbId);
    LeaveCriticalSection(&s_cs);
    return e;
}

// Posalje korisniku ERROR za izvadjen zapis i oslobodi zapis.
static void replyError(PendingEntry* e, uint32_t code) {
    LbMsg* err = makeErrorMsg(e->userRequestId, code);
    if (err != NULL) replyPush(e->session, err);   // preuzima referencu sesije
    else sessionRelease(e->session);
    free(e);
}

void pendingComplete(uint32_t lbId, LbMsg* msg) {
    PendingEntry* e = takeEntry(lbId);
    if (e == NULL) {          // vec je dobio ERROR (npr. pad WR-a): odgovor se baca
        lbMsgFree(msg);
        return;
    }
    msg->hdr.requestId = e->userRequestId;   // korisnik dobija svoj requestId
    replyPush(e->session, msg);              // preuzima referencu sesije
    free(e);
}

void pendingFail(uint32_t lbId, uint32_t code) {
    PendingEntry* e = takeEntry(lbId);
    if (e != NULL) replyError(e, code);
}

void pendingSetWorker(uint32_t lbId, uint32_t workerId) {
    EnterCriticalSection(&s_cs);
    PendingEntry* e = (PendingEntry*)hmGet(&s_map, lbId);
    if (e != NULL) e->worker = workerId;
    LeaveCriticalSection(&s_cs);
}

// Kontekst za obilazak pri padu WR-a.
typedef struct {
    uint32_t worker;
    DynArray failed;   // PendingEntry* koji su izvadjeni iz mape
} FailCtx;

// Izbaci zapis iz mape ako je upucen palom WR-u (brisanje trenutnog kljuca je
// dozvoljeno u obilasku, vidi HashMap.h).
static bool collectFailed(uint32_t key, void* value, void* p) {
    FailCtx* c = (FailCtx*)p;
    PendingEntry* e = (PendingEntry*)value;
    if (e->worker == c->worker && daPush(&c->failed, &e))
        hmRemove(&s_map, key);
    return true;
}

void pendingFailWorker(uint32_t workerId) {
    FailCtx c;
    c.worker = workerId;
    if (!daInit(&c.failed, sizeof(PendingEntry*), 64)) return;

    // Pod sekcijom samo vadimo zapise; ERROR-e pravimo posle, bez drzanja mape.
    EnterCriticalSection(&s_cs);
    hmForEachRange(&s_map, 0, hmBucketCount(&s_map), collectFailed, &c);
    LeaveCriticalSection(&s_cs);

    for (size_t i = 0; i < daCount(&c.failed); i++)
        replyError(*(PendingEntry**)daGet(&c.failed, i), ERR_WORKER_FAILED);
    if (daCount(&c.failed) > 0)
        printf("[LB] WR %u je pao: %zu zahteva na cekanju dobija ERROR.\n",
            workerId, daCount(&c.failed));
    daFree(&c.failed);
}
