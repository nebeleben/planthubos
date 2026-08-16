/* test_uri_handler_budget.c -- guards cfg.max_uri_handlers in
 * components/webserver/webserver.c against the number of URI handlers the
 * firmware actually registers.
 *
 * Why this test exists, in the words of the failure it is here to prevent:
 * V2 M3 Task 7 added seven routes (wrappers, unknown devices, bind keys) and
 * took the registered total from 41 to 48 against a cap of 40. Nothing
 * caught it. Both firmware targets built clean, all host suites passed, the
 * browser suite passed, and two separate code reviews read the diff without
 * seeing it -- because the defect is not IN the diff, it is in the
 * relationship between the diff and a constant in another component. On
 * hardware it is not subtle at all: httpd_register_uri_handler() returns
 * ESP_ERR_HTTPD_HANDLERS_FULL, ESP_ERROR_CHECK aborts, and the board boot
 * loops at ~520 ms. It was found by flashing a board three tasks later.
 *
 * This test is deliberately a TEXT check over the sources rather than
 * anything that links ESP-IDF: the quantity it protects is a static count of
 * registration sites, and a host test cannot start an httpd server to ask it.
 * That makes the check approximate in one direction only -- it can miscount
 * if someone writes a registration in a shape it does not recognise -- so it
 * prints its arithmetic on every run instead of silently asserting, and the
 * shapes it recognises are pinned below. A miscount that reads LOW is the
 * dangerous one, so the parser also fails loudly if any file yields zero
 * registrations, which is what a changed idiom would look like.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SRC_DIR "../../components/webserver/"

/* The one registration shape used everywhere in this codebase. Counting the
 * ESP_ERROR_CHECK wrapper rather than the bare call is what keeps prose
 * mentions of httpd_register_uri_handler() in comments from being counted. */
static const char REG[] = "ESP_ERROR_CHECK(httpd_register_uri_handler(";

static char *slurp(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "FAIL: cannot open %s\n", path); exit(1); }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = malloc((size_t)n + 1);
    if (!buf || fread(buf, 1, (size_t)n, f) != (size_t)n) {
        fprintf(stderr, "FAIL: cannot read %s\n", path);
        exit(1);
    }
    buf[n] = '\0';
    fclose(f);
    return buf;
}

static int count_occurrences(const char *hay, const char *needle)
{
    int n = 0;
    for (const char *p = hay; (p = strstr(p, needle)) != NULL; p += strlen(needle)) n++;
    return n;
}

/* Number of entries in webserver.c's ASSETS[] table. The asset registrations
 * happen inside a for loop, so ONE textual registration site stands for this
 * many real handlers -- the single place static counting has to know
 * something about the code's shape. */
static int count_assets(const char *src)
{
    const char *start = strstr(src, "ASSETS[] = {");
    if (!start) { fprintf(stderr, "FAIL: ASSETS[] table not found -- has webserver.c been restructured?\n"); exit(1); }
    const char *end = strstr(start, "};");
    if (!end) { fprintf(stderr, "FAIL: ASSETS[] table has no terminator\n"); exit(1); }
    int n = 0;
    for (const char *p = start; (p = strchr(p, '{')) != NULL && p < end; p++) {
        /* Each entry is `{ "/uri", "mime", ... }`; the table's own opening
         * brace is the one at `start`, which has no quote after it. */
        const char *q = p + 1;
        while (*q == ' ' || *q == '\t' || *q == '\n') q++;
        if (*q == '"') n++;
    }
    return n;
}

static int parse_cap(const char *src)
{
    const char *p = strstr(src, "cfg.max_uri_handlers");
    if (!p) { fprintf(stderr, "FAIL: cfg.max_uri_handlers not found in webserver.c\n"); exit(1); }
    p = strchr(p, '=');
    if (!p) { fprintf(stderr, "FAIL: cfg.max_uri_handlers has no assignment\n"); exit(1); }
    return atoi(p + 1);
}

int main(void)
{
    char *api  = slurp(SRC_DIR "api_v1.c");
    char *sse  = slurp(SRC_DIR "sse.c");
    char *web  = slurp(SRC_DIR "webserver.c");

    int n_api = count_occurrences(api, REG);
    int n_sse = count_occurrences(sse, REG);
    int n_web_sites = count_occurrences(web, REG);
    int n_assets = count_assets(web);
    int cap = parse_cap(web);

    /* webserver.c's two sites are the ASSETS loop (n_assets handlers) and the
     * captive-portal fallback (1). If that ever stops being exactly two
     * sites, this arithmetic is wrong and must be revisited rather than
     * quietly adjusted. */
    if (n_web_sites != 2) {
        fprintf(stderr, "FAIL: expected exactly 2 registration sites in webserver.c "
                        "(ASSETS loop + captive-portal fallback), found %d -- "
                        "update this test's arithmetic deliberately\n", n_web_sites);
        return 1;
    }
    int n_web = n_assets + 1;

    if (n_api == 0 || n_sse == 0 || n_assets == 0) {
        fprintf(stderr, "FAIL: counted zero registrations in one of the sources "
                        "(api_v1=%d sse=%d assets=%d) -- the registration idiom "
                        "has changed and this test is now blind\n",
                n_api, n_sse, n_assets);
        return 1;
    }

    int total = n_api + n_sse + n_web;
    printf("uri handlers: api_v1=%d sse=%d assets=%d fallback=1 total=%d cap=%d headroom=%d\n",
           n_api, n_sse, n_assets, total, cap, cap - total);

    if (total > cap) {
        fprintf(stderr,
                "FAIL: %d URI handlers registered against cfg.max_uri_handlers=%d.\n"
                "      On hardware this is not a warning: httpd_register_uri_handler()\n"
                "      returns ESP_ERR_HTTPD_HANDLERS_FULL, ESP_ERROR_CHECK aborts, and\n"
                "      the board BOOT LOOPS. Raise cfg.max_uri_handlers in\n"
                "      components/webserver/webserver.c (each slot costs one pointer).\n",
                total, cap);
        return 1;
    }

    /* Headroom is the whole point -- a cap raised to exactly today's count
     * puts the next task back in the boot loop. */
    if (cap - total < 4) {
        fprintf(stderr,
                "FAIL: only %d spare URI handler slots (%d of %d used). Raise\n"
                "      cfg.max_uri_handlers with headroom rather than to clear\n"
                "      today's count -- each spare slot costs one pointer of heap.\n",
                cap - total, total, cap);
        return 1;
    }

    free(api); free(sse); free(web);
    printf("test_uri_handler_budget: OK\n");
    return 0;
}
