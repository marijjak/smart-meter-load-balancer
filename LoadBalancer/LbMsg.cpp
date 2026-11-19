#include "Dogovor.h"
#include <stdlib.h>

// Pravi poruku i preuzima telo. Vraca NULL ako nema memorije (telo ostaje pozivaocu).
LbMsg* lbMsgNew(const MsgHeader* hdr, char* body) {
    LbMsg* m = (LbMsg*)malloc(sizeof(LbMsg));
    if (m == NULL) return NULL;
    m->hdr = *hdr;
    m->body = body;
    return m;
}

// Oslobadja poruku i telo. Prihvata NULL.
void lbMsgFree(LbMsg* msg) {
    if (msg == NULL) return;
    freeBody(msg->body);
    free(msg);
}
