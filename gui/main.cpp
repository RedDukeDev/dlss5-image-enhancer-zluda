#include <QApplication>

#include "main_window.h"
#include "../core/precompile.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cstdio>

namespace {

// Ties the lifetime of every process this one spawns to its own.
//
// Precompilation runs copies of this program (--compile-one) and waits on
// them with an unbounded timeout. If the window is closed, crashes, or is
// killed while that wait is still blocking the worker thread, CreateProcessW
// alone leaves those copies as ordinary orphans: nothing about Windows process
// creation ties a child's life to its parent's. A job object does, and
// unconditionally -- however this process ends, not only on a clean exit --
// which a WM_CLOSE handler trying to walk the process list and terminate each
// one by hand could not promise.
//
// The handle is deliberately never closed. Windows closes it when this
// process exits, and closing the last handle to a job created with
// JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE is what kills every process still in it.
void kill_children_when_this_process_ends() {
    HANDLE job = CreateJobObjectW(nullptr, nullptr);
    if (!job) return;
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION info{};
    info.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (!SetInformationJobObject(job, JobObjectExtendedLimitInformation, &info, sizeof info) ||
        !AssignProcessToJobObject(job, GetCurrentProcess())) {
        // Already in a job that does not allow nesting, most likely -- rare,
        // and not worth failing the program over. The children just outlive
        // it in that case, as they always used to.
        CloseHandle(job);
    }
}

} // namespace

int main(int argc, char **argv) {
    kill_children_when_this_process_ends();

    QApplication application(argc, argv);
    QApplication::setApplicationName("DLSS 5 Image Enhancer");

    qRegisterMetaType<enhancer::Image>("enhancer::Image");
    qRegisterMetaType<enhancer::Settings>("enhancer::Settings");
    qRegisterMetaType<enhancer::Paths>("enhancer::Paths");

    const QStringList arguments = QApplication::arguments();

    // One module, then exit: what the parallel translation spawns copies of
    // this program to do.
    if (arguments.size() == 4 && arguments[1] == QLatin1String("--compile-one"))
        return enhancer::compile_one(arguments[2].toStdWString(), arguments[3].toStdWString());

    // A way to exercise the whole path without a window, so it can be checked
    // rather than clicked through.
    if (arguments.size() > 1 && arguments[1] == QLatin1String("--selftest"))
        return enhancer::run_self_test(arguments.mid(1));

    // The translation on its own, for a machine being prepared before anyone
    // sits down at it.
    if (arguments.size() >= 4 && arguments[1] == QLatin1String("--precompile")) {
        std::string error;
        const bool ok = enhancer::precompile(
            arguments[2].toStdWString(), arguments[3].toStdWString(),
            arguments.size() > 4 ? (unsigned)arguments[4].toUInt() : 0u,
            [](const enhancer::Progress &progress) {
                fprintf(stderr, "%s\n", progress.message.c_str());
                fflush(stderr);
            },
            error);
        if (!ok) fprintf(stderr, "%s\n", error.c_str());
        return ok ? 0 : 1;
    }

    enhancer::MainWindow window;
    window.show();
    return QApplication::exec();
}
