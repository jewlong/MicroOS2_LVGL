/* SPDX-License-Identifier: Apache-2.0
 * New ARM1176 / GNU port. Tasks run in privileged ARM System mode.
 * Only IRQ scheduling is supported: FIQ stays masked, no nested interrupts.
 */
#ifndef OS_CPU_H
#define OS_CPU_H
#include <stdint.h>

typedef uint8_t  BOOLEAN;
typedef uint8_t  INT8U;
typedef int8_t   INT8S;
typedef uint16_t INT16U;
typedef int16_t  INT16S;
typedef uint32_t INT32U;
typedef int32_t  INT32S;
typedef float   FP32;
typedef double  FP64;
typedef uint32_t OS_STK;
typedef uint32_t OS_CPU_SR;

#define OS_CRITICAL_METHOD 3u
#define OS_STK_GROWTH      1u
#define OS_TASK_SW()       OSCtxSw()
#define OS_ENTER_CRITICAL() do { cpu_sr = OS_CPU_SR_Save(); } while (0)
#define OS_EXIT_CRITICAL()  do { OS_CPU_SR_Restore(cpu_sr); } while (0)

/* Compiler barriers are in the caller as well as the out-of-line assembly.
 * Restore only the CPSR control field; never change the caller's mode.
 */
OS_CPU_SR OS_CPU_SR_Save(void);
void OS_CPU_SR_Restore(OS_CPU_SR saved);
void OSCtxSw(void);
void OSIntCtxSw(void);
void OSStartHighRdy(void);

#endif
