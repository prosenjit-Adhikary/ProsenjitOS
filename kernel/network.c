#include "network.h"

#define ETH_HEADER_LEN 14u
#define ARP_HTYPE_ETHERNET 1u
#define ARP_PTYPE_IPV4 0x0800u
#define ARP_OPER_REQUEST 1u
#define ARP_OPER_REPLY 2u
#define TCP_FIN 0x01u
#define TCP_SYN 0x02u
#define TCP_RST 0x04u
#define TCP_ACK 0x10u

static struct pros_net_socket sockets[PROS_NET_SOCKET_MAX];
static uint32_t next_socket_id;
static uint16_t next_ephemeral_port;

static uint16_t be16(const uint8_t *p)
{
    return (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
}

static uint32_t be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | p[3];
}

static void put_be16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)v;
}

static void put_be32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

uint16_t pros_net_checksum(const uint8_t *data, uint32_t length)
{
    uint32_t sum = 0;
    if (!data) return 0;
    while (length >= 2u) {
        sum += be16(data);
        data += 2;
        length -= 2u;
        if (sum > 0xFFFFu) sum = (sum & 0xFFFFu) + (sum >> 16);
    }
    if (length) sum += (uint32_t)data[0] << 8;
    while (sum >> 16) sum = (sum & 0xFFFFu) + (sum >> 16);
    return (uint16_t)~sum;
}

int pros_net_parse_ethernet(const uint8_t *frame, uint32_t length,
                            struct pros_net_eth_header *out,
                            uint32_t *payload_offset)
{
    if (!frame || !out || length < ETH_HEADER_LEN ||
        length > PROS_NET_ETH_MAX_FRAME)
        return PROS_NET_EBOUNDS;
    for (uint32_t i = 0; i < 6u; ++i) {
        out->dst[i] = frame[i];
        out->src[i] = frame[6u + i];
    }
    out->ethertype = be16(frame + 12u);
    if (out->ethertype != PROS_NET_ETH_IPV4 &&
        out->ethertype != PROS_NET_ETH_ARP)
        return PROS_NET_ENOTSUP;
    if (payload_offset) *payload_offset = ETH_HEADER_LEN;
    return PROS_NET_OK;
}

int pros_net_parse_arp(const uint8_t *data, uint32_t length,
                       struct pros_net_arp *out)
{
    if (!data || !out || length < PROS_NET_ARP_PAYLOAD)
        return PROS_NET_EBOUNDS;
    out->htype = be16(data);
    out->ptype = be16(data + 2u);
    out->hlen = data[4];
    out->plen = data[5];
    out->oper = be16(data + 6u);
    if (out->htype != ARP_HTYPE_ETHERNET || out->ptype != ARP_PTYPE_IPV4 ||
        out->hlen != 6u || out->plen != 4u ||
        (out->oper != ARP_OPER_REQUEST && out->oper != ARP_OPER_REPLY))
        return PROS_NET_EINVAL;
    for (uint32_t i = 0; i < 6u; ++i) {
        out->sha[i] = data[8u + i];
        out->tha[i] = data[18u + i];
    }
    out->spa = be32(data + 14u);
    out->tpa = be32(data + 24u);
    return PROS_NET_OK;
}

int pros_net_parse_ipv4(const uint8_t *data, uint32_t length,
                        struct pros_net_ipv4 *out, uint32_t *payload_offset)
{
    uint32_t header_len, total;
    if (!data || !out || length < PROS_NET_IPV4_MIN_HEADER)
        return PROS_NET_EBOUNDS;
    if ((data[0] >> 4) != 4u)
        return PROS_NET_EINVAL;
    out->ihl = (uint8_t)(data[0] & 0x0Fu);
    if (out->ihl < 5u || out->ihl > 15u)
        return PROS_NET_EINVAL;
    header_len = (uint32_t)out->ihl * 4u;
    if (header_len > length || header_len > PROS_NET_IPV4_MAX_HEADER)
        return PROS_NET_EBOUNDS;
    total = be16(data + 2u);
    if (total < header_len || total > length)
        return PROS_NET_EBOUNDS;
    if (pros_net_checksum(data, header_len) != 0u)
        return PROS_NET_ECHECKSUM;
    out->version = 4u;
    out->tos = data[1];
    out->total_length = (uint16_t)total;
    out->identification = be16(data + 4u);
    out->flags_fragment = be16(data + 6u);
    out->ttl = data[8];
    if (out->ttl == 0u)
        return PROS_NET_EINVAL;
    out->protocol = data[9];
    out->checksum = be16(data + 10u);
    out->src = be32(data + 12u);
    out->dst = be32(data + 16u);
    if (payload_offset) *payload_offset = header_len;
    return PROS_NET_OK;
}

