#include "peare_fs.h"

#include <cstdlib>
#include <cstring>
#include <new>
#include <string>

#include "DiscStore.h"
#include "DiscFileSystem.h"
#include "Iso9660Reader.h"

using peare::fs::ByteStorePtr;
using peare::fs::DiscFileSystemPtr;
using peare::fs::DiscEntry;
using peare::fs::Iso9660Reader;
using peare::fs::MemoryStore;

struct peare_fs {
    ByteStorePtr disc;           // keeps the image bytes alive
    DiscFileSystemPtr fs;        // the detected reader
    std::string name;
};

struct peare_source {
    ByteStorePtr store;
};

namespace {

// Factory: try each supported reader in turn. Only ISO is wired now; WIM and
// others slot in here behind the same ABI.
DiscFileSystemPtr openReader(const ByteStorePtr& disc) {
    if (Iso9660Reader::detect(*disc)) {
        return std::make_shared<Iso9660Reader>(disc);
    }
    return DiscFileSystemPtr();
}

char* dupString(const std::string& s) {
    char* p = static_cast<char*>(std::malloc(s.size() + 1));
    if (p) { std::memcpy(p, s.c_str(), s.size() + 1); }
    return p;
}

}  // namespace

extern "C" {

peare_fs* peare_fs_open_memory(const uint8_t* data, size_t len) {
    if (!data || len == 0) return nullptr;
    ByteStorePtr disc = std::make_shared<MemoryStore>(data, len);
    DiscFileSystemPtr reader = openReader(disc);
    if (!reader || !reader->valid()) return nullptr;

    peare_fs* handle = new (std::nothrow) peare_fs;
    if (!handle) return nullptr;
    handle->disc = disc;
    handle->fs = reader;
    handle->name = reader->friendlyName();
    return handle;
}

int32_t peare_fs_valid(const peare_fs* fs) {
    return (fs && fs->fs && fs->fs->valid()) ? 1 : 0;
}

const char* peare_fs_name(const peare_fs* fs) {
    return fs ? fs->name.c_str() : "";
}

void peare_fs_close(peare_fs* fs) { delete fs; }

int32_t peare_fs_list(const peare_fs* fs, const char* dir_utf8, peare_fs_entry** out_list) {
    if (!fs || !fs->fs || !out_list) return -1;
    *out_list = nullptr;
    const std::vector<DiscEntry> entries = fs->fs->list(dir_utf8 ? dir_utf8 : "");
    if (entries.empty()) return 0;

    peare_fs_entry* arr = static_cast<peare_fs_entry*>(
        std::malloc(sizeof(peare_fs_entry) * entries.size()));
    if (!arr) return -1;
    for (std::size_t i = 0; i < entries.size(); ++i) {
        arr[i].name = dupString(entries[i].name);
        arr[i].is_directory = entries[i].isDirectory ? 1 : 0;
        arr[i].length = entries[i].length;
    }
    *out_list = arr;
    return static_cast<int32_t>(entries.size());
}

void peare_fs_free_list(peare_fs_entry* list, int32_t count) {
    if (!list) return;
    for (int32_t i = 0; i < count; ++i) std::free(const_cast<char*>(list[i].name));
    std::free(list);
}

peare_source* peare_fs_open_file(const peare_fs* fs, const char* path_utf8) {
    if (!fs || !fs->fs || !path_utf8) return nullptr;
    ByteStorePtr store = fs->fs->openFile(path_utf8);
    if (!store) return nullptr;
    peare_source* source = new (std::nothrow) peare_source;
    if (!source) return nullptr;
    source->store = store;
    return source;
}

int64_t peare_source_size(const peare_source* source) {
    return (source && source->store) ? source->store->capacity() : 0;
}

int32_t peare_source_read(const peare_source* source, int64_t pos, uint8_t* dst, int32_t count) {
    if (!source || !source->store || !dst || count <= 0) return 0;
    return source->store->read(pos, dst, count);
}

void peare_source_close(peare_source* source) { delete source; }

}  // extern "C"
