#pragma once
// Prednji deo LB-a (Osoba 1): sve od korisnika do reda Q i put odgovora nazad.
//
// Niti:
//   TPRIH - select nad oba listen soketa (5059 korisnici, 5060 radnici)
//   TCIT  - select nad svim korisnickim soketima, cita zahteve, stavlja ih u red Q
//   TRAS  - skida zahteve sa Q i salje ih dalje preko meterRoute
//   TODG  - jedina nit koja salje korisnicima (iz reda odgovora)

#include "Dogovor.h"

// ---------------------------------------------------------------------------
// Sesija korisnika
// ---------------------------------------------------------------------------

// Jedan povezani korisnik. Ima brojac referenci: drze je TCIT (dok je korisnik
// povezan), svaki zahtev na cekanju i svaki odgovor u redu odgovora.
// Soket se zatvara tek kad sesiju niko vise ne drzi, pa TODG nikad ne salje na
// zatvoren ili ponovo iskoriscen soket.
typedef struct {
    SOCKET        s;
    uint32_t      id;      // redni broj korisnika, za ispis
    volatile LONG refs;
    volatile LONG closed;  // 1 = korisnik je otisao; odgovori mu se samo oslobode
} Session;

// Povecava brojac referenci sesije.
void sessionAddRef(Session* s);

// Smanjuje brojac; poslednji zatvara soket i oslobadja sesiju.
void sessionRelease(Session* s);

// ---------------------------------------------------------------------------
// Red odgovora (koristi ga i Pending.cpp)
// ---------------------------------------------------------------------------

// Stavlja odgovor u red za TODG. Preuzima JEDNU referencu sesije i poruku
// (zaglavlje vec nosi korisnikov originalni requestId).
// Ako ne uspe, sam oslobodi poruku i pusti referencu.
void replyPush(Session* s, LbMsg* msg);

// Pravi ERROR poruku sa datim requestId-jem i kodom greske. NULL ako nema memorije.
LbMsg* makeErrorMsg(uint32_t requestId, uint32_t code);

// ---------------------------------------------------------------------------
// Pokretanje i gasenje (poziva glavna nit)
// ---------------------------------------------------------------------------

// Pravi redove i mapu zahteva na cekanju, otvara portove 5059 i 5060 i pokrece
// TPRIH, TCIT, TRAS i TODG. Poziva se posle workersStart.
// Vraca false ako nesto ne uspe (tada je vec sve pocisceno).
bool usersStart(void);

// Ceka da se zavrse TPRIH, TCIT i TRAS (posle postavljanja g_stopEvent).
// Posle ovoga niko vise ne zove meterRoute, pa sme workersShutdown.
void usersStopInput(void);

// Ceka TODG i oslobadja sve: redove, sesije, zahteve na cekanju, sokete.
// Poziva se posle workersShutdown (TPR niti vise ne zovu pendingComplete).
void usersShutdown(void);

// Ispise brojace (korisnici, zahtevi, odgovori, bajtovi).
void usersPrintStats(void);
