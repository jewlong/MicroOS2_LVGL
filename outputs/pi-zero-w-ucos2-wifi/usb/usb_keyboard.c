/* SPDX-License-Identifier: Apache-2.0 */
#include "usb_host.h"
#include "mouse_queue.h"
#include "touch_queue.h"
#include "hid_touch.h"
#include "net_platform.h"
#include <string.h>
#include <stdio.h>

volatile uint32_t usb_state,usb_error,usb_reports,usb_connects,usb_disconnects,usb_rollovers,usb_modifiers;
volatile uint32_t usb_step,usb_vid_pid;
volatile uint32_t usb_init_err,usb_init_val;

static usb_keyboard_device_t device,kbd_dev,mouse_dev,touch_dev;
static bool has_kbd,has_mouse,has_touch,mouse_use_report_proto;
static hid_keyboard_t keyboard;
static hid_touch_layout_t touch_layout;
static hid_mouse_layout_t mouse_layout;
static uint8_t configuration[1024],report_desc[1536],report[64];

static uint8_t last_mouse_raw[4];
static uint8_t last_mouse_got;
static bool root_is_hub,root_hub_low,root_hub_is_hs;
static uint8_t root_hub_ep0,root_hub_ports,next_usb_addr;
static uint16_t root_hub_port_mask;

extern volatile uint32_t app_preempt_pass;

static usb_result_t request(uint8_t type,uint8_t code,uint16_t value,uint16_t index,void *buf,uint16_t length,uint16_t *got)
{return usb_control(&device,type,code,value,index,buf,length,got);}

