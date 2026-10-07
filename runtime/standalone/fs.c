/* Synchronous Cafe FS over explicit host roots. ABI reference: Cemu
 * coreinit_FS.cpp (FSOpenFile, FSReadFile, FSReadFileWithPos, FSGetStat,
 * FSInitCmdBlock, FSSetUserData), fsa_types.h (FS_RESULT, FSStat_t).
 * Content is read-only; update files override base files. Linux openat2 keeps
 * all guest paths beneath the configured root and disallows symlink traversal.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <dirent.h>
#include <fcntl.h>
#include <linux/openat2.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/syscall.h>
#include <unistd.h>
#include "rt.h"

#define A(n) c->r[3 + (n)]
#define RET(v) (c->r[3] = (uint32_t)(v))
static int base_root = -1, update_root = -1, save_root = -1;
static struct { int used, fd; } files[256];
enum { FS_OK = 0, FS_FULL = -3, FS_EXISTS = -5, FS_MISSING = -6,
       FS_NOT_FILE = -7, FS_NOT_DIR = -8, FS_DENIED = -10, FS_FATAL = -1024 };
static int error(void) {
    switch (errno) {
    case ENOENT: return FS_MISSING;
    case EEXIST: return FS_EXISTS;
    case EISDIR: return FS_NOT_FILE;
    case ENOTDIR: return FS_NOT_DIR;
    case EACCES: case EPERM: case ELOOP: case EXDEV: case EROFS: return FS_DENIED;
    case EMFILE: case ENFILE: return FS_FULL;
    default: return FS_FATAL;
    }
}
static int open_root(const char *path) {
    if (!path) return -1;
    int fd = open(path, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd < 0) { perror(path); exit(1); }
    return fd;
}
void hle_fs_configure(const char *base, const char *update, const char *save) {
    base_root = open_root(base); update_root = open_root(update); save_root = open_root(save);
}
static int beneath(int root, const char *path, int flags) {
    if (root < 0) { errno = ENOENT; return -1; }
    struct open_how how = {.flags = flags | O_CLOEXEC,
                          .mode = flags & O_CREAT ? 0600 : 0,
                          .resolve = RESOLVE_BENEATH | RESOLVE_NO_SYMLINKS};
    return (int)syscall(SYS_openat2, root, *path ? path : ".", &how, sizeof how);
}
static const char *gstr(uint32_t addr) { return (const char *)(g_mem + addr); }
static int guest_open(const char *path, int flags) {
    if (!strncmp(path, "/vol/content", 12) && (!path[12] || path[12] == '/')) {
        if ((flags & O_ACCMODE) != O_RDONLY || flags & (O_CREAT | O_TRUNC | O_APPEND)) {
            errno = EACCES; return -1;
        }
        const char *rel = path + 12; while (*rel == '/') rel++;
        int fd = beneath(update_root, rel, flags);
        if (fd >= 0 || errno != ENOENT) return fd;
        return beneath(base_root, rel, flags);
    }
    if (!strncmp(path, "/vol/save", 9) && (!path[9] || path[9] == '/')) {
        const char *rel = path + 9; while (*rel == '/') rel++;
        return beneath(save_root, rel, flags);
    }
    errno = EACCES; return -1;
}
static int file_fd(uint32_t handle) {
    return handle && handle <= 256 && files[handle - 1].used ? files[handle - 1].fd : -1;
}
static void FSInit(ppc_ctx *c) { RET(0); }
static void FSAddClient(ppc_ctx *c) { memset(g_mem + A(0), 0, 5888); RET(0); }
static void FSDelClient(ppc_ctx *c) { RET(0); }
static void FSInitCmdBlock(ppc_ctx *c) {
    uint32_t a = A(0), body = (a + 63) & ~63u;
    memset(g_mem + a, 0, 2688); st32(body + 0x93c, 0x0d900a21);
    st8(body + 0x9e8, 16); st32(body + 0x9f8, a);
}
static void FSSetUserData(ppc_ctx *c) { st32(((A(0) + 63) & ~63u) + 0x994, A(1)); }
static void FSGetUserData(ppc_ctx *c) { RET(ld32(((A(0) + 63) & ~63u) + 0x994)); }
static void FSOpenFilePath(ppc_ctx *c, const char *path) {
    const char *mode = gstr(A(3));
    int flags;
    if (*mode == 'r') flags = strchr(mode, '+') ? O_RDWR : O_RDONLY;
    else if (*mode == 'w') flags = (strchr(mode, '+') ? O_RDWR : O_WRONLY) | O_CREAT | O_TRUNC;
    else if (*mode == 'a') flags = (strchr(mode, '+') ? O_RDWR : O_WRONLY) | O_CREAT | O_APPEND;
    else { RET(FS_FATAL); return; }
    unsigned i;
    for (i = 0; i < 256 && files[i].used; i++) {}
    if (i == 256) { RET(FS_FULL); return; }
    int fd = guest_open(path, flags);
    if(getenv("WIIU_TRACE_FS"))fprintf(stderr,"FS open %s (%s): %d\n",path,mode,fd<0?error():0);
    if (fd < 0) { RET(error()); return; }
    struct stat st;
    if (fstat(fd, &st)) { int e = error(); close(fd); RET(e); return; }
    if (!S_ISREG(st.st_mode)) { close(fd); RET(FS_NOT_FILE); return; }
    files[i].used = 1; files[i].fd = fd; st32(A(4), i + 1); RET(0);
}
static void FSOpenFile(ppc_ctx *c) { FSOpenFilePath(c, gstr(A(2))); }
static void FSCloseFile(ppc_ctx *c) {
    uint32_t handle = A(2); int fd = file_fd(handle);
    if (fd < 0) { RET(FS_FATAL); return; }
    files[handle - 1].used = 0; RET(close(fd) ? error() : FS_OK);
}
static void transfer(ppc_ctx *c, int writing, int positioned) {
    uint32_t buffer = A(2), size = A(3), count = A(4);
    int fd = file_fd(A(positioned ? 6 : 5));
    if (fd < 0) { RET(FS_FATAL); return; }
    uint64_t bytes = (uint64_t)size * count;
    if (bytes > UINT32_MAX || bytes > (1ull << 32) - buffer) { RET(FS_FATAL); return; }
    if (!size || !count) { RET(0); return; }
    if (positioned && lseek(fd, A(5), SEEK_SET) < 0) { RET(error()); return; }
    size_t done = 0;
    while (done < bytes) {
        ssize_t n = writing ? write(fd, g_mem + buffer + done, bytes - done)
                            : read(fd, g_mem + buffer + done, bytes - done);
        if (n < 0) { if (errno == EINTR) continue; RET(error()); return; }
        if (!n) break;
        done += (size_t)n;
    }
    RET(done / size);
}
static void FSReadFile(ppc_ctx *c) { transfer(c, 0, 0); }
static void FSReadFileWithPos(ppc_ctx *c) { transfer(c, 0, 1); }
static void FSWriteFile(ppc_ctx *c) { transfer(c, 1, 0); }
static void FSSetPosFile(ppc_ctx *c) {
    int fd = file_fd(A(2)); RET(fd < 0 ? FS_FATAL : lseek(fd, A(3), SEEK_SET) < 0 ? error() : 0);
}
static void FSGetPosFile(ppc_ctx *c) {
    int fd = file_fd(A(2));
    if (fd < 0) { RET(FS_FATAL); return; }
    off_t pos = lseek(fd, 0, SEEK_CUR);
    if (pos < 0) { RET(error()); return; }
    st32(A(3), (uint32_t)pos); RET(0);
}
static void FSFlushFile(ppc_ctx *c) {
    int fd = file_fd(A(2)); RET(fd < 0 ? FS_FATAL : fsync(fd) ? error() : 0);
}
static void write_stat(uint32_t out, const struct stat *st) {
    /* Cemu iosu_fsa.cpp::__FSA_GetStatFromFSCFile: directory size is zero
     * and Cafe permission nibbles are 0x777 (not POSIX octal 0777). */
    memset(g_mem + out, 0, 0x64);
    st32(out, S_ISDIR(st->st_mode) ? 0x80000000 : 0x01000000);
    st32(out + 4, 0x777);
    st32(out + 0x10, S_ISDIR(st->st_mode) ? 0 : (uint32_t)st->st_size);
}
static void FSGetStatPath(ppc_ctx *c, const char *path) {
    int fd = guest_open(path, O_RDONLY);
    if (fd < 0) { RET(error()); return; }
    struct stat st; int result = fstat(fd, &st) ? error() : 0; close(fd);
    if (!result) write_stat(A(3), &st);
    RET(result);
}
static void FSGetStat(ppc_ctx *c) { FSGetStatPath(c, gstr(A(2))); }
static void FSGetStatFile(ppc_ctx *c) {
    int fd = file_fd(A(2));
    if (fd < 0) { RET(FS_FATAL); return; }
    struct stat st; int result = fstat(fd, &st) ? error() : 0;
    if (!result) write_stat(A(3), &st);
    RET(result);
}
typedef struct { char name[128]; struct stat stat; } directory_entry;
static struct { int used; directory_entry *entries; size_t count, index; } dirs[64];
static int entry_compare(const void *a, const void *b) {
    return strcmp(((const directory_entry *)a)->name, ((const directory_entry *)b)->name);
}
static int collect_dir(int fd, directory_entry **entries, size_t *count) {
    DIR *dir = fdopendir(fd);
    if (!dir) { int e = error(); close(fd); return e; }
    struct dirent *ent;
    errno = 0;
    while ((ent = readdir(dir))) {
        if (!strcmp(ent->d_name, ".") || !strcmp(ent->d_name, "..")) continue;
        size_t i;
        for (i = 0; i < *count; i++) if (!strcmp((*entries)[i].name, ent->d_name)) break;
        if (i < *count) continue; /* update wins */
        if (strlen(ent->d_name) >= 128) { closedir(dir); return FS_FATAL; }
        directory_entry item = {0};
        if (fstatat(fd, ent->d_name, &item.stat, AT_SYMLINK_NOFOLLOW)) {
            int e = error(); closedir(dir); return e;
        }
        if (S_ISLNK(item.stat.st_mode)) continue;
        strcpy(item.name, ent->d_name);
        directory_entry *next = realloc(*entries, (*count + 1) * sizeof *next);
        if (!next) { closedir(dir); return FS_FATAL; }
        *entries = next; (*entries)[(*count)++] = item;
        errno = 0;
    }
    int result = errno ? error() : 0;
    closedir(dir); return result;
}
static void FSOpenDir(ppc_ctx *c) {
    unsigned i;
    for (i = 0; i < 64 && dirs[i].used; i++) {}
    if (i == 64) { RET(FS_FULL); return; }
    const char *path = gstr(A(2));
    int fd = guest_open(path, O_RDONLY | O_DIRECTORY);
    if (fd < 0) { RET(error()); return; }
    directory_entry *entries = NULL; size_t count = 0;
    int result = collect_dir(fd, &entries, &count);
    if (!result && !strncmp(path, "/vol/content", 12) && (!path[12] || path[12] == '/')) {
        const char *rel = path + 12; while (*rel == '/') rel++;
        fd = beneath(base_root, rel, O_RDONLY | O_DIRECTORY);
        if (fd >= 0) result = collect_dir(fd, &entries, &count);
        else if (errno != ENOENT && errno != ENOTDIR) result = error();
    }
    if (result) { free(entries); RET(result); return; }
    if (count) qsort(entries, count, sizeof *entries, entry_compare);
    dirs[i].used = 1; dirs[i].entries = entries; dirs[i].count = count; dirs[i].index = 0;
    st32(A(3), 0x10000 + i); RET(0);
}
static void FSReadDir(ppc_ctx *c) {
    uint32_t i = A(2) - 0x10000;
    if (i >= 64 || !dirs[i].used) { RET(FS_FATAL); return; }
    if (dirs[i].index == dirs[i].count) { RET(-2); return; }
    directory_entry *entry = &dirs[i].entries[dirs[i].index++];
    write_stat(A(3), &entry->stat);
    memset(g_mem + A(3) + 0x64, 0, 128);
    strcpy((char *)g_mem + A(3) + 0x64, entry->name); RET(0);
}
static void FSCloseDir(ppc_ctx *c) {
    uint32_t i = A(2) - 0x10000;
    if (i >= 64 || !dirs[i].used) { RET(FS_FATAL); return; }
    free(dirs[i].entries); memset(&dirs[i], 0, sizeof dirs[i]); RET(0);
}
static int save_parent(const char *path, char **leaf) {
    if (strncmp(path, "/vol/save/", 10)) { errno = EACCES; return -1; }
    char *rel = strdup(path + 10);
    if (!rel) { errno = ENOMEM; return -1; }
    char *slash = strrchr(rel, '/');
    *leaf = strdup(slash ? slash + 1 : rel);
    if (slash) *slash = 0;
    if (!*leaf || !**leaf || !strcmp(*leaf, ".") || !strcmp(*leaf, "..")) {
        free(rel); free(*leaf); *leaf = NULL; errno = EACCES; return -1;
    }
    int fd = beneath(save_root, slash ? rel : ".", O_RDONLY | O_DIRECTORY);
    free(rel); return fd;
}
static void FSMakeDirPath(ppc_ctx *c, const char *path) {
    char *leaf = NULL; int fd = save_parent(path, &leaf);
    int result = fd < 0 ? error() : mkdirat(fd, leaf, 0700) ? error() : 0;
    if (fd >= 0) close(fd);
    free(leaf); RET(result);
}
static void FSMakeDir(ppc_ctx *c) { FSMakeDirPath(c, gstr(A(2))); }
static void FSRemovePath(ppc_ctx *c, const char *path) {
    char *leaf = NULL; int fd = save_parent(path, &leaf);
    int result = fd < 0 ? error() : 0;
    if (fd >= 0) {
        if (unlinkat(fd, leaf, 0)) {
            if (errno == EISDIR) result = unlinkat(fd, leaf, AT_REMOVEDIR) ? error() : 0;
            else result = error();
        }
        close(fd);
    }
    free(leaf); RET(result);
}
static void FSRemove(ppc_ctx *c) { FSRemovePath(c, gstr(A(2))); }
hle_fn hle_fs_lookup(const char *name) {
#define EXPORT(n) if (!strcmp(name, #n)) return n
    EXPORT(FSInit); EXPORT(FSAddClient); EXPORT(FSDelClient); EXPORT(FSInitCmdBlock);
    EXPORT(FSSetUserData); EXPORT(FSGetUserData); EXPORT(FSOpenFile); EXPORT(FSCloseFile);
    EXPORT(FSReadFile); EXPORT(FSReadFileWithPos); EXPORT(FSWriteFile);
    EXPORT(FSSetPosFile); EXPORT(FSGetPosFile); EXPORT(FSFlushFile);
    EXPORT(FSGetStat); EXPORT(FSGetStatFile); EXPORT(FSMakeDir); EXPORT(FSRemove);
    EXPORT(FSOpenDir); EXPORT(FSReadDir); EXPORT(FSCloseDir);
    return NULL;
#undef EXPORT
}

