/* APT: the applet manager. It tells the app when it may run (the "wakeup"
 * parameter), when HOME was pressed, when to sleep and when to close.
 *
 * libctru's aptInit: GetLockHandle -> Initialize (gets two events:
 * notification and parameter) -> starts its APT event thread -> Enable ->
 * waits until a WAKEUP parameter arrives. On a real 3DS the HOME menu sends
 * that parameter; here Enable queues it straight away. */
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>

#include "service.h"

#define APPID_HOMEMENU      0x101
#define APTCMD_WAKEUP       1
#define RES_NO_PARAMETER    0xC8A0CFEFu

static KObj *lock_mutex, *notify_event, *param_event;
static struct { int pending; uint32_t sender, command; } param;

static void send_parameter(uint32_t sender, uint32_t command)
{
    param.pending = 1;
    param.sender = sender;
    param.command = command;
    kevent_signal(param_event);
}

/* ReceiveParameter (0xD) and GlanceParameter (0xE): the same reply; only
   Receive takes the parameter off the queue. */
static void parameter(uint32_t *cmd, int receive)
{
    uint32_t id = cmd[0] >> 16;
    if (!param.pending) {
        ipc_reply(cmd, RES_NO_PARAMETER);
        return;
    }
    cmd[0] = ipc_header(id, 4, 4);
    cmd[1] = RES_OK;
    cmd[2] = param.sender;
    cmd[3] = param.command;
    cmd[4] = 0;                             /* size of the parameter data */
    cmd[5] = IPC_MOVE_HANDLES(1);
    cmd[6] = 0;                             /* no handle travels with it */
    cmd[7] = IPC_STATIC_BUFFER(0, 0);
    cmd[8] = ipc_static_buffer(0, NULL);
    if (hle_trace || receive)
        fprintf(stderr, "    -> APT parameter: command %" PRIu32 " from applet %03" PRIx32 "%s\n",
                param.command, param.sender, receive ? "" : " (glance)");
    if (receive)
        param.pending = 0;
}

void apt_handle(KObj *session, uint32_t *cmd)
{
    (void)session;
    switch (cmd[0] >> 16) {
    case 0x01:      /* GetLockHandle(flags) -> applet attributes, power state, lock mutex */
        if (!lock_mutex)
            lock_mutex = kobj_new(KO_MUTEX, "APT lock");
        cmd[0] = ipc_header(0x1, 3, 2);
        cmd[1] = RES_OK;
        cmd[2] = 0;
        cmd[3] = 0;
        cmd[4] = IPC_COPY_HANDLES(1);
        cmd[5] = handle_new(lock_mutex);
        return;
    case 0x02:      /* Initialize(app id, attributes) -> notification + parameter events */
        notify_event = kevent_new(KRESET_ONESHOT, "APT notification");
        param_event = kevent_new(KRESET_ONESHOT, "APT parameter");
        cmd[0] = ipc_header(0x2, 1, 3);
        cmd[1] = RES_OK;
        cmd[2] = IPC_COPY_HANDLES(2);
        cmd[3] = handle_new(notify_event);
        cmd[4] = handle_new(param_event);
        return;
    case 0x03:      /* Enable(attributes): HOME menu answers with WAKEUP */
        ipc_reply(cmd, RES_OK);
        send_parameter(APPID_HOMEMENU, APTCMD_WAKEUP);
        return;
    case 0x06:      /* GetAppletInfo(app id) -> title id, media type, registered, loaded, attributes */
        cmd[0] = ipc_header(0x6, 7, 0);
        cmd[1] = RES_OK;
        cmd[2] = cmd[3] = 0;
        cmd[4] = 0;
        cmd[5] = 1;
        cmd[6] = 1;
        cmd[7] = 0;
        return;
    case 0x0B:      /* InquireNotification(app id) -> signal (0 = none) */
        cmd[0] = ipc_header(0xB, 2, 0);
        cmd[1] = RES_OK;
        cmd[2] = 0;
        return;
    case 0x0D:
        parameter(cmd, 1);
        return;
    case 0x0E:
        parameter(cmd, 0);
        return;
    case 0x04:      /* Finalize */
    case 0x22:      /* PrepareToCloseApplication */
    case 0x27:      /* CloseApplication */
    case 0x43:      /* NotifyToWait */
    case 0x4F:      /* SetAppCpuTimeLimit */
        ipc_reply(cmd, RES_OK);
        return;
    case 0x4B:      /* AppletUtility(id, ...) -> the utility's own result */
        cmd[0] = ipc_header(0x4B, 2, 0);
        cmd[1] = RES_OK;
        cmd[2] = RES_OK;
        return;
    case 0x44:      /* GetSharedFont: the system font lives in the console's firmware */
        fprintf(stderr, "hle: APT GetSharedFont: the system font is firmware data, which we don't have\n");
        exit(3);
    case 0x102:     /* CheckNew3DS -> 0: an original 3DS (no C-stick/ZL/ZR, 268 MHz) */
        cmd[0] = ipc_header(0x102, 2, 0);
        cmd[1] = RES_OK;
        cmd[2] = 0;
        return;
    case 0x50:      /* GetAppCpuTimeLimit -> percent */
        cmd[0] = ipc_header(0x50, 2, 0);
        cmd[1] = RES_OK;
        cmd[2] = 0;
        return;
    default:
        ipc_unhandled("APT", cmd);
    }
}
