#include "NetLib.h"
#include <stdlib.h>
#include <string.h>

#pragma comment(lib, "Ws2_32.lib")



bool netInit() {
    WSADATA wsa;
    return WSAStartup(MAKEWORD(2, 2), &wsa) == 0;
}

void netCleanup() {
    WSACleanup();
}



SOCKET netListen(uint16_t port) {
    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) return INVALID_SOCKET;

    sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(port);

    if (bind(s, (sockaddr*)&addr, sizeof(addr)) == SOCKET_ERROR ||
        listen(s, SOMAXCONN) == SOCKET_ERROR) {
        closesocket(s);
        return INVALID_SOCKET;
    }
    return s;
}

SOCKET netConnect(const char* ip, uint16_t port) {
    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) return INVALID_SOCKET;

    sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);

    if (inet_pton(AF_INET, ip, &addr.sin_addr) != 1 ||
        connect(s, (sockaddr*)&addr, sizeof(addr)) == SOCKET_ERROR) {
        closesocket(s);
        return INVALID_SOCKET;
    }
    return s;
}

// ---------- Slanje i prijem tacnog broja bajtova ----------

int recvAll(SOCKET s, char* buf, int len) {
    int got = 0;
    while (got < len) {
        int r = recv(s, buf + got, len - got, 0);
        if (r == 0) return NET_CLOSED;
        if (r == SOCKET_ERROR) return NET_ERROR;
        got += r;
    }
    return NET_OK;
}

int sendAll(SOCKET s, const char* buf, int len) {
    int sent = 0;
    while (sent < len) {
        int r = send(s, buf + sent, len - sent, 0);
        if (r == SOCKET_ERROR) return NET_ERROR;
        sent += r;
    }
    return NET_OK;
}

// ---------- Okretanje bajtova ----------

uint32_t floatToNet(float f) {
    uint32_t u;
    memcpy(&u, &f, sizeof(u));
    return htonl(u);
}

float netToFloat(uint32_t n) {
    uint32_t u = ntohl(n);
    float f;
    memcpy(&f, &u, sizeof(f));
    return f;
}

uint64_t i64ToNet(int64_t v) {
    uint64_t u = (uint64_t)v, out;
    uint32_t hi = htonl((uint32_t)(u >> 32));
    uint32_t lo = htonl((uint32_t)u);
    memcpy((char*)&out, &hi, 4);        // gornja polovina prva
    memcpy((char*)&out + 4, &lo, 4);
    return out;
}

int64_t netToI64(uint64_t n) {
    uint32_t hi, lo;
    memcpy(&hi, (char*)&n, 4);
    memcpy(&lo, (char*)&n + 4, 4);
    return (int64_t)(((uint64_t)ntohl(hi) << 32) | ntohl(lo));
}

// Okrece 4 bajta na adresi p (uint32 ili float).
static void swap32(char* p, bool toNet) {
    uint32_t v;
    memcpy(&v, p, 4);
    v = toNet ? htonl(v) : ntohl(v);
    memcpy(p, &v, 4);
}

// Okrece 8 bajtova na adresi p (int64).
static void swap64(char* p, bool toNet) {
    if (toNet) {
        int64_t v;
        memcpy(&v, p, 8);
        uint64_t n = i64ToNet(v);
        memcpy(p, &n, 8);
    }
    else {
        uint64_t n;
        memcpy(&n, p, 8);
        int64_t v = netToI64(n);
        memcpy(p, &v, 8);
    }
}

// Cita broj elemenata PRE okretanja: ka mrezi je jos u redosledu racunara,
// a od mreze je jos u mreznom redosledu.
static uint32_t readCount(const char* p, bool toNet) {
    uint32_t v;
    memcpy(&v, p, 4);
    return toNet ? v : ntohl(v);
}

// Okrece jedan Reading: meterId (4), timestamp (8), kwh (4).
static void convertReading(char* p, bool toNet) {
    swap32(p, toNet);
    swap64(p + 4, toNet);
    swap32(p + 12, toNet);
}

