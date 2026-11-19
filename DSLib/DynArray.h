#pragma once
#include <stddef.h>


typedef struct {
    char* data;      // elementi, jedan za drugim
    size_t elemSize;  // velicina jednog elementa u bajtovima
    size_t count;     // broj elemenata u nizu
    size_t capacity;  // koliko elemenata stane bez prosirenja
} DynArray;


bool   daInit(DynArray* a, size_t elemSize, size_t initialCapacity);

void   daFree(DynArray* a);

bool   daPush(DynArray* a, const void* elem);

void* daGet(const DynArray* a, size_t index);

bool   daRemoveAt(DynArray* a, size_t index);

void   daClear(DynArray* a);

size_t daCount(const DynArray* a);