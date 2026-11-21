#pragma once
#include <stddef.h>
#include <stdint.h>
#include "Common.h"
#include "HashMap.h"
#include "DynArray.h"

// Skladiste Worker-a: brojilo -> niz ocitavanja (ReadingItem, redom kojim su stigla).
// Niz je ReadingItem, isti oblik kao u QUERY_RESP, pa se na upit salje odmah.
// Koristi ga samo nit TOBR, pa nije zasticeno kriticnom sekcijom.

typedef struct {
    HashMap map;       // meterId -> DynArray* (elementi ReadingItem)
    size_t  readings;  // ukupan broj ocitavanja svih brojila
} Storage;

// Funkcija koju stForEach zove za svako brojilo. Vraca false da se obilazak prekine.
typedef bool (*StVisitFn)(uint32_t meterId, const DynArray* items, void* ctx);

// Priprema prazno skladiste. Vraca false ako nema memorije.
bool   stInit(Storage* st);

// Oslobadja sva brojila i njihove nizove.
void   stFree(Storage* st);

// Dodaje ocitavanje na kraj niza tog brojila (pravi brojilo ako ga nema).
// Koristi se za WRITE i za podatke koji stignu seobom. Vraca false ako nema memorije.
bool   stAdd(Storage* st, const Reading* r);

// Niz ocitavanja brojila, ili NULL ako brojila nema.
const DynArray* stGet(Storage* st, uint32_t meterId);

// Brise brojilo i njegova ocitavanja (posle predaje drugom WR-u).
// Vraca broj obrisanih ocitavanja (0 ako brojila nije bilo).
size_t stRemove(Storage* st, uint32_t meterId);

// Brise sva brojila; skladiste ostaje spremno za dalji rad.
void   stClear(Storage* st);

// Za svako brojilo pozove fn (redosled nije odredjen). fn ne sme da menja skladiste.
void   stForEach(Storage* st, StVisitFn fn, void* ctx);

// Broj brojila i ukupan broj ocitavanja.
size_t stMeterCount(const Storage* st);
size_t stReadingCount(const Storage* st);
