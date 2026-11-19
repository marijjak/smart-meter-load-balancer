// Test biblioteke NetLib: dva procesa razmene po jednu poruku svake vrste.
//
//   NetTests.exe          pokrene sebe kao klijenta i radi kao server (jedan klik)
//   NetTests.exe server   samo server (port 5099)
//   NetTests.exe client   samo klijent
//
// Klijent posalje svaku poruku, server proveri da je stiglo tacno ono sto je
// poslato i vrati istu poruku nazad, a klijent proveri i odgovor. Tako je svaka
// vrsta poruke prosla kroz mrezu u oba smera (okretanje bajtova tamo i nazad).
// Pre toga se lokalno proveravaju funkcije za okretanje bajtova.

#include "NetLib.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TEST_PORT 5099
#define BIG_QUERY_COUNT 100000   // ~1.2 MB, TCP ga sigurno isece na delove

static int g_failed = 0;

// Ispise rezultat jedne provere i broji neuspele.
static void check(bool ok, const char* what) {
    printf("  [%s] %s\n", ok ? " OK " : "FAIL", what);
    if (!ok) g_failed++;
}

// ---------------------------------------------------------------------------
// Test slucajevi: za redni broj i napravi poruku koja se salje.
// ---------------------------------------------------------------------------

typedef struct {
    uint16_t type;
    uint16_t flags;
    char*    body;     // malloc; oslobadja freeCase
    uint32_t length;
    const char* name;  // opis za ispis
} TestCase;

#define CASE_COUNT 15

// Vreme vece od 32 bita, da se proveri i gornja polovina int64.
static const int64_t TS_BIG = 0x0000000123456789LL;

static void fillReading(Reading* r, uint32_t meterId, int64_t ts, float kwh) {
    r->meterId = meterId;
    r->timestamp = ts;
    r->kwh = kwh;
}

