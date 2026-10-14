/* FS (fs:USER): files.
 *
 * Three archives are backed by something real:
 *   SDMC (9)      the SD card: a host folder (runner --sdmc DIR); none by default
 *   SAVEDATA (4)  this app's save data: a host folder (runner --save DIR) holding
 *                 data/ (the files) and format.bin (written by FormatSaveData;
 *                 until then the archive is "not formatted" and the app formats it)
 *   EXTSAVEDATA (6) extra save data, by id: DIR/extdata/<id>/ (CreateExtSaveData
 *                 makes it; until then opening it says "not found")
 *   ROMFS (3)     this app's read-only RomFS: served from a host file (runner
 *                 --romfs FILE), a raw RomFS image, a decrypted .cxi/.3ds, or a .3dsx, whose
 *                 RomFS sits at the offset its header gives (fsOffset)
 * Opening a file or directory returns a new session (handle) on which the
 * app sends FSFILE_* / FSDIR_* commands.
 *
 * Paths arrive as FS_Path: a type (1 empty, 2 binary, 3 ASCII, 4 UTF-16)
 * plus bytes the kernel copies across as a static buffer. SD paths are kept
 * inside the host folder: ".." is refused. */
#define _POSIX_C_SOURCE 200809L
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include "service.h"

#define ARCHIVE_ROMFS    3
#define ARCHIVE_SAVEDATA 4
#define ARCHIVE_EXTSAVEDATA 6
#define ARCHIVE_SDMC     9

#define RES_FS_NOT_FOUND      0xC8804478u
#define RES_FS_EXISTS         0xC82044BEu
#define RES_FS_BAD_PATH       0xE0E046BEu
#define RES_FS_NOT_SUPPORTED  0xE0C046BEu
/* An unformatted save data archive: level Status, summary InvalidState,
   module FS, description 340. Games format their save when they see it. */
#define RES_FS_NOT_FORMATTED  0xC8A04554u

#define PATH_EMPTY  1
#define PATH_BINARY 2
#define PATH_ASCII  3
#define PATH_UTF16  4

#define OPEN_WRITE  2
#define OPEN_CREATE 4

static const char *sdmc_dir;
static const char *save_dir;
static char save_data_dir[512];
static uint8_t *romfs;              /* the RomFS image, from the RomFS header on */
static uint64_t romfs_size;

/* An open file: a host file descriptor, or a window on the RomFS image. */
typedef struct {
    int fd;                         /* -1: RomFS */
    char path[512];
} File;

typedef struct {
    char **names;
    int n, next;
    char path[512];
} Dir;

static const Service file_service = { "FS file", fs_file_handle };
static const Service dir_service = { "FS directory", fs_dir_handle };

/* Archive handles (u64 to the app): which archive each one is. */
#define MAX_ARCHIVES 32
static struct { uint32_t id; char root[512]; } archives[MAX_ARCHIVES];
static int narchives;

void fs_set_sdmc(const char *dir)
{
    sdmc_dir = dir;
}

void fs_set_save(const char *dir)
{
    save_dir = dir;
    mkdir(dir, 0755);
    snprintf(save_data_dir, sizeof save_data_dir, "%s/data", dir);
}

/* An archive's format (GetFormatInfo): total size, directories, files,
   duplicate. Kept in a small file: format.bin for save data (from
   FormatSaveData), <extdata folder>.format for ExtSaveData (from
   CreateExtSaveData). */
typedef struct { uint32_t total_size, dirs, files, duplicate; } SaveFormat;

static int format_read(const char *path, SaveFormat *out)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return 0;
    int ok = fread(out, sizeof *out, 1, f) == 1;
    fclose(f);
    return ok;
}

static int format_write(const char *path, const SaveFormat *fmt)
{
    FILE *f = fopen(path, "wb");
    int ok = f && fwrite(fmt, sizeof *fmt, 1, f) == 1;
    if (f)
        fclose(f);
    return ok;
}

