/* SPDX-License-Identifier: Apache-2.0 */
#ifndef GUI_H
#define GUI_H
#include <stdbool.h>
#include <stdint.h>
void gui_queue_init(void);
bool gui_post_char(char ch);
void gui_task(void *arg);
_Noreturn void gui_fault(void);
extern volatile uint32_t app_display_frames;
extern volatile uint32_t gui_ready;
extern volatile uint32_t gui_commands;
extern volatile uint32_t gui_dropped;
extern volatile uint32_t gui_value;
extern volatile uint32_t gui_button_events;
extern volatile uint32_t gui_heap_free;
extern volatile uint32_t gui_stack_free;
extern volatile uint32_t gui_owner_checks;
#endif