// Pravi test slucaj broj i. Vraca false ako i ne postoji.
static bool makeCase(int i, TestCase* c) {
    memset(c, 0, sizeof(*c));
    switch (i) {
    case 0: {
        c->type = MSG_WRITE; c->name = "WRITE (vreme > 32 bita)";
        c->length = sizeof(Reading);
        c->body = (char*)malloc(c->length);
        fillReading((Reading*)c->body, 13, TS_BIG, 1.25f);
        return true;
    }
    case 1: {
        c->type = MSG_WRITE; c->name = "WRITE (negativno vreme, negativan kWh)";
        c->length = sizeof(Reading);
        c->body = (char*)malloc(c->length);
        fillReading((Reading*)c->body, 0xFFFFFFFFu, -5, -0.001f);
        return true;
    }
    case 2: {
        c->type = MSG_QUERY; c->name = "QUERY";
        c->length = sizeof(QueryBody);
        c->body = (char*)malloc(c->length);
        ((QueryBody*)c->body)->meterId = 13;
        return true;
    }
    case 3: {
        c->type = MSG_ACK; c->name = "ACK";
        c->length = sizeof(AckBody);
        c->body = (char*)malloc(c->length);
        ((AckBody*)c->body)->status = 0;
        return true;
    }
    case 4: {
        c->type = MSG_QUERY_RESP; c->name = "QUERY_RESP (3 ocitavanja)";
        uint32_t n = 3;
        c->length = sizeof(QueryRespHeader) + n * sizeof(ReadingItem);
        c->body = (char*)malloc(c->length);
        QueryRespHeader* h = (QueryRespHeader*)c->body;
        h->meterId = 13; h->count = n;
        ReadingItem* it = (ReadingItem*)(c->body + sizeof(QueryRespHeader));
        for (uint32_t k = 0; k < n; k++) {
            it[k].timestamp = TS_BIG + k;
            it[k].kwh = 0.5f * (float)(k + 1);
        }
        return true;
    }
    case 5: {
        c->type = MSG_QUERY_RESP; c->name = "QUERY_RESP (0 ocitavanja)";
        c->length = sizeof(QueryRespHeader);
        c->body = (char*)malloc(c->length);
        QueryRespHeader* h = (QueryRespHeader*)c->body;
        h->meterId = 99; h->count = 0;
        return true;
    }
    case 6: {
        c->type = MSG_QUERY_RESP; c->name = "QUERY_RESP (100.000 ocitavanja, ~1.2 MB)";
        uint32_t n = BIG_QUERY_COUNT;
        c->length = sizeof(QueryRespHeader) + n * sizeof(ReadingItem);
        c->body = (char*)malloc(c->length);
        QueryRespHeader* h = (QueryRespHeader*)c->body;
        h->meterId = 7; h->count = n;
        ReadingItem* it = (ReadingItem*)(c->body + sizeof(QueryRespHeader));
        for (uint32_t k = 0; k < n; k++) {
            it[k].timestamp = 1700000000LL + k;
            it[k].kwh = (float)k / 1000.0f;
        }
        return true;
    }
    case 7: {
        c->type = MSG_ERROR; c->name = "ERROR";
        c->length = sizeof(ErrorBody);
        c->body = (char*)malloc(c->length);
        ((ErrorBody*)c->body)->code = ERR_LAST_WORKER;
        return true;
    }
    case 8: {
        c->type = MSG_REGISTER; c->name = "REGISTER";
        c->length = sizeof(RegisterBody);
        c->body = (char*)malloc(c->length);
        ((RegisterBody*)c->body)->workerId = 7;
        return true;
    }
    case 9:  c->type = MSG_REGISTER_OK; c->name = "REGISTER_OK (prazno telo)"; return true;
    case 10: c->type = MSG_LEAVE_BEGIN; c->name = "LEAVE_BEGIN (prazno telo)"; return true;
    case 11: {
        c->type = MSG_TRANSFER; c->name = "TRANSFER (512 ocitavanja, pun paket)";
        uint32_t n = TRANSFER_BATCH;
        c->length = sizeof(TransferHeader) + n * sizeof(Reading);
        c->body = (char*)malloc(c->length);
        ((TransferHeader*)c->body)->count = n;
        Reading* r = (Reading*)(c->body + sizeof(TransferHeader));
        for (uint32_t k = 0; k < n; k++)
            fillReading(&r[k], 1000 + k, TS_BIG + k, 0.25f * (float)k);
        return true;
    }
    case 12: {
        c->type = MSG_TRANSFER_END; c->name = "TRANSFER_END";
        c->length = sizeof(TransferEndBody);
        c->body = (char*)malloc(c->length);
        ((TransferEndBody*)c->body)->total = TRANSFER_BATCH;
        return true;
    }
    case 13: {
        c->type = MSG_RECLAIM; c->name = "RECLAIM (lista od 3 brojila)";
        uint32_t n = 3;
        c->length = sizeof(ReclaimHeader) + n * sizeof(uint32_t);
        c->body = (char*)malloc(c->length);
        ((ReclaimHeader*)c->body)->count = n;
        uint32_t* ids = (uint32_t*)(c->body + sizeof(ReclaimHeader));
        ids[0] = 3; ids[1] = 13; ids[2] = 0x12345678u;
        return true;
    }
    case 14: {
        c->type = MSG_RECLAIM; c->flags = FLAG_ALL; c->name = "RECLAIM sa FLAG_ALL";
        c->length = sizeof(ReclaimHeader);
        c->body = (char*)malloc(c->length);
        ((ReclaimHeader*)c->body)->count = 0;
        return true;
    }
    }
    return false;
}

static void freeCase(TestCase* c) {
    free(c->body);
    c->body = NULL;
}

// Uporedi primljenu poruku sa ocekivanom.
static bool sameMsg(const TestCase* c, uint32_t requestId,
    const MsgHeader* h, const char* body) {
    return h->type == c->type && h->flags == c->flags &&
        h->requestId == requestId && h->length == c->length &&
        (c->length == 0 || memcmp(body, c->body, c->length) == 0);
}

