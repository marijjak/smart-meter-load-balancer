#pragma once
// Mapa brojila (Osoba 2): brojilo -> vlasnik, kucni WR i stanje.
// meterRoute je u Dogovor.h; ovde je ono sto koriste ostali delovi Osobe 2.

#include "Dogovor.h"

// Stanje jednog brojila. Zahtevi cekaju u parkingu samo dok se brojilo seli ili vraca.
typedef enum {
    METER_HOME      = 0,  // kod kuce (vlasnik = meterId % N)
    METER_MOVING    = 1,  // seli se sa WR-a koji se odjavljuje
    METER_BORROWED  = 2,  // pozajmljeno: vlasnik nije kucni WR
    METER_RETURNING = 3   // vraca se kucnom WR-u posle njegovog povratka
} MeterState;

// Pravi praznu mapu i kriticnu sekciju. Vraca false ako nema memorije.
bool   meterMapInit(void);

// Oslobadja mapu i ispise statistiku (broj brojila po WR-u, najduze drzanje sekcije).
void   meterMapFree(void);

// WR workerId je pao bez odjave: njegova brojila se brisu iz mape (podaci su izgubljeni),
// pa sledeci zahtev za njih bira novog vlasnika kao za novo brojilo.
void   meterMapDropWorker(uint32_t workerId);

// Najduze neprekidno drzanje kriticne sekcije mape, u milisekundama.
double meterMapMaxHoldMs(void);
