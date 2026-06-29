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

#ifndef VILLAGESQL_VEB_PRECHECK_PROTOCOL_H_
#define VILLAGESQL_VEB_PRECHECK_PROTOCOL_H_

// Framed binary protocol for the pre-check subprocess (Phase 2 of the
// ALTER EXTENSION ... AT RESTART hardening). The parent forks a sandboxed
// mysqld child to harvest type metadata from a candidate extension's .so;
// the child writes one of these frames to the result pipe and exits.
//
// The protocol is internal between two instances of the same mysqld
// binary. It is NOT part of any VEF ABI. Bump kProtocolVersion freely.
// Child and parent are guaranteed to be the same build (the parent
// posix_spawns /proc/self/exe or equivalent), so version mismatch is a
// bug in the parent code, not an external concern.
//
// Reader (parent) treats the child as untrusted: the child has loaded
// extension code that could have written arbitrary bytes. Every length
// prefix and string is bounds-checked against fixed caps. See
// Docs/VEF_PRECHECK_SUBPROCESS_DESIGN.md for the threat model.

#include <cstdint>
#include <string>
#include <vector>

#include "villagesql/veb/precheck_harvest.h"

namespace villagesql {
namespace veb {

// Bumped when wire format changes. Parent and child are always the same
// build, so this is a sanity check, not a compatibility negotiation.
constexpr uint16_t kPrecheckProtocolVersion = 1;

// Hard caps to bound parser work and reject malformed/malicious input.
// These are deliberately conservative; real extensions are well under.
constexpr uint32_t kMaxFrameBytes = 1 << 20;   // 1 MiB total
constexpr uint32_t kMaxTypes = 256;            // types per harvest
constexpr uint32_t kMaxTypeNameBytes = 256;    // per type_name
constexpr uint32_t kMaxErrorBytes = 4096;      // per failure message

enum PrecheckMessageKind : uint16_t {
  kHarvestSuccess = 1,
  kHarvestFailure = 2,
};

// Write a kHarvestSuccess frame containing the harvested types to `fd`.
// Returns false on success, true on write failure (errno set).
//
// Frame layout:
//   header:
//     u32 total_length            (header + body, network byte order)
//     u16 protocol_version
//     u16 message_kind            (= kHarvestSuccess)
//   body:
//     u32 type_count
//     repeated type_count times:
//       u32 name_length
//       <name_length bytes>       (type_name, no NUL terminator)
//       i64 persisted_length      (network byte order, two's complement)
bool WriteHarvestSuccessFrame(int fd,
                              const std::vector<HarvestedTargetType> &types);

// Write a kHarvestFailure frame with an error message.
// Returns false on success, true on write failure (errno set).
//
// Frame layout:
//   header (same as above, message_kind = kHarvestFailure)
//   body:
//     u32 error_length
//     <error_length bytes>        (error message, no NUL terminator)
bool WriteHarvestFailureFrame(int fd, const std::string &error_message);

// Parsed message from the child. Exactly one of `types` (when kind is
// kHarvestSuccess) or `error_message` (when kind is kHarvestFailure) is
// populated.
struct PrecheckMessage {
  PrecheckMessageKind kind{kHarvestSuccess};
  std::vector<HarvestedTargetType> types;
  std::string error_message;
};

// Read and parse a complete frame from `fd`. Blocks until the full frame
// is read or an error occurs. Validates every length prefix and string
// against the hard caps above; rejects malformed input.
//
// Returns false on success, true on failure with `parse_error` set to a
// short reason (e.g. "frame too large", "truncated header", "type_name
// length exceeds cap").
bool ReadPrecheckFrame(int fd, PrecheckMessage &out,
                       std::string &parse_error);

}  // namespace veb
}  // namespace villagesql

#endif  // VILLAGESQL_VEB_PRECHECK_PROTOCOL_H_
