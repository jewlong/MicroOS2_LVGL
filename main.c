/* SPDX-License-Identifier: Apache-2.0 */
#include <ucos_ii.h>
#include "platform.h"
#include "uart.h"
#include "bsp.h"
#include "port.h"
#include "framebuffer.h"
#include "gui.h"
#include "net_platform.h"
_Alignas(8) static OS_STK network_stack[8192];

_Alignas(8) static OS_STK high_stack[APP_STACK_WORDS];
_Alignas(8) static OS_STK uart_stack[APP_STACK_WORDS];
_Alignas(8) static OS_STK low_stack[APP_STACK_WORDS];
_Alignas(8) static OS_STK gui_stack[APP_GUI_STACK_WORDS];
static volatile uint32_t data_cookie = 0x1234abcdu;
static volatile uint32_t bss_cookie;

volatile uint32_t app_low_busy;
volatile uint32_t app_low_progress;
volatile uint32_t app_low_completed;
volatile uint32_t app_preempt_pass;
volatile uint32_t app_high_runs;
volatile uint32_t app_uart_commands;

static void report_status(void)
{
    OS_CPU_SR cpu_sr;
    uint32_t tick, irq, svc, preempt, work, late;
    OS_ENTER_CRITICAL();
    tick = OSTime;
    irq = bsp_irq_count;
    svc = port_svc_switches;
    preempt = port_irq_switches;
    work = app_low_progress;
    late = bsp_late_ticks;
    OS_EXIT_CRITICAL();

    bsp_log_lock();
    uart_puts("[status] tick="); uart_puthex32(tick);
    uart_puts(" irq="); uart_puthex32(irq);
    uart_puts(" svc_sw="); uart_puthex32(svc);
    uart_puts(" irq_sw="); uart_puthex32(preempt);
    uart_puts(" work="); uart_puthex32(work);
    uart_puts(" late="); uart_puthex32(late);
    uart_puts(" gui_value="); uart_puthex32(gui_value);
    uart_puts(" gui_events="); uart_puthex32(gui_commands);
    uart_puts(" gui_drops="); uart_puthex32(gui_dropped);
    uart_puts(" net="); uart_puthex32(net_stage);
    uart_puts(" err="); uart_puthex32(net_error);
    uart_puts(" rx="); uart_puthex32(net_rx_frames);
    uart_puts(" tx="); uart_puthex32(net_tx_frames);
    uart_puts(" retry="); uart_puthex32(net_reconnects);
    uart_puts(" sdio_err="); uart_puthex32(net_sdio_errors);
    uart_puts(" display="); uart_puthex32(app_display_frames);
    uart_puts("\n");
    bsp_log_unlock();
}

static void high_task(void *arg)
{
    uint32_t cpsr;
    const INT32U before = OSTimeGet();
    (void)arg;
    __asm__ volatile ("mrs %0, cpsr" : "=r" (cpsr));
    if ((cpsr & 0xffu) != 0x5fu) { bsp_panic("task mode/IRQ mask"); }

    /* LOW spends 150ms in a loop without any OS call. Wake after 50ms.
     * If a cooperative-only port was used, LOW would finish before this runs.
     */
    OSTimeDly(5u);
    if ((app_low_busy == 0u) || (app_low_progress == 0u) ||
        (app_low_completed != 0u) || (port_irq_switches == 0u) ||
        ((INT32U)(OSTimeGet() - before) < 5u)) {
        bsp_panic("preemption self-test");
    }
    app_preempt_pass = 1u;
    bsp_log_lock();
    uart_puts("[PASS] HIGH woke by IRQ while LOW was busy (no yield).\n");
    bsp_log_unlock();

    for (;;) {
        ++app_high_runs;
        report_status();
        OSTimeDly(OS_TICKS_PER_SEC);
    }
}

static void uart_task(void *arg)
{
    (void)arg;
    for (;;) {
        char ch;
        if (uart_try_getc(&ch)) {
            ++app_uart_commands;
            if ((ch == 's') || (ch == 'S')) {
                report_status();
            } else if (gui_post_char(ch)) {
                /* Enqueued for the sole GUI owner. */
            } else {
                bsp_log_lock();
                if (ch == '\r') { uart_putc('\n'); }
                else if (ch != '\n') { uart_putc(ch); }
                bsp_log_unlock();
            }
        }
        OSTimeDly(1u); /* 10ms polling; does not monopolize the CPU. */
    }
}

static void low_task(void *arg)
{
    (void)arg;
    for (;;) {
        const uint32_t start = timer_us();
        app_low_busy = 1u;
        while ((uint32_t)(timer_us() - start) < 150000u) {
            ++app_low_progress;
            /* Deliberately no OSTimeDly, semaphore or scheduler call here. */
        }
        app_low_busy = 0u;
        ++app_low_completed;
        OSTimeDly(25u);
    }
}

static void create_task(void (*entry)(void *), OS_STK *stack, INT32U words, INT8U prio)
{
    const INT8U err = OSTaskCreateExt(entry, (void *)0,
        &stack[words - 1u], prio, prio, &stack[0], words,
        (void *)0, OS_TASK_OPT_STK_CHK | OS_TASK_OPT_STK_CLR);
    if (err != OS_ERR_NONE) { bsp_panic("OSTaskCreateExt"); }
}

int main(void)
{
    uart_init();
    bsp_interrupts_init();
    if ((data_cookie != 0x1234abcdu) || (bss_cookie != 0u)) {
        bsp_panic("data/BSS self-check");
    }
    uart_puts("\nRaspberry Pi Zero W - uC/OS-II + HDMI + LVGL + Wi-Fi\n");
    uart_puts("ARM1176 / SYS tasks / 100 Hz IRQ tick / soft-float\n");
    uart_puts("Core header V2.93.01; OSVersion()=");
    uart_puthex32(OSVersion()); uart_puts("\n");
    uart_puts("Tasks: HIGH=5, WHD=8, UART=10, TCPIP=11, NET=12, GUI=15, LOW=20\n");
    uart_puts("First test: HIGH must interrupt LOW's 150ms busy loop.\n");
    uart_puts("Type s for status; + / - / r control LVGL through a queue.\n");
    uart_flush();

    const fb_result_t video = framebuffer_init();
    if (video == FB_OK) {
        const framebuffer_t *info = framebuffer_info();
        uart_puts("[HDMI] framebuffer ready: 640x480x32 pitch=");
        uart_puthex32(info->pitch);
        uart_puts(" ARM address="); uart_puthex32((uint32_t)(uintptr_t)info->pixels);
        uart_puts(" order="); uart_puthex32(info->pixel_order);
        uart_puts("\n");

    } else {
        uart_puts("[HDMI] unavailable: "); uart_puts(framebuffer_error(video));
        uart_puts("; UART/RTOS continue.\n");
    }
    uart_flush();

    OSInit();
    bsp_log_init();
    gui_queue_init();
    net_platform_init();
    create_task(network_task, network_stack, 8192, 12);
    create_task(high_task, high_stack, APP_STACK_WORDS, APP_HIGH_PRIO);
    create_task(uart_task, uart_stack, APP_STACK_WORDS, APP_UART_PRIO);
    create_task(low_task, low_stack, APP_STACK_WORDS, APP_LOW_PRIO);
    create_task(gui_task, gui_stack, APP_GUI_STACK_WORDS, APP_GUI_PRIO);
    bsp_tick_start();
    OSStart();
    bsp_panic("OSStart returned");
}
