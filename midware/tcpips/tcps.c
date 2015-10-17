/*
    RExOS - embedded RTOS
    Copyright (c) 2011-2015, Alexey Kramarenko
    All rights reserved.
*/

#include "tcps.h"
#include "tcpips_private.h"
#include "../../userspace/tcp.h"
#include "../../userspace/stdio.h"
#include "../../userspace/endian.h"
#include "../../userspace/systime.h"
#include "icmps.h"
#include <string.h>

#define TCP_MSS_MAX                                      (IP_FRAME_MAX_DATA_SIZE - sizeof(TCP_HEADER))
#define TCP_MSS_MIN                                      536

#pragma pack(push, 1)
typedef struct {
    uint8_t src_port_be[2];
    uint8_t dst_port_be[2];
    uint8_t seq_be[4];
    uint8_t ack_be[4];
    //not byte aligned
    uint8_t data_off;
    uint8_t flags;
    uint8_t window_be[2];
    uint8_t checksum_be[2];
    uint8_t urgent_pointer_be[2];
} TCP_HEADER;

typedef struct {
    uint8_t kind;
    uint8_t len;
    uint8_t data[65495];
} TCP_OPT;
#pragma pack(pop)

typedef struct {
    HANDLE process;
    uint16_t port;
} TCP_LISTEN_HANDLE;

typedef enum {
    TCP_STATE_CLOSED = 0,
    TCP_STATE_LISTEN,
    TCP_STATE_SYN_SENT,
    TCP_STATE_SYN_RECEIVED,
    TCP_STATE_ESTABLISHED,
    TCP_STATE_FIN_WAIT_1,
    TCP_STATE_FIN_WAIT_2,
    TCP_STATE_CLOSING,
    TCP_STATE_LAST_ACK,
    TCP_STATE_TIME_WAIT,
    TCP_STATE_MAX
} TCP_STATE;

typedef struct {
    HANDLE process;
    IP remote_addr;
    IO* rx;
    IO* rx_tmp;
    IO* tx;
    uint32_t snd_una, snd_nxt, rcv_nxt;

    TCP_STATE state;
    uint16_t remote_port, local_port, mss, rx_wnd, tx_wnd;
    bool active, transmit, fin;
    //TODO: time
} TCP_TCB;

#if (TCP_DEBUG_FLOW)
static const char* __TCP_FLAGS[TCP_FLAGS_COUNT] =                   {"FIN", "SYN", "RST", "PSH", "ACK", "URG"};
static const char* __TCP_STATES[TCP_STATE_MAX] =                    {"CLOSED", "LISTEN", "SYN SENT", "SYN RECEIVED", "ESTABLISHED", "FIN WAIT1",
                                                                     "FIN WAIT2", "CLOSING", "LAST ACK", "TIME_WAIT"};
#endif //TCP_DEBUG_FLOW

static inline unsigned int tcps_data_offset(IO* io)
{
    TCP_HEADER* tcp = io_data(io);
    return (tcp->data_off >> 4) << 2;
}

static inline unsigned int tcps_data_len(IO* io)
{
    unsigned int res = tcps_data_offset(io);
    return io->data_size > res ? io->data_size - res : 0;
}

static inline unsigned int tcps_seg_len(IO* io)
{
    unsigned int res;
    TCP_HEADER* tcp = io_data(io);
    res = tcps_data_len(io);
    if (tcp->flags & TCP_FLAG_SYN)
        ++res;
    if (tcp->flags & TCP_FLAG_FIN)
        ++res;
    return res;
}

static uint32_t tcps_delta(uint32_t from, uint32_t to)
{
    if (to >= from)
        return to - from;
    else
        return (0xffffffff - from) + to + 1;
}

static int tcps_diff(uint32_t from, uint32_t to)
{
    uint32_t res = tcps_delta(from, to);
    if (res <= 0xffff)
        return (int)res;
    res = tcps_delta(to, from);
    if (res <= 0xffff)
        return -(int)res;
    return 0x10000;
}

static unsigned int tcps_get_first_opt(IO* io)
{
    TCP_OPT* opt;
    if (tcps_data_offset(io) <= sizeof(TCP_HEADER))
        return 0;
    opt = (TCP_OPT*)((uint8_t*)io_data(io) + sizeof(TCP_HEADER));
    return (opt->kind == TCP_OPTS_END) ? 0 : sizeof(TCP_HEADER);
}

static unsigned int tcps_get_next_opt(IO* io, unsigned int prev)
{
    TCP_OPT* opt;
    unsigned int res;
    unsigned int offset = tcps_data_offset(io);
    opt = (TCP_OPT*)((uint8_t*)io_data(io) + prev);
    switch(opt->kind)
    {
    case TCP_OPTS_END:
        //end of list
        return 0;
    case TCP_OPTS_NOOP:
        res = prev + 1;
        break;
    default:
        res = prev + opt->len;
    }
    return res < offset ? res : 0;
}

