#include "NetLib.h"
#include <stdio.h>

// PRIVREMENI TEST NetLib-a: prima jednog Worker-a i odgovara na REGISTER i WRITE.
int main() {
    if (!netInit()) {
        printf("WSAStartup nije uspeo\n");
        return 1;
    }

    SOCKET listenSock = netListen(LB_WORKER_PORT);
    if (listenSock == INVALID_SOCKET) {
        printf("netListen nije uspeo\n");
        netCleanup();
        return 1;
    }
    printf("LB ceka Worker-a na portu %d...\n", LB_WORKER_PORT);

    SOCKET w = accept(listenSock, NULL, NULL);
    closesocket(listenSock);
    if (w == INVALID_SOCKET) {
        printf("accept nije uspeo\n");
        netCleanup();
        return 1;
    }

    MsgHeader h;
    char* body;
    while (recvMsg(w, &h, &body) == NET_OK) {
        printf("Primljeno: %s (requestId=%u, duzina=%u)\n",
            msgTypeName(h.type), h.requestId, h.length);

        if (h.type == MSG_REGISTER) {
            RegisterBody* rb = (RegisterBody*)body;
            printf("  Worker id = %u\n", rb->workerId);
            sendMsg(w, MSG_REGISTER_OK, 0, h.requestId, NULL, 0);
        }
        else if (h.type == MSG_WRITE) {
            Reading* r = (Reading*)body;
            printf("  brojilo=%u  vreme=%lld  kWh=%.3f\n",
                r->meterId, (long long)r->timestamp, r->kwh);
            AckBody ack = { 0 };
            sendMsg(w, MSG_ACK, 0, h.requestId, &ack, sizeof(ack));
        }
        freeBody(body);
    }

    printf("Worker se odvezao.\n");
    closesocket(w);
    netCleanup();
    printf("Pritisni Enter za kraj...\n");
    getchar();
    return 0;
}