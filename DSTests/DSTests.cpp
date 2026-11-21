// Testovi struktura podataka iz DSLib-a.
// Svaka struktura ima svoj fajl Test<Ime>.cpp i svoju funkciju test<Ime>().
//   Osoba 1: dinamicki niz, hash mapa
//   Osoba 2: kruzni bafer, FIFO lista (dodaje testRingBuffer i testFifoList)

#include "TestUtil.h"

int g_checks = 0;
int g_failed = 0;

int main() {
    printf("Testovi struktura podataka (DSLib)\n");

    testDynArray();
    testHashMap();
    testRingBuffer();

    printf("\nUkupno provera: %d, neuspelih: %d\n", g_checks, g_failed);
    printf(g_failed == 0 ? "SVI TESTOVI SU PROSLI\n" : "IMA GRESAKA\n");
    printf("Pritisni Enter za kraj...\n");
    getchar();
    return g_failed == 0 ? 0 : 1;
}
