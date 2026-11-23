#pragma once
#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <dirent.h>
#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#ifndef FBR_TEST_ROOT
#define FBR_TEST_ROOT "/tmp/feberis-test-sd"
#endif
#define EXT_PATH(x) FBR_TEST_ROOT "/" x
#define FSE_OK 0
#define FSE_EXIST 1
#define FSE_ERROR 2
#define FSAM_WRITE 1
#define FSAM_READ 2
#define FSAM_READ_WRITE 3
#define FSOM_CREATE_NEW 1
#define FSOM_CREATE_ALWAYS 2
#define FSOM_OPEN_EXISTING 3
#define FSOM_OPEN_APPEND 4
#define COUNT_OF(x) (sizeof(x) / sizeof((x)[0]))
typedef void FuriMutex;
typedef void FuriMessageQueue;
typedef void FuriStreamBuffer;
typedef void FuriThread;
typedef void FuriHalSerialHandle;
typedef enum { FuriHalSerialIdUsart, FuriHalSerialIdLpuart } FuriHalSerialId;
typedef int Storage;
typedef int FS_Error;
typedef struct {
    FILE* handle;
    DIR* directory;
    char directory_path[240];
    bool sync_ok, truncate_ok;
    size_t write_limit;
    int error;
} File;
typedef struct {
    unsigned year, month, day, hour, minute, second;
} DateTime;
static FuriHalSerialId acquired_port;
static FuriHalSerialHandle* furi_hal_serial_control_acquire(FuriHalSerialId port) {
    acquired_port = port;
    return (FuriHalSerialHandle*)1;
}
static uint32_t ticks;
static char transmissions[1024][272];
static unsigned tx_count;
static unsigned sync_count, sync_at_tx[1024];
static uint32_t furi_get_tick(void) { return ticks; }
static uint32_t furi_ms_to_ticks(uint32_t ms) { return ms; }
static void furi_hal_serial_tx(void* h, const uint8_t* data, size_t size) {
    (void)h;
    if(tx_count < 1024) {
        memcpy(transmissions[tx_count], data, size);
        transmissions[tx_count][size] = 0;
        sync_at_tx[tx_count++] = sync_count;
    }
}
static File* storage_file_alloc(Storage* s) {
    (void)s;
    File* f = calloc(1, sizeof(File));
    f->sync_ok = f->truncate_ok = true;
    f->write_limit = SIZE_MAX;
    return f;
}
static bool storage_file_sync(File* f) {
    ++sync_count;
    return f->handle && !fflush(f->handle) && f->sync_ok;
}
static bool storage_file_close(File* f) {
    if(!f->handle) return true;
    bool ok = !fclose(f->handle);
    f->handle = NULL;
    return ok;
}
static void storage_file_free(File* f) {
    storage_file_close(f);
    free(f);
}
static bool storage_file_seek(File* f, uint32_t offset, bool from_start) {
    return f->handle && !fseek(f->handle, offset, from_start ? SEEK_SET : SEEK_CUR);
}
static bool storage_file_truncate(File* f) {
    return f->handle && f->truncate_ok && !fflush(f->handle) &&
           !ftruncate(fileno(f->handle), ftell(f->handle));
}
static uint64_t storage_file_tell(File* f) { return ftell(f->handle); }
static uint64_t storage_file_size(File* f) {
    long at = ftell(f->handle);
    fseek(f->handle, 0, SEEK_END);
    long size = ftell(f->handle);
    fseek(f->handle, at, SEEK_SET);
    return size;
}
static size_t storage_file_write(File* f, const void* data, size_t size) {
    if(size > f->write_limit) size = f->write_limit;
    return fwrite(data, 1, size, f->handle);
}
static size_t storage_file_read(File* f, void* data, size_t size) {
    size_t n = fread(data, 1, size, f->handle);
    f->error = ferror(f->handle) ? FSE_ERROR : FSE_OK;
    return n;
}
static int storage_common_mkdir(Storage* s, const char* path) {
    (void)s;
    return !mkdir(path, 0700) ? FSE_OK : errno == EEXIST ? FSE_EXIST : FSE_ERROR;
}
static bool storage_simply_mkdir(Storage* s, const char* path) {
    int error = storage_common_mkdir(s, path);
    return error == FSE_OK || error == FSE_EXIST;
}
static bool storage_file_open(File* f, const char* p, int mode, int create) {
    (void)mode;
    if(f->handle) return false;
    const char* m = create == FSOM_CREATE_NEW      ? "w+bx"
                    : create == FSOM_CREATE_ALWAYS ? "w+b"
                    : create == FSOM_OPEN_APPEND   ? "a+b"
                                                   : "r+b";
    f->handle = fopen(p, m);
    f->error = f->handle ? FSE_OK : errno == EEXIST ? FSE_EXIST : FSE_ERROR;
    return f->handle != NULL;
}
static FS_Error storage_file_get_error(File* f) { return f->error; }
static FS_Error storage_common_rename(Storage* s, const char* from, const char* to) {
    (void)s;
    return rename(from, to) ? FSE_ERROR : FSE_OK;
}
static void furi_hal_rtc_get_datetime(DateTime* d) { *d = (DateTime){2026, 11, 23, 18, 30, 0}; }

#define FSF_DIRECTORY 1
typedef struct {
    unsigned flags;
} FileInfo;
static bool storage_dir_open(File* file, const char* path) {
    snprintf(file->directory_path, sizeof(file->directory_path), "%s", path);
    file->directory = opendir(path);
    return file->directory != NULL;
}
static bool storage_dir_read(File* file, FileInfo* info, char* name, uint16_t size) {
    struct dirent* entry = readdir(file->directory);
    if(!entry) return false;
    snprintf(name, size, "%s", entry->d_name);
    char path[512];
    snprintf(path, sizeof(path), "%s/%s", file->directory_path, entry->d_name);
    struct stat st;
    assert(stat(path, &st) == 0);
    info->flags = S_ISDIR(st.st_mode) ? FSF_DIRECTORY : 0;
    return true;
}
static bool storage_dir_close(File* file) {
    bool ok = !closedir(file->directory);
    file->directory = NULL;
    return ok;
}
