#ifndef MINTSCAN_MDNS_ENDPOINT_H
#define MINTSCAN_MDNS_ENDPOINT_H

#include <stddef.h>

#define MS_MDNS_NAME_MAX 256
#define MS_MDNS_PATH_MAX 128
#define MS_MDNS_LABEL_MAX 96

struct MSMdnsEndpoint {
    char instance[MS_MDNS_NAME_MAX];
    char path[MS_MDNS_PATH_MAX];
    char label[MS_MDNS_LABEL_MAX];
    int port;
    int is_escl;
};

/* Parse PTR/SRV/TXT records from one mDNS response.  The endpoint structure
 * is deliberately cumulative: callers may feed the initial PTR response and
 * later SRV/TXT detail responses into the same object. */
int ms_mdns_parse_endpoint(const unsigned char *packet, size_t packet_len,
                           struct MSMdnsEndpoint *endpoint);

#endif
