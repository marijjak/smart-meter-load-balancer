#include "TestUtil.h"
#include "RingBuffer.h"
#include <stdint.h>
#include <stdlib.h>

// Broj -> pokazivac i nazad (+1 jer NULL nije dozvoljen).
static void* itemFor(size_t v) {
    return (void*)(uintptr_t)(v + 1);
}
static size_t valueOf(void* item) {
    return (size_t)(uintptr_t)item - 1;
}

// FIFO redosled, prazan red, NULL element.
static void testBasic(void) {
    RingBuffer rb;
    CHECK(rbInit(&rb, 4), "rbInit sa 4 mesta");
    CHECK(rbCount(&rb) == 0, "nov bafer je prazan");

    void* out = itemFor(999);
    CHECK(!rbTryPop(&rb, &out) && out == itemFor(999), "rbTryPop na praznom vraca false i ne dira *out");
    CHECK(!rbPush(&rb, NULL), "NULL ne moze da se stavi u bafer");

    bool pushOk = true;
    for (size_t i = 0; i < 3; i++)
        if (!rbPush(&rb, itemFor(i))) pushOk = false;
    CHECK(pushOk && rbCount(&rb) == 3, "3 x rbPush, count 3");

    bool orderOk = true;
    for (size_t i = 0; i < 3; i++)
        if (rbPop(&rb, NULL) != itemFor(i)) orderOk = false;
    CHECK(orderOk, "rbPop vraca elemente redom kojim su stavljeni");
    CHECK(rbCount(&rb) == 0, "posle skidanja bafer je prazan");

    rbFree(&rb);
    CHECK(rb.items == NULL && rb.itemsSem == NULL, "rbFree oslobodi niz i semafor");
}

// Prosirenje kad red ide preko kraja niza; mesano stavljanje i skidanje.
static void testGrowWrapAround(void) {
    RingBuffer rb;
    rbInit(&rb, 4);

    for (size_t i = 0; i < 3; i++) rbPush(&rb, itemFor(i));
    for (size_t i = 0; i < 3; i++) rbPop(&rb, NULL);

    for (size_t i = 10; i < 15; i++) rbPush(&rb, itemFor(i));
    CHECK(rb.capacity == 8, "peti element duplira kapacitet sa 4 na 8");

    bool orderOk = true;
    for (size_t i = 10; i < 15; i++)
        if (rbPop(&rb, NULL) != itemFor(i)) orderOk = false;
    CHECK(orderOk, "posle prosirenja preko ivice redosled je isti");

    size_t next = 0, expect = 0;
    bool mixOk = true;
    for (int round = 0; round < 1000; round++) {
        for (int k = 0; k < 7; k++) rbPush(&rb, itemFor(next++));
        for (int k = 0; k < 5; k++)
            if (valueOf(rbPop(&rb, NULL)) != expect++) mixOk = false;
    }
    CHECK(rbCount(&rb) == next - expect, "count prati broj elemenata posle 12.000 operacija");

    void* item;
    while (rbTryPop(&rb, &item))
        if (valueOf(item) != expect++) mixOk = false;
    CHECK(mixOk && expect == next, "mesano stavljanje/skidanje sa prosirenjima: sve redom");

    rbFree(&rb);
}

// Gasenje i budjenje: nit ceka u rbPop dok glavna nit stavi element ili postavi stopEvent.
typedef struct {
    RingBuffer* rb;
    HANDLE      stop;
    void*       result;
} PopArgs;

// Nit koja jednom pozove rbPop i zapamti rezultat.
static DWORD WINAPI popThread(LPVOID p) {
    PopArgs* a = (PopArgs*)p;
    a->result = rbPop(a->rb, a->stop);
    return 0;
}

// rbPush budi nit u rbPop; stopEvent je budi i ima prednost nad elementima.
static void testStopAndWake(void) {
    RingBuffer rb;
    rbInit(&rb, 0);
    CHECK(rb.capacity == 16, "rbInit(0) daje 16 mesta");
    HANDLE stop = CreateEvent(NULL, TRUE, FALSE, NULL);

    // 1) Nit ceka na praznom redu, druga nit stavi element: mora da se probudi.
    PopArgs a = { &rb, stop, NULL };
    HANDLE t = CreateThread(NULL, 0, popThread, &a, 0, NULL);
    Sleep(50);
    rbPush(&rb, itemFor(42));
    bool woke = WaitForSingleObject(t, 2000) == WAIT_OBJECT_0;
    CloseHandle(t);
    CHECK(woke && a.result == itemFor(42), "rbPush budi nit koja ceka u rbPop");

    // 2) Nit ceka na praznom redu, postavi se stopEvent: rbPop vraca NULL.
    a.result = itemFor(1);
    t = CreateThread(NULL, 0, popThread, &a, 0, NULL);
    Sleep(50);
    SetEvent(stop);
    woke = WaitForSingleObject(t, 2000) == WAIT_OBJECT_0;
    CloseHandle(t);
    CHECK(woke && a.result == NULL, "stopEvent budi nit u rbPop, vraca NULL");

    // 3) stopEvent postavljen, a u redu ima elemenata: gasenje ima prednost,
    //    elementi ostaju za rbTryPop.
    rbPush(&rb, itemFor(7));
    CHECK(rbPop(&rb, stop) == NULL, "kad je stopEvent postavljen, rbPop vraca NULL i kad red nije prazan");
    void* item = NULL;
    CHECK(rbTryPop(&rb, &item) && item == itemFor(7), "element ostaje i pokupi se sa rbTryPop");

    CloseHandle(stop);
    rbFree(&rb);
}