int pros_net_parse_tcp(const uint8_t *data, uint32_t length,
                       struct pros_net_tcp *out, uint32_t *payload_offset)
{
    uint32_t header_len;
    if (!data || !out || length < PROS_NET_TCP_MIN_HEADER)
        return PROS_NET_EBOUNDS;
    out->data_offset = (uint8_t)(data[12] >> 4);
    if (out->data_offset < 5u || out->data_offset > 15u)
        return PROS_NET_EINVAL;
    header_len = (uint32_t)out->data_offset * 4u;
    if (header_len > length)
        return PROS_NET_EBOUNDS;
    out->src_port = be16(data);
    out->dst_port = be16(data + 2u);
    out->seq = be32(data + 4u);
    out->ack = be32(data + 8u);
    out->flags = data[13];
    out->window = be16(data + 14u);
    out->checksum = be16(data + 16u);
    out->urgent = be16(data + 18u);
    if (payload_offset) *payload_offset = header_len;
    return PROS_NET_OK;
}

int pros_net_tcp_transition(enum pros_net_tcp_state state, uint8_t flags,
                            enum pros_net_tcp_state *next)
{
    if (!next || (flags & ~(TCP_FIN | TCP_SYN | TCP_RST | TCP_ACK)) != 0u)
        return PROS_NET_EINVAL;
    if (flags & TCP_RST) {
        *next = PROS_NET_TCP_CLOSED;
        return PROS_NET_OK;
    }
    switch (state) {
    case PROS_NET_TCP_CLOSED:
        if (flags == TCP_SYN) { *next = PROS_NET_TCP_SYN_RECEIVED; return PROS_NET_OK; }
        break;
    case PROS_NET_TCP_LISTEN:
        if (flags == TCP_SYN) { *next = PROS_NET_TCP_SYN_RECEIVED; return PROS_NET_OK; }
        break;
    case PROS_NET_TCP_SYN_SENT:
        if ((flags & (TCP_SYN | TCP_ACK)) == (TCP_SYN | TCP_ACK)) {
            *next = PROS_NET_TCP_ESTABLISHED; return PROS_NET_OK;
        }
        break;
    case PROS_NET_TCP_SYN_RECEIVED:
        if (flags & TCP_ACK) { *next = PROS_NET_TCP_ESTABLISHED; return PROS_NET_OK; }
        break;
    case PROS_NET_TCP_ESTABLISHED:
        if (flags & TCP_FIN) { *next = PROS_NET_TCP_CLOSE_WAIT; return PROS_NET_OK; }
        if (flags & TCP_ACK) { *next = PROS_NET_TCP_ESTABLISHED; return PROS_NET_OK; }
        break;
    case PROS_NET_TCP_FIN_WAIT:
        if (flags & TCP_ACK) { *next = PROS_NET_TCP_CLOSED; return PROS_NET_OK; }
        break;
    case PROS_NET_TCP_CLOSE_WAIT:
        if (flags & TCP_ACK) { *next = PROS_NET_TCP_LAST_ACK; return PROS_NET_OK; }
        break;
    case PROS_NET_TCP_LAST_ACK:
        if (flags & TCP_ACK) { *next = PROS_NET_TCP_CLOSED; return PROS_NET_OK; }
        break;
    default:
        break;
    }
    return PROS_NET_ESTATE;
}

