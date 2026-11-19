#include "TestUtil.h"
#include "DynArray.h"
#include <stdint.h>

// Element kao u WR-u: jedno ocitavanje (vreme + kWh).
typedef struct {
    int64_t timestamp;
    float   kwh;
} TestItem;

// Rast od 0 do 1.000.000 elemenata: svi su tu i redosled je isti.
static void testGrowth(void) {
    DynArray a;
    CHECK(daInit(&a, sizeof(TestItem), 0), "daInit sa kapacitetom 0");
    CHECK(daCount(&a) == 0 && daGet(&a, 0) == NULL, "prazan niz: count 0, daGet(0) je NULL");

    const size_t N = 1000000;
    bool pushOk = true;
    for (size_t i = 0; i < N; i++) {
        TestItem it = { (int64_t)i, (float)i * 0.5f };
        if (!daPush(&a, &it)) pushOk = false;
    }
    CHECK(pushOk, "1.000.000 x daPush uspelo");
    CHECK(daCount(&a) == N, "count je 1.000.000");
    CHECK(a.capacity >= N && a.capacity < 2 * N + 4, "kapacitet raste duplo (nije preveliki)");

    bool orderOk = true;
    for (size_t i = 0; i < N; i++) {
        TestItem* it = (TestItem*)daGet(&a, i);
        if (it == NULL || it->timestamp != (int64_t)i || it->kwh != (float)i * 0.5f) {
            orderOk = false;
            break;
        }
    }
    CHECK(orderOk, "svi elementi su na svom mestu, redosled isti");
    CHECK(daGet(&a, N) == NULL, "daGet van opsega vraca NULL");

    // Elementi su jedan za drugim: data je ceo niz (tako WR salje odgovor).
    TestItem* raw = (TestItem*)a.data;
    CHECK(raw[12345].timestamp == 12345, "data pokazuje na niz elemenata jedan za drugim");

    daFree(&a);
    CHECK(a.data == NULL && daCount(&a) == 0, "daFree oslobodi i isprazni niz");
}

// Brisanje sa pocetka, iz sredine i sa kraja; redosled ostalih ostaje isti.
static void testRemove(void) {
    DynArray a;
    daInit(&a, sizeof(int), 2);
    for (int i = 0; i < 10; i++) daPush(&a, &i);   // 0 1 2 3 4 5 6 7 8 9

    CHECK(daRemoveAt(&a, 0), "brisanje prvog");    // 1 2 3 4 5 6 7 8 9
    CHECK(daRemoveAt(&a, 4), "brisanje iz sredine"); // 1 2 3 4 6 7 8 9
    CHECK(daRemoveAt(&a, daCount(&a) - 1), "brisanje poslednjeg"); // 1 2 3 4 6 7 8
    CHECK(!daRemoveAt(&a, 100), "brisanje van opsega vraca false");

    int expected[] = { 1, 2, 3, 4, 6, 7, 8 };
    bool ok = daCount(&a) == 7;
    for (size_t i = 0; ok && i < 7; i++) ok = *(int*)daGet(&a, i) == expected[i];
    CHECK(ok, "posle brisanja: 1 2 3 4 6 7 8");

    // Brisanje svega jedan po jedan.
    while (daCount(&a) > 0) daRemoveAt(&a, 0);
    CHECK(daCount(&a) == 0, "obrisani svi elementi");

    daFree(&a);
}

// daClear zadrzava memoriju; niz moze ponovo da se puni.
static void testClear(void) {
    DynArray a;
    daInit(&a, sizeof(int), 0);
    for (int i = 0; i < 100; i++) daPush(&a, &i);
    size_t cap = a.capacity;

    daClear(&a);
    CHECK(daCount(&a) == 0 && a.capacity == cap, "daClear: count 0, kapacitet isti");

    int x = 42;
    daPush(&a, &x);
    CHECK(daCount(&a) == 1 && *(int*)daGet(&a, 0) == 42, "ponovno punjenje posle daClear");

    daFree(&a);
    CHECK(!daInit(&a, 0, 10), "daInit sa velicinom elementa 0 vraca false");
}

void testDynArray(void) {
    printf("\nDinamicki niz:\n");
    testGrowth();
    testRemove();
    testClear();
}
