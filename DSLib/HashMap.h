#pragma once
#include <stddef.h>
#include <stdint.h>


typedef struct HashNode {
    uint32_t         key;
    void* value;
    struct HashNode* next;
} HashNode;

typedef struct {
    HashNode** buckets;      // niz kanti
    size_t     bucketCount;  // broj kanti
    size_t     count;        // broj parova u mapi
    uint32_t   resizeCount;  // koliko puta je mapa prosirena
} HashMap;

typedef bool (*HmVisitFn)(uint32_t key, void* value, void* ctx);

bool     hmInit(HashMap* m, size_t initialBuckets);

void     hmFree(HashMap* m);

bool     hmPut(HashMap* m, uint32_t key, void* value);

void* hmGet(const HashMap* m, uint32_t key);
bool     hmContains(const HashMap* m, uint32_t key);

void* hmRemove(HashMap* m, uint32_t key);
size_t   hmCount(const HashMap* m);

void     hmForEachRange(HashMap* m, size_t fromBucket, size_t toBucket,
    HmVisitFn fn, void* ctx);
size_t   hmBucketCount(const HashMap* m);

uint32_t hmResizeCount(const HashMap* m);