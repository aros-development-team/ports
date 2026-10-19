/*
 * aros_extra.c - what SolidCraft's AROS shell needs beyond aros-gui-sys:
 * the event loop's task priority, the busy pointer and OpenURL. Library bases come from the autoinit link libraries.
 */

#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/intuition.h>

#include <intuition/intuition.h>
#include <intuition/pointerclass.h>

#include <stdint.h>
#include <string.h>

/* The calling task's priority (the event loop lowers itself behind workers). */
void sc_set_task_priority(int32_t pri)
{
    SetTaskPri(FindTask(NULL), pri);
}

/* The window's pointer: the busy (wait) pointer, or the usual one. */
void sc_window_set_busy(uintptr_t window, int32_t busy)
{
    struct Window *w = (struct Window *)window;

    if (!w)
        return;
    SetWindowPointer(w, WA_BusyPointer, busy ? TRUE : FALSE, TAG_DONE);
}

/* Hands the URL to the OpenURL command (openurl.library's); 1 when it ran. */
int32_t sc_open_url(const char *url)
{
    char cmd[2048];

    if (!url || strlen(url) > sizeof cmd - 16)
        return 0;
    strcpy(cmd, "OpenURL \"");
    strcat(cmd, url);
    strcat(cmd, "\"");
    return SystemTags((CONST_STRPTR)cmd, SYS_Asynch, FALSE, TAG_DONE) == 0;
}
