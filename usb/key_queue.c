/* SPDX-License-Identifier: Apache-2.0 */
#include "keyboard.h"
#include <ucos_ii.h>
#define CAPACITY 64
static key_event_t events[CAPACITY];
static unsigned head,tail,count;
static bool lost;
volatile uint32_t usb_queue_drops,usb_key_presses;
void keyboard_post(const key_event_t *event)
{
    OS_CPU_SR sr=OS_CPU_SR_Save();
    if(count==CAPACITY || lost){++usb_queue_drops;lost=true;}
    else {events[tail]=*event;tail=(tail+1)%CAPACITY;++count;}
    if(event->pressed)++usb_key_presses;
    OS_CPU_SR_Restore(sr);
}
bool keyboard_pop(key_event_t *event)
{
    OS_CPU_SR sr=OS_CPU_SR_Save();
    if(lost){
        head=tail=count=0;lost=false;
        *event=(key_event_t){0,0,0,0,1};OS_CPU_SR_Restore(sr);return true;
    }
    if(!count){OS_CPU_SR_Restore(sr);return false;}
    *event=events[head];head=(head+1)%CAPACITY;--count;OS_CPU_SR_Restore(sr);return true;
}
void keyboard_all_up(void)
{
    OS_CPU_SR sr=OS_CPU_SR_Save();
    head=tail=count=0;lost=true;OS_CPU_SR_Restore(sr);
}