static struct pros_net_socket *socket_find(uint32_t id)
{
    for (uint32_t i = 0; i < PROS_NET_SOCKET_MAX; ++i)
        if (sockets[i].used && sockets[i].id == id)
            return &sockets[i];
    return 0;
}

static int port_conflict(uint32_t addr, uint16_t port, const struct pros_net_socket *ignore)
{
    if (port == 0u) return 1;
    for (uint32_t i = 0; i < PROS_NET_SOCKET_MAX; ++i) {
        const struct pros_net_socket *s = &sockets[i];
        if (!s->used || s == ignore || s->local_port != port) continue;
        if (s->local_addr == 0u || addr == 0u || s->local_addr == addr)
            return 1;
    }
    return 0;
}

static uint16_t allocate_ephemeral(uint32_t addr, const struct pros_net_socket *ignore)
{
    for (uint32_t tries = 0; tries < 16384u; ++tries) {
        uint16_t p = next_ephemeral_port++;
        if (next_ephemeral_port < 49152u) next_ephemeral_port = 49152u;
        if (p < 49152u) p = 49152u;
        if (!port_conflict(addr, p, ignore)) return p;
    }
    return 0;
}

int pros_net_socket_init(void)
{
    for (uint32_t i = 0; i < PROS_NET_SOCKET_MAX; ++i)
        sockets[i] = (struct pros_net_socket){0};
    next_socket_id = 1u;
    next_ephemeral_port = 49152u;
    return PROS_NET_OK;
}

int pros_net_socket_create(uint32_t *socket_id)
{
    if (!socket_id) return PROS_NET_EINVAL;
    for (uint32_t i = 0; i < PROS_NET_SOCKET_MAX; ++i) {
        if (!sockets[i].used) {
            sockets[i] = (struct pros_net_socket){0};
            sockets[i].used = 1u;
            sockets[i].id = next_socket_id++;
            if (sockets[i].id == 0u) sockets[i].id = next_socket_id++;
            sockets[i].state = PROS_NET_TCP_CLOSED;
            *socket_id = sockets[i].id;
            return PROS_NET_OK;
        }
    }
    return PROS_NET_ENOSPC;
}

int pros_net_socket_bind(uint32_t socket_id, uint32_t addr, uint16_t port)
{
    struct pros_net_socket *s = socket_find(socket_id);
    if (!s || s->state != PROS_NET_TCP_CLOSED || port_conflict(addr, port, s))
        return PROS_NET_ESTATE;
    if (port == 0u) {
        port = allocate_ephemeral(addr, s);
        if (port == 0u) return PROS_NET_ENOSPC;
    }
    s->local_addr = addr;
    s->local_port = port;
    return PROS_NET_OK;
}

int pros_net_socket_listen(uint32_t socket_id)
{
    struct pros_net_socket *s = socket_find(socket_id);
    if (!s || s->local_port == 0u || s->state != PROS_NET_TCP_CLOSED)
        return PROS_NET_ESTATE;
    s->listening = 1u;
    s->state = PROS_NET_TCP_LISTEN;
    return PROS_NET_OK;
}

int pros_net_socket_connect(uint32_t socket_id, uint32_t addr, uint16_t port,
                            uint32_t initial_seq)
{
    struct pros_net_socket *s = socket_find(socket_id);
    if (!s || addr == 0u || port == 0u ||
        (s->state != PROS_NET_TCP_CLOSED) || s->listening)
        return PROS_NET_ESTATE;
    if (s->local_port == 0u) {
        s->local_port = allocate_ephemeral(s->local_addr, s);
        if (s->local_port == 0u) return PROS_NET_ENOSPC;
    }
    s->remote_addr = addr;
    s->remote_port = port;
    s->snd_nxt = initial_seq + 1u;
    s->state = PROS_NET_TCP_SYN_SENT;
    return PROS_NET_OK;
}

int pros_net_socket_accept_syn(uint32_t socket_id, uint32_t seq, uint32_t ack)
{
    struct pros_net_socket *s = socket_find(socket_id);
    if (!s || !s->listening || s->state != PROS_NET_TCP_LISTEN ||
        ack != s->snd_nxt)
        return PROS_NET_ESTATE;
    s->rcv_nxt = seq + 1u;
    s->state = PROS_NET_TCP_SYN_RECEIVED;
    return PROS_NET_OK;
}