static int save_format(SaveFormat *out)
{
    char path[600];
    if (!save_dir)
        return 0;
    snprintf(path, sizeof path, "%s/format.bin", save_dir);
    return format_read(path, out);
}

/* The host folder of an ExtSaveData id (its binary archive path: media
   type, then the u64 id). */
static void extdata_dir(uint32_t lo, uint32_t hi, char *out, size_t cap)
{
    snprintf(out, cap, "%s/extdata/%08" PRIx32 "%08" PRIx32, save_dir ? save_dir : ".", hi, lo);
}

/* The host folder behind an archive (id, and the archive's own path for
   ExtSaveData), or NULL if it has none (yet). */
static const char *archive_root_path(uint32_t id, uint32_t type, uint32_t size, uint32_t addr)
{
    static char ext[512];
    SaveFormat fmt;
    struct stat st;
    if (id == ARCHIVE_SDMC)
        return sdmc_dir;
    if (id == ARCHIVE_SAVEDATA && save_format(&fmt))
        return save_data_dir;
    if (id == ARCHIVE_EXTSAVEDATA && save_dir && type == PATH_BINARY && size >= 12) {
        const uint32_t *w = (const uint32_t *)(kernel_mem() + addr);
        extdata_dir(w[1], w[2], ext, sizeof ext);
        return stat(ext, &st) == 0 && S_ISDIR(st.st_mode) ? ext : NULL;
    }
    return NULL;
}

static void remove_tree(const char *path)
{
    DIR *d = opendir(path);
    if (d) {
        for (struct dirent *e; (e = readdir(d));) {
            if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0)
                continue;
            char child[1024];
            snprintf(child, sizeof child, "%s/%s", path, e->d_name);
            remove_tree(child);
        }
        closedir(d);
        rmdir(path);
    } else {
        unlink(path);
    }
}

static uint32_t le32(const uint8_t *p)
{
    uint32_t v;
    memcpy(&v, p, 4);
    return v;
}

/* Where the RomFS level 3 (the actual file system) sits inside a file:
     - a .3dsx: the extended header's fsOffset (word at 0x28);
     - a decrypted NCCH (.cxi), or a .3ds whose first partition is one:
       the NCCH's RomFS is wrapped in an IVFC hash tree; level 3 starts
       after the 0x60-byte header and the master hash, aligned to its block
       size (3dbrew: RomFS);
     - anything else: a raw RomFS image.
   Returns 0 if the file has no RomFS we can read. */
static int romfs_offset(const uint8_t *data, uint64_t size, uint64_t *offset, uint64_t *length)
{
    *offset = 0;
    *length = size;
    if (size >= 0x2C && memcmp(data, "3DSX", 4) == 0) {
        uint16_t header_size;
        memcpy(&header_size, data + 4, 2);
        uint32_t off = header_size >= 0x2C ? le32(data + 0x28) : 0;
        if (!off || off >= size)
            return 0;
        *offset = off;
        *length = size - off;
        return 1;
    }
    uint64_t ncch = 0;
    if (size >= 0x200 && memcmp(data + 0x100, "NCSD", 4) == 0)
        ncch = (uint64_t)le32(data + 0x120) * 0x200;            /* partition 0 */
    if (ncch + 0x200 <= size && memcmp(data + ncch + 0x100, "NCCH", 4) == 0) {
        const uint8_t *h = data + ncch;
        if (!(h[0x18F] & 4)) {
            fprintf(stderr, "hle: FS: this NCCH is encrypted; decrypt your own dump on your console first\n");
            return 0;
        }
        uint64_t ivfc = ncch + (uint64_t)le32(h + 0x1B0) * 0x200;
        if (!le32(h + 0x1B4) || ivfc + 0x60 > size || memcmp(data + ivfc, "IVFC", 4) != 0)
            return 0;
        uint32_t master = le32(data + ivfc + 0x08), block = 1u << le32(data + ivfc + 0x4C);
        uint64_t level3 = ivfc + ((0x60 + (uint64_t)master + block - 1) & ~(uint64_t)(block - 1));
        uint64_t level3_size;
        memcpy(&level3_size, data + ivfc + 0x44, 8);
        if (level3 + level3_size > size)
            return 0;
        *offset = level3;
        *length = level3_size;
    }
    return 1;
}

