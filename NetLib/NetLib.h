#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <stdint.h>
#include "Common.h"


#define NET_OK      1    // sve poslato / primljeno
#define NET_CLOSED  0    // druga strana je zatvorila vezu
#define NET_ERROR  -1    // greska na soketu ili neispravna poruka


bool netInit();

void netCleanup();

// Otvara soket koji slusa na datom portu. Vraca INVALID_SOCKET pri gresci.
SOCKET netListen(uint16_t port);
// Povezuje se na ip:port (sa ukljucenim TCP_NODELAY). Vraca INVALID_SOCKET pri gresci.
SOCKET netConnect(const char* ip, uint16_t port);

// Iskljuci Nagle-ov algoritam (TCP_NODELAY): male poruke idu odmah, bez cekanja
// da se skupi vise podataka. Bez ovoga jedan zahtev moze da kasni 40-200 ms.
// Poziva se i za sokete dobijene sa accept. Vraca false pri gresci.
bool netSetNoDelay(SOCKET s);

// Prima tacno len bajtova (vrti petlju dok ne primi sve).
int recvAll(SOCKET s, char* buf, int len);
// Salje tacno len bajtova (vrti petlju dok ne posalje sve).
int sendAll(SOCKET s, const char* buf, int len);

// Salje celu poruku: zaglavlje + telo. Telo se predaje u redosledu bajtova
// racunara; funkcija sama pravi kopiju u mreznom redosledu.
int sendMsg(SOCKET s, uint16_t type, uint16_t flags, uint32_t requestId,
    const void* body, uint32_t length);

// Prima celu poruku. Zaglavlje i telo vraca u redosledu bajtova racunara.
// Telo se alocira ovde; pozivalac ga oslobadja sa freeBody.
int recvMsg(SOCKET s, MsgHeader* hdr, char** body);
void freeBody(char* body);

// Okretanje float i int64 vrednosti u mrezni redosled bajtova i nazad.
uint32_t floatToNet(float f);
float    netToFloat(uint32_t n);
uint64_t i64ToNet(int64_t v);
int64_t  netToI64(uint64_t n);

// Okrece sve brojeve u telu poruke date vrste (toNet = ka mrezi ili od mreze).
// Proverava i da li duzina tela odgovara vrsti poruke. Vraca false ako ne odgovara.
bool convertBody(uint16_t type, char* body, uint32_t length, bool toNet);

// Ime vrste poruke, za ispis u logu.
const char* msgTypeName(uint16_t type);
