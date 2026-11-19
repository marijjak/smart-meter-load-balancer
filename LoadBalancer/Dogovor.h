#pragma once
// Dogovor izmedju dva dela Load Balancer-a.
//
//   Osoba 1: korisnici (TPRIH, TCIT, red Q, TRAS, mapa zahteva na cekanju, TODG)
//   Osoba 2: radnici   (tabela radnika, TSL, TPR, mapa brojila, parking, TSEOBA)
//
// Ovo su jedina mesta gde kod jednog dela poziva kod drugog. Potpisi se ne
// menjaju bez dogovora. Dok druga strana ne zavrsi svoje, svaka moze da napravi
// laznu verziju funkcije koja samo ispise poruku.
//
// Pravilo zakljucavanja (da ne dodje do deadlock-a):
//   kriticna sekcija mape brojila  ->  kriticna sekcija mape zahteva na cekanju
//   Dozvoljeno je drzati mapu brojila i pozvati pending* funkciju.
//   Obrnuto nije: dok se drzi mapa zahteva na cekanju, ne zove se nista od Osobe 2.

#include "NetLib.h"

// ---------------------------------------------------------------------------
// Zajednicko za ceo LB
// ---------------------------------------------------------------------------

// Broj radnika N, iz argumenta komandne linije. Postavlja ga glavna nit pre
// pokretanja svih ostalih niti i posle se ne menja.
extern uint32_t g_workerCount;

// Event za gasenje. Kad je postavljen, sve niti treba da zavrse posao i izadju.
extern HANDLE g_stopEvent;

// Jedna poruka u redovima LB-a (red Q, red za slanje WR-a, red odgovora, parking).
// Zaglavlje je u redosledu bajtova racunara, kao posle recvMsg.
// Za zahteve i odgovore hdr.requestId je lbId (jedinstven u celom LB-u),
// a ne korisnikov requestId.
typedef struct {
    MsgHeader hdr;
    char*     body;   // telo iz recvMsg; poruka je njegov vlasnik
} LbMsg;

// Pravi novu poruku. Preuzima body (posle ovoga ga oslobadja lbMsgFree).
// Vraca NULL ako nema memorije; tada body ostaje pozivaocu.
LbMsg* lbMsgNew(const MsgHeader* hdr, char* body);

// Oslobadja poruku i njeno telo. Prihvata i NULL.
// Poruku oslobadja onaj ko je poslednji koristi (npr. TSL posle slanja,
// TODG posle slanja korisniku).
void lbMsgFree(LbMsg* msg);

// ---------------------------------------------------------------------------
// Pise Osoba 2, poziva Osoba 1
// ---------------------------------------------------------------------------

// Pravi tabelu radnika i mapu brojila i pokrece niti TSL x N, TPR x N i TSEOBA.
// Poziva glavna nit LB-a posle postavljanja g_workerCount, a pre TPRIH.
// Vraca false ako nesto ne uspe (tada je sve vec pocisceno).
bool workersStart(void);

// Gasi deo Osobe 2: zatvara sokete radnika, budi i ceka svoje niti,
// oslobadja tabelu, mapu brojila i parking.
// Poziva glavna nit posle postavljanja g_stopEvent i posle gasenja
// TPRIH, TCIT i TRAS, a pre gasenja TODG (TPR jos zove pendingComplete).
void workersShutdown(void);

// TPRIH je primio REGISTER na portu 5060 i zove ovo.
// Upise soket u slot workerId, posalje REGISTER_OK, probudi TSL i TPR tog
// slota, a TSEOBA vrati brojila tom WR-u.
// Vraca false ako je workerId >= N ili je slot zauzet; tada TPRIH posalje
// ERROR (ERR_BAD_REQUEST) i zatvori soket.
bool workerAttach(uint32_t workerId, SOCKET s);

// Rezultat funkcije meterRoute.
typedef enum {
    ROUTE_SENT      = 0,  // poruka je u redu za slanje WR-a; pendingSetWorker je vec pozvan
    ROUTE_PARKED    = 1,  // brojilo se seli; poruka ceka u parkingu
    ROUTE_NO_WORKER = 2   // nema aktivnog WR-a; poruka ostaje pozivaocu
} RouteResult;

// TRAS zove ovo za svaki zahtev (WRITE ili QUERY) skinut sa reda Q.
// Pod kriticnom sekcijom mape brojila nadje vlasnika brojila i:
//   - ako brojilo nije u mapi, upise ga (kucni WR = meterId % N, a ako on nije
//     aktivan, aktivni WR sa najmanje brojila);
//   - ako se brojilo seli ili vraca, stavi poruku u parking -> ROUTE_PARKED;
//   - inace pozove pendingSetWorker(lbId, wr) i workerEnqueue(wr, msg),
//     sve DOK DRZI mapu brojila -> ROUTE_SENT.
// Kod ROUTE_SENT i ROUTE_PARKED poruka vise nije pozivaocu.
// Kod ROUTE_NO_WORKER TRAS posalje korisniku ERROR (ERR_NO_WORKERS)
// preko pendingComplete i oslobodi poruku.
//
// Zasto se upis u red radi unutar meterRoute, a ne posle u TRAS-u:
// kad bi TRAS stavio zahtev u red tek posle otkljucavanja mape, TSEOBA bi
// izmedju mogla da oznaci brojilo i posalje RECLAIM. Zahtev bi tada stigao
// IZA RECLAIM-a, kod WR-a koji je podatke vec predao, i upis bi se izgubio.
RouteResult meterRoute(uint32_t meterId, LbMsg* msg);

// Stavi poruku u red za slanje WR-a workerId i probudi njegovu TSL nit.
// Vraca false ako slot nije aktivan; tada poruka ostaje pozivaocu.
bool workerEnqueue(uint32_t workerId, LbMsg* msg);

// ---------------------------------------------------------------------------
// Pise Osoba 1, poziva Osoba 2
// ---------------------------------------------------------------------------

// TPR je primio ACK ili QUERY_RESP (msg->hdr.requestId je lbId).
// Nadje zapis na cekanju, izvadi ga iz mape i stavi odgovor u red odgovora,
// odakle ga TODG salje korisniku sa njegovim originalnim requestId-jem.
// Uvek preuzima poruku: ako zapisa nema (zahtev je vec dobio ERROR) ili je
// korisnik otisao, poruka se samo oslobodi.
void pendingComplete(uint32_t lbId, LbMsg* msg);

// Upise u zapis na cekanju kom WR-u je zahtev poslat.
// Zove se PRE stavljanja poruke u red WR-a (u meterRoute i pri praznjenju
// parkinga), da pendingFailWorker uvek zna za taj zahtev.
// Ako zapisa nema, ne radi nista.
void pendingSetWorker(uint32_t lbId, uint32_t workerId);

// WR workerId je pao bez odjave. Svaki zahtev na cekanju upucen njemu dobije
// ERROR (ERR_WORKER_FAILED) i izbaci se iz mape.
// Osoba 2 je zove tek posto je slot oznacen kao prazan i red za slanje
// ispraznjen, da posle ovoga niko vise ne posalje zahtev tom WR-u.
void pendingFailWorker(uint32_t workerId);