int fs_set_romfs(const char *path)
{
    /* Mapped, not read: a game's RomFS is often over a gigabyte. */
    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return 0;
    struct stat st;
    if (fstat(fd, &st) != 0 || st.st_size <= 0) {
        close(fd);
        return 0;
    }
    uint8_t *data = mmap(NULL, (size_t)st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    if (data == MAP_FAILED)
        return 0;
    uint64_t offset, length;
    if (!romfs_offset(data, (uint64_t)st.st_size, &offset, &length)) {
        munmap(data, (size_t)st.st_size);
        return 0;
    }
    romfs = data + offset;
    romfs_size = length;
    return 1;
}

/* UTF-16 or ASCII FS_Path -> UTF-8 host path relative to the archive root.
   Returns 0 for a path we refuse. */
static int path_to_utf8(uint32_t type, uint32_t size, uint32_t addr, char *out, size_t cap)
{
    const uint8_t *p = kernel_mem() + addr;
    size_t o = 0;
    if (type == PATH_EMPTY) {
        out[0] = 0;
        return 1;
    }
    if (type == PATH_ASCII) {
        for (uint32_t i = 0; i < size && p[i] && o + 1 < cap; i++)
            out[o++] = (char)p[i];
    } else if (type == PATH_UTF16) {
        for (uint32_t i = 0; i + 1 < size; i += 2) {
            uint32_t c = (uint32_t)p[i] | (uint32_t)p[i + 1] << 8;
            if (!c)
                break;
            if (c >= 0xD800 && c < 0xDC00 && i + 3 < size) {      /* surrogate pair */
                uint32_t lo = (uint32_t)p[i + 2] | (uint32_t)p[i + 3] << 8;
                c = 0x10000 + ((c - 0xD800) << 10) + (lo - 0xDC00);
                i += 2;
            }
            if (o + 5 >= cap)
                return 0;
            if (c < 0x80) {
                out[o++] = (char)c;
            } else if (c < 0x800) {
                out[o++] = (char)(0xC0 | c >> 6);
                out[o++] = (char)(0x80 | (c & 0x3F));
            } else if (c < 0x10000) {
                out[o++] = (char)(0xE0 | c >> 12);
                out[o++] = (char)(0x80 | (c >> 6 & 0x3F));
                out[o++] = (char)(0x80 | (c & 0x3F));
            } else {
                out[o++] = (char)(0xF0 | c >> 18);
                out[o++] = (char)(0x80 | (c >> 12 & 0x3F));
                out[o++] = (char)(0x80 | (c >> 6 & 0x3F));
                out[o++] = (char)(0x80 | (c & 0x3F));
            }
        }
    } else {
        return 0;
    }
    out[o] = 0;
    return strstr(out, "..") == NULL;
}

/* The host path of a path in archive `id`; 0 if the archive has no host
   folder or the path is refused. */
static int arch_path(const char *root, uint32_t type, uint32_t size, uint32_t addr, char *out, size_t cap)
{
    char rel[400];
    if (!root || !path_to_utf8(type, size, addr, rel, sizeof rel))
        return 0;
    snprintf(out, cap, "%s/%s", root, rel[0] == '/' ? rel + 1 : rel);
    return 1;
}

/* The host folder of an open archive handle (its low word), or NULL. */
static const char *archive_of(uint32_t lo)
{
    uint32_t i = lo - 0x100000u;
    return i < (uint32_t)narchives ? archives[i].root : NULL;
}

static void reply_handle(uint32_t *cmd, KObj *o)
{
    cmd[0] = ipc_header(cmd[0] >> 16, 1, 2);
    cmd[1] = RES_OK;
    cmd[2] = IPC_MOVE_HANDLES(1);
    cmd[3] = handle_new(o);
}

static void open_file(const char *root, uint32_t *cmd, uint32_t type, uint32_t size, uint32_t addr, uint32_t flags)
{
    File *f = calloc(1, sizeof *f);
    if (!f || !arch_path(root, type, size, addr, f->path, sizeof f->path)) {
        free(f);
        ipc_reply(cmd, root ? RES_FS_BAD_PATH : RES_FS_NOT_FOUND);
        return;
    }
    int mode = (flags & OPEN_WRITE) ? O_RDWR : O_RDONLY;
    if (flags & OPEN_CREATE)
        mode |= O_CREAT;
    f->fd = open(f->path, mode, 0644);
    if (f->fd < 0) {
        free(f);
        ipc_reply(cmd, RES_FS_NOT_FOUND);
        return;
    }
    reply_handle(cmd, session_new(&file_service, f));
}

static int name_cmp(const void *a, const void *b)
{
    return strcmp(*(char *const *)a, *(char *const *)b);
}

static void open_dir(const char *root, uint32_t *cmd, uint32_t type, uint32_t size, uint32_t addr)
{
    Dir *d = calloc(1, sizeof *d);
    DIR *h = NULL;
    if (!d || !arch_path(root, type, size, addr, d->path, sizeof d->path) || !(h = opendir(d->path))) {
        free(d);
        ipc_reply(cmd, RES_FS_NOT_FOUND);
        return;
    }
    for (struct dirent *e; (e = readdir(h));) {
        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0)
            continue;
        char **grown = realloc(d->names, (size_t)(d->n + 1) * sizeof *grown);
        if (!grown)
            break;
        d->names = grown;
        d->names[d->n++] = strdup(e->d_name);
    }
    closedir(h);
    qsort(d->names, (size_t)d->n, sizeof *d->names, name_cmp);    /* deterministic order */
    reply_handle(cmd, session_new(&dir_service, d));
}

