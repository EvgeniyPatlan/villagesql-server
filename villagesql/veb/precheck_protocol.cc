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

#include "villagesql/veb/precheck_protocol.h"

#include <arpa/inet.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>

namespace villagesql {
namespace veb {

namespace {

// Header layout: u32 total_length + u16 protocol_version + u16 kind.
constexpr size_t kHeaderBytes = 8;

// 64-bit byte order helpers. htonll/ntohll aren't in POSIX; do it by hand.
uint64_t hton64(uint64_t v) {
  // Detect host endianness at runtime once; cheap and portable.
  const uint16_t probe = 1;
  const bool host_is_le = (*reinterpret_cast<const uint8_t *>(&probe)) == 1;
  if (!host_is_le) return v;  // host is already big-endian
  return (static_cast<uint64_t>(htonl(static_cast<uint32_t>(v & 0xffffffff)))
          << 32) |
         htonl(static_cast<uint32_t>(v >> 32));
}

uint64_t ntoh64(uint64_t v) { return hton64(v); }  // symmetric

void append_u16(std::vector<uint8_t> &buf, uint16_t v) {
  const uint16_t n = htons(v);
  buf.push_back(static_cast<uint8_t>(n & 0xff));
  buf.push_back(static_cast<uint8_t>((n >> 8) & 0xff));
}

void append_u32(std::vector<uint8_t> &buf, uint32_t v) {
  const uint32_t n = htonl(v);
  for (int i = 0; i < 4; ++i)
    buf.push_back(static_cast<uint8_t>((n >> (i * 8)) & 0xff));
}

void append_i64(std::vector<uint8_t> &buf, int64_t v) {
  const uint64_t n = hton64(static_cast<uint64_t>(v));
  for (int i = 0; i < 8; ++i)
    buf.push_back(static_cast<uint8_t>((n >> (i * 8)) & 0xff));
}

void append_bytes(std::vector<uint8_t> &buf, const std::string &s) {
  buf.insert(buf.end(), s.begin(), s.end());
}

// Write exactly `len` bytes to `fd`, handling short writes and EINTR.
// Returns false on success, true on failure.
bool write_full(int fd, const uint8_t *data, size_t len) {
  while (len > 0) {
    const ssize_t n = ::write(fd, data, len);
    if (n < 0) {
      if (errno == EINTR) continue;
      return true;
    }
    if (n == 0) return true;  // shouldn't happen on a pipe but treat as error
    data += n;
    len -= static_cast<size_t>(n);
  }
  return false;
}

// Read exactly `len` bytes from `fd`. Returns false on success, true on
// failure (EOF before len bytes, or read error).
bool read_full(int fd, uint8_t *data, size_t len) {
  while (len > 0) {
    const ssize_t n = ::read(fd, data, len);
    if (n < 0) {
      if (errno == EINTR) continue;
      return true;
    }
    if (n == 0) return true;  // EOF before we got everything
    data += n;
    len -= static_cast<size_t>(n);
  }
  return false;
}

// Cursor-based body reader. Validates every advance against the body
// length so a truncated or mis-sized frame can't read past the buffer.
struct Cursor {
  const uint8_t *p;
  size_t remaining;

  bool read_u16(uint16_t &out) {
    if (remaining < 2) return true;
    uint16_t n;
    std::memcpy(&n, p, 2);
    out = ntohs(n);
    p += 2;
    remaining -= 2;
    return false;
  }

  bool read_u32(uint32_t &out) {
    if (remaining < 4) return true;
    uint32_t n;
    std::memcpy(&n, p, 4);
    out = ntohl(n);
    p += 4;
    remaining -= 4;
    return false;
  }

  bool read_i64(int64_t &out) {
    if (remaining < 8) return true;
    uint64_t n;
    std::memcpy(&n, p, 8);
    out = static_cast<int64_t>(ntoh64(n));
    p += 8;
    remaining -= 8;
    return false;
  }

