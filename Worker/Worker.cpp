// Worker (WR): cuva ocitavanja svojih brojila.
//
//   Worker.exe <id> [-v]
//
// Niti:
//   glavna - prijava na LB (port 5060), konzola (q = odjava, s = stanje), gasenje
//   TPRIJ  - prima poruke od LB-a i stavlja ih u ulazni red
//   TOBR   - obradjuje poruke iz ulaznog reda redom i odgovara LB-u
//
// Na soket salju dve niti (TOBR odgovore, glavna LEAVE_BEGIN), pa slanje ide
// kroz kriticnu sekciju g_sendCs.

#include "NetLib.h"
#include "RingBuffer.h"
#include "Storage.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <conio.h>

#define LB_ADDRESS "127.0.0.1"

// Jedna primljena poruka u ulaznom redu.
typedef struct {
    MsgHeader hdr;
    char*     body;   // telo iz recvMsg; oslobadja se sa freeBody
} WrMsg;

static uint32_t         g_id;
static bool             g_verbose = false;
static SOCKET           g_sock = INVALID_SOCKET;
static HANDLE           g_stopEvent;        // postavljen = sve niti se gase
static HANDLE           g_doneEvent;        // postavljen kad je gasenje zavrseno (za Ctrl+C / zatvaranje prozora)
static CRITICAL_SECTION g_sendCs;           // stiti slanje na g_sock
static RingBuffer       g_inQueue;          // TPRIJ -> TOBR
static Storage          g_store;            // koristi ga samo TOBR

static volatile LONG    g_leaving = 0;      // 1 = poslat LEAVE_BEGIN, cekamo LEAVE_OK
static volatile LONG    g_leaveOk = 0;      // 1 = LB je dozvolio odjavu
static volatile LONG    g_meterCount = 0;   // za ispis stanja iz glavne niti
static volatile LONG    g_readingCount = 0;

static uint32_t         g_transferRecv = 0; // TOBR: ocitavanja primljena seobom od poslednjeg TRANSFER_END

// ---------------------------------------------------------------------------
// Pomocne funkcije
// ---------------------------------------------------------------------------

// Salje poruku LB-u pod kriticnom sekcijom. Ako slanje ne uspe, veza je prekinuta:
// postavi stopEvent i vrati false.
static bool sendToLb(uint16_t type, uint16_t flags, uint32_t requestId,
    const void* body, uint32_t length) {
    EnterCriticalSection(&g_sendCs);
    int r = sendMsg(g_sock, type, flags, requestId, body, length);
    LeaveCriticalSection(&g_sendCs);
    if (r != NET_OK) {
        if (WaitForSingleObject(g_stopEvent, 0) != WAIT_OBJECT_0)
            printf("[WR %u] Slanje %s nije uspelo, gasim se.\n", g_id, msgTypeName(type));
        SetEvent(g_stopEvent);
        return false;
    }
    return true;
}

// Osvezi brojace koje glavna nit ispisuje komandom s.
static void updateStats(void) {
    InterlockedExchange(&g_meterCount, (LONG)stMeterCount(&g_store));
    InterlockedExchange(&g_readingCount, (LONG)stReadingCount(&g_store));
}

static void freeWrMsg(WrMsg* m) {
    if (m == NULL) return;
    freeBody(m->body);
    free(m);
}

// ---------------------------------------------------------------------------
// Slanje podataka pri seobi: TRANSFER paketi po najvise TRANSFER_BATCH ocitavanja
// ---------------------------------------------------------------------------

typedef struct {
    uint32_t requestId;  // requestId RECLAIM-a, vraca se u TRANSFER i TRANSFER_END
    uint32_t count;      // ocitavanja u trenutnom paketu
    uint32_t total;      // ukupno poslato
    bool     ok;         // false = slanje nije uspelo
    char     buf[sizeof(TransferHeader) + TRANSFER_BATCH * sizeof(Reading)];
} TransferBatch;