static uint32_t priority;    /* SetPriority: remembered for GetPriority */

void fs_handle(KObj *session, uint32_t *cmd)
{
    (void)session;
    char path[512];
    switch (cmd[0] >> 16) {
    case 0x0862:    /* SetPriority(priority): FS request scheduling, nothing for us */
        priority = cmd[1];
        ipc_reply(cmd, RES_OK);
        return;
    case 0x0863:    /* GetPriority -> priority */
        cmd[0] = ipc_header(0x0863, 2, 0);
        cmd[1] = RES_OK;
        cmd[2] = priority;
        return;
    case 0x0801:    /* Initialize(process id descriptor) */
    case 0x0861:    /* InitializeWithSdkVersion */
        ipc_reply(cmd, RES_OK);
        return;
    case 0x080C: {  /* OpenArchive(id, path type, path size; path) -> u64 archive handle */
        uint32_t id = cmd[1];
        const char *root = archive_root_path(id, cmd[2], cmd[3], cmd[5]);
        if (id == ARCHIVE_SAVEDATA && !root) {
            ipc_reply(cmd, RES_FS_NOT_FORMATTED);      /* the app formats it, then opens it again */
            return;
        }
        if (id == ARCHIVE_EXTSAVEDATA && !root) {
            ipc_reply(cmd, RES_FS_NOT_FOUND);          /* the app creates it, then opens it again */
            return;
        }
        if (!root || narchives == MAX_ARCHIVES) {
            fprintf(stderr, "hle: FS OpenArchive(0x%" PRIx32 "): not available, returning not-found\n", id);
            ipc_reply(cmd, RES_FS_NOT_FOUND);
            return;
        }
        archives[narchives].id = id;
        snprintf(archives[narchives].root, sizeof archives[narchives].root, "%s", root);
        cmd[0] = ipc_header(0x80C, 3, 0);
        cmd[1] = RES_OK;
        cmd[2] = 0x100000u + (uint32_t)narchives++;
        cmd[3] = 0;
        return;
    }
    case 0x080E:    /* CloseArchive */
        ipc_reply(cmd, RES_OK);
        return;
    case 0x0802:    /* OpenFile(0, archive, path type, size, flags, attributes; path) */
        open_file(archive_of(cmd[2]), cmd, cmd[4], cmd[5], cmd[9], cmd[6]);
        return;
    case 0x0803: {  /* OpenFileDirectly(0, archive id, archive path type/size, file path type/size, flags, attrs) */
        uint32_t id = cmd[2];
        if (id == ARCHIVE_ROMFS) {
            if (!romfs) {
                fprintf(stderr, "hle: FS: the app opened its RomFS, but none was given (runner --romfs)\n");
                ipc_reply(cmd, RES_FS_NOT_FOUND);
                return;
            }
            File *f = calloc(1, sizeof *f);
            f->fd = -1;
            snprintf(f->path, sizeof f->path, "romfs");
            reply_handle(cmd, session_new(&file_service, f));
            return;
        }
        const char *root = archive_root_path(id, cmd[3], cmd[4], cmd[10]);
        if (root) {
            open_file(root, cmd, cmd[5], cmd[6], cmd[12], cmd[7]);
            return;
        }
        fprintf(stderr, "hle: FS OpenFileDirectly on archive 0x%" PRIx32 ": not available\n", id);
        ipc_reply(cmd, RES_FS_NOT_FOUND);
        return;
    }
    case 0x080B:    /* OpenDirectory(archive, path type, size; path) */
        open_dir(archive_of(cmd[1]), cmd, cmd[3], cmd[4], cmd[6]);
        return;
    case 0x0804:    /* DeleteFile(0, archive, path type, size; path) */
        if (!arch_path(archive_of(cmd[2]), cmd[4], cmd[5], cmd[7], path, sizeof path)) {
            ipc_reply(cmd, RES_FS_NOT_FOUND);
            return;
        }
        ipc_reply(cmd, unlink(path) == 0 ? RES_OK : RES_FS_NOT_FOUND);
        return;
    case 0x0805: {  /* RenameFile(0, src archive, type, size, dst archive, type, size; src, dst paths) */
        char to[512];
        if (!arch_path(archive_of(cmd[2]), cmd[4], cmd[5], cmd[11], path, sizeof path)
            || !arch_path(archive_of(cmd[6]), cmd[8], cmd[9], cmd[13], to, sizeof to)) {
            ipc_reply(cmd, RES_FS_NOT_FOUND);
            return;
        }
        ipc_reply(cmd, rename(path, to) == 0 ? RES_OK : RES_FS_NOT_FOUND);
        return;
    }
    case 0x0806:    /* DeleteDirectory(0, archive, path type, size; path) */
    case 0x0807:    /* DeleteDirectoryRecursively(0, archive, path type, size; path) */
        if (!arch_path(archive_of(cmd[2]), cmd[4], cmd[5], cmd[7], path, sizeof path)) {
            ipc_reply(cmd, RES_FS_NOT_FOUND);
            return;
        }
        if ((cmd[0] >> 16) == 0x0807)
            remove_tree(path);
        ipc_reply(cmd, (cmd[0] >> 16) == 0x0807 || rmdir(path) == 0 ? RES_OK : RES_FS_NOT_FOUND);
        return;
    case 0x080D:    /* ControlArchive(archive, action, ...): 0 = commit save data */
        if (cmd[3] != 0) {
            fprintf(stderr, "hle: FS ControlArchive action %" PRIu32 " is not implemented\n", cmd[3]);
            exit(3);
        }
        ipc_reply(cmd, RES_OK);                 /* our files are written as they go */
        return;
    case 0x0845: {  /* GetFormatInfo(archive id, path type, size; path) -> size, dirs, files, duplicate */
        SaveFormat fmt;
        if (cmd[1] == ARCHIVE_EXTSAVEDATA) {
            /* What CreateExtSaveData was given; "doesn't exist" before that. */
            const char *root = archive_root_path(cmd[1], cmd[2], cmd[3], cmd[5]);
            char fpath[600];
            snprintf(fpath, sizeof fpath, "%s.format", root ? root : "");
            if (!root || !format_read(fpath, &fmt)) {
                ipc_reply(cmd, RES_FS_NOT_FOUND);
                return;
            }
        } else if (cmd[1] != ARCHIVE_SAVEDATA || !save_format(&fmt)) {
            ipc_reply(cmd, cmd[1] == ARCHIVE_SAVEDATA ? RES_FS_NOT_FORMATTED : RES_FS_NOT_SUPPORTED);
            return;
        }
        cmd[0] = ipc_header(0x0845, 5, 0);
        cmd[1] = RES_OK;
        cmd[2] = fmt.total_size;
        cmd[3] = fmt.dirs;
        cmd[4] = fmt.files;
        cmd[5] = fmt.duplicate;
        return;
    }
    case 0x0851: {  /* CreateExtSaveData(info[4]: media, u64 id, -; dirs, files, u64 limit, SMDH size; SMDH) */
        char dir[512], icon[600];
        if (!save_dir) {
            fprintf(stderr, "hle: FS CreateExtSaveData: give the runner a folder for save data (--save DIR)\n");
            exit(3);
        }
        snprintf(dir, sizeof dir, "%s/extdata", save_dir);
        mkdir(dir, 0755);
        extdata_dir(cmd[2], cmd[3], dir, sizeof dir);
        if (mkdir(dir, 0755) != 0 && errno == EEXIST) {
            ipc_reply(cmd, RES_FS_EXISTS);
            return;
        }
        snprintf(icon, sizeof icon, "%s.icon", dir);            /* the SMDH, beside it */
        FILE *f = fopen(icon, "wb");
        if (f) {
            fwrite(kernel_mem() + cmd[11], 1, cmd[9], f);
            fclose(f);
        }
        SaveFormat fmt = { 0, cmd[5], cmd[6], 0 };              /* for GetFormatInfo */
        snprintf(icon, sizeof icon, "%s.format", dir);
        format_write(icon, &fmt);
        ipc_reply(cmd, RES_OK);
        return;
    }
    case 0x0852: {  /* DeleteExtSaveData(info[4]) */
        char dir[512];
        char side[600];
        extdata_dir(cmd[2], cmd[3], dir, sizeof dir);
        remove_tree(dir);
        snprintf(side, sizeof side, "%s.icon", dir);
        unlink(side);
        snprintf(side, sizeof side, "%s.format", dir);
        unlink(side);
        ipc_reply(cmd, RES_OK);
        return;
    }
    case 0x084C: {  /* FormatSaveData(archive id, path type, size, block size, dirs, files,
                       dir buckets, file buckets, duplicate; path) */
        if (cmd[1] != ARCHIVE_SAVEDATA || !save_dir) {
            fprintf(stderr, "hle: FS FormatSaveData: give the runner a folder for save data (--save DIR)\n");
            exit(3);
        }
        remove_tree(save_data_dir);
        mkdir(save_data_dir, 0755);
        SaveFormat fmt = { 0x100000, cmd[5], cmd[6], cmd[9] & 0xFF };
        char fpath[600];
        snprintf(fpath, sizeof fpath, "%s/format.bin", save_dir);
        ipc_reply(cmd, format_write(fpath, &fmt) ? RES_OK : RES_FS_NOT_FOUND);
        return;
    }
    case 0x0808: {  /* CreateFile(0, archive, path type, size, attributes, u64 file size; path) */
        if (!arch_path(archive_of(cmd[2]), cmd[4], cmd[5], cmd[10], path, sizeof path)) {
            ipc_reply(cmd, RES_FS_NOT_FOUND);
            return;
        }
        int fd = open(path, O_RDWR | O_CREAT | O_EXCL, 0644);
        if (fd < 0) {
            ipc_reply(cmd, errno == EEXIST ? RES_FS_EXISTS : RES_FS_NOT_FOUND);
            return;
        }
        int ok = ftruncate(fd, (off_t)((uint64_t)cmd[8] << 32 | cmd[7])) == 0;
        close(fd);
        ipc_reply(cmd, ok ? RES_OK : RES_FS_NOT_FOUND);
        return;
    }
    case 0x0809:    /* CreateDirectory(0, archive, path type, size, attributes; path) */
        if (!arch_path(archive_of(cmd[2]), cmd[4], cmd[5], cmd[8], path, sizeof path)) {
            ipc_reply(cmd, RES_FS_NOT_FOUND);
            return;
        }
        ipc_reply(cmd, mkdir(path, 0755) == 0 ? RES_OK : errno == EEXIST ? RES_FS_EXISTS : RES_FS_NOT_FOUND);
        return;
    default:
        ipc_unhandled("fs:USER", cmd);
    }
}

