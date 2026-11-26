/* Test-only: execute pure parser/queue paths on the actual ARM CPU port. */
#include "usb_host.h"
#include "hid_touch.h"
#include "mouse_queue.h"
#include "touch_queue.h"
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

    /* --- USB HID Touch Report Descriptor & Decoder tests --- */
    /* 1. Standard Single-Touch Digitizer (Report ID=1, Tip+InRange+6pad, X 0..4095, Y 0..4095) */
    static const uint8_t rdesc_st[]={
        0x05,0x0D, 0x09,0x04, 0xA1,0x01,             /* Usage Page(Digitizers), Usage(Touch Screen), Coll(App) */
        0x85,0x01,                                   /* Report ID (1) */
        0x09,0x22, 0xA1,0x02,                        /* Usage(Finger), Coll(Logical) */
        0x09,0x42, 0x15,0x00, 0x25,0x01,             /* Usage(Tip Switch), LogMin(0), LogMax(1) */
        0x75,0x01, 0x95,0x01, 0x81,0x02,             /* 1-bit Data,Var,Abs */
        0x09,0x32, 0x81,0x02,                        /* Usage(In Range) 1-bit */
        0x95,0x06, 0x81,0x03,                        /* 6-bit Const padding */
        0x05,0x01, 0x09,0x30, 0x09,0x31,             /* Generic Desktop, Usage(X), Usage(Y) */
        0x15,0x00, 0x26,0xFF,0x0F,                   /* LogMin(0), LogMax(4095) */
        0x75,0x10, 0x95,0x02, 0x81,0x02,             /* 16-bit x 2, Data,Var,Abs */
        0xC0, 0xC0
    };
    hid_touch_layout_t tl;
    check(hid_touch_parse_report_desc(rdesc_st,sizeof(rdesc_st),&tl));
    check(tl.valid && tl.has_report_id && tl.report_id==1 && tl.is_digitizer && tl.num_slots==1);
    check(tl.min_report_bytes==6 && tl.slots[0].x_max==4095 && tl.slots[0].y_max==4095);
    hid_touch_sample_t ts;
    uint8_t pkt_st[6]={0x01, 0x03, 0x00,0x08, 0xFF,0x0F}; /* Tip=1, X=2048, Y=4095 */
    check(hid_touch_decode_report(&tl,pkt_st,sizeof(pkt_st),&ts));
    check(ts.pressed && ts.raw_x==2048 && ts.raw_y==4095 && ts.x_norm>=32770 && ts.x_norm<=32780 && ts.y_norm==65535);
    pkt_st[0]=0x02; /* Wrong report ID -> ignored */
    check(!hid_touch_decode_report(&tl,pkt_st,sizeof(pkt_st),&ts));
    pkt_st[0]=0x01; pkt_st[1]=0x02; /* Tip=0 (finger lift) */
    check(hid_touch_decode_report(&tl,pkt_st,sizeof(pkt_st),&ts));
    check(!ts.pressed);

    /* 2. Multi-Touch 2-slot parallel Digitizer (Report ID=4, Logical Max 0xFFFF quirk) */
    static const uint8_t rdesc_mt[]={
        0x05,0x0D, 0x09,0x04, 0xA1,0x01,
        0x85,0x04,
        /* Slot 0 */
        0x09,0x22, 0xA1,0x02,
        0x09,0x42, 0x15,0x00, 0x25,0x01, 0x75,0x01, 0x95,0x01, 0x81,0x02,
        0x95,0x07, 0x81,0x03,
        0x09,0x51, 0x75,0x08, 0x95,0x01, 0x81,0x02, /* Contact ID */
        0x05,0x01, 0x09,0x30, 0x15,0x00, 0x26,0xFF,0xFF, 0x75,0x10, 0x95,0x01, 0x81,0x02,
        0x09,0x31, 0x81,0x02,
        0xC0,
        /* Slot 1 */
        0x05,0x0D, 0x09,0x22, 0xA1,0x02,
        0x09,0x42, 0x15,0x00, 0x25,0x01, 0x75,0x01, 0x95,0x01, 0x81,0x02,
        0x95,0x07, 0x81,0x03,
        0x09,0x51, 0x75,0x08, 0x95,0x01, 0x81,0x02,
        0x05,0x01, 0x09,0x30, 0x15,0x00, 0x26,0xFF,0xFF, 0x75,0x10, 0x95,0x01, 0x81,0x02,
        0x09,0x31, 0x81,0x02,
        0xC0,
        0x05,0x0D, 0x09,0x54, 0x75,0x08, 0x95,0x01, 0x81,0x02, /* Contact Count */
        0xC0
    };
    check(hid_touch_parse_report_desc(rdesc_mt,sizeof(rdesc_mt),&tl));
    check(tl.valid && tl.report_id==4 && tl.num_slots==2 && tl.slots[0].x_max==65535 && tl.slots[1].y_max==65535);
    /* Slot 0 released, Slot 1 active at (10000, 20000) -> picks Slot 1 */
    uint8_t pkt_mt[14]={0x04,  0x00,0x00, 0x00,0x00, 0x00,0x00,  0x01,0x01, 0x10,0x27, 0x20,0x4E,  0x01};
    check(hid_touch_decode_report(&tl,pkt_mt,sizeof(pkt_mt),&ts));
    check(ts.pressed && ts.x_norm==10000 && ts.y_norm==20000);
    pkt_mt[7]=0x00; /* Both slots up */
    check(hid_touch_decode_report(&tl,pkt_mt,sizeof(pkt_mt),&ts));
    check(!ts.pressed);

    /* 3. Relative Boot Mouse MUST be rejected by touch parser */
    static const uint8_t rdesc_rel_mouse[]={
        0x05,0x01, 0x09,0x02, 0xA1,0x01, 0x09,0x01, 0xA1,0x00,
        0x05,0x09, 0x19,0x01, 0x29,0x03, 0x15,0x00, 0x25,0x01, 0x95,0x03, 0x75,0x01, 0x81,0x02,
        0x95,0x01, 0x75,0x05, 0x81,0x01,
        0x05,0x01, 0x09,0x30, 0x09,0x31, 0x15,0x81, 0x25,0x7F, 0x75,0x08, 0x95,0x02, 0x81,0x06,
        0xC0, 0xC0
    };
    check(!hid_touch_parse_report_desc(rdesc_rel_mouse,sizeof(rdesc_rel_mouse),&tl));

    /* 4. USB Config Descriptor touch candidate parsing */
    uint8_t touch_cfg[]={9,2,34,0,1,1,0,0x80,50,
        9,4,0,0,1,3,0,0,0,
        9,0x21,0x11,1,0,1,0x22,0x90,0x01,
        7,5,0x82,3,64,0,4};
    usb_hid_touch_candidate_t cands[2];
    size_t cand_cnt=0;
    check(usb_parse_hid_touch_candidates(touch_cfg,sizeof(touch_cfg),false,&d,cands,2,&cand_cnt)==USB_OK);
    check(cand_cnt==1 && cands[0].dev.endpoint==2 && cands[0].dev.packet_size==64 && cands[0].report_desc_len==0x0190);

    /* 5. Touch queue quick-tap preservation, drag coalesce, and disconnect reset */
    touch_reset();
    uint16_t qx=0,qy=0; bool qp=false,qr=false;
    touch_post(1000,2000,true);
    touch_post(1500,2500,true); /* Coalesced with previous pressed */
    touch_post(1500,2500,false); /* Release preserved as separate edge */
    check(touch_poll(&qx,&qy,&qp,&qr) && qp && !qr && qx==1500 && qy==2500);
    check(!touch_poll(&qx,&qy,&qp,&qr) && !qp && !qr);
    touch_post(3000,4000,true);
    (void)touch_poll(&qx,&qy,&qp,&qr);
    check(qp && !qr);
    touch_reset(); /* Forces safe cancel-reset without click */
    (void)touch_poll(&qx,&qy,&qp,&qr);
    check(!qp && qr);
    usb_touch_reports=0;

    /* 6. Wireless 2.4GHz Combo Receiver HID Report Descriptor (Report ID 1=Media, ID 2=12-bit packed Mouse) */
    static const uint8_t rdesc_combo[]={
        /* Report ID 1: Consumer Control (Media keys) */
        0x05,0x0C, 0x09,0x01, 0xA1,0x01,
        0x85,0x01,
        0x19,0x00, 0x2A,0x3C,0x02, 0x15,0x00, 0x26,0x3C,0x02,
        0x95,0x01, 0x75,0x10, 0x81,0x00,
        0xC0,
        /* Report ID 2: Generic Desktop Mouse (5 buttons + 3pad + 12-bit X + 12-bit Y + 8-bit Wheel) */
        0x05,0x01, 0x09,0x02, 0xA1,0x01,
        0x85,0x02,
        0x09,0x01, 0xA1,0x00,
        0x05,0x09, 0x19,0x01, 0x29,0x05, 0x15,0x00, 0x25,0x01,
        0x95,0x05, 0x75,0x01, 0x81,0x02,
        0x95,0x01, 0x75,0x03, 0x81,0x01,
        0x05,0x01, 0x09,0x30, 0x09,0x31,
        0x16,0x01,0xF8, 0x26,0xFF,0x07, /* -2047..2047 */
        0x75,0x0C, 0x95,0x02, 0x81,0x06, /* 12-bit x 2, Data,Var,Rel */
        0x09,0x38, 0x15,0x81, 0x25,0x7F, 0x75,0x08, 0x95,0x01, 0x81,0x06,
        0xC0, 0xC0
    };
    check(!hid_touch_parse_report_desc(rdesc_combo,sizeof(rdesc_combo),&tl));
    hid_mouse_layout_t ml;
    check(hid_mouse_parse_report_desc(rdesc_combo,sizeof(rdesc_combo),&ml));
    check(ml.valid && ml.has_report_id && ml.report_id==2 && ml.x_size==12 && ml.y_size==12 && ml.btn_bits==5);
    hid_mouse_sample_t ms;
    uint8_t pkt_media[3]={0x01, 0xE9,0x00}; /* Volume Up on Report ID 1 -> ignored by mouse decoder */
    check(!hid_mouse_decode_report(&ml,pkt_media,sizeof(pkt_media),&ms));
    /* Report ID=2, LeftBtn=1, X=+300 (0x12C), Y=-150 (0xF6A):
     * packed 24 bits: low12=0x12C, high12=0xF6A -> bytes: 0x2C, 0xA1, 0xF6 */
    uint8_t pkt_combo[6]={0x02, 0x01, 0x2C, 0xA1, 0xF6, 0x00};
    check(hid_mouse_decode_report(&ml,pkt_combo,sizeof(pkt_combo),&ms));
    check((ms.buttons&1u)==1u && ms.dx==300 && ms.dy==-150);

    /* 7. Wired 16-bit Optical/Gaming Mouse (no Report ID) + fast-click edge preservation */
    static const uint8_t rdesc_16b[]={
        0x05,0x01, 0x09,0x02, 0xA1,0x01, 0x09,0x01, 0xA1,0x00,
        0x05,0x09, 0x19,0x01, 0x29,0x05, 0x15,0x00, 0x25,0x01, 0x95,0x05, 0x75,0x01, 0x81,0x02,
        0x95,0x01, 0x75,0x03, 0x81,0x01,
        0x05,0x01, 0x09,0x30, 0x09,0x31, 0x16,0x00,0x80, 0x26,0xFF,0x7F,
        0x75,0x10, 0x95,0x02, 0x81,0x06,
        0xC0, 0xC0
    };
    check(hid_mouse_parse_report_desc(rdesc_16b,sizeof(rdesc_16b),&ml));
    check(ml.valid && !ml.has_report_id && ml.x_size==16 && ml.y_size==16 && ml.min_report_bytes==5);
    uint8_t pkt_16b[5]={0x01, 0xEC,0xFF, 0x28,0x00}; /* LeftBtn=1, X=-20, Y=+40 */
    check(hid_mouse_decode_report(&ml,pkt_16b,sizeof(pkt_16b),&ms));
    check(ms.buttons==1 && ms.dx==-20 && ms.dy==40);

    mouse_reset();
    int32_t mdx=0,mdy=0; bool mleft=false;
    mouse_post(10,-5,1); /* Press */
    mouse_post(2,3,0);   /* Release within same LVGL tick! */
    check(mouse_poll(&mdx,&mdy,&mleft) && mleft && mdx==12 && mdy==-2);
    check(mouse_poll(&mdx,&mdy,&mleft) && !mleft && mdx==0 && mdy==0);
    usb_mouse_reports=0;

    usb_unit_pass=1;net_log("[TEST] USB parser/queue checks=%lu PASS\n",(unsigned long)usb_unit_checks);
}
