#include "precompile.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <psapi.h>

#include <QtCore/QFile>
#include <QtSql/QSqlDatabase>
#include <QtSql/QSqlError>
#include <QtSql/QSqlQuery>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>

#pragma comment(lib, "psapi.lib")

namespace enhancer {
namespace {

// How much room to keep free for one more translation before starting it.
//
// This is the peak a single translation was measured to reach, on the largest
// module the network carries -- around nine megabytes of PTX with twenty-odd
// thousand matrix operations. It is not a guess, and it must not be smaller
// than the peak: admitting a translation with less free than it will go on to
// use eats into the reserve below rather than into the headroom, which makes
// the reserve a number this file prints rather than one it honours.
//
// What keeps the machine busy is not a small constant here but the question
// being asked again every time a translation finishes, against the memory
// actually free at that moment. One is always allowed to run, so a machine too
// small for two still makes progress, one module at a time.
constexpr unsigned long long kHeadroomMb = 2600;

// What to leave for everything else. Someone may well be using the machine.
constexpr unsigned long long kReserveMb = 3000;

unsigned long long free_physical_mb() {
    MEMORYSTATUSEX status{};
    status.dwLength = sizeof status;
    if (!GlobalMemoryStatusEx(&status)) return 0;
    return status.ullAvailPhys / (1024 * 1024);
}

unsigned logical_processors() {
    SYSTEM_INFO info{};
    GetSystemInfo(&info);
    return info.dwNumberOfProcessors ? info.dwNumberOfProcessors : 1;
}

// What a running translation holds right now, in MB.
unsigned long long resident_mb(HANDLE process) {
    PROCESS_MEMORY_COUNTERS counters{};
    if (!GetProcessMemoryInfo(process, &counters, sizeof counters)) return 0;
    return counters.WorkingSetSize / (1024 * 1024);
}

// Whether there is room to start one more right now.
//
// Not simply "is a translation's worth free": a translation just started has
// not allocated anything yet, so the memory it is about to take still looks
// free, and that test passes again and again -- asked that way, every slot
// fills in the same second and the machine runs out a few minutes later. Each
// running translation is charged what it has yet to claim, the peak less what
// it already holds, and one more is admitted only if it still fits on top.
bool room_for_another(const std::vector<HANDLE> &running) {
    if (running.empty()) return true; // always make progress, whatever the machine says
    const unsigned long long free_mb = free_physical_mb();
    if (!free_mb) return running.size() < 2; // no reading available: stay cautious
    unsigned long long still_to_claim = 0;
    for (HANDLE process : running) {
        const unsigned long long held = resident_mb(process);
        if (held < kHeadroomMb) still_to_claim += kHeadroomMb - held;
    }
    return free_mb >= kReserveMb + still_to_claim + kHeadroomMb;
}

std::wstring own_path() {
    wchar_t buffer[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, buffer, MAX_PATH);
    return buffer;
}

std::wstring temporary_directory() {
    wchar_t base[MAX_PATH] = {};
    GetTempPathW(MAX_PATH, base);
    std::wstring directory = std::wstring(base) + L"dlss5-precompile";
    CreateDirectoryW(directory.c_str(), nullptr);
    return directory;
}

// CreateDirectory refuses a path whose parent does not exist yet, so each
// component is made in turn. An existing directory is not an error here.
void make_directories(const std::wstring &path) {
    for (size_t at = path.find(L'\\', 3); at != std::wstring::npos;
         at = path.find(L'\\', at + 1))
        CreateDirectoryW(path.substr(0, at).c_str(), nullptr);
    CreateDirectoryW(path.c_str(), nullptr);
}

void remove_tree(const std::wstring &path) {
    // Doubly terminated, which is what SHFileOperation's predecessor in this
    // role expects; the loop below is simpler and has no such requirement.
    WIN32_FIND_DATAW entry{};
    const HANDLE search = FindFirstFileW((path + L"\\*").c_str(), &entry);
    if (search == INVALID_HANDLE_VALUE) return;
    do {
        const std::wstring name = entry.cFileName;
        if (name == L"." || name == L"..") continue;
        const std::wstring child = path + L"\\" + name;
        if (entry.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
            remove_tree(child);
        else
            DeleteFileW(child.c_str());
    } while (FindNextFileW(search, &entry));
    FindClose(search);
    RemoveDirectoryW(path.c_str());
}

// Where the translations have to end up, resolved the same way
// image_processor.cpp resolves it before handing the network to ZLUDA: the
// environment when it says, otherwise the cache shipped beside the program.
// The two must agree, or this would fill a cache nothing ever reads.
std::wstring cache_directory() {
    wchar_t named[MAX_PATH] = {};
    if (GetEnvironmentVariableW(L"ZLUDA_CACHE_DIR", named, MAX_PATH)) return named;
    std::wstring path = own_path();
    const size_t slash = path.find_last_of(L"/" L"\\");
    if (slash == std::wstring::npos) return L"zluda\\ComputeCache";
    path.resize(slash + 1);
    return path + L"zluda\\ComputeCache";
}

// How many modules a cache database holds; -1 if it cannot be read at all.
int rows_in(const QString &database) {
    if (!QFile::exists(database)) return 0;
    int held = -1;
    {
        QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"),
                                                    QStringLiteral("count"));
        db.setDatabaseName(database);
        if (db.open()) {
            QSqlQuery query(db);
            if (query.exec(QStringLiteral("SELECT COUNT(*) FROM modules")) && query.next())
                held = query.value(0).toInt();
            db.close();
        }
    }
    QSqlDatabase::removeDatabase(QStringLiteral("count"));
    return held;
}

// Puts the per-translation databases into the cache the program will read.
//
// Unlike the tools that build a release cache, this cannot start from a copy of
// the first shard: the destination may already hold something -- the cache
// shipped with the program, or what an earlier run left -- and copying over it
// would throw that away. So a missing destination is seeded from one shard, and
// an existing one is inserted into.
//
// The merge is exact rather than approximate because the schema carries a
// unique index on (hash, compiler_version, zluda_version, device, backend_key):
// a module already present collapses to one row instead of doubling.
bool merge_shards(const std::wstring &destination, const std::vector<std::wstring> &shards,
                  std::string &error) {
    const QString target = QString::fromStdWString(destination + L"\\zluda2.db");
    std::vector<QString> sources;
    for (const std::wstring &shard : shards) {
        const QString path = QString::fromStdWString(shard + L"\\zluda2.db");
        if (QFile::exists(path)) sources.push_back(path);
    }
    if (sources.empty()) {
        error = "no translation produced a cache database";
        return false;
    }

    size_t first = 0;
    if (!QFile::exists(target)) {
        // Seeded from a shard rather than from a CREATE TABLE written here, so
        // the schema is ZLUDA's own -- indexes and the triggers that maintain
        // globals.total_size included -- instead of a second copy of it that
        // would have to be kept in step with ZLUDA by hand.
        if (!QFile::copy(sources[0], target)) {
            error = "the module cache could not be created beside the program";
            return false;
        }
        first = 1;
    }

    bool ok = true;
    {
        QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"),
                                                    QStringLiteral("merge"));
        db.setDatabaseName(target);
        if (!db.open()) {
            error = "the module cache could not be opened: " +
                    db.lastError().text().toStdString();
            QSqlDatabase::removeDatabase(QStringLiteral("merge"));
            return false;
        }
        QSqlQuery query(db);
        for (size_t i = first; i < sources.size() && ok; ++i) {
            query.prepare(QStringLiteral("ATTACH DATABASE ? AS shard"));
            query.addBindValue(sources[i]);
            if (!query.exec()) {
                error = "a translation could not be read back: " +
                        query.lastError().text().toStdString();
                ok = false;
                break;
            }
            ok = query.exec(QStringLiteral(
                "INSERT OR IGNORE INTO modules "
                "(hash, compiler_version, zluda_version, device, backend_key, binary, "
                "last_access) SELECT hash, compiler_version, zluda_version, device, "
                "backend_key, binary, last_access FROM shard.modules"));
            if (!ok)
                error = "a translation could not be merged: " +
                        query.lastError().text().toStdString();
            query.exec(QStringLiteral("DETACH DATABASE shard"));
        }
        db.close();
    }
    QSqlDatabase::removeDatabase(QStringLiteral("merge"));
    return ok;
}

} // namespace

