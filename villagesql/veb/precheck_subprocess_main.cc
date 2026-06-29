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

#include "villagesql/veb/precheck_subprocess_main.h"

#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "villagesql/veb/precheck_harvest.h"
#include "villagesql/veb/precheck_protocol.h"

namespace villagesql {
namespace veb {

namespace {

constexpr const char *kExtensionFlag = "--vsql-precheck-extension";
constexpr const char *kProtocolFlag = "--vsql-precheck-protocol";
constexpr const char *kResultFdFlag = "--vsql-precheck-result-fd";
constexpr const char *kStderrFdFlag = "--vsql-precheck-stderr-fd";

// True if `arg` is exactly `flag`, or starts with `flag` followed by '='.
bool arg_matches(const char *arg, const char *flag) {
  const size_t flag_len = std::strlen(flag);
  if (std::strncmp(arg, flag, flag_len) != 0) return false;
  return arg[flag_len] == '\0' || arg[flag_len] == '=';
}

// Extract the value attached to a flag arg.
//   --foo=bar          → returns "bar", *consumed_next_arg unchanged.
//   --foo bar          → returns argv[i+1], *consumed_next_arg set true.
// Returns nullptr if the flag is the last arg with no '=' value.
const char *flag_value(int argc, char **argv, int i, const char *flag,
                       bool *consumed_next_arg) {
  *consumed_next_arg = false;
  const size_t flag_len = std::strlen(flag);
  if (argv[i][flag_len] == '=') return argv[i] + flag_len + 1;
  if (i + 1 >= argc) return nullptr;
  *consumed_next_arg = true;
  return argv[i + 1];
}

// Report a failure as a kHarvestFailure frame to the result fd if we
// have one, plus a stderr line so the parent's stderr-capture path also
// sees something. Returns the exit code to propagate (always non-zero).
int report_failure(int result_fd, const std::string &message) {
  std::fprintf(stderr, "vsql-precheck: %s\n", message.c_str());
  if (result_fd >= 0) {
    // Best-effort; ignore write failure (we're already failing).
    WriteHarvestFailureFrame(result_fd, message);
  }
  return 1;
}

}  // namespace

int precheck_subprocess_main(int argc, char **argv) {
  // Parse flags. We accept both `--foo value` and `--foo=value` forms.
  // argv[0] is the binary path; flags start at argv[1].
  std::string extension_path;
  int server_protocol = 0;
  int result_fd = -1;
  int stderr_fd = -1;

  bool have_extension = false;
  bool have_protocol = false;
  bool have_result_fd = false;

  for (int i = 1; i < argc; ++i) {
    const char *arg = argv[i];
    if (arg_matches(arg, kExtensionFlag)) {
      bool consumed = false;
      const char *v = flag_value(argc, argv, i, kExtensionFlag, &consumed);
      if (v == nullptr)
        return report_failure(-1, std::string("missing value for ") +
                                      kExtensionFlag);
      extension_path = v;
      have_extension = true;
      if (consumed) ++i;
    } else if (arg_matches(arg, kProtocolFlag)) {
      bool consumed = false;
      const char *v = flag_value(argc, argv, i, kProtocolFlag, &consumed);
      if (v == nullptr)
        return report_failure(
            -1, std::string("missing value for ") + kProtocolFlag);
      char *end = nullptr;
      const long parsed = std::strtol(v, &end, 10);
      if (end == v || *end != '\0' || parsed < 0)
        return report_failure(
            -1, std::string("invalid value for ") + kProtocolFlag + ": " + v);
      server_protocol = static_cast<int>(parsed);
      have_protocol = true;
      if (consumed) ++i;
    } else if (arg_matches(arg, kResultFdFlag)) {
      bool consumed = false;
      const char *v = flag_value(argc, argv, i, kResultFdFlag, &consumed);
      if (v == nullptr)
        return report_failure(
            -1, std::string("missing value for ") + kResultFdFlag);
      char *end = nullptr;
      const long parsed = std::strtol(v, &end, 10);
      if (end == v || *end != '\0' || parsed < 0 || parsed > 65535)
        return report_failure(
            -1, std::string("invalid value for ") + kResultFdFlag + ": " + v);
      result_fd = static_cast<int>(parsed);
      have_result_fd = true;
      if (consumed) ++i;
    } else if (arg_matches(arg, kStderrFdFlag)) {
      bool consumed = false;
      const char *v = flag_value(argc, argv, i, kStderrFdFlag, &consumed);
      if (v == nullptr)
        return report_failure(
            result_fd, std::string("missing value for ") + kStderrFdFlag);
      char *end = nullptr;
      const long parsed = std::strtol(v, &end, 10);
      if (end == v || *end != '\0' || parsed < 0 || parsed > 65535)
        return report_failure(
            result_fd,
            std::string("invalid value for ") + kStderrFdFlag + ": " + v);
      stderr_fd = static_cast<int>(parsed);
      if (consumed) ++i;
    } else {
      return report_failure(
          result_fd, std::string("unrecognized argument: ") + arg);
    }
  }

  // If the parent passed an explicit stderr fd, redirect our stderr to
  // it so the parent's captured-stderr pipe sees diagnostics.
  if (stderr_fd >= 0) {
    if (dup2(stderr_fd, STDERR_FILENO) < 0) {
      // dup2 failed -- best-effort, fall through with our existing stderr.
    }
    ::close(stderr_fd);
    stderr_fd = -1;
  }

  if (!have_extension)
    return report_failure(result_fd, std::string("missing required flag: ") +
                                         kExtensionFlag);
  if (!have_protocol)
    return report_failure(result_fd, std::string("missing required flag: ") +
                                         kProtocolFlag);
  if (!have_result_fd)
    return report_failure(result_fd, std::string("missing required flag: ") +
                                         kResultFdFlag);

  // Run the harvest in-process here in the child. The parent-side
  // HarvestTargetTypes is the function that posix_spawned *us*; calling
  // it from here would recurse infinitely. The in-process variant does
  // the real dlopen + vef_register work this subprocess exists for.
  std::vector<HarvestedTargetType> types;
  std::string harvest_error;
  if (HarvestTargetTypesInProcess(extension_path, server_protocol, types,
                                  harvest_error)) {
    return report_failure(result_fd, harvest_error);
  }

  // Write the framed success result. Frame write failure is fatal but
  // there's not much we can do beyond logging to stderr.
  if (WriteHarvestSuccessFrame(result_fd, types)) {
    std::fprintf(stderr,
                 "vsql-precheck: failed to write harvest success frame: "
                 "errno=%d\n",
                 errno);
    return 1;
  }

  return 0;
}

}  // namespace veb
}  // namespace villagesql