#if (TCP_DEBUG_FLOW)
static void tcps_debug(IO* io, const IP* src, const IP* dst)
{
    bool has_flag;
    int i, j;
    TCP_HEADER* tcp = io_data(io);
    TCP_OPT* opt;
    printf("TCP: ");
    ip_print(src);
    printf(":%d -> ", be2short(tcp->src_port_be));
    ip_print(dst);
    printf(":%d <SEQ=%u>", be2short(tcp->dst_port_be), be2int(tcp->seq_be));
    if (tcp->flags & TCP_FLAG_ACK)
        printf("<ACK=%u>", be2int(tcp->ack_be));
    printf("<WND=%d>", be2short(tcp->window_be));
    if (tcp->flags & TCP_FLAG_MSK)
    {
        printf("<CTL=");
        has_flag = false;
        for (i = 0; i < TCP_FLAGS_COUNT; ++i)
            if (tcp->flags & (1 << i))
            {
                if (has_flag)
                    printf(",");
                printf(__TCP_FLAGS[i]);
                has_flag = true;
            }
        printf(">");
    }
    if (tcps_data_len(io))
        printf("<DATA=%d byte(s)>", tcps_data_len(io));
    if ((i = tcps_get_first_opt(io)) != 0)
    {
        has_flag = false;
        printf("<OPTS=");
        for (; i; i = tcps_get_next_opt(io, i))
        {
            if (has_flag)
                printf(",");
            opt = (TCP_OPT*)((uint8_t*)io_data(io) + i);
            switch(opt->kind)
            {
            case TCP_OPTS_NOOP:
                printf("NOOP");
                break;
            case TCP_OPTS_MSS:
                printf("MSS:%d", be2short(opt->data));
                break;
            default:
                printf("K%d", opt->kind);
                for (j = 0; j < opt->len - 2; ++j)
                {
                    printf(j ? " " : ":");
                    printf("%02X", opt->data[j]);
                }
            }
            has_flag = true;
        }
        printf(">");
    }
    printf("\n");
}

static void tcps_debug_state(TCP_STATE from, TCP_STATE to)
{
    printf("%s -> %s\n", __TCP_STATES[from], __TCP_STATES[to]);
}
#endif //TCP_DEBUG_FLOW

static inline void tcps_set_state(TCP_TCB* tcb, TCP_STATE state)
{
#if (TCP_DEBUG_FLOW)
    tcps_debug_state(tcb->state, state);
#endif //TCP_DEBUG_FLOW
    tcb->state = state;
}

static uint32_t tcps_gen_isn()
{
    SYSTIME uptime;
    get_uptime(&uptime);
    //increment every 4us
    return (uptime.sec % 17179) + (uptime.usec >> 2);
}

static void tcps_update_rx_wnd(TCP_TCB* tcb)
{
    tcb->rx_wnd = TCP_MSS_MAX;
    if (tcb->rx != NULL)
        tcb->rx_wnd += io_get_free(tcb->rx);
    if (tcb->rx_tmp != NULL)
        tcb->rx_wnd = io_get_free(tcb->rx_tmp);
}

static HANDLE tcps_find_listener(TCPIPS* tcpips, uint16_t port)
{
    HANDLE handle;
    TCP_LISTEN_HANDLE* tlh;
    for (handle = so_first(&tcpips->tcps.listen); handle != INVALID_HANDLE; handle = so_next(&tcpips->tcps.listen, handle))
    {
        tlh = so_get(&tcpips->tcps.listen, handle);
        if (tlh->port == port)
            return tlh->process;
    }
    return INVALID_HANDLE;
}

static HANDLE tcps_find_tcb(TCPIPS* tcpips, const IP* src, uint16_t remote_port, uint16_t local_port)
{
    HANDLE handle;
    TCP_TCB* tcb;
    for (handle = so_first(&tcpips->tcps.tcbs); handle != INVALID_HANDLE; handle = so_next(&tcpips->tcps.tcbs, handle))
    {
        tcb = so_get(&tcpips->tcps.tcbs, handle);
        if (tcb->remote_port == remote_port && tcb->local_port == local_port && tcb->remote_addr.u32.ip == src->u32.ip)
            return handle;
    }
    return INVALID_HANDLE;
}

//TODO:
/*
static inline uint16_t tcps_allocate_port(TCPIPS* tcpips)
{
    unsigned int res;
    //from current to HI
    for (res = tcpips->tcps.dynamic; res <= TCPIP_DYNAMIC_RANGE_HI; ++res)
    {
        if (udps_find(tcpips, res) == INVALID_HANDLE)
        {
            tcpips->tcps.dynamic = res == TCPIP_DYNAMIC_RANGE_HI ? TCPIP_DYNAMIC_RANGE_LO : res + 1;
            return (uint16_t)res;
        }

    }
    //from LO to current
    for (res = TCPIP_DYNAMIC_RANGE_LO; res < tcpips->tcps.dynamic; ++res)
    {
        if (udps_find(tcpips, res) == INVALID_HANDLE)
        {
            tcpips->udps.dynamic = res + 1;
            return res;
        }

    }
    error(ERROR_TOO_MANY_HANDLES);
    return 0;
}
*/

