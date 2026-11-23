// Prednji deo LB-a (Osoba 1): TPRIH, TCIT, red Q, TRAS, red odgovora i TODG.
//
// Tok zahteva:   korisnik -> TCIT -> (mapa na cekanju) -> Q -> TRAS -> meterRoute -> red WR-a
// Tok odgovora:  TPR -> pendingComplete -> red odgovora -> TODG -> korisnik

// select na Windows-u po difoltu prima samo 64 soketa; mora pre winsock2.h.
#define FD_SETSIZE 1024

#include "Users.h"
#include "Pending.h"
#include "RingBuffer.h"
#include "DynArray.h"
#include <stdio.h>
#include <stdlib.h>

#define MAX_USERS        (FD_SETSIZE - 24)  // najvise istovremenih korisnika
#define SELECT_TIMEOUT_MS 50                // koliko cesto niti proveravaju g_stopEvent
#define REGISTER_TIMEOUT_MS 3000            // koliko TPRIH ceka REGISTER od novog WR-a

// Jedan odgovor u redu odgovora.
typedef struct {
    Session* session;   // drzi jednu referencu
    LbMsg*   msg;       // requestId je vec korisnikov
} Reply;

static SOCKET     s_userListen = INVALID_SOCKET;
static SOCKET     s_workerListen = INVALID_SOCKET;
static RingBuffer s_newSessions;   // TPRIH -> TCIT (Session*)
static RingBuffer s_q;             // TCIT -> TRAS (LbMsg*), centralni red Q
static RingBuffer s_replies;       // pendingComplete -> TODG (Reply*)
static HANDLE     s_tprih = NULL, s_tcit = NULL, s_tras = NULL, s_todg = NULL;
static bool       s_queuesReady = false;

// Brojaci za ispis (menjaju se sa Interlocked*).
static volatile LONG     s_activeUsers = 0;
static volatile LONG     s_totalUsers = 0;
static volatile LONG     s_requests = 0;
static volatile LONG     s_replyCount = 0;
static volatile LONG     s_errorCount = 0;
static volatile LONGLONG s_bytesIn = 0;    // od korisnika
static volatile LONGLONG s_bytesOut = 0;   // ka korisnicima

// ---------------------------------------------------------------------------
// Sesija korisnika
// ---------------------------------------------------------------------------

// Pravi sesiju sa jednom referencom (ona pripada TCIT-u).
static Session* sessionNew(SOCKET s, uint32_t id) {
    Session* ses = (Session*)malloc(sizeof(Session));
    if (ses == NULL) return NULL;
    ses->s = s;
    ses->id = id;
    ses->refs = 1;
    ses->closed = 0;
    InterlockedIncrement(&s_activeUsers);
    return ses;
}

void sessionAddRef(Session* s) {
    InterlockedIncrement(&s->refs);
}

void sessionRelease(Session* s) {
    if (InterlockedDecrement(&s->refs) == 0) {
        closesocket(s->s);
        free(s);
        InterlockedDecrement(&s_activeUsers);
    }
}

// Oznaci da je korisnik otisao i prekini vezu. TCIT ce to primetiti i pustiti svoju
// referencu; soket se zatvara kad i poslednji odgovor bude pusten.
static void sessionClose(Session* s) {
    if (InterlockedExchange(&s->closed, 1) == 0) shutdown(s->s, SD_BOTH);
}

// ---------------------------------------------------------------------------
// Odgovori
// ---------------------------------------------------------------------------

LbMsg* makeErrorMsg(uint32_t requestId, uint32_t code) {
    ErrorBody* b = (ErrorBody*)malloc(sizeof(ErrorBody));
    if (b == NULL) return NULL;
    b->code = code;
    MsgHeader h = { MSG_ERROR, 0, requestId, sizeof(ErrorBody) };
    LbMsg* m = lbMsgNew(&h, (char*)b);
    if (m == NULL) free(b);
    return m;
}

void replyPush(Session* s, LbMsg* msg) {
    Reply* r = (Reply*)malloc(sizeof(Reply));
    if (r != NULL) {
        r->session = s;
        r->msg = msg;
        if (rbPush(&s_replies, r)) return;
        free(r);
    }
    lbMsgFree(msg);   // nema memorije: odgovor se gubi, ali ne curi
    sessionRelease(s);
}

// Posalje ERROR korisniku za zahtev koji nije ni usao u mapu na cekanju.
static void replyErrorDirect(Session* s, uint32_t userRequestId, uint32_t code) {
    LbMsg* err = makeErrorMsg(userRequestId, code);
    sessionAddRef(s);
    if (err != NULL) replyPush(s, err);
    else sessionRelease(s);
}

// Oslobodi jedan odgovor iz reda (posle slanja ili pri gasenju).
static void replyFree(Reply* r) {
    sessionRelease(r->session);
    lbMsgFree(r->msg);
    free(r);
}

