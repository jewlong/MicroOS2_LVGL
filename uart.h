#ifndef UART_H
#define UART_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

void uart_init(void);
void uart_putc(char ch);
void uart_puts(const char *text);
void uart_puthex32(uint32_t value);
bool uart_try_getc(char *ch);
void uart_flush(void);

/* Modbus RTU raw mode on PL011 UART0 (pauses debug console output while active).
 * parity: 0=none, 1=even, 2=odd; stop_bits: 1 or 2. */
void uart_configure_rtu(uint32_t baud_rate, uint8_t parity, uint8_t stop_bits);
bool uart_rtu_active(void);
void uart_tx_bytes(const uint8_t *buf, size_t len);
void uart_rx_drain(void);
bool uart_rx_byte_raw(uint8_t *out);

#endif
