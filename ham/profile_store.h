#pragma once
#include <storage/storage.h>

#define HAM_PROFILE "/ext/apps_data/ham/profile.txt"
#define HAM_PROFILE_TEMP HAM_PROFILE ".tmp"
#define HAM_PROFILE_BACKUP HAM_PROFILE ".bak"
#define HAM_PROFILE_PENDING HAM_PROFILE ".pending"

/* A pending marker means the verified backup is authoritative until recovery finishes. */
const char* ham_profile_source(Storage* storage);
bool ham_profile_recover(Storage* storage);
bool ham_profile_store(Storage* storage, const char* contents);
