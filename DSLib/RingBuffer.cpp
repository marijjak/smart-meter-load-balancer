#include "RingBuffer.h"
#include <stdlib.h>
#include <stdint.h>
#include <limits.h>

#define RB_DEFAULT_CAPACITY 16

// Duplira kapacitet i prepise elemente redom od pocetka novog niza (head = 0).
// Poziva se pod kriticnom sekcijom. Vraca false ako nema memorije; tada bafer
// ostaje kakav je bio.
static bool rbGrow(RingBuffer* rb) {
    size_t newCap = rb->capacity * 2;
    if (newCap < rb->capacity || newCap > SIZE_MAX / sizeof(void*)) return false;

    void** n = (void**)malloc(newCap * sizeof(void*));
    if (n == NULL) return false;

    for (size_t i = 0; i < rb->count; i++)
        n[i] = rb->items[(rb->head + i) % rb->capacity];

    free(rb->items);
    rb->items = n;
    rb->capacity = newCap;
    rb->head = 0;
    return true;
}

// Skida prvi element. Poziva se pod kriticnom sekcijom i samo kad red nije prazan.
static void* rbTakeFirst(RingBuffer* rb) {
    void* item = rb->items[rb->head];
    rb->items[rb->head] = NULL;
    rb->head = (rb->head + 1) % rb->capacity;
    rb->count--;
    return item;
}

bool rbInit(RingBuffer* rb, size_t initialCapacity) {
    if (initialCapacity == 0) initialCapacity = RB_DEFAULT_CAPACITY;
    rb->head = 0;
    rb->count = 0;
    rb->capacity = 0;
    rb->itemsSem = NULL;

    rb->items = (void**)malloc(initialCapacity * sizeof(void*));
    if (rb->items == NULL) return false;

    // Pocetna vrednost 0 (red je prazan), najveca LONG_MAX (bafer nema gornju granicu).
    rb->itemsSem = CreateSemaphore(NULL, 0, LONG_MAX, NULL);
    if (rb->itemsSem == NULL) {
        free(rb->items);
        rb->items = NULL;
        return false;
    }

    InitializeCriticalSection(&rb->cs);
    rb->capacity = initialCapacity;
    return true;
}

void rbFree(RingBuffer* rb) {
    if (rb->items == NULL) return;   
    DeleteCriticalSection(&rb->cs);
    CloseHandle(rb->itemsSem);
    free(rb->items);
    rb->items = NULL;
    rb->itemsSem = NULL;
    rb->capacity = 0;
    rb->head = 0;
    rb->count = 0;
}

bool rbPush(RingBuffer* rb, void* item) {
    if (item == NULL) return false;

    EnterCriticalSection(&rb->cs);
    if (rb->count == rb->capacity && !rbGrow(rb)) {
        LeaveCriticalSection(&rb->cs);
        return false;
    }
    rb->items[(rb->head + rb->count) % rb->capacity] = item;
    rb->count++;
    LeaveCriticalSection(&rb->cs);

    
    ReleaseSemaphore(rb->itemsSem, 1, NULL);
    return true;
}

void* rbPop(RingBuffer* rb, HANDLE stopEvent) {
    if (stopEvent != NULL) {
        HANDLE h[2] = { stopEvent, rb->itemsSem };
        DWORD r = WaitForMultipleObjects(2, h, FALSE, INFINITE);
        if (r != WAIT_OBJECT_0 + 1) return NULL;  
    }
    else {
        if (WaitForSingleObject(rb->itemsSem, INFINITE) != WAIT_OBJECT_0) return NULL;
    }

  
    EnterCriticalSection(&rb->cs);
    void* item = rbTakeFirst(rb);
    LeaveCriticalSection(&rb->cs);
    return item;
}

bool rbTryPop(RingBuffer* rb, void** out) {
    
    if (WaitForSingleObject(rb->itemsSem, 0) != WAIT_OBJECT_0) return false;

    EnterCriticalSection(&rb->cs);
    *out = rbTakeFirst(rb);
    LeaveCriticalSection(&rb->cs);
    return true;
}

size_t rbCount(RingBuffer* rb) {
    EnterCriticalSection(&rb->cs);
    size_t c = rb->count;
    LeaveCriticalSection(&rb->cs);
    return c;
}
