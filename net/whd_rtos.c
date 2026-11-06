/* SPDX-License-Identifier: Apache-2.0 */
#include "cyabs_rtos.h"
#include "net_platform.h"
#include "bsp.h"
#include <stdlib.h>
typedef struct { OS_EVENT *os; uint32_t max; } port_sem;
static cy_thread_entry_fn_t whd_entry;
static cy_thread_arg_t whd_arg;
static volatile bool whd_thread_alive;
static INT8U whd_thread_prio;
uint32_t net_now_ms(void) { return (uint32_t)OSTimeGet()*10u; }
INT32U net_ms_ticks(uint32_t ms) { return ms/10u+(ms%10u!=0u); }
bool net_sem_wait(OS_EVENT *sem,uint32_t ms)
{
    INT8U e;
    if(ms==0) return OSSemAccept(sem)!=0;
    OSSemPend(sem,ms==UINT32_MAX?0:net_ms_ticks(ms),&e);
    return e==OS_ERR_NONE;
}
cy_rslt_t cy_rtos_init_semaphore(cy_semaphore_t *s,uint32_t max,uint32_t count)
{
    if(!s || !max || max>65535 || count>max) return CY_RTOS_BAD_PARAM;
    port_sem *p=malloc(sizeof(*p)); if(!p) return CY_RTOS_NO_MEMORY;
    p->max=max; p->os=OSSemCreate((INT16U)count);
    if(!p->os) {free(p);return CY_RTOS_NO_MEMORY;}
    *s=(uint32_t)(uintptr_t)p;return CY_RSLT_SUCCESS;
}
cy_rslt_t cy_rtos_get_semaphore(cy_semaphore_t *s,cy_time_t ms,bool in_isr)
{
    if(!s || !*s || (in_isr && ms)) return CY_RTOS_BAD_PARAM;
    port_sem *p=(port_sem *)(uintptr_t)*s;
    return net_sem_wait(p->os,ms)?CY_RSLT_SUCCESS:CY_RTOS_TIMEOUT;
}
cy_rslt_t cy_rtos_set_semaphore(cy_semaphore_t *s,bool in_isr)
{
    (void)in_isr;
    if(!s || !*s)return CY_RTOS_BAD_PARAM;
    port_sem *p=(port_sem *)(uintptr_t)*s;
    OS_CPU_SR sr=OS_CPU_SR_Save();
    /* Saturate binary notifications; querying and posting are one atomic step.
     * OS nesting prevents scheduling from an actual IRQ callback. */
    INT8U e=OS_ERR_NONE;
    if(p->os->OSEventCnt<p->max) e=OSSemPost(p->os);
    OS_CPU_SR_Restore(sr);
    return e?CY_RTOS_GENERAL_ERROR:CY_RSLT_SUCCESS;
}
cy_rslt_t cy_rtos_deinit_semaphore(cy_semaphore_t *s)
{
    if(!s || !*s) return CY_RTOS_BAD_PARAM;
    port_sem *p=(port_sem *)(uintptr_t)*s;INT8U e;
    OSSemDel(p->os,OS_DEL_NO_PEND,&e);
    if(e)return CY_RTOS_GENERAL_ERROR;
    free(p);*s=0;return CY_RSLT_SUCCESS;
}
static void whd_trampoline(void *arg)
{
    (void)arg; whd_entry(whd_arg); (void)cy_rtos_exit_thread();
    bsp_panic("WHD thread exit returned");
}
cy_rslt_t cy_rtos_create_thread(cy_thread_t *thread,cy_thread_entry_fn_t fn,
    const char *name,void *stack,uint32_t size,cy_thread_priority_t priority,cy_thread_arg_t arg)
{
    (void)name;
    if(whd_thread_alive || !thread || !fn || !stack || ((uintptr_t)stack&7u) ||
       (size&7u) || size<4096 || (uint32_t)priority!=8u)return CY_RTOS_BAD_PARAM;
    whd_entry=fn;whd_arg=arg;whd_thread_prio=8;whd_thread_alive=true;*thread=8;
    INT8U e=OSTaskCreateExt(whd_trampoline,NULL,((OS_STK *)stack)+size/4-1,8,8,stack,size/4,
                          NULL,OS_TASK_OPT_STK_CHK|OS_TASK_OPT_STK_CLR);
    if(e)whd_thread_alive=false;
    return e?CY_RTOS_GENERAL_ERROR:CY_RSLT_SUCCESS;
}
cy_rslt_t cy_rtos_exit_thread(void)
{
    OS_CPU_SR sr=OS_CPU_SR_Save();whd_thread_alive=false;
    (void)OSTaskDel(OS_PRIO_SELF);OS_CPU_SR_Restore(sr);
    return CY_RTOS_GENERAL_ERROR;
}
cy_rslt_t cy_rtos_join_thread(cy_thread_t *thread)
{
    if(!thread || *thread!=whd_thread_prio || OSPrioCur==whd_thread_prio) return CY_RTOS_BAD_PARAM;
    while(whd_thread_alive)OSTimeDly(1);
    *thread=0;return CY_RSLT_SUCCESS;
}
cy_rslt_t cy_rtos_get_time(cy_time_t *t) {if(!t)return CY_RTOS_BAD_PARAM;*t=net_now_ms();return CY_RSLT_SUCCESS;}
cy_rslt_t cy_rtos_delay_milliseconds(cy_time_t ms)
{ if(ms)OSTimeDly(net_ms_ticks(ms));return CY_RSLT_SUCCESS; }