// Posalje trenutni paket ako nije prazan.
static void batchFlush(TransferBatch* b) {
    if (!b->ok || b->count == 0) return;
    TransferHeader th = { b->count };
    memcpy(b->buf, &th, sizeof(th));
    uint32_t len = (uint32_t)(sizeof(TransferHeader) + b->count * sizeof(Reading));
    b->ok = sendToLb(MSG_TRANSFER, 0, b->requestId, b->buf, len);
    b->count = 0;
}

// Doda sva ocitavanja jednog brojila u pakete (puni paket odmah salje).
static bool batchAddMeter(uint32_t meterId, const DynArray* items, void* ctx) {
    TransferBatch* b = (TransferBatch*)ctx;
    for (size_t i = 0; i < daCount(items) && b->ok; i++) {
        const ReadingItem* it = (const ReadingItem*)daGet(items, i);
        Reading r;
        r.meterId = meterId;
        r.timestamp = it->timestamp;
        r.kwh = it->kwh;
        memcpy(b->buf + sizeof(TransferHeader) + b->count * sizeof(Reading), &r, sizeof(r));
        b->count++;
        b->total++;
        if (b->count == TRANSFER_BATCH) batchFlush(b);
    }
    return b->ok;
}

// ---------------------------------------------------------------------------
// Obrada pojedinih poruka (nit TOBR)
// ---------------------------------------------------------------------------

// WRITE: upise ocitavanje i potvrdi sa ACK (status 0 = OK).
static void handleWrite(const WrMsg* m) {
    const Reading* r = (const Reading*)m->body;
    AckBody ack;
    ack.status = stAdd(&g_store, r) ? 0 : 1;
    if (g_verbose)
        printf("[WR %u] WRITE brojilo=%u kWh=%.3f -> ACK %u\n", g_id, r->meterId, r->kwh, ack.status);
    sendToLb(MSG_ACK, 0, m->hdr.requestId, &ack, sizeof(ack));
}

// QUERY: vrati sva ocitavanja brojila (QUERY_RESP; count 0 ako brojila nema).
static void handleQuery(const WrMsg* m) {
    uint32_t meterId = ((const QueryBody*)m->body)->meterId;
    const DynArray* items = stGet(&g_store, meterId);
    uint32_t count = items != NULL ? (uint32_t)daCount(items) : 0;

    uint32_t len = (uint32_t)(sizeof(QueryRespHeader) + (size_t)count * sizeof(ReadingItem));
    char* body = (char*)malloc(len);
    if (body == NULL) {
        printf("[WR %u] Nema memorije za odgovor na upit (brojilo %u).\n", g_id, meterId);
        return;
    }
    QueryRespHeader qh = { meterId, count };
    memcpy(body, &qh, sizeof(qh));
    if (count > 0) memcpy(body + sizeof(qh), items->data, (size_t)count * sizeof(ReadingItem));

    if (g_verbose) printf("[WR %u] QUERY brojilo=%u -> %u ocitavanja\n", g_id, meterId, count);
    sendToLb(MSG_QUERY_RESP, 0, m->hdr.requestId, body, len);
    free(body);
}

// RECLAIM: posalje LB-u trazena brojila (ili sva, uz FLAG_ALL) u TRANSFER paketima,
// pa TRANSFER_END sa ukupnim brojem ocitavanja, i tek onda ih obrise kod sebe.
static void handleReclaim(const WrMsg* m) {
    TransferBatch* b = (TransferBatch*)malloc(sizeof(TransferBatch));
    if (b == NULL) {
        printf("[WR %u] Nema memorije za seobu, gasim se.\n", g_id);
        SetEvent(g_stopEvent);
        return;
    }
    b->requestId = m->hdr.requestId;
    b->count = 0;
    b->total = 0;
    b->ok = true;

    bool all = (m->hdr.flags & FLAG_ALL) != 0;
    const ReclaimHeader* rh = (const ReclaimHeader*)m->body;
    const uint32_t* ids = (const uint32_t*)(m->body + sizeof(ReclaimHeader));
    size_t meters = all ? stMeterCount(&g_store) : rh->count;

    if (all) {
        stForEach(&g_store, batchAddMeter, b);
    }
    else {
        for (uint32_t i = 0; i < rh->count && b->ok; i++) {
            const DynArray* items = stGet(&g_store, ids[i]);
            if (items != NULL) batchAddMeter(ids[i], items, b);
        }
    }
    batchFlush(b);

    if (b->ok) {
        TransferEndBody te = { b->total };
        if (sendToLb(MSG_TRANSFER_END, 0, b->requestId, &te, sizeof(te))) {
            // Podaci su predati: brojila vise nisu nasa.
            if (all) stClear(&g_store);
            else for (uint32_t i = 0; i < rh->count; i++) stRemove(&g_store, ids[i]);
            updateStats();
            printf("[WR %u] RECLAIM: predato %zu brojila, %u ocitavanja.\n", g_id, meters, b->total);
        }
    }
    free(b);
}