/* Returns: 0 = not claimed, 1 = claimed touch, 2 = claimed report-protocol mouse */
static int probe_hid_candidate(const usb_hid_touch_candidate_t *cand,const uint8_t *descriptor,bool skip_mouse)
{
    uint16_t want=cand->report_desc_len;
    if(want<12u)want=256u;
    if(want>sizeof(report_desc))want=(uint16_t)sizeof(report_desc);
    uint16_t got=0u;
    /* GET_DESCRIPTOR (HID Report Descriptor = 0x22) directed to interface (0x81) */
    usb_result_t r=request(0x81,6,0x2200,cand->dev.interface_number,report_desc,want,&got);
    if((r!=USB_OK || got<12u) && cand->dev.interface_number==0u){
        r=request(0x80,6,0x2200,0u,report_desc,want,&got);
    }
    if(r!=USB_OK || got<12u)return 0;

    if(!has_touch){
        hid_touch_layout_t tlayout;
        if(hid_touch_parse_report_desc(report_desc,got,&tlayout)){
            uint16_t dummy=0u;
            (void)request(0x21,10,0,cand->dev.interface_number,NULL,0,&dummy);
            touch_dev=cand->dev;
            touch_layout=tlayout;
            has_touch=true;
            usb_touch_connected=1u;
            usb_touch_x_max=(uint32_t)(tlayout.slots[0].x_max>0?tlayout.slots[0].x_max:0);
            usb_touch_y_max=(uint32_t)(tlayout.slots[0].y_max>0?tlayout.slots[0].y_max:0);
            net_log("[USB] HID touch VID=%04x PID=%04x %s IF=%u EP=%u MPS=%u id=%u slots=%u X=%ld..%ld Y=%ld..%ld\n",
                    descriptor[8]|(descriptor[9]<<8),descriptor[10]|(descriptor[11]<<8),
                    touch_dev.low_speed?"low-speed":"full-speed",
                    touch_dev.interface_number,touch_dev.endpoint,touch_dev.packet_size,
                    tlayout.has_report_id?tlayout.report_id:0u,tlayout.num_slots,
                    (long)tlayout.slots[0].x_min,(long)tlayout.slots[0].x_max,
                    (long)tlayout.slots[0].y_min,(long)tlayout.slots[0].y_max);
            return 1;
        }
    }

    if(!has_mouse && !skip_mouse){
        hid_mouse_layout_t mlayout;
        if(hid_mouse_parse_report_desc(report_desc,got,&mlayout)){
            /* Keep device in default Report Protocol (do NOT send SET_PROTOCOL(BOOT), which breaks
             * 2.4GHz wireless combo dongles and Report-ID / 12-bit / 16-bit optical mice). */
            uint16_t dummy=0u;
            (void)request(0x21,10,0,cand->dev.interface_number,NULL,0,&dummy);
            mouse_dev=cand->dev;
            mouse_layout=mlayout;
            mouse_use_report_proto=true;
            has_mouse=true;
            usb_mouse_connected=1u;
            net_log("[USB] HID report mouse VID=%04x PID=%04x %s IF=%u EP=%u MPS=%u id=%u X=%u@%u Y=%u@%u\n",
                    descriptor[8]|(descriptor[9]<<8),descriptor[10]|(descriptor[11]<<8),
                    mouse_dev.low_speed?"low-speed":"full-speed",
                    mouse_dev.interface_number,mouse_dev.endpoint,mouse_dev.packet_size,
                    mlayout.has_report_id?mlayout.report_id:0u,
                    (unsigned)mlayout.x_size,(unsigned)mlayout.x_bit,
                    (unsigned)mlayout.y_size,(unsigned)mlayout.y_bit);
            return 2;
        }
    }
    return 0;
}

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
    usb_result_t r_km;
    if(has_mouse && !has_kbd){
        r_km=usb_parse_config(configuration,total,device.low_speed,&cand_kbd);
        fk=(r_km==USB_OK);
    }else{
        r_km=usb_parse_hid_config(configuration,total,device.low_speed,&cand_kbd,&fk,&cand_mouse,&fm);
    }
    usb_hid_touch_candidate_t touch_cands[USB_MAX_TOUCH_CANDIDATES];
    size_t touch_cand_cnt=0u;
    usb_result_t r_tc=USB_UNSUPPORTED;
    if(!has_touch || !has_mouse){
        r_tc=usb_parse_hid_touch_candidates(configuration,total,device.low_speed,&device,
                                            touch_cands,USB_MAX_TOUCH_CANDIDATES,&touch_cand_cnt);
    }
    if(r_km==USB_BAD_DESCRIPTOR || r_tc==USB_BAD_DESCRIPTOR)return USB_BAD_DESCRIPTOR;
    if(!fk && !fm && touch_cand_cnt==0u)return USB_UNSUPPORTED;

    uint8_t cfg=fk?cand_kbd.configuration:(fm?cand_mouse.configuration:touch_cands[0].dev.configuration);
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
            descriptor[8]|(descriptor[9]<<8),descriptor[10]|(descriptor[11]<<8),
            kbd_dev.low_speed?"low-speed":"full-speed",kbd_dev.endpoint,kbd_dev.packet_size);
    }
    bool found_touch_here=false;
    if((!has_touch || !has_mouse) && touch_cand_cnt>0u){
        for(size_t i=0u;i<touch_cand_cnt;++i){
            int claimed=probe_hid_candidate(&touch_cands[i],descriptor,found_touch_here);
            if(claimed==1)found_touch_here=true;
            if(has_touch && has_mouse)break;
        }
    }
    /* Fallback to HID Boot Protocol mouse if Report Descriptor fetch/parse was unavailable. */
    if(fm && !has_mouse && !found_touch_here){
        r=request(0x21,11,0,cand_mouse.interface_number,NULL,0,&n); /* SET_PROTOCOL(BOOT) */
        if(r!=USB_OK && r!=USB_STALL)return r;
        (void)request(0x21,10,0,cand_mouse.interface_number,NULL,0,&n);
        mouse_dev=cand_mouse;
        mouse_use_report_proto=false;
        has_mouse=true;
        usb_mouse_connected=1u;
        net_log("[USB] HID boot mouse VID=%04x PID=%04x %s EP=%u MPS=%u\n",
            descriptor[8]|(descriptor[9]<<8),descriptor[10]|(descriptor[11]<<8),
            mouse_dev.low_speed?"low-speed":"full-speed",mouse_dev.endpoint,mouse_dev.packet_size);
    }
    (void)m;
    return (has_kbd || has_mouse || has_touch)?USB_OK:USB_UNSUPPORTED;
}

