/*
    RExOS - embedded RTOS
    Copyright (c) 2011-2014, Alexey Kramarenko
    All rights reserved.
*/

#ifndef RCC_H
#define RCC_H

/*
    - reset
    - clock
 */

#include "types.h"

typedef enum {
    RESET_REASON_UNKNOWN    = 0,
    RESET_REASON_STANBY,
    RESET_REASON_WATCHDOG,
    RESET_REASON_SOFTWARE,
    RESET_REASON_POWERON,
    RESET_REASON_PIN_RST
} RESET_REASON;

extern unsigned long set_core_freq(unsigned long desired_freq);
extern RESET_REASON get_reset_reason();

#endif // RCC_H
