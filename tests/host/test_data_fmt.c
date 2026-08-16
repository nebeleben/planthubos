#include <assert.h>
#include <stdio.h>
#include "data_fmt.h"
#include "plants_table.h"   /* PLANTS_MAX -- cross-check against data_fmt.c's own copy below */

/* data_fmt.c's device-side wipe loop cannot #include plants_table.h itself
 * (would create an app_config <-> plants component requires cycle -- see
 * data_fmt.c's WIPE_PLANT_ID_MAX comment), so it carries its own copy of
 * this constant. This assertion is the drift guard: if plants_table.h's
 * PLANTS_MAX ever changes, ./run.sh fails here until data_fmt.c's copy is
 * updated to match, instead of silently under- or over-wiping plant ids on
 * a real device. */
_Static_assert(PLANTS_MAX == 16, "data_fmt.c's WIPE_PLANT_ID_MAX (16) must be updated to match PLANTS_MAX");

int main(void)
{
    /* absent marker -> wipe (fresh device, or a genuinely pre-M2 one) */
    assert(data_fmt_decide(false, 0) == DATA_FMT_WIPE);
    assert(data_fmt_decide(false, 2) == DATA_FMT_WIPE);   /* stored value is meaningless when absent */

    /* stored 1 (V1) -> wipe */
    assert(data_fmt_decide(true, 1) == DATA_FMT_WIPE);

    /* stored 0 -> wipe too (defensively below current, same as 1) */
    assert(data_fmt_decide(true, 0) == DATA_FMT_WIPE);

    /* stored 2 (current) -> ok, no wipe */
    assert(data_fmt_decide(true, DATA_FMT_CURRENT) == DATA_FMT_OK);
    assert(data_fmt_decide(true, 2) == DATA_FMT_OK);

    /* stored 3 (a future format -- e.g. a downgrade after newer firmware
     * already ran) -> refuse to touch data, do not wipe */
    assert(data_fmt_decide(true, 3) == DATA_FMT_FUTURE);
    assert(data_fmt_decide(true, 255) == DATA_FMT_FUTURE);

    printf("test_data_fmt: OK\n");
    return 0;
}