static bool reset_hub_port(uint8_t hub_addr,uint8_t hub_ep0,bool hub_low,
                           uint8_t tt_addr,uint8_t tt_port,bool tt_hs,
                           uint8_t port,uint8_t pstat_out[4])
{
    uint16_t n=0u;
    memset(&device,0,sizeof(device));
    device.address=hub_addr;device.ep0=hub_ep0;device.low_speed=hub_low;
    device.hub_addr=tt_addr;device.hub_port=tt_port;device.hub_hs=tt_hs;

    if(request(0x23,3,4,port,NULL,0,&n)!=USB_OK)return false; /* PORT_RESET */
    OSTimeDly(5);
    for(unsigned w=0;w<8;++w){
        if(request(0xa3,0,0,port,pstat_out,4,&n)!=USB_OK || n!=4)return false;
        if((pstat_out[2]&0x10u) || (pstat_out[0]&2u))break;
        OSTimeDly(2);
    }
    (void)request(0x23,1,20,port,NULL,0,&n); /* Clear C_PORT_RESET */
    (void)request(0x23,1,16,port,NULL,0,&n); /* Clear C_PORT_CONNECTION */
    OSTimeDly(4); /* 40ms post-reset recovery for optical/wireless mice */
    if(request(0xa3,0,0,port,pstat_out,4,&n)!=USB_OK || n!=4)return false;
    return (pstat_out[0]&3u)==3u;
}

static usb_result_t enumerate_hub_ports(uint8_t hub_addr,uint8_t hub_ep0,bool hub_low_speed,
                                        uint8_t hub_tt_addr,uint8_t hub_tt_port,bool hub_tt_hs,
                                        bool hub_is_hs,uint8_t depth);

static void enumerate_single_hub_port(uint8_t hub_addr,uint8_t hub_ep0,bool hub_low_speed,
                                      uint8_t hub_tt_addr,uint8_t hub_tt_port,bool hub_tt_hs,
                                      bool hub_is_hs,uint8_t p,uint8_t depth)
{
    if(next_usb_addr>15u)return;
    uint8_t pstat[4]={0,0,0,0};
    if(!reset_hub_port(hub_addr,hub_ep0,hub_low_speed,hub_tt_addr,hub_tt_port,hub_tt_hs,p,pstat))return;

    bool child_low=(pstat[1]&2u)!=0; /* Bit 9 of wPortStatus = PORT_LOW_SPEED */
    bool child_hs=(pstat[1]&4u)!=0;  /* Bit 10 of wPortStatus = PORT_HIGH_SPEED */
    uint8_t c_tt_addr=0u,c_tt_port=0u;
    bool c_tt_hs=false;
    if(hub_is_hs && !child_hs){
        c_tt_addr=hub_addr;c_tt_port=p;c_tt_hs=true;
    }else if(hub_tt_hs){
        c_tt_addr=hub_tt_addr;c_tt_port=hub_tt_port;c_tt_hs=true;
    }

    uint16_t n=0u;
    uint8_t desc[18];
    memset(&device,0,sizeof(device));
    device.address=0;device.ep0=8;device.low_speed=child_low;
    device.hub_addr=c_tt_addr;device.hub_port=c_tt_port;device.hub_hs=c_tt_hs;

    usb_result_t r=request(0x80,6,0x100,0,desc,8,&n);
    if(r!=USB_OK || n<8 || desc[1]!=1){
        /* Second reset retry for slow-booting downstream devices */
        if(!reset_hub_port(hub_addr,hub_ep0,hub_low_speed,hub_tt_addr,hub_tt_port,hub_tt_hs,p,pstat))return;
        memset(&device,0,sizeof(device));
        device.address=0;device.ep0=8;device.low_speed=child_low;
        device.hub_addr=c_tt_addr;device.hub_port=c_tt_port;device.hub_hs=c_tt_hs;
        r=request(0x80,6,0x100,0,desc,8,&n);
        if(r!=USB_OK || n<8 || desc[1]!=1)return;
    }
    uint8_t m=desc[7];
    if(m!=8 && m!=16 && m!=32 && m!=64)return;

    /* Windows-style second reset after 8-byte truncated GET_DESCRIPTOR if SET_ADDRESS fails */
    device.ep0=m;
    uint8_t child_addr=next_usb_addr;
    r=request(0,5,child_addr,0,NULL,0,&n);
    if(r!=USB_OK){
        if(!reset_hub_port(hub_addr,hub_ep0,hub_low_speed,hub_tt_addr,hub_tt_port,hub_tt_hs,p,pstat))return;
        memset(&device,0,sizeof(device));
        device.address=0;device.ep0=m;device.low_speed=child_low;
        device.hub_addr=c_tt_addr;device.hub_port=c_tt_port;device.hub_hs=c_tt_hs;
        r=request(0,5,child_addr,0,NULL,0,&n);
        if(r!=USB_OK)return;
    }
    next_usb_addr++;
    device.address=child_addr;
    OSTimeDly(2);
    r=request(0x80,6,0x100,0,desc,18,&n);
    if(r!=USB_OK || n!=18 || desc[1]!=1 || !desc[17])return;
    usb_vid_pid=((uint32_t)(desc[8]|(desc[9]<<8))<<16)|(uint32_t)(desc[10]|(desc[11]<<8));
    if(desc[4]==9){
        if(depth<1u){
            (void)enumerate_hub_ports(child_addr,m,child_low,c_tt_addr,c_tt_port,c_tt_hs,child_hs,(uint8_t)(depth+1u));
        }
        return;
    }
    (void)configure_hid_device(desc,m);
}