// ---------------------------------------------------------------------------
// TPRIH: prihvat veza na 5059 i 5060
// ---------------------------------------------------------------------------

// Novi korisnik: napravi sesiju i predaj je TCIT-u.
static void acceptUser(void) {
    SOCKET s = accept(s_userListen, NULL, NULL);
    if (s == INVALID_SOCKET) return;
    netSetNoDelay(s);
    if (InterlockedCompareExchange(&s_activeUsers, 0, 0) >= MAX_USERS) {
        printf("[LB] Previse korisnika (%d), nova veza se odbija.\n", MAX_USERS);
        closesocket(s);
        return;
    }
    uint32_t id = (uint32_t)InterlockedIncrement(&s_totalUsers);
    Session* ses = sessionNew(s, id);
    if (ses == NULL) { closesocket(s); return; }
    if (!rbPush(&s_newSessions, ses)) { sessionRelease(ses); return; }
    printf("[LB] Korisnik %u povezan.\n", id);
}

// Postavi koliko recv sme da ceka na soketu (0 = beskonacno).
static void setRecvTimeout(SOCKET s, DWORD ms) {
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char*)&ms, sizeof(ms));
}

// Novi WR: primi REGISTER i predaj soket delu Osobe 2 (workerAttach).
static void acceptWorker(void) {
    SOCKET s = accept(s_workerListen, NULL, NULL);
    if (s == INVALID_SOCKET) return;
    netSetNoDelay(s);

    // REGISTER mora da stigne brzo; inace bi jedan los klijent blokirao TPRIH.
    setRecvTimeout(s, REGISTER_TIMEOUT_MS);
    MsgHeader h;
    char* body;
    if (recvMsg(s, &h, &body) != NET_OK) {
        printf("[LB] Nova veza na %d nije poslala REGISTER, zatvaram.\n", LB_WORKER_PORT);
        closesocket(s);
        return;
    }
    setRecvTimeout(s, 0);

    if (h.type != MSG_REGISTER) {
        printf("[LB] Ocekivan REGISTER, stigao %s. Zatvaram vezu.\n", msgTypeName(h.type));
        freeBody(body);
        closesocket(s);
        return;
    }
    uint32_t id = ((RegisterBody*)body)->workerId;
    freeBody(body);

    if (!workerAttach(id, s)) {   // workerAttach sam salje REGISTER_OK
        printf("[LB] WR %u odbijen (id >= N ili je slot zauzet).\n", id);
        ErrorBody e = { ERR_BAD_REQUEST };
        sendMsg(s, MSG_ERROR, 0, h.requestId, &e, sizeof(e));
        closesocket(s);
    }
}

static DWORD WINAPI acceptThread(LPVOID p) {
    (void)p;
    while (WaitForSingleObject(g_stopEvent, 0) != WAIT_OBJECT_0) {
        fd_set rd;
        FD_ZERO(&rd);
        FD_SET(s_userListen, &rd);
        FD_SET(s_workerListen, &rd);
        SOCKET maxS = s_userListen > s_workerListen ? s_userListen : s_workerListen;
        timeval tv = { 0, SELECT_TIMEOUT_MS * 1000 };

        int r = select((int)maxS + 1, &rd, NULL, NULL, &tv);
        if (r == SOCKET_ERROR) { Sleep(SELECT_TIMEOUT_MS); continue; }
        if (r == 0) continue;
        if (FD_ISSET(s_userListen, &rd)) acceptUser();
        if (FD_ISSET(s_workerListen, &rd)) acceptWorker();
    }
    return 0;
}

// ---------------------------------------------------------------------------
// TCIT: citanje zahteva od svih korisnika
// ---------------------------------------------------------------------------

// Sledeci lbId (koristi ga samo TCIT, pa je obican brojac dovoljan). Nikad 0.
static uint32_t nextLbId(void) {
    static uint32_t counter = 0;
    if (++counter == 0) counter = 1;
    return counter;
}

