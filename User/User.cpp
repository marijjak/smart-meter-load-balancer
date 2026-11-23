// User (UR): salje upise i upite LB-u i ispisuje odgovore.
//
//   User.exe                                     meni (upis, upit, izlaz)
//   User.exe test1                               funkcionalni test 1 (10 upisa za 3 brojila + upiti)
//   User.exe stress <prvo> <broj> [poBrojilu]    stress: upisi za brojila prvo .. prvo+broj-1
//   User.exe otisak <prvo> <broj> [fajl]         otisak: broj ocitavanja i zbir Wh po brojilu
//
// Niti: glavna (meni i slanje) i TPO (prijem odgovora, u Client.cpp).

#include "Client.h"
#include "DynArray.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <math.h>

#define LB_ADDRESS "127.0.0.1"

static HANDLE g_doneEvent = NULL;   // postavljen kad je gasenje zavrseno (za zatvaranje prozora)

// ---------------------------------------------------------------------------
// Pomocne funkcije
// ---------------------------------------------------------------------------

// Opis koda greske iz ERROR poruke.
static const char* errorText(uint32_t code) {
    switch (code) {
    case ERR_NO_WORKERS:    return "nema nijednog aktivnog Worker-a";
    case ERR_WORKER_FAILED: return "Worker je pao dok je obradjivao zahtev";
    case ERR_LAST_WORKER:   return "poslednji Worker ne sme da se odjavi";
    case ERR_BAD_REQUEST:   return "neispravan zahtev";
    default:                return "nepoznata greska";
    }
}

// Unix vreme kao tekst "dd.mm.gggg hh:mm:ss" (lokalno vreme).
static void formatTime(int64_t ts, char* buf, size_t size) {
    time_t t = (time_t)ts;
    struct tm tm;
    if (localtime_s(&tm, &t) != 0 || strftime(buf, size, "%d.%m.%Y %H:%M:%S", &tm) == 0)
        snprintf(buf, size, "%lld", (long long)ts);
}

// Potrosnja za test podatke: ista za isto (brojilo, k), pa se otisak moze proveriti.
static float testKwh(uint32_t meterId, uint32_t k) {
    return (float)((meterId * 37u + k * 101u) % 5000u) / 1000.0f;
}

// kWh u Wh kao ceo broj (za otisak; zbir celih brojeva ne zavisi od redosleda).
static long long toWh(float kwh) {
    return llround((double)kwh * 1000.0);
}

// Posalje jedan upis. Vraca requestId ili 0.
static uint32_t sendWrite(uint32_t meterId, int64_t ts, float kwh) {
    Reading r;
    r.meterId = meterId;
    r.timestamp = ts;
    r.kwh = kwh;
    return clientSend(MSG_WRITE, &r, sizeof(r), meterId);
}

// Posalje jedan upit. Vraca requestId ili 0.
static uint32_t sendQuery(uint32_t meterId) {
    QueryBody q = { meterId };
    return clientSend(MSG_QUERY, &q, sizeof(q), meterId);
}

// Procita broj iz teksta; vraca false ako tekst nije ceo broj.
static bool parseUInt(const char* s, uint32_t* out) {
    char* end;
    unsigned long v = strtoul(s, &end, 10);
    if (end == s || (*end != '\0' && *end != '\n' && *end != '\r') || v > 0xFFFFFFFFul) return false;
    *out = (uint32_t)v;
    return true;
}

// ---------------------------------------------------------------------------
// Meni (interaktivni rezim)
// ---------------------------------------------------------------------------

