#include "common/ProcessRunner.h"

#include <cerrno>
#include <cstring>
#include <stdexcept>
#include <mutex>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace common {
namespace {

constexpr auto kPollInterval = std::chrono::milliseconds(25);

bool ShouldTerminate(ProcessResult& result, const ProcessOptions& options,
                     const CancellationToken& cancellation,
                     std::chrono::steady_clock::time_point started) {
    result.cancelled = cancellation.isCancellationRequested();
    result.timed_out = !result.cancelled && options.timeout.count() > 0 &&
        std::chrono::steady_clock::now() - started >= options.timeout;
    return result.cancelled || result.timed_out;
}

#ifdef _WIN32
class Handle {
public:
    explicit Handle(HANDLE value = nullptr) : value_(value) {}
    ~Handle() { if (value_ && value_ != INVALID_HANDLE_VALUE) CloseHandle(value_); }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    HANDLE get() const { return value_; }
private:
    HANDLE value_;
};

std::wstring Wide(const std::string& value) {
    if (value.empty()) return {};
    const int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                                          value.data(), static_cast<int>(value.size()),
                                          nullptr, 0);
    if (!count) throw std::invalid_argument("ProcessRunner: invalid UTF-8 path/argument");
    std::wstring out(count, L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
                        static_cast<int>(value.size()), out.data(), count);
    return out;
}

// Windows CRT argv quoting: double backslashes before quotes and the closing
// quote, while preserving every other backslash literally.
std::wstring QuoteArgument(const std::wstring& value) {
    std::wstring out = L"\"";
    size_t slashes = 0;
    for (const wchar_t character : value) {
        if (character == L'\\') {
            ++slashes;
            continue;
        }
        out.append(character == L'"' ? slashes * 2 + 1 : slashes, L'\\');
        slashes = 0;
        out += character;
    }
    out.append(slashes * 2, L'\\');
    out += L'"';
    return out;
}

std::string WindowsError(const char* operation) {
    return std::string(operation) + " failed (Windows error " +
        std::to_string(GetLastError()) + ")";
}

