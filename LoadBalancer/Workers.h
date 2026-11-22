#pragma once
// Tabela radnika (Osoba 2): N slotova, po jedan za svaki WR.
// Javne funkcije (workersStart, workersShutdown, workerAttach, workerEnqueue)
// su u Dogovor.h; ovde je samo ono sto koriste ostali delovi Osobe 2.

#include "Dogovor.h"

// Stanje slota.
typedef enum {
    SLOT_EMPTY   = 0,  // WR nije prijavljen
    SLOT_ACTIVE  = 1,  // WR radi i moze da dobije nova brojila
    SLOT_LEAVING = 2   // WR se odjavljuje: prima jos poruke, ali ne i nova brojila
} SlotState;

// Trenutno stanje slota workerId (SLOT_EMPTY ako workerId >= N).
SlotState workerState(uint32_t workerId);
