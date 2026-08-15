#pragma once
#include "esp_http_server.h"

void sse_init(httpd_handle_t server);

/* Pushes a rules/event_log event (spec §6) to every connected SSE client as
 * a named `event` message: `event: event\ndata: <json>\n\n` -- distinct
 * from the unnamed sensor-update messages sse_init() already pushes, so an
 * EventSource client can addEventListener('event', ...) for just this
 * feed. json is a complete, already-escaped JSON object text (the event_log
 * hook in main.c builds it); this function copies it, so json need not
 * outlive the call. No-op if json is NULL. Safe to call from any task --
 * same httpd_queue_work handoff as the sensor-update path (see sse.c). */
void sse_push_event(const char *json);