int pros_net_socket_ack(uint32_t socket_id, uint32_t ack)
{
    struct pros_net_socket *s = socket_find(socket_id);
    if (!s) return PROS_NET_ENOENT;
    if (s->state == PROS_NET_TCP_SYN_SENT) {
        if (ack != s->snd_nxt) return PROS_NET_ESTATE;
        s->state = PROS_NET_TCP_ESTABLISHED;
        return PROS_NET_OK;
    }
    if (s->state == PROS_NET_TCP_SYN_RECEIVED) {
        if (ack != s->snd_nxt) return PROS_NET_ESTATE;
        s->state = PROS_NET_TCP_ESTABLISHED;
        return PROS_NET_OK;
    }
    if (s->state == PROS_NET_TCP_ESTABLISHED) {
        if ((int32_t)(ack - s->snd_nxt) > 0) return PROS_NET_ESTATE;
        return PROS_NET_OK;
    }
    return PROS_NET_ESTATE;
}

int pros_net_socket_send(uint32_t socket_id, const uint8_t *data, uint32_t length)
{
    struct pros_net_socket *s = socket_find(socket_id);
    if (!s || s->state != PROS_NET_TCP_ESTABLISHED)
        return PROS_NET_ESTATE;
    if (!data && length) return PROS_NET_EINVAL;
    if (length > PROS_NET_PAYLOAD_MAX) return PROS_NET_EBOUNDS;
    /* Transport state is advanced only after the payload has been accepted. */
    if (length) s->snd_nxt += length;
    return (int)length;
}

int pros_net_socket_receive(uint32_t socket_id, uint8_t *data, uint32_t capacity,
                            uint32_t *length_out)
{
    struct pros_net_socket *s = socket_find(socket_id);
    if (!s || !length_out || (!data && capacity))
        return PROS_NET_EINVAL;
    if (s->state != PROS_NET_TCP_ESTABLISHED &&
        s->state != PROS_NET_TCP_CLOSE_WAIT)
        return PROS_NET_ESTATE;
    if (s->rx_len == 0u) return PROS_NET_EAGAIN;
    if (capacity < s->rx_len) return PROS_NET_EBOUNDS;
    for (uint32_t i = 0; i < s->rx_len; ++i) data[i] = s->rx[i];
    *length_out = s->rx_len;
    s->rx_len = 0u;
    return PROS_NET_OK;
}

int pros_net_socket_close(uint32_t socket_id)
{
    struct pros_net_socket *s = socket_find(socket_id);
    if (!s) return PROS_NET_ENOENT;
    if (s->state == PROS_NET_TCP_ESTABLISHED) {
        s->state = PROS_NET_TCP_FIN_WAIT;
        return PROS_NET_OK;
    }
    if (s->state == PROS_NET_TCP_FIN_WAIT ||
        s->state == PROS_NET_TCP_CLOSE_WAIT ||
        s->state == PROS_NET_TCP_LAST_ACK ||
        s->state == PROS_NET_TCP_SYN_SENT ||
        s->state == PROS_NET_TCP_SYN_RECEIVED ||
        s->state == PROS_NET_TCP_LISTEN) {
        s->state = PROS_NET_TCP_CLOSED;
    }
    s->used = 0u;
    return PROS_NET_OK;
}

