/*
    RExOS - embedded RTOS
    Copyright (c) 2011-2014, Alexey Kramarenko
    All rights reserved.
*/

#include "mt.h"
#include "stm32/stm32_bitbang.h"
#include "../gpio.h"
#if (SYS_INFO)
#include "../../userspace/stdio.h"
#include "../../userspace/timer.h"
#endif
#if (MT_DRIVER)
#include "../../userspace/block.h"
#include "../../userspace/direct.h"
#endif

#if (MT_DRIVER)
void mt();

const REX __MT = {
    //name
    "MT LCD driver",
    //size
    MT_STACK_SIZE,
    //priority - driver priority
    90,
    //flags
    PROCESS_FLAGS_ACTIVE | REX_HEAP_FLAGS(HEAP_PERSISTENT_NAME),
    //ipc size
    MT_IPC_COUNT,
    //function
    mt
};
#endif

#define ADDSET_MASK                     (MT_CS1 | MT_CS2 | MT_RW | MT_A)

#define MT_PAGES_COUNT                  8
#define MT_SIZE_X                       64
#define MT_SIZE_Y                       128

#define MT_STATUS_RESET                 (1 << 4)
#define MT_STATUS_OFF                   (1 << 5)
#define MT_STATUS_BUSY                  (1 << 7)

#define MT_CMD_DISPLAY_ON               0x3f
#define MT_CMD_DISPLAY_OFF              0x3e
#define MT_CMD_START_LINE               0xc0
#define MT_CMD_DISPLAY_OFF              0x3e
#define MT_CMD_SET_PAGE                 0xb8
#define MT_CMD_SET_ADDRESS              0x40

#if (X_MIRROR)

#define X_TRANSFORM(x, size)            (MT_SIZE_X - (x) - (size))
#define PAGE_TRANSFORM(page)            (MT_PAGES_COUNT - (page) - 1)
#define BIT_OUT_TRANSFORM(byte)         (byte)

#else

#define X_TRANSFORM(x, size)            (x)
#define PAGE_TRANSFORM(page)            (page)
#define BIT_OUT_TRANSFORM(byte)         (bitswap(byte))

#endif

uint8_t bitswap(uint8_t x)
{
    x = ((x & 0x55) << 1) | ((x & 0xAA) >> 1);
    x = ((x & 0x33) << 2) | ((x & 0xCC) >> 2);
    x = ((x & 0x0F) << 4) | ((x & 0xF0) >> 4);
    return x;
}

__STATIC_INLINE void delay_clks(unsigned int clks)
{
    int i;
    for (i = 0; i < clks; ++i)
        __NOP();
}

__STATIC_INLINE uint8_t mt_read(unsigned int mask)
{
    delay_clks(TW);
    stm32_bitbang_set_data_in(DATA_PORT);
    stm32_bitbang_reset_mask(DATA_PORT, DATA_MASK);
    stm32_bitbang_reset_mask(ADDSET_PORT, ADDSET_MASK);
    stm32_bitbang_reset_pin(MT_STROBE);
    //Tah is only 20ns
    stm32_bitbang_set_mask(ADDSET_PORT, mask | MT_RW);
    delay_clks(TAS);
    stm32_bitbang_set_pin(MT_STROBE);
    delay_clks(TDDR);
    return stm32_bitbang_get_mask(DATA_PORT, DATA_MASK) & 0xff;
}

__STATIC_INLINE uint8_t mt_status(unsigned int cs)
{
    return mt_read(cs);
}

__STATIC_INLINE void mt_write(unsigned int mask, uint8_t data)
{
    delay_clks(TW);
    stm32_bitbang_set_data_out(DATA_PORT);
    stm32_bitbang_reset_mask(DATA_PORT, DATA_MASK);
    stm32_bitbang_reset_mask(ADDSET_PORT, ADDSET_MASK);
    stm32_bitbang_reset_pin(MT_STROBE);
    //Tah is only 20ns
    stm32_bitbang_set_mask(ADDSET_PORT, mask);
    delay_clks(TAS);
    stm32_bitbang_set_pin(MT_STROBE);
    stm32_bitbang_set_mask(DATA_PORT, data);
}

