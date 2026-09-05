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
// A guess, and deliberately a modest one, because it is no longer the thing
// that decides the answer: the number of translations running at once is
// reconsidered every time one finishes, against the memory actually free at
// that moment. A constant that is too small only means the count ramps up in
// steps; one that is too large would leave the machine idle, which is what the
// earlier fixed estimate did.
constexpr unsigned long long kHeadroomMb = 1200;

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

// Whether there is room to start one more right now.
bool room_for_another(size_t running) {
    if (running == 0) return true; // always make progress, whatever the machine says
    const unsigned long long free_mb = free_physical_mb();
    return free_mb > kReserveMb + kHeadroomMb;
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

    const std::wstring self = own_path();
    std::vector<HANDLE> running;
    size_t next = 0;
    int failures = 0;

    while (next < files.size() || !running.empty()) {
        while (next < files.size() && running.size() < ceiling &&
               (!adaptive || room_for_another(running.size()))) {
            std::wstring command = L"\"" + self + L"\" --compile-one \"" + files[next] + L"\" \"" +
                                   driver + L"\"";
            STARTUPINFOW startup{};
            startup.cb = sizeof startup;
            PROCESS_INFORMATION process{};
            if (CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
                               nullptr, nullptr, &startup, &process)) {
                CloseHandle(process.hThread);
                running.push_back(process.hProcess);
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
        if (code != 0) ++failures;
        CloseHandle(running[index]);
        running.erase(running.begin() + index);

        ++progress.done;
        {
            char buffer[192];
            snprintf(buffer, sizeof buffer, "translated %d of %d, %zu running, %llu MB free",
                     progress.done, progress.total, running.size(), free_physical_mb());
            progress.message = buffer;
        }
        report(progress);
    }

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