std::vector<std::vector<unsigned char>> extract_modules(const std::wstring &library,
                                                        std::string &error) {
    std::vector<std::vector<unsigned char>> modules;

    std::ifstream file(library, std::ios::binary);
    if (!file) {
        error = "the network library could not be opened";
        return modules;
    }
    const std::vector<unsigned char> bytes((std::istreambuf_iterator<char>(file)),
                                           std::istreambuf_iterator<char>());
    if (bytes.size() < 16) {
        error = "the network library is empty";
        return modules;
    }

    // A module begins with a fixed magic, and its header says how long the whole
    // thing is: two bytes of header size at +6, eight of body size at +8.
    const unsigned char magic[4] = {0x50, 0xED, 0x55, 0xBA};
    for (size_t i = 0; i + 16 < bytes.size(); ++i) {
        if (std::memcmp(&bytes[i], magic, 4) != 0) continue;
        unsigned short header_size = 0;
        unsigned long long body_size = 0;
        std::memcpy(&header_size, &bytes[i + 6], sizeof header_size);
        std::memcpy(&body_size, &bytes[i + 8], sizeof body_size);
        const unsigned long long total = (unsigned long long)header_size + body_size;
        // A plausible module: it fits, and it is neither empty nor absurd.
        if (header_size < 16 || total < 64 || total > 64ull * 1024 * 1024 ||
            i + total > bytes.size())
            continue;
        modules.emplace_back(bytes.begin() + i, bytes.begin() + i + (size_t)total);
        i += (size_t)total - 1;
    }

    if (modules.empty()) error = "no code modules were found in the network library";
    return modules;
}