// Ispis odgovora u meniju (zove se iz niti TPO).
static void printReply(const RequestInfo* req, const MsgHeader* h,
    const char* body, double latencyMs, void* ctx) {
    (void)ctx;
    if (h->type == MSG_ACK) {
        const AckBody* a = (const AckBody*)body;
        printf("[UR] #%u upis za brojilo %u: %s (%.2f ms)\n", req->requestId, req->meterId,
            a->status == 0 ? "OK" : "GRESKA", latencyMs);
    }
    else if (h->type == MSG_QUERY_RESP) {
        const QueryRespHeader* q = (const QueryRespHeader*)body;
        const ReadingItem* it = (const ReadingItem*)(body + sizeof(QueryRespHeader));
        printf("[UR] #%u brojilo %u: %u ocitavanja (%.2f ms)\n",
            req->requestId, q->meterId, q->count, latencyMs);
        double sum = 0;
        for (uint32_t i = 0; i < q->count; i++) {
            char t[32];
            formatTime(it[i].timestamp, t, sizeof(t));
            printf("       %3u.  %s   %10.3f kWh\n", i + 1, t, it[i].kwh);
            sum += it[i].kwh;
        }
        if (q->count > 0) printf("       ukupno: %.3f kWh\n", sum);
    }
    else if (h->type == MSG_ERROR) {
        printf("[UR] #%u brojilo %u: GRESKA - %s\n", req->requestId, req->meterId,
            errorText(((const ErrorBody*)body)->code));
    }
    else {
        printf("[UR] #%u: neocekivan odgovor %s\n", req->requestId, msgTypeName(h->type));
    }
}

// Procita jedan red sa konzole. Vraca false na kraj ulaza (Ctrl+C, Ctrl+Z).
static bool readLine(const char* prompt, char* buf, int size) {
    printf("%s", prompt);
    fflush(stdout);
    if (fgets(buf, size, stdin) == NULL) return false;
    buf[strcspn(buf, "\r\n")] = '\0';
    return true;
}

static void menuWrite(void) {
    char line[64];
    uint32_t meterId;
    if (!readLine("Broj brojila: ", line, sizeof(line))) return;
    if (!parseUInt(line, &meterId)) { printf("Neispravan broj brojila.\n"); return; }
    if (!readLine("Potrosnja (kWh): ", line, sizeof(line))) return;
    char* end;
    double kwh = strtod(line, &end);
    if (end == line || *end != '\0') { printf("Neispravna potrosnja.\n"); return; }

    if (sendWrite(meterId, (int64_t)time(NULL), (float)kwh) == 0) return;
    clientWaitAll(3000);   // sacekaj odgovor, da ispis ne upadne u meni
}

static void menuQuery(void) {
    char line[64];
    uint32_t meterId;
    if (!readLine("Broj brojila: ", line, sizeof(line))) return;
    if (!parseUInt(line, &meterId)) { printf("Neispravan broj brojila.\n"); return; }
    if (sendQuery(meterId) == 0) return;
    clientWaitAll(3000);
}

static int runMenu(void) {
    char line[16];
    while (clientAlive()) {
        printf("\n1 - upis   2 - upit   0 - izlaz\n");
        if (!readLine("> ", line, sizeof(line))) break;
        if (!clientAlive()) break;
        if (strcmp(line, "1") == 0) menuWrite();
        else if (strcmp(line, "2") == 0) menuQuery();
        else if (strcmp(line, "0") == 0) break;
        else if (line[0] != '\0') printf("Nepoznata komanda.\n");
    }
    return 0;
}

// ---------------------------------------------------------------------------
// Funkcionalni test 1: 10 upisa za 3 brojila, pa upit za svako
// ---------------------------------------------------------------------------

#define T1_METERS 3
#define T1_WRITES 10

typedef struct {
    uint32_t meters[T1_METERS];
    Reading  sent[T1_WRITES];      // sta je poslato
    uint32_t sentCount;
    uint32_t acks, errors;
    bool     queryOk[T1_METERS];   // upit vratio sve poslate vrednosti, istim redom
    uint32_t queryCount[T1_METERS];
} Test1;

// Proveri da li odgovor na upit sadrzi poslata ocitavanja tog brojila, istim redom.
static bool test1Check(const Test1* t, uint32_t meterId, const QueryRespHeader* q,
    const ReadingItem* it) {
    uint32_t pos = 0;   // poslata ocitavanja moraju biti na kraju liste (starija mogu biti pre)
    uint32_t mine = 0;
    for (uint32_t i = 0; i < t->sentCount; i++) if (t->sent[i].meterId == meterId) mine++;
    if (q->count < mine) return false;
    pos = q->count - mine;
    for (uint32_t i = 0; i < t->sentCount; i++) {
        if (t->sent[i].meterId != meterId) continue;
        if (it[pos].timestamp != t->sent[i].timestamp || it[pos].kwh != t->sent[i].kwh) return false;
        pos++;
    }
    return true;
}

