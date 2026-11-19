#pragma once
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <stddef.h>


typedef struct {
    void** items;     // elementi
    size_t           capacity;  // velicina niza items
    size_t           head;      // pozicija prvog elementa
    size_t           count;     // broj elemenata
    CRITICAL_SECTION cs;        // stiti sva polja iznad
    HANDLE           itemsSem;  // semafor: koliko elemenata ceka
} RingBuffer;

bool   rbInit(RingBuffer* rb, size_t initialCapacity);

void   rbFree(RingBuffer* rb);

bool   rbPush(RingBuffer* rb, void* item);

void* rbPop(RingBuffer* rb, HANDLE stopEvent);

bool   rbTryPop(RingBuffer* rb, void** out);
size_t rbCount(RingBuffer* rb);