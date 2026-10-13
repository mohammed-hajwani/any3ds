/* IPC: ports, service sessions and request dispatch. */
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "service.h"

static const Service services[] = {
    { "APT:U",    apt_handle },
    { "APT:S",    apt_handle },
    { "APT:A",    apt_handle },
    { "gsp::Gpu", gsp_handle },
    { "hid:USER", hid_handle },
    { "hid:SPVR", hid_handle },
    { "fs:USER",  fs_handle },
    { "cfg:u",    cfg_handle },
    { "cfg:s",    cfg_handle },
    { "cfg:i",    cfg_handle },
    { "dsp::DSP", dsp_handle },
};

static const Service srv_port = { "srv:", srv_handle };

KObj *session_new(const Service *s, void *data)
{
    KObj *o = kobj_new(KO_SESSION, s->name);
    o->service = s;
    o->data = data;
    return o;
}

KObj *port_connect(const char *name)
{
    if (strcmp(name, srv_port.name) == 0)
        return session_new(&srv_port, NULL);
    fprintf(stderr, "hle: ConnectToPort(\"%s\"): no such port\n", name);
    return NULL;
}

int service_known(const char *name)
{
    for (size_t i = 0; i < sizeof services / sizeof services[0]; i++)
        if (strcmp(name, services[i].name) == 0)
            return 1;
    return stub_service(name) != NULL;
}

KObj *service_connect(const char *name)
{
    for (size_t i = 0; i < sizeof services / sizeof services[0]; i++)
        if (strcmp(name, services[i].name) == 0)
            return session_new(&services[i], NULL);
    const Service *stub = stub_service(name);
    return stub ? session_new(stub, NULL) : NULL;
}

static uint32_t *command_buffer(void)
{
    return (uint32_t *)(kernel_mem() + kernel_current()->tls + 0x80);
}

uint32_t ipc_static_buffer(int index, uint32_t *size)
{
    uint32_t *desc = (uint32_t *)(kernel_mem() + kernel_current()->tls + 0x180 + 8 * (uint32_t)index);
    if (size)
        *size = desc[0] >> 14;
    return desc[1];
}

void ipc_reply(uint32_t *cmd, uint32_t result)
{
    cmd[0] = ipc_header(cmd[0] >> 16, 1, 0);
    cmd[1] = result;
}

void ipc_unhandled(const char *service, uint32_t *cmd)
{
    uint32_t header = cmd[0];
    if (getenv("HLE_LENIENT")) {
        fprintf(stderr, "hle: UNHANDLED %s command 0x%04" PRIx32 " (header %08" PRIx32 "): replying success\n",
                service, header >> 16, header);
        memset(&cmd[2], 0, 14 * 4);
        ipc_reply(cmd, RES_OK);
        return;
    }
    fprintf(stderr, "hle: %s command 0x%04" PRIx32 " (header %08" PRIx32 ", words %08" PRIx32 " %08" PRIx32
            " %08" PRIx32 " %08" PRIx32 ") is not implemented; HLE_LENIENT=1 replies success instead\n",
            service, header >> 16, header, cmd[1], cmd[2], cmd[3], cmd[4]);
    exit(3);
}

/* Log the first use of every (service, command), so a run shows what an
   app needs without drowning in per-frame requests. */
static void log_first_use(const char *service, uint32_t header)
{
    typedef struct { const char *service; uint32_t id; } Seen;
    static Seen seen[256];
    static int nseen;
    uint32_t id = header >> 16;
    for (int i = 0; i < nseen; i++)
        if (seen[i].id == id && strcmp(seen[i].service, service) == 0) {
            if (!hle_trace)
                return;
            goto print;
        }
    if (nseen < 256)
        seen[nseen++] = (Seen){ service, id };
print:
    fprintf(stderr, "    IPC %-8s cmd 0x%04" PRIx32 " (header %08" PRIx32 ")\n", service, id, header);
}

void ipc_request(KObj *session)
{
    uint32_t *cmd = command_buffer();
    log_first_use(session->service->name, cmd[0]);
    session->service->handle(session, cmd);
}