static void test1Reply(const RequestInfo* req, const MsgHeader* h,
    const char* body, double latencyMs, void* ctx) {
    Test1* t = (Test1*)ctx;
    printReply(req, h, body, latencyMs, NULL);
    if (h->type == MSG_ACK) t->acks++;
    else if (h->type == MSG_ERROR) t->errors++;
    else if (h->type == MSG_QUERY_RESP) {
        const QueryRespHeader* q = (const QueryRespHeader*)body;
        for (int m = 0; m < T1_METERS; m++) {
            if (t->meters[m] != q->meterId) continue;
            t->queryCount[m] = q->count;
            t->queryOk[m] = test1Check(t, q->meterId,
                q, (const ReadingItem*)(body + sizeof(QueryRespHeader)));
        }
    }
}

static int runTest1(void) {
    static Test1 t;
    memset(&t, 0, sizeof(t));
    t.meters[0] = 3; t.meters[1] = 13; t.meters[2] = 27;
    clientSetHandler(test1Reply, &t);

    printf("Funkcionalni test 1: %d upisa za brojila %u, %u, %u, pa upit za svako.\n\n",
        T1_WRITES, t.meters[0], t.meters[1], t.meters[2]);
    int64_t base = (int64_t)time(NULL);
    for (uint32_t i = 0; i < T1_WRITES; i++) {
        Reading* r = &t.sent[i];
        r->meterId = t.meters[i % T1_METERS];
        r->timestamp = base + i;
        r->kwh = testKwh(r->meterId, i) + 0.5f;
        t.sentCount++;
        if (sendWrite(r->meterId, r->timestamp, r->kwh) == 0) break;
    }
    clientWaitAll(5000);   // svi upisi potvrdjeni pre upita
    printf("\n");
    for (int m = 0; m < T1_METERS; m++) sendQuery(t.meters[m]);
    clientWaitAll(5000);

    bool ok = t.acks == T1_WRITES && t.errors == 0;
    printf("\nPotvrda upisa: %u / %d, gresaka: %u\n", t.acks, T1_WRITES, t.errors);
    for (int m = 0; m < T1_METERS; m++) {
        printf("Brojilo %u: %u ocitavanja, poslate vrednosti %s\n", t.meters[m],
            t.queryCount[m], t.queryOk[m] ? "se poklapaju" : "SE NE POKLAPAJU");
        ok = ok && t.queryOk[m];
    }
    printf("\nFUNKCIONALNI TEST 1: %s\n", ok ? "PROSAO" : "PAO");
    return ok ? 0 : 1;
}

// ---------------------------------------------------------------------------
// Stress test: mnogo upisa, meri propusnost i kasnjenje
// ---------------------------------------------------------------------------

typedef struct {
    DynArray latencies;   // double, ms
    uint32_t acks;
    uint32_t errors;
} Stress;

static void stressReply(const RequestInfo* req, const MsgHeader* h,
    const char* body, double latencyMs, void* ctx) {
    (void)req; (void)body;
    Stress* s = (Stress*)ctx;
    if (h->type == MSG_ACK) s->acks++;
    else s->errors++;
    daPush(&s->latencies, &latencyMs);
}

static int compareDouble(const void* a, const void* b) {
    double x = *(const double*)a, y = *(const double*)b;
    return x < y ? -1 : (x > y ? 1 : 0);
}

