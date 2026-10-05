#include "netproto.h"

#define PROS_ARP_CACHE_MAX 16u

struct arp_cache_entry {
    struct pros_ipv4_addr ip;
    struct pros_mac mac;
    uint8_t valid;
};

static struct arp_cache_entry arp_cache[PROS_ARP_CACHE_MAX];
static uint32_t arp_cache_next;

static uint16_t pros_net_get_be16(const uint8_t *p)
{
    return (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
}

static void pros_net_put_be16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)v;
}

uint16_t pros_ipv4_checksum(const uint8_t *data, uint32_t length)
{
    uint32_t sum = 0u;
    if (!data)
        return 0u;
    while (length >= 2u) {
        sum += pros_net_get_be16(data);
        data += 2u;
        length -= 2u;
        if (sum > 0xFFFFu)
            sum = (sum & 0xFFFFu) + (sum >> 16);
    }
    if (length != 0u)
        sum += (uint32_t)data[0] << 8;
    while (sum >> 16)
        sum = (sum & 0xFFFFu) + (sum >> 16);
    return (uint16_t)~sum;
}

int pros_ipv4_build_header(uint8_t *packet, uint32_t packet_len,
                           uint8_t tos, uint16_t total_length,
                           uint16_t identification, uint16_t flags_fragment,
                           uint8_t ttl, uint8_t protocol,
                           const struct pros_ipv4_addr *src,
                           const struct pros_ipv4_addr *dst)
{
    if (!packet || !src || !dst || packet_len < 20u ||
        total_length < 20u || total_length > packet_len || ttl == 0u)
        return PROS_NETPROTO_EINVAL;
    packet[0] = 0x45u;
    packet[1] = tos;
    pros_net_put_be16(packet + 2u, total_length);
    pros_net_put_be16(packet + 4u, identification);
    pros_net_put_be16(packet + 6u, flags_fragment);
    packet[8] = ttl;
    packet[9] = protocol;
    packet[10] = 0u;
    packet[11] = 0u;
    for (uint32_t i = 0; i < 4u; ++i) {
        packet[12u + i] = src->octet[i];
        packet[16u + i] = dst->octet[i];
    }
    pros_net_put_be16(packet + 10u, pros_ipv4_checksum(packet, 20u));
    return PROS_NETPROTO_OK;
}

int pros_ipv4_parse_header(const uint8_t *packet, uint32_t packet_len,
                           struct pros_ipv4_header *header)
{
    if (!packet || !header || packet_len < 20u)
        return PROS_NETPROTO_EINVAL;
    uint8_t version = (uint8_t)(packet[0] >> 4);
    uint8_t ihl = (uint8_t)(packet[0] & 0x0Fu);
    uint16_t total_length = pros_net_get_be16(packet + 2u);
    if (version != 4u || ihl < 5u || (uint32_t)ihl * 4u > packet_len ||
        total_length < (uint16_t)ihl * 4u || total_length > packet_len ||
        pros_ipv4_checksum(packet, (uint32_t)ihl * 4u) != 0u)
        return PROS_NETPROTO_ECORRUPT;
    header->version_ihl = packet[0];
    header->tos = packet[1];
    header->total_length = total_length;
    header->identification = pros_net_get_be16(packet + 4u);
    header->flags_fragment = pros_net_get_be16(packet + 6u);
    header->ttl = packet[8];
    header->protocol = packet[9];
    header->checksum = pros_net_get_be16(packet + 10u);
    for (uint32_t i = 0; i < 4u; ++i) {
        header->src.octet[i] = packet[12u + i];
        header->dst.octet[i] = packet[16u + i];
    }
    return PROS_NETPROTO_OK;
}

int pros_ipv4_selftest(void)
{
    uint8_t packet[20];
    struct pros_ipv4_header header;
    struct pros_ipv4_addr src = {{192,168,1,10}};
    struct pros_ipv4_addr dst = {{192,168,1,1}};

    if (pros_ipv4_build_header(packet, sizeof(packet), 0u, 20u,
                               0x1234u, 0u, 64u, 17u, &src, &dst) !=
            PROS_NETPROTO_OK ||
        pros_ipv4_parse_header(packet, sizeof(packet), &header) !=
            PROS_NETPROTO_OK ||
        header.version_ihl != 0x45u ||
        header.total_length != 20u ||
        header.identification != 0x1234u ||
        header.ttl != 64u || header.protocol != 17u ||
        !ip_equal(&header.src, &src) || !ip_equal(&header.dst, &dst))
        return -1;

    packet[8] = 0u;
    if (pros_ipv4_parse_header(packet, sizeof(packet), &header) !=
            PROS_NETPROTO_ECORRUPT)
        return -2;
    return PROS_NETPROTO_OK;
}

static uint16_t get_be16(const uint8_t *p)
{
    return (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
}

static void put_be16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)v;
}

