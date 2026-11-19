/* SPDX-License-Identifier: Apache-2.0 */
#ifndef USB_HOST_H
#define USB_HOST_H
#include "keyboard.h"
typedef enum { USB_OK,USB_NAK,USB_STALL,USB_REMOVED,USB_TIMEOUT,USB_IO,USB_BAD_DESCRIPTOR,USB_UNSUPPORTED } usb_result_t;
typedef struct { uint8_t address,ep0,endpoint,interface_number,configuration,interval;uint16_t packet_size;bool low_speed; } usb_keyboard_device_t;
bool usb_host_init(void);
bool usb_host_connected(void);
bool usb_host_operational(void);
usb_result_t usb_host_reset_port(bool *low_speed);
usb_result_t usb_control(usb_keyboard_device_t *dev,uint8_t type,uint8_t request,uint16_t value,uint16_t index,void *data,uint16_t length,uint16_t *actual);
usb_result_t usb_interrupt_in(usb_keyboard_device_t *dev,uint8_t pid,void *data,uint16_t *actual);
usb_result_t usb_parse_config(const uint8_t *data,size_t length,bool low_speed,usb_keyboard_device_t *out);
usb_result_t usb_parse_hid_config(const uint8_t *data,size_t length,bool low_speed,
                                  usb_keyboard_device_t *kbd_out,bool *has_kbd,
                                  usb_keyboard_device_t *mouse_out,bool *has_mouse);
#endif