__STATIC_INLINE void mt_cmd(unsigned int cs, uint8_t cmd)
{
    mt_write(cs, cmd);
    while(mt_status(cs) & MT_STATUS_BUSY) {}
}

__STATIC_INLINE void mt_dataout(unsigned int cs, uint8_t data)
{
    mt_write(cs | MT_A, data);
    while(mt_status(cs) & MT_STATUS_BUSY) {}
}

__STATIC_INLINE uint8_t mt_datain(unsigned int cs)
{
    return mt_read(cs | MT_A);
}

void mt_set_backlight(bool on)
{
    gpio_set_pin(MT_BACKLIGHT, on);
}

void mt_cls()
{
    unsigned int page, addr;
    for (page = 0; page < MT_PAGES_COUNT; ++page)
    {
        mt_cmd(MT_CS1, MT_CMD_SET_PAGE | page);
        mt_cmd(MT_CS1, MT_CMD_SET_ADDRESS | 0);
        for (addr = 0; addr < MT_SIZE_X; ++ addr)
            mt_dataout(MT_CS1, 0x00);
        mt_cmd(MT_CS2, MT_CMD_SET_PAGE | page);
        mt_cmd(MT_CS2, MT_CMD_SET_ADDRESS | 0);
        for (addr = 0; addr < MT_SIZE_X; ++ addr)
            mt_dataout(MT_CS2, 0x00);
    }
}

void mt_reset()
{
    stm32_bitbang_reset_pin(MT_RESET);
    delay_clks(TRI);
    stm32_bitbang_set_pin(MT_RESET);
    delay_clks(TR);
}

void mt_show(bool on)
{
    if (on)
        mt_cmd(MT_CS1 | MT_CS2, MT_CMD_DISPLAY_ON);
    else
        mt_cmd(MT_CS1 | MT_CS2, MT_CMD_DISPLAY_OFF);
}

void mt_set_pixel(unsigned int x, unsigned int y, bool set)
{
    uint8_t data;
    unsigned int cs, page, xr, yr;
    if (x >= MT_SIZE_X || y >= MT_SIZE_Y)
    {
        error(ERROR_OUT_OF_RANGE);
        return;
    }
    //find page & CS
    xr = X_TRANSFORM(x, 1);
    page = xr >> 3;
    if (y >= MT_SIZE_X)
    {
        yr = y - MT_SIZE_X;
        cs = MT_CS2;
    }
    else
    {
        yr = y;
        cs = MT_CS1;
    }
    mt_cmd(cs, MT_CMD_SET_PAGE | page);
    mt_cmd(cs, MT_CMD_SET_ADDRESS | yr);
    mt_datain(cs);
    data = mt_datain(cs);
    if (set)
        data |= 1 << (xr & 7);
    else
        data &= ~(1 << (xr & 7));
    mt_cmd(cs, MT_CMD_SET_ADDRESS | yr);
    mt_dataout(cs, data);
}

bool mt_get_pixel(unsigned int x, unsigned int y)
{
    uint8_t data;
    unsigned int cs, page, xr, yr;
    if (x >= MT_SIZE_X || y >= MT_SIZE_Y)
    {
        error(ERROR_OUT_OF_RANGE);
        return false;
    }
    //find page & CS
    xr = X_TRANSFORM(x, 1);
    page = xr >> 3;
    if (y >= MT_SIZE_X)
    {
        yr = y - MT_SIZE_X;
        cs = MT_CS2;
    }
    else
    {
        yr = y;
        cs = MT_CS1;
    }
    mt_cmd(cs, MT_CMD_SET_PAGE | page);
    mt_cmd(cs, MT_CMD_SET_ADDRESS | yr);
    mt_datain(cs);
    data = mt_datain(cs);
    return (data >> (xr & 7)) & 1;
}

