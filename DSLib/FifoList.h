#pragma once
#include <stddef.h>


typedef struct FifoNode {
    void* item;
    struct FifoNode* next;
} FifoNode;

typedef struct {
    FifoNode* head;  
    FifoNode* tail;   
    size_t    count;
} FifoList;

void   flInit(FifoList* l);

void   flFree(FifoList* l);

bool   flPush(FifoList* l, void* item);

void* flPop(FifoList* l);
bool   flIsEmpty(const FifoList* l);
size_t flCount(const FifoList* l);