static int mac_equal(const struct pros_mac *a, const struct pros_mac *b)
{
    for (uint32_t i = 0; i < PROS_ETH_ADDR_LEN; ++i)
        if (a->octet[i] != b->octet[i])
            return 0;
    return 1;
}

static int ip_equal(const struct pros_ipv4_addr *a,
                    const struct pros_ipv4_addr *b)
{
    for (uint32_t i = 0; i < 4u; ++i)
        if (a->octet[i] != b->octet[i])
            return 0;
    return 1;
}

static void copy_mac(struct pros_mac *dst, const struct pros_mac *src)
{
    for (uint32_t i = 0; i < PROS_ETH_ADDR_LEN; ++i)
        dst->octet[i] = src->octet[i];
}

static void copy_ip(struct pros_ipv4_addr *dst,
                    const struct pros_ipv4_addr *src)
{
    for (uint32_t i = 0; i < 4u; ++i)
        dst->octet[i] = src->octet[i];
}

int pros_eth_build_header(uint8_t *frame, uint32_t frame_len,
                          const struct pros_mac *dst,
                          const struct pros_mac *src, uint16_t ethertype)
{
    if (!frame || !dst || !src || frame_len < PROS_ETH_HEADER_LEN)
        return PROS_NETPROTO_EINVAL;
    for (uint32_t i = 0; i < PROS_ETH_ADDR_LEN; ++i)
        frame[i] = dst->octet[i];
    for (uint32_t i = 0; i < PROS_ETH_ADDR_LEN; ++i)
        frame[6u + i] = src->octet[i];
    put_be16(frame + 12u, ethertype);
    return PROS_NETPROTO_OK;
}

int pros_eth_parse_header(const uint8_t *frame, uint32_t frame_len,
                          struct pros_eth_header *header)
{
    if (!frame || !header || frame_len < PROS_ETH_HEADER_LEN)
        return PROS_NETPROTO_EINVAL;
    for (uint32_t i = 0; i < PROS_ETH_ADDR_LEN; ++i) {
        header->dst.octet[i] = frame[i];
        header->src.octet[i] = frame[6u + i];
    }
    header->ethertype = get_be16(frame + 12u);
    return PROS_NETPROTO_OK;
}

int pros_arp_build(uint8_t *packet, uint32_t packet_len, uint16_t operation,
                   const struct pros_mac *sha, const struct pros_ipv4_addr *spa,
                   const struct pros_mac *tha, const struct pros_ipv4_addr *tpa)
{
    if (!packet || !sha || !spa || !tha || !tpa ||
        packet_len < PROS_ARP_PACKET_LEN ||
        (operation != PROS_ARP_REQUEST && operation != PROS_ARP_REPLY))
        return PROS_NETPROTO_EINVAL;

    put_be16(packet + 0u, PROS_ARP_HTYPE_ETHERNET);
    put_be16(packet + 2u, PROS_ARP_PTYPE_IPV4);
    packet[4] = PROS_ARP_HLEN_ETHERNET;
    packet[5] = PROS_ARP_PLEN_IPV4;
    put_be16(packet + 6u, operation);
    for (uint32_t i = 0; i < 6u; ++i) packet[8u + i] = sha->octet[i];
    for (uint32_t i = 0; i < 4u; ++i) packet[14u + i] = spa->octet[i];
    for (uint32_t i = 0; i < 6u; ++i) packet[18u + i] = tha->octet[i];
    for (uint32_t i = 0; i < 4u; ++i) packet[24u + i] = tpa->octet[i];
    return PROS_NETPROTO_OK;
}

int pros_arp_parse(const uint8_t *packet, uint32_t packet_len,
                   struct pros_arp_packet *arp)
{
    if (!packet || !arp || packet_len < PROS_ARP_PACKET_LEN)
        return PROS_NETPROTO_EINVAL;
    if (get_be16(packet + 0u) != PROS_ARP_HTYPE_ETHERNET ||
        get_be16(packet + 2u) != PROS_ARP_PTYPE_IPV4 ||
        packet[4] != PROS_ARP_HLEN_ETHERNET ||
        packet[5] != PROS_ARP_PLEN_IPV4)
        return PROS_NETPROTO_ECORRUPT;

    arp->htype = get_be16(packet + 0u);
    arp->ptype = get_be16(packet + 2u);
    arp->hlen = packet[4];
    arp->plen = packet[5];
    arp->oper = get_be16(packet + 6u);
    for (uint32_t i = 0; i < 6u; ++i) {
        arp->sha.octet[i] = packet[8u + i];
        arp->tha.octet[i] = packet[18u + i];
    }
    for (uint32_t i = 0; i < 4u; ++i) {
        arp->spa.octet[i] = packet[14u + i];
        arp->tpa.octet[i] = packet[24u + i];
    }
    if (arp->oper != PROS_ARP_REQUEST && arp->oper != PROS_ARP_REPLY)
        return PROS_NETPROTO_ECORRUPT;
    return PROS_NETPROTO_OK;
}

