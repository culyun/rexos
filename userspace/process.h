/*
    RExOS - embedded RTOS
    Copyright (c) 2011-2014, Alexey Kramarenko
    All rights reserved.
*/

#ifndef PROCESS_H
#define PROCESS_H

/** \addtogroup process process
    process is the main object in kernel.

    process can be in one of the following states:
    - active
    - waiting (for sync object)
    - frozen
    - waiting frozen

    After creation process is in the frozen state

    Process is also specified by:
    - it's name
    - priority
    - stack size
    \{
 */

#include "kernel_config.h"
#include "sys.h"
#include "lib/time.h"


#define PROCESS_FLAGS_ACTIVE                                     (1 << 0)
#define PROCESS_FLAGS_WAITING                                    (1 << 1)
#define PROCESS_FLAGS_TIMER_ACTIVE                               (1 << 2)

#define PROCESS_MODE_MASK                                        0x3
#define PROCESS_MODE_FROZEN                                      (0)
#define PROCESS_MODE_ACTIVE                                      (PROCESS_FLAGS_ACTIVE)
#define PROCESS_MODE_WAITING_FROZEN                              (PROCESS_FLAGS_WAITING)
#define PROCESS_MODE_WAITING                                     (PROCESS_FLAGS_WAITING | PROCESS_FLAGS_ACTIVE)

#define PROCESS_SYNC_MASK                                        (0xf << 4)

typedef enum {
    PROCESS_SYNC_TIMER_ONLY =    (0x0 << 4),
    PROCESS_SYNC_MUTEX =         (0x1 << 4),
    PROCESS_SYNC_EVENT =         (0x2 << 4),
    PROCESS_SYNC_SEM =           (0x3 << 4),
    PROCESS_SYNC_IPC =           (0x4 << 4),
    PROCESS_SYNC_STREAM =        (0x5 << 4)
}PROCESS_SYNC_TYPE;

typedef struct {
    //header size including name
    int struct_size;
    POOL pool;
    int error;
    //self handle
    HANDLE handle;
    STDOUT stdout;
    void* stdout_param;
    STDIN stdin;
    void* stdin_param;
    //name is following
} HEAP;

#define PROCESS_NAME(heap)                              ((char*)((unsigned int)heap + sizeof(HEAP)))

#define __PROCESS_NAME                                  (PROCESS_NAME(__HEAP))

typedef struct {
    const char* name;
    unsigned int size;
    unsigned int priority;
    unsigned int flags;
    unsigned int ipc_size;
    void (*fn) (void);
}REX;

/**
    \brief creates process object. By default, process is frozen after creation
    \param rex: pointer to process header
    \retval process HANDLE on success, or INVALID_HANDLE on failure
*/
__STATIC_INLINE HANDLE process_create(const REX* rex)
{
    HANDLE handle;
    sys_call(SVC_PROCESS_CREATE, (unsigned int)rex, (unsigned int)&handle, 0);
    return handle;
}

/**
    \brief get process flags
    \param process: handle of created process
    \retval process flags
*/
__STATIC_INLINE unsigned int process_get_flags(HANDLE process)
{
    unsigned int flags;
    sys_call(SVC_PROCESS_GET_FLAGS, (unsigned int)process, (unsigned int)&flags, 0);
    return flags;
}

/**
    \brief set process flags
    \param process: handle of created process
    \param flags: set of flags. Only PROCESS_FLAGS_ACTIVE is supported
    \retval process flags
*/
__STATIC_INLINE void process_set_flags(HANDLE process, unsigned int flags)
{
    sys_call(SVC_PROCESS_SET_FLAGS, (unsigned int)process, flags, 0);
}

/**
    \brief unfreeze process
    \param process: handle of created process
    \retval none
*/
__STATIC_INLINE void process_unfreeze(HANDLE process)
{
    process_set_flags(process, PROCESS_FLAGS_ACTIVE);
}

/**
    \brief freeze process
    \param process: handle of created process
    \retval none
*/
__STATIC_INLINE void process_freeze(HANDLE process)
{
    process_set_flags(process, 0);
}

/**
    \brief return handle to currently running process
    \retval handle to current process
*/
__STATIC_INLINE HANDLE process_get_current()
{
    return __HEAP->handle;
}

/**
    \brief get priority
    \param process: handle of created process
    \retval process base priority
*/
__STATIC_INLINE unsigned int process_get_priority(HANDLE process)
{
    unsigned int priority;
    sys_call(SVC_PROCESS_GET_PRIORITY, (unsigned int)process, (unsigned int)&priority, 0);
    return priority;
}

/**
    \brief get current process priority
    \param process: handle of created process
    \retval process base priority
*/
__STATIC_INLINE unsigned int process_get_current_priority()
{
    return process_get_priority(__HEAP->handle);
}

/**
    \brief set priority
    \param process: handle of created process
    \param priority: new priority. 0 - highest, init -1 - lowest. Cannot be called for init process.
    \retval none
*/
__STATIC_INLINE void process_set_priority(HANDLE process, unsigned int priority)
{
    sys_call(SVC_PROCESS_SET_PRIORITY, (unsigned int)process, priority, 0);
}

/**
    \brief set currently running process priority
    \param priority: new priority. 0 - highest, init -1 - lowest. Cannot be called for init process.
    \retval none
*/
__STATIC_INLINE void process_set_current_priority(unsigned int priority)
{
    process_set_priority(__HEAP->handle, priority);
}

/**
    \brief destroys process
    \param process: previously created process
    \retval none
*/
__STATIC_INLINE void process_destroy(HANDLE process)
{
    sys_call(SVC_PROCESS_DESTROY, (unsigned int)process, 0, 0);
}

/**
    \brief destroys current process
    \details this function should be called instead of leaving process function
    \retval none
*/
__STATIC_INLINE void process_exit()
{
    process_destroy(__HEAP->handle);
}

/**
    \brief put current process in waiting state
    \param time: pointer to TIME structure
    \retval none
*/
__STATIC_INLINE void sleep(TIME* time)
{
    sys_call(SVC_PROCESS_SLEEP, (unsigned int)time, 0, 0);
}

/**
    \brief put current process in waiting state
    \param ms: time to sleep in milliseconds
    \retval none
*/
__STATIC_INLINE void sleep_ms(unsigned int ms)
{
    TIME time;
    __GLOBAL->lib->ms_to_time(ms, &time);
    sleep(&time);
}

/**
    \brief put current process in waiting state
    \param us: time to sleep in microseconds
    \retval none
*/
__STATIC_INLINE void sleep_us(unsigned int us)
{
    TIME time;
    __GLOBAL->lib->us_to_time(us, &time);
    sleep(&time);
}

/**
    \brief process switch test. Only for kernel debug/perfomance testing reasons
    \param us: time to sleep in microseconds
    \retval none
*/
__STATIC_INLINE void process_switch_test()
{
    sys_call(SVC_PROCESS_SWITCH_TEST, 0, 0, 0);
}

/**
    \brief process info for all processes. Only for kernel debug/perfomance testing reasons
    \retval none
*/
__STATIC_INLINE void process_info()
{
    sys_call(SVC_PROCESS_INFO, 0, 0, 0);
}

/** \} */ // end of process group

#endif // PROCESS_H
