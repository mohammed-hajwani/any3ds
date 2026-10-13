/* Services an app talks to but whose work we don't do: network daemons,
 * power/battery, friends, notifications and the like. A game asks them for
 * permission or state at boot and carries on when they say "fine".
 *
 * Every command answered here is listed explicitly (from 3dbrew) with the
 * number of words its reply carries after the result: the first is `value`,
 * the rest zero. A command not listed is still a hard stop (ipc_unhandled),
 * so an app never runs on silently invented answers. */
#include <stdio.h>
#include <string.h>

#include "service.h"

typedef struct {
    uint16_t id;
    uint8_t outputs;            /* reply words after the result */
    const char *name;
    uint32_t value;             /* the first of them (the others are zero) */
    uint8_t buffer;             /* if non-zero: the request word holding an output buffer's
                                   descriptor (the next word is its address): it is zeroed */
} StubCmd;

typedef struct {
    Service service;            /* name + stub_handle, handed out by srv */
    const StubCmd *cmds;
} StubService;

/* ndm:u, the network daemon manager: apps ask it to pause background
   networking (StreetPass, SpotPass) while they run. We have none. */
static const StubCmd ndm_cmds[] = {
    { 0x0001, 0, "EnterExclusiveState", 0, 0 },
    { 0x0002, 0, "LeaveExclusiveState", 0, 0 },
    { 0x0003, 1, "QueryExclusiveMode", 0, 0 },
    { 0x0004, 0, "LockState", 0, 0 },
    { 0x0005, 0, "UnlockState", 0, 0 },
    { 0x0006, 0, "SuspendDaemons", 0, 0 },
    { 0x0007, 0, "ResumeDaemons", 0, 0 },
    { 0x0008, 0, "SuspendScheduler", 0, 0 },
    { 0x0009, 0, "ResumeScheduler", 0, 0 },
    { 0x000A, 1, "GetCurrentState", 0, 0 },
    { 0x000B, 1, "GetTargetState", 0, 0 },
    { 0x000D, 1, "QueryStatus", 0, 0 },
    { 0x000E, 1, "GetDaemonDisableCount", 0, 0 },
    { 0x000F, 1, "GetSchedulerDisableCount", 0, 0 },
    { 0x0010, 0, "SetScanInterval", 0, 0 },
    { 0x0011, 1, "GetScanInterval", 0, 0 },
    { 0x0012, 0, "SetRetryInterval", 0, 0 },
    { 0x0013, 1, "GetRetryInterval", 0, 0 },
    { 0x0014, 0, "OverrideDefaultDaemons", 0, 0 },
    { 0x0015, 0, "ResetDefaultDaemons", 0, 0 },
    { 0x0016, 1, "GetDefaultDaemons", 0, 0 },
    { 0x0017, 0, "ClearHalfAwakeMacFilter", 0, 0 },
    { 0 },
};

/* ptm:u, power and the pedometer: a console on its charger with the lid
   open and a full battery, which has walked nowhere. */
static const StubCmd ptm_cmds[] = {
    { 0x0001, 0, "RegisterAlarmClient", 0, 0 },
    { 0x0002, 0, "SetRtcAlarm", 0, 0 },
    { 0x0003, 2, "GetRtcAlarm", 0, 0 },
    { 0x0004, 0, "CancelRtcAlarm", 0, 0 },
    { 0x0005, 1, "GetAdapterState", 1, 0 },          /* 1: charger connected */
    { 0x0006, 1, "GetShellState", 1, 0 },            /* 1: lid open */
    { 0x0007, 1, "GetBatteryLevel", 5, 0 },          /* 0-5: 5 is full */
    { 0x0008, 1, "GetBatteryChargeState", 0, 0 },    /* 0: not charging (full) */
    { 0x0009, 1, "GetPedometerState", 0, 0 },        /* 0: not counting */
    { 0x000B, 0, "GetStepHistory", 0, 4 },        /* steps per hour: none */
    { 0x000C, 1, "GetTotalStepCount", 0, 0 },
    { 0x000D, 0, "SetPedometerRecordingMode", 0, 0 },
    { 0x000E, 1, "GetPedometerRecordingMode", 0, 0 },
    { 0 },
};

static const StubService stub_services[] = {
    { { "ndm:u", stub_handle }, ndm_cmds },
    { { "ptm:u", stub_handle }, ptm_cmds },
};

static const StubService *find(const char *name)
{
    for (size_t i = 0; i < sizeof stub_services / sizeof stub_services[0]; i++)
        if (strcmp(stub_services[i].service.name, name) == 0)
            return &stub_services[i];
    return NULL;
}

const Service *stub_service(const char *name)
{
    const StubService *s = find(name);
    return s ? &s->service : NULL;
}

void stub_handle(KObj *session, uint32_t *cmd)
{
    const char *name = session->service->name;
    const StubService *s = find(name);
    uint32_t id = cmd[0] >> 16;
    for (const StubCmd *c = s ? s->cmds : NULL; c && c->name; c++) {
        if (c->id != id)
            continue;
        uint32_t desc = c->buffer ? cmd[c->buffer] : 0, addr = c->buffer ? cmd[c->buffer + 1] : 0;
        if (c->buffer)
            memset(kernel_mem() + addr, 0, desc >> 4);     /* a mapped buffer: size << 4 | flags */
        cmd[0] = ipc_header(id, 1u + c->outputs, c->buffer ? 2 : 0);
        cmd[1] = RES_OK;
        for (uint32_t i = 0; i < c->outputs; i++)
            cmd[2 + i] = i == 0 ? c->value : 0;
        if (c->buffer) {
            cmd[2 + c->outputs] = desc;
            cmd[3 + c->outputs] = addr;
        }
        return;
    }
    ipc_unhandled(name, cmd);
}