// Procita jedan zahtev od korisnika i stavi ga u Q.
// Vraca false ako je veza prekinuta (korisnik je otisao).
static bool readRequest(Session* ses) {
    MsgHeader h;
    char* body;
    if (recvMsg(ses->s, &h, &body) != NET_OK) return false;
    InterlockedExchangeAdd64(&s_bytesIn, (LONGLONG)sizeof(MsgHeader) + h.length);

    if (h.type != MSG_WRITE && h.type != MSG_QUERY) {
        replyErrorDirect(ses, h.requestId, ERR_BAD_REQUEST);
        freeBody(body);
        return true;
    }

    LbMsg* m = lbMsgNew(&h, body);
    if (m == NULL) {
        freeBody(body);
        replyErrorDirect(ses, h.requestId, ERR_BAD_REQUEST);
        return true;
    }

    // Novi jedinstveni ID; korisnikov se cuva u mapi na cekanju.
    uint32_t lbId = nextLbId();
    m->hdr.requestId = lbId;
    sessionAddRef(ses);   // referenca za zapis na cekanju
    if (!pendingAdd(lbId, ses, h.requestId)) {
        sessionRelease(ses);
        lbMsgFree(m);
        replyErrorDirect(ses, h.requestId, ERR_BAD_REQUEST);
        return true;
    }
    InterlockedIncrement(&s_requests);

    // Zapis je u mapi pre nego sto zahtev udje u Q, pa ga odgovor uvek nadje.
    if (!rbPush(&s_q, m)) {
        lbMsgFree(m);
        pendingFail(lbId, ERR_BAD_REQUEST);
    }
    return true;
}

static DWORD WINAPI readThread(LPVOID p) {
    (void)p;
    DynArray sessions;   // Session* koje TCIT trenutno prati
    if (!daInit(&sessions, sizeof(Session*), 16)) return 1;

    while (WaitForSingleObject(g_stopEvent, 0) != WAIT_OBJECT_0) {
        // Preuzmi nove korisnike od TPRIH.
        void* item;
        while (rbTryPop(&s_newSessions, &item)) {
            Session* ns = (Session*)item;
            if (!daPush(&sessions, &ns)) { sessionClose(ns); sessionRelease(ns); }
        }

        size_t n = daCount(&sessions);
        if (n == 0) {   // select bez ijednog soketa na Windows-u vraca gresku
            WaitForSingleObject(g_stopEvent, SELECT_TIMEOUT_MS);
            continue;
        }

        fd_set rd;
        FD_ZERO(&rd);
        SOCKET maxS = 0;
        for (size_t i = 0; i < n; i++) {
            Session* ses = *(Session**)daGet(&sessions, i);
            FD_SET(ses->s, &rd);
            if (ses->s > maxS) maxS = ses->s;
        }
        timeval tv = { 0, SELECT_TIMEOUT_MS * 1000 };
        int r = select((int)maxS + 1, &rd, NULL, NULL, &tv);
        if (r == SOCKET_ERROR) { Sleep(SELECT_TIMEOUT_MS); continue; }
        if (r == 0) continue;

        // Od kraja ka pocetku, da brisanje iz niza ne preskoci nijednu sesiju.
        for (size_t i = n; i-- > 0;) {
            Session* ses = *(Session**)daGet(&sessions, i);
            if (!FD_ISSET(ses->s, &rd)) continue;
            if (!readRequest(ses)) {
                printf("[LB] Korisnik %u se odjavio.\n", ses->id);
                sessionClose(ses);
                daRemoveAt(&sessions, i);
                sessionRelease(ses);   // referenca TCIT-a
            }
        }
    }

    // Gasenje: pusti sve sesije koje je TCIT drzao.
    for (size_t i = 0; i < daCount(&sessions); i++) {
        Session* ses = *(Session**)daGet(&sessions, i);
        sessionClose(ses);
        sessionRelease(ses);
    }
    daFree(&sessions);
    return 0;
}

// ---------------------------------------------------------------------------
// TRAS: raspodela zahteva po mapi brojila
// ---------------------------------------------------------------------------

static DWORD WINAPI routeThread(LPVOID p) {
    (void)p;
    for (;;) {
        LbMsg* m = (LbMsg*)rbPop(&s_q, g_stopEvent);
        if (m == NULL) return 0;   // gasenje

        // Telo je vec provereno u recvMsg: WRITE nosi Reading, QUERY nosi QueryBody.
        uint32_t meterId = m->hdr.type == MSG_WRITE
            ? ((Reading*)m->body)->meterId
            : ((QueryBody*)m->body)->meterId;
        uint32_t lbId = m->hdr.requestId;   // zapamti pre predaje: posle poruka nije nasa

        if (meterRoute(meterId, m) == ROUTE_NO_WORKER) {
            lbMsgFree(m);
            pendingFail(lbId, ERR_NO_WORKERS);
        }
    }
}

// ---------------------------------------------------------------------------
// TODG: slanje odgovora korisnicima
// ---------------------------------------------------------------------------

static DWORD WINAPI replyThread(LPVOID p) {
    (void)p;
    for (;;) {
        Reply* r = (Reply*)rbPop(&s_replies, g_stopEvent);
        if (r == NULL) return 0;   // gasenje; ostatak reda oslobadja usersShutdown

        Session* ses = r->session;
        LbMsg* m = r->msg;
        if (InterlockedCompareExchange(&ses->closed, 0, 0) == 0) {
            if (sendMsg(ses->s, m->hdr.type, m->hdr.flags, m->hdr.requestId,
                    m->body, m->hdr.length) == NET_OK) {
                InterlockedExchangeAdd64(&s_bytesOut, (LONGLONG)sizeof(MsgHeader) + m->hdr.length);
                InterlockedIncrement(&s_replyCount);
                if (m->hdr.type == MSG_ERROR) InterlockedIncrement(&s_errorCount);
            }
            else {
                sessionClose(ses);   // TCIT ce primetiti prekid i ukloniti korisnika
            }
        }
        replyFree(r);   // korisnik je otisao: odgovor se samo oslobodi
    }
}