  bool read_string(uint32_t len, std::string &out) {
    if (remaining < len) return true;
    out.assign(reinterpret_cast<const char *>(p), len);
    p += len;
    remaining -= len;
    return false;
  }
};

// Frame header writer. Builds the body first into `body`, then prepends
// the header and writes the whole frame to `fd`.
bool write_frame(int fd, PrecheckMessageKind kind,
                 const std::vector<uint8_t> &body) {
  const size_t total = kHeaderBytes + body.size();
  if (total > kMaxFrameBytes) {
    errno = EMSGSIZE;
    return true;
  }
  std::vector<uint8_t> frame;
  frame.reserve(total);
  append_u32(frame, static_cast<uint32_t>(total));
  append_u16(frame, kPrecheckProtocolVersion);
  append_u16(frame, static_cast<uint16_t>(kind));
  frame.insert(frame.end(), body.begin(), body.end());
  return write_full(fd, frame.data(), frame.size());
}

}  // namespace

bool WriteHarvestSuccessFrame(int fd,
                              const std::vector<HarvestedTargetType> &types) {
  if (types.size() > kMaxTypes) {
    errno = E2BIG;
    return true;
  }
  std::vector<uint8_t> body;
  append_u32(body, static_cast<uint32_t>(types.size()));
  for (const auto &t : types) {
    if (t.type_name.size() > kMaxTypeNameBytes) {
      errno = E2BIG;
      return true;
    }
    append_u32(body, static_cast<uint32_t>(t.type_name.size()));
    append_bytes(body, t.type_name);
    append_i64(body, t.persisted_length);
  }
  return write_frame(fd, kHarvestSuccess, body);
}

bool WriteHarvestFailureFrame(int fd, const std::string &error_message) {
  if (error_message.size() > kMaxErrorBytes) {
    // Truncate rather than fail; the operator should still see something.
    return WriteHarvestFailureFrame(
        fd, error_message.substr(0, kMaxErrorBytes - 16) + " ...[truncated]");
  }
  std::vector<uint8_t> body;
  append_u32(body, static_cast<uint32_t>(error_message.size()));
  append_bytes(body, error_message);
  return write_frame(fd, kHarvestFailure, body);
}

bool ReadPrecheckFrame(int fd, PrecheckMessage &out,
                       std::string &parse_error) {
  // Read header.
  uint8_t header[kHeaderBytes];
  if (read_full(fd, header, kHeaderBytes)) {
    parse_error = "truncated frame header";
    return true;
  }
  uint32_t total_length;
  uint16_t version;
  uint16_t kind_raw;
  std::memcpy(&total_length, header, 4);
  total_length = ntohl(total_length);
  std::memcpy(&version, header + 4, 2);
  version = ntohs(version);
  std::memcpy(&kind_raw, header + 6, 2);
  kind_raw = ntohs(kind_raw);

  if (total_length < kHeaderBytes) {
    parse_error = "frame length smaller than header";
    return true;
  }
  if (total_length > kMaxFrameBytes) {
    parse_error = "frame too large";
    return true;
  }
  if (version != kPrecheckProtocolVersion) {
    parse_error =
        std::string("unsupported protocol version ") + std::to_string(version);
    return true;
  }
  if (kind_raw != kHarvestSuccess && kind_raw != kHarvestFailure) {
    parse_error =
        std::string("unknown message kind ") + std::to_string(kind_raw);
    return true;
  }

  // Read body.
  const size_t body_len = total_length - kHeaderBytes;
  std::vector<uint8_t> body(body_len);
  if (body_len > 0 && read_full(fd, body.data(), body_len)) {
    parse_error = "truncated frame body";
    return true;
  }

  Cursor c{body.data(), body_len};
  if (kind_raw == kHarvestSuccess) {
    uint32_t type_count;
    if (c.read_u32(type_count)) {
      parse_error = "truncated success body (type_count)";
      return true;
    }
    if (type_count > kMaxTypes) {
      parse_error = "type_count exceeds cap";
      return true;
    }
    out.kind = kHarvestSuccess;
    out.types.clear();
    out.types.reserve(type_count);
    for (uint32_t i = 0; i < type_count; ++i) {
      uint32_t name_len;
      if (c.read_u32(name_len)) {
        parse_error = "truncated type entry (name_length)";
        return true;
      }
      if (name_len > kMaxTypeNameBytes) {
        parse_error = "type_name length exceeds cap";
        return true;
      }
      HarvestedTargetType t;
      if (c.read_string(name_len, t.type_name)) {
        parse_error = "truncated type entry (name bytes)";
        return true;
      }
      if (c.read_i64(t.persisted_length)) {
        parse_error = "truncated type entry (persisted_length)";
        return true;
      }
      out.types.push_back(std::move(t));
    }
  } else {
    uint32_t err_len;
    if (c.read_u32(err_len)) {
      parse_error = "truncated failure body (error_length)";
      return true;
    }
    if (err_len > kMaxErrorBytes) {
      parse_error = "error message length exceeds cap";
      return true;
    }
    out.kind = kHarvestFailure;
    if (c.read_string(err_len, out.error_message)) {
      parse_error = "truncated failure body (error bytes)";
      return true;
    }
  }

  if (c.remaining != 0) {
    parse_error = "trailing bytes after declared body length";
    return true;
  }

  return false;
}

}  // namespace veb
}  // namespace villagesql
