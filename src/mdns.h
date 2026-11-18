/* SPDX-License-Identifier: MPL-2.0 */
/* A small mDNS responder (RFC 6762) for one name: A queries for <name>.local are answered with the address of the
 * interface the query came in on, so http://remotefm1.local:7730/ finds the Move. avahi-daemon also listens on 5353:
 * both bind with SO_REUSEADDR / SO_REUSEPORT and each answers for its own names. Optional (--no-mdns). */
#ifndef RFM14_MDNS_H
#define RFM14_MDNS_H
#include <stddef.h>
#include <stdint.h>

/* the answer to one mDNS message q (n bytes) for name ("remotefm1.local"), with the IPv4 address addr: its length
 * in out (0: not a question for us). *unicast: 1 when the asker wants a unicast reply (the QU bit, or a legacy
 * resolver: from_port != 5353, which also gets the query's id and question back) */
size_t mdns_answer(const uint8_t *q, size_t n, const char *name, const uint8_t addr[4], int from_port, uint8_t *out,
                   size_t cap, int *unicast);
/* an unsolicited announcement of name -> addr */
size_t mdns_announce(const char *name, const uint8_t addr[4], uint8_t *out, size_t cap);

int mdns_start(const char *name);   /* the responder's thread; 0, or -1 (no socket: said, and carried on without) */
void mdns_stop(void);

#endif
