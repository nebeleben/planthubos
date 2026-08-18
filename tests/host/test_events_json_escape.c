#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "events_json_escape.h"

int main(void)
{
    char dst[EVENTS_JSON_ESC_MAX];

    /* plain text passes through unchanged */
    events_json_escape("hello world", dst, sizeof dst);
    assert(strcmp(dst, "hello world") == 0);

    /* empty string -> empty, still NUL-terminated */
    events_json_escape("", dst, sizeof dst);
    assert(strcmp(dst, "") == 0);

    /* quote and backslash each become a 2-char escape */
    events_json_escape("say \"hi\" \\ bye", dst, sizeof dst);
    assert(strcmp(dst, "say \\\"hi\\\" \\\\ bye") == 0);

    /* a control character with no short JSON escape form becomes \u00XX,
     * lowercase hex. */
    {
        char raw[2] = { 0x01, '\0' };
        events_json_escape(raw, dst, sizeof dst);
        assert(strcmp(dst, "\\u0001") == 0);
    }

    /* \b \f \n \r \t each get cJSON's own short two-character escape, not
     * the generic \u00XX form -- this function stands in for a
     * cJSON_PrintUnformatted() call, so it must match cJSON's
     * print_string_ptr() byte for byte, not just produce equivalent JSON. */
    {
        char raw[6] = { '\b', '\f', '\n', '\r', '\t', '\0' };
        events_json_escape(raw, dst, sizeof dst);
        assert(strcmp(dst, "\\b\\f\\n\\r\\t") == 0);
    }

    /* 0x1f (last control char before printable range) and 0x20 (first
     * printable, must NOT be escaped) sit right at the boundary. */
    {
        char raw[2] = { 0x1f, '\0' };
        events_json_escape(raw, dst, sizeof dst);
        assert(strcmp(dst, "\\u001f") == 0);
    }
    {
        char raw[2] = { 0x20, '\0' };
        events_json_escape(raw, dst, sizeof dst);
        assert(strcmp(dst, " ") == 0);
    }

    /* UTF-8 continuation/high bytes (>=0x80) pass through untouched -- not
     * control chars, not quote/backslash. */
    {
        const char *raw = "caf\xc3\xa9";   /* "café" in UTF-8 */
        events_json_escape(raw, dst, sizeof dst);
        assert(strcmp(dst, raw) == 0);
    }

    /* a maximum-length message (EVENT_MSG_MAX chars, every one a control
     * char so each becomes the maximal 6-byte \u00XX unit) fits exactly in
     * an EVENTS_JSON_ESC_MAX buffer without truncating. */
    {
        char raw[EVENT_MSG_MAX + 1];
        memset(raw, 0x07, EVENT_MSG_MAX);
        raw[EVENT_MSG_MAX] = '\0';
        events_json_escape(raw, dst, sizeof dst);
        assert(strlen(dst) == (size_t)EVENT_MSG_MAX * 6);
        for (int i = 0; i < EVENT_MSG_MAX; i++) {
            assert(memcmp(dst + i * 6, "\\u0007", 6) == 0);
        }
    }

    /* a maximum-length ordinary (non-control) message also round-trips
     * whole, and just barely fits without truncation. */
    {
        char raw[EVENT_MSG_MAX + 1];
        memset(raw, 'x', EVENT_MSG_MAX);
        raw[EVENT_MSG_MAX] = '\0';
        events_json_escape(raw, dst, sizeof dst);
        assert(strcmp(dst, raw) == 0);
    }

    /* dst_cap == 0 is a documented no-op: dst must be left untouched. */
    {
        char sentinel[4] = { 'A', 'B', 'C', '\0' };
        events_json_escape("xyz", sentinel, 0);
        assert(strcmp(sentinel, "ABC") == 0);
    }

    /* truncation happens at a whole-escape-unit boundary, never mid-unit,
     * and dst is always left NUL-terminated. Room for exactly one 'a' plus
     * NUL: the following control char (which would need 6 bytes) must be
     * dropped whole, not partially written. */
    {
        char small[2];
        char raw[3] = { 'a', 0x01, '\0' };
        events_json_escape(raw, small, sizeof small);
        assert(strcmp(small, "a") == 0);
    }

    printf("test_events_json_escape: OK\n");
    return 0;
}
