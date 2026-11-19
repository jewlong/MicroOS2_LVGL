/* Test-only: execute pure parser/queue paths on the actual ARM CPU port. */
#include "usb_host.h"
#include "bsp.h"
#include "net_platform.h"
#include <string.h>
volatile uint32_t usb_unit_pass,usb_unit_checks;
void usb_unit_test(void);
static key_event_t captured[64];
static unsigned captured_count;
static void check_line(bool ok,unsigned line){if(!ok){net_log("[TEST] USB check failed at line %u\n",line);bsp_panic("USB unit check");}++usb_unit_checks;}
#define check(x) check_line((x),__LINE__)
static void capture(const key_event_t *e){check(captured_count<64);captured[captured_count++]=*e;}
void usb_unit_test(void)
{
    hid_keyboard_t s;hid_keyboard_init(&s);
    uint8_t r[8]={2,0,4,4,0,0,0,0}; /* duplicate Shift+A */
    check(hid_keyboard_report(&s,r,8,capture));check(captured_count==1);
    check(captured[0].key=='A' && captured[0].pressed);
    check(hid_keyboard_report(&s,r,8,capture));check(captured_count==1);
    r[0]=0;check(hid_keyboard_report(&s,r,8,capture));check(captured_count==1);
    r[2]=r[3]=1;check(!hid_keyboard_report(&s,r,8,capture));check(captured_count==1);
    memset(r,0,8);check(hid_keyboard_report(&s,r,8,capture));
    check(captured_count==2 && !captured[1].pressed && captured[1].key=='A');
    r[2]=57;check(hid_keyboard_report(&s,r,8,capture));check(hid_keyboard_leds(&s)==2);
    r[2]=5;check(hid_keyboard_report(&s,r,8,capture));check(captured[2].key=='B');
    r[0]=2;r[2]=6;check(hid_keyboard_report(&s,r,8,capture));
    check(captured[3].key=='B' && !captured[3].pressed);
    check(captured[4].key=='c' && captured[4].pressed);
    check(!hid_keyboard_report(&s,r,7,capture));check(captured_count==5);
    hid_keyboard_release(&s,capture);check(captured_count==6 && captured[5].key=='c' && !captured[5].pressed);
    r[0]=0;for(unsigned i=0;i<6;i++)r[i+2]=4+i;
    check(hid_keyboard_report(&s,r,8,capture));check(captured_count==12);
    hid_keyboard_release(&s,capture);check(captured_count==18);
    uint8_t config[]={9,2,34,0,1,1,0,0x80,50, 9,4,7,0,1,3,1,1,0,
        9,0x21,0x11,1,0,1,0x22,63,0, 7,5,0x81,3,8,0,10};
    usb_keyboard_device_t d={0};check(usb_parse_config(config,sizeof(config),true,&d)==USB_OK);
    check(d.interface_number==7 && d.endpoint==1 && d.packet_size==8);
    config[32]=2;check(usb_parse_config(config,sizeof(config),false,&d)==USB_BAD_DESCRIPTOR);config[32]=0;
    config[31]=64;check(usb_parse_config(config,sizeof(config),true,&d)==USB_BAD_DESCRIPTOR);
    check(usb_parse_config(config,sizeof(config),false,&d)==USB_OK);config[31]=8;
    config[16]=0;check(usb_parse_config(config,sizeof(config),false,&d)==USB_UNSUPPORTED);config[16]=1;
    config[27]=0;check(usb_parse_config(config,sizeof(config),false,&d)==USB_BAD_DESCRIPTOR);config[27]=7;
    check(usb_parse_config(config,sizeof(config)-1,false,&d)==USB_BAD_DESCRIPTOR);
    config[13]=2;check(usb_parse_config(config,sizeof(config),false,&d)==USB_BAD_DESCRIPTOR);config[13]=1;
    key_event_t event={42,4,0,1,0},got;
    keyboard_all_up();check(keyboard_pop(&got) && got.reset);check(!keyboard_pop(&got));
    for(unsigned i=0;i<65;i++)keyboard_post(&event);
    check(usb_queue_drops==1);check(keyboard_pop(&got) && got.reset);check(!keyboard_pop(&got));
    for(unsigned i=0;i<100;i++){
        event.key=i;keyboard_post(&event);event.key=999;
        check(keyboard_pop(&got) && got.key==i && !got.reset);
    }
    usb_key_presses=0;usb_queue_drops=0;
    usb_unit_pass=1;net_log("[TEST] USB parser/queue checks=%lu PASS\n",(unsigned long)usb_unit_checks);
}