bool convertBody(uint16_t type, char* body, uint32_t length, bool toNet) {
    switch (type) {
    case MSG_WRITE:
        if (length != sizeof(Reading)) return false;
        convertReading(body, toNet);
        return true;

    case MSG_QUERY:
    case MSG_ACK:
    case MSG_ERROR:
    case MSG_REGISTER:
    case MSG_TRANSFER_END:
        if (length != 4) return false;
        swap32(body, toNet);
        return true;

    case MSG_REGISTER_OK:
    case MSG_LEAVE_BEGIN:
    case MSG_LEAVE_OK:
        return length == 0;

    case MSG_QUERY_RESP: {
        if (length < sizeof(QueryRespHeader)) return false;
        uint32_t count = readCount(body + 4, toNet);
        if ((uint64_t)length != sizeof(QueryRespHeader) + (uint64_t)count * sizeof(ReadingItem))
            return false;
        swap32(body, toNet);       // meterId
        swap32(body + 4, toNet);   // count
        char* p = body + sizeof(QueryRespHeader);
        for (uint32_t i = 0; i < count; i++, p += sizeof(ReadingItem)) {
            swap64(p, toNet);      // timestamp
            swap32(p + 8, toNet);  // kwh
        }
        return true;
    }

    case MSG_TRANSFER: {
        if (length < sizeof(TransferHeader)) return false;
        uint32_t count = readCount(body, toNet);
        if (count > TRANSFER_BATCH ||
            (uint64_t)length != sizeof(TransferHeader) + (uint64_t)count * sizeof(Reading))
            return false;
        swap32(body, toNet);
        char* p = body + sizeof(TransferHeader);
        for (uint32_t i = 0; i < count; i++, p += sizeof(Reading))
            convertReading(p, toNet);
        return true;
    }

    case MSG_RECLAIM: {
        if (length < sizeof(ReclaimHeader)) return false;
        uint32_t count = readCount(body, toNet);
        if ((uint64_t)length != sizeof(ReclaimHeader) + (uint64_t)count * 4)
            return false;
        swap32(body, toNet);
        char* p = body + sizeof(ReclaimHeader);
        for (uint32_t i = 0; i < count; i++, p += 4)
            swap32(p, toNet);
        return true;
    }

    default:
        return false;  
    }
}

// ---------- Slanje i prijem cele poruke ----------

int sendMsg(SOCKET s, uint16_t type, uint16_t flags, uint32_t requestId,
    const void* body, uint32_t length) {
    if (length > MAX_BODY_SIZE) return NET_ERROR;

    uint32_t total = (uint32_t)sizeof(MsgHeader) + length;
    char* buf = (char*)malloc(total);
    if (buf == NULL) return NET_ERROR;

    MsgHeader h;
    h.type = htons(type);
    h.flags = htons(flags);
    h.requestId = htonl(requestId);
    h.length = htonl(length);
    memcpy(buf, &h, sizeof(h));
    if (length > 0) memcpy(buf + sizeof(h), body, length);

    if (!convertBody(type, buf + sizeof(h), length, true)) {
        free(buf);
        return NET_ERROR;
    }

    int r = sendAll(s, buf, (int)total);   // zaglavlje i telo jednim slanjem
    free(buf);
    return r;
}

int recvMsg(SOCKET s, MsgHeader* hdr, char** body) {
    *body = NULL;

    MsgHeader h;
    int r = recvAll(s, (char*)&h, sizeof(h));
    if (r != NET_OK) return r;

    hdr->type = ntohs(h.type);
    hdr->flags = ntohs(h.flags);
    hdr->requestId = ntohl(h.requestId);
    hdr->length = ntohl(h.length);
    if (hdr->length > MAX_BODY_SIZE) return NET_ERROR;

    char* b = (char*)malloc(hdr->length > 0 ? hdr->length : 1);
    if (b == NULL) return NET_ERROR;

    if (hdr->length > 0) {
        r = recvAll(s, b, (int)hdr->length);
        if (r != NET_OK) {
            free(b);
            return r;
        }
    }

    if (!convertBody(hdr->type, b, hdr->length, false)) {
        free(b);
        return NET_ERROR;
    }

    *body = b;
    return NET_OK;
}

void freeBody(char* body) {
    free(body);
}



const char* msgTypeName(uint16_t type) {
    switch (type) {
    case MSG_WRITE:        return "WRITE";
    case MSG_QUERY:        return "QUERY";
    case MSG_ACK:          return "ACK";
    case MSG_QUERY_RESP:   return "QUERY_RESP";
    case MSG_ERROR:        return "ERROR";
    case MSG_REGISTER:     return "REGISTER";
    case MSG_REGISTER_OK:  return "REGISTER_OK";
    case MSG_LEAVE_BEGIN:  return "LEAVE_BEGIN";
    case MSG_TRANSFER:     return "TRANSFER";
    case MSG_TRANSFER_END: return "TRANSFER_END";
    case MSG_RECLAIM:      return "RECLAIM";
    case MSG_LEAVE_OK:     return "LEAVE_OK";
    default:               return "UNKNOWN";
    }
}