static HANDLE tcps_create_tcb(TCPIPS* tcpips, const IP* remote_addr, uint16_t remote_port, uint16_t local_port)
{
    TCP_TCB* tcb;
    HANDLE handle = so_allocate(&tcpips->tcps.tcbs);
    if (handle == INVALID_HANDLE)
        return handle;
    tcb = so_get(&tcpips->tcps.tcbs, handle);
    tcb->process = INVALID_HANDLE;
    tcb->remote_addr.u32.ip = remote_addr->u32.ip;
    tcb->snd_una = tcb->snd_nxt = 0;
    tcb->rcv_nxt = 0;
    tcb->state = TCP_STATE_CLOSED;
    tcb->remote_port = remote_port;
    tcb->local_port = local_port;
    tcb->mss = TCP_MSS_MAX;
    tcb->active = false;
    tcb->transmit = false;
    tcb->fin = false;
    tcb->rx = tcb->tx = tcb->rx_tmp = NULL;
    tcps_update_rx_wnd(tcb);
    tcb->tx_wnd = 0;
    return handle;
}

static void tcps_destroy_tcb(TCPIPS* tcpips, HANDLE tcb_handle)
{
    //TODO: free timers
#if (TCP_DEBUG_FLOW)
    TCP_TCB* tcb = so_get(&tcpips->tcps.tcbs, tcb_handle);
    printf("%s -> 0\n", __TCP_STATES[tcb->state]);
#endif //TCP_DEBUG_FLOW
    so_free(&tcpips->tcps.tcbs, tcb_handle);
}

static inline bool tcps_set_mss(TCP_TCB* tcb, uint16_t mss)
{
    if (mss < TCP_MSS_MIN || mss > TCP_MSS_MAX)
        return false;
    tcb->mss = mss;
    return true;
}

static void tcps_apply_options(TCPIPS* tcpips, IO* io, TCP_TCB* tcb)
{
    int i;
    TCP_OPT* opt;
    for (i = tcps_get_first_opt(io); i; i = tcps_get_next_opt(io, i))
    {
        opt = (TCP_OPT*)((uint8_t*)io_data(io) + i);
        switch(opt->kind)
        {
        case TCP_OPTS_MSS:
#if (ICMP)
            if (!tcps_set_mss(tcb, be2short(opt->data)))
                icmps_tx_error(tcpips, io, ICMP_ERROR_PARAMETER, ((IP_STACK*)io_stack(io))->hdr_size + sizeof(TCP_HEADER) + i);
#else
            tcps_set_mss(tcpips, tcb, be2short(opt->data));
#endif //ICMP
            break;
        default:
            break;
        }
    }
}

static IO* tcps_allocate_io(TCPIPS* tcpips, TCP_TCB* tcb)
{
    TCP_HEADER* tcp;
    IO* io = ips_allocate_io(tcpips, IP_FRAME_MAX_DATA_SIZE, PROTO_TCP);
    if (io == NULL)
        return NULL;
    tcp = io_data(io);
    short2be(tcp->src_port_be, tcb->local_port);
    short2be(tcp->dst_port_be, tcb->remote_port);
    int2be(tcp->seq_be, 0);
    int2be(tcp->ack_be, 0);
    tcp->data_off = (sizeof(TCP_HEADER) >> 2) << 4;
    tcp->flags = 0;
    short2be(tcp->urgent_pointer_be, 0);
    short2be(tcp->checksum_be, 0);
    io->data_size = sizeof(TCP_HEADER);
    return io;
}

static void tcps_tx(TCPIPS* tcpips, IO* io, TCP_TCB* tcb)
{
    TCP_HEADER* tcp = io_data(io);
    short2be(tcp->window_be, tcb->rx_wnd);
    short2be(tcp->checksum_be, tcp_checksum(io_data(io), io->data_size, &tcpips->ip, &tcb->remote_addr));
#if (TCP_DEBUG_FLOW)
    tcps_debug(io, &tcpips->ip, &tcb->remote_addr);
#endif //TCP_DEBUG_FLOW
    ips_tx(tcpips, io, &tcb->remote_addr);
}

static void tcps_tx_rst(TCPIPS* tcpips, HANDLE tcb_handle, uint32_t seq)
{
    IO* tx;
    TCP_HEADER* tcp_tx;
    TCP_TCB* tcb = so_get(&tcpips->tcps.tcbs, tcb_handle);

    if ((tx = tcps_allocate_io(tcpips, tcb)) == NULL)
        return;

    tcp_tx = io_data(tx);
    tcp_tx->flags |= TCP_FLAG_RST;
    int2be(tcp_tx->seq_be, seq);
    tcps_tx(tcpips, tx, tcb);
}

static void tcps_tx_rst_ack(TCPIPS* tcpips, HANDLE tcb_handle, uint32_t ack)
{
    IO* tx;
    TCP_HEADER* tcp_tx;
    TCP_TCB* tcb = so_get(&tcpips->tcps.tcbs, tcb_handle);

    if ((tx = tcps_allocate_io(tcpips, tcb)) == NULL)
        return;

    tcp_tx = io_data(tx);
    tcp_tx->flags |= TCP_FLAG_RST | TCP_FLAG_ACK;
    int2be(tcp_tx->seq_be, 0);
    int2be(tcp_tx->ack_be, ack);
    tcps_tx(tcpips, tx, tcb);
}

static void tcps_tx_ack(TCPIPS* tcpips, HANDLE tcb_handle)
{
    IO* tx;
    TCP_HEADER* tcp_tx;
    TCP_TCB* tcb = so_get(&tcpips->tcps.tcbs, tcb_handle);

    if ((tx = tcps_allocate_io(tcpips, tcb)) == NULL)
        return;

    tcp_tx = io_data(tx);

    tcp_tx->flags |= TCP_FLAG_ACK;
    int2be(tcp_tx->seq_be, tcb->snd_una);
    int2be(tcp_tx->ack_be, tcb->rcv_nxt);
    tcps_tx(tcpips, tx, tcb);
}

