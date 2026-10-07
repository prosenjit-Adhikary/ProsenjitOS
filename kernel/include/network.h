#ifndef PROSENJIT_NETWORK_H
#define PROSENJIT_NETWORK_H

#include <stdint.h>
#include "driver_class.h"

#define PROS_NET_OK        0
#define PROS_NET_EINVAL   -1
#define PROS_NET_EBOUNDS  -2
#define PROS_NET_ECHECKSUM -3
#define PROS_NET_ESTATE   -4
#define PROS_NET_ENOSPC   -5
#define PROS_NET_ENOENT   -6
#define PROS_NET_EAGAIN   -7
#define PROS_NET_ENOTSUP  -8

#define PROS_NET_ETH_MIN_FRAME 14u
#define PROS_NET_ETH_MAX_FRAME 1518u
#define PROS_NET_ARP_PAYLOAD 28u
#define PROS_NET_IPV4_MIN_HEADER 20u
#define PROS_NET_IPV4_MAX_HEADER 60u
#define PROS_NET_TCP_MIN_HEADER 20u
#define PROS_NET_SOCKET_MAX 32u
#define PROS_NET_QUEUE_MAX 8u
#define PROS_NET_PAYLOAD_MAX 1460u
#define PROS_NET_ARP_CACHE_MAX 16u
#define PROS_NET_ROUTE_MAX 16u

enum pros_net_ethertype {
    PROS_NET_ETH_IPV4 = 0x0800,
    PROS_NET_ETH_ARP  = 0x0806
};

enum pros_net_ip_protocol {
    PROS_NET_IP_TCP = 6,
    PROS_NET_IP_ICMP = 1
};

enum pros_net_tcp_state {
    PROS_NET_TCP_CLOSED = 0,
    PROS_NET_TCP_LISTEN,
    PROS_NET_TCP_SYN_SENT,
    PROS_NET_TCP_SYN_RECEIVED,
    PROS_NET_TCP_ESTABLISHED,
    PROS_NET_TCP_FIN_WAIT,
    PROS_NET_TCP_CLOSE_WAIT,
    PROS_NET_TCP_LAST_ACK
};

struct pros_net_eth_header {
    uint8_t dst[6];
    uint8_t src[6];
    uint16_t ethertype;
};

struct pros_net_arp {
    uint16_t htype;
    uint16_t ptype;
    uint8_t hlen;
    uint8_t plen;
    uint16_t oper;
    uint8_t sha[6];
    uint32_t spa;
    uint8_t tha[6];
    uint32_t tpa;
};

struct pros_net_ipv4 {
    uint8_t version;
    uint8_t ihl;
    uint8_t tos;
    uint16_t total_length;
    uint16_t identification;
    uint16_t flags_fragment;
    uint8_t ttl;
    uint8_t protocol;
    uint16_t checksum;
    uint32_t src;
    uint32_t dst;
};

struct pros_net_icmp {
    uint8_t type;
    uint8_t code;
    uint16_t checksum;
    uint32_t rest;
};

struct pros_net_tcp {
    uint16_t src_port;
    uint16_t dst_port;
    uint32_t seq;
    uint32_t ack;
    uint8_t data_offset;
    uint8_t flags;
    uint16_t window;
    uint16_t checksum;
    uint16_t urgent;
};

struct pros_net_socket {
    uint32_t id;
    uint8_t used;
    uint8_t listening;
    uint8_t connected;
    enum pros_net_tcp_state state;
    uint32_t local_addr;
    uint16_t local_port;
    uint32_t remote_addr;
    uint16_t remote_port;
    uint32_t snd_nxt;
    uint32_t rcv_nxt;
    uint8_t rx[PROS_NET_PAYLOAD_MAX];
    uint16_t rx_len;
};

uint16_t pros_net_checksum(const uint8_t *data, uint32_t length);
int pros_net_parse_ethernet(const uint8_t *frame, uint32_t length,
                            struct pros_net_eth_header *out,
                            uint32_t *payload_offset);
int pros_net_parse_arp(const uint8_t *data, uint32_t length,
                       struct pros_net_arp *out);
int pros_net_parse_ipv4(const uint8_t *data, uint32_t length,
                        struct pros_net_ipv4 *out, uint32_t *payload_offset);
int pros_net_parse_icmp(const uint8_t *data, uint32_t length,
                        struct pros_net_icmp *out);
int pros_net_arp_update(uint32_t ip, const uint8_t mac[6], uint64_t now,
                        uint64_t ttl);
int pros_net_arp_lookup(uint32_t ip, uint8_t mac[6], uint64_t now);
int pros_net_route_add(uint32_t network, uint32_t mask, uint32_t gateway,
                       uint32_t metric);
int pros_net_route_lookup(uint32_t destination, uint32_t *gateway_out,
                          uint32_t *mask_out);
uint16_t pros_net_tcp_checksum(uint32_t src, uint32_t dst,
                               const uint8_t *tcp, uint32_t length);
int pros_net_validate_tcp_checksum(uint32_t src, uint32_t dst,
                                   const uint8_t *tcp, uint32_t length);
int pros_net_parse_tcp(const uint8_t *data, uint32_t length,
                       struct pros_net_tcp *out, uint32_t *payload_offset);
int pros_net_tcp_transition(enum pros_net_tcp_state state, uint8_t flags,
                            enum pros_net_tcp_state *next);
int pros_net_socket_init(void);
int pros_net_socket_create(uint32_t *socket_id);
int pros_net_socket_bind(uint32_t socket_id, uint32_t addr, uint16_t port);
int pros_net_socket_listen(uint32_t socket_id);
int pros_net_socket_connect(uint32_t socket_id, uint32_t addr, uint16_t port,
                            uint32_t initial_seq);
int pros_net_socket_accept(uint32_t listener_id, uint32_t seq,
                           uint32_t *accepted_id);
int pros_net_socket_ack(uint32_t socket_id, uint32_t ack);
int pros_net_socket_receive_segment(uint32_t socket_id, uint32_t seq,
                                    const uint8_t *data, uint32_t length,
                                    uint8_t flags);
int pros_net_socket_send(uint32_t socket_id, const uint8_t *data, uint32_t length);
int pros_net_socket_receive(uint32_t socket_id, uint8_t *data, uint32_t capacity,
                            uint32_t *length_out);
int pros_net_socket_close(uint32_t socket_id);
int pros_net_selftest(void);

#endif