static int runStress(uint32_t first, uint32_t count, uint32_t perMeter) {
    Stress s;
    memset(&s, 0, sizeof(s));
    uint64_t total = (uint64_t)count * perMeter;
    if (total == 0 || total > 10000000u || !daInit(&s.latencies, sizeof(double), (size_t)total)) {
        printf("Neispravan broj upisa.\n");
        return 1;
    }
    clientSetHandler(stressReply, &s);

    printf("Stress: %llu upisa (brojila %u .. %u, po %u ocitavanja), prozor %d...\n",
        (unsigned long long)total, first, first + count - 1, perMeter, USER_WINDOW);
    LONGLONG start = clientNow();
    uint64_t sent = 0;
    // Brojila se smenjuju, pa uzastopni upisi idu razlicitim WR-ovima.
    for (uint32_t k = 0; k < perMeter && clientAlive(); k++) {
        for (uint32_t m = 0; m < count; m++) {
            uint32_t meterId = first + m;
            if (sendWrite(meterId, 1700000000LL + k, testKwh(meterId, k)) == 0) break;
            sent++;
        }
    }
    bool all = clientWaitAll(60000);
    double secs = clientTicksToMs(clientNow() - start) / 1000.0;

    size_t n = daCount(&s.latencies);
    double avg = 0, p99 = 0, maxL = 0;
    if (n > 0) {
        double* lat = (double*)daGet(&s.latencies, 0);
        qsort(lat, n, sizeof(double), compareDouble);
        for (size_t i = 0; i < n; i++) avg += lat[i];
        avg /= (double)n;
        p99 = lat[(size_t)((double)(n - 1) * 0.99)];
        maxL = lat[n - 1];
    }

    printf("\nPoslato:          %llu\n", (unsigned long long)sent);
    printf("Potvrdjeno (ACK): %u\n", s.acks);
    printf("Gresaka (ERROR):  %u\n", s.errors);
    printf("Vreme:            %.3f s\n", secs);
    printf("Upisa u sekundi:  %.0f\n", secs > 0 ? (double)s.acks / secs : 0.0);
    printf("Kasnjenje:        prosecno %.3f ms, najgorih 1%% od %.3f ms, najvece %.3f ms\n",
        avg, p99, maxL);
    if (!all) printf("UPOZORENJE: nisu stigli svi odgovori.\n");

    clientSetHandler(NULL, NULL);
    daFree(&s.latencies);
    return (all && s.errors == 0) ? 0 : 1;
}

// ---------------------------------------------------------------------------
// Otisak: za svako brojilo broj ocitavanja i zbir u Wh (za proveru seobe)
// ---------------------------------------------------------------------------

typedef struct {
    uint32_t  count;   // broj ocitavanja
    long long wh;      // zbir u Wh
    bool      answered;
    bool      error;
} MeterPrint;

typedef struct {
    uint32_t    first;
    uint32_t    n;
    MeterPrint* meters;
} Print;

static void printReplyFn(const RequestInfo* req, const MsgHeader* h,
    const char* body, double latencyMs, void* ctx) {
    (void)latencyMs;
    Print* p = (Print*)ctx;
    if (req->meterId < p->first || req->meterId - p->first >= p->n) return;
    MeterPrint* mp = &p->meters[req->meterId - p->first];
    mp->answered = true;
    if (h->type != MSG_QUERY_RESP) { mp->error = true; return; }
    const QueryRespHeader* q = (const QueryRespHeader*)body;
    const ReadingItem* it = (const ReadingItem*)(body + sizeof(QueryRespHeader));
    mp->count = q->count;
    mp->wh = 0;
    for (uint32_t i = 0; i < q->count; i++) mp->wh += toWh(it[i].kwh);
}

