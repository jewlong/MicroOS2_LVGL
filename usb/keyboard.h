/* SPDX-License-Identifier: Apache-2.0 */
#ifndef USB_KEYBOARD_H
#define USB_KEYBOARD_H
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
typedef struct { uint32_t key; uint8_t usage,modifiers,pressed,reset; } key_event_t;
enum { KEY_ENTER=10,KEY_TAB=9,KEY_BACKTAB=11,KEY_ESC=27,KEY_BACKSPACE=8,
       KEY_UP=17,KEY_DOWN=18,KEY_RIGHT=19,KEY_LEFT=20,KEY_HOME=2,KEY_END=3,KEY_DELETE=127,KEY_F12=140 };
typedef struct { uint8_t keys[6],modifiers; uint32_t mapped[6]; bool caps,num; } hid_keyboard_t;
typedef void (*hid_emit_t)(const key_event_t *);
void hid_keyboard_init(hid_keyboard_t *state);
bool hid_keyboard_report(hid_keyboard_t *state,const uint8_t *report,size_t length,hid_emit_t emit);
void hid_keyboard_release(hid_keyboard_t *state,hid_emit_t emit);
uint8_t hid_keyboard_leds(const hid_keyboard_t *state);
void keyboard_post(const key_event_t *event);
bool keyboard_pop(key_event_t *event);
void keyboard_all_up(void);
void usb_power_init(void); /* pre-OS, serializes mailbox by boot ordering */
void usb_keyboard_task(void *arg);
extern volatile uint32_t usb_state,usb_error,usb_reports,usb_key_presses,usb_queue_drops;
extern volatile uint32_t usb_connects,usb_disconnects,usb_rollovers,usb_modifiers;
extern volatile uint32_t usb_step,usb_vid_pid;
extern volatile uint32_t usb_init_err,usb_init_val;
uint32_t usb_diag_hprt(void);
void usb_diag_line(char *out, size_t max_out);
#endif
