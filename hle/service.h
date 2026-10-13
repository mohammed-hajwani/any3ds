/* HLE services and the IPC plumbing they share.
 *
 * An IPC request is a command buffer in the calling thread's TLS at
 * +0x80. Word 0 is the header:
 *     command id << 16 | normal words << 6 | translate words
 * "Normal" words are plain values. "Translate" words come in pairs of a
 * descriptor and a value, and the kernel converts them on the way through:
 * handles get copied or moved into the receiving process, buffers get
 * mapped or copied. The reply is written over the request in the same
 * format; word 1 of a reply is always the result code. */
#ifndef HLE_SERVICE_H
#define HLE_SERVICE_H

#include <stdint.h>

#include "kernel.h"

typedef struct Service {
    const char *name;
    void (*handle)(KObj *session, uint32_t *cmd);   /* session: per-session state in session->data */
} Service;

static inline uint32_t ipc_header(uint32_t id, uint32_t normal, uint32_t translate)
{
    return id << 16 | normal << 6 | translate;
}

#define IPC_COPY_HANDLES(n)          (((uint32_t)(n) - 1) << 26)
#define IPC_MOVE_HANDLES(n)          ((((uint32_t)(n) - 1) << 26) | 0x10)
#define IPC_STATIC_BUFFER(size, idx) ((uint32_t)(size) << 14 | (uint32_t)(idx) << 10 | 2)

/* Reply with just "success" (or another result) and no outputs. */
void ipc_reply(uint32_t *cmd, uint32_t result);

/* An unimplemented command: a hard stop that names it (with HLE_LENIENT=1,
   a logged bare-success reply instead, for exploring what an app needs). */
void ipc_unhandled(const char *service, uint32_t *cmd);

/* Where the calling thread wants static buffer `index` delivered (its
   receive descriptors live at TLS + 0x180). */
uint32_t ipc_static_buffer(int index, uint32_t *size);

KObj *port_connect(const char *name);       /* svcConnectToPort; NULL if unknown */
KObj *service_connect(const char *name);    /* srv:GetServiceHandle; NULL if unknown */
int   service_known(const char *name);      /* srv:IsServiceRegistered */
void  ipc_request(KObj *session);           /* svcSendSyncRequest */

/* The services. */
void srv_handle(KObj *session, uint32_t *cmd);
void apt_handle(KObj *session, uint32_t *cmd);
void gsp_handle(KObj *session, uint32_t *cmd);
void hid_handle(KObj *session, uint32_t *cmd);
void fs_handle(KObj *session, uint32_t *cmd);
void cfg_handle(KObj *session, uint32_t *cmd);
void fs_file_handle(KObj *session, uint32_t *cmd);     /* an open file (FSFILE_*) */
void fs_dir_handle(KObj *session, uint32_t *cmd);      /* an open directory (FSDIR_*) */

/* Services that only need to say "fine" (stubs.c): each command listed. */
void stub_handle(KObj *session, uint32_t *cmd);
const Service *stub_service(const char *name);         /* NULL if not a stubbed service */

/* Make a session to an object served by a service (an open file, ...). */
KObj *session_new(const Service *service, void *data);

#endif
