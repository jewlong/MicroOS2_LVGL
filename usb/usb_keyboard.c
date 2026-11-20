/* SPDX-License-Identifier: Apache-2.0 */
#include "usb_host.h"
#include "mouse_queue.h"
#include "net_platform.h"
#include <string.h>
volatile uint32_t usb_state,usb_error,usb_reports,usb_connects,usb_disconnects,usb_rollovers,usb_modifiers;
volatile uint32_t usb_step,usb_vid_pid;
volatile uint32_t usb_init_err,usb_init_val;
static usb_keyboard_device_t device,kbd_dev,mouse_dev;
static bool has_kbd,has_mouse;
static hid_keyboard_t keyboard;
static uint8_t configuration[1024],report[64];
extern volatile uint32_t app_preempt_pass;
static usb_result_t request(uint8_t type,uint8_t code,uint16_t value,uint16_t index,void *buf,uint16_t length,uint16_t *got)
{return usb_control(&device,type,code,value,index,buf,length,got);}

static usb_result_t configure_hid_device(const uint8_t *descriptor,uint8_t m)
{
    uint16_t n;
    usb_step=6;
    usb_result_t r=request(0x80,6,0x200,0,configuration,9,&n);if(r)return r;
    if(n!=9 || configuration[0]!=9 || configuration[1]!=2)return USB_BAD_DESCRIPTOR;
    uint16_t total=configuration[2]|((uint16_t)configuration[3]<<8);
    if(total<9 || total>sizeof(configuration))return USB_BAD_DESCRIPTOR;
    usb_step=7;
    r=request(0x80,6,0x200,0,configuration,total,&n);if(r)return r;
    if(n!=total)return USB_BAD_DESCRIPTOR;
    usb_step=8;
    usb_keyboard_device_t cand_kbd=device,cand_mouse=device;
    bool fk=false,fm=false;
    if(has_mouse && !has_kbd){
        r=usb_parse_config(configuration,total,device.low_speed,&cand_kbd);
        fk=(r==USB_OK);
    }else{
        r=usb_parse_hid_config(configuration,total,device.low_speed,&cand_kbd,&fk,&cand_mouse,&fm);
    }
    if(r)return r;
    uint8_t cfg=fk?cand_kbd.configuration:cand_mouse.configuration;
    usb_step=9;
    r=request(0,9,cfg,0,NULL,0,&n);if(r)return r;
    usb_step=10;
    if(fk && !has_kbd){
        r=request(0x21,11,0,cand_kbd.interface_number,NULL,0,&n); /* SET_PROTOCOL(BOOT) */
        if(r!=USB_OK && r!=USB_STALL)return r;
        r=request(0x21,10,20u<<8,cand_kbd.interface_number,NULL,0,&n); /* idle 80ms */
        if(r!=USB_OK && r!=USB_STALL)return r;
        kbd_dev=cand_kbd;has_kbd=true;
        net_log("[USB] HID boot keyboard VID=%04x PID=%04x %s EP=%u MPS=%u\n",
            descriptor[8]|descriptor[9]<<8,descriptor[10]|descriptor[11]<<8,
            kbd_dev.low_speed?"low-speed":"full-speed",kbd_dev.endpoint,kbd_dev.packet_size);
    }
    if(fm && !has_mouse){
        r=request(0x21,11,0,cand_mouse.interface_number,NULL,0,&n); /* SET_PROTOCOL(BOOT) */
        if(r!=USB_OK && r!=USB_STALL)return r;
        (void)request(0x21,10,0,cand_mouse.interface_number,NULL,0,&n);
        mouse_dev=cand_mouse;has_mouse=true;usb_mouse_connected=1u;
        net_log("[USB] HID boot mouse VID=%04x PID=%04x %s EP=%u MPS=%u\n",
            descriptor[8]|descriptor[9]<<8,descriptor[10]|descriptor[11]<<8,
            mouse_dev.low_speed?"low-speed":"full-speed",mouse_dev.endpoint,mouse_dev.packet_size);
    }
    (void)m;
    return (has_kbd || has_mouse)?USB_OK:USB_UNSUPPORTED;
}

