/*
    RExOS - embedded RTOS
    Copyright (c) 2011-2014, Alexey Kramarenko
    All rights reserved.
*/

#include "kstream.h"
#include "kernel.h"
#include "kmalloc.h"
#include "kipc.h"
#include "../userspace/error.h"
#include <string.h>

void kstream_lock_release(STREAM_HANDLE* handle, PROCESS* process)
{
    CHECK_MAGIC(handle, MAGIC_STREAM_HANDLE);
    switch (handle->mode)
    {
    case STREAM_MODE_READ:
        dlist_remove((DLIST**)&(handle->stream->read_waiters), (DLIST*)handle);
        break;
    case STREAM_MODE_WRITE:
        dlist_remove((DLIST**)&(handle->stream->write_waiters), (DLIST*)handle);
        break;
    default:
        break;
    }
    handle->mode = STREAM_MODE_IDLE;
}

void kstream_create(STREAM** stream, int size)
{
    *stream = kmalloc(sizeof(STREAM));
    if (*stream != NULL)
    {
        memset(*stream, 0, sizeof(STREAM));
        DO_MAGIC((*stream), MAGIC_STREAM);
        rb_init(&(*stream)->rb, size);
        //allocate stream data
        if (((*stream)->data = paged_alloc(size)) == NULL)
        {
            kfree(*stream);
            error(ERROR_OUT_OF_PAGED_MEMORY);
        }
    }
    else
        error(ERROR_OUT_OF_SYSTEM_MEMORY);
}

void kstream_open(STREAM* stream, STREAM_HANDLE** handle)
{
    CHECK_MAGIC(stream, MAGIC_STREAM);
    *handle = kmalloc(sizeof(STREAM_HANDLE));
    if (*handle != NULL)
    {
        DO_MAGIC((*handle), MAGIC_STREAM_HANDLE);
        (*handle)->process = kprocess_get_current();
        (*handle)->stream = stream;
        (*handle)->mode = STREAM_MODE_IDLE;
    }
    else
        error(ERROR_OUT_OF_SYSTEM_MEMORY);
}

void kstream_close(STREAM_HANDLE* handle)
{
    CHECK_MAGIC(handle, MAGIC_STREAM_HANDLE);
    switch (handle->mode)
    {
    case STREAM_MODE_READ:
        dlist_remove((DLIST**)&(handle->stream->read_waiters), (DLIST*)handle);
        kprocess_wakeup(handle->process);
        kprocess_error(handle->process, ERROR_SYNC_OBJECT_DESTROYED);
        break;
    case STREAM_MODE_WRITE:
        dlist_remove((DLIST**)&(handle->stream->write_waiters), (DLIST*)handle);
        kprocess_wakeup(handle->process);
        kprocess_error(handle->process, ERROR_SYNC_OBJECT_DESTROYED);
        break;
    default:
        break;
    }
    kfree(handle);
}

void kstream_get_size(STREAM* stream, int* size)
{
    CHECK_MAGIC(stream, MAGIC_STREAM);
    *size = rb_size(&stream->rb);
}

void kstream_get_free(STREAM *stream, int* size)
{
    CHECK_MAGIC(stream, MAGIC_STREAM);
    *size = rb_free(&stream->rb);
}

void kstream_start_listen(STREAM* stream)
{
    CHECK_MAGIC(stream, MAGIC_STREAM);
    if (stream->listener == NULL)
        stream->listener = kprocess_get_current();
    else
        error(ERROR_ACCESS_DENIED);
}

void kstream_stop_listen(STREAM* stream)
{
    PROCESS* process = kprocess_get_current();
    CHECK_MAGIC(stream, MAGIC_STREAM);
    if (stream->listener == process)
        stream->listener = process;
    else
        error(ERROR_ACCESS_DENIED);
}

void kstream_write(STREAM_HANDLE *handle, char* buf, int size)
{
    ASSERT(handle->mode == STREAM_MODE_IDLE);
    TIME time;
    IPC ipc;
    register STREAM_HANDLE* reader;
    CHECK_MAGIC(handle, MAGIC_STREAM_HANDLE);
    handle->size = size;
    //write directly to output
    while (handle->size > 0 && (reader = handle->stream->read_waiters) != NULL)
    {
        //all can go directly
        if (handle->size >= reader->size)
        {
            memcpy(reader->buf, buf, reader->size);
            handle->size -= reader->size;
            buf += reader->size;
            dlist_remove_head((DLIST**)&handle->stream->read_waiters);
            reader->mode = STREAM_MODE_IDLE;
            //wakeup reader
            kprocess_wakeup(reader->process);
        }
        else
        //part can go directly
        {
            memcpy(reader->buf, buf, handle->size);
            reader->buf += handle->size;
            reader->size -= handle->size;
            buf += handle->size;
            handle->size = 0;
        }
    }
    //write rest to stream
    for(; handle->size > 0 && !rb_is_full(&handle->stream->rb); --handle->size)
        handle->stream->data[rb_put(&handle->stream->rb)] = *buf++;
    //still need more? Wait.
    if (handle->size > 0)
    {
        handle->full_size = size;
        handle->buf = buf;
        handle->mode = STREAM_MODE_WRITE;
        time.sec = time.usec = 0;
        kprocess_sleep(&time, PROCESS_SYNC_STREAM, handle);
    }
    else if (handle->stream->listener != NULL)
    {
        ipc.process = (HANDLE)handle->stream->listener;
        ipc.cmd = IPC_STREAM_WRITE;
        ipc.param1 = size;
        ipc.param2 = 0;
        ipc_post(&ipc);
    }
}

