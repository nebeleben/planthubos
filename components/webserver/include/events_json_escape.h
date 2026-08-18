/* events_json_escape.h -- the pure (no ESP-IDF) half of the events-poll
 * JSON streamer's logic: escaping one event's msg for embedding as a JSON
 * string body.
 *
 * Split out from sse.c specifically so this logic is host-testable
 * (test_events_json_escape.c, tests/host/run.sh), same pattern as
 * bthome/bindkey_core.h -- sse.c stays the ESP-IDF-only httpd shell around
 * this, which carries zero platform dependency of its own. */
#pragma once
#include <stddef.h>
#include "event_log.h"

/* Worst case every byte of an EVENT_MSG_MAX-long msg becomes \u00XX (6
 * chars); +1 for the NUL. events_json_get() in sse.c sizes its per-event
 * stack buffer off this. */
#define EVENTS_JSON_ESC_MAX (EVENT_MSG_MAX * 6 + 1)

/* Escapes src (an event's msg -- operator-supplied text via rules and
 * alert formatting, so it can contain anything) for embedding as the body
 * of a JSON string; the caller supplies the surrounding quotes. Matches
 * cJSON's own print_string_ptr() byte for byte (this replaces a
 * cJSON_PrintUnformatted() call, so the escaped output must be identical,
 * not merely equivalent under some other conformant escaping): '"' and '\'
 * become \" / \\; '\b' '\f' '\n' '\r' '\t' get their short two-character
 * form; any other control character (<0x20) becomes \u00XX (lowercase
 * hex); everything else -- including UTF-8 continuation bytes, which are
 * always >=0x80 -- copies through unescaped. dst is always
 * NUL-terminated; if src would not fit in dst_cap, the output truncates at
 * a whole-escape-unit boundary rather than overflow dst or emit a
 * half-escape -- a truncated message beats a corrupt response. A dst_cap
 * of 0 is a no-op (dst is left untouched -- there is no room even for a
 * NUL). */
void events_json_escape(const char *src, char *dst, size_t dst_cap);