// ---------------------------------------------------------------------------
// Pokretanje i gasenje
// ---------------------------------------------------------------------------

// Oslobodi sve sto je ostalo u redovima i zatvori listen sokete.
static void freeAll(void) {
    void* item;
    if (s_queuesReady) {
        while (rbTryPop(&s_newSessions, &item)) { sessionClose((Session*)item); sessionRelease((Session*)item); }
        while (rbTryPop(&s_q, &item)) lbMsgFree((LbMsg*)item);
        while (rbTryPop(&s_replies, &item)) replyFree((Reply*)item);
        rbFree(&s_newSessions);
        rbFree(&s_q);
        rbFree(&s_replies);
        s_queuesReady = false;
    }
    pendingFree();   // pusta sesije koje su drzali zahtevi bez odgovora
    if (s_userListen != INVALID_SOCKET) { closesocket(s_userListen); s_userListen = INVALID_SOCKET; }
    if (s_workerListen != INVALID_SOCKET) { closesocket(s_workerListen); s_workerListen = INVALID_SOCKET; }
}

// Saceka nit i zatvori njen handle.
static void joinThread(HANDLE* t) {
    if (*t == NULL) return;
    WaitForSingleObject(*t, INFINITE);
    CloseHandle(*t);
    *t = NULL;
}

bool usersStart(void) {
    bool okNew = rbInit(&s_newSessions, 0);
    bool okQ = rbInit(&s_q, 1024);
    bool okR = rbInit(&s_replies, 1024);
    bool okP = pendingInit();
    if (!okNew || !okQ || !okR || !okP) {
        printf("[LB] Pravljenje redova nije uspelo.\n");
        if (okNew) rbFree(&s_newSessions);
        if (okQ) rbFree(&s_q);
        if (okR) rbFree(&s_replies);
        if (okP) pendingFree();
        return false;
    }
    s_queuesReady = true;

    s_userListen = netListen(LB_USER_PORT);
    s_workerListen = netListen(LB_WORKER_PORT);
    if (s_userListen == INVALID_SOCKET || s_workerListen == INVALID_SOCKET) {
        printf("[LB] Ne mogu da otvorim port %d ili %d (da li je LB vec pokrenut?).\n",
            LB_USER_PORT, LB_WORKER_PORT);
        freeAll();
        return false;
    }

    s_todg = CreateThread(NULL, 0, replyThread, NULL, 0, NULL);
    s_tras = CreateThread(NULL, 0, routeThread, NULL, 0, NULL);
    s_tcit = CreateThread(NULL, 0, readThread, NULL, 0, NULL);
    s_tprih = CreateThread(NULL, 0, acceptThread, NULL, 0, NULL);
    if (s_todg == NULL || s_tras == NULL || s_tcit == NULL || s_tprih == NULL) {
        printf("[LB] Pravljenje niti nije uspelo.\n");
        SetEvent(g_stopEvent);
        usersStopInput();
        joinThread(&s_todg);
        freeAll();
        return false;
    }
    printf("[LB] Korisnici na portu %d, radnici na portu %d. Niti: TPRIH, TCIT, TRAS, TODG.\n",
        LB_USER_PORT, LB_WORKER_PORT);
    return true;
}

void usersStopInput(void) {
    joinThread(&s_tprih);
    joinThread(&s_tcit);
    joinThread(&s_tras);
}

void usersShutdown(void) {
    joinThread(&s_todg);
    freeAll();
}

void usersPrintStats(void) {
    printf("[LB] Korisnika: %ld aktivno, %ld ukupno | zahteva: %ld, odgovora: %ld (ERROR: %ld), na cekanju: %zu\n",
        InterlockedCompareExchange(&s_activeUsers, 0, 0),
        InterlockedCompareExchange(&s_totalUsers, 0, 0),
        InterlockedCompareExchange(&s_requests, 0, 0),
        InterlockedCompareExchange(&s_replyCount, 0, 0),
        InterlockedCompareExchange(&s_errorCount, 0, 0),
        s_queuesReady ? pendingCount() : (size_t)0);
    printf("[LB] Bajtova od korisnika: %lld, ka korisnicima: %lld\n",
        (long long)InterlockedCompareExchange64(&s_bytesIn, 0, 0),
        (long long)InterlockedCompareExchange64(&s_bytesOut, 0, 0));
}