// LEAVE_OK je poslednja poruka; po njoj server zna da je kraj.
static const uint32_t LAST_ID = 1000;

// ---------------------------------------------------------------------------
// Lokalni testovi (bez mreze)
// ---------------------------------------------------------------------------

static void localTests(void) {
    printf("Lokalni testovi okretanja bajtova:\n");

    // 1.0f je 0x3F800000; u mreznom redosledu prvi bajt mora biti 0x3F.
    uint32_t n = floatToNet(1.0f);
    unsigned char* b = (unsigned char*)&n;
    check(b[0] == 0x3F && b[1] == 0x80 && b[2] == 0 && b[3] == 0,
        "floatToNet(1.0) daje bajtove 3F 80 00 00");
    check(netToFloat(floatToNet(-123.456f)) == -123.456f, "float tamo i nazad");

    // 0x0102030405060708 u mreznom redosledu: bajtovi 01 02 ... 08.
    uint64_t m = i64ToNet(0x0102030405060708LL);
    unsigned char* c = (unsigned char*)&m;
    bool order = true;
    for (int i = 0; i < 8; i++) if (c[i] != i + 1) order = false;
    check(order, "i64ToNet daje bajtove 01 02 03 04 05 06 07 08");
    check(netToI64(i64ToNet(-1)) == -1, "int64 -1 tamo i nazad");
    check(netToI64(i64ToNet(TS_BIG)) == TS_BIG, "int64 > 32 bita tamo i nazad");

    // convertBody mora da odbije pogresnu duzinu tela.
    char buf[64] = { 0 };
    check(!convertBody(MSG_WRITE, buf, 15, true), "WRITE sa 15 bajtova se odbija");
    check(!convertBody(MSG_ACK, buf, 0, true), "ACK bez tela se odbija");
    check(!convertBody(MSG_LEAVE_OK, buf, 4, true), "LEAVE_OK sa telom se odbija");
    ((TransferHeader*)buf)->count = TRANSFER_BATCH + 1;
    check(!convertBody(MSG_TRANSFER, buf, 64, true), "TRANSFER sa 513 ocitavanja se odbija");
    check(!convertBody(99, buf, 0, true), "nepoznata vrsta poruke se odbija");
}

// ---------------------------------------------------------------------------
// Klijent i server
// ---------------------------------------------------------------------------

static int runClient(void) {
    printf("[klijent] povezujem se na port %d\n", TEST_PORT);
    SOCKET s = INVALID_SOCKET;
    for (int tries = 0; tries < 50 && s == INVALID_SOCKET; tries++) {
        s = netConnect("127.0.0.1", TEST_PORT);
        if (s == INVALID_SOCKET) Sleep(100);   // server mozda jos nije spreman
    }
    if (s == INVALID_SOCKET) {
        printf("[klijent] ne mogu da se povezem\n");
        return 1;
    }

    // Poruka koja ne prolazi proveru ne sme ni da se posalje.
    check(sendMsg(s, MSG_WRITE, 0, 1, "x", 1) == NET_ERROR,
        "[klijent] sendMsg odbija WRITE pogresne duzine");

    for (int i = 0; i < CASE_COUNT; i++) {
        TestCase c;
        makeCase(i, &c);
        uint32_t id = 100 + (uint32_t)i;

        bool ok = sendMsg(s, c.type, c.flags, id, c.body, c.length) == NET_OK;
        MsgHeader h;
        char* body = NULL;
        ok = ok && recvMsg(s, &h, &body) == NET_OK && sameMsg(&c, id, &h, body);

        char what[128];
        snprintf(what, sizeof(what), "[klijent] eho: %s", c.name);
        check(ok, what);
        freeBody(body);
        freeCase(&c);
    }

    // Poslednja poruka: LEAVE_OK, server posle nje zatvara vezu.
    sendMsg(s, MSG_LEAVE_OK, 0, LAST_ID, NULL, 0);
    MsgHeader h;
    char* body = NULL;
    check(recvMsg(s, &h, &body) == NET_CLOSED, "[klijent] LEAVE_OK, server zatvorio vezu");
    freeBody(body);

    closesocket(s);
    return g_failed == 0 ? 0 : 1;
}