// 4 proizvodjaca i 4 potrosaca: svaki element stize tacno jednom,
// a elementi istog proizvodjaca kod jednog potrosaca stizu redom.
#define MT_PRODUCERS 4
#define MT_CONSUMERS 4
#define MT_PER_PRODUCER 50000
#define MT_TOTAL (MT_PRODUCERS * MT_PER_PRODUCER)

typedef struct {
    RingBuffer*    rb;
    HANDLE         stop;
    int            id;
    volatile LONG* seen;      // seen[v] = koliko puta je primljena vrednost v
    volatile LONG* consumed;  // ukupno primljenih (svi potrosaci)
    bool           orderOk;   // potrosac: redosled po proizvodjacu je ispravan
    bool           pushOk;    // proizvodjac: svi rbPush su uspeli
} MtArgs;

// Proizvodjac: stavi MT_PER_PRODUCER svojih vrednosti redom.
static DWORD WINAPI producerThread(LPVOID p) {
    MtArgs* a = (MtArgs*)p;
    a->pushOk = true;
    for (size_t i = 0; i < MT_PER_PRODUCER; i++) {
        if (!rbPush(a->rb, itemFor((size_t)a->id * MT_PER_PRODUCER + i))) a->pushOk = false;
    }
    return 0;
}

// Potrosac: skida dok se ne postavi stopEvent, broji i proverava redosled.
static DWORD WINAPI consumerThread(LPVOID p) {
    MtArgs* a = (MtArgs*)p;
    long long last[MT_PRODUCERS];
    for (int i = 0; i < MT_PRODUCERS; i++) last[i] = -1;
    a->orderOk = true;

    for (;;) {
        void* item = rbPop(a->rb, a->stop);
        if (item == NULL) break;  
        size_t v = valueOf(item);
        if (v >= MT_TOTAL) { a->orderOk = false; continue; }

        int prod = (int)(v / MT_PER_PRODUCER);
        long long seq = (long long)(v % MT_PER_PRODUCER);
        if (seq <= last[prod]) a->orderOk = false;
        last[prod] = seq;

        InterlockedIncrement(&a->seen[v]);
        InterlockedIncrement(a->consumed);
    }
    return 0;
}

// Atomsko citanje brojaca koji druge niti povecavaju.
static LONG readCounter(volatile LONG* p) {
    return InterlockedCompareExchange(p, 0, 0);
}

// Pokrece niti, ceka da se sve primi, gasi potrosace i proverava rezultate.
static void testMultiThreaded(void) {
    RingBuffer rb;
    rbInit(&rb, 2);
    HANDLE stop = CreateEvent(NULL, TRUE, FALSE, NULL);
    volatile LONG* seen = (volatile LONG*)calloc(MT_TOTAL, sizeof(LONG));
    volatile LONG consumed = 0;

    MtArgs prod[MT_PRODUCERS], cons[MT_CONSUMERS];
    HANDLE pt[MT_PRODUCERS], ct[MT_CONSUMERS];

    for (int i = 0; i < MT_CONSUMERS; i++) {
        cons[i] = MtArgs{ &rb, stop, i, seen, &consumed, true, true };
        ct[i] = CreateThread(NULL, 0, consumerThread, &cons[i], 0, NULL);
    }
    for (int i = 0; i < MT_PRODUCERS; i++) {
        prod[i] = MtArgs{ &rb, stop, i, seen, &consumed, true, true };
        pt[i] = CreateThread(NULL, 0, producerThread, &prod[i], 0, NULL);
    }

    bool prodDone = WaitForMultipleObjects(MT_PRODUCERS, pt, TRUE, 30000) == WAIT_OBJECT_0;
    CHECK(prodDone, "4 proizvodjaca zavrsila (4 x 50.000 elemenata)");

    for (int ms = 0; readCounter(&consumed) < MT_TOTAL && ms < 30000; ms += 10) Sleep(10);
    CHECK(readCounter(&consumed) == MT_TOTAL, "potrosaci su primili svih 200.000 elemenata");

    SetEvent(stop);
    bool consDone = WaitForMultipleObjects(MT_CONSUMERS, ct, TRUE, 5000) == WAIT_OBJECT_0;
    CHECK(consDone, "posle stopEvent sva 4 potrosaca su se sama zavrsila");

    bool pushOk = true, orderOk = true, onceOk = true;
    for (int i = 0; i < MT_PRODUCERS; i++) pushOk = pushOk && prod[i].pushOk;
    for (int i = 0; i < MT_CONSUMERS; i++) orderOk = orderOk && cons[i].orderOk;
    for (size_t v = 0; v < MT_TOTAL; v++) if (readCounter(&seen[v]) != 1) onceOk = false;
    CHECK(pushOk, "svi rbPush su uspeli");
    CHECK(onceOk, "svaki element je primljen tacno jednom (nista izgubljeno ni duplirano)");
    CHECK(orderOk, "kod svakog potrosaca elementi istog proizvodjaca stizu redom");
    CHECK(rbCount(&rb) == 0, "bafer je na kraju prazan");

    for (int i = 0; i < MT_PRODUCERS; i++) CloseHandle(pt[i]);
    for (int i = 0; i < MT_CONSUMERS; i++) CloseHandle(ct[i]);
    CloseHandle(stop);
    free((void*)seen);
    rbFree(&rb);
}

// Svi testovi kruznog bafera (poziva ih DSTests.cpp).
void testRingBuffer(void) {
    printf("\nKruzni bafer:\n");
    testBasic();
    testGrowWrapAround();
    testStopAndWake();
    testMultiThreaded();
}
