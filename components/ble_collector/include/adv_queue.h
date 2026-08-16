#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#define ADV_PAYLOAD_MAX 31
#define ADV_QUEUE_LEN   16

typedef struct {
    uint8_t  mac[6];
    int8_t   rssi;
    uint8_t  addr_type;
    uint8_t  len;
    uint8_t  payload[ADV_PAYLOAD_MAX];
    uint32_t uptime_s;
} adv_item_t;                      /* 44 B; assert <= 48 */

/* Pure ring used by the queue and by the host test (no FreeRTOS). */
typedef struct { adv_item_t slots[ADV_QUEUE_LEN]; uint8_t head, tail; bool full; uint32_t dropped; } adv_ring_t;
void  adv_ring_init(adv_ring_t *r);
/* Returns false and increments r->dropped when the ring is full. */
bool  adv_ring_push(adv_ring_t *r, const adv_item_t *it);
bool  adv_ring_pop(adv_ring_t *r, adv_item_t *out);
size_t adv_ring_count(const adv_ring_t *r);
uint32_t adv_ring_dropped(const adv_ring_t *r);