// Prihvati jednog klijenta, proveri svaku poruku i vrati je nazad.
static void serveOne(SOCKET listenSock) {
    SOCKET s = accept(listenSock, NULL, NULL);
    if (s == INVALID_SOCKET) {
        check(false, "[server] accept");
        return;
    }

    int got = 0;
    MsgHeader h;
    char* body = NULL;
    while (recvMsg(s, &h, &body) == NET_OK) {
        if (h.requestId == LAST_ID) {
            check(h.type == MSG_LEAVE_OK && h.length == 0, "[server] LEAVE_OK (prazno telo)");
            freeBody(body);
            break;
        }
        TestCase c;
        int i = (int)h.requestId - 100;
        bool known = makeCase(i, &c);
        char what[128];
        snprintf(what, sizeof(what), "[server] primljeno: %s",
            known ? c.name : msgTypeName(h.type));
        check(known && sameMsg(&c, h.requestId, &h, body), what);
        sendMsg(s, h.type, h.flags, h.requestId, body, h.length);
        freeBody(body);
        if (known) freeCase(&c);
        got++;
    }
    check(got == CASE_COUNT, "[server] stigle su sve poruke");
    closesocket(s);
}

// Pokrene ovaj isti program sa argumentom "client" kao poseban proces.
static bool startClientProcess(PROCESS_INFORMATION* pi) {
    char exe[MAX_PATH];
    if (GetModuleFileNameA(NULL, exe, MAX_PATH) == 0) return false;
    char cmd[MAX_PATH + 16];
    snprintf(cmd, sizeof(cmd), "\"%s\" client", exe);

    STARTUPINFOA si;
    memset(&si, 0, sizeof(si));
    si.cb = sizeof(si);
    return CreateProcessA(NULL, cmd, NULL, NULL, FALSE, 0, NULL, NULL, &si, pi) != 0;
}

static int runServer(bool spawnClient) {
    SOCKET ls = netListen(TEST_PORT);
    if (ls == INVALID_SOCKET) {
        printf("[server] netListen nije uspeo (port %d zauzet?)\n", TEST_PORT);
        return 1;
    }
    printf("[server] slusam na portu %d\n", TEST_PORT);

    PROCESS_INFORMATION pi;
    memset(&pi, 0, sizeof(pi));
    if (spawnClient && !startClientProcess(&pi)) {
        printf("[server] ne mogu da pokrenem klijenta\n");
        closesocket(ls);
        return 1;
    }

    serveOne(ls);
    closesocket(ls);

    if (spawnClient) {
        WaitForSingleObject(pi.hProcess, INFINITE);
        DWORD code = 1;
        GetExitCodeProcess(pi.hProcess, &code);
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
        check(code == 0, "klijent zavrsio bez gresaka");
    }
    return g_failed == 0 ? 0 : 1;
}

int main(int argc, char* argv[]) {
    bool client = argc > 1 && strcmp(argv[1], "client") == 0;
    bool serverOnly = argc > 1 && strcmp(argv[1], "server") == 0;

    if (!netInit()) {
        printf("WSAStartup nije uspeo\n");
        return 1;
    }

    int rc;
    if (client) {
        rc = runClient();
    }
    else {
        localTests();
        rc = runServer(!serverOnly);
        printf("\n%s (neuspelih provera: %d)\n",
            rc == 0 ? "SVI TESTOVI SU PROSLI" : "IMA GRESAKA", g_failed);
    }

    netCleanup();
    if (!client) {
        printf("Pritisni Enter za kraj...\n");
        getchar();
    }
    return rc;
}
