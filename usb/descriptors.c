/* SPDX-License-Identifier: Apache-2.0 */
#include "usb_host.h"
static uint16_t u16(const uint8_t *p){return (uint16_t)(p[0]|((uint16_t)p[1]<<8));}

usb_result_t usb_parse_hid_config(const uint8_t *b,size_t n,bool low,
                                  usb_keyboard_device_t *kbd_out,bool *has_kbd,
                                  usb_keyboard_device_t *mouse_out,bool *has_mouse)
{
    if(has_kbd)*has_kbd=false;
    if(has_mouse)*has_mouse=false;
    if(n<9 || b[0]!=9 || b[1]!=2 || u16(b+2)!=n || !b[5])return USB_BAD_DESCRIPTOR;
    uint8_t cand_proto=0; /* 0=none, 1=kbd, 2=mouse */
    bool found_kbd=false,found_mouse=false;
    uint8_t iface=0,declared=0,seen=0;
    usb_keyboard_device_t chosen_kbd,chosen_mouse;
    if(kbd_out)chosen_kbd=*kbd_out; else chosen_kbd=(usb_keyboard_device_t){0};
    if(mouse_out)chosen_mouse=*mouse_out; else chosen_mouse=(usb_keyboard_device_t){0};

    for(size_t off=9;off<n;){
        if(n-off<2 || b[off]<2 || b[off]>n-off)return USB_BAD_DESCRIPTOR;
        const uint8_t *d=b+off;unsigned len=d[0];
        if(d[1]==4){
            if(len<9 || (cand_proto && seen!=declared))return USB_BAD_DESCRIPTOR;
            cand_proto=0;
            if(d[3]==0 && d[5]==3 && (d[6]==1 || d[6]==0)){
                if(d[7]==1 && kbd_out)cand_proto=1;
                else if(d[7]==2 && mouse_out)cand_proto=2;
            }
            iface=d[2];declared=d[4];seen=0;
        }else if(d[1]==5 && cand_proto){
            if(len<7)return USB_BAD_DESCRIPTOR;
            ++seen;uint16_t m=u16(d+4);
            if((d[2]&0x80u) && (d[3]&3u)==3){
                uint16_t min_mps=(cand_proto==1)?8u:3u;
                if((d[2]&0x70u) || !(d[2]&15u) || !d[6] || m<min_mps || m>64 || (low && m>8))
                    return USB_BAD_DESCRIPTOR;
                if(cand_proto==1 && !found_kbd){
                    chosen_kbd.interface_number=iface;chosen_kbd.endpoint=d[2]&15;
                    chosen_kbd.packet_size=m;chosen_kbd.interval=d[6];chosen_kbd.configuration=b[5];
                    found_kbd=true;
                }else if(cand_proto==2 && !found_mouse){
                    chosen_mouse.interface_number=iface;chosen_mouse.endpoint=d[2]&15;
                    chosen_mouse.packet_size=m;chosen_mouse.interval=d[6];chosen_mouse.configuration=b[5];
                    found_mouse=true;
                }
            }
        }
        off+=len;
    }
    if(cand_proto && seen!=declared)return USB_BAD_DESCRIPTOR;
    if(!found_kbd && !found_mouse)return USB_UNSUPPORTED;
    if(found_kbd && kbd_out){*kbd_out=chosen_kbd;if(has_kbd)*has_kbd=true;}
    if(found_mouse && mouse_out){*mouse_out=chosen_mouse;if(has_mouse)*has_mouse=true;}
    return USB_OK;
}

usb_result_t usb_parse_config(const uint8_t *b,size_t n,bool low,usb_keyboard_device_t *out)
{
    bool hk=false;
    usb_result_t r=usb_parse_hid_config(b,n,low,out,&hk,NULL,NULL);
    if(r!=USB_OK)return r;
    return hk?USB_OK:USB_UNSUPPORTED;
}
