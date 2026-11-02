/*
 * amiga_wblaunch.c - started from the Workbench icon, run the `run` script.
 *
 * WHY. Going from one level to the next loads a different city: another
 * tile set, map, sound bank and script section. This port does that the way
 * it already reloads a new build in the test rig - the game leaves with
 * return code 5 and the `run` script beside it starts it again (`if warn`,
 * `skip again back`). From a Shell that just works. From the Workbench icon
 * there is no script: the game would simply have closed at the end of the
 * first level.
 *
 * So when the program finds it was started from Workbench, it does not play.
 * It runs `Execute run` in its own drawer (Workbench makes that the current
 * directory) and waits for it, and the script - which also sets the 1 MB
 * stack the game needs - starts the game from a Shell and keeps restarting it
 * on code 5. The cost is this launching copy staying loaded while the game
 * runs: the program's code and BSS, about 650 KB.
 *
 * No `run` script in the drawer, or SystemTags failing: return 0 and the
 * game plays from the icon as it always did, one level at a time.
 */
#include <exec/types.h>
#include <dos/dos.h>
#include <dos/dostags.h>
#include <workbench/startup.h>
#include <proto/dos.h>

#include "amiga_wblaunch.h"

extern struct WBStartup *_WBenchMsg;   /* libnix: non-NULL from Workbench */

int amiga_wb_relaunch(void)
{
    BPTR lock, in, out;
    LONG rc;

    if (_WBenchMsg == NULL)
        return 0;
    /* the drawer the program was loaded from, whatever Workbench set */
    if (GetProgramDir())
        CurrentDir(GetProgramDir());
    lock = Lock((CONST_STRPTR)"run", ACCESS_READ);
    if (!lock)
        return 0;
    UnLock(lock);

    in  = Open((CONST_STRPTR)"NIL:", MODE_OLDFILE);
    out = Open((CONST_STRPTR)"NIL:", MODE_NEWFILE);
    if (!in || !out) {
        if (in)  Close(in);
        if (out) Close(out);
        return 0;
    }
    rc = SystemTags((CONST_STRPTR)"Execute run",
                    SYS_Input,  in,
                    SYS_Output, out,
                    NP_StackSize, 65536,
                    TAG_END);
    /* SystemTags closes neither handle: they belong to the caller. */
    Close(in);
    Close(out);
    return rc == -1 ? 0 : 1;
}