static int runPrint(uint32_t first, uint32_t count, const char* fileName) {
    Print p;
    p.first = first;
    p.n = count;
    p.meters = (MeterPrint*)calloc(count > 0 ? count : 1, sizeof(MeterPrint));
    if (p.meters == NULL) return 1;
    clientSetHandler(printReplyFn, &p);

    for (uint32_t m = 0; m < count && clientAlive(); m++)
        if (sendQuery(first + m) == 0) break;
    bool all = clientWaitAll(60000);

    FILE* f = stdout;
    if (fileName != NULL && fopen_s(&f, fileName, "w") != 0) {
        printf("Ne mogu da otvorim fajl %s.\n", fileName);
        f = stdout;
    }

    // Kontrolni zbir (FNV-1a) svih redova: dva otiska su ista ako im je isti i on.
    uint64_t hash = 1469598103934665603ull;
    uint64_t readings = 0;
    long long wh = 0;
    uint32_t errors = 0, withData = 0;
    for (uint32_t m = 0; m < count; m++) {
        MeterPrint* mp = &p.meters[m];
        if (!mp->answered || mp->error) { errors++; continue; }
        if (mp->count == 0) continue;
        fprintf(f, "%u %u %lld\n", first + m, mp->count, mp->wh);
        uint64_t parts[3] = { first + m, mp->count, (uint64_t)mp->wh };
        for (int i = 0; i < 3; i++) { hash ^= parts[i]; hash *= 1099511628211ull; }
        readings += mp->count;
        wh += mp->wh;
        withData++;
    }
    fprintf(f, "UKUPNO brojila=%u ocitavanja=%llu Wh=%lld kontrolni_zbir=%016llx\n",
        withData, (unsigned long long)readings, wh, (unsigned long long)hash);
    if (f != stdout) {
        fclose(f);
        printf("Otisak upisan u %s\n", fileName);
        printf("UKUPNO brojila=%u ocitavanja=%llu Wh=%lld kontrolni_zbir=%016llx\n",
            withData, (unsigned long long)readings, wh, (unsigned long long)hash);
    }
    if (errors > 0) printf("UPOZORENJE: %u brojila bez ispravnog odgovora.\n", errors);

    clientSetHandler(NULL, NULL);
    free(p.meters);
    return (all && errors == 0) ? 0 : 1;
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------

// Ctrl+C, Ctrl+Break ili zatvaranje prozora: prekini slanje i ugasi se uredno.
static BOOL WINAPI consoleHandler(DWORD type) {
    HANDLE stop = clientStopEvent();
    if (stop != NULL) SetEvent(stop);
    if (type == CTRL_CLOSE_EVENT) WaitForSingleObject(g_doneEvent, 4000);
    return TRUE;
}

static void usage(void) {
    printf("Upotreba:\n");
    printf("  User.exe                                   meni\n");
    printf("  User.exe test1                             funkcionalni test 1\n");
    printf("  User.exe stress <prvo> <broj> [poBrojilu]  stress test upisa\n");
    printf("  User.exe otisak <prvo> <broj> [fajl]       otisak brojila\n");
}

int main(int argc, char* argv[]) {
    // Provera argumenata pre povezivanja.
    const char* mode = argc > 1 ? argv[1] : "meni";
    uint32_t a = 0, b = 0, c = 1;
    bool argsOk = true;
    if (strcmp(mode, "stress") == 0)
        argsOk = argc >= 4 && parseUInt(argv[2], &a) && parseUInt(argv[3], &b) &&
            (argc < 5 || parseUInt(argv[4], &c)) && b > 0 && c > 0;
    else if (strcmp(mode, "otisak") == 0)
        argsOk = argc >= 4 && parseUInt(argv[2], &a) && parseUInt(argv[3], &b) && b > 0;
    else if (strcmp(mode, "meni") != 0 && strcmp(mode, "test1") != 0)
        argsOk = false;
    if (!argsOk) { usage(); return 1; }

    g_doneEvent = CreateEvent(NULL, TRUE, FALSE, NULL);
    if (g_doneEvent == NULL || !netInit()) {
        printf("[UR] Inicijalizacija nije uspela.\n");
        return 1;
    }

    int rc = 1;
    if (clientStart(LB_ADDRESS, LB_USER_PORT, printReply, NULL)) {
        SetConsoleCtrlHandler(consoleHandler, TRUE);
        printf("[UR] Povezan na LB (%s:%d).\n", LB_ADDRESS, LB_USER_PORT);

        if (strcmp(mode, "stress") == 0)      rc = runStress(a, b, c);
        else if (strcmp(mode, "otisak") == 0) rc = runPrint(a, b, argc > 4 ? argv[4] : NULL);
        else if (strcmp(mode, "test1") == 0)  rc = runTest1();
        else                                  rc = runMenu();

        // Uredno gasenje: sacekaj odgovore koji jos putuju, pa zatvori vezu.
        clientWaitAll(3000);
        clientStop();
        SetConsoleCtrlHandler(consoleHandler, FALSE);
    }

    netCleanup();
    printf("[UR] Kraj.\n");
    SetEvent(g_doneEvent);
    CloseHandle(g_doneEvent);
    return rc;
}
