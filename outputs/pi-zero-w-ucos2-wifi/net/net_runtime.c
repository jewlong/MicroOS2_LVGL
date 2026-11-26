/* SPDX-License-Identifier: Apache-2.0 */
#include "net_platform.h"
#include "bsp.h"
#include "uart.h"
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#define NANOPRINTF_IMPLEMENTATION
#define NANOPRINTF_USE_FIELD_WIDTH_FORMAT_SPECIFIERS 1
#define NANOPRINTF_USE_PRECISION_FORMAT_SPECIFIERS 1
#define NANOPRINTF_USE_FLOAT_FORMAT_SPECIFIERS 0
#define NANOPRINTF_USE_LARGE_FORMAT_SPECIFIERS 1
#define NANOPRINTF_USE_SMALL_FORMAT_SPECIFIERS 1
#define NANOPRINTF_USE_BINARY_FORMAT_SPECIFIERS 0
#define NANOPRINTF_USE_WRITEBACK_FORMAT_SPECIFIERS 0
#include "nanoprintf.h"
typedef struct block { uint32_t size,used; struct block *next; uint32_t magic; } block_t;
_Alignas(8) static uint8_t net_heap[512*1024];
static block_t *head;
void net_heap_init(void)
{
    head=(block_t *)(void *)net_heap;
    head->size=sizeof(net_heap)-sizeof(*head);head->used=0;head->next=NULL;head->magic=0x48454150;
}
void *malloc(size_t n)
{
    if(!n || n>sizeof(net_heap)-sizeof(block_t))return NULL;
    n=(n+7u)&~7u;OS_CPU_SR sr=OS_CPU_SR_Save();
    for(block_t *b=head;b;b=b->next) if(!b->used && b->size>=n) {
        if(b->size>=n+sizeof(block_t)+8) {
            block_t *tail=(block_t *)((uint8_t *)(b+1)+n);
            tail->size=b->size-(uint32_t)n-sizeof(*tail);tail->used=0;tail->next=b->next;tail->magic=0x48454150;
            b->next=tail;b->size=n;
        }
        b->used=1;OS_CPU_SR_Restore(sr);return b+1;
    }
    OS_CPU_SR_Restore(sr);return NULL;
}
void free(void *p)
{
    if(!p)return;
    OS_CPU_SR sr=OS_CPU_SR_Save();block_t *b=((block_t *)p)-1;
    if((uintptr_t)b<(uintptr_t)net_heap || (uintptr_t)b>=(uintptr_t)(net_heap+sizeof(net_heap)) ||
       b->magic!=0x48454150 || !b->used)bsp_panic("network heap corruption");
    b->used=0;
    for(block_t *q=head;q && q->next;) {
        if(!q->used && !q->next->used) {q->size+=sizeof(*q)+q->next->size;q->next=q->next->next;}
        else q=q->next;
    }
    OS_CPU_SR_Restore(sr);
}
void *calloc(size_t count,size_t size)
{
    if(size && count>SIZE_MAX/size)return NULL;
    void *p=malloc(count*size);if(p)memset(p,0,count*size);return p;
}
void *realloc(void *p,size_t n)
{
    if(!p)return malloc(n);
    if(!n){free(p);return NULL;}
    block_t *b=((block_t *)p)-1;
    if(b->size>=n)return p;
    void *q=malloc(n);if(q){memcpy(q,p,b->size);free(p);}return q;
}
static int logv(const char *fmt,va_list ap)
{
    char text[384];int n=npf_vsnprintf(text,sizeof(text),fmt,ap);
    bsp_log_lock();uart_puts(text);bsp_log_unlock();return n;
}
void net_log(const char *fmt,...)
{ va_list ap;va_start(ap,fmt);(void)logv(fmt,ap);va_end(ap); }
int printf(const char *fmt,...)
{ va_list ap;va_start(ap,fmt);int n=logv(fmt,ap);va_end(ap);return n; }
int snprintf(char *out,size_t size,const char *fmt,...)
{ va_list ap;va_start(ap,fmt);int n=npf_vsnprintf(out,size,fmt,ap);va_end(ap);return n; }
int vsnprintf(char *out,size_t size,const char *fmt,va_list ap)
{ return npf_vsnprintf(out,size,fmt,ap); }
int puts(const char *text) {net_log("%s\n",text);return 0;}
int putchar(int ch) {net_log("%c",ch);return ch;}
