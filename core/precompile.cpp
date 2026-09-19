#include "precompile.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <psapi.h>

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

// How long a child has to run before it is taken to have translated its module
// and written it to the cache, rather than found it there. A module already
// cached comes back in a second or two -- load the driver, look the module up,
// load its code object -- while the modules that matter take minutes to
// translate. A small module that translates faster than this is not checked
// again; if its row were lost, translating it on demand later costs seconds.
constexpr ULONGLONG kTranslatedAfterMs = 10000;

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

// The cache the translations have to land in, resolved the same way
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

// One module, translated by a copy of this program. Null if it would not start.
HANDLE start_compile_one(const std::wstring &self, const std::wstring &module_file,
                         const std::wstring &driver) {
    std::wstring command =
        L"\"" + self + L"\" --compile-one \"" + module_file + L"\" \"" + driver + L"\"";
    STARTUPINFOW startup{};
    startup.cb = sizeof startup;
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
                        nullptr, nullptr, &startup, &process))
        return nullptr;
    CloseHandle(process.hThread);
    return process.hProcess;
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
    std::sort(files.begin(), files.end(), [](const std::wstring &a, const std::wstring &b) {
        auto size_of = [](const std::wstring &path) -> unsigned long long {
            WIN32_FILE_ATTRIBUTE_DATA data{};
            if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &data)) return 0;
            return ((unsigned long long)data.nFileSizeHigh << 32) | data.nFileSizeLow;
        };
        return size_of(a) > size_of(b);
    });

    // The children translate into the cache the program itself will read, so a
    // module already there -- shipped with the program, or left by an earlier
    // run -- is found in a second instead of translated again for minutes. That
    // is the ordinary case: most starts find every module cached.
    //
    // Several processes writing one cache database can lose rows without a
    // word: filling a cache with four large modules in flight produced 12 and
    // then 14 rows out of 15 across two identical runs, with a busy timeout and
    // WAL already in place. That is dealt with below, after the fact, rather
    // than by giving each child a database of its own: a child with a cache of
    // its own cannot see the one that already holds its module, and would
    // translate everything on every start.
    wchar_t previous_cache[MAX_PATH] = {};
    const bool had_cache_variable =
        GetEnvironmentVariableW(L"ZLUDA_CACHE_DIR", previous_cache, MAX_PATH) > 0;
    SetEnvironmentVariableW(L"ZLUDA_CACHE_DIR", cache_directory().c_str());

    const std::wstring self = own_path();
    std::vector<HANDLE> running;
    std::vector<size_t> running_module;
    std::vector<ULONGLONG> running_since;
    std::vector<size_t> translated;
    size_t next = 0;
    int failures = 0;

    while (next < files.size() || !running.empty()) {
        while (next < files.size() && running.size() < ceiling &&
               (!adaptive || room_for_another(running))) {
            if (HANDLE process = start_compile_one(self, files[next], driver)) {
                running.push_back(process);
                running_module.push_back(next);
                running_since.push_back(GetTickCount64());
            } else {
                ++failures;
                ++progress.done;
            }
            ++next;
        }
        if (running.empty()) break;

        const DWORD which = WaitForMultipleObjects((DWORD)running.size(), running.data(), FALSE,
                                                   INFINITE);
        const size_t index = (size_t)(which - WAIT_OBJECT_0);
        if (index >= running.size()) break;
        DWORD code = 1;
        GetExitCodeProcess(running[index], &code);
        const size_t module_index = running_module[index];
        const ULONGLONG took = GetTickCount64() - running_since[index];
        CloseHandle(running[index]);
        running.erase(running.begin() + index);
        running_module.erase(running_module.begin() + index);
        running_since.erase(running_since.begin() + index);

        std::string why;
        if (code != 0) {
            ++failures;
            // The code itself, because an access violation and a refusal from
            // the driver call for different things and "failed" says neither.
            char hex[48];
            snprintf(hex, sizeof hex, "exit code 0x%08lX", code);
            why = hex;
        } else if (took >= kTranslatedAfterMs) {
            translated.push_back(module_index);
        }

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

    // What the parallel pass wrote, it may have lost. Each module that was
    // actually translated rather than found is loaded once more, alone: one
    // whose row survived is found in a second, and one whose row was lost is
    // translated again -- this time with nobody else writing. On a start that
    // found everything cached there is nothing to check and this costs nothing.
    if (!translated.empty()) {
        char buffer[160];
        snprintf(buffer, sizeof buffer, "checking %zu translated modules one at a time",
                 translated.size());
        progress.message = buffer;
        report(progress);
        for (size_t module_index : translated) {
            HANDLE process = start_compile_one(self, files[module_index], driver);
            if (!process) {
                ++failures;
                continue;
            }
            WaitForSingleObject(process, INFINITE);
            DWORD code = 1;
            GetExitCodeProcess(process, &code);
            CloseHandle(process);
            if (code != 0) ++failures;
        }
    }

    if (had_cache_variable)
        SetEnvironmentVariableW(L"ZLUDA_CACHE_DIR", previous_cache);
    else
        SetEnvironmentVariableW(L"ZLUDA_CACHE_DIR", nullptr);

    for (const std::wstring &path : files) DeleteFileW(path.c_str());
    RemoveDirectoryW(directory.c_str());

    if (failures) {
        error = std::to_string(failures) + " of " + std::to_string(progress.total) +
                " modules could not be translated";
        return false;
    }
    return true;
}

} // namespace enhancer
