/* Copyright (c) 2026 VillageSQL Contributors
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, see <https://www.gnu.org/licenses/>.
 */

#include "villagesql/veb/precheck_harvest.h"

#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#if defined(__APPLE__)
#include <mach-o/dyld.h>
#endif

#include "villagesql/veb/precheck_helper_open.h"
#include "villagesql/veb/precheck_protocol.h"
#include "villagesql/veb/precheck_subprocess_main.h"

extern char **environ;

namespace villagesql {
namespace veb {

bool HarvestTargetTypesInProcess(
    const std::string &target_so_path, int server_protocol,
    std::vector<HarvestedTargetType> &out_types, std::string &out_error) {
  // Use the minimal opener so this function can also live inside the
  // mysqld-vef-precheck helper binary without dragging in the rest of
  // veb_file.cc (logging, victionary, libarchive). The full server-side
  // open_vef_extension wraps the same minimal opener and adds LogVSQL.
  MinimalExtensionRegistration target;
  if (OpenVefExtensionMinimal(target_so_path,
                              static_cast<vef_protocol_t>(server_protocol),
                              target, out_error)) {
    out_error = std::string("failed to load target .so at ") + target_so_path +
                ": " + out_error;
    return true;
  }

  // Harvest target-side type metadata from the loaded registration. We don't
  // hold pointers into the registration past the unload call below.
  std::vector<HarvestedTargetType> harvested;
  if (target.registration != nullptr) {
    const vef_registration_t *reg = target.registration;
    harvested.reserve(reg->type_count);
    for (unsigned int i = 0; i < reg->type_count; ++i) {
      const vef_type_desc_t *t = reg->types[i];
      if (t == nullptr || t->name == nullptr) continue;
      HarvestedTargetType h;
      h.type_name = t->name;
      h.persisted_length = static_cast<int64_t>(t->persisted_length);
      harvested.push_back(std::move(h));
    }
  }

  // Unload before returning. The harvest copied every byte it needs out of
  // the registration; the target .so has no business staying loaded past
  // this point.
  CloseVefExtensionMinimal(target);

  out_types = std::move(harvested);
  return false;
}

namespace {

// Deadline for the helper to write its result, in milliseconds. The
// helper binary's startup is light (no mysqld static-init), so 10s is
// generous.
constexpr int kPrecheckTimeoutMs = 10000;

// Name of the standalone helper binary the parent spawns. Installed
// next to mysqld by CMake; the parent locates it relative to its own
// path.
constexpr const char kPrecheckHelperBinary[] = "mysqld-vef-precheck";

// Resolve the path to the running executable. Used as a starting point
// for finding the helper binary next to mysqld. Returns true on failure.
bool resolve_self_path(std::string &out, std::string &error) {
#if defined(__APPLE__)
  uint32_t size = 0;
  _NSGetExecutablePath(nullptr, &size);
  std::vector<char> buf(size);
  if (_NSGetExecutablePath(buf.data(), &size) != 0) {
    error = "_NSGetExecutablePath failed";
    return true;
  }
  out.assign(buf.data());
  return false;
#else
  // Linux (and any Unix with /proc/self/exe).
  std::vector<char> buf(4096);
  for (;;) {
    const ssize_t n = ::readlink("/proc/self/exe", buf.data(), buf.size());
    if (n < 0) {
      error = std::string("readlink /proc/self/exe failed: ") +
              std::strerror(errno);
      return true;
    }
    if (static_cast<size_t>(n) < buf.size()) {
      out.assign(buf.data(), static_cast<size_t>(n));
      return false;
    }
    // Buffer too small; grow and retry.
    if (buf.size() >= (1 << 20)) {
      error = "/proc/self/exe path improbably long";
      return true;
    }
    buf.resize(buf.size() * 2);
  }
#endif
}

// Compute the path to the helper binary by stripping the basename from
// `self_path` and appending the helper name. The CMake install puts both
// binaries in the same directory.
std::string helper_path_from_self(const std::string &self_path) {
  const auto slash = self_path.find_last_of('/');
  if (slash == std::string::npos) return kPrecheckHelperBinary;
  return self_path.substr(0, slash + 1) + kPrecheckHelperBinary;
}

// RAII wrapper for a fd; closes on destruction unless released.
class FdGuard {
 public:
  explicit FdGuard(int fd = -1) : fd_(fd) {}
  ~FdGuard() {
    if (fd_ >= 0) ::close(fd_);
  }
  FdGuard(const FdGuard &) = delete;
  FdGuard &operator=(const FdGuard &) = delete;
  FdGuard(FdGuard &&o) noexcept : fd_(o.fd_) { o.fd_ = -1; }

