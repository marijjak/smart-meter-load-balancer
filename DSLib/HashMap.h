#pragma once
#include <stddef.h>
#include <stdint.h>

// Hash mapa: kljuc uint32 -> pokazivac na vrednost.
// Sudari se resavaju ulancavanjem (svaka kanta je ulancana lista).
// Kad broj parova predje 75% broja kanti, broj kanti se duplira.
// Mapa cuva samo pokazivace: vrednosti alocira i oslobadja pozivalac.
// Nije zasticena kriticnom sekcijom: ako je koristi vise niti, stiti je pozivalac.
//
// Koristi se u LB-u (brojilo -> vlasnik, lbId -> zahtev na cekanju)
// i u WR-u (brojilo -> niz ocitavanja).

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

// Funkcija koju hmForEachRange zove za svaki par.
// Vraca true da se nastavi, false da se obilazak prekine.
typedef bool (*HmVisitFn)(uint32_t key, void* value, void* ctx);

// Priprema praznu mapu sa initialBuckets kanti (0 znaci podrazumevano, 16).
// Vraca false ako nema memorije.
bool     hmInit(HashMap* m, size_t initialBuckets);

// Oslobadja kante i cvorove. Vrednosti NE oslobadja: to radi pozivalac pre
// ovoga (npr. preko hmForEachRange).
void     hmFree(HashMap* m);

// Upisuje par key -> value. Ako kljuc vec postoji, vrednost se zameni
// (stara se NE oslobadja; ako treba, pozivalac je prvo uzme sa hmGet).
// Moze da prosiri mapu. Vraca false ako nema memorije (mapa ostaje ista).
bool     hmPut(HashMap* m, uint32_t key, void* value);

// Vraca vrednost za kljuc, ili NULL ako kljuca nema.
void* hmGet(const HashMap* m, uint32_t key);

// Da li kljuc postoji (korisno ako vrednost moze biti NULL).
bool     hmContains(const HashMap* m, uint32_t key);

// Izbacuje kljuc iz mape i vraca njegovu vrednost, ili NULL ako ga nema.
void* hmRemove(HashMap* m, uint32_t key);

// Broj parova u mapi.
size_t   hmCount(const HashMap* m);

// Obilazi kante od fromBucket do toBucket (toBucket se ne racuna) i za svaki
// par zove fn. Ovo omogucava obilazak u delovima (npr. po 256 kanti), sa
// otkljucavanjem kriticne sekcije izmedju delova.
// Unutar fn sme da se pozove hmRemove za TRENUTNI kljuc, ali ne i hmPut
// (moze da prosiri mapu i promeni raspored kanti).
// toBucket veci od broja kanti se tretira kao broj kanti.
void     hmForEachRange(HashMap* m, size_t fromBucket, size_t toBucket,
    HmVisitFn fn, void* ctx);

// Trenutni broj kanti (za obilazak u delovima).
size_t   hmBucketCount(const HashMap* m);

// Koliko puta je mapa prosirena. Ako se promeni izmedju dva dela obilaska,
// raspored kanti je drugaciji i obilazak treba poceti ispocetka.
uint32_t hmResizeCount(const HashMap* m);