static void tcps_tx_text_ack_fin(TCPIPS* tcpips, HANDLE tcb_handle)
{
    IO* tx;
    TCP_HEADER* tcp_tx;
    TCP_TCB* tcb = so_get(&tcpips->tcps.tcbs, tcb_handle);

    if ((tx = tcps_allocate_io(tcpips, tcb)) == NULL)
        return;

    tcp_tx = io_data(tx);

    //TODO: real data
    tcp_tx->flags |= TCP_FLAG_ACK;
    if (tcb->fin && tcb->snd_una != tcb->snd_nxt)
        tcp_tx->flags |= TCP_FLAG_FIN;
    int2be(tcp_tx->seq_be, tcb->snd_una);
    int2be(tcp_tx->ack_be, tcb->rcv_nxt);
    tcps_tx(tcpips, tx, tcb);
    //TODO: retransmit timer
}

static void tcps_tx_syn_ack(TCPIPS* tcpips, HANDLE tcb_handle)
{
    IO* tx;
    TCP_HEADER* tcp_tx;
    TCP_TCB* tcb = so_get(&tcpips->tcps.tcbs, tcb_handle);

    if ((tx = tcps_allocate_io(tcpips, tcb)) == NULL)
        return;

    tcp_tx = io_data(tx);

    //add ACK, SYN flags
    tcp_tx->flags |= TCP_FLAG_ACK | TCP_FLAG_SYN;
    //TODO: add MSS to option list

    int2be(tcp_tx->seq_be, tcb->snd_una);
    int2be(tcp_tx->ack_be, tcb->rcv_nxt);
    tcps_tx(tcpips, tx, tcb);
}

static inline void tcps_rx_closed(TCPIPS* tcpips, IO* io, HANDLE tcb_handle)
{
    TCP_HEADER* tcp;
    tcp = io_data(io);

    do {
        //An incoming segment containing a RST is discarded
        if (tcp->flags & TCP_FLAG_RST)
            break;

        //An incoming segment not containing a RST causes a RST to be sent in response
        if (tcp->flags & TCP_FLAG_ACK)
            tcps_tx_rst(tcpips, tcb_handle, be2int(tcp->ack_be));
        else
            tcps_tx_rst_ack(tcpips, tcb_handle, be2int(tcp->seq_be) + tcps_seg_len(io));
    } while (false);
    tcps_destroy_tcb(tcpips, tcb_handle);
}

static inline void tcps_rx_listen(TCPIPS* tcpips, IO* io, HANDLE tcb_handle)
{
    TCP_HEADER* tcp;
    TCP_TCB* tcb = so_get(&tcpips->tcps.tcbs, tcb_handle);
    tcp = io_data(io);

    do {
        //An incoming RST should be ignored
        if (tcp->flags & TCP_FLAG_RST)
            break;

        //An acceptable reset segment should be formed for any arriving ACK-bearing segment
        if (tcp->flags & TCP_FLAG_ACK)
        {
            tcps_tx_rst(tcpips, tcb_handle, be2int(tcp->ack_be));
            break;
        }

        if (tcp->flags & TCP_FLAG_SYN)
        {
            tcps_set_state(tcb, TCP_STATE_SYN_RECEIVED);
            tcb->rcv_nxt = be2int(tcp->seq_be) + 1;
            tcb->snd_una = tcb->snd_nxt = tcps_gen_isn();
            ++tcb->snd_nxt;

            tcps_tx_syn_ack(tcpips, tcb_handle);
            return;
        }

        //You are unlikely to get here, but if you do, drop the segment, and return
    } while (false);
    tcps_destroy_tcb(tcpips, tcb_handle);
}

