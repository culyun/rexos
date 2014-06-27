/*
    RExOS - embedded RTOS
    Copyright (c) 2011-2014, Alexey Kramarenko
    All rights reserved.
*/

#include "sys_call.h"
#include "../userspace/core/core.h"
#include "../userspace/core/sys_calls.h"
#include "../userspace/error.h"

#include "dbg.h"
#include "thread_kernel.h"
#include "mutex_kernel.h"
#include "event_kernel.h"
#include "sem_kernel.h"
#include "mem_kernel.h"

void sys_handler_direct(unsigned int num, unsigned int param1, unsigned int param2, unsigned int param3)
{
    clear_error();
    switch (num & 0x0000ff00)
    {
    case SYS_CALL_THREAD:
        svc_thread_handler(num, param1, param2);
        break;
    case SYS_CALL_MUTEX:
        svc_mutex_handler(num, param1, param2);
        break;
    case SYS_CALL_EVENT:
        svc_event_handler(num, param1, param2);
        break;
    case SYS_CALL_SEM:
        svc_sem_handler(num, param1, param2);
        break;
    case SYS_CALL_MEM:
        svc_mem_handler(num);
        break;
    case (SYS_CALL_DBG):
        svc_dbg_handler(num, param1, param2);
        break;
    default:
        error(ERROR_INVALID_SYS_CALL);
    }
}

void sys_handler(unsigned int num, unsigned int param1, unsigned int param2, unsigned int param3)
{
    ///TODO: CONTEXT_PUSH(SVC); really need this shit?
    sys_handler_direct(num, param1, param2, param3);
    ///TODO: CONTEXT_POP();
}
