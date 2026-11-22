// Tabela radnika i niti TSL/TPR (Osoba 2).
//
// Svaki slot ima svoj red za slanje i dve niti koje se prave jednom, u workersStart:
//   TSL - jedina nit koja salje tom WR-u (skida poruke iz reda za slanje)
//   TPR - jedina nit koja prima od tog WR-a
// Obe cekaju dok se WR ne prijavi (TPR na attachedEvent, TSL na prvu poruku u redu,
// a poruka moze da udje u red tek kad je slot aktivan).
//
// Veza sa WR-om (WorkerConn) ima brojac referenci: soket se zatvara tek kad ga
// vise niko ne koristi, pa TSL nikad ne salje na zatvoren (ili ponovo iskoriscen) soket.

#include "Workers.h"
#include "MeterMap.h"
#include "RingBuffer.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    SOCKET        s;
    volatile LONG refs;   // slot + svaka nit koja trenutno koristi soket
} WorkerConn;

typedef struct {
    uint32_t         id;
    volatile LONG    state;          // SlotState
    WorkerConn*      conn;           // NULL kad je slot prazan; menja se pod cs
    volatile LONG    detaching;      // 1 dok traje obrada pada (slot jos ne prima novog WR-a)
    CRITICAL_SECTION cs;             // stiti conn, detaching i promene stanja
    RingBuffer       sendQueue;      // poruke za ovog WR-a (LbMsg*)
    HANDLE           attachedEvent;  // manual-reset: postavljen dok je WR prijavljen
    HANDLE           tsl;
    HANDLE           tpr;
} WorkerSlot;

static WorkerSlot* s_slots = NULL;
static uint32_t    s_slotCount = 0;

// ---------------------------------------------------------------------------
// Veza sa WR-om
// ---------------------------------------------------------------------------

// Uzme vezu slota (povecava brojac referenci), ili NULL ako WR nije prijavljen.
static WorkerConn* connAcquire(WorkerSlot* w) {
    EnterCriticalSection(&w->cs);
    WorkerConn* c = w->conn;
    if (c != NULL) InterlockedIncrement(&c->refs);
    LeaveCriticalSection(&w->cs);
    return c;
}

// Pusti vezu; poslednji zatvara soket.
static void connRelease(WorkerConn* c) {
    if (InterlockedDecrement(&c->refs) == 0) {
        closesocket(c->s);
        free(c);
    }
}

// Oslobodi sve poruke koje su ostale u redu za slanje.
static void drainSendQueue(WorkerSlot* w) {
    void* item;
    while (rbTryPop(&w->sendQueue, &item)) lbMsgFree((LbMsg*)item);
}

// Odvoji WR od slota: slot postaje prazan, veza se zatvara, red za slanje se prazni.
// Ako je WR pao (crashed), njegovi zahtevi dobiju ERROR, a brojila se brisu iz mape.
// Ako je slot vec odvojen od ove veze (npr. posle LEAVE_OK), ne radi nista.
static void workerDetach(WorkerSlot* w, WorkerConn* c, bool crashed) {
    EnterCriticalSection(&w->cs);
    if (w->conn != c) {
        LeaveCriticalSection(&w->cs);
        return;
    }
    w->conn = NULL;
    w->detaching = 1;
    InterlockedExchange(&w->state, SLOT_EMPTY);
    ResetEvent(w->attachedEvent);
    drainSendQueue(w);   // pod sekcijom: posle promene stanja nista novo ne ulazi u red
    LeaveCriticalSection(&w->cs);

    shutdown(c->s, SD_BOTH);   // odblokira TSL ako bas salje
    connRelease(c);            // referenca slota

    if (crashed) {
        printf("[LB] Veza sa WR %u je prekinuta bez odjave.\n", w->id);
        meterMapDropWorker(w->id);
        pendingFailWorker(w->id);
    }

    // Tek sad slot moze da primi novog WR-a sa istim id-jem (da pendingFailWorker
    // ne bi pogodio i zahteve upucene novom WR-u).
    EnterCriticalSection(&w->cs);
    w->detaching = 0;
    LeaveCriticalSection(&w->cs);
}

// Stavi u red WR-a poruku sa praznim zaglavljem i datim telom (telo se kopira).
static bool enqueueSimple(uint32_t workerId, uint16_t type, const void* body, uint32_t length) {
    char* b = (char*)malloc(length > 0 ? length : 1);
    if (b == NULL) return false;
    if (length > 0) memcpy(b, body, length);
    MsgHeader h = { type, 0, 0, length };
    LbMsg* m = lbMsgNew(&h, b);
    if (m == NULL) { freeBody(b); return false; }
    if (!workerEnqueue(workerId, m)) { lbMsgFree(m); return false; }
    return true;
}

