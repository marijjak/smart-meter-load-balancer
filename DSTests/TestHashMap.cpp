#include "TestUtil.h"
#include "HashMap.h"
#include <stdint.h>

// Vrednost za kljuc k u testovima: neki pokazivac izveden iz kljuca.
static void* valFor(uint32_t k) {
    return (void*)(uintptr_t)(k * 2u + 1u);
}

// 100.000 umetanja, pretraga, provera da posle povecanja nista ne fali.
static void testManyKeys(void) {
    HashMap m;
    CHECK(hmInit(&m, 16), "hmInit sa 16 kanti");

    const uint32_t N = 100000;
    bool putOk = true;
    for (uint32_t k = 0; k < N; k++)
        if (!hmPut(&m, k, valFor(k))) putOk = false;
    CHECK(putOk, "100.000 x hmPut uspelo");
    CHECK(hmCount(&m) == N, "count je 100.000");
    CHECK(hmResizeCount(&m) > 0, "mapa se prosirila");
    CHECK(hmCount(&m) * 4 <= hmBucketCount(&m) * 3, "popunjenost nije preko 75%");

    bool getOk = true;
    for (uint32_t k = 0; k < N; k++)
        if (hmGet(&m, k) != valFor(k)) getOk = false;
    CHECK(getOk, "posle prosirenja svi kljucevi imaju tacnu vrednost");
    CHECK(hmGet(&m, N) == NULL && !hmContains(&m, N + 5), "nepostojeci kljuc: NULL / false");

    // Brisanje svakog drugog.
    bool remOk = true;
    for (uint32_t k = 0; k < N; k += 2)
        if (hmRemove(&m, k) != valFor(k)) remOk = false;
    CHECK(remOk, "hmRemove vraca tacnu vrednost");
    CHECK(hmCount(&m) == N / 2, "posle brisanja count je 50.000");

    bool restOk = true;
    for (uint32_t k = 0; k < N; k++) {
        bool shouldBe = (k % 2) == 1;
        if (hmContains(&m, k) != shouldBe) restOk = false;
    }
    CHECK(restOk, "obrisani kljucevi su nestali, ostali su tu");
    CHECK(hmRemove(&m, 0) == NULL, "ponovno brisanje vraca NULL");

    hmFree(&m);
    CHECK(hmCount(&m) == 0 && m.buckets == NULL, "hmFree isprazni mapu");
}

// Zamena vrednosti, NULL kao vrednost, veliki kljucevi i sudari.
static void testEdgeCases(void) {
    HashMap m;
    hmInit(&m, 0);
    CHECK(hmBucketCount(&m) == 16, "hmInit(0) daje 16 kanti");

    hmPut(&m, 7, valFor(7));
    hmPut(&m, 7, valFor(8));
    CHECK(hmCount(&m) == 1 && hmGet(&m, 7) == valFor(8), "isti kljuc: vrednost zamenjena, count 1");

    hmPut(&m, 9, NULL);
    CHECK(hmContains(&m, 9) && hmGet(&m, 9) == NULL, "NULL vrednost: hmContains je true");

    hmPut(&m, 0xFFFFFFFFu, valFor(1));
    CHECK(hmGet(&m, 0xFFFFFFFFu) == valFor(1), "najveci uint32 kljuc");
    hmFree(&m);

    // Mapa sa jednom kantom: sve ide u istu listu dok se ne prosiri.
    hmInit(&m, 1);
    for (uint32_t k = 1; k <= 50; k++) hmPut(&m, k * 1000, valFor(k));
    bool ok = hmCount(&m) == 50;
    for (uint32_t k = 1; k <= 50; k++) if (hmGet(&m, k * 1000) != valFor(k)) ok = false;
    CHECK(ok, "pocetak sa 1 kantom: sudari i prosirenja rade");

    hmFree(&m);

    // Brisanje iz sredine liste (cvor koji nije prvi u kanti).
    // Sa 1024 kanti kljucevi koji se razlikuju za 1024 padaju u istu kantu,
    // jer je hes konstanta neparna (donjih 10 bita kljuca odredi kantu).
    hmInit(&m, 1024);
    uint32_t same[3] = { 5, 5 + 1024, 5 + 2048 };
    for (int i = 0; i < 3; i++) hmPut(&m, same[i], valFor(same[i]));
    size_t inBucket = 0;
    for (size_t i = 0; i < hmBucketCount(&m); i++) {
        size_t len = 0;
        for (HashNode* n = m.buckets[i]; n != NULL; n = n->next) len++;
        if (len > inBucket) inBucket = len;
    }
    CHECK(inBucket == 3, "tri kljuca su u istoj kanti (lista duzine 3)");
    CHECK(hmRemove(&m, same[1]) == valFor(same[1]), "brisanje srednjeg u listi");
    CHECK(hmGet(&m, same[0]) == valFor(same[0]) && hmGet(&m, same[2]) == valFor(same[2]),
        "prvi i poslednji u listi su ostali");
    hmFree(&m);
}

typedef struct {
    size_t   visited;
    uint64_t keySum;
    uint32_t stopAfter;   // 0 = ne prekidaj
    bool     removeOdd;
    HashMap* map;
} VisitCtx;

static bool visit(uint32_t key, void* value, void* p) {
    VisitCtx* c = (VisitCtx*)p;
    c->visited++;
    c->keySum += key;
    if (value != valFor(key)) c->keySum = 0;   // pogresna vrednost pokvari zbir
    if (c->removeOdd && key % 2 == 1) hmRemove(c->map, key);
    return c->stopAfter == 0 || c->visited < c->stopAfter;
}

// Obilazak u delovima po 256 kanti (kao TSEOBA), prekid i brisanje u toku obilaska.
static void testForEachRange(void) {
    HashMap m;
    hmInit(&m, 16);
    const uint32_t N = 10000;
    uint64_t expectedSum = 0;
    for (uint32_t k = 0; k < N; k++) {
        hmPut(&m, k, valFor(k));
        expectedSum += k;
    }

    VisitCtx c = { 0, 0, 0, false, &m };
    size_t chunks = 0;
    for (size_t from = 0; from < hmBucketCount(&m); from += 256, chunks++)
        hmForEachRange(&m, from, from + 256, visit, &c);
    CHECK(c.visited == N && c.keySum == expectedSum, "obilazak po 256 kanti obidje svaki par tacno jednom");
    CHECK(chunks > 1, "obilazak je stvarno isao u vise delova");

    VisitCtx s = { 0, 0, 10, false, &m };
    hmForEachRange(&m, 0, hmBucketCount(&m), visit, &s);
    CHECK(s.visited == 10, "fn vrati false: obilazak staje");

    VisitCtx r = { 0, 0, 0, true, &m };
    hmForEachRange(&m, 0, (size_t)-1, visit, &r);
    CHECK(r.visited == N, "toBucket preko kraja: obidje sve");
    CHECK(hmCount(&m) == N / 2 && !hmContains(&m, 1) && hmContains(&m, 2),
        "brisanje trenutnog kljuca u toku obilaska radi");

    uint32_t before = hmResizeCount(&m);
    for (uint32_t k = N; k < 4 * N; k++) hmPut(&m, k, valFor(k));
    CHECK(hmResizeCount(&m) > before, "hmResizeCount se poveca pri prosirenju");

    hmFree(&m);
}

void testHashMap(void) {
    printf("\nHash mapa:\n");
    testManyKeys();
    testEdgeCases();
    testForEachRange();
}