static inline bool tcps_rx_otw_check_seq(TCPIPS* tcpips, IO* io, HANDLE tcb_handle)
{
    int seq_delta, seg_len;
    unsigned int data_off;
    uint32_t seq;
    IO* tx;
    TCP_HEADER* tcp;
    TCP_HEADER* tcp_tx;
    TCP_TCB* tcb = so_get(&tcpips->tcps.tcbs, tcb_handle);
    tcp = io_data(io);

    seq = be2int(tcp->seq_be);
    seq_delta = tcps_diff(tcb->rcv_nxt, seq);
    seg_len = tcps_seg_len(io);
    //already paritally received segment
    if (seq_delta < 0)
    {
        if (seg_len + seq_delta <= 0)
        {
#if (TCP_DEBUG_FLOW)
            printf("TCP: Dup\n");
#endif //TCP_DEBUG_FLOW
            return false;
        }
#if (TCP_DEBUG_FLOW)
        printf("TCP: partial receive %d seq\n", seg_len + seq_delta);
#endif //TCP_DEBUG_FLOW
        //SYN flag space goes first, remove from sequence
        if (tcp->flags & TCP_FLAG_SYN)
        {
            tcp->flags &= ~TCP_FLAG_SYN;
            --seg_len;
            ++seq_delta;
            ++seq;
        }
        data_off = tcps_data_offset(io);
        seg_len -= -seq_delta;
        //FIN is not in data, but occupying virtual byte
        if (tcp->flags & TCP_FLAG_FIN)
        {
            memmove((uint8_t*)io_data(io) + data_off, (uint8_t*)io_data(io) + data_off + (-seq_delta), seg_len - 1);
            io->data_size -= (-seq_delta) - 1;
        }
        else
        {
            memmove((uint8_t*)io_data(io) + data_off, (uint8_t*)io_data(io) + data_off + (-seq_delta), seg_len);
            io->data_size -= -seq_delta;
        }
        seq += -seq_delta;
        seq_delta = 0;
    }
    //don't fit in rx window
    if (seg_len > tcb->rx_wnd && tcb->rx_wnd > 0)
    {
#if (TCP_DEBUG_FLOW)
        printf("TCP: chop rx wnd %d seq\n", seg_len - tcb->rx_wnd);
#endif //TCP_DEBUG_FLOW
        //FIN is last virtual byte, remove it first
        if (tcp->flags & TCP_FLAG_FIN)
        {
            tcp->flags &= ~TCP_FLAG_FIN;
            --seg_len;
        }
        //still don't fit? remove some data
        if (seg_len > tcb->rx_wnd)
        {
            io->data_size = seg_len - tcb->rx_wnd;
            seg_len = tcb->rx_wnd;
        }
        //remove PSH flag, cause it's goes after all bytes
        tcp->flags &= ~TCP_FLAG_PSH;
    }
    if (seq != tcb->rcv_nxt || seg_len > tcb->rx_wnd)
    {
#if (TCP_DEBUG_FLOW)
        printf("TCP: boundary fail\n");
#endif //TCP_DEBUG_FLOW
        //RST bit is set, drop the segment and return:
        if (tcp->flags & TCP_FLAG_RST)
            return false;

        if ((tx = tcps_allocate_io(tcpips, tcb)) == NULL)
            return false;

        tcp_tx = io_data(tx);
        tcp_tx->flags |= TCP_FLAG_ACK;
        int2be(tcp_tx->seq_be, tcb->snd_una);
        tcps_tx(tcpips, tx, tcb);
        return false;
    }
    return true;
}

static inline void tcps_rx_otw_syn_rst(TCPIPS* tcpips, IO* io, HANDLE tcb_handle)
{
    TCP_TCB* tcb = so_get(&tcpips->tcps.tcbs, tcb_handle);
    switch (tcb->state)
    {
    case TCP_STATE_SYN_RECEIVED:
        if (tcb->active)
        {
            printf("TODO: SYN-RECEIVED: inform user\n");
        }
        break;
    case TCP_STATE_ESTABLISHED:
    case TCP_STATE_FIN_WAIT_1:
    case TCP_STATE_FIN_WAIT_2:
        printf("TODO: RST flush rx/tx\n");
        printf("TODO: RST inform user on connection closed\n");
        break;
    default:
        break;
    }
    //enter closed state, destroy TCB and return
    tcps_destroy_tcb(tcpips, tcb_handle);
}

static inline bool tcps_rx_otw_ack(TCPIPS* tcpips, IO* io, HANDLE tcb_handle)
{
    int snd_diff, ack_diff;
    TCP_HEADER* tcp;
    TCP_TCB* tcb = so_get(&tcpips->tcps.tcbs, tcb_handle);
    tcp = io_data(io);
    snd_diff = tcps_diff(tcb->snd_una, tcb->snd_nxt);
    ack_diff = tcps_diff(tcb->snd_una, be2int(tcp->ack_be));

    if (tcb->state == TCP_STATE_SYN_RECEIVED)
    {
        //SND.UNA =< SEG.ACK =< SND.NXT
        if (ack_diff >= 0 && ack_diff <= snd_diff)
        {
            tcps_set_state(tcb, TCP_STATE_ESTABLISHED);
            ipc_post_inline(tcb->process, HAL_CMD(HAL_TCP, IPC_OPEN), tcb_handle, tcb->remote_addr.u32.ip, 0);
            //and continue processing in that state
            return true;
        }
        else
        {
            //form a reset segment
            tcps_tx_rst(tcpips, tcb_handle, be2int(tcp->ack_be));
            return false;
        }
    }
    //SEG.ACK > SND.NXT
    if (ack_diff > snd_diff)
    {
#if (TCP_DEBUG_FLOW)
        printf("TCP: SEG.ACK > SND.NEXT. Keep-alive?\n");
#endif //TCP_DEBUG_FLOW
        tcps_tx_ack(tcpips, tcb_handle);
        return false;
    }

    //adjust ack
    if (ack_diff > 0)
    {
        tcb->snd_una += ack_diff;
        //TODO: return sent buffers to user
    }

    switch (tcb->state)
    {
    case TCP_STATE_FIN_WAIT_1:
        if (tcb->snd_nxt == tcb->snd_una)
            tcps_set_state(tcb, TCP_STATE_FIN_WAIT_2);
        break;
    case TCP_STATE_FIN_WAIT_2:
        printf("In addition to the processing for the ESTABLISHED state, if the retransmission queue is empty, the user’s CLOSE can be acknowledged\n");
        break;
    case TCP_STATE_CLOSING:
        if (tcb->snd_nxt == tcb->snd_una)
            tcps_set_state(tcb, TCP_STATE_TIME_WAIT);
        break;
    case TCP_STATE_LAST_ACK:
        if (tcb->snd_nxt == tcb->snd_una)
        {
            tcps_destroy_tcb(tcpips, tcb_handle);
            return false;
        }
        break;
    default:
        break;
    }
    return true;
}