// ---------------------------------------------------------------------------
// Niti
// ---------------------------------------------------------------------------

// TSL: salje poruke iz reda svom WR-u. Ako WR nije prijavljen, poruka se samo oslobodi
// (zahtev je vec dobio ili ce dobiti ERROR preko pendingFailWorker).
static DWORD WINAPI senderThread(LPVOID p) {
    WorkerSlot* w = (WorkerSlot*)p;
    for (;;) {
        LbMsg* m = (LbMsg*)rbPop(&w->sendQueue, g_stopEvent);
        if (m == NULL) return 0;   // gasenje

        WorkerConn* c = connAcquire(w);
        if (c != NULL) {
            if (sendMsg(c->s, m->hdr.type, m->hdr.flags, m->hdr.requestId,
                    m->body, m->hdr.length) != NET_OK) {
                shutdown(c->s, SD_BOTH);   // TPR ce primetiti prekid i odvojiti WR
            }
            connRelease(c);
        }
        lbMsgFree(m);
    }
}

// Obradi jednu poruku primljenu od WR-a. Preuzima telo.
static void dispatchFromWorker(WorkerSlot* w, const MsgHeader* h, char* body) {
    switch (h->type) {
    case MSG_ACK:
    case MSG_QUERY_RESP: {
        LbMsg* m = lbMsgNew(h, body);
        if (m == NULL) { freeBody(body); return; }
        pendingComplete(h->requestId, m);   // preuzima poruku
        return;
    }
    case MSG_LEAVE_BEGIN: {
        // TODO (seoba): predati niti TSEOBA. Dok seoba ne postoji, odjava se odbija
        // da WR ne bi ugasio sebe zajedno sa podacima.
        printf("[LB] WR %u trazi odjavu, ali seoba jos nije napravljena: odbijam.\n", w->id);
        ErrorBody e = { ERR_LAST_WORKER };
        enqueueSimple(w->id, MSG_ERROR, &e, sizeof(e));
        freeBody(body);
        return;
    }
    case MSG_TRANSFER:
    case MSG_TRANSFER_END:
        // TODO (seoba): predati niti TSEOBA.
        printf("[LB] WR %u: %s stigao, a seoba jos nije napravljena.\n", w->id, msgTypeName(h->type));
        freeBody(body);
        return;
    default:
        printf("[LB] WR %u: neocekivana poruka %s.\n", w->id, msgTypeName(h->type));
        freeBody(body);
        return;
    }
}

// TPR: ceka da se WR prijavi, pa prima njegove poruke. Prekid veze = pad WR-a
// (osim ako se LB gasi ili je WR vec uredno odvojen).
static DWORD WINAPI receiverThread(LPVOID p) {
    WorkerSlot* w = (WorkerSlot*)p;
    HANDLE wait[2] = { g_stopEvent, w->attachedEvent };
    for (;;) {
        if (WaitForMultipleObjects(2, wait, FALSE, INFINITE) != WAIT_OBJECT_0 + 1) return 0;
        WorkerConn* c = connAcquire(w);
        if (c == NULL) { Sleep(1); continue; }   // bas se odvaja; event ce biti ponisten

        for (;;) {
            MsgHeader h;
            char* body;
            if (recvMsg(c->s, &h, &body) != NET_OK) break;
            dispatchFromWorker(w, &h, body);
        }

        bool stopping = WaitForSingleObject(g_stopEvent, 0) == WAIT_OBJECT_0;
        workerDetach(w, c, !stopping);
        connRelease(c);
    }
}

// ---------------------------------------------------------------------------
// Funkcije iz Dogovor.h
// ---------------------------------------------------------------------------

SlotState workerState(uint32_t workerId) {
    if (workerId >= s_slotCount) return SLOT_EMPTY;
    return (SlotState)InterlockedCompareExchange(&s_slots[workerId].state, 0, 0);
}

