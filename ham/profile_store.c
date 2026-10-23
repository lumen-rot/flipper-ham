#include "profile_store.h"
#include <string.h>

static bool remove_if_present(Storage* storage, const char* path) {
    FS_Error error = storage_common_remove(storage, path);
    return error == FSE_OK || error == FSE_NOT_EXIST;
}

static bool matches(Storage* storage, const char* path, const char* contents) {
    File* file = storage_file_alloc(storage);
    size_t length = strlen(contents), offset = 0;
    bool ok = storage_file_open(file, path, FSAM_READ, FSOM_OPEN_EXISTING);
    if(ok) {
        ok = storage_file_size(file) == length;
        char buffer[64];
        while(ok && offset < length) {
            size_t count = length - offset;
            if(count > sizeof(buffer)) count = sizeof(buffer);
            size_t read = storage_file_read(file, buffer, count);
            ok = read == count && memcmp(buffer, contents + offset, count) == 0;
            offset += read;
        }
        ok = storage_file_close(file) && ok;
    }
    storage_file_free(file);
    return ok;
}

static bool write_verified(Storage* storage, const char* path, const char* contents) {
    File* file = storage_file_alloc(storage);
    size_t length = strlen(contents);
    bool ok = storage_file_open(file, path, FSAM_WRITE, FSOM_CREATE_ALWAYS);
    if(ok) {
        ok = storage_file_write(file, contents, length) == length;
        if(ok) ok = storage_file_sync(file);
        ok = storage_file_close(file) && ok;
    }
    storage_file_free(file);
    return ok && matches(storage, path, contents);
}

/* Check copied bytes as well as the API result before allowing a backup to be used. */
static bool copy_verified(Storage* storage, const char* source, const char* target) {
    if(storage_common_stat(storage, source, NULL) != FSE_OK) return false;
    if(!remove_if_present(storage, target)) return false;
    if(storage_common_copy(storage, source, target) != FSE_OK) return false;
    File* from = storage_file_alloc(storage);
    File* to = storage_file_alloc(storage);
    bool from_open = storage_file_open(from, source, FSAM_READ, FSOM_OPEN_EXISTING);
    bool to_open = storage_file_open(to, target, FSAM_READ_WRITE, FSOM_OPEN_EXISTING);
    bool ok = from_open && to_open;
    if(ok) {
        uint64_t remaining = storage_file_size(from);
        ok = remaining == storage_file_size(to);
        char a[64], b[64];
        while(ok && remaining) {
            size_t count = remaining > sizeof(a) ? sizeof(a) : (size_t)remaining;
            ok = storage_file_read(from, a, count) == count &&
                 storage_file_read(to, b, count) == count && memcmp(a, b, count) == 0;
            remaining -= count;
        }
        if(ok) ok = storage_file_sync(to);
    }
    if(from_open) ok = storage_file_close(from) && ok;
    if(to_open) ok = storage_file_close(to) && ok;
    storage_file_free(from);
    storage_file_free(to);
    return ok;
}

const char* ham_profile_source(Storage* storage) {
    return storage_common_stat(storage, HAM_PROFILE_PENDING, NULL) == FSE_OK ?
               HAM_PROFILE_BACKUP : HAM_PROFILE;
}

bool ham_profile_recover(Storage* storage) {
    FS_Error pending = storage_common_stat(storage, HAM_PROFILE_PENDING, NULL);
    if(pending == FSE_NOT_EXIST) return true;
    if(pending != FSE_OK) return false;
    if(!copy_verified(storage, HAM_PROFILE_BACKUP, HAM_PROFILE)) return false;
    /* Keep the backup authoritative if clearing the marker fails. */
    if(!remove_if_present(storage, HAM_PROFILE_PENDING)) return false;
    remove_if_present(storage, HAM_PROFILE_BACKUP);
    return true;
}

bool ham_profile_store(Storage* storage, const char* contents) {
    if(!ham_profile_recover(storage)) return false;
    if(!write_verified(storage, HAM_PROFILE_TEMP, contents)) return false;
    FS_Error existing = storage_common_stat(storage, HAM_PROFILE, NULL);
    if(existing != FSE_OK && existing != FSE_NOT_EXIST) return false;
    bool backup = existing == FSE_OK;
    if(backup) {
        if(!copy_verified(storage, HAM_PROFILE, HAM_PROFILE_BACKUP)) return false;
        /* Written only AFTER the backup is verified and flushed. A partial marker is
           sufficient for recovery; an interrupted backup copy has no marker. */
        if(!write_verified(storage, HAM_PROFILE_PENDING, "ready\n")) return false;
    }
    /* Flipper rename is copy/delete, not an atomic filesystem replacement. Leave
       the marker and backup intact on failure so startup can recover old settings. */
    if(storage_common_rename(storage, HAM_PROFILE_TEMP, HAM_PROFILE) != FSE_OK ||
       !matches(storage, HAM_PROFILE, contents)) return false;
    if(backup) {
        if(!remove_if_present(storage, HAM_PROFILE_PENDING)) return false;
        remove_if_present(storage, HAM_PROFILE_BACKUP);
    }
    return true;
}
