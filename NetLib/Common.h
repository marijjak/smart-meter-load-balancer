#pragma once
#include <stdint.h>



//  Konstante 
#define LB_USER_PORT     5059   // port na kome LB ceka korisnike (UR)
#define LB_WORKER_PORT   5060   // port na kome LB ceka radnike (WR)
#define MAX_WORKERS      32     // najveci dozvoljeni N
#define TRANSFER_BATCH   512    // najvise ocitavanja u jednom TRANSFER paketu
#define USER_WINDOW      256    // najvise zahteva "u letu" po korisniku
#define MAX_BODY_SIZE    (16 * 1024 * 1024) // veca poruka = greska u protokolu

//  Vrste poruka 
typedef enum {
    MSG_WRITE = 1,  // UR -> LB -> WR : jedan Reading
    MSG_QUERY = 2,  // UR -> LB -> WR : QueryBody
    MSG_ACK = 3,  // WR -> LB -> UR : AckBody
    MSG_QUERY_RESP = 4,  // WR -> LB -> UR : QueryRespHeader + niz ReadingItem
    MSG_ERROR = 5,  // LB -> UR ili LB -> WR : ErrorBody
    MSG_REGISTER = 6,  // WR -> LB : RegisterBody
    MSG_REGISTER_OK = 7,  // LB -> WR : prazno
    MSG_LEAVE_BEGIN = 8,  // WR -> LB : prazno, WR najavljuje odjavu
    MSG_TRANSFER = 9,  // WR -> LB -> WR : TransferHeader + niz Reading
    MSG_TRANSFER_END = 10, // WR -> LB, LB -> WR : TransferEndBody
    MSG_RECLAIM = 11, // LB -> WR : ReclaimHeader + niz meterId
    MSG_LEAVE_OK = 12  // LB -> WR : prazno, WR sme da se ugasi
} MsgType;

//Zastavice u zaglavlju 
#define FLAG_ALL  0x0001   // RECLAIM: predaj SVA brojila (pri odjavi)

//  Kodovi gresaka 
typedef enum {
    ERR_NO_WORKERS = 1, // nema nijednog aktivnog WR-a
    ERR_WORKER_FAILED = 2, // WR je pukao dok je zahtev bio kod njega
    ERR_LAST_WORKER = 3, // poslednji WR ne sme da se odjavi
    ERR_BAD_REQUEST = 4  // neispravna poruka
} ErrorCode;

#pragma pack(push, 1)

// Zaglavlje svake poruke (12 bajtova). Iza njega ide telo duzine length.
typedef struct {
    uint16_t type;       // MsgType
    uint16_t flags;      
    uint32_t requestId;  // uparuje zahtev i odgovor
    uint32_t length;     // duzina tela u bajtovima
} MsgHeader;

// Jedno ocitavanje brojila (16 bajtova).
typedef struct {
    uint32_t meterId;    // broj brojila
    int64_t  timestamp;  // vreme ocitavanja, Unix sekunde
    float    kwh;        
} Reading;

// Ocitavanje bez broja brojila, za odgovor na upit (12 bajtova).
typedef struct {
    int64_t timestamp;
    float   kwh;
} ReadingItem;

typedef struct { uint32_t meterId; }              QueryBody;
typedef struct { uint32_t status; }               AckBody;        // 0 = OK
typedef struct { uint32_t meterId; uint32_t count; } QueryRespHeader;
typedef struct { uint32_t code; }                 ErrorBody;      // ErrorCode
typedef struct { uint32_t workerId; }             RegisterBody;
typedef struct { uint32_t count; }                TransferHeader;
typedef struct { uint32_t total; }                TransferEndBody;
typedef struct { uint32_t count; }                ReclaimHeader;  // 0 uz FLAG_ALL

#pragma pack(pop)

// Provera da kompajler nije dodao prazne bajtove.
static_assert(sizeof(MsgHeader) == 12, "MsgHeader mora imati 12 bajtova");
static_assert(sizeof(Reading) == 16, "Reading mora imati 16 bajtova");
static_assert(sizeof(ReadingItem) == 12, "ReadingItem mora imati 12 bajtova");