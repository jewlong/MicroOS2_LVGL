/* SPDX-License-Identifier: Apache-2.0 */
#ifndef MAILBOX_H
#define MAILBOX_H
#include <stdint.h>
#include <stddef.h>
typedef enum {
    MAILBOX_OK = 0,
    MAILBOX_BAD_ARGUMENT,
    MAILBOX_WRITE_TIMEOUT,
    MAILBOX_REPLY_TIMEOUT
} mailbox_result_t;
/* Boot-time only, sole mailbox owner; MMU and D-cache must remain off.
 * The caller must keep the buffer alive even after a response timeout.
 */
mailbox_result_t mailbox_property_call(volatile uint32_t *message,
                                      size_t bytes, uint32_t timeout_us);
#endif
