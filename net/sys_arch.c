/* SPDX-License-Identifier: Apache-2.0 */
#include "lwip/opt.h"
#include "lwip/sys.h"
#include "lwip/err.h"
#include "net_platform.h"
#include "bsp.h"
#include <stdlib.h>
#include <string.h>
#define MBOX_MAX 64
struct port_mbox { OS_EVENT *queue, *slots; void *items[MBOX_MAX]; };
static char null_message;
static const INT8U ceiling_pool[] = {1,2,3,6,7};
static uint8_t ceiling_used[5];
static OS_EVENT *ceiling_events[5];
_Alignas(8) static OS_STK tcpip_stack[4096];
static bool tcpip_created;
static volatile uint32_t random_state = 0x6d2b79f5u;

uint32_t port_lwip_rand(void)
{
    OS_CPU_SR sr=OS_CPU_SR_Save();
    uint32_t x=random_state; x^=x<<13; x^=x>>17; x^=x<<5;
    random_state=x; OS_CPU_SR_Restore(sr); return x;
}
void sys_init(void) {}
u32_t sys_now(void) { return net_now_ms(); }
u32_t sys_jiffies(void) { return OSTimeGet(); }
sys_prot_t sys_arch_protect(void) { return OS_CPU_SR_Save(); }
void sys_arch_unprotect(sys_prot_t sr) { OS_CPU_SR_Restore(sr); }
err_t sys_sem_new(sys_sem_t *s,u8_t count)
{ *s=OSSemCreate(count); return *s ? ERR_OK:ERR_MEM; }
void sys_sem_signal(sys_sem_t *s) { if(OSSemPost(*s)!=OS_ERR_NONE) bsp_panic("lwIP semaphore post"); }
u32_t sys_arch_sem_wait(sys_sem_t *s,u32_t ms)
{
    uint32_t start=sys_now();
    if(!net_sem_wait(*s,ms==0?UINT32_MAX:ms)) return SYS_ARCH_TIMEOUT;
    return sys_now()-start;
}
void sys_sem_free(sys_sem_t *s)
{ INT8U e; OSSemDel(*s,OS_DEL_NO_PEND,&e); if(e) bsp_panic("lwIP semaphore in use"); *s=0; }
err_t sys_mutex_new(sys_mutex_t *m)
{
    OS_CPU_SR sr=OS_CPU_SR_Save(); INT8U e;
    for(unsigned i=0;i<5;i++) if(!ceiling_used[i]) {
        ceiling_used[i]=1; OS_CPU_SR_Restore(sr);
        *m=OSMutexCreate(ceiling_pool[i],&e);
        sr=OS_CPU_SR_Save();
        if(e) ceiling_used[i]=0; else ceiling_events[i]=*m;
        OS_CPU_SR_Restore(sr); return e?ERR_MEM:ERR_OK;
    }
    OS_CPU_SR_Restore(sr); *m=0; return ERR_MEM;
}
void sys_mutex_lock(sys_mutex_t *m)
{ INT8U e; OSMutexPend(*m,0,&e); if(e) bsp_panic("lwIP mutex pend"); }
void sys_mutex_unlock(sys_mutex_t *m)
{ if(OSMutexPost(*m)) bsp_panic("lwIP mutex post"); }
void sys_mutex_free(sys_mutex_t *m)
{
    INT8U e; OS_CPU_SR sr=OS_CPU_SR_Save();
    unsigned i; for(i=0;i<5;i++) if(ceiling_events[i]==*m) break;
    OS_CPU_SR_Restore(sr);
    OSMutexDel(*m,OS_DEL_NO_PEND,&e); if(e || i==5) bsp_panic("lwIP mutex free");
    sr=OS_CPU_SR_Save(); ceiling_used[i]=0; ceiling_events[i]=0; *m=0; OS_CPU_SR_Restore(sr);
}
err_t sys_mbox_new(sys_mbox_t *m,int size)
{
    *m=NULL; if(size<=0 || size>MBOX_MAX) return ERR_ARG;
    struct port_mbox *b=calloc(1,sizeof(*b)); if(!b) return ERR_MEM;
    b->queue=OSQCreate(b->items,(INT16U)size);
    b->slots=OSSemCreate((INT16U)size);
    if(!b->queue || !b->slots) {
        INT8U e; if(b->queue) OSQDel(b->queue,OS_DEL_NO_PEND,&e);
        if(b->slots) OSSemDel(b->slots,OS_DEL_NO_PEND,&e);
        free(b); return ERR_MEM;
    }
    *m=b; return ERR_OK;
}
static err_t post(sys_mbox_t *m,void *message,bool wait)
{
    if(!net_sem_wait((*m)->slots,wait?UINT32_MAX:0)) return ERR_MEM;
    INT8U e=OSQPost((*m)->queue,message?message:&null_message);
    if(e) { OSSemPost((*m)->slots); return ERR_MEM; }
    return ERR_OK;
}
void sys_mbox_post(sys_mbox_t *m,void *message)
{ if(post(m,message,true)!=ERR_OK) bsp_panic("lwIP mailbox post"); }
err_t sys_mbox_trypost(sys_mbox_t *m,void *message) { return post(m,message,false); }
err_t sys_mbox_trypost_fromisr(sys_mbox_t *m,void *message) { return post(m,message,false); }
u32_t sys_arch_mbox_fetch(sys_mbox_t *m,void **msg,u32_t ms)
{
    uint32_t start=sys_now(); INT8U e; void *value;
    /* uC/OS-II's 32-bit timeout is ticks. Round upward; 0 means infinite. */
    value=OSQPend((*m)->queue,ms?net_ms_ticks(ms):0,&e);
    if(e==OS_ERR_TIMEOUT) return SYS_ARCH_TIMEOUT;
    if(e) bsp_panic("lwIP mailbox fetch");
    if(msg) *msg=value==&null_message?NULL:value;
    OSSemPost((*m)->slots);
    return sys_now()-start;
}
u32_t sys_arch_mbox_tryfetch(sys_mbox_t *m,void **msg)
{
    INT8U e; void *value=OSQAccept((*m)->queue,&e);
    if(e==OS_ERR_Q_EMPTY) return SYS_MBOX_EMPTY;
    if(e) bsp_panic("lwIP mailbox tryfetch");
    if(msg) *msg=value==&null_message?NULL:value;
    OSSemPost((*m)->slots); return 0;
}
void sys_mbox_free(sys_mbox_t *m)
{
    INT8U e; OSQDel((*m)->queue,OS_DEL_NO_PEND,&e);
    if(e) bsp_panic("lwIP mailbox in use");
    OSSemDel((*m)->slots,OS_DEL_NO_PEND,&e);
    if(e) bsp_panic("lwIP mailbox slots");
    free(*m); *m=NULL;
}
sys_thread_t sys_thread_new(const char *name,lwip_thread_fn fn,void *arg,int bytes,int prio)
{
    (void)name;
    if(tcpip_created || bytes>(int)sizeof(tcpip_stack) || prio!=11) bsp_panic("lwIP thread config");
    tcpip_created=true;
    INT8U e=OSTaskCreateExt(fn,arg,&tcpip_stack[4095],11,11,tcpip_stack,4096,NULL,
                          OS_TASK_OPT_STK_CHK|OS_TASK_OPT_STK_CLR);
    if(e) bsp_panic("lwIP thread create");
    return 11;
}
