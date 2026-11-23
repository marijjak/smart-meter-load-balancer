#include "Client.h"
#include "HashMap.h"
#include <stdio.h>
#include <stdlib.h>

static SOCKET           s_sock = INVALID_SOCKET;
static HANDLE           s_stopEvent = NULL;   // veza prekinuta ili gasenje
static HANDLE           s_window = NULL;      // semafor-prozor: slobodna mesta
static HANDLE           s_tpo = NULL;
static HashMap          s_requests;           // requestId -> RequestInfo*
static CRITICAL_SECTION s_cs;                 // stiti s_requests
static uint32_t         s_nextId = 0;         // koristi ga samo glavna nit
static ReplyFn          s_fn = NULL;
static void*            s_ctx = NULL;
static LARGE_INTEGER    s_freq;
static bool             s_ready = false;

LONGLONG clientNow(void) {
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return t.QuadPart;
}

double clientTicksToMs(LONGLONG ticks) {
    return (double)ticks * 1000.0 / (double)s_freq.QuadPart;
}

// TPO: prima odgovore, uparuje ih po requestId-ju i oslobadja mesto u prozoru.
static DWORD WINAPI receiveThread(LPVOID p) {
    (void)p;
    for (;;) {
        MsgHeader h;
        char* body;
        if (recvMsg(s_sock, &h, &body) != NET_OK) break;
        LONGLONG now = clientNow();

        EnterCriticalSection(&s_cs);
        RequestInfo* req = (RequestInfo*)hmRemove(&s_requests, h.requestId);
        LeaveCriticalSection(&s_cs);

        if (req == NULL) {
            printf("[UR] Odgovor %s za nepoznat requestId %u.\n", msgTypeName(h.type), h.requestId);
        }
        else {
            if (s_fn != NULL) s_fn(req, &h, body, clientTicksToMs(now - req->sentTicks), s_ctx);
            free(req);
            ReleaseSemaphore(s_window, 1, NULL);
        }
        freeBody(body);
    }

    if (WaitForSingleObject(s_stopEvent, 0) != WAIT_OBJECT_0)
        printf("\n[UR] Veza sa LB-om je prekinuta.\n");
    SetEvent(s_stopEvent);   // budi glavnu nit ako ceka na prozor
    return 0;
}

bool clientStart(const char* ip, uint16_t port, ReplyFn fn, void* ctx) {
    QueryPerformanceFrequency(&s_freq);
    s_fn = fn;
    s_ctx = ctx;
    if (!hmInit(&s_requests, 512)) return false;
    InitializeCriticalSection(&s_cs);
    s_stopEvent = CreateEvent(NULL, TRUE, FALSE, NULL);
    s_window = CreateSemaphore(NULL, USER_WINDOW, USER_WINDOW, NULL);
    s_ready = true;
    if (s_stopEvent == NULL || s_window == NULL) {
        clientStop();
        return false;
    }

    s_sock = netConnect(ip, port);
    if (s_sock == INVALID_SOCKET) {
        printf("[UR] Ne mogu da se povezem na LB (%s:%u). Da li je LB pokrenut?\n", ip, port);
        clientStop();
        return false;
    }
    s_tpo = CreateThread(NULL, 0, receiveThread, NULL, 0, NULL);
    if (s_tpo == NULL) {
        clientStop();
        return false;
    }
    return true;
}

void clientSetHandler(ReplyFn fn, void* ctx) {
    s_fn = fn;
    s_ctx = ctx;
}

uint32_t clientSend(uint16_t type, const void* body, uint32_t length, uint32_t meterId) {
    // Cekaj slobodno mesto u prozoru, ili prekid veze.
    HANDLE wait[2] = { s_stopEvent, s_window };
    if (WaitForMultipleObjects(2, wait, FALSE, INFINITE) != WAIT_OBJECT_0 + 1) return 0;

    RequestInfo* req = (RequestInfo*)malloc(sizeof(RequestInfo));
    if (req == NULL) { ReleaseSemaphore(s_window, 1, NULL); return 0; }
    if (++s_nextId == 0) s_nextId = 1;
    req->requestId = s_nextId;
    req->type = type;
    req->meterId = meterId;
    req->sentTicks = clientNow();

    // Zahtev se upisuje PRE slanja: odgovor moze da stigne pre nego sto send vrati.
    EnterCriticalSection(&s_cs);
    bool ok = hmPut(&s_requests, req->requestId, req);
    LeaveCriticalSection(&s_cs);
    if (!ok) { free(req); ReleaseSemaphore(s_window, 1, NULL); return 0; }

    if (sendMsg(s_sock, type, 0, req->requestId, body, length) != NET_OK) {
        // Veza je pukla; zahtev ostaje u mapi i oslobadja se u clientStop.
        SetEvent(s_stopEvent);
        return 0;
    }
    return req->requestId;
}

size_t clientInFlight(void) {
    EnterCriticalSection(&s_cs);
    size_t n = hmCount(&s_requests);
    LeaveCriticalSection(&s_cs);
    return n;
}

bool clientWaitAll(DWORD timeoutMs) {
    LONGLONG start = clientNow();
    while (clientInFlight() > 0) {
        if (!clientAlive()) return false;
        if (clientTicksToMs(clientNow() - start) > (double)timeoutMs) return false;
        Sleep(5);
    }
    return true;
}

bool clientAlive(void) {
    return s_stopEvent != NULL && WaitForSingleObject(s_stopEvent, 0) != WAIT_OBJECT_0;
}

HANDLE clientStopEvent(void) {
    return s_stopEvent;
}

// Oslobadja jedan zahtev koji nije dobio odgovor (za obilazak pri gasenju).
static bool freeRequest(uint32_t key, void* value, void* ctx) {
    (void)key; (void)ctx;
    free(value);
    return true;
}

void clientStop(void) {
    if (!s_ready) return;
    if (s_stopEvent != NULL) SetEvent(s_stopEvent);
    if (s_sock != INVALID_SOCKET) shutdown(s_sock, SD_BOTH);   // odblokira recv u TPO
    if (s_tpo != NULL) {
        WaitForSingleObject(s_tpo, INFINITE);
        CloseHandle(s_tpo);
        s_tpo = NULL;
    }
    if (s_sock != INVALID_SOCKET) { closesocket(s_sock); s_sock = INVALID_SOCKET; }

    size_t lost = hmCount(&s_requests);
    if (lost > 0) printf("[UR] %zu zahteva je ostalo bez odgovora.\n", lost);
    hmForEachRange(&s_requests, 0, hmBucketCount(&s_requests), freeRequest, NULL);
    hmFree(&s_requests);
    DeleteCriticalSection(&s_cs);
    if (s_window != NULL) { CloseHandle(s_window); s_window = NULL; }
    if (s_stopEvent != NULL) { CloseHandle(s_stopEvent); s_stopEvent = NULL; }
    s_ready = false;
}
