#pragma once
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <stddef.h>

// Prosirivi kruzni bafer (FIFO red pokazivaca), bezbedan za vise niti.
// Kad se napuni, kapacitet se duplira. Elemente alocira i oslobadja pozivalac.
// NULL se ne sme staviti u bafer (rbPop vraca NULL pri gasenju).

typedef struct {
    void**           items;     // elementi
    size_t           capacity;  // velicina niza items
    size_t           head;      // pozicija prvog elementa
    size_t           count;     // broj elemenata
    CRITICAL_SECTION cs;        // stiti sva polja iznad
    HANDLE           itemsSem;  // semafor: koliko elemenata ceka
} RingBuffer;

// Priprema prazan bafer (0 = podrazumevano, 16 mesta). Vraca false pri gresci.
bool   rbInit(RingBuffer* rb, size_t initialCapacity);

// Oslobadja bafer. Elemente NE oslobadja: pre toga ih pokupiti sa rbTryPop.
void   rbFree(RingBuffer* rb);

// Stavlja item na kraj reda i budi jednog potrosaca.
// Vraca false za NULL ili ako nema memorije.
bool   rbPush(RingBuffer* rb, void* item);

// Skida prvi element; ceka ako je red prazan. Vraca NULL kad je postavljen
// stopEvent (gasenje ima prednost). stopEvent sme biti NULL.
void* rbPop(RingBuffer* rb, HANDLE stopEvent);

// Skida prvi element bez cekanja. Vraca false ako je red prazan.
bool   rbTryPop(RingBuffer* rb, void** out);

// Trenutni broj elemenata.
size_t rbCount(RingBuffer* rb);
