#pragma once
#include <stdio.h>

// Zajednicko za sve testove struktura: CHECK ispise rezultat i broji greske.

extern int g_checks;   // ukupno provera
extern int g_failed;   // neuspelih provera

#define CHECK(cond, what)                                              \
    do {                                                               \
        g_checks++;                                                    \
        if (cond) printf("  [ OK ] %s\n", what);                       \
        else { g_failed++; printf("  [FAIL] %s  (%s:%d)\n", what, __FILE__, __LINE__); } \
    } while (0)

// Testovi pojedinih struktura (svaka u svom .cpp fajlu).
void testDynArray(void);
void testHashMap(void);
void testRingBuffer(void);