static inline void tcps_rx_otw_text(TCPIPS* tcpips, IO* io, HANDLE tcb_handle)
{
    TCP_STACK* tcp_stack;
    TCP_HEADER* tcp;
    TCP_HEADER* tcp_tmp;
    unsigned int data_size, data_offset, size, data_offset_tmp;
    uint16_t urg, urg_tmp;
    TCP_TCB* tcb = so_get(&tcpips->tcps.tcbs, tcb_handle);
    tcp = io_data(io);

    switch (tcb->state)
    {
    case TCP_STATE_ESTABLISHED:
    case TCP_STATE_FIN_WAIT_1:
    case TCP_STATE_FIN_WAIT_2:
        data_size = tcps_data_len(io);
        if (data_size)
        {
            data_offset = tcps_data_offset(io);
            urg = 0;
            if (tcp->flags & TCP_FLAG_URG)
            {
                urg = be2int(tcp->urgent_pointer_be);
                //make sure urg don't overlaps total data size
                if (urg > data_size)
                {
                    urg = data_size;
                    short2be(tcp->urgent_pointer_be, urg);
                }
            }
            tcb->rcv_nxt += data_size;
            //has user block
            if (tcb->rx != NULL)
            {
                size = data_size;
                if (size > io_get_free(tcb->rx))
                    size = io_get_free(tcb->rx);
                memcpy((uint8_t*)io_data(tcb->rx) + tcb->rx->data_size, (uint8_t*)io_data(io) + data_offset, size);
                tcb->rx->data_size += size;
                data_offset += size;
                data_size -= size;

                //apply flags, urgent data
                tcp_stack = (TCP_STACK*)io_stack(tcb->rx);
                if (tcp->flags & TCP_FLAG_PSH)
                    tcp_stack->flags |= TCP_PSH;
                if (urg)
                {
                    tcp_stack->flags |= TCP_URG;
                    tcp_stack->urg_len = urg;
                    if (urg > size)
                        urg -= size;
                    else
                        urg = 0;
                }

                //filled, send to user
                if ((io_get_free(tcb->rx) == 0) || (tcp->flags & TCP_FLAG_PSH))
                {
                    io_complete(tcb->process, HAL_IO_CMD(HAL_TCP, IPC_READ), tcb_handle, tcb->rx);
                    tcb->rx = NULL;
                }

            }
            //no user block/no fit
            if (data_size)
            {
                //move to tmp
                if (tcb->rx_tmp == NULL)
                    tcb->rx_tmp = io;
                //append to tmp
                else
                {
                    tcp_tmp = io_data(tcb->rx_tmp);
                    //apply flags, urgent data
                    if (tcp->flags & TCP_FLAG_PSH)
                        tcp_tmp->flags |= TCP_PSH;
                    if (urg)
                    {
                        data_offset_tmp = tcps_data_offset(tcb->rx_tmp);
                        urg_tmp = 0;
                        //already have urgent pointer? add to end of urgent data
                        if (tcp_tmp->flags & TCP_FLAG_URG)
                            urg_tmp = be2short(tcp_tmp->urgent_pointer_be);
                        memmove((uint8_t*)io_data(tcb->rx_tmp) + data_offset_tmp + urg_tmp + urg,
                                (uint8_t*)io_data(tcb->rx_tmp) + data_offset_tmp + urg_tmp,
                                tcb->rx_tmp->data_size - data_offset - urg_tmp);
                        memcpy((uint8_t*)io_data(tcb->rx_tmp) + data_offset_tmp + urg_tmp, (uint8_t*)io_data(io) + data_offset, urg);
                        tcb->rx_tmp->data_size += urg;
                        data_offset += urg;
                        data_size -= urg;
                        tcp_tmp->flags |= TCP_FLAG_URG;
                        short2be(tcp_tmp->urgent_pointer_be, urg_tmp + urg);
                    }
                    memcpy((uint8_t*)io_data(tcb->rx_tmp) + tcb->rx_tmp->data_size, (uint8_t*)io_data(io) + data_offset, data_size);
                    tcb->rx_tmp->data_size += data_size;
                }
            }
            tcps_update_rx_wnd(tcb);
        }
        break;
    default:
        //Ignore the segment text
        break;
    }
}

static inline void tcps_rx_otw_fin(TCPIPS* tcpips, HANDLE tcb_handle)
{
    TCP_TCB* tcb = so_get(&tcpips->tcps.tcbs, tcb_handle);

    //ack FIN
    ++tcb->rcv_nxt;
    if (!tcb->fin)
    {
        tcb->fin = true;
        ++tcb->snd_nxt;
    }
    switch (tcb->state)
    {
    case TCP_STATE_ESTABLISHED:
        //TODO: return all rx buffers
        //inform user
        ipc_post_inline(tcb->process, HAL_CMD(HAL_TCP, IPC_CLOSE), tcb_handle, 0, 0);
        //follow down
    case TCP_STATE_SYN_RECEIVED:
        tcps_set_state(tcb, TCP_STATE_LAST_ACK);
        break;
    case TCP_STATE_FIN_WAIT_1:
        if (tcb->snd_una == tcb->snd_nxt)
            tcps_set_state(tcb, TCP_STATE_TIME_WAIT);
        else
            tcps_set_state(tcb, TCP_STATE_CLOSING);
        break;
    case TCP_STATE_FIN_WAIT_2:
        tcps_set_state(tcb, TCP_STATE_TIME_WAIT);
        break;
    default:
        break;
    }
}