int pros_net_selftest(void)
{
    uint8_t eth[14] = {0};
    uint8_t arp[28] = {0};
    uint8_t ip[20] = {0};
    uint8_t tcp[20] = {0};
    struct pros_net_eth_header eh;
    struct pros_net_arp ah;
    struct pros_net_ipv4 ih;
    struct pros_net_tcp th;
    enum pros_net_tcp_state state;
    uint32_t off, sid;
    uint8_t payload[1] = {0xA5};
    uint32_t received = 0;

    eth[12] = 0x08; eth[13] = 0x00;
    if (pros_net_parse_ethernet(eth, sizeof(eth), &eh, &off) != PROS_NET_OK ||
        eh.ethertype != PROS_NET_ETH_IPV4 || off != 14u)
        return -1;
    eth[12] = 0x12; eth[13] = 0x34;
    if (pros_net_parse_ethernet(eth, sizeof(eth), &eh, &off) != PROS_NET_ENOTSUP)
        return -2;

    put_be16(arp, ARP_HTYPE_ETHERNET);
    put_be16(arp + 2u, ARP_PTYPE_IPV4);
    arp[4] = 6u; arp[5] = 4u; put_be16(arp + 6u, ARP_OPER_REQUEST);
    put_be32(arp + 14u, 0xC0A80101u); put_be32(arp + 24u, 0xC0A80102u);
    if (pros_net_parse_arp(arp, sizeof(arp), &ah) != PROS_NET_OK ||
        ah.spa != 0xC0A80101u || ah.tpa != 0xC0A80102u)
        return -3;
    arp[4] = 5u;
    if (pros_net_parse_arp(arp, sizeof(arp), &ah) != PROS_NET_EINVAL)
        return -4;

    ip[0] = 0x45; put_be16(ip + 2u, 20u); ip[8] = 64u; ip[9] = PROS_NET_IP_TCP;
    put_be32(ip + 12u, 0xC0A80101u); put_be32(ip + 16u, 0xC0A80102u);
    put_be16(ip + 10u, 0u); put_be16(ip + 10u, pros_net_checksum(ip, 20u));
    if (pros_net_parse_ipv4(ip, sizeof(ip), &ih, &off) != PROS_NET_OK ||
        ih.ttl != 64u || ih.protocol != PROS_NET_IP_TCP)
        return -5;
    ip[8] = 0u;
    put_be16(ip + 10u, 0u);
    put_be16(ip + 10u, pros_net_checksum(ip, 20u));
    if (pros_net_parse_ipv4(ip, sizeof(ip), &ih, &off) != PROS_NET_EINVAL)
        return -6;

    tcp[12] = 0x50; tcp[13] = TCP_SYN; put_be16(tcp, 1000u); put_be16(tcp + 2u, 80u);
    if (pros_net_parse_tcp(tcp, sizeof(tcp), &th, &off) != PROS_NET_OK ||
        th.flags != TCP_SYN)
        return -7;
    if (pros_net_tcp_transition(PROS_NET_TCP_SYN_SENT, TCP_SYN | TCP_ACK, &state) != PROS_NET_OK ||
        state != PROS_NET_TCP_ESTABLISHED ||
        pros_net_tcp_transition(PROS_NET_TCP_ESTABLISHED, TCP_RST, &state) != PROS_NET_OK ||
        state != PROS_NET_TCP_CLOSED)
        return -8;

    if (pros_net_socket_init() != PROS_NET_OK ||
        pros_net_socket_create(&sid) != PROS_NET_OK ||
        pros_net_socket_bind(sid, 0xC0A80101u, 4000u) != PROS_NET_OK ||
        pros_net_socket_connect(sid, 0xC0A80102u, 80u, 100u) != PROS_NET_OK ||
        pros_net_socket_ack(sid, 101u) != PROS_NET_OK ||
        pros_net_socket_send(sid, payload, sizeof(payload)) != 1 ||
        pros_net_socket_receive(sid, payload, sizeof(payload), &received) != PROS_NET_EAGAIN ||
        pros_net_socket_close(sid) != PROS_NET_OK)
        return -9;

    /* Invalid ACKs and oversized payloads must not advance transport state. */
    if (pros_net_socket_create(&sid) != PROS_NET_OK ||
        pros_net_socket_bind(sid, 0u, 0u) != PROS_NET_OK ||
        pros_net_socket_connect(sid, 0xC0A80102u, 80u, 10u) != PROS_NET_OK ||
        pros_net_socket_ack(sid, 999u) != PROS_NET_ESTATE ||
        pros_net_socket_send(sid, payload, PROS_NET_PAYLOAD_MAX + 1u) != PROS_NET_EBOUNDS)
        return -10;

    return PROS_NET_OK;
}
