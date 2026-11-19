#pragma once
#include <stddef.h>

// Dinamicki niz: elementi iste velicine, jedan za drugim u memoriji.
// Kad se napuni, kapacitet se duplira. Redosled elemenata se uvek cuva.
// Nije zasticen kriticnom sekcijom: ako ga koristi vise niti, stiti ga pozivalac.
//
// Koristi se u WR-u za ocitavanja jednog brojila: posto su elementi jedan za
// drugim, ceo niz moze odmah da se posalje kao odgovor na upit.

typedef struct {
    char* data;      // elementi, jedan za drugim
    size_t elemSize;  // velicina jednog elementa u bajtovima
    size_t count;     // broj elemenata u nizu
    size_t capacity;  // koliko elemenata stane bez prosirenja
} DynArray;

// Priprema prazan niz za elemente velicine elemSize.
// initialCapacity moze biti 0; tada se memorija zauzima pri prvom daPush.
// Vraca false ako je elemSize 0 ili nema memorije.
bool   daInit(DynArray* a, size_t elemSize, size_t initialCapacity);

// Oslobadja memoriju niza. Posle ovoga niz je prazan i moze ponovo daInit.
void   daFree(DynArray* a);

// Dodaje kopiju elementa (elemSize bajtova sa adrese elem) na kraj niza.
// Ako nema mesta, kapacitet se duplira. Vraca false ako nema memorije
// (tada niz ostaje kakav je bio).
bool   daPush(DynArray* a, const void* elem);

// Vraca pokazivac na element na poziciji index, ili NULL ako index ne postoji.
// Pokazivac vazi samo do sledeceg daPush (niz se tada moze premestiti).
void* daGet(const DynArray* a, size_t index);

// Brise element na poziciji index; elementi iza njega se pomeraju za jedno
// mesto napred, pa redosled ostaje isti. Vraca false ako index ne postoji.
bool   daRemoveAt(DynArray* a, size_t index);

// Brise sve elemente, ali zadrzava zauzetu memoriju za ponovno punjenje.
void   daClear(DynArray* a);

// Broj elemenata u nizu.
size_t daCount(const DynArray* a);