/* Cemu nn_save.cpp::GetAbsoluteFullPath / SAVEOpenFile: SAVE wraps coreinit
 * FS with a persistent account prefix. One local account uses slot 1. */
static const char *account_dir(uint32_t slot) {
    return slot==1 ? "80000001" : slot==255 ? "common" : NULL;
}
static void SAVEInitSaveDir(ppc_ctx *c) {
    const char *dir=account_dir(A(0));
    if(!dir||save_root<0){RET(FS_MISSING);return;}
    int result=mkdirat(save_root,dir,0700);
    if(result&&errno!=EEXIST){RET(error());return;}
    int fd=beneath(save_root,dir,O_RDONLY|O_DIRECTORY);
    if(fd<0){RET(error());return;}close(fd);RET(0);
}
static void save_path_call(ppc_ctx *c,void (*fn)(ppc_ctx*,const char*)) {
    const char *dir=account_dir(A(2)),*rel=gstr(A(3));char path[1024];
    if(!dir){RET(FS_MISSING);return;}
    while(*rel=='/')rel++;
    for(const char *p=rel;*p;){const char *end=strchr(p,'/');size_t n=end?(size_t)(end-p):strlen(p);
        if(n==2&&!strncmp(p,"..",2)){RET(FS_DENIED);return;}if(!end)break;p=end+1;}
    if(snprintf(path,sizeof path,"/vol/save/%s/%s",dir,rel)>=(int)sizeof path){RET(FS_FATAL);return;}
    ppc_ctx call={0};memcpy(call.r,c->r,sizeof call.r);
    for(unsigned i=5;i<10;i++)call.r[i]=c->r[i+1];
    fn(&call,path);RET(call.r[3]);
}
static void SAVEOpenFile(ppc_ctx *c){save_path_call(c,FSOpenFilePath);}
static void SAVEGetStat(ppc_ctx *c){save_path_call(c,FSGetStatPath);}
static void SAVEMakeDir(ppc_ctx *c){save_path_call(c,FSMakeDirPath);}
static void SAVERemove(ppc_ctx *c){save_path_call(c,FSRemovePath);}
static void SAVEGetFreeSpaceSize(ppc_ctx *c){
    if(!account_dir(A(2))||save_root<0){RET(FS_MISSING);return;}
    struct statvfs s;if(fstatvfs(save_root,&s)){RET(error());return;}
    st64(A(3),(uint64_t)s.f_bavail*s.f_frsize);RET(0);
}
static void SAVEFlushQuota(ppc_ctx *c){
    if(!account_dir(A(2))||save_root<0){RET(FS_MISSING);return;}
    for(unsigned i=0;i<256;i++)if(files[i].used&&fsync(files[i].fd)){RET(error());return;}
    RET(fsync(save_root)?error():0);
}
hle_fn hle_save_lookup(const char *name){
#define E(n) if(!strcmp(name,#n))return n
    E(SAVEInitSaveDir);E(SAVEOpenFile);E(SAVEGetStat);E(SAVEMakeDir);E(SAVERemove);
    E(SAVEGetFreeSpaceSize);E(SAVEFlushQuota);
    if(!strcmp(name,"SAVEInit")||!strcmp(name,"SAVEShutdown"))return FSInit;
    return NULL;
#undef E
}
