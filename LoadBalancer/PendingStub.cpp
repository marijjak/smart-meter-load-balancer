// PRIVREMENO - lazne verzije funkcija koje pise Osoba 1 (vidi Dogovor.h).
// Postoje samo da bi LoadBalancer mogao da se prevede dok njen deo ne bude gotov.
// Kad Osoba 1 napise prave pendingComplete / pendingSetWorker / pendingFailWorker
// i definise g_workerCount i g_stopEvent, OVAJ FAJL SE BRISE iz projekta.

#include "Dogovor.h"
#include <stdio.h>

uint32_t g_workerCount = 0;
HANDLE   g_stopEvent = NULL;

// Lazna: samo ispise odgovor i oslobodi poruku.
void pendingComplete(uint32_t lbId, LbMsg* msg) {
    printf("[stub] pendingComplete lbId=%u %s\n", lbId, msgTypeName(msg->hdr.type));
    lbMsgFree(msg);
}

// Lazna: ne radi nista.
void pendingSetWorker(uint32_t lbId, uint32_t workerId) {
    (void)lbId; (void)workerId;
}

// Lazna: samo ispise poruku.
void pendingFailWorker(uint32_t workerId) {
    printf("[stub] pendingFailWorker WR %u\n", workerId);
}