static usb_result_t enumerate_hub_ports(uint8_t hub_addr,uint8_t hub_ep0,bool hub_low_speed,
                                        uint8_t hub_tt_addr,uint8_t hub_tt_port,bool hub_tt_hs,
                                        bool hub_is_hs,uint8_t depth)
{
    uint16_t n=0u;
    usb_step=5;
    memset(&device,0,sizeof(device));
    device.address=hub_addr;device.ep0=hub_ep0;device.low_speed=hub_low_speed;
    device.hub_addr=hub_tt_addr;device.hub_port=hub_tt_port;device.hub_hs=hub_tt_hs;

    usb_result_t r=request(0x80,6,0x200,0,configuration,9,&n);if(r)return r;
    if(n<9 || configuration[1]!=2 || !configuration[5])return USB_BAD_DESCRIPTOR;
    r=request(0,9,configuration[5],0,NULL,0,&n);if(r)return r;
    OSTimeDly(2);

    uint8_t hub_desc[16];
    memset(hub_desc,0,sizeof(hub_desc));
    r=request(0xa0,6,0x2900,0,hub_desc,9,&n);
    if(r)r=request(0xa0,6,0,0,hub_desc,9,&n);
    uint8_t num_ports=(r==USB_OK && n>=3 && hub_desc[2]>=1 && hub_desc[2]<=7)?hub_desc[2]:4;

    if(depth==0u){
        root_is_hub=true;
        root_hub_ep0=hub_ep0;
        root_hub_low=hub_low_speed;
        root_hub_is_hs=hub_is_hs;
        root_hub_ports=num_ports;
        root_hub_port_mask=0u;
    }

    for(uint8_t p=1;p<=num_ports;++p){
        (void)request(0x23,3,8,p,NULL,0,&n); /* PORT_POWER */
    }
    OSTimeDly(15); /* 150ms hub port power stabilization */

    for(uint8_t p=1;p<=num_ports;++p){
        memset(&device,0,sizeof(device));
        device.address=hub_addr;device.ep0=hub_ep0;device.low_speed=hub_low_speed;
        device.hub_addr=hub_tt_addr;device.hub_port=hub_tt_port;device.hub_hs=hub_tt_hs;

        uint8_t pstat[4]={0,0,0,0};
        r=request(0xa3,0,0,p,pstat,4,&n);
        if(r!=USB_OK || n!=4)continue;
        if(pstat[2]&1u)(void)request(0x23,1,16,p,NULL,0,&n);
        if(!(pstat[0]&1u))continue;

        if(depth==0u)root_hub_port_mask|=(uint16_t)(1u<<p);
        enumerate_single_hub_port(hub_addr,hub_ep0,hub_low_speed,
                                  hub_tt_addr,hub_tt_port,hub_tt_hs,
                                  hub_is_hs,p,depth);
        if(has_kbd && has_mouse && has_touch)return USB_OK;
    }
    if(depth==0u)return USB_OK; /* Keep hub alive even if devices hot-plug later */
    return (has_kbd || has_mouse || has_touch)?USB_OK:USB_UNSUPPORTED;
}

