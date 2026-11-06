#ifndef UART_H
#define UART_H

#include <stdbool.h>
#include <stdint.h>

void uart_init(void);
void uart_putc(char ch);
void uart_puts(const char *text);
void uart_puthex32(uint32_t value);
bool uart_try_getc(char *ch);
void uart_flush(void);

#endif
