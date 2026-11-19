#include "DynArray.h"
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#define DA_FIRST_CAPACITY 4   // kapacitet pri prvom daPush ako je pocetni 0

// Prosiri niz tako da stane bar jos jedan element (kapacitet x2).
static bool daGrow(DynArray* a) {
    size_t newCap = a->capacity == 0 ? DA_FIRST_CAPACITY : a->capacity * 2;
    if (newCap < a->capacity || newCap > SIZE_MAX / a->elemSize) return false;  // prelivanje

    char* p = (char*)realloc(a->data, newCap * a->elemSize);
    if (p == NULL) return false;   // stari niz ostaje netaknut
    a->data = p;
    a->capacity = newCap;
    return true;
}

bool daInit(DynArray* a, size_t elemSize, size_t initialCapacity) {
    a->data = NULL;
    a->elemSize = elemSize;
    a->count = 0;
    a->capacity = 0;
    if (elemSize == 0) return false;

    if (initialCapacity > 0) {
        if (initialCapacity > SIZE_MAX / elemSize) return false;
        a->data = (char*)malloc(initialCapacity * elemSize);
        if (a->data == NULL) return false;
        a->capacity = initialCapacity;
    }
    return true;
}

void daFree(DynArray* a) {
    free(a->data);
    a->data = NULL;
    a->count = 0;
    a->capacity = 0;
}

bool daPush(DynArray* a, const void* elem) {
    if (a->count == a->capacity && !daGrow(a)) return false;
    memcpy(a->data + a->count * a->elemSize, elem, a->elemSize);
    a->count++;
    return true;
}

void* daGet(const DynArray* a, size_t index) {
    if (index >= a->count) return NULL;
    return a->data + index * a->elemSize;
}

bool daRemoveAt(DynArray* a, size_t index) {
    if (index >= a->count) return false;
    size_t after = a->count - index - 1;   // koliko elemenata ima iza
    if (after > 0) {
        memmove(a->data + index * a->elemSize,
            a->data + (index + 1) * a->elemSize,
            after * a->elemSize);
    }
    a->count--;
    return true;
}

void daClear(DynArray* a) {
    a->count = 0;
}

size_t daCount(const DynArray* a) {
    return a->count;
}
