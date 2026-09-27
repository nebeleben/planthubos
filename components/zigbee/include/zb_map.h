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

/* Capability id for a (cluster, attr) pair, or ZB_MAP_NONE. Attr-aware
 * because Electrical Measurement (0x0B04) maps ActivePower/RMSVoltage/
 * RMSCurrent to three DIFFERENT caps; every other cluster ignores attr and
 * returns zb_map_cluster_to_cap(cluster). The report/read-response path uses
 * this instead of cluster_to_cap so a 0x0B04 attribute lands in the right cap. */
uint8_t zb_map_attr_to_cap(uint16_t cluster, uint16_t attr);

/* Fills out[] with the action ids a cluster provides, returns how many
 * (never more than max). Sensor clusters return 0. */
int zb_map_cluster_to_actions(uint16_t cluster, uint8_t *out, int max);

/* The On/Off actions (switch.on/switch.off) to backfill onto a device that
 * reported the On/Off cluster, given its current capability list -- returns
 * 0 (backfill nothing) for an INPUT device that already carries
 * CAP_BUTTON_ACTION or CAP_DIM_ROTATE, since a button/knob only
 * non-compliantly advertises On/Off and its switch caps/actions are stripped
 * on purpose (zb_interview_finalize / knob_reclassify). `caps` may be NULL
 * with cap_count 0 (a device that interviewed with no clusters). Pure, so
 * tests/host/test_zb_map.c exercises the guard directly. */
int zb_map_onoff_backfill_actions(const uint8_t *caps, uint8_t cap_count, uint8_t *out, int max);

/* The attribute id to configure reporting on, or ZB_MAP_NO_ATTR. */
uint16_t zb_map_report_attr(uint16_t cluster);

/* Converts a raw ZCL attribute value into the capability's own unit (the
 * float data_core_submit_cap_id() expects -- capability.c does the storage
 * scaling from there). Returns false when the cluster is unmapped or the
 * raw value is one of ZCL's not-a-reading sentinels, in which case *out is
 * untouched: a sentinel must never be stored as a measurement. */
bool zb_map_zcl_to_value(uint16_t cluster, int32_t raw, float *out);

/* Does (cluster, attr) carry a reading this mapper can convert? True for the
 * cluster's own reportable attribute, and additionally -- for Power
 * Configuration (0x0001) -- for BOTH BatteryPercentageRemaining (0x0021) and
 * BatteryVoltage (0x0020), since many devices (e.g. Xiaomi coin-cell sensors)
 * only report voltage. The report handler drops any pair this rejects. */
bool zb_map_accepts_attr(uint16_t cluster, uint16_t attr);

/* Attribute-aware value conversion. Like zb_map_zcl_to_value(), but also
 * knows how to read Power Configuration's BatteryVoltage (0x0020, 100 mV
 * units) and turn it into a battery percentage with a coin-cell curve; every
 * other (cluster, attr) delegates to zb_map_zcl_to_value() for its one mapped
 * attribute. Returns false (and leaves *out untouched) for an unaccepted pair
 * or a ZCL not-a-reading sentinel. */
bool zb_map_zcl_attr_to_value(uint16_t cluster, uint16_t attr, int32_t raw, float *out);