ProcessResult RunNative(const ProcessOptions& options,
                        const CancellationToken& cancellation) {
    ProcessResult result;
    const auto started = std::chrono::steady_clock::now();
    const auto executable = Wide(options.executable);
    const auto directory = Wide(options.working_directory);
    std::wstring command = QuoteArgument(executable);
    for (const auto& argument : options.arguments) {
        command += L' ';
        command += QuoteArgument(Wide(argument));
    }

    Handle job(CreateJobObjectW(nullptr, nullptr));
    if (!job.get()) { result.error = WindowsError("CreateJobObject"); return result; }
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (!SetInformationJobObject(job.get(), JobObjectExtendedLimitInformation,
                                  &limits, sizeof(limits))) {
        result.error = WindowsError("SetInformationJobObject");
        return result;
    }

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, FALSE,
                         CREATE_SUSPENDED | CREATE_NO_WINDOW, nullptr,
                         directory.empty() ? nullptr : directory.c_str(), &startup, &process)) {
        result.error = WindowsError("CreateProcess");
        return result;
    }
    Handle process_handle(process.hProcess);
    Handle thread_handle(process.hThread);
    // Assign before resuming: children cannot escape between launch and assignment.
    if (!AssignProcessToJobObject(job.get(), process.hProcess)) {
        result.error = WindowsError("AssignProcessToJobObject");
        TerminateProcess(process.hProcess, 1);
        WaitForSingleObject(process.hProcess, INFINITE);
        return result;
    }
    if (ResumeThread(process.hThread) == static_cast<DWORD>(-1)) {
        result.error = WindowsError("ResumeThread");
        TerminateJobObject(job.get(), 1);
        WaitForSingleObject(process.hProcess, INFINITE);
        return result;
    }
    result.started = true;
    while (true) {
        const DWORD wait = WaitForSingleObject(process.hProcess,
                                               static_cast<DWORD>(kPollInterval.count()));
        if (wait == WAIT_OBJECT_0) break;
        if (wait == WAIT_FAILED) {
            result.error = WindowsError("WaitForSingleObject");
            TerminateJobObject(job.get(), 1);
            WaitForSingleObject(process.hProcess, INFINITE);
            return result;
        }
        if (ShouldTerminate(result, options, cancellation, started)) {
            if (!TerminateJobObject(job.get(), 1)) {
                result.error = WindowsError("TerminateJobObject");
            }
            WaitForSingleObject(process.hProcess, INFINITE);
            return result;
        }
    }
    DWORD code = 0;
    if (!GetExitCodeProcess(process.hProcess, &code)) {
        result.error = WindowsError("GetExitCodeProcess");
    } else {
        result.exit_code = static_cast<int>(code);
    }
    // Closing the job also prevents orphan descendants on a normal parent exit.
    return result;
}
#else
ProcessResult RunNative(const ProcessOptions& options,
                        const CancellationToken& cancellation) {
    ProcessResult result;
    std::vector<char*> arguments;
    arguments.push_back(const_cast<char*>(options.executable.c_str()));
    for (const auto& argument : options.arguments)
        arguments.push_back(const_cast<char*>(argument.c_str()));
    arguments.push_back(nullptr);

    const auto started = std::chrono::steady_clock::now();
    int errors[2];
    pid_t pid = -1;
    // Keep descriptor setup and fork together. This closes the race where
    // another ProcessRunner invocation forks while FD_CLOEXEC is pending.
    static std::mutex launch_mutex;
    // Do not let a C++ lock guard cross fork().  In the child, only
    // async-signal-safe operations are valid before exec/_exit; destroying a
    // std::lock_guard there would call std::mutex::unlock() in an undefined
    // state.  The parent releases the lock after fork, while the child exits
    // through exec/_exit without touching it.
    launch_mutex.lock();
    if (pipe(errors) != 0) {
        result.error = std::strerror(errno);
        launch_mutex.unlock();
        return result;
    }
    if (fcntl(errors[0], F_SETFD, FD_CLOEXEC) != 0 ||
        fcntl(errors[1], F_SETFD, FD_CLOEXEC) != 0) {
        result.error = std::strerror(errno);
        close(errors[0]);
        close(errors[1]);
        launch_mutex.unlock();
        return result;
    }
    pid = fork();
    if (pid != 0) {
        close(errors[1]);
        launch_mutex.unlock();
    }
    if (pid == 0) {
        close(errors[0]);
        if (setpgid(0, 0) != 0 ||
            (!options.working_directory.empty() && chdir(options.working_directory.c_str()) != 0)) {
            const int error = errno;
            (void)write(errors[1], &error, sizeof(error));
            _exit(127);
        }
        execvp(options.executable.c_str(), arguments.data());
        const int error = errno;
        (void)write(errors[1], &error, sizeof(error));
        _exit(127);
    }
    if (pid < 0) {
        result.error = std::strerror(errno);
        close(errors[0]);
        return result;
    }
    int launch_error = 0;
    ssize_t count;
    do { count = read(errors[0], &launch_error, sizeof(launch_error)); }
    while (count < 0 && errno == EINTR);
    close(errors[0]);
    if (count > 0) {
        result.error = std::strerror(launch_error);
        while (waitpid(pid, nullptr, 0) < 0 && errno == EINTR) {}
        return result;
    }
    result.started = true;
    int status = 0;
    while (true) {
        const pid_t waited = waitpid(pid, &status, WNOHANG);
        if (waited == pid) break;
        if (waited < 0 && errno != EINTR) {
            result.error = std::strerror(errno);
            kill(-pid, SIGKILL);
            return result;
        }
        if (ShouldTerminate(result, options, cancellation, started)) {
            kill(-pid, SIGKILL);
            while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
            return result;
        }
        cancellation.waitFor(kPollInterval);
    }
    result.exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
    return result;
}
#endif

}  // namespace

ProcessResult ProcessRunner::run(const ProcessOptions& options,
                                 const CancellationToken& cancellation) {
    ProcessResult result;
    if (cancellation.isCancellationRequested()) { result.cancelled = true; return result; }
    if (options.executable.empty() || options.timeout.count() < 0) {
        result.error = "ProcessRunner: executable is empty or timeout is negative";
        return result;
    }
    try {
        return RunNative(options, cancellation);
    } catch (const std::exception& error) {
        result.error = error.what();
        return result;
    }
}

}  // namespace common
