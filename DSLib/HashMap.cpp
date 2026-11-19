#include "HashMap.h"
#include <stdlib.h>

#define HM_DEFAULT_BUCKETS 16

// Multiplikativni hes (Knuth). Uzastopni brojevi brojila se ravnomerno
// rasporede po kantama.
static size_t hmIndex(uint32_t key, size_t bucketCount) {
    uint32_t h = key * 2654435761u;
    return (size_t)h % bucketCount;
}

// Nadje cvor sa datim kljucem, ili NULL.
static HashNode* hmFind(const HashMap* m, uint32_t key) {
    HashNode* n = m->buckets[hmIndex(key, m->bucketCount)];
    while (n != NULL && n->key != key) n = n->next;
    return n;
}

// Duplira broj kanti i prebaci postojece cvorove (bez nove alokacije cvorova).
// Ako nema memorije za nove kante, mapa ostaje kakva je bila (i dalje radi,
// samo su liste duze).
static void hmGrow(HashMap* m) {
    size_t newCount = m->bucketCount * 2;
    HashNode** nb = (HashNode**)calloc(newCount, sizeof(HashNode*));
    if (nb == NULL) return;

    for (size_t i = 0; i < m->bucketCount; i++) {
        HashNode* n = m->buckets[i];
        while (n != NULL) {
            HashNode* next = n->next;
            size_t j = hmIndex(n->key, newCount);
            n->next = nb[j];
            nb[j] = n;
            n = next;
        }
    }
    free(m->buckets);
    m->buckets = nb;
    m->bucketCount = newCount;
    m->resizeCount++;
}

bool hmInit(HashMap* m, size_t initialBuckets) {
    if (initialBuckets == 0) initialBuckets = HM_DEFAULT_BUCKETS;
    m->buckets = (HashNode**)calloc(initialBuckets, sizeof(HashNode*));
    m->bucketCount = m->buckets != NULL ? initialBuckets : 0;
    m->count = 0;
    m->resizeCount = 0;
    return m->buckets != NULL;
}

void hmFree(HashMap* m) {
    for (size_t i = 0; i < m->bucketCount; i++) {
        HashNode* n = m->buckets[i];
        while (n != NULL) {
            HashNode* next = n->next;
            free(n);
            n = next;
        }
    }
    free(m->buckets);
    m->buckets = NULL;
    m->bucketCount = 0;
    m->count = 0;
}

bool hmPut(HashMap* m, uint32_t key, void* value) {
    HashNode* n = hmFind(m, key);
    if (n != NULL) {           // kljuc vec postoji: samo zameni vrednost
        n->value = value;
        return true;
    }

    n = (HashNode*)malloc(sizeof(HashNode));
    if (n == NULL) return false;
    size_t i = hmIndex(key, m->bucketCount);
    n->key = key;
    n->value = value;
    n->next = m->buckets[i];   // novi cvor ide na pocetak liste
    m->buckets[i] = n;
    m->count++;

    // Popunjenost preko 75%: count / bucketCount > 3/4
    if (m->count * 4 > m->bucketCount * 3) hmGrow(m);
    return true;
}

void* hmGet(const HashMap* m, uint32_t key) {
    HashNode* n = hmFind(m, key);
    return n != NULL ? n->value : NULL;
}

bool hmContains(const HashMap* m, uint32_t key) {
    return hmFind(m, key) != NULL;
}

void* hmRemove(HashMap* m, uint32_t key) {
    HashNode** link = &m->buckets[hmIndex(key, m->bucketCount)];
    while (*link != NULL) {
        HashNode* n = *link;
        if (n->key == key) {
            void* value = n->value;
            *link = n->next;       // izbaci cvor iz liste
            free(n);
            m->count--;
            return value;
        }
        link = &n->next;
    }
    return NULL;
}

size_t hmCount(const HashMap* m) {
    return m->count;
}

void hmForEachRange(HashMap* m, size_t fromBucket, size_t toBucket,
    HmVisitFn fn, void* ctx) {
    if (toBucket > m->bucketCount) toBucket = m->bucketCount;
    for (size_t i = fromBucket; i < toBucket; i++) {
        HashNode* n = m->buckets[i];
        while (n != NULL) {
            HashNode* next = n->next;   // uzmi pre poziva: fn sme da obrise n
            if (!fn(n->key, n->value, ctx)) return;
            n = next;
        }
    }
}

size_t hmBucketCount(const HashMap* m) {
    return m->bucketCount;
}

uint32_t hmResizeCount(const HashMap* m) {
    return m->resizeCount;
}
