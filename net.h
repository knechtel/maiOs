#ifndef MAIOS_NET_H
#define MAIOS_NET_H

#include <stdint.h>

typedef struct {
    uint32_t source;
    uint32_t destination;
    uint16_t length;
    uint8_t protocol;
} ipv4_packet_t;

typedef struct {
    uint32_t source;
    uint16_t source_port;
    uint16_t destination_port;
    uint16_t length;
} udp_packet_t;

void net_init(const uint8_t mac[6]);
void net_poll(void);
int net_ping(uint32_t destination);
int net_dhcp_configure(void);
int net_dns_resolve(const char *name, uint32_t *address);
int net_ipv4_send(uint32_t destination, uint8_t protocol,
                  const uint8_t *payload, uint16_t length);
int net_udp_send(uint32_t source, uint32_t destination,
                 uint16_t source_port, uint16_t destination_port,
                 const uint8_t *payload, uint16_t length);
int net_ipv4_receive(ipv4_packet_t *packet, uint8_t *payload, uint16_t capacity);
int net_udp_receive(udp_packet_t *packet, uint8_t *payload, uint16_t capacity);
void net_get_config(uint32_t *address, uint32_t *mask, uint32_t *gateway);
uint32_t net_get_dns(void);
int net_tcp_connect(uint32_t destination, uint16_t port);
int net_tcp_send(const uint8_t *data, uint16_t length);
int net_tcp_receive(uint8_t *data, uint16_t capacity, uint16_t *length);
int net_tcp_close(void);
typedef int (*net_https_sink_fn)(const uint8_t *data, uint16_t length, void *context);
int net_https_request(const char *host, const char *path, const char *method,
                      const char *headers, const uint8_t *body, uint32_t body_length,
                      net_https_sink_fn sink, void *context,
                      uint32_t *response_length, uint16_t *status_code);
int net_https_get(const char *host, const char *path, uint8_t *response,
                  uint16_t capacity, uint16_t *response_length);

#endif
