#include "doomgeneric.h"

int main(void)
{
    char arg0[] = "doomcpu";
    char arg1[] = "-iwad";
    char arg2[] = "freedoom1.wad";
    char *argv[] = { arg0, arg1, arg2, 0 };

    doomgeneric_Create(3, argv);

    for (;;)
        doomgeneric_Tick();

    return 0;
}
