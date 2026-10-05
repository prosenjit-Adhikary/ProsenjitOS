#ifndef PROSENJIT_NETPROTO_H
#define PROSENJIT_NETPROTO_H

#include <stdint.h>

#define PROS_NETPROTO_OK       0
#define PROS_NETPROTO_EINVAL  -1
#define PROS_NETPROTO_ENOSPC  -2
#define PROS_NETPROTO_ECORRUPT -3
#define PROS_NETPROTO_ENOENT  -4

#define PROS_ETH_ADDR_LEN 6u
#define PROS_ETH_HEADER_LEN 14u
#define PROS_ETH_MIN_FRAME_LEN 60u
#define PROS_ETH_MAX_FRAME_LEN 1514u

#define PROS_ETHERTYPE_IPV4 0x0800u
#define PROS_ETHERTYPE_ARP  0x0806u

#define PROS_ARP_HTYPE_ETHERNET 1u
#define PROS_ARP_PTYPE_IPV4 0x0800u
#define PROS_ARP_HLEN_ETHERNET 6u
#define PROS_ARP_PLEN_IPV4 4u
#define PROS_ARP_REQUEST 1u
#define PROS_ARP_REPLY 2u
#define PROS_ARP_PACKET_LEN 28u

struct pros_mac {
    uint8_t octet[PROS_ETH_ADDR_LEN];
};

struct pros_ipv4_addr {
    uint8_t octet[4];
};

struct pros_eth_header {
    struct pros_mac dst;
    struct pros_mac src;
    uint16_t ethertype;
};

struct pros_arp_packet {
    uint16_t htype;
    uint16_t ptype;
    uint8_t hlen;
    uint8_t plen;
    uint16_t oper;
    struct pros_mac sha;
    struct pros_ipv4_addr spa;
    struct pros_mac tha;
    struct pros_ipv4_addr tpa;
};

int pros_eth_build_header(uint8_t *frame, uint32_t frame_len,
                          const struct pros_mac *dst,
                          const struct pros_mac *src, uint16_t ethertype);
int pros_eth_parse_header(const uint8_t *frame, uint32_t frame_len,
                          struct pros_eth_header *header);

struct pros_ipv4_header {
    uint8_t version_ihl;
    uint8_t tos;
    uint16_t total_length;
    uint16_t identification;
    uint16_t flags_fragment;
    uint8_t ttl;
    uint8_t protocol;
    uint16_t checksum;
    struct pros_ipv4_addr src;
    struct pros_ipv4_addr dst;
};

uint16_t pros_ipv4_checksum(const uint8_t *data, uint32_t length);
int pros_ipv4_build_header(uint8_t *packet, uint32_t packet_len,
                           uint8_t tos, uint16_t total_length,
                           uint16_t identification, uint16_t flags_fragment,
                           uint8_t ttl, uint8_t protocol,
                           const struct pros_ipv4_addr *src,
                           const struct pros_ipv4_addr *dst);
int pros_ipv4_parse_header(const uint8_t *packet, uint32_t packet_len,
                           struct pros_ipv4_header *header);

int pros_arp_build(uint8_t *packet, uint32_t packet_len, uint16_t operation,
                   const struct pros_mac *sha, const struct pros_ipv4_addr *spa,
                   const struct pros_mac *tha, const struct pros_ipv4_addr *tpa);
int pros_arp_parse(const uint8_t *packet, uint32_t packet_len,
                   struct pros_arp_packet *arp);

int pros_ipv4_selftest(void);

int pros_arp_cache_init(void);
int pros_arp_cache_insert(const struct pros_ipv4_addr *ip,
                          const struct pros_mac *mac);
int pros_arp_cache_lookup(const struct pros_ipv4_addr *ip,
                          struct pros_mac *mac);
int pros_arp_cache_remove(const struct pros_ipv4_addr *ip);
int pros_arp_selftest(void);

#endif