// TRANSFER od LB-a: ocitavanja brojila koja su sada nasa (seoba ili povratak).
static void handleTransfer(const WrMsg* m) {
    const TransferHeader* th = (const TransferHeader*)m->body;
    const Reading* r = (const Reading*)(m->body + sizeof(TransferHeader));
    for (uint32_t i = 0; i < th->count; i++) {
        if (!stAdd(&g_store, &r[i]))
            printf("[WR %u] Nema memorije za ocitavanje brojila %u iz seobe!\n", g_id, r[i].meterId);
    }
    g_transferRecv += th->count;
    updateStats();
}

// TRANSFER_END od LB-a: provera da je stiglo onoliko ocitavanja koliko je poslato.
static void handleTransferEnd(const WrMsg* m) {
    uint32_t total = ((const TransferEndBody*)m->body)->total;
    if (total == g_transferRecv)
        printf("[WR %u] Seoba primljena: %u ocitavanja (provera OK).\n", g_id, total);
    else
        printf("[WR %u] GRESKA U SEOBI: LB javlja %u ocitavanja, primljeno %u!\n", g_id, total, g_transferRecv);
    g_transferRecv = 0;
}

// ERROR od LB-a. ERR_LAST_WORKER je odgovor na odjavu: WR radi dalje.
static void handleError(const WrMsg* m) {
    uint32_t code = ((const ErrorBody*)m->body)->code;
    if (code == ERR_LAST_WORKER) {
        printf("[WR %u] LB je odbio odjavu: ovo je poslednji aktivni WR. Radim dalje.\n", g_id);
        InterlockedExchange(&g_leaving, 0);
    }
    else {
        printf("[WR %u] ERROR od LB-a, kod %u.\n", g_id, code);
    }
}

// ---------------------------------------------------------------------------
// Niti
// ---------------------------------------------------------------------------

// TPRIJ: prima poruke i stavlja ih u ulazni red. Prekid veze gasi WR.
static DWORD WINAPI receiveThread(LPVOID p) {
    (void)p;
    for (;;) {
        MsgHeader h;
        char* body;
        int r = recvMsg(g_sock, &h, &body);
        if (r != NET_OK) {
            if (WaitForSingleObject(g_stopEvent, 0) != WAIT_OBJECT_0) {
                printf(r == NET_CLOSED ? "[WR %u] LB je zatvorio vezu, gasim se.\n"
                                       : "[WR %u] Greska na vezi sa LB-om, gasim se.\n", g_id);
                SetEvent(g_stopEvent);
            }
            return 0;
        }

        WrMsg* m = (WrMsg*)malloc(sizeof(WrMsg));
        if (m == NULL) {
            freeBody(body);
            printf("[WR %u] Nema memorije za prijem, gasim se.\n", g_id);
            SetEvent(g_stopEvent);
            return 0;
        }
        m->hdr = h;
        m->body = body;
        if (!rbPush(&g_inQueue, m)) {
            freeWrMsg(m);
            printf("[WR %u] Ulazni red je pun (nema memorije), gasim se.\n", g_id);
            SetEvent(g_stopEvent);
            return 0;
        }
    }
}