#if (MT_TEST)
static void mt_poly_test(unsigned int y, unsigned int size)
{
    for (i = 0; i < MT_SIZE_X; ++i)
    {
        for (j = y; j < y + size; ++j)
            mt_set_pixel(i, j, true);
    }
}

void mt_pixel_test()
{
    unsigned int i, j, off, sz;
    off = 0;
    sz = 8;
    mt_poly_test(off, sz);
    off += sz * 2;

    mt_poly_test(off, sz);
    off += sz * 2;
    sz = 6;

    mt_poly_test(off, sz);
    off += sz * 2;

    mt_poly_test(off, sz);
    off += sz * 2;
    sz = 5;

    mt_poly_test(off, sz);
    off += sz * 2;

    mt_poly_test(off, sz);
    off += sz * 2;
    sz = 4;

    mt_poly_test(off, sz);
    off += sz * 2;

    mt_poly_test(off, sz);
    off += sz * 2;
    sz = 3;

    mt_poly_test(off, sz);
    off += sz * 2;

    mt_poly_test(off, sz);
    off += sz * 2;

    mt_poly_test(off, sz);
    off += sz * 2;

    mt_poly_test(off, sz);
    off += sz * 2;
    sz = 2;

    mt_poly_test(off, sz);
    off += sz * 2;

    mt_poly_test(off, sz);
    off += sz * 2;
    sz = 1;

    mt_poly_test(off, sz);
    off += sz * 2;

    mt_poly_test(off, sz);
    off += sz * 2;
}
#endif //MT_TEST

//here goes in each chip coords
static void mt_clear_rect_cs(unsigned int cs, RECT* rect, unsigned int mode)
{
    uint8_t buf[rect->height];
    uint8_t mask;
    uint8_t shift;
    unsigned int first_page, last_page, page, i;
    first_page = rect->left >> 3;
    last_page = ((rect->left + rect->width + 7) >> 3) - 1;
    shift = rect->left & 7;

    for (page = first_page; page <= last_page; ++page)
    {
        mask = 0xff;
        if (mode != MT_MODE_IGNORE)
        {
            if (page == first_page)
                mask >>= shift;
            if (page == last_page)
                mask &= ~((1 << (((last_page + 1) << 3) - (rect->left + rect->width))) - 1);
            mask = BIT_OUT_TRANSFORM(mask);
            //dump for masking
            if (mask != 0xff)
            {
                mt_cmd(cs, MT_CMD_SET_PAGE | PAGE_TRANSFORM(page));
                mt_cmd(cs, MT_CMD_SET_ADDRESS | rect->top);
                mt_datain(cs);
                for (i = 0; i < rect->height; ++i)
                    buf[i] = mt_datain(cs);
            }
        }
        mt_cmd(cs, MT_CMD_SET_PAGE | PAGE_TRANSFORM(page));
        mt_cmd(cs, MT_CMD_SET_ADDRESS | rect->top);
        for (i = 0; i < rect->height; ++i)
        {
            if (mode != MT_MODE_IGNORE && mask != 0xff)
                mt_dataout(cs, buf[i] & ~mask);
            else
                mt_dataout(cs, 0x00);
        }
    }
}

