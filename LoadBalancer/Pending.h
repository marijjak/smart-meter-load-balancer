#pragma once
// Mapa zahteva na cekanju (Osoba 1): lbId -> ko je pitao i kom WR-u je poslato.
// Tri funkcije koje poziva Osoba 2 (pendingComplete, pendingSetWorker,
// pendingFailWorker) su u Dogovor.h; ovde je ostalo sto koristi prednji deo LB-a.
//
// Zapis ulazi u mapu cim TCIT primi zahtev, a izlazi kad stigne odgovor ili ERROR.
// Mapa je zasticena svojom kriticnom sekcijom. Dok se ona drzi, ne zove se nista
// od Osobe 2 (pravilo zakljucavanja iz Dogovor.h).

#include "Users.h"

// Pravi praznu mapu i kriticnu sekciju. Vraca false ako nema memorije.
bool pendingInit(void);

// Oslobadja sve zapise koji su ostali (i pusta njihove sesije), pa mapu.
void pendingFree(void);

// Upisuje novi zahtev. Preuzima jednu referencu sesije (pozivalac je prethodno
// pozvao sessionAddRef). Vraca false ako nema memorije; tada referenca ostaje
// pozivaocu.
bool pendingAdd(uint32_t lbId, Session* s, uint32_t userRequestId);

// Zahtev lbId nije uspeo u samom LB-u: korisnik dobije ERROR sa datim kodom.
void pendingFail(uint32_t lbId, uint32_t code);

// Broj zahteva koji trenutno cekaju odgovor.
size_t pendingCount(void);
