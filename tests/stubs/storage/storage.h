#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
typedef struct Storage Storage;
typedef struct File File;
typedef enum { FSE_OK, FSE_NOT_EXIST, FSE_INTERNAL } FS_Error;
typedef enum { FSAM_READ, FSAM_WRITE, FSAM_READ_WRITE } FS_AccessMode;
typedef enum { FSOM_OPEN_EXISTING, FSOM_CREATE_ALWAYS } FS_OpenMode;
File* storage_file_alloc(Storage* storage);
void storage_file_free(File* file);
bool storage_file_open(File* file, const char* path, FS_AccessMode access, FS_OpenMode mode);
bool storage_file_close(File* file);
bool storage_file_sync(File* file);
uint64_t storage_file_size(File* file);
size_t storage_file_read(File* file, void* buffer, size_t length);
size_t storage_file_write(File* file, const void* buffer, size_t length);
FS_Error storage_common_stat(Storage* storage, const char* path, void* info);
FS_Error storage_common_remove(Storage* storage, const char* path);
FS_Error storage_common_copy(Storage* storage, const char* source, const char* target);
FS_Error storage_common_rename(Storage* storage, const char* source, const char* target);
