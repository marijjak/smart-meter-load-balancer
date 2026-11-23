// Load Balancer (LB): prima zahteve korisnika i raspodeljuje ih Worker-ima.
//
//   LoadBalancer.exe <N>        N = broj Worker-a (1 .. MAX_WORKERS)
//
// Glavna nit: pokrece sve niti, konzola (s = stanje, q = gasenje), gasenje.
// Deo Osobe 1 (korisnici) je u Users.cpp i Pending.cpp,
// deo Osobe 2 (radnici i mapa brojila) u Workers.cpp i MeterMap.cpp.

#include "Users.h"
#include "MeterMap.h"
#include <stdio.h>
#include <stdlib.h>
#include <conio.h>

// Zajednicke promenljive iz Dogovor.h.
uint32_t g_workerCount = 0;
HANDLE   g_stopEvent = NULL;

static HANDLE g_doneEvent = NULL;   // postavljen kad je gasenje zavrseno (za zatvaranje prozora)

// Ctrl+C, Ctrl+Break ili zatvaranje prozora: uredno gasenje.
static BOOL WINAPI consoleHandler(DWORD type) {
    SetEvent(g_stopEvent);
    if (type == CTRL_CLOSE_EVENT) {
        // Windows ubija proces cim se handler vrati: sacekaj da main zavrsi gasenje.
        WaitForSingleObject(g_doneEvent, 4000);
    }
    return TRUE;
}

// Ispise stanje cele LB (komanda s i kraj rada).
static void printStats(void) {
    usersPrintStats();
    printf("[LB] Najduze drzanje sekcije mape brojila: %.3f ms\n", meterMapMaxHoldMs());
}

// Konzola dok ne stigne g_stopEvent: s = stanje, q = gasenje.
static void consoleLoop(void) {
    printf("Komande: s = stanje, q = gasenje. Ctrl+C = gasenje.\n");
    while (WaitForSingleObject(g_stopEvent, 100) == WAIT_TIMEOUT) {
        if (!_kbhit()) continue;
        int c = _getch();
        if (c == 's' || c == 'S') printStats();
        else if (c == 'q' || c == 'Q') break;
    }
}

int main(int argc, char* argv[]) {
    if (argc < 2) {
        printf("Upotreba: LoadBalancer.exe <N>   (N = broj Worker-a, 1 .. %d)\n", MAX_WORKERS);
        return 1;
    }
    char* end;
    unsigned long n = strtoul(argv[1], &end, 10);
    if (*end != '\0' || n == 0 || n > MAX_WORKERS) {
        printf("Neispravan N \"%s\" (dozvoljeno 1 .. %d).\n", argv[1], MAX_WORKERS);
        return 1;
    }
    g_workerCount = (uint32_t)n;

    g_stopEvent = CreateEvent(NULL, TRUE, FALSE, NULL);
    g_doneEvent = CreateEvent(NULL, TRUE, FALSE, NULL);
    bool netOk = netInit();
    bool workersOk = false;
    bool usersOk = false;
    int exitCode = 1;

    if (g_stopEvent == NULL || g_doneEvent == NULL || !netOk) {
        printf("[LB] Inicijalizacija nije uspela.\n");
        goto cleanup;
    }
    SetConsoleCtrlHandler(consoleHandler, TRUE);
    printf("[LB] N = %u\n", g_workerCount);

    // Prvo radnici (tabela mora da postoji pre nego sto TPRIH primi prvog WR-a),
    // pa korisnici.
    workersOk = workersStart();
    if (workersOk) usersOk = usersStart();
    if (workersOk && usersOk) {
        consoleLoop();
        exitCode = 0;
    }

    // Gasenje, redom:
    //  1. stopEvent: sve niti zavrsavaju posao
    //  2. TPRIH, TCIT, TRAS: posle njih niko vise ne zove meterRoute
    //  3. deo Osobe 2 (TSL, TPR, TSEOBA): posle njega niko vise ne zove pendingComplete
    //  4. TODG i oslobadjanje redova, sesija i zahteva na cekanju
    printf("[LB] Gasim se...\n");
    SetEvent(g_stopEvent);
    if (usersOk) usersStopInput();
    if (workersOk) workersShutdown();
    if (usersOk) {
        printStats();
        usersShutdown();
    }

cleanup:
    if (netOk) netCleanup();
    printf("[LB] Ugasen.\n");
    if (g_doneEvent != NULL) SetEvent(g_doneEvent);   // pusta consoleHandler ako ceka
    SetConsoleCtrlHandler(consoleHandler, FALSE);
    if (g_stopEvent != NULL) CloseHandle(g_stopEvent);
    if (g_doneEvent != NULL) CloseHandle(g_doneEvent);
    return exitCode;
}