int compile_one(const std::wstring &module_file, const std::wstring &driver) {
    HMODULE cuda = LoadLibraryW(driver.c_str());
    if (!cuda) return 1;

    auto cuInit = (int (*)(unsigned))GetProcAddress(cuda, "cuInit");
    auto cuDeviceGet = (int (*)(int *, int))GetProcAddress(cuda, "cuDeviceGet");
    auto cuCtxCreate = (int (*)(void **, unsigned, int))GetProcAddress(cuda, "cuCtxCreate_v2");
    if (!cuCtxCreate) cuCtxCreate = (int (*)(void **, unsigned, int))GetProcAddress(cuda, "cuCtxCreate");
    auto cuModuleLoadData = (int (*)(void **, const void *))GetProcAddress(cuda, "cuModuleLoadData");
    if (!cuInit || !cuDeviceGet || !cuCtxCreate || !cuModuleLoadData) return 2;

    std::ifstream file(module_file, std::ios::binary);
    if (!file) return 3;
    std::vector<unsigned char> image((std::istreambuf_iterator<char>(file)),
                                     std::istreambuf_iterator<char>());
    // One trailing zero: a module held as text is read as a C string, and a file
    // on disk carries no terminator of its own.
    image.push_back(0);

    if (cuInit(0) != 0) return 4;
    int device = 0;
    void *context = nullptr;
    if (cuDeviceGet(&device, 0) != 0 || cuCtxCreate(&context, 0, device) != 0) return 5;

    void *module = nullptr;
    return cuModuleLoadData(&module, image.data()) == 0 ? 0 : 6;
}

