#pragma once
// Veza User-a sa LB-om (Osoba 1).
//
// Glavna nit salje zahteve (clientSend), a nit TPO prima odgovore, uparuje ih
// sa zahtevima po requestId-ju i za svaki pozove funkciju koju je dao pozivalac.
// Semafor-prozor ogranicava koliko zahteva moze biti "u letu" (USER_WINDOW = 256):
// clientSend ga smanji pre slanja, TPO ga poveca kad stigne odgovor.

#include "NetLib.h"

// Ono sto User pamti o poslatom zahtevu dok ceka odgovor.
typedef struct {
    uint32_t requestId;
    uint16_t type;        // MSG_WRITE ili MSG_QUERY
    uint32_t meterId;
    LONGLONG sentTicks;   // QueryPerformanceCounter u trenutku slanja
} RequestInfo;

// Funkcija koju TPO zove za svaki odgovor (ACK, QUERY_RESP ili ERROR).
// Zove se iz niti TPO. latencyMs = vreme od slanja do prijema odgovora.
typedef void (*ReplyFn)(const RequestInfo* req, const MsgHeader* h,
    const char* body, double latencyMs, void* ctx);

// Povezuje se na LB i pokrece nit TPO. Vraca false ako ne uspe.
bool clientStart(const char* ip, uint16_t port, ReplyFn fn, void* ctx);

// Menja funkciju za odgovore (npr. pri prelasku u stress rezim).
// Sme da se zove samo kad nema zahteva u letu (posle clientWaitAll).
void clientSetHandler(ReplyFn fn, void* ctx);

// Salje zahtev. Ako je prozor pun, ceka da stigne neki odgovor.
// Vraca requestId (> 0), ili 0 ako je veza prekinuta.
uint32_t clientSend(uint16_t type, const void* body, uint32_t length, uint32_t meterId);

// Ceka dok svi poslati zahtevi ne dobiju odgovor, najvise timeoutMs.
// Vraca true ako je sve stiglo.
bool clientWaitAll(DWORD timeoutMs);

// Broj zahteva koji cekaju odgovor.
size_t clientInFlight(void);

// Da li je veza sa LB-om jos ziva.
bool clientAlive(void);

// Event koji se postavi kad se veza prekine ili kad pocne gasenje.
HANDLE clientStopEvent(void);

// Gasi vezu: zatvara soket, ceka TPO i oslobadja sve.
void clientStop(void);

// Trenutno vreme za merenje (QueryPerformanceCounter) i pretvaranje u ms.
LONGLONG clientNow(void);
double   clientTicksToMs(LONGLONG ticks);
