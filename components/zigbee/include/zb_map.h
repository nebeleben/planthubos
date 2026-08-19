/* zb_map.h -- ZCL cluster -> PlantHub capability/action, and raw ZCL ->
 * the capability's own unit (M6b spec section 6).
 *
 * Deliberately pure and table-driven: no ESP-IDF, no state, no allocation,
 * so tests/host/test_zb_map.c exercises every conversion directly. This is
 * also the file M6c extends -- a quirk is, in the end, a different answer
 * to the same three questions this header asks.
 *
 * This milestone adds NO new capability or action ids. Every value returned
 * here already exists in capability.h / action.h.
 */
#pragma once
#include <stdbool.h>
#include <stdint.h>

#define ZB_MAP_NONE      0xFF     /* no capability for this cluster */
#define ZB_MAP_NO_ATTR   0xFFFF   /* no reportable attribute */

/* Capability id for a cluster, or ZB_MAP_NONE. An unmapped cluster is not
 * an error: the device is kept and shown as unmapped, which is M6c's
 * starting evidence (spec section 5). */
uint8_t zb_map_cluster_to_cap(uint16_t cluster);

/* Fills out[] with the action ids a cluster provides, returns how many
 * (never more than max). Sensor clusters return 0. */
int zb_map_cluster_to_actions(uint16_t cluster, uint8_t *out, int max);

/* The attribute id to configure reporting on, or ZB_MAP_NO_ATTR. */
uint16_t zb_map_report_attr(uint16_t cluster);

/* Converts a raw ZCL attribute value into the capability's own unit (the
 * float data_core_submit_cap_id() expects -- capability.c does the storage
 * scaling from there). Returns false when the cluster is unmapped or the
 * raw value is one of ZCL's not-a-reading sentinels, in which case *out is
 * untouched: a sentinel must never be stored as a measurement. */
bool zb_map_zcl_to_value(uint16_t cluster, int32_t raw, float *out);
