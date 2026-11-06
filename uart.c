#include "platform.h"
#include "uart.h"

#define GPFSEL1    (GPIO_BASE + 0x04u)
#define GPFSEL3    (GPIO_BASE + 0x0cu)
#define GPPUD      (GPIO_BASE + 0x94u)
#define GPPUDCLK0  (GPIO_BASE + 0x98u)
#define UART_DR    (UART0_BASE + 0x00u)
#define UART_ECR   (UART0_BASE + 0x04u)
#define UART_FR    (UART0_BASE + 0x18u)
#define UART_IBRD  (UART0_BASE + 0x24u)
#define UART_FBRD  (UART0_BASE + 0x28u)
#define UART_LCRH  (UART0_BASE + 0x2cu)
#define UART_CR    (UART0_BASE + 0x30u)
#define UART_IMSC  (UART0_BASE + 0x38u)
#define UART_ICR   (UART0_BASE + 0x44u)
#define UART_DMACR (UART0_BASE + 0x48u)
#define FR_BUSY    (1u << 3)
#define FR_RXFE    (1u << 4)
#define FR_TXFF    (1u << 5)

void uart_init(void)
{
    uint32_t select;

    mmio_write(UART_CR, 0u);
    while ((mmio_read(UART_FR) & FR_BUSY) != 0u) {
        /* Wait for any firmware transmission to complete before changing baud. */
    }
    mmio_write(UART_LCRH, 0u); /* Disable/flush FIFOs. */
    mmio_write(UART_IMSC, 0u); /* Polling only. */
    mmio_write(UART_DMACR, 0u);
    mmio_write(UART_ICR, 0x7ffu);
    mmio_write(UART_ECR, 0u);

    /* Zero W may route UART0 to the Bluetooth module on GPIO32/33.
     * Disconnect that pair, especially RXD0, before selecting header pins.
     * No Linux device-tree overlay is needed or processed by this program.
     */
    select = mmio_read(GPFSEL3);
    select &= ~((7u << 6) | (7u << 9));
    mmio_write(GPFSEL3, select); /* GPIO32/33 -> input. */

    select = mmio_read(GPFSEL1);
    select &= ~((7u << 12) | (7u << 15));
    select |= (4u << 12) | (4u << 15); /* ALT0: TXD0 / RXD0. */
    mmio_write(GPFSEL1, select);

    /* BCM2835 pull sequence: 5 us exceeds the required 150 peripheral cycles. */
    mmio_write(GPPUD, 0u);
    delay_us(5u);
    mmio_write(GPPUDCLK0, (1u << 14) | (1u << 15));
    delay_us(5u);
    mmio_write(GPPUD, 0u);
    mmio_write(GPPUDCLK0, 0u);

    /* config.txt MUST set init_uart_clock=48000000.
     * 48,000,000 / (16 * 115,200) = 26.0416667
     * IBRD=26; FBRD=round(0.0416667*64)=3 -> about 115176.96 baud.
     * Write LCRH after the divisors to latch them.
     */
    mmio_write(UART_IBRD, 26u);
    mmio_write(UART_FBRD, 3u);
    mmio_write(UART_LCRH, (3u << 5) | (1u << 4)); /* 8N1, FIFO on. */
    mmio_write(UART_CR, (1u << 9) | (1u << 8) | 1u); /* RX, TX, UART. */
}

static void uart_write_raw(char ch)
{
    while ((mmio_read(UART_FR) & FR_TXFF) != 0u) {
    }
    mmio_write(UART_DR, (uint32_t)(unsigned char)ch);
}

void uart_putc(char ch)
{
    if (ch == '\n') {
        uart_write_raw('\r');
    }
    uart_write_raw(ch);
}

void uart_puts(const char *text)
{
    while (*text != '\0') {
        uart_putc(*text++);
    }
}

void uart_puthex32(uint32_t value)
{
    static const char digits[] = "0123456789ABCDEF";
    uart_puts("0x");
    for (unsigned int i = 0u; i < 8u; ++i) {
        uart_putc(digits[(value >> 28) & 0x0fu]);
        value <<= 4;
    }
}

bool uart_try_getc(char *ch)
{
    uint32_t data;
    if ((mmio_read(UART_FR) & FR_RXFE) != 0u) {
        return false;
    }
    data = mmio_read(UART_DR);
    if ((data & 0x0f00u) != 0u) {
        mmio_write(UART_ECR, 0u); /* Drop byte on break/parity/framing/overrun. */
        return false;
    }
    *ch = (char)(data & 0xffu);
    return true;
}

void uart_flush(void)
{
    while ((mmio_read(UART_FR) & FR_BUSY) != 0u) {
    }
}