// TOBR: obradjuje poruke redom kojim su stigle. Posle LEAVE_OK postavlja stopEvent.
static DWORD WINAPI processThread(LPVOID p) {
    (void)p;
    for (;;) {
        WrMsg* m = (WrMsg*)rbPop(&g_inQueue, g_stopEvent);
        if (m == NULL) return 0;   // gasenje

        switch (m->hdr.type) {
        case MSG_WRITE:        handleWrite(m); break;
        case MSG_QUERY:        handleQuery(m); break;
        case MSG_RECLAIM:      handleReclaim(m); break;
        case MSG_TRANSFER:     handleTransfer(m); break;
        case MSG_TRANSFER_END: handleTransferEnd(m); break;
        case MSG_ERROR:        handleError(m); break;
        case MSG_LEAVE_OK:
            printf("[WR %u] LEAVE_OK: LB je preuzeo podatke, gasim se.\n", g_id);
            InterlockedExchange(&g_leaveOk, 1);
            SetEvent(g_stopEvent);
            break;
        default:
            printf("[WR %u] Neocekivana poruka %s, preskacem.\n", g_id, msgTypeName(m->hdr.type));
            break;
        }
        freeWrMsg(m);
    }
}

// Ctrl+C, Ctrl+Break ili zatvaranje prozora: uredno gasenje bez odjave.
static BOOL WINAPI consoleHandler(DWORD type) {
    SetEvent(g_stopEvent);
    if (type == CTRL_CLOSE_EVENT) {
        // Windows ubija proces cim se handler vrati: sacekaj da main zavrsi gasenje.
        WaitForSingleObject(g_doneEvent, 4000);
    }
    return TRUE;
}

// ---------------------------------------------------------------------------
// Prijava, konzola i gasenje (glavna nit)
// ---------------------------------------------------------------------------

// Posalje REGISTER i saceka odgovor. Vraca true ako je LB odgovorio REGISTER_OK.
static bool registerWithLb(void) {
    RegisterBody rb = { g_id };
    if (sendMsg(g_sock, MSG_REGISTER, 0, 0, &rb, sizeof(rb)) != NET_OK) {
        printf("[WR %u] Slanje REGISTER nije uspelo.\n", g_id);
        return false;
    }

    MsgHeader h;
    char* body;
    if (recvMsg(g_sock, &h, &body) != NET_OK) {
        printf("[WR %u] LB nije odgovorio na prijavu.\n", g_id);
        return false;
    }
    bool ok = h.type == MSG_REGISTER_OK;
    if (!ok && h.type == MSG_ERROR)
        printf("[WR %u] LB je odbio prijavu (kod %u): id >= N ili je slot zauzet.\n",
            g_id, ((ErrorBody*)body)->code);
    else if (!ok)
        printf("[WR %u] Neocekivan odgovor na prijavu: %s.\n", g_id, msgTypeName(h.type));
    freeBody(body);
    return ok;
}

// Konzola dok ne stigne stopEvent: q = odjava, s = stanje.
static void consoleLoop(void) {
    printf("Komande: q = odjava (LEAVE_BEGIN), s = stanje. Ctrl+C = gasenje bez odjave.\n");
    while (WaitForSingleObject(g_stopEvent, 100) == WAIT_TIMEOUT) {
        if (!_kbhit()) continue;
        int c = _getch();
        if (c == 'q' || c == 'Q') {
            if (InterlockedCompareExchange(&g_leaving, 1, 0) != 0) {
                printf("[WR %u] Odjava je vec u toku, cekam LEAVE_OK...\n", g_id);
                continue;
            }
            printf("[WR %u] Saljem LEAVE_BEGIN, cekam da LB preuzme podatke...\n", g_id);
            sendToLb(MSG_LEAVE_BEGIN, 0, 0, NULL, 0);
        }
        else if (c == 's' || c == 'S') {
            printf("[WR %u] Brojila: %ld, ocitavanja: %ld%s\n", g_id,
                InterlockedCompareExchange(&g_meterCount, 0, 0),
                InterlockedCompareExchange(&g_readingCount, 0, 0),
                InterlockedCompareExchange(&g_leaving, 0, 0) ? " (odjava u toku)" : "");
        }
    }
}