bool workersStart(void) {
    if (g_workerCount == 0 || g_workerCount > MAX_WORKERS) {
        printf("[LB] N mora biti od 1 do %d.\n", MAX_WORKERS);
        return false;
    }
    s_slots = (WorkerSlot*)calloc(g_workerCount, sizeof(WorkerSlot));
    if (s_slots == NULL) return false;
    if (!meterMapInit()) { free(s_slots); s_slots = NULL; return false; }

    // Prvo svi slotovi, pa tek onda niti (niti citaju samo svoj slot).
    uint32_t ready = 0;
    for (; ready < g_workerCount; ready++) {
        WorkerSlot* w = &s_slots[ready];
        w->id = ready;
        w->state = SLOT_EMPTY;
        w->attachedEvent = CreateEvent(NULL, TRUE, FALSE, NULL);
        if (w->attachedEvent == NULL) break;
        if (!rbInit(&w->sendQueue, 0)) { CloseHandle(w->attachedEvent); break; }
        InitializeCriticalSection(&w->cs);
    }
    s_slotCount = ready;
    if (ready < g_workerCount) {
        printf("[LB] Pravljenje tabele radnika nije uspelo.\n");
        workersShutdown();
        return false;
    }

    for (uint32_t i = 0; i < g_workerCount; i++) {
        s_slots[i].tsl = CreateThread(NULL, 0, senderThread, &s_slots[i], 0, NULL);
        s_slots[i].tpr = CreateThread(NULL, 0, receiverThread, &s_slots[i], 0, NULL);
        if (s_slots[i].tsl == NULL || s_slots[i].tpr == NULL) {
            printf("[LB] Pravljenje TSL/TPR niti nije uspelo.\n");
            SetEvent(g_stopEvent);   // pusta niti koje su vec pokrenute
            workersShutdown();
            return false;
        }
    }
    printf("[LB] Tabela radnika: %u slotova, %u TSL + %u TPR niti.\n",
        g_workerCount, g_workerCount, g_workerCount);
    return true;
}

void workersShutdown(void) {
    if (s_slots == NULL) return;

    // Odblokiraj TPR niti koje cekaju u recv (TSL i ostale TPR izlaze na g_stopEvent).
    for (uint32_t i = 0; i < s_slotCount; i++) {
        WorkerConn* c = connAcquire(&s_slots[i]);
        if (c != NULL) { shutdown(c->s, SD_BOTH); connRelease(c); }
    }
    for (uint32_t i = 0; i < s_slotCount; i++) {
        if (s_slots[i].tsl != NULL) { WaitForSingleObject(s_slots[i].tsl, INFINITE); CloseHandle(s_slots[i].tsl); }
        if (s_slots[i].tpr != NULL) { WaitForSingleObject(s_slots[i].tpr, INFINITE); CloseHandle(s_slots[i].tpr); }
    }

    for (uint32_t i = 0; i < s_slotCount; i++) {
        WorkerSlot* w = &s_slots[i];
        if (w->conn != NULL) connRelease(w->conn);   // WR koji nije stigao da bude odvojen
        drainSendQueue(w);
        rbFree(&w->sendQueue);
        DeleteCriticalSection(&w->cs);
        CloseHandle(w->attachedEvent);
    }
    meterMapFree();
    free(s_slots);
    s_slots = NULL;
    s_slotCount = 0;
}

bool workerAttach(uint32_t workerId, SOCKET s) {
    if (workerId >= s_slotCount) return false;
    WorkerSlot* w = &s_slots[workerId];

    WorkerConn* c = (WorkerConn*)malloc(sizeof(WorkerConn));
    if (c == NULL) return false;
    c->s = s;
    c->refs = 1;   // referenca slota

    EnterCriticalSection(&w->cs);
    if (w->conn != NULL || w->detaching) {   // slot zauzet ili se upravo oslobadja
        LeaveCriticalSection(&w->cs);
        free(c);
        return false;
    }
    // REGISTER_OK se salje pre aktiviranja: dok slot nije aktivan, TSL ne moze nista
    // da posalje, pa je REGISTER_OK sigurno prva poruka koju WR dobije.
    if (sendMsg(s, MSG_REGISTER_OK, 0, 0, NULL, 0) != NET_OK) {
        LeaveCriticalSection(&w->cs);
        free(c);
        return false;
    }
    w->conn = c;
    InterlockedExchange(&w->state, SLOT_ACTIVE);
    SetEvent(w->attachedEvent);
    LeaveCriticalSection(&w->cs);

    // TODO (seoba): javiti TSEOBA da vrati brojila ciji je kucni WR ovaj.
    printf("[LB] WR %u prijavljen.\n", workerId);
    return true;
}

bool workerEnqueue(uint32_t workerId, LbMsg* msg) {
    if (workerId >= s_slotCount) return false;
    WorkerSlot* w = &s_slots[workerId];

    // Provera stanja i upis u red pod istom sekcijom: posle workerDetach (koji menja
    // stanje pod ovom sekcijom) nista vise ne moze da udje u red, pa ga detach isprazni do kraja.
    EnterCriticalSection(&w->cs);
    LONG st = InterlockedCompareExchange(&w->state, 0, 0);
    bool ok = (st == SLOT_ACTIVE || st == SLOT_LEAVING) && rbPush(&w->sendQueue, msg);
    LeaveCriticalSection(&w->cs);
    return ok;
}