bool precompile(const std::wstring &library, const std::wstring &driver, unsigned jobs,
                const std::function<void(const Progress &)> &report, std::string &error) {
    const std::vector<std::vector<unsigned char>> modules = extract_modules(library, error);
    if (modules.empty()) return false;

    // Zero means "as many as the machine turns out to allow", which is decided
    // again every time one finishes rather than once at the start.
    const unsigned ceiling = jobs ? jobs : logical_processors();
    const bool adaptive = jobs == 0;

    Progress progress;
    progress.total = (int)modules.size();
    {
        char buffer[192];
        snprintf(buffer, sizeof buffer, "%d modules, up to %u at a time%s", progress.total,
                 ceiling, adaptive ? " as memory allows" : "");
        progress.message = buffer;
    }
    report(progress);

    // Written out because the work happens in separate processes: each one is
    // a whole compiler pipeline over a large module, and running them in
    // threads of one process would share a driver context that is not built for
    // it.
    const std::wstring directory = temporary_directory();
    std::vector<std::wstring> files;
    for (size_t i = 0; i < modules.size(); ++i) {
        wchar_t name[64];
        swprintf(name, 64, L"\\module_%03zu.bin", i);
        const std::wstring path = directory + name;
        std::ofstream out(path, std::ios::binary);
        if (!out) {
            error = "the extracted modules could not be written to a temporary directory";
            return false;
        }
        out.write((const char *)modules[i].data(), (std::streamsize)modules[i].size());
        files.push_back(path);
    }

    // Largest first: the long poles then start immediately instead of being
    // picked up last, which is the difference between finishing in one wave and
    // waiting on a single straggler.
    std::sort(files.begin(), files.end(), [&modules, &files](const std::wstring &a,
                                                             const std::wstring &b) {
        auto size_of = [](const std::wstring &path) -> unsigned long long {
            WIN32_FILE_ATTRIBUTE_DATA data{};
            if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &data)) return 0;
            return ((unsigned long long)data.nFileSizeHigh << 32) | data.nFileSizeLow;
        };
        return size_of(a) > size_of(b);
    });

    // One cache database per translation, merged when they are all done.
    //
    // Several processes writing one zluda2.db can lose rows and report nothing:
    // filling a cache with four large modules in flight produced 12 and then 14
    // rows out of 15 across two identical runs, with a 30-second busy timeout
    // and WAL already in place. A database with a single writer cannot race, and
    // it also makes a loss visible -- a shard must hold exactly one module, so a
    // hole is named here instead of being discovered later as an unexplained
    // pause while that one module is translated again.
    const std::wstring destination = cache_directory();
    const std::wstring shard_base = destination + L"\\.shards";
    make_directories(destination);
    make_directories(shard_base);
    wchar_t previous_cache[MAX_PATH] = {};
    const bool had_cache_variable =
        GetEnvironmentVariableW(L"ZLUDA_CACHE_DIR", previous_cache, MAX_PATH) > 0;

    const std::wstring self = own_path();
    std::vector<HANDLE> running;
    std::vector<std::wstring> shards(files.size());
    std::vector<size_t> shard_of_running;
    size_t next = 0;
    int failures = 0;

    while (next < files.size() || !running.empty()) {
        while (next < files.size() && running.size() < ceiling &&
               (!adaptive || room_for_another(running))) {
            wchar_t shard[MAX_PATH];
            swprintf(shard, MAX_PATH, L"%s\\unit_%03zu", shard_base.c_str(), next);
            shards[next] = shard;
            make_directories(shards[next]);
            // The child inherits the environment as it stands when it is
            // created, so setting this here is what gives each translation a
            // database nobody else writes to.
            SetEnvironmentVariableW(L"ZLUDA_CACHE_DIR", shard);

            std::wstring command = L"\"" + self + L"\" --compile-one \"" + files[next] + L"\" \"" +
                                   driver + L"\"";
            STARTUPINFOW startup{};
            startup.cb = sizeof startup;
            PROCESS_INFORMATION process{};
            if (CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
                               nullptr, nullptr, &startup, &process)) {
                CloseHandle(process.hThread);
                running.push_back(process.hProcess);
                shard_of_running.push_back(next);
            } else {
                ++failures;
                ++progress.done;
                shards[next].clear();
            }
            ++next;
        }
        // Put it back straight away, so nothing else in this process inherits a
        // shard as its cache.
        if (had_cache_variable)
            SetEnvironmentVariableW(L"ZLUDA_CACHE_DIR", previous_cache);
        else
            SetEnvironmentVariableW(L"ZLUDA_CACHE_DIR", nullptr);

        if (running.empty()) break;

        const DWORD which = WaitForMultipleObjects((DWORD)running.size(), running.data(), FALSE,
                                                   INFINITE);
        const size_t index = (size_t)(which - WAIT_OBJECT_0);
        if (index >= running.size()) break;
        DWORD code = 1;
        GetExitCodeProcess(running[index], &code);
        const size_t module_index = shard_of_running[index];
        CloseHandle(running[index]);
        running.erase(running.begin() + index);
        shard_of_running.erase(shard_of_running.begin() + index);

        bool landed = code == 0;
        std::string why;
        if (landed) {
            // Exactly one module, or the translation reported a success it did
            // not deliver.
            const int held = rows_in(QString::fromStdWString(shards[module_index] +
                                                             L"\\zluda2.db"));
            landed = held == 1;
            if (!landed) {
                shards[module_index].clear();
                why = "translated, but its cache holds " + std::to_string(held) + " rows";
            }
        } else {
            shards[module_index].clear();
            // The code itself, because an access violation and a refusal from
            // the driver call for different things and "failed" says neither.
            char hex[48];
            snprintf(hex, sizeof hex, "exit code 0x%08lX", code);
            why = hex;
        }
        if (!landed) ++failures;

        ++progress.done;
        {
            char buffer[320];
            snprintf(buffer, sizeof buffer, "translated %d of %d, %zu running, %llu MB free%s%s",
                     progress.done, progress.total, running.size(), free_physical_mb(),
                     why.empty() ? "" : " -- one module failed: ", why.c_str());
            progress.message = buffer;
        }
        report(progress);
    }

    for (const std::wstring &path : files) DeleteFileW(path.c_str());
    RemoveDirectoryW(directory.c_str());

    std::vector<std::wstring> landed;
    for (const std::wstring &shard : shards)
        if (!shard.empty()) landed.push_back(shard);

    bool merged = false;
    if (!landed.empty()) {
        progress.message = "merging " + std::to_string(landed.size()) + " translations";
        report(progress);
        merged = merge_shards(destination, landed, error);
    }
    // Kept when the merge fails: they are then the only copy of work that took
    // minutes per module.
    if (merged) remove_tree(shard_base);

    if (failures) {
        error = std::to_string(failures) + " of " + std::to_string(progress.total) +
                " modules could not be translated";
        return false;
    }
    if (!merged) {
        if (error.empty()) error = "the translations could not be merged into the cache";
        return false;
    }
    return true;
}

} // namespace enhancer