static inline void tcps_rx_send(TCPIPS* tcpips, HANDLE tcb_handle)
{
    TCP_TCB* tcb = so_get(&tcpips->tcps.tcbs, tcb_handle);

    //ack from remote host - we transmitted all
    if (tcb->state == TCP_STATE_ESTABLISHED && tcb->transmit && (tcb->snd_una == tcb->snd_nxt) && !tcb->fin)
    {
        tcb->transmit = false;
        return;
    }

    if (tcb->state == TCP_STATE_TIME_WAIT)
    {
        //TODO: Don't send anything, just restart timer
    }
    else
        tcps_tx_text_ack_fin(tcpips, tcb_handle);
}

static inline void tcps_rx_otw(TCPIPS* tcpips, IO* io, HANDLE tcb_handle)
{
    TCP_HEADER* tcp = io_data(io);

    //first check sequence number
    if (!tcps_rx_otw_check_seq(tcpips, io, tcb_handle))
        return;

    //second check the RST bit
    //fourth, check the SYN bit
    if (tcp->flags & (TCP_FLAG_RST | TCP_FLAG_SYN))
    {
        tcps_rx_otw_syn_rst(tcpips, io, tcb_handle);
        return;
    }
    //fifth check the ACK field
    if (tcp->flags & TCP_FLAG_ACK)
    {
        if (!tcps_rx_otw_ack(tcpips, io, tcb_handle))
            return;
    }
    else
        return;

    //sixth, check the URG bit
    //seventh, process the segment text
    tcps_rx_otw_text(tcpips, io, tcb_handle);

    //eighth, check the FIN bit
    if (tcp->flags & TCP_FLAG_FIN)
        tcps_rx_otw_fin(tcpips, tcb_handle);

    //finally send ACK reply/data/fin/etc
    tcps_rx_send(tcpips, tcb_handle);
}

static inline void tcps_rx_process(TCPIPS* tcpips, IO* io, HANDLE tcb_handle)
{
    TCP_TCB* tcb = so_get(&tcpips->tcps.tcbs, tcb_handle);
    switch (tcb->state)
    {
    case TCP_STATE_CLOSED:
        tcps_rx_closed(tcpips, io, tcb_handle);
        break;
    case TCP_STATE_LISTEN:
        tcps_rx_listen(tcpips, io, tcb_handle);
        break;
    case TCP_STATE_SYN_SENT:
        printf("TODO: SYN-SENT\n");
        break;
    default:
        tcps_rx_otw(tcpips, io, tcb_handle);
        break;
    }
}

void tcps_init(TCPIPS* tcpips)
{
    so_create(&tcpips->tcps.listen, sizeof(TCP_LISTEN_HANDLE), 1);
    so_create(&tcpips->tcps.tcbs, sizeof(TCP_TCB), 1);
}

void tcps_rx(TCPIPS* tcpips, IO* io, IP* src)
{
    TCP_HEADER* tcp;
    TCP_TCB* tcb;
    HANDLE tcb_handle, process;
    uint16_t src_port, dst_port;
    if (io->data_size < sizeof(TCP_HEADER) || tcp_checksum(io_data(io), io->data_size, src, &tcpips->ip))
    {
        ips_release_io(tcpips, io);
        return;
    }
    tcp = io_data(io);
    src_port = be2short(tcp->src_port_be);
    dst_port = be2short(tcp->dst_port_be);
#if (TCP_DEBUG_FLOW)
    tcps_debug(io, src, &tcpips->ip);
#endif //TCP_DEBUG_FLOW

    if ((tcb_handle = tcps_find_tcb(tcpips, src, src_port, dst_port)) == INVALID_HANDLE)
    {
        if ((tcb_handle = tcps_create_tcb(tcpips, src, src_port, dst_port)) != INVALID_HANDLE)
        {
            //listening?
            if ((process = tcps_find_listener(tcpips, dst_port)) != INVALID_HANDLE)
            {
                tcb = so_get(&tcpips->tcps.tcbs, tcb_handle);
                tcb->state = TCP_STATE_LISTEN;
                tcb->active = false;
                tcb->process = process;
            }
        }
    }
    if (tcb_handle != INVALID_HANDLE)
    {
        tcb = so_get(&tcpips->tcps.tcbs, tcb_handle);
        tcps_apply_options(tcpips, io, tcb);
        tcb->tx_wnd = be2short(tcp->window_be);
        tcps_rx_process(tcpips, io, tcb_handle);
        //make sure not queued in rx
        if (tcb->rx_tmp == io)
            return;
    }
    ips_release_io(tcpips, io);
}

static inline void tcps_listen(TCPIPS* tcpips, IPC* ipc)
{
    HANDLE handle;
    TCP_LISTEN_HANDLE* tlh;
    if (tcps_find_listener(tcpips, (uint16_t)ipc->param1) != INVALID_HANDLE)
    {
        error(ERROR_ALREADY_CONFIGURED);
        return;
    }
    handle = so_allocate(&tcpips->tcps.listen);
    if (handle == INVALID_HANDLE)
        return;
    tlh = so_get(&tcpips->tcps.listen, handle);
    tlh->port = (uint16_t)ipc->param1;
    tlh->process = ipc->process;
    ipc->param2 = handle;
}

