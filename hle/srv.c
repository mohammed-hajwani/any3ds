/* srv: the service manager. Apps connect to the "srv:" port, then ask it
 * for a session to each service by name. */
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "service.h"

#define RES_SRV_NOT_FOUND 0xD8E06406u

void srv_handle(KObj *session, uint32_t *cmd)
{
    (void)session;
    switch (cmd[0] >> 16) {
    case 0x1:       /* RegisterClient(process id descriptor) */
        ipc_reply(cmd, RES_OK);
        return;
    case 0x2: {     /* EnableNotification -> semaphore that counts notifications */
        KObj *sem = kobj_new(KO_SEMAPHORE, "srv notification");
        sem->sem.max = 64;
        cmd[0] = ipc_header(0x2, 1, 2);
        cmd[1] = RES_OK;
        cmd[2] = IPC_COPY_HANDLES(1);
        cmd[3] = handle_new(sem);
        return;
    }
    case 0x5: {     /* GetServiceHandle(name[8], name length, flags) */
        char name[9] = { 0 };
        memcpy(name, &cmd[1], 8);
        KObj *session = service_connect(name);
        if (!session) {
            fprintf(stderr, "hle: srv:GetServiceHandle(\"%s\"): service not implemented, returning an error\n", name);
            ipc_reply(cmd, RES_SRV_NOT_FOUND);
            return;
        }
        cmd[0] = ipc_header(0x5, 1, 2);
        cmd[1] = RES_OK;
        cmd[2] = IPC_MOVE_HANDLES(1);
        cmd[3] = handle_new(session);
        return;
    }
    case 0xE: {     /* IsServiceRegistered(name[8], name length) -> bool */
        char name[9] = { 0 };
        memcpy(name, &cmd[1], 8);
        cmd[0] = ipc_header(0xE, 2, 0);
        cmd[1] = RES_OK;
        cmd[2] = service_known(name);
        return;
    }
    default:
        ipc_unhandled("srv:", cmd);
    }
}