static usb_result_t enumerate_via_hub(uint8_t hub_ep0,bool hub_low_speed)
{
    uint16_t n;
    usb_step=5;
    usb_result_t r=request(0x80,6,0x200,0,configuration,9,&n);if(r)return r;
    if(n<9 || configuration[1]!=2 || !configuration[5])return USB_BAD_DESCRIPTOR;
    r=request(0,9,configuration[5],0,NULL,0,&n);if(r)return r;
    OSTimeDly(2);

    uint8_t hub_desc[16];
    memset(hub_desc,0,sizeof(hub_desc));
    r=request(0xa0,6,0x2900,0,hub_desc,9,&n);
    if(r)r=request(0xa0,6,0,0,hub_desc,9,&n);
    uint8_t num_ports=(r==USB_OK && n>=3 && hub_desc[2]>=1 && hub_desc[2]<=7)?hub_desc[2]:4;

    /* Power on all hub downstream ports (PORT_POWER = 8) */
    for(uint8_t p=1;p<=num_ports;++p){
        (void)request(0x23,3,8,p,NULL,0,&n);
    }
    OSTimeDly(12); /* Wait 120ms for hub port power stabilization */

    bool hub_hs = usb_host_high_speed();
    for(uint8_t p=1;p<=num_ports;++p){
        /* Re-select hub at address 1 */
        memset(&device,0,sizeof(device));
        device.address=1;device.ep0=hub_ep0;device.low_speed=hub_low_speed;

        uint8_t pstat[4]={0,0,0,0};
        r=request(0xa3,0,0,p,pstat,4,&n);
        if(r!=USB_OK || n!=4)continue;
        if(pstat[2]&1u)(void)request(0x23,1,16,p,NULL,0,&n); /* Clear C_PORT_CONNECTION */
        if(!(pstat[0]&1u))continue; /* No device on this port */

        /* Reset downstream port (PORT_RESET = 4) */
        r=request(0x23,3,4,p,NULL,0,&n);
        if(r!=USB_OK)continue;
        OSTimeDly(6);
        r=request(0xa3,0,0,p,pstat,4,&n);
        if(r!=USB_OK || n!=4)continue;
        (void)request(0x23,1,20,p,NULL,0,&n); /* Clear C_PORT_RESET */
        OSTimeDly(2);
        if((pstat[0]&3u)!=3u)continue; /* Port not enabled */

        bool child_low=(pstat[1]&2u)!=0; /* Bit 9 of wPortStatus = PORT_LOW_SPEED */
        bool child_hs=(pstat[1]&4u)!=0;  /* Bit 10 of wPortStatus = PORT_HIGH_SPEED */
        memset(&device,0,sizeof(device));
        device.address=0;device.ep0=8;device.low_speed=child_low;
        device.hub_addr=1;device.hub_port=p;device.hub_hs=(hub_hs && !child_hs);

        uint8_t desc[18];
        r=request(0x80,6,0x100,0,desc,8,&n);
        if(r!=USB_OK || n!=8 || desc[0]!=18 || desc[1]!=1)continue;
        uint8_t m=desc[7];
        if(m!=8 && m!=16 && m!=32 && m!=64)continue;
        device.ep0=m;
        uint8_t child_addr=(uint8_t)(1u+p);
        r=request(0,5,child_addr,0,NULL,0,&n);
        if(r!=USB_OK)continue;
        device.address=child_addr;
        OSTimeDly(2);
        r=request(0x80,6,0x100,0,desc,18,&n);
        if(r!=USB_OK || n!=18 || desc[1]!=1 || !desc[17])continue;
        usb_vid_pid=((uint32_t)(desc[8]|(desc[9]<<8))<<16)|(uint32_t)(desc[10]|(desc[11]<<8));
        if(desc[4]==9)continue; /* Skip nested hub */
        (void)configure_hid_device(desc,m);
        if(has_kbd && has_mouse)return USB_OK;
    }
    return (has_kbd || has_mouse)?USB_OK:USB_UNSUPPORTED;
}

