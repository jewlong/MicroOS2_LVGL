/* SPDX-License-Identifier: Apache-2.0
 * Single root-port FS/LS host, DWC2 internal DMA, one packet/channel at a time.
 * Register meanings: Synopsys DWC2 register map and BCM2835 integration.
 * No hub/split transactions. MMU and D-cache MUST stay disabled.
 */
#include "usb_host.h"
#include "platform.h"
#include "mailbox.h"
#include "uart.h"
#include <ucos_ii.h>
#include <string.h>
#define BASE (PERIPHERAL_BASE+0x980000u)
#define AHB 0x008u
#define USB_CFG 0x00cu
#define RESET 0x010u
#define GINT 0x014u
#define GMASK 0x018u
#define HCFG 0x400u
#define HFIR 0x404u
#define HFNUM 0x408u
#define HPRT 0x440u
#define HCCHAR 0x500u
#define HCINT 0x508u
#define HCTSIZ 0x510u
#define HCDMA 0x514u
#define ENABLE (1u<<31)
#define HALT (1u<<30)
_Alignas(16) static volatile uint32_t power_message[8];
/* Padded bounce buffer is never handed back while hardware can still DMA. */
_Alignas(64) static uint8_t dma_buffer[128];
static bool powered,operational;
static uint32_t read_reg(uint32_t r){return mmio_read(BASE+r);}
static void write_reg(uint32_t r,uint32_t v){mmio_write(BASE+r,v);}
static bool wait_reg(uint32_t r,uint32_t mask,uint32_t expected,uint32_t timeout)
{
    uint32_t start=timer_us();
    for(unsigned fast=0;fast<25;++fast){
        if((read_reg(r)&mask)==expected)return true;
        delay_us(10);
    }
    do {if((read_reg(r)&mask)==expected)return true;OSTimeDly(1);}
    while((uint32_t)(timer_us()-start)<timeout);
    return false;
}
void usb_power_init(void)
{
    const uint32_t req[8]={32,0,0x00028001,8,8,3,3,0};
    for(unsigned i=0;i<8;i++)power_message[i]=req[i];
    mailbox_result_t mr=mailbox_property_call(power_message,32,1000000);
    powered=mr==MAILBOX_OK &&
        power_message[1]==0x80000000u && power_message[4]==0x80000008u &&
        power_message[5]==3 && (power_message[6]&1u)==1u;
    if(!powered){
        usb_init_err=1u;
        usb_init_val=(mr!=MAILBOX_OK)?(0xE0000000u|(uint32_t)mr):power_message[6];
    }
    uart_puts(powered?"[USB] host power on.\n":"[USB] power request failed; USB disabled.\n");
}
static void port_bits(uint32_t set,uint32_t clear)
{
    uint32_t v=read_reg(HPRT);
    /* HPRT enabled and change bits have destructive write semantics. */
    v&=~((1u<<1)|(1u<<2)|(1u<<3)|(1u<<5));
    v=(v&~clear)|set;write_reg(HPRT,v);
}
bool usb_host_connected(void)
{return operational && (read_reg(HPRT)&0x11u)==1u;}
bool usb_host_operational(void){return operational;}
uint32_t usb_diag_hprt(void){return operational?read_reg(HPRT):0u;}
static bool stop_channel(void)
{
    if(!(read_reg(HCCHAR)&ENABLE))return true;
    write_reg(HCCHAR,read_reg(HCCHAR)|ENABLE|HALT);
    if(wait_reg(HCCHAR,ENABLE,0,100000))return true;
    /* Quarantine DMA storage until a core reset, even if halt fails. */
    operational=false;write_reg(GMASK,0);write_reg(AHB,0);return false;
}
bool usb_host_init(void)
{
    operational=false;if(!powered)return false;
    uint32_t id=read_reg(0x40);
    if((id&0xfffff000u)!=0x4f542000u || ((read_reg(0x48)>>3)&3u)!=2u ||
       (read_reg(0x4c)>>16)<3072u){usb_init_err=2u;usb_init_val=id;return false;}
    write_reg(AHB,0);write_reg(GMASK,0);write_reg(0xe00,0);
    uint32_t cfg=read_reg(USB_CFG);
    cfg&=~((1u<<3)|(1u<<4)|(1u<<6)|(1u<<8)|(1u<<9)|(15u<<10)|
           (1u<<17)|(1u<<19)|(1u<<20)|(1u<<22)|(1u<<30));
    write_reg(USB_CFG,cfg|(9u<<10)|(1u<<29));
    if(!wait_reg(RESET,1u<<31,1u<<31,100000)){usb_init_err=3u;usb_init_val=read_reg(RESET);return false;}
    write_reg(RESET,1);
    if(!wait_reg(RESET,1,0,100000)){usb_init_err=4u;usb_init_val=read_reg(RESET);return false;}
    write_reg(USB_CFG,cfg|(9u<<10)|(1u<<29));
    OSTimeDly(10);
    if(!wait_reg(GINT,1,1,250000)){usb_init_err=5u;usb_init_val=read_reg(GINT);return false;}
    write_reg(AHB,(1u<<5)|(1u<<4)); /* DMA, wait for AXI writes; CPU IRQ masked */
    write_reg(HCFG,0u);write_reg(HFIR,60000);
    write_reg(0x24,1024);write_reg(0x28,(1024u<<16)|1024u);
    write_reg(0x100,(1024u<<16)|2048u);
    write_reg(RESET,(0x10u<<6)|(1u<<5));
    if(!wait_reg(RESET,1u<<5,0,100000)){usb_init_err=6u;usb_init_val=read_reg(RESET);return false;}
    write_reg(RESET,1u<<4);
    if(!wait_reg(RESET,1u<<4,0,100000)){usb_init_err=7u;usb_init_val=read_reg(RESET);return false;}
    write_reg(GINT,0xffffffffu);write_reg(0x418,1);write_reg(0x50c,0x7ff);
    port_bits(1u<<12,0);
    OSTimeDly(20);operational=true;return true;
}
usb_result_t usb_host_reset_port(bool *low)
{
    if(!usb_host_connected())return USB_REMOVED;
    if(!stop_channel())return USB_IO;
    OSTimeDly(10); /* attachment debounce */
    if(!usb_host_connected())return USB_REMOVED;
    port_bits(1u<<8,0);OSTimeDly(6);port_bits(0,1u<<8);OSTimeDly(3);
    uint32_t p=read_reg(HPRT);
    if(!(p&1))return USB_REMOVED;
    if(!(p&4) || (p&16))return USB_IO;
    uint32_t speed=(p>>17)&3u;
    if(speed>2)return USB_UNSUPPORTED;
    *low=speed==2;return USB_OK;
}
static usb_result_t packet(usb_keyboard_device_t *d,uint8_t ep,uint8_t type,bool in,
                           uint8_t pid,void *data,uint16_t length,uint16_t *actual)
{
    uint16_t mps=ep?d->packet_size:d->ep0;
    if(!mps || mps>64 || length>mps || !operational){usb_init_val=0xA1000000u|mps;return USB_IO;}
    if(!usb_host_connected())return USB_REMOVED;
    if(!stop_channel()){usb_init_val=0xA2000000u|read_reg(HCCHAR);return USB_IO;}
    uint32_t bytes=in?mps:length;
    memset(dma_buffer,0,sizeof(dma_buffer));
    if(!in && length)memcpy(dma_buffer,data,length);
    arm_dmb();write_reg(HCINT,0x7ff);write_reg(0x504,0);
    write_reg(HCTSIZ,bytes|(1u<<19)|((uint32_t)pid<<29));
    write_reg(HCDMA,(uint32_t)(uintptr_t)dma_buffer|DMA_BUS_ALIAS);
    uint32_t ch=mps|((uint32_t)ep<<11)|(in?1u<<15:0)|
        (d->low_speed?1u<<17:0)|((uint32_t)type<<18)|(1u<<20)|((uint32_t)d->address<<22);
    if(type==3 && !(read_reg(HFNUM)&1u))ch|=1u<<29; /* next frame parity */
    write_reg(HCCHAR,ch|ENABLE);
    uint32_t start=timer_us(),status=0;usb_result_t result=USB_TIMEOUT;
    uint32_t wait_mask=2u|1u|(1u<<3)|(1u<<2)|(1u<<7)|(1u<<8)|(1u<<9)|(1u<<10);
    if(type==3)wait_mask|=(1u<<4);
    for(unsigned fast=0;fast<35;++fast){
        status=read_reg(HCINT);
        if(status&wait_mask)break;
        delay_us(50);
    }
    while(!(status&wait_mask) && (uint32_t)(timer_us()-start)<250000u){
        if(!usb_host_connected()){result=USB_REMOVED;break;}
        if((status&(1u<<4)) && (uint32_t)(timer_us()-start)>20000u)break;
        OSTimeDly(1);
        status=read_reg(HCINT);
    }
    if(!stop_channel()){usb_init_val=0xA3000000u|status;return USB_IO;}
    status|=read_reg(HCINT);
    if(status&1u)result=USB_OK;
    else if(status&(1u<<3))result=USB_STALL;
    else if(status&((1u<<2)|(1u<<7)|(1u<<8)|(1u<<9)|(1u<<10))){result=USB_IO;usb_init_val=status;}
    else if(status&(1u<<4))result=USB_NAK;
    else if(status&2u){result=USB_IO;usb_init_val=status;}
    if(result==USB_OK){
        arm_dmb();
        if(in){
            uint32_t remaining=read_reg(HCTSIZ)&0x7ffffu;
            if(remaining>bytes || bytes-remaining>length){usb_init_val=0xA4000000u|remaining;return USB_IO;}
            *actual=(uint16_t)(bytes-remaining);
            if(*actual)memcpy(data,dma_buffer,*actual);
        }else{
            *actual=length;
        }
    }
    write_reg(HCINT,0x7ff);return result;
}
static usb_result_t control_packet(usb_keyboard_device_t *d,bool in,uint8_t pid,void *data,uint16_t n,uint16_t *actual)
{
    uint32_t start=timer_us();usb_result_t r;
    do {r=packet(d,0,0,in,pid,data,n,actual);if(r!=USB_NAK)return r;OSTimeDly(1);}
    while((uint32_t)(timer_us()-start)<1000000u);
    return USB_TIMEOUT;
}
usb_result_t usb_control(usb_keyboard_device_t *d,uint8_t type,uint8_t request,
    uint16_t value,uint16_t index,void *data,uint16_t length,uint16_t *actual)
{
    uint8_t setup[8]={type,request,(uint8_t)value,(uint8_t)(value>>8),
        (uint8_t)index,(uint8_t)(index>>8),(uint8_t)length,(uint8_t)(length>>8)};
    uint16_t n=0;*actual=0;
    usb_result_t r=control_packet(d,false,3,setup,8,&n);
    if(r!=USB_OK || n!=8){if(r==USB_OK)usb_init_val=0xA5000000u|n;return r==USB_OK?USB_IO:r;}
    uint8_t pid=2;bool in=(type&128u)!=0;
    while(*actual<length){
        uint16_t want=length-*actual;if(want>d->ep0)want=d->ep0;
        r=control_packet(d,in,pid,(uint8_t *)data+*actual,want,&n);
        if(r!=USB_OK)return r;
        *actual+=n;pid^=2;
        if(n<want)break;
    }
    /* No-data requests always have an IN status stage. */
    r=control_packet(d,length?!in:true,2,NULL,0,&n);
    if(r==USB_OK && n){usb_init_val=0xA6000000u|n;return USB_IO;}
    return r;
}
usb_result_t usb_interrupt_in(usb_keyboard_device_t *d,uint8_t pid,void *data,uint16_t *actual)
{return packet(d,d->endpoint,3,true,pid,data,d->packet_size,actual);}
