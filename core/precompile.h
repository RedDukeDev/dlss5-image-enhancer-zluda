// Translating the network's code ahead of time, in parallel.
//
// Why this exists at all: the network ships two forms of every module, machine
// code for NVIDIA hardware and PTX. On an NVIDIA card the driver takes the
// machine code and there is nothing to compile, which is why it runs in real
// time there. On anything else the machine code is useless and the PTX has to
// be compiled -- once per machine, after which a cache answers.
//
// Why in parallel: a program loading the network gets its modules one at a
// time, so translation inside it is serial by construction and leaves every
// core but one idle. The cache is keyed by the content of a module rather than
// by who compiled it, so the same work done ahead of time in several processes
// is found later as a hit.

#pragma once

#include <functional>
#include <string>
#include <vector>

namespace enhancer {

// How many modules are done, out of how many, and what just happened.
struct Progress {
    int done = 0;
    int total = 0;
    std::string message;
};

// Pulls the embedded code modules out of the network's library, exactly as they
// sit in it. Extracted PTX would not do: the driver picks which embedded module
// to translate and derives the cache key from that choice, so only the same
// bytes produce the same key.
std::vector<std::vector<unsigned char>> extract_modules(const std::wstring &library,
                                                        std::string &error);

// Runs the translation, spawning copies of this program. `report` is called
// from the calling thread as each one finishes.
//
// `jobs` of zero chooses for itself: cores set the ceiling, free memory usually
// sets the real limit, because each translation is a whole compiler pipeline
// over a large module.
bool precompile(const std::wstring &library, const std::wstring &driver, unsigned jobs,
                const std::function<void(const Progress &)> &report, std::string &error);

// Translates one module and exits: what the spawned copies run. Returns a
// process exit code.
int compile_one(const std::wstring &module_file, const std::wstring &driver);

} // namespace enhancer