static usb_result_t enumerate(void)
{
    has_kbd=false;has_mouse=false;usb_mouse_connected=0u;
    memset(&device,0,sizeof(device));device.ep0=8;
    usb_step=1;
    usb_result_t r=usb_host_reset_port(&device.low_speed);if(r)return r;
    uint8_t descriptor[18];uint16_t n;
    usb_step=2;
    r=request(0x80,6,0x100,0,descriptor,8,&n);
    if(r)return r;
    if(n!=8 || descriptor[0]!=18 || descriptor[1]!=1)return USB_BAD_DESCRIPTOR;
    uint8_t m=descriptor[7];
    if((m!=8 && m!=16 && m!=32 && m!=64) || (device.low_speed && m!=8))return USB_BAD_DESCRIPTOR;
    device.ep0=m;
    usb_step=3;
    r=request(0,5,1,0,NULL,0,&n);
    if(r){
        /* Some keyboards need a fresh port reset after an 8-byte truncated GET_DESCRIPTOR */
        if(usb_host_reset_port(&device.low_speed)!=USB_OK)return r;
        device.ep0=m;
        r=request(0,5,1,0,NULL,0,&n);
        if(r)return r;
    }
    device.address=1;OSTimeDly(2);
    usb_step=4;
    r=request(0x80,6,0x100,0,descriptor,18,&n);if(r)return r;
    if(n!=18 || descriptor[1]!=1 || descriptor[7]!=m || !descriptor[17])return USB_BAD_DESCRIPTOR;
    usb_vid_pid=((uint32_t)(descriptor[8]|(descriptor[9]<<8))<<16)|(uint32_t)(descriptor[10]|(descriptor[11]<<8));
    if(descriptor[4]==9)return enumerate_via_hub(m,device.low_speed);
    return configure_hid_device(descriptor,m);
}
static void emit_key(const key_event_t *e)
{
    keyboard_post(e);
    if(e->pressed)net_log("[KEY] usage=%02x mods=%02x key=%lu\n",e->usage,e->modifiers,(unsigned long)e->key);
}
#ifdef USB_UNIT
void usb_unit_test(void);
#endif
void usb_keyboard_task(void *arg)
{
    (void)arg;while(!app_preempt_pass)OSTimeDly(1);
    #ifdef USB_UNIT
    usb_unit_test();
    #endif
    usb_state=1;
    if(!usb_host_init()){
        usb_state=255;usb_error=USB_IO;
        net_log("[USB] controller unavailable; GUI/Wi-Fi/UART continue.\n");
        for(;;)OSTimeDly(100);
    }
    usb_state=2;net_log("[USB] waiting for directly attached boot keyboard.\n");
    for(;;){
        while(!usb_host_connected()){usb_state=2;OSTimeDly(10);}
        if(usb_state!=254)usb_state=3;
        usb_result_t r=USB_IO;
        for(unsigned attempt=0;attempt<3 && usb_host_connected();++attempt){
            r=enumerate();
            if(r==USB_OK || r==USB_BAD_DESCRIPTOR || r==USB_UNSUPPORTED)break;
            OSTimeDly(15);
        }
        if(r){
            usb_error=r;usb_state=254;net_log("[USB] enumeration failed code=%u step=%lu; retrying.\n",r,(unsigned long)usb_step);
            if(!usb_host_operational()){usb_host_init();}
            if(r==USB_BAD_DESCRIPTOR){
                while(usb_host_connected())OSTimeDly(10);
                usb_state=2;continue;
            }
            for(unsigned w=0;w<10 && usb_host_connected();++w)OSTimeDly(10);
            if(!usb_host_connected())usb_state=2;
            continue;
        }
        ++usb_connects;usb_state=4;usb_error=0;hid_keyboard_init(&keyboard);keyboard_all_up();
        uint8_t kbd_pid=0,mouse_pid=0,last_led=255;unsigned errors=0;
        while(usb_host_connected()){
            if(has_kbd){
                uint16_t got=0;
                r=usb_interrupt_in(&kbd_dev,kbd_pid,report,&got);
                if(r==USB_OK){
                    kbd_pid^=2;errors=0;++usb_reports;
                    if(!hid_keyboard_report(&keyboard,report,got,emit_key))++usb_rollovers;
                    usb_modifiers=keyboard.modifiers;
                }else if(r==USB_NAK || r==USB_TIMEOUT){errors=0;}
                else if(r==USB_REMOVED)break;
                else {usb_error=r;if(++errors>=3)break;}
                uint8_t leds=hid_keyboard_leds(&keyboard);
                if(leds!=last_led){
                    r=usb_control(&kbd_dev,0x21,9,0x200,kbd_dev.interface_number,&leds,1,&got);
                    if(r!=USB_OK && r!=USB_STALL){usb_error=r;break;}
                    last_led=leds;
                }
            }
            if(has_mouse){
                uint16_t got=0;
                r=usb_interrupt_in(&mouse_dev,mouse_pid,report,&got);
                if(r==USB_OK){
                    mouse_pid^=2;errors=0;
                    if(usb_mouse_reports<3u){
                        net_log("[USB] mouse got=%u [%02x %02x %02x %02x]\n",
                                got,report[0],got>1u?report[1]:0u,got>2u?report[2]:0u,got>3u?report[3]:0u);
                    }
                    if(got>=3u)mouse_post((int8_t)report[1],(int8_t)report[2],report[0]);
                }else if(r==USB_NAK || r==USB_TIMEOUT){errors=0;}
                else if(r==USB_REMOVED)break;
                else {usb_error=r;if(++errors>=3)break;}
            }
            OSTimeDly(1);
        }
        hid_keyboard_release(&keyboard,keyboard_post);keyboard_all_up();mouse_reset();usb_modifiers=0;
        ++usb_disconnects;usb_state=2;net_log("[USB] keyboard disconnected or transfer failed; keys released.\n");
        if(!usb_host_operational()){usb_state=255;for(;;)OSTimeDly(100);}
        /* Persistent fault requires physical reconnect, avoids endless bus resets. */
        while(usb_host_connected())OSTimeDly(10);
    }
}
