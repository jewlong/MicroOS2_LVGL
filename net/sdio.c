/* SPDX-License-Identifier: Apache-2.0
 * BCM2835 Arasan SDHCI, PIO CMD52/CMD53. No DMA, no SD card filesystem.
 * Register definitions: BCM2835 ARM peripherals, SD host chapter.
 */
#include "net_platform.h"
#include "platform.h"
#include "mailbox.h"
#include "cyhal_sdio.h"
#include "cyhal_gpio.h"
#include <string.h>
#define EMMC (PERIPHERAL_BASE+0x300000u)
#define BLKSZ 0x04u
#define ARG 0x08u
#define CMD 0x0cu
#define RESP 0x10u
#define DATA 0x20u
#define STATUS 0x24u
#define CTRL0 0x28u
#define CTRL1 0x2cu
#define INTR 0x30u
#define MASK 0x34u
#define SIGNAL 0x38u
#define ERRMASK 0xffff8000u
static cyhal_sdio_irq_handler_t callback;
static void *callback_arg;
static volatile bool card_events;
static OS_EVENT *bus_lock;
static bool attempted, initialized;
volatile uint32_t net_sdio_commands,net_sdio_errors;
uint8_t net_board_mac[6];
_Alignas(16) static volatile uint32_t clock_message[8];
_Alignas(16) static volatile uint32_t serial_message[8];
static uint32_t rd(uint32_t reg){return mmio_read(EMMC+reg);}
static void wr(uint32_t reg,uint32_t v){mmio_write(EMMC+reg,v);}
static bool wait_bits(uint32_t reg,uint32_t mask,uint32_t expected,uint32_t us)
{
    uint32_t start=timer_us(),yield=OSTimeGet();
    do {
        if((rd(reg)&mask)==expected)return true;
        if((uint32_t)(OSTimeGet()-yield)>=1u) {OSTimeDly(1);yield=OSTimeGet();}
    }while((uint32_t)(timer_us()-start)<us);
    return false;
}
static bool wait_event(uint32_t bits)
{
    uint32_t start=timer_us(),yield=OSTimeGet();
    do {
        uint32_t v=rd(INTR);
        if(v&ERRMASK)return false;
        if(v&bits){wr(INTR,bits);return true;}
        if((uint32_t)(OSTimeGet()-yield)>=1u){OSTimeDly(1);yield=OSTimeGet();}
    }while((uint32_t)(timer_us()-start)<250000u);
    return false;
}
static void gpio_mode(unsigned pin,unsigned mode)
{
    uint32_t reg=GPIO_BASE+(pin/10u)*4u,shift=(pin%10u)*3u;
    mmio_write(reg,(mmio_read(reg)&~(7u<<shift))|(mode<<shift));
}
static void gpio_pull(unsigned pin,unsigned mode)
{
    mmio_write(GPIO_BASE+0x94u,mode);delay_us(5);
    mmio_write(GPIO_BASE+0x98u+(pin/32u)*4u,1u<<(pin%32u));delay_us(5);
    mmio_write(GPIO_BASE+0x94u,0);
    mmio_write(GPIO_BASE+0x98u+(pin/32u)*4u,0);
}
static void gpio_write(unsigned pin,bool on)
{ mmio_write(GPIO_BASE+(on?0x1cu:0x28u)+(pin/32u)*4u,1u<<(pin%32u)); }
static bool clock_set(uint32_t base,uint32_t rate)
{
    uint32_t divisor=(base+rate-1u)/rate;
    divisor=(divisor+1u)&~1u;if(divisor<2)divisor=2;
    if(divisor>2046)return false;
    uint32_t d=divisor/2u;
    wr(CTRL1,rd(CTRL1)&~4u);
    wr(CTRL1,(14u<<16)|((d&255u)<<8)|((d&768u)>>2)|1u);
    if(!wait_bits(CTRL1,2,2,100000))return false;
    wr(CTRL1,rd(CTRL1)|4u);delay_us(100);return true;
}
static cy_rslt_t fail(void)
{
    ++net_sdio_errors;
    net_log("[SDIO] failure cmd=%lu interrupt=0x%08lx status=0x%08lx\n",
        (unsigned long)(rd(CMD)>>24),(unsigned long)rd(INTR),(unsigned long)rd(STATUS));
    wr(INTR,rd(INTR)&~0x100u);
    wr(CTRL1,rd(CTRL1)|(3u<<25));
    (void)wait_bits(CTRL1,3u<<25,0,100000);
    return CYHAL_SDIO_RSLT_ERR_FUNC_RET(CYHAL_SDIO_RET_NO_SP_ERRORS);
}
static cy_rslt_t command(unsigned cmd,uint32_t arg,bool data,bool read,uint32_t *response)
{
    if(!wait_bits(STATUS,data?3u:1u,0,250000))return fail();
    wr(INTR,rd(INTR)&~0x100u);
    uint32_t flags=cmd<<24;
    if(cmd!=0)flags|=(cmd==7?3u:2u)<<16;
    if(cmd!=0 && cmd!=5)flags|=(1u<<19)|(1u<<20);
    if(data)flags|=(1u<<21)|(1u<<1)|(read?(1u<<4):0u)|
                         ((rd(BLKSZ)>>16)>1u?(1u<<5):0u);
    wr(ARG,arg);wr(CMD,flags);++net_sdio_commands;
    if(!wait_event(1))return fail();
    uint32_t r=rd(RESP);if(response)*response=r;
    if((cmd==52 || cmd==53) && (r&0xcb00u))return fail();
    if(cmd==3 && (r&0xe000u))return fail();
    if(cmd==7 && !wait_event(2))return fail();
    return CY_RSLT_SUCCESS;
}
cy_rslt_t cyhal_sdio_send_cmd(const cyhal_sdio_t *obj,cyhal_transfer_t dir,
    cyhal_sdio_command_t cmd,uint32_t arg,uint32_t *response)
{
    (void)obj;(void)dir;
    if(!initialized)return CYHAL_SDIO_RSLT_ERR_BAD_PARAM;
    if(!net_sem_wait(bus_lock,UINT32_MAX))return CYHAL_SDIO_RSLT_ERR_BAD_PARAM;
    cy_rslt_t result=command((unsigned)cmd,arg,false,false,response);
    OSSemPost(bus_lock);return result;
}
cy_rslt_t cyhal_sdio_bulk_transfer(cyhal_sdio_t *obj,cyhal_transfer_t dir,uint32_t arg,
    const uint32_t *data,uint16_t length,uint32_t *response)
{
    (void)obj;
    if(!initialized || !data || !length || ((arg>>31)&1u)!=(dir==CYHAL_WRITE))
        return CYHAL_SDIO_RSLT_ERR_BAD_PARAM;
    uint32_t count=arg&511u,block_size,blocks;
    if(arg&(1u<<27)){blocks=count?count:512u;block_size=64;}
    else {blocks=1;block_size=count?count:512u;}
    /* WHD may request the last padded 64-byte block. Only access length bytes
     * of the caller buffer; wire padding is zero on TX and discarded on RX. */
    uint32_t wire_size=blocks*block_size;
    if(length>wire_size || ((arg&(1u<<27)) ? wire_size-length>=64u : length!=wire_size))return CYHAL_SDIO_RSLT_ERR_BAD_PARAM;
    if(!net_sem_wait(bus_lock,UINT32_MAX))return CYHAL_SDIO_RSLT_ERR_BAD_PARAM;
    wr(BLKSZ,(blocks<<16)|block_size);
    cy_rslt_t result=command(53,arg,true,dir==CYHAL_READ,response);
    uint8_t *bytes=(uint8_t *)(uintptr_t)data;
    for(uint32_t b=0;!result && b<blocks;b++){
        if(!wait_event(dir==CYHAL_READ?32u:16u)){result=fail();break;}
        for(uint32_t p=0;p<block_size;p+=4){
            uint32_t word=0,offset=b*block_size+p;
            uint32_t n=block_size-p;if(n>4)n=4;
            if(dir==CYHAL_READ){
                word=rd(DATA);
                for(uint32_t j=0;j<n && offset+j<length;j++)bytes[offset+j]=(uint8_t)(word>>(8*j));
            }else{
                for(uint32_t j=0;j<n && offset+j<length;j++)word|=(uint32_t)bytes[offset+j]<<(8*j);
                wr(DATA,word);
            }
        }
    }
    if(!result && !wait_event(2))result=fail();
    OSSemPost(bus_lock);return result;
}
void cyhal_sdio_register_irq(cyhal_sdio_t *obj,cyhal_sdio_irq_handler_t fn,void *arg)
{ (void)obj;OS_CPU_SR sr=OS_CPU_SR_Save();callback_arg=arg;callback=fn;OS_CPU_SR_Restore(sr); }
void cyhal_sdio_irq_enable(cyhal_sdio_t *obj,cyhal_sdio_irq_event_t event,bool enable)
{ (void)obj;if(event==CYHAL_SDIO_CARD_INTERRUPT)card_events=enable; }
void wifi_sdio_poll_irq(void)
{
    /* Called inside the existing timer ISR, after OSIntEnter. Notify only:
     * never perform SDIO transactions or network processing in the ISR. */
    if(initialized && card_events && callback && (rd(INTR)&0x100u)){
        wr(INTR,0x100u);callback(callback_arg,CYHAL_SDIO_CARD_INTERRUPT);
    }
}
/* This board has no configured separate OOB GPIO. Reject attempts to use it. */
cy_rslt_t cyhal_gpio_init(cyhal_gpio_t pin,cyhal_gpio_direction_t direction,
    cyhal_gpio_drive_mode_t mode,bool val)
{ (void)pin;(void)direction;(void)mode;(void)val;return CYHAL_SDIO_RSLT_ERR_BAD_PARAM; }
void cyhal_gpio_free(cyhal_gpio_t pin){(void)pin;}
void cyhal_gpio_register_irq(cyhal_gpio_t p,uint8_t pri,cyhal_gpio_irq_handler_t h,void *a)
{(void)p;(void)pri;(void)h;(void)a;}
void cyhal_gpio_irq_enable(cyhal_gpio_t p,cyhal_gpio_irq_event_t e,bool on)
{(void)p;(void)e;(void)on;}
static bool board_info(void)
{
    const uint32_t clock_req[8]={32,0,0x00030002,8,4,1,0,0};
    const uint32_t serial_req[8]={32,0,0x00010004,8,0,0,0,0};
    for(unsigned i=0;i<8;i++){clock_message[i]=clock_req[i];serial_message[i]=serial_req[i];}
    if(mailbox_property_call(clock_message,32,500000)!=MAILBOX_OK ||
       clock_message[1]!=0x80000000u || clock_message[4]!=0x80000008u ||
       clock_message[5]!=1 || clock_message[6]<1000000u || clock_message[6]>1000000000u)return false;
    if(mailbox_property_call(serial_message,32,500000)!=MAILBOX_OK ||
       serial_message[1]!=0x80000000u || serial_message[4]!=0x80000008u ||
       (serial_message[5]|serial_message[6])==0)return false;
    uint32_t serial=serial_message[5]^serial_message[6];
    net_board_mac[0]=0x02;net_board_mac[1]=0x52;
    for(unsigned i=0;i<4;i++)net_board_mac[i+2]=(uint8_t)(serial>>(24-i*8));
    return true; /* Locally administered MAC, derived from board serial. */
}
bool wifi_sdio_init(void)
{
    if(attempted)return false;
    attempted=true;
    if(!board_info())return false;
    bus_lock=OSSemCreate(1);if(!bus_lock)return false;
    /* SD card moves to SDHOST; all Wi-Fi images are linked into kernel.img. */
    for(unsigned p=48;p<=53;p++)gpio_mode(p,4);
    for(unsigned p=34;p<=39;p++){gpio_mode(p,7);gpio_pull(p,p==34?0:2);}
    gpio_mode(41,1);gpio_write(41,false);
    /* Dedicated GPCLK2 -> GPIO43, 19.2 MHz / 585.9375 = 32768 Hz. */
    const uint32_t ctl=PERIPHERAL_BASE+0x101080u,div=ctl+4;
    mmio_write(ctl,0x5a000020u); /* kill clock before divider change */
    uint32_t start=timer_us();
    while(mmio_read(ctl)&128u)if((uint32_t)(timer_us()-start)>100000u)return false;
    mmio_write(div,0x5a000000u|(585u<<12)|3840u);
    mmio_write(ctl,0x5a000000u|(1u<<9)|1u|16u);
    gpio_mode(43,4);OSTimeDly(15);gpio_write(41,true);OSTimeDly(15);
    wr(SIGNAL,0);wr(CTRL1,1u<<24);
    if(!wait_bits(CTRL1,1u<<24,0,100000))return false;
    wr(CTRL0,0);wr(0x3c,0);wr(MASK,0xffffffffu);wr(INTR,0xffffffffu);
    if(!clock_set(clock_message[6],400000))return false;
    uint32_t response;
    if(command(0,0,false,false,NULL))return false;
    if(command(5,0,false,false,&response))return false;
    if((response&0x00ff8000u)==0 || ((response>>28)&7u)<2u)return false;
    uint32_t voltage=response&0x00300000u;
    if(!voltage)return false;
    start=timer_us();
    do{
        if(command(5,voltage,false,false,&response))return false;
        if(response&(1u<<31))break;
        OSTimeDly(1);
    }while((uint32_t)(timer_us()-start)<1000000u);
    if(!(response&(1u<<31)))return false;
    if(command(3,0,false,false,&response))return false;
    uint32_t rca=response&0xffff0000u;if(!rca)return false;
    if(command(7,rca,false,false,NULL))return false;
    /* Host is set to four-bit before WHD changes CCCR with CMD52 (command-only). */
    wr(CTRL0,2);if(!clock_set(clock_message[6],25000000u))return false;
    initialized=true;net_log("[SDIO] selected card RCA=%08lx, 25 MHz PIO\n",(unsigned long)rca);
    return true;
}
