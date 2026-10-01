/* nogame.c - used when the app is built without the game file: the front
 * end shows a message instead of the game. */
#include "c64.h"

int lj_game_init(int sample_rate)
{
    lj_power_on(sample_rate);
    return -1;
}

const char *lj_game_version(void) { return "no game"; }
