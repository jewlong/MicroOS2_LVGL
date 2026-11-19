/* SPDX-License-Identifier: Apache-2.0 */
#include "usb_host.h"
static uint16_t u16(const uint8_t *p){return (uint16_t)(p[0]|((uint16_t)p[1]<<8));}
usb_result_t usb_parse_config(const uint8_t *b,size_t n,bool low,usb_keyboard_device_t *out)
{
    if(n<9 || b[0]!=9 || b[1]!=2 || u16(b+2)!=n || !b[5])return USB_BAD_DESCRIPTOR;
    bool candidate=false,found=false;uint8_t iface=0,declared=0,seen=0;
    usb_keyboard_device_t chosen=*out;
    for(size_t off=9;off<n;){
        if(n-off<2 || b[off]<2 || b[off]>n-off)return USB_BAD_DESCRIPTOR;
        const uint8_t *d=b+off;unsigned len=d[0];
        if(d[1]==4){
            if(len<9 || (candidate && seen!=declared))return USB_BAD_DESCRIPTOR;
            candidate=d[3]==0 && d[5]==3 && (d[6]==1 || d[6]==0) && d[7]==1;
            iface=d[2];declared=d[4];seen=0;
        }else if(d[1]==5 && candidate){
            if(len<7)return USB_BAD_DESCRIPTOR;
            ++seen;uint16_t m=u16(d+4);
            if((d[2]&0x80u) && (d[3]&3u)==3){
                if((d[2]&0x70u) || !(d[2]&15u) || !d[6] || m<8 || m>64 || (low && m!=8))
                    return USB_BAD_DESCRIPTOR;
                if(!found){chosen.interface_number=iface;chosen.endpoint=d[2]&15;
                    chosen.packet_size=m;chosen.interval=d[6];chosen.configuration=b[5];found=true;}
            }
        }
        off+=len;
    }
    if(candidate && seen!=declared)return USB_BAD_DESCRIPTOR;
    if(!found)return USB_UNSUPPORTED;
    *out=chosen;return USB_OK;
}