static usb_result_t enumerate(void)
{
    has_kbd=false;has_mouse=false;has_touch=false;mouse_use_report_proto=false;
    root_is_hub=false;root_hub_ports=0u;root_hub_port_mask=0u;next_usb_addr=2u;
    last_mouse_got=0u;memset(last_mouse_raw,0,sizeof(last_mouse_raw));
    usb_mouse_connected=0u;mouse_reset();touch_reset();
    memset(&device,0,sizeof(device));device.ep0=8;
    usb_step=1;
    usb_result_t r=usb_host_reset_port(&device.low_speed);if(r)return r;
    uint8_t descriptor[18];uint16_t n;
    usb_step=2;
    r=request(0x80,6,0x100,0,descriptor,8,&n);
    if(r){
        if(usb_host_reset_port(&device.low_speed)!=USB_OK)return r;
        OSTimeDly(4);
        r=request(0x80,6,0x100,0,descriptor,8,&n);
        if(r)return r;
    }
    if(n!=8 || descriptor[0]!=18 || descriptor[1]!=1)return USB_BAD_DESCRIPTOR;
    uint8_t m=descriptor[7];
    if((m!=8 && m!=16 && m!=32 && m!=64) || (device.low_speed && m!=8))return USB_BAD_DESCRIPTOR;
    device.ep0=m;
    usb_step=3;
    r=request(0,5,1,0,NULL,0,&n);
    if(r){
        /* Some keyboards/mice need a fresh port reset after an 8-byte truncated GET_DESCRIPTOR */
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
    if(descriptor[4]==9){
        return enumerate_hub_ports(1u,m,device.low_speed,0u,0u,false,usb_host_high_speed(),0u);
    }
    return configure_hid_device(descriptor,m);
}

void usb_diag_line(char *out,size_t max_out)
{
    if(!out || max_out<16u)return;
    uint32_t hprt=usb_diag_hprt();
    const char *spd=(!(hprt&1u))?"無裝置":((((hprt>>17)&3u)==0u)?"HS":((((hprt>>17)&3u)==1u)?"FS":"LS"));
    char topo[48];
    if(root_is_hub){
        snprintf(topo,sizeof(topo),"Hub:%u埠(m:%02x)",(unsigned)root_hub_ports,(unsigned)root_hub_port_mask);
    }else{
        snprintf(topo,sizeof(topo),"OTG直連");
    }
    char mstr[96];
    if(has_mouse){
        snprintf(mstr,sizeof(mstr),"%s A%u/I%u/E%u/M%u id=%u %ub/%ub cnt=%lu [%02x %02x %02x %02x]",
                 mouse_use_report_proto?"RPT":"BOOT",
                 (unsigned)mouse_dev.address,(unsigned)mouse_dev.interface_number,
                 (unsigned)mouse_dev.endpoint,(unsigned)mouse_dev.packet_size,
                 (unsigned)(mouse_use_report_proto && mouse_layout.has_report_id?mouse_layout.report_id:0u),
                 (unsigned)(mouse_use_report_proto?mouse_layout.x_size:8u),
                 (unsigned)(mouse_use_report_proto?mouse_layout.y_size:8u),
                 (unsigned long)usb_mouse_reports,
                 (unsigned)last_mouse_raw[0],(unsigned)last_mouse_raw[1],
                 (unsigned)last_mouse_raw[2],(unsigned)last_mouse_raw[3]);
    }else{
        snprintf(mstr,sizeof(mstr),"未偵測(err=%lu st=%lu vp=%04lx:%04lx)",
                 (unsigned long)usb_error,(unsigned long)usb_step,
                 (unsigned long)(usb_vid_pid>>16),(unsigned long)(usb_vid_pid&0xffffu));
    }
    snprintf(out,max_out,"USB 詳細: %s | %s | KBD:%s(A%u/E%u) | MOUSE:%s",
             spd,topo,
             has_kbd?"OK":"-",(unsigned)(has_kbd?kbd_dev.address:0u),(unsigned)(has_kbd?kbd_dev.endpoint:0u),
             mstr);
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
    usb_state=2;net_log("[USB] waiting for HID keyboard / mouse / touchscreen.\n");
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
        uint8_t kbd_pid=0,mouse_pid=0,touch_pid=0,last_led=255;
        unsigned kbd_err=0,mouse_err=0,touch_err=0,hub_poll_div=0;
        while(usb_host_connected()){
            if(has_kbd){
                uint16_t got=0;
                r=usb_interrupt_in(&kbd_dev,kbd_pid,report,&got);
                if(r==USB_OK){
                    kbd_pid^=2;kbd_err=0;++usb_reports;
                    if(!hid_keyboard_report(&keyboard,report,got,emit_key))++usb_rollovers;
                    usb_modifiers=keyboard.modifiers;
                }else if(r==USB_NAK || r==USB_TIMEOUT){kbd_err=0;}
                else if(r==USB_REMOVED)break;
                else {
                    usb_error=r;
                    if(++kbd_err>=5u){
                        if(root_is_hub && (has_mouse || has_touch)){
                            has_kbd=false;hid_keyboard_release(&keyboard,keyboard_post);keyboard_all_up();
                        }else break;
                    }
                }
                if(has_kbd){
                    uint8_t leds=hid_keyboard_leds(&keyboard);
                    if(leds!=last_led){
                        r=usb_control(&kbd_dev,0x21,9,0x200,kbd_dev.interface_number,&leds,1,&got);
                        if(r!=USB_OK && r!=USB_STALL){usb_error=r;if(!root_is_hub)break;}
                        last_led=leds;
                    }
                }
            }
            if(has_mouse){
                uint16_t got=0;
                r=usb_interrupt_in(&mouse_dev,mouse_pid,report,&got);
                if(r==USB_OK){
                    mouse_pid^=2;mouse_err=0;
                    last_mouse_got=(uint8_t)(got>255u?255u:got);
                    for(unsigned b=0;b<4u;++b)last_mouse_raw[b]=(b<got)?report[b]:0u;
                    if(usb_mouse_reports<3u){
                        net_log("[USB] mouse(%s) got=%u [%02x %02x %02x %02x]\n",
                                mouse_use_report_proto?"RPT":"BOOT",
                                got,last_mouse_raw[0],last_mouse_raw[1],last_mouse_raw[2],last_mouse_raw[3]);
                    }
                    if(mouse_use_report_proto){
                        hid_mouse_sample_t ms;
                        if(hid_mouse_decode_report(&mouse_layout,report,got,&ms)){
                            mouse_post(ms.dx,ms.dy,ms.buttons);
                        }
                    }else if(got>=3u){
                        mouse_post((int16_t)(int8_t)report[1],(int16_t)(int8_t)report[2],report[0]);
                    }
                }else if(r==USB_NAK || r==USB_TIMEOUT){mouse_err=0;}
                else if(r==USB_REMOVED)break;
                else {
                    usb_error=r;
                    if(++mouse_err>=8u){
                        if(has_kbd || has_touch || root_is_hub){
                            if(root_is_hub && mouse_dev.hub_addr==1u && mouse_dev.hub_port>=1u && mouse_dev.hub_port<=7u){
                                root_hub_port_mask&=(uint16_t)~(1u<<mouse_dev.hub_port);
                            }
                            has_mouse=false;mouse_reset();mouse_pid=0;mouse_err=0;
                        }else break;
                    }
                }
            }
            if(has_touch){
                uint16_t got=0;
                r=usb_interrupt_in(&touch_dev,touch_pid,report,&got);
                if(r==USB_OK){
                    touch_pid^=2;touch_err=0;
                    hid_touch_sample_t sample;
                    if(hid_touch_decode_report(&touch_layout,report,got,&sample)){
                        if(usb_touch_reports<3u){
                            net_log("[USB] touch got=%u down=%u raw=(%ld,%ld) norm=(%u,%u)\n",
                                    got,sample.pressed?1u:0u,(long)sample.raw_x,(long)sample.raw_y,
                                    (unsigned)sample.x_norm,(unsigned)sample.y_norm);
                        }
                        touch_post(sample.x_norm,sample.y_norm,sample.pressed);
                    }
                }else if(r==USB_NAK || r==USB_TIMEOUT){touch_err=0;}
                else if(r==USB_REMOVED)break;
                else {
                    usb_error=r;
                    if(++touch_err>=8u){
                        if(has_kbd || has_mouse || root_is_hub){
                            has_touch=false;touch_reset();touch_pid=0;touch_err=0;
                        }else break;
                    }
                }
            }
            /* Periodic downstream hot-plug / unplug check on USB Hub ports (~500ms) */
            if(root_is_hub && ++hub_poll_div>=50u){
                hub_poll_div=0u;
                for(uint8_t p=1;p<=root_hub_ports;++p){
                    uint16_t got=0u;
                    uint8_t pstat[4]={0,0,0,0};
                    memset(&device,0,sizeof(device));
                    device.address=1u;device.ep0=root_hub_ep0;device.low_speed=root_hub_low;
                    if(request(0xa3,0,0,p,pstat,4,&got)!=USB_OK || got!=4u)continue;
                    bool conn=(pstat[0]&1u)!=0;
                    bool was_conn=(root_hub_port_mask&(uint16_t)(1u<<p))!=0;
                    bool changed=(pstat[2]&1u)!=0;
                    if(changed)(void)request(0x23,1,16,p,NULL,0,&got);
                    if((!conn && was_conn) || (conn && changed && was_conn)){
                        root_hub_port_mask&=(uint16_t)~(1u<<p);
                        if(has_mouse && mouse_dev.hub_addr==1u && mouse_dev.hub_port==p){
                            has_mouse=false;mouse_reset();mouse_pid=0;mouse_err=0;
                        }
                        if(has_kbd && kbd_dev.hub_addr==1u && kbd_dev.hub_port==p){
                            has_kbd=false;hid_keyboard_release(&keyboard,keyboard_post);keyboard_all_up();kbd_pid=0;kbd_err=0;last_led=255;
                        }
                        if(has_touch && touch_dev.hub_addr==1u && touch_dev.hub_port==p){
                            has_touch=false;touch_reset();touch_pid=0;touch_err=0;
                        }
                        was_conn=false;
                    }
                    if(conn && !was_conn){
                        root_hub_port_mask|=(uint16_t)(1u<<p);
                        if(next_usb_addr>14u)next_usb_addr=8u;
                        bool prev_m=has_mouse,prev_k=has_kbd,prev_t=has_touch;
                        enumerate_single_hub_port(1u,root_hub_ep0,root_hub_low,0u,0u,false,root_hub_is_hs,p,0u);
                        if(!prev_m && has_mouse){mouse_pid=0;mouse_err=0;}
                        if(!prev_k && has_kbd){kbd_pid=0;kbd_err=0;last_led=255;}
                        if(!prev_t && has_touch){touch_pid=0;touch_err=0;}
                    }
                }
            }
            (void)last_mouse_got;
            OSTimeDly(1);
        }
        hid_keyboard_release(&keyboard,keyboard_post);keyboard_all_up();mouse_reset();touch_reset();usb_modifiers=0;
        ++usb_disconnects;usb_state=2;net_log("[USB] HID device disconnected or transfer failed; inputs released.\n");
        if(!usb_host_operational()){usb_state=255;for(;;)OSTimeDly(100);}
        /* Persistent fault requires physical reconnect, avoids endless bus resets. */
        while(usb_host_connected())OSTimeDly(10);
    }
}