void mt_clear_rect(RECT* rect, unsigned int mode)
{
    RECT csrect;
    if (rect->left >= MT_SIZE_X || rect->top >= MT_SIZE_Y)
    {
        error(ERROR_INVALID_PARAMS);
        return;
    }
    csrect.width = rect->width;
    if (rect->left + rect->height > MT_SIZE_X)
        csrect.width = MT_SIZE_X - rect->left;
    csrect.left = rect->left;
    if (rect->left + rect->width > MT_SIZE_X)
        csrect.width = MT_SIZE_X - rect->left;
    //apply for CS1
    if (rect->top < MT_SIZE_X)
    {
        csrect.top = rect->top;
        csrect.height = rect->height;
        if (csrect.top + rect->height > MT_SIZE_X)
            csrect.height = MT_SIZE_X - rect->top;
        mt_clear_rect_cs(MT_CS1, &csrect, mode);
    }
    //apply for CS2
    if (rect->top + rect->height > MT_SIZE_X)
    {
        if (rect->top < MT_SIZE_X)
        {
            csrect.top = 0;
            csrect.height = rect->height - (MT_SIZE_X - rect->top);
        }
        else
        {
            csrect.top = rect->top - MT_SIZE_X;
            csrect.height = rect->height;
        }
        if (csrect.top + rect->height > MT_SIZE_X)
            csrect.height = MT_SIZE_X - rect->top;
        mt_clear_rect_cs(MT_CS2, &csrect, mode);
    }
}

//here goes in each chip coords
static void mt_write_rect_cs(unsigned int cs, RECT* rect, unsigned int mode, const uint8_t* data, unsigned int bpl, unsigned int offset)
{
    uint8_t buf[rect->height];
    uint8_t byte, mask;
    uint8_t shift = 0;
    unsigned int first_page, last_page, page, i, cur, byte_pos, bit_pos;
    first_page = rect->left >> 3;
    last_page = ((rect->left + rect->width + 7) >> 3) - 1;
    shift = rect->left & 7;

    for (page = first_page; page <= last_page; ++page)
    {
        mask = 0xff;
        if (page == first_page)
            mask >>= shift;
        if (page == last_page)
            mask &= ~((1 << (((last_page + 1) << 3) - (rect->left + rect->width))) - 1);
        mask = BIT_OUT_TRANSFORM(mask);
        if (mode != MT_MODE_IGNORE)
        {
            mt_cmd(cs, MT_CMD_SET_PAGE | PAGE_TRANSFORM(page));
            mt_cmd(cs, MT_CMD_SET_ADDRESS | rect->top);
            mt_datain(cs);
            for (i = 0; i < rect->height; ++i)
                buf[i] = mt_datain(cs);
        }
        mt_cmd(cs, MT_CMD_SET_PAGE | PAGE_TRANSFORM(page));
        mt_cmd(cs, MT_CMD_SET_ADDRESS | rect->top);
        for (i = 0; i < rect->height; ++i)
        {
            //absolute first bit offset in data stream
            cur = offset + i * bpl + ((page - first_page) << 3) + 8 - shift;
            byte_pos = cur >> 3;
            bit_pos = cur & 7;
            byte = (data[byte_pos] << (bit_pos)) & 0xff;
            if (bit_pos)
                byte |= data[byte_pos + 1] >> (8 - bit_pos);
            byte = BIT_OUT_TRANSFORM(byte) & mask;
            switch (mode)
            {
            case MT_MODE_OR:
                mt_dataout(cs, buf[i] | byte);
                break;
            case MT_MODE_XOR:
                mt_dataout(cs, buf[i] ^ byte);
                break;
            case MT_MODE_FILL:
                mt_dataout(cs, (buf[i] & ~mask) | byte);
                break;
            default:
                mt_dataout(cs, byte);
            }
        }
    }
}

void mt_write_rect(RECT* rect, unsigned int mode, const uint8_t* data)
{
    RECT csrect;
    //bits per line
    unsigned int offset;
    if (rect->left >= MT_SIZE_X || rect->top >= MT_SIZE_Y)
    {
        error(ERROR_INVALID_PARAMS);
        return;
    }
    offset = 0;
    csrect.width = rect->width;
    if (rect->left + rect->height > MT_SIZE_X)
        csrect.width = MT_SIZE_X - rect->left;
    csrect.left = rect->left;
    if (rect->left + rect->width > MT_SIZE_X)
        csrect.width = MT_SIZE_X - rect->left;
    //apply for CS1
    if (rect->top < MT_SIZE_X)
    {
        csrect.top = rect->top;
        csrect.height = rect->height;
        if (csrect.top + rect->height > MT_SIZE_X)
            csrect.height = MT_SIZE_X - rect->top;
        offset = rect->width * csrect.height;
        mt_write_rect_cs(MT_CS1, &csrect, mode, data, rect->width, 0);
    }
    //apply for CS2
    if (rect->top + rect->height > MT_SIZE_X)
    {
        if (rect->top < MT_SIZE_X)
        {
            csrect.top = 0;
            csrect.height = rect->height - (MT_SIZE_X - rect->top);
        }
        else
        {
            csrect.top = rect->top - MT_SIZE_X;
            csrect.height = rect->height;
        }
        if (csrect.top + rect->height > MT_SIZE_X)
            csrect.height = MT_SIZE_X - rect->top;
        mt_write_rect_cs(MT_CS2, &csrect, mode, data, rect->width, offset);
    }
}

