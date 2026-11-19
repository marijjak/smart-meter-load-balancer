#include "NetLib.h"
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

// PRIVREMENI TEST NetLib-a: javi se LB-u i posalji jedno ocitavanje.
int main(int argc, char* argv[]) {
    uint32_t id = (argc > 1) ? (uint32_t)atoi(argv[1]) : 0;

    if (!netInit()) {
        printf("WSAStartup nije uspeo\n");
        return 1;
    }

    SOCKET s = netConnect("127.0.0.1", LB_WORKER_PORT);
    if (s == INVALID_SOCKET) {
        printf("Ne mogu da se povezem na LB. Da li je LB pokrenut?\n");
        netCleanup();
        return 1;
    }

    MsgHeader h;
    char* body;

    // 1) Prijava
    RegisterBody rb = { id };
    sendMsg(s, MSG_REGISTER, 0, 1, &rb, sizeof(rb));
    if (recvMsg(s, &h, &body) == NET_OK) {
        printf("Odgovor na prijavu: %s\n", msgTypeName(h.type));
        freeBody(body);
    }

    // 2) Jedno ocitavanje
    Reading r;
    r.meterId = 13;
    r.timestamp = (int64_t)time(NULL);
    r.kwh = 1.25f;
    printf("Saljem: brojilo=%u  vreme=%lld  kWh=%.3f\n",
        r.meterId, (long long)r.timestamp, r.kwh);
    sendMsg(s, MSG_WRITE, 0, 2, &r, sizeof(r));

    if (recvMsg(s, &h, &body) == NET_OK) {
        AckBody* a = (AckBody*)body;
        printf("Odgovor na upis: %s, status=%u, requestId=%u\n",
            msgTypeName(h.type), a->status, h.requestId);
        freeBody(body);
    }

    closesocket(s);
    netCleanup();
    printf("Pritisni Enter za kraj...\n");
    getchar();
    return 0;
}