  int get() const { return fd_; }
  int release() {
    int f = fd_;
    fd_ = -1;
    return f;
  }
  void reset(int fd = -1) {
    if (fd_ >= 0) ::close(fd_);
    fd_ = fd;
  }

 private:
  int fd_;
};

// Read all available bytes from `fd` into `out` until EOF or read error.
// Used to drain the child's stderr after harvest.
void drain_fd_to_string(int fd, std::string &out) {
  char buf[4096];
  for (;;) {
    const ssize_t n = ::read(fd, buf, sizeof(buf));
    if (n <= 0) return;
    out.append(buf, static_cast<size_t>(n));
  }
}

// Wait for the child to finish; return a short description of how it
// exited. `pid` is consumed (reaped).
std::string reap_child(pid_t pid) {
  int status = 0;
  while (true) {
    const pid_t r = ::waitpid(pid, &status, 0);
    if (r == pid) break;
    if (r < 0 && errno == EINTR) continue;
    return std::string("waitpid failed: ") + std::strerror(errno);
  }
  if (WIFEXITED(status)) {
    const int code = WEXITSTATUS(status);
    if (code == 0) return std::string();  // success
    return std::string("child exited with code ") + std::to_string(code);
  }
  if (WIFSIGNALED(status)) {
    return std::string("child killed by signal ") +
           std::to_string(WTERMSIG(status));
  }
  return std::string("child terminated abnormally (status=") +
         std::to_string(status) + ")";
}

}  // namespace

bool HarvestTargetTypes(const std::string &target_so_path, int server_protocol,
                        std::vector<HarvestedTargetType> &out_types,
                        std::string &out_error) {
  // Resolve the helper binary path: same directory as the running mysqld,
  // filename "mysqld-vef-precheck" (installed there by CMake).
  std::string self_path;
  if (resolve_self_path(self_path, out_error)) return true;
  const std::string helper_path = helper_path_from_self(self_path);

  // Two pipes: result (child -> parent, framed payload) and stderr
  // (child -> parent, free-form diagnostics).
  int result_pipe[2] = {-1, -1};
  int stderr_pipe[2] = {-1, -1};
  if (::pipe(result_pipe) != 0) {
    out_error = std::string("pipe(result) failed: ") + std::strerror(errno);
    return true;
  }
  FdGuard result_read(result_pipe[0]);
  FdGuard result_write(result_pipe[1]);

  if (::pipe(stderr_pipe) != 0) {
    out_error = std::string("pipe(stderr) failed: ") + std::strerror(errno);
    return true;
  }
  FdGuard stderr_read(stderr_pipe[0]);
  FdGuard stderr_write(stderr_pipe[1]);

  // CLOEXEC on the parent-side fds so we don't leak them into the child.
  // The child gets the other ends explicitly via posix_spawn file actions.
  ::fcntl(result_read.get(), F_SETFD, FD_CLOEXEC);
  ::fcntl(stderr_read.get(), F_SETFD, FD_CLOEXEC);

  // Build the argv for the helper. argv[0] is the helper path (by
  // convention), flags follow.
  const std::string protocol_str = std::to_string(server_protocol);
  // The helper sees the pipe write-ends dup2'd to fds 3 and 4
  // respectively (see file_actions below). Pass those constants in argv
  // so the helper knows where to write.
  const std::string result_fd_arg = "3";
  const std::string stderr_fd_arg = "4";

  // posix_spawn requires non-const argv; build a buffer.
  std::vector<std::string> argv_storage;
  argv_storage.reserve(10);
  argv_storage.push_back(helper_path);  // argv[0]
  argv_storage.push_back("--vsql-precheck-extension");
  argv_storage.push_back(target_so_path);
  argv_storage.push_back("--vsql-precheck-protocol");
  argv_storage.push_back(protocol_str);
  argv_storage.push_back("--vsql-precheck-result-fd");
  argv_storage.push_back(result_fd_arg);
  argv_storage.push_back("--vsql-precheck-stderr-fd");
  argv_storage.push_back(stderr_fd_arg);

  std::vector<char *> argv;
  argv.reserve(argv_storage.size() + 1);
  for (auto &s : argv_storage) argv.push_back(const_cast<char *>(s.c_str()));
  argv.push_back(nullptr);

  // File actions: dup write-ends into the child's fd 3/4, then close
  // the original parent-side fds. Also redirect stdin/stdout/stderr to
  // /dev/null so the child can't write to our terminal.
  posix_spawn_file_actions_t actions;
  if (::posix_spawn_file_actions_init(&actions) != 0) {
    out_error = std::string("posix_spawn_file_actions_init failed: ") +
                std::strerror(errno);
    return true;
  }
  bool actions_ok = true;
  actions_ok &= (::posix_spawn_file_actions_adddup2(
                     &actions, result_write.get(), 3) == 0);
  actions_ok &= (::posix_spawn_file_actions_adddup2(
                     &actions, stderr_write.get(), 4) == 0);
  // Close the original parent-side write ends in the child after dup2.
  actions_ok &=
      (::posix_spawn_file_actions_addclose(&actions, result_write.get()) == 0);
  actions_ok &=
      (::posix_spawn_file_actions_addclose(&actions, stderr_write.get()) == 0);
  // Close stdin/stdout/stderr; pre-check subprocess doesn't need them.
  // The dup2 to fd 4 covers our stderr-to-parent channel separately.
  actions_ok &=
      (::posix_spawn_file_actions_addopen(&actions, STDIN_FILENO, "/dev/null",
                                          O_RDONLY, 0) == 0);
  actions_ok &=
      (::posix_spawn_file_actions_addopen(&actions, STDOUT_FILENO, "/dev/null",
                                          O_WRONLY, 0) == 0);
  if (!actions_ok) {
    ::posix_spawn_file_actions_destroy(&actions);
    out_error = "failed to build posix_spawn file actions";
    return true;
  }

  pid_t pid = -1;
  const int spawn_err =
      ::posix_spawn(&pid, helper_path.c_str(), &actions, /*attrp=*/nullptr,
                    argv.data(), environ);
  ::posix_spawn_file_actions_destroy(&actions);

  if (spawn_err != 0) {
    out_error = std::string("posix_spawn failed: ") + std::strerror(spawn_err);
    return true;
  }

  // Close the parent-side copies of the write ends; only the child
  // should hold them open. This lets us see EOF on read after the
  // child exits without writing.
  result_write.reset();
  stderr_write.reset();

  // Wait for the child to write its frame, with a hard deadline.
  pollfd pfd;
  pfd.fd = result_read.get();
  pfd.events = POLLIN;
  pfd.revents = 0;
  int poll_rc = 0;
  do {
    poll_rc = ::poll(&pfd, 1, kPrecheckTimeoutMs);
  } while (poll_rc < 0 && errno == EINTR);

  if (poll_rc == 0) {
    // Timed out. Kill the child, reap, surface "timeout".
    ::kill(pid, SIGKILL);
    std::string stderr_text;
    drain_fd_to_string(stderr_read.get(), stderr_text);
    (void)reap_child(pid);
    out_error = std::string("pre-check subprocess timed out after ") +
                std::to_string(kPrecheckTimeoutMs / 1000) + "s";
    if (!stderr_text.empty()) out_error += "; stderr: " + stderr_text;
    return true;
  }
  if (poll_rc < 0) {
    ::kill(pid, SIGKILL);
    std::string stderr_text;
    drain_fd_to_string(stderr_read.get(), stderr_text);
    (void)reap_child(pid);
    out_error = std::string("poll(result_fd) failed: ") + std::strerror(errno);
    if (!stderr_text.empty()) out_error += "; stderr: " + stderr_text;
    return true;
  }

  // Read and parse the framed result.
  PrecheckMessage msg;
  std::string parse_error;
  const bool parse_failed =
      ReadPrecheckFrame(result_read.get(), msg, parse_error);

  // Drain stderr regardless of parse outcome.
  std::string stderr_text;
  drain_fd_to_string(stderr_read.get(), stderr_text);

  // Reap the child.
  const std::string reap_status = reap_child(pid);

  if (parse_failed) {
    out_error = std::string("pre-check subprocess result parse failed: ") +
                parse_error;
    if (!reap_status.empty()) out_error += "; " + reap_status;
    if (!stderr_text.empty()) out_error += "; stderr: " + stderr_text;
    return true;
  }

  if (msg.kind == kHarvestFailure) {
    out_error = msg.error_message;
    // If the child died unexpectedly after writing failure, surface that
    // too -- helps diagnose cases where stderr has more context.
    if (!reap_status.empty()) out_error += " [" + reap_status + "]";
    return true;
  }

  // kHarvestSuccess. The child should have exited cleanly; warn (in the
  // returned message? no -- we succeeded) if it didn't, but treat a
  // non-zero exit after a success frame as a failure since something
  // went wrong inside the child after writing.
  if (!reap_status.empty()) {
    out_error = "pre-check subprocess wrote a success frame but " +
                reap_status;
    if (!stderr_text.empty()) out_error += "; stderr: " + stderr_text;
    return true;
  }

  out_types = std::move(msg.types);
  return false;
}

}  // namespace veb
}  // namespace villagesql