//push data to stream internally after read
void kstream_push(STREAM* stream)
{
    STREAM_HANDLE* handle;
    IPC ipc;
    while ((handle = stream->write_waiters) != NULL && !rb_is_full(&stream->rb))
    {
        for(; !rb_is_full(&stream->rb) && handle->size > 0; --handle->size)
            stream->data[rb_put(&stream->rb)] = *handle->buf++;
        //writed all from waiter? Wake him up.
        if (handle->size <= 0)
        {
            handle->mode = STREAM_MODE_IDLE;
            dlist_remove_head((DLIST**)&stream->write_waiters);
            kprocess_wakeup(handle->process);
            if (stream->listener != NULL)
            {
                ipc.process = (HANDLE)stream->listener;
                ipc.cmd = IPC_STREAM_WRITE;
                ipc.param1 = handle->full_size;
                ipc.param2 = 0;
                ipc_post(&ipc);
            }
        }
    }
}

void kstream_read(STREAM_HANDLE* handle, char* buf, int size)
{
    ASSERT(handle->mode == STREAM_MODE_IDLE);
    register STREAM_HANDLE* writer;
    IPC ipc;
    TIME time;
    CHECK_MAGIC(handle, MAGIC_STREAM_HANDLE);
    handle->size = size;
    //read from stream
    for(; handle->size > 0 && !rb_is_empty(&handle->stream->rb); --handle->size)
        *buf++ = handle->stream->data[rb_get(&handle->stream->rb)];
    //read directly from input
    while (handle->size > 0 && (writer = handle->stream->write_waiters) != NULL)
    {
        //all can go directly
        if (handle->size >= writer->size)
        {
            memcpy(buf, writer->buf, writer->size);
            handle->size -= writer->size;
            buf += writer->size;
            //wakeup writer
            writer->mode = STREAM_MODE_IDLE;
            dlist_remove_head((DLIST**)&handle->stream->write_waiters);
            kprocess_wakeup(writer->process);
            if (handle->stream->listener != NULL)
            {
                ipc.process = (HANDLE)handle->stream->listener;
                ipc.cmd = IPC_STREAM_WRITE;
                ipc.param1 = handle->full_size;
                ipc.param2 = 0;
                ipc_post(&ipc);
            }
        }
        else
        //part can go directly
        {
            memcpy(buf, writer->buf, handle->size);
            writer->size -= handle->size;
            writer->buf += handle->size;
            buf += handle->size;
            handle->size = 0;
        }
    }
    //still need more? Wait.
    if (handle->size > 0)
    {
        handle->buf = buf;
        handle->mode = STREAM_MODE_READ;
        time.sec = time.usec = 0;
        kprocess_sleep(&time, PROCESS_SYNC_STREAM, handle);
    }
    kstream_push(handle->stream);
}

void kstream_flush(STREAM* stream)
{
    STREAM_HANDLE* handle;
    CHECK_MAGIC(stream, MAGIC_STREAM);
    //flush stream
    rb_clear(&stream->rb);
    //flush waiters
    while ((handle = stream->write_waiters) != NULL)
    {
        handle->mode = STREAM_MODE_IDLE;
        dlist_remove_head((DLIST**)&stream->write_waiters);
        kprocess_wakeup(handle->process);
    }
}

void kstream_destroy(STREAM* stream)
{
    STREAM_HANDLE* handle;
    CHECK_MAGIC(stream, MAGIC_STREAM);
    while (stream->write_waiters)
    {
        handle = stream->write_waiters;
        CHECK_MAGIC(handle, MAGIC_STREAM_HANDLE);
        dlist_remove_head((DLIST**)&stream->write_waiters);
        kprocess_wakeup(handle->process);
        kprocess_error(handle->process, ERROR_SYNC_OBJECT_DESTROYED);
        kfree(handle);
    }
    while (stream->read_waiters)
    {
        handle = stream->read_waiters;
        CHECK_MAGIC(handle, MAGIC_STREAM_HANDLE);
        dlist_remove_head((DLIST**)&stream->read_waiters);
        kprocess_wakeup(handle->process);
        kprocess_error(handle->process, ERROR_SYNC_OBJECT_DESTROYED);
        kfree(handle);
    }
    paged_free(stream->data);
    kfree(stream);
}
