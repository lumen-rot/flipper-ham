/* Fault-injected storage adapter: exercise the production save/recovery code. */
#include "../ham/profile_store.h"
#include <assert.h>
#include <setjmp.h>
#include <stdio.h>
#include <string.h>

typedef struct { bool exists; char bytes[2048]; size_t size; } Entry;
struct Storage { Entry entries[4]; };
struct File { Storage* storage; unsigned slot; size_t offset; bool used; };
static File handles[8];
static jmp_buf power_loss;
static unsigned operations, interrupt_at, fail_at;
static bool partial_write, corrupt_copy, fail_sync, fail_close;
static const char old_text[] = "activity=25\nradius=600\nduration_seconds=600\n";
static const char new_text[] = "activity=75\nradius=500\nduration_seconds=0\n";

static bool step(void) {
    ++operations;
    if(interrupt_at && operations == interrupt_at) longjmp(power_loss, 1);
    return fail_at && operations == fail_at;
}
static unsigned slot(const char* path) {
    const char* paths[] = {HAM_PROFILE, HAM_PROFILE_TEMP, HAM_PROFILE_BACKUP, HAM_PROFILE_PENDING};
    for(unsigned i = 0; i < 4; ++i) if(!strcmp(path, paths[i])) return i;
    assert(false); return 0;
}
File* storage_file_alloc(Storage* storage) {
    for(unsigned i = 0; i < 8; ++i) if(!handles[i].used) {
        handles[i] = (File){.storage=storage, .used=true}; return &handles[i];
    }
    assert(false); return NULL;
}
void storage_file_free(File* file) { file->used = false; }
bool storage_file_open(File* file, const char* path, FS_AccessMode access, FS_OpenMode mode) {
    (void)access;
    if(step()) return false;
    file->slot = slot(path); file->offset = 0;
    Entry* entry = &file->storage->entries[file->slot];
    if(mode == FSOM_CREATE_ALWAYS) *entry = (Entry){.exists=true};
    return entry->exists;
}
bool storage_file_close(File* file) { (void)file; return !step() && !fail_close; }
bool storage_file_sync(File* file) { (void)file; return !step() && !fail_sync; }
uint64_t storage_file_size(File* file) {
    return step() ? UINT64_MAX : file->storage->entries[file->slot].size;
}
size_t storage_file_read(File* file, void* buffer, size_t length) {
    if(step()) return 0;
    Entry* entry = &file->storage->entries[file->slot];
    if(length > entry->size - file->offset) length = entry->size - file->offset;
    memcpy(buffer, entry->bytes + file->offset, length); file->offset += length;
    return length;
}
size_t storage_file_write(File* file, const void* buffer, size_t length) {
    if(step()) return 0;
    if(partial_write) length /= 2;
    Entry* entry = &file->storage->entries[file->slot];
    assert(file->offset + length < sizeof(entry->bytes));
    memcpy(entry->bytes + file->offset, buffer, length); file->offset += length;
    entry->size = file->offset;
    return length;
}
FS_Error storage_common_stat(Storage* storage, const char* path, void* info) {
    (void)info;
    if(step()) return FSE_INTERNAL;
    return storage->entries[slot(path)].exists ? FSE_OK : FSE_NOT_EXIST;
}
FS_Error storage_common_remove(Storage* storage, const char* path) {
    if(step()) return FSE_INTERNAL;
    Entry* entry = &storage->entries[slot(path)];
    if(!entry->exists) return FSE_NOT_EXIST;
    entry->exists = false; return FSE_OK;
}
FS_Error storage_common_copy(Storage* storage, const char* source, const char* target) {
    if(step()) return FSE_INTERNAL;
    Entry* from = &storage->entries[slot(source)];
    Entry* to = &storage->entries[slot(target)];
    if(!from->exists || to->exists) return FSE_INTERNAL;
    *to = *from;
    if(corrupt_copy && to->size) --to->size;
    return FSE_OK;
}
FS_Error storage_common_rename(Storage* storage, const char* source, const char* target) {
    /* Match Flipper's non-atomic remove/copy/delete behavior. */
    if(step()) return FSE_INTERNAL;
    if(!storage->entries[slot(source)].exists) return FSE_NOT_EXIST;
    FS_Error result = storage_common_remove(storage, target);
    if(result != FSE_OK && result != FSE_NOT_EXIST) return result;
    result = storage_common_copy(storage, source, target);
    if(result == FSE_OK) result = storage_common_remove(storage, source);
    return result;
}
static void reset(Storage* storage) {
    memset(storage, 0, sizeof(*storage)); memset(handles, 0, sizeof(handles));
    operations = interrupt_at = fail_at = 0;
    partial_write = corrupt_copy = fail_sync = fail_close = false;
    storage->entries[0].exists = true;
    strcpy(storage->entries[0].bytes, old_text);
    storage->entries[0].size = strlen(old_text);
}
static bool equals(Entry* entry, const char* text) {
    return entry->exists && entry->size == strlen(text) &&
           !memcmp(entry->bytes, text, entry->size);
}
static void check_recovery(Storage* storage) {
    interrupt_at = fail_at = 0;
    partial_write = corrupt_copy = fail_sync = fail_close = false;
    memset(handles, 0, sizeof(handles));
    assert(ham_profile_recover(storage));
    assert(equals(&storage->entries[0], old_text) || equals(&storage->entries[0], new_text));
    assert(!strcmp(ham_profile_source(storage), HAM_PROFILE));
    assert(ham_profile_store(storage, new_text));
    assert(equals(&storage->entries[0], new_text));
    for(unsigned i = 0; i < 8; ++i) assert(!handles[i].used);
}
int main(void) {
    static Storage storage; /* survives simulated power loss via longjmp */
    reset(&storage); assert(ham_profile_store(&storage, new_text));
    assert(equals(&storage.entries[0], new_text));
    unsigned count = operations;
    for(unsigned i = 1; i <= count; ++i) {
        reset(&storage); fail_at = i;
        ham_profile_store(&storage, new_text);
        check_recovery(&storage);
    }
    for(unsigned i = 1; i <= count; ++i) {
        reset(&storage); interrupt_at = i;
        if(setjmp(power_loss) == 0) ham_profile_store(&storage, new_text);
        check_recovery(&storage);
    }
    for(unsigned failure = 0; failure < 4; ++failure) {
        reset(&storage);
        partial_write = failure == 0; corrupt_copy = failure == 1;
        fail_sync = failure == 2; fail_close = failure == 3;
        assert(!ham_profile_store(&storage, new_text));
        assert(equals(&storage.entries[0], old_text));
        check_recovery(&storage);
    }
    /* A partial backup without a marker must never replace the original. */
    reset(&storage); storage.entries[2] = (Entry){.exists=true,.size=1,.bytes="x"};
    assert(ham_profile_recover(&storage)); assert(equals(&storage.entries[0], old_text));
    /* Missing backup must not delete the remaining profile during recovery. */
    reset(&storage); storage.entries[3].exists = true;
    assert(!ham_profile_recover(&storage)); assert(equals(&storage.entries[0], old_text));
    /* Recovery itself may lose power or encounter errors; backup remains usable. */
    for(unsigned i = 1; i <= count; ++i) {
        reset(&storage); storage.entries[2] = storage.entries[0];
        storage.entries[3].exists = true; storage.entries[0].size = 1;
        fail_at = i; ham_profile_recover(&storage); check_recovery(&storage);
        reset(&storage); storage.entries[2] = storage.entries[0];
        storage.entries[3].exists = true; storage.entries[0].size = 1;
        interrupt_at = i;
        if(setjmp(power_loss) == 0) ham_profile_recover(&storage);
        check_recovery(&storage);
    }
    printf("Profile storage passed: failures and interruptions at %u save operations, recovery retries, short writes, corrupt copies, sync/close errors.\n", count);
}