static inline void tcps_connect(TCPIPS* tcpips, IPC* ipc)
{
    //TODO:
/*    HANDLE handle;
    UDP_HANDLE* uh;
    IP dst;
    uint16_t local_port;
    dst.u32.ip = ipc->param2;
    local_port = udps_allocate_port(tcpips);
    if ((local_port = udps_allocate_port(tcpips)) == 0)
        return;
    if ((handle = so_allocate(&tcpips->udps.handles)) == INVALID_HANDLE)
        return;
    uh = so_get(&tcpips->udps.handles, handle);
    uh->remote_port = (uint16_t)ipc->param1;
    uh->local_port = local_port;
    uh->remote_addr.u32.ip = dst.u32.ip;
    uh->process = ipc->process;
    uh->head = NULL;
#if (ICMP)
    uh->err = ERROR_OK;
#endif //ICMP
    ipc->param2 = handle;*/
    error(ERROR_NOT_SUPPORTED);
}

static inline void tcps_read(TCPIPS* tcpips, HANDLE tcb_handle, IO* io)
{
    TCP_TCB* tcb;
    TCP_STACK* tcp_stack;
    TCP_HEADER* tcp;
    uint16_t urg;
    tcb = so_get(&tcpips->tcps.tcbs, tcb_handle);
    unsigned int size, data_size, data_offset;
    if (tcb == NULL)
        return;
    if (tcb->rx != NULL)
    {
        error(ERROR_ALREADY_CONFIGURED);
        return;
    }
    switch (tcb->state)
    {
    case TCP_STATE_ESTABLISHED:
    case TCP_STATE_FIN_WAIT_1:
    case TCP_STATE_FIN_WAIT_2:
        io->data_size = 0;
        tcp_stack = io_push(io, sizeof(TCP_STACK));
        tcp_stack->flags = 0;
        tcp_stack->urg_len = 0;
        size = io_get_free(io);
        error(ERROR_SYNC);
        //already on tmp buffers
        if (tcb->rx_tmp != NULL)
        {
            data_offset = tcps_data_offset(tcb->rx_tmp);
            data_size = tcps_data_len(tcb->rx_tmp);
            if (size > data_size)
                size = data_size;
            memcpy(io_data(io), (uint8_t*)io_data(tcb->rx_tmp) + data_offset, size);
            io->data_size = size;
            //apply flags
            tcp = io_data(tcb->rx_tmp);
            if (tcp->flags & TCP_FLAG_PSH)
                tcp_stack->flags |= TCP_PSH;
            if (tcp->flags & TCP_FLAG_URG)
            {
                urg = be2int(tcp->urgent_pointer_be);
                //make sure urg don't overlaps total data size
                if (urg > size)
                {
                    tcp_stack->urg_len = size;
                    short2be(tcp->urgent_pointer_be, urg - size);
                }
                else
                {
                    //all URG data processed
                    tcp_stack->urg_len = urg;
                    if (size < data_size)
                    {
                        short2be(tcp->urgent_pointer_be, 0);
                        tcp->flags &= ~TCP_FLAG_URG;
                    }
                }
                tcp_stack->flags |= TCP_URG;
            }
            //all copied?
            if (size == data_size)
            {
                ips_release_io(tcpips, tcb->rx_tmp);
                tcb->rx_tmp = NULL;
            }
            else
            {
                memmove((uint8_t*)io_data(tcb->rx_tmp) + data_offset, (uint8_t*)io_data(tcb->rx_tmp) + data_offset + size, data_size - size);
                tcb->rx_tmp->data_size -= size;
            }
            //can return to user?
            if ((io_get_free(io) == 0) || (tcp_stack->flags & TCP_PSH))
            {
                tcps_update_rx_wnd(tcb);
                io_complete(tcb->process, HAL_IO_CMD(HAL_TCP, IPC_READ), tcb_handle, io);
                return;
            }
        }
        tcb->rx = io;
        tcps_update_rx_wnd(tcb);
        break;
    default:
        error(ERROR_INVALID_STATE);
    }
}

void tcps_request(TCPIPS* tcpips, IPC* ipc)
{
    if (!tcpips->connected)
    {
        error(ERROR_NOT_ACTIVE);
        return;
    }
    switch (HAL_ITEM(ipc->cmd))
    {
    case IPC_OPEN:
        if (ipc->param2 == LOCALHOST)
            tcps_listen(tcpips, ipc);
        else
            tcps_connect(tcpips, ipc);
        break;
    case IPC_CLOSE:
        //TODO:
//        tcps_close(tcpips, ipc->param1);
        break;
    case IPC_READ:
        tcps_read(tcpips, ipc->param1, (IO*)ipc->param2);
        break;
    case IPC_WRITE:
        //TODO:
//        tcps_write(tcpips, ipc->param1, (IO*)ipc->param2);
        break;
    case IPC_FLUSH:
        //TODO:
//        tcps_flush(tcpips, ipc->param1);
        break;
    //TODO: accept/reject
    default:
        error(ERROR_NOT_SUPPORTED);
    }
}