void mt_init()
{
    stm32_bitbang_enable_mask(DATA_PORT, DATA_MASK);
    stm32_bitbang_enable_mask(ADDSET_PORT, ADDSET_MASK);
    stm32_bitbang_enable_pin(MT_RESET);
    stm32_bitbang_enable_pin(MT_STROBE);
    stm32_bitbang_set_pin(MT_STROBE);
    //doesn't need to be so fast as others
    gpio_enable_pin(MT_BACKLIGHT, PIN_MODE_OUT);

    mt_reset();
    mt_cls();

}

#if (MT_DRIVER)
#if (SYS_INFO)
static inline void mt_info()
{
    TIME uptime;
    get_uptime(&uptime);

    mt_pixel_test();
    printf("pixel test time(us): %dus\n\r", time_elapsed_us(&uptime));
}
#endif


void mt_clear_rect_driver(HANDLE process)
{
    MT_REQUEST req;
    if (direct_read(process, (void*)&req, sizeof(MT_REQUEST)))
        mt_clear_rect(&req.rect, req.mode);
}

void mt_write_rect_driver(HANDLE process)
{
    MT_REQUEST req;
    uint8_t* ptr;
    if (direct_read(process, (void*)&req, sizeof(MT_REQUEST)))
    {
        ptr = block_open(req.block);
        if (ptr)
            mt_write_rect(&req.rect, req.mode, ptr);
    }
}

void mt()
{
    mt_init();
    IPC ipc;
#if (SYS_INFO)
    open_stdout();
#endif
    for (;;)
    {
        error(ERROR_OK);
        need_post = false;
        ipc_read_ms(&ipc, 0, 0);
        switch (ipc.cmd)
        {
        case IPC_PING:
            need_post = true;
            break;
        case IPC_CALL_ERROR:
            break;
#if (SYS_INFO)
        case IPC_GET_INFO:
            mt_info(drv);
            need_post = true;
            break;
#endif
        case MT_RESET:
            mt_reset();
            need_post = true;
            break;
        case MT_SHOW:
            mt_show(ipc.param1);
            need_post = true;
            break;
        case MT_BACKLIGHT:
            mt_backlight(ipc.param1);
            need_post = true;
            break;
        case MT_CLS:
            mt_cls();
            need_post = true;
            break;
        case MT_SET_PIXEL:
            mt_set_pixel(ipc.param1, ipc.param2, ipc.param3);
            need_post = true;
            break;
        case MT_GET_PIXEL:
            ipc.param1 = mt_get_pixel(ipc.param1, ipc.param2);
            need_post = true;
            break;
        case MT_CLEAR_RECT:
            mt_clear_rect_driver(ipc.process);
            need_post = true;
            break;
        case MT_WRITE_RECT:
            mt_write_rect_driver(ipc.process);
            need_post = true;
            break;
#if (MT_TEST)
        case MT_PIXEL_TEST:
            mt_pixel_test();
            need_post = true;
            break;
#endif
        default:
            error(ERROR_NOT_SUPPORTED);
            need_post = true;
        }
        if (need_post)
            ipc_post_or_error(&ipc);
    }
}

#endif