int pros_arp_cache_init(void)
{
    for (uint32_t i = 0; i < PROS_ARP_CACHE_MAX; ++i)
        arp_cache[i] = (struct arp_cache_entry){0};
    arp_cache_next = 0u;
    return PROS_NETPROTO_OK;
}

int pros_arp_cache_insert(const struct pros_ipv4_addr *ip,
                          const struct pros_mac *mac)
{
    if (!ip || !mac)
        return PROS_NETPROTO_EINVAL;

    for (uint32_t i = 0; i < PROS_ARP_CACHE_MAX; ++i) {
        if (arp_cache[i].valid && ip_equal(&arp_cache[i].ip, ip)) {
            copy_mac(&arp_cache[i].mac, mac);
            return PROS_NETPROTO_OK;
        }
    }
    for (uint32_t i = 0; i < PROS_ARP_CACHE_MAX; ++i) {
        if (!arp_cache[i].valid) {
            copy_ip(&arp_cache[i].ip, ip);
            copy_mac(&arp_cache[i].mac, mac);
            arp_cache[i].valid = 1u;
            return PROS_NETPROTO_OK;
        }
    }
    {
        uint32_t slot = arp_cache_next++ % PROS_ARP_CACHE_MAX;
        copy_ip(&arp_cache[slot].ip, ip);
        copy_mac(&arp_cache[slot].mac, mac);
        arp_cache[slot].valid = 1u;
    }
    return PROS_NETPROTO_OK;
}

int pros_arp_cache_lookup(const struct pros_ipv4_addr *ip,
                          struct pros_mac *mac)
{
    if (!ip || !mac)
        return PROS_NETPROTO_EINVAL;
    for (uint32_t i = 0; i < PROS_ARP_CACHE_MAX; ++i)
        if (arp_cache[i].valid && ip_equal(&arp_cache[i].ip, ip)) {
            copy_mac(mac, &arp_cache[i].mac);
            return PROS_NETPROTO_OK;
        }
    return PROS_NETPROTO_ENOENT;
}

int pros_arp_cache_remove(const struct pros_ipv4_addr *ip)
{
    if (!ip)
        return PROS_NETPROTO_EINVAL;
    for (uint32_t i = 0; i < PROS_ARP_CACHE_MAX; ++i)
        if (arp_cache[i].valid && ip_equal(&arp_cache[i].ip, ip)) {
            arp_cache[i] = (struct arp_cache_entry){0};
            return PROS_NETPROTO_OK;
        }
    return PROS_NETPROTO_ENOENT;
}

int pros_arp_selftest(void)
{
    uint8_t eth[PROS_ETH_HEADER_LEN];
    uint8_t arp_buf[PROS_ARP_PACKET_LEN];
    struct pros_eth_header eth_out;
    struct pros_arp_packet arp_out;
    struct pros_mac src = {{0x02,0x11,0x22,0x33,0x44,0x55}};
    struct pros_mac dst = {{0x02,0xAA,0xBB,0xCC,0xDD,0xEE}};
    struct pros_ipv4_addr spa = {{192,168,1,10}};
    struct pros_ipv4_addr tpa = {{192,168,1,1}};
    struct pros_mac lookup;

    if (pros_arp_cache_init() != PROS_NETPROTO_OK ||
        pros_eth_build_header(eth, sizeof(eth), &dst, &src,
                               PROS_ETHERTYPE_ARP) != PROS_NETPROTO_OK ||
        pros_eth_parse_header(eth, sizeof(eth), &eth_out) != PROS_NETPROTO_OK ||
        eth_out.ethertype != PROS_ETHERTYPE_ARP ||
        !mac_equal(&eth_out.src, &src) || !mac_equal(&eth_out.dst, &dst))
        return -1;

    if (pros_arp_build(arp_buf, sizeof(arp_buf), PROS_ARP_REQUEST,
                       &src, &spa, &dst, &tpa) != PROS_NETPROTO_OK ||
        pros_arp_parse(arp_buf, sizeof(arp_buf), &arp_out) != PROS_NETPROTO_OK ||
        arp_out.oper != PROS_ARP_REQUEST ||
        !ip_equal(&arp_out.spa, &spa) || !ip_equal(&arp_out.tpa, &tpa) ||
        !mac_equal(&arp_out.sha, &src))
        return -2;

    arp_buf[4] = 5u;
    if (pros_arp_parse(arp_buf, sizeof(arp_buf), &arp_out) !=
            PROS_NETPROTO_ECORRUPT)
        return -3;

    if (pros_arp_cache_insert(&tpa, &dst) != PROS_NETPROTO_OK ||
        pros_arp_cache_lookup(&tpa, &lookup) != PROS_NETPROTO_OK ||
        !mac_equal(&lookup, &dst) ||
        pros_arp_cache_remove(&tpa) != PROS_NETPROTO_OK ||
        pros_arp_cache_lookup(&tpa, &lookup) != PROS_NETPROTO_ENOENT)
        return -4;

    return PROS_NETPROTO_OK;
}