int main(int argc, char* argv[]) {
    if (argc < 2) {
        printf("Upotreba: Worker.exe <id> [-v]\n");
        return 1;
    }
    char* end;
    unsigned long id = strtoul(argv[1], &end, 10);
    if (*end != '\0' || id >= MAX_WORKERS) {
        printf("Neispravan id \"%s\" (dozvoljeno 0 .. %d).\n", argv[1], MAX_WORKERS - 1);
        return 1;
    }
    g_id = (uint32_t)id;
    g_verbose = argc > 2 && strcmp(argv[2], "-v") == 0;

    // Sve sto se pravi, gasi se obrnutim redom na kraju main-a.
    g_stopEvent = CreateEvent(NULL, TRUE, FALSE, NULL);
    g_doneEvent = CreateEvent(NULL, TRUE, FALSE, NULL);
    InitializeCriticalSection(&g_sendCs);
    bool queueOk = rbInit(&g_inQueue, 0);
    bool storeOk = stInit(&g_store);
    bool netOk = netInit();
    int exitCode = 1;
    HANDLE threads[2] = { NULL, NULL };

    if (g_stopEvent == NULL || g_doneEvent == NULL || !queueOk || !storeOk || !netOk) {
        printf("[WR %u] Inicijalizacija nije uspela.\n", g_id);
        goto cleanup;
    }
    SetConsoleCtrlHandler(consoleHandler, TRUE);

    g_sock = netConnect(LB_ADDRESS, LB_WORKER_PORT);
    if (g_sock == INVALID_SOCKET) {
        printf("[WR %u] Ne mogu da se povezem na LB (%s:%d). Da li je LB pokrenut?\n",
            g_id, LB_ADDRESS, LB_WORKER_PORT);
        goto cleanup;
    }
    if (!registerWithLb()) goto cleanup;
    printf("[WR %u] Prijavljen na LB.\n", g_id);

    threads[0] = CreateThread(NULL, 0, receiveThread, NULL, 0, NULL);
    threads[1] = CreateThread(NULL, 0, processThread, NULL, 0, NULL);
    if (threads[0] == NULL || threads[1] == NULL) {
        printf("[WR %u] Pravljenje niti nije uspelo.\n", g_id);
        SetEvent(g_stopEvent);
    }
    else {
        consoleLoop();
        exitCode = 0;
    }

    // Gasenje: prvo TOBR (izlazi iz rbPop na stopEvent), pa zatvaranje soketa
    // koje odblokira recv u TPRIJ.
    SetEvent(g_stopEvent);
    if (threads[1] != NULL) WaitForSingleObject(threads[1], INFINITE);
    shutdown(g_sock, SD_BOTH);
    closesocket(g_sock);
    g_sock = INVALID_SOCKET;
    if (threads[0] != NULL) WaitForSingleObject(threads[0], INFINITE);

cleanup:
    for (int i = 0; i < 2; i++) if (threads[i] != NULL) CloseHandle(threads[i]);
    if (g_sock != INVALID_SOCKET) closesocket(g_sock);

    // Poruke koje TOBR nije stigao da obradi.
    if (queueOk) {
        void* item;
        while (rbTryPop(&g_inQueue, &item)) freeWrMsg((WrMsg*)item);
        rbFree(&g_inQueue);
    }
    if (storeOk) stFree(&g_store);
    if (netOk) netCleanup();
    DeleteCriticalSection(&g_sendCs);

    printf("[WR %u] Ugasen%s.\n", g_id, g_leaveOk ? " posle odjave" : "");
    if (g_doneEvent != NULL) {
        SetEvent(g_doneEvent);   // pusta consoleHandler ako ceka zatvaranje prozora
    }
    SetConsoleCtrlHandler(consoleHandler, FALSE);
    if (g_stopEvent != NULL) CloseHandle(g_stopEvent);
    if (g_doneEvent != NULL) CloseHandle(g_doneEvent);
    return exitCode;
}