void fs_file_handle(KObj *session, uint32_t *cmd)
{
    File *f = session->data;
    uint64_t offset = (uint64_t)cmd[2] << 32 | cmd[1];
    switch (cmd[0] >> 16) {
    case 0x0802: {  /* Read(u64 offset, size; buffer) -> bytes read */
        uint32_t size = cmd[3], addr = cmd[5], got = 0;
        if (f->fd < 0) {
            if (offset < romfs_size) {
                got = (uint32_t)(romfs_size - offset < size ? romfs_size - offset : size);
                memcpy(kernel_mem() + addr, romfs + offset, got);
            }
        } else {
            ssize_t n = pread(f->fd, kernel_mem() + addr, size, (off_t)offset);
            got = n > 0 ? (uint32_t)n : 0;
        }
        cmd[0] = ipc_header(0x802, 2, 2);
        cmd[1] = RES_OK;
        cmd[2] = got;
        cmd[3] = size << 4 | 0xC;   /* the buffer descriptor, handed back */
        cmd[4] = addr;
        return;
    }
    case 0x0803: {  /* Write(u64 offset, size, flags; buffer) -> bytes written */
        uint32_t size = cmd[3], addr = cmd[6];
        ssize_t n = f->fd < 0 ? -1 : pwrite(f->fd, kernel_mem() + addr, size, (off_t)offset);
        cmd[0] = ipc_header(0x803, 2, 2);
        cmd[1] = n < 0 ? RES_FS_NOT_SUPPORTED : RES_OK;
        cmd[2] = n > 0 ? (uint32_t)n : 0;
        cmd[3] = size << 4 | 0xA;
        cmd[4] = addr;
        return;
    }
    case 0x0804: {  /* GetSize -> u64 */
        uint64_t size = romfs_size;
        struct stat st;
        if (f->fd >= 0)
            size = fstat(f->fd, &st) == 0 ? (uint64_t)st.st_size : 0;
        cmd[0] = ipc_header(0x804, 3, 0);
        cmd[1] = RES_OK;
        cmd[2] = (uint32_t)size;
        cmd[3] = (uint32_t)(size >> 32);
        return;
    }
    case 0x0805:    /* SetSize(u64) */
        ipc_reply(cmd, f->fd >= 0 && ftruncate(f->fd, (off_t)offset) == 0 ? RES_OK : RES_FS_NOT_SUPPORTED);
        return;
    case 0x0808:    /* Close */
        if (f->fd >= 0)
            close(f->fd);
        f->fd = -1;
        ipc_reply(cmd, RES_OK);
        return;
    case 0x0809:    /* Flush */
    case 0x0807:    /* SetAttributes(attributes): nothing to keep for us */
    case 0x080A:    /* SetPriority(priority): FS request scheduling, nothing for us */
        ipc_reply(cmd, RES_OK);
        return;
    case 0x0806:    /* GetAttributes -> 0 (not a directory, hidden, archive or read-only) */
    case 0x080B:    /* GetPriority -> 0 */
        cmd[0] = ipc_header(cmd[0] >> 16, 2, 0);
        cmd[1] = RES_OK;
        cmd[2] = 0;
        return;
    case 0x080C: {  /* OpenLinkFile -> another handle to the same file */
        File *g = malloc(sizeof *g);
        if (!g) {
            perror("OpenLinkFile");
            exit(1);
        }
        *g = *f;
        g->fd = f->fd >= 0 ? dup(f->fd) : -1;   /* its own descriptor: closing one leaves the other */
        cmd[0] = ipc_header(0x080C, 1, 2);
        cmd[1] = RES_OK;
        cmd[2] = IPC_MOVE_HANDLES(1);
        cmd[3] = handle_new(session_new(&file_service, g));
        return;
    }
    default:
        ipc_unhandled("FS file", cmd);
    }
}

/* FS_DirectoryEntry: UTF-16 name[0x106], 8.3 short name and extension,
   valid flag, attributes (bit 0: directory), u64 size. 0x228 bytes. */
static void dir_entry(uint8_t *e, const char *dir, const char *name)
{
    memset(e, 0, 0x228);
    size_t k = 0;
    for (const unsigned char *p = (const unsigned char *)name; *p && k < 0x105;) {
        uint32_t c = *p++;
        if (c >= 0xC0) {                /* UTF-8 -> code point */
            int extra = c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : 1;
            c &= 0x3Fu >> extra;
            while (extra-- && *p)
                c = c << 6 | (*p++ & 0x3Fu);
        }
        if (c >= 0x10000) {             /* surrogate pair */
            c -= 0x10000;
            uint16_t hi = (uint16_t)(0xD800 + (c >> 10)), lo = (uint16_t)(0xDC00 + (c & 0x3FF));
            memcpy(e + 2 * k++, &hi, 2);
            c = lo;
        }
        uint16_t u = (uint16_t)c;
        memcpy(e + 2 * k++, &u, 2);
    }
    for (int i = 0; i < 8 && name[i] && name[i] != '.'; i++)
        e[0x20C + i] = (uint8_t)(name[i] >= 'a' && name[i] <= 'z' ? name[i] - 32 : name[i]);
    e[0x21A] = 1;                       /* valid */
    char full[1024];
    snprintf(full, sizeof full, "%s/%s", dir, name);
    struct stat st;
    if (stat(full, &st) == 0) {
        uint32_t attr = S_ISDIR(st.st_mode) ? 1u : 0u;
        uint64_t size = S_ISDIR(st.st_mode) ? 0 : (uint64_t)st.st_size;
        memcpy(e + 0x21C, &attr, 4);
        memcpy(e + 0x220, &size, 8);
    }
}

void fs_dir_handle(KObj *session, uint32_t *cmd)
{
    Dir *d = session->data;
    switch (cmd[0] >> 16) {
    case 0x0801: {  /* Read(count; buffer) -> entries read */
        uint32_t count = cmd[1], addr = cmd[3], n = 0;
        while (n < count && d->next < d->n)
            dir_entry(kernel_mem() + addr + 0x228 * n++, d->path, d->names[d->next++]);
        cmd[0] = ipc_header(0x801, 2, 2);
        cmd[1] = RES_OK;
        cmd[2] = n;
        cmd[3] = (count * 0x228) << 4 | 0xC;
        cmd[4] = addr;
        return;
    }
    case 0x0802:    /* Close */
        ipc_reply(cmd, RES_OK);
        return;
    default:
        ipc_unhandled("FS directory", cmd);
    }
}
