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

#include <gtest/gtest.h>
#include <unistd.h>

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "villagesql/veb/precheck_protocol.h"

namespace villagesql_unittest {

using namespace villagesql::veb;

// A pipe pair with RAII close. Tests write the parent-side frame to
// write_fd and read it back from read_fd; simulates the parent/child
// pipe without spawning a real subprocess.
class Pipe {
 public:
  Pipe() {
    int fds[2] = {-1, -1};
    if (pipe(fds) != 0) {
      read_fd_ = -1;
      write_fd_ = -1;
      return;
    }
    read_fd_ = fds[0];
    write_fd_ = fds[1];
  }
  ~Pipe() {
    if (read_fd_ >= 0) ::close(read_fd_);
    if (write_fd_ >= 0) ::close(write_fd_);
  }
  Pipe(const Pipe &) = delete;
  Pipe &operator=(const Pipe &) = delete;

  int read_fd() const { return read_fd_; }
  int write_fd() const { return write_fd_; }

  // Close the write end so subsequent reads see EOF.
  void close_write() {
    if (write_fd_ >= 0) {
      ::close(write_fd_);
      write_fd_ = -1;
    }
  }

 private:
  int read_fd_{-1};
  int write_fd_{-1};
};

class PrecheckProtocolTest : public ::testing::Test {};

// ---- Round-trip success cases ----

TEST_F(PrecheckProtocolTest, RoundTripEmptySuccess) {
  Pipe p;
  ASSERT_GE(p.read_fd(), 0);

  std::vector<HarvestedTargetType> in;
  ASSERT_FALSE(WriteHarvestSuccessFrame(p.write_fd(), in));

  PrecheckMessage out;
  std::string err;
  ASSERT_FALSE(ReadPrecheckFrame(p.read_fd(), out, err)) << err;
  EXPECT_EQ(kHarvestSuccess, out.kind);
  EXPECT_TRUE(out.types.empty());
  EXPECT_TRUE(out.error_message.empty());
}

TEST_F(PrecheckProtocolTest, RoundTripSingleType) {
  Pipe p;
  std::vector<HarvestedTargetType> in;
  HarvestedTargetType t;
  t.type_name = "COUNTER";
  t.persisted_length = 4;
  in.push_back(t);

  ASSERT_FALSE(WriteHarvestSuccessFrame(p.write_fd(), in));

  PrecheckMessage out;
  std::string err;
  ASSERT_FALSE(ReadPrecheckFrame(p.read_fd(), out, err)) << err;
  EXPECT_EQ(kHarvestSuccess, out.kind);
  ASSERT_EQ(1u, out.types.size());
  EXPECT_EQ("COUNTER", out.types[0].type_name);
  EXPECT_EQ(4, out.types[0].persisted_length);
}

TEST_F(PrecheckProtocolTest, RoundTripMultipleTypes) {
  Pipe p;
  std::vector<HarvestedTargetType> in;
  for (int i = 0; i < 16; ++i) {
    HarvestedTargetType t;
    t.type_name = "type_" + std::to_string(i);
    t.persisted_length = i * 8 - 1;  // exercise non-trivial values
    in.push_back(t);
  }

  ASSERT_FALSE(WriteHarvestSuccessFrame(p.write_fd(), in));

  PrecheckMessage out;
  std::string err;
  ASSERT_FALSE(ReadPrecheckFrame(p.read_fd(), out, err)) << err;
  ASSERT_EQ(in.size(), out.types.size());
  for (size_t i = 0; i < in.size(); ++i) {
    EXPECT_EQ(in[i].type_name, out.types[i].type_name);
    EXPECT_EQ(in[i].persisted_length, out.types[i].persisted_length);
  }
}

TEST_F(PrecheckProtocolTest, RoundTripFailure) {
  Pipe p;
  const std::string msg = "Cannot update extension 'foo': bad thing happened";
  ASSERT_FALSE(WriteHarvestFailureFrame(p.write_fd(), msg));

  PrecheckMessage out;
  std::string err;
  ASSERT_FALSE(ReadPrecheckFrame(p.read_fd(), out, err)) << err;
  EXPECT_EQ(kHarvestFailure, out.kind);
  EXPECT_TRUE(out.types.empty());
  EXPECT_EQ(msg, out.error_message);
}

TEST_F(PrecheckProtocolTest, RoundTripEmptyFailureMessage) {
  Pipe p;
  ASSERT_FALSE(WriteHarvestFailureFrame(p.write_fd(), ""));

  PrecheckMessage out;
  std::string err;
  ASSERT_FALSE(ReadPrecheckFrame(p.read_fd(), out, err)) << err;
  EXPECT_EQ(kHarvestFailure, out.kind);
  EXPECT_EQ("", out.error_message);
}

TEST_F(PrecheckProtocolTest, RoundTripNegativePersistedLength) {
  // Variable-length types use -1; the wire format must round-trip
  // signed values correctly.
  Pipe p;
  std::vector<HarvestedTargetType> in;
  HarvestedTargetType t;
  t.type_name = "var";
  t.persisted_length = -1;
  in.push_back(t);

  ASSERT_FALSE(WriteHarvestSuccessFrame(p.write_fd(), in));

  PrecheckMessage out;
  std::string err;
  ASSERT_FALSE(ReadPrecheckFrame(p.read_fd(), out, err)) << err;
  ASSERT_EQ(1u, out.types.size());
  EXPECT_EQ(-1, out.types[0].persisted_length);
}

// ---- Writer cap enforcement ----

TEST_F(PrecheckProtocolTest, WriteFailsOnTooManyTypes) {
  Pipe p;
  std::vector<HarvestedTargetType> in;
  // kMaxTypes + 1 entries triggers the writer's E2BIG.
  in.resize(kMaxTypes + 1);
  for (size_t i = 0; i < in.size(); ++i) in[i].type_name = "x";

  EXPECT_TRUE(WriteHarvestSuccessFrame(p.write_fd(), in));
}

TEST_F(PrecheckProtocolTest, WriteFailsOnOversizeTypeName) {
  Pipe p;
  std::vector<HarvestedTargetType> in;
  HarvestedTargetType t;
  t.type_name.assign(kMaxTypeNameBytes + 1, 'a');
  in.push_back(t);

  EXPECT_TRUE(WriteHarvestSuccessFrame(p.write_fd(), in));
}

TEST_F(PrecheckProtocolTest, WriteFailureTruncatesOversizeErrorMessage) {
  Pipe p;
  std::string huge(kMaxErrorBytes * 2, 'x');
  ASSERT_FALSE(WriteHarvestFailureFrame(p.write_fd(), huge));

  PrecheckMessage out;
  std::string err;
  ASSERT_FALSE(ReadPrecheckFrame(p.read_fd(), out, err)) << err;
  EXPECT_EQ(kHarvestFailure, out.kind);
  EXPECT_LE(out.error_message.size(), kMaxErrorBytes);
  // Truncation marker present.
  EXPECT_NE(out.error_message.find("[truncated]"), std::string::npos);
}

// ---- Reader rejections of malformed input ----

namespace {
// Write raw bytes to fd, bypassing the framing helpers. Used to inject
// adversarial payloads.
void write_raw(int fd, const std::vector<uint8_t> &bytes) {
  ssize_t n = ::write(fd, bytes.data(), bytes.size());
  ASSERT_EQ(static_cast<ssize_t>(bytes.size()), n);
}
}  // namespace

TEST_F(PrecheckProtocolTest, ReadRejectsTruncatedHeader) {
  Pipe p;
  // Write only 3 bytes; header is 8.
  const std::vector<uint8_t> partial{0, 0, 0};
  write_raw(p.write_fd(), partial);
  p.close_write();

  PrecheckMessage out;
  std::string err;
  EXPECT_TRUE(ReadPrecheckFrame(p.read_fd(), out, err));
  EXPECT_NE(err.find("truncated"), std::string::npos);
}

TEST_F(PrecheckProtocolTest, ReadRejectsFrameSmallerThanHeader) {
  Pipe p;
  // Build a header claiming total_length=4, version=1, kind=1. That
  // length is less than the header itself; reader must reject.
  std::vector<uint8_t> bad{0, 0, 0, 4, 0, 1, 0, 1};
  write_raw(p.write_fd(), bad);
  p.close_write();

  PrecheckMessage out;
  std::string err;
  EXPECT_TRUE(ReadPrecheckFrame(p.read_fd(), out, err));
  EXPECT_NE(err.find("smaller than header"), std::string::npos);
}

TEST_F(PrecheckProtocolTest, ReadRejectsTooLargeFrame) {
  Pipe p;
  // total_length = kMaxFrameBytes + 1, version=1, kind=1.
  const uint32_t too_big = kMaxFrameBytes + 1;
  std::vector<uint8_t> bad{
      static_cast<uint8_t>((too_big >> 24) & 0xff),
      static_cast<uint8_t>((too_big >> 16) & 0xff),
      static_cast<uint8_t>((too_big >> 8) & 0xff),
      static_cast<uint8_t>(too_big & 0xff),
      0, 1, 0, 1};
  write_raw(p.write_fd(), bad);
  p.close_write();

  PrecheckMessage out;
  std::string err;
  EXPECT_TRUE(ReadPrecheckFrame(p.read_fd(), out, err));
  EXPECT_NE(err.find("too large"), std::string::npos);
}

TEST_F(PrecheckProtocolTest, ReadRejectsUnknownProtocolVersion) {
  Pipe p;
  // total_length=8, version=0xff, kind=1.
  std::vector<uint8_t> bad{0, 0, 0, 8, 0, 0xff, 0, 1};
  write_raw(p.write_fd(), bad);
  p.close_write();

  PrecheckMessage out;
  std::string err;
  EXPECT_TRUE(ReadPrecheckFrame(p.read_fd(), out, err));
  EXPECT_NE(err.find("protocol version"), std::string::npos);
}

TEST_F(PrecheckProtocolTest, ReadRejectsUnknownMessageKind) {
  Pipe p;
  // total_length=8, version=1, kind=99.
  std::vector<uint8_t> bad{0, 0, 0, 8, 0, 1, 0, 99};
  write_raw(p.write_fd(), bad);
  p.close_write();

  PrecheckMessage out;
  std::string err;
  EXPECT_TRUE(ReadPrecheckFrame(p.read_fd(), out, err));
  EXPECT_NE(err.find("message kind"), std::string::npos);
}

TEST_F(PrecheckProtocolTest, ReadRejectsTruncatedBody) {
  Pipe p;
  // Header says total_length=16 (8 header + 8 body) but we only write
  // 4 body bytes.
  std::vector<uint8_t> bad{0, 0, 0, 16, 0, 1, 0, 1, 0, 0, 0, 1};
  write_raw(p.write_fd(), bad);
  p.close_write();

  PrecheckMessage out;
  std::string err;
  EXPECT_TRUE(ReadPrecheckFrame(p.read_fd(), out, err));
  EXPECT_NE(err.find("truncated"), std::string::npos);
}

TEST_F(PrecheckProtocolTest, ReadRejectsTypeCountAboveCap) {
  Pipe p;
  // Header: total_length=12, version=1, kind=kHarvestSuccess.
  // Body: type_count=kMaxTypes+1 (no actual entries; the reader must
  // reject before trying to read them).
  const uint32_t bad_count = kMaxTypes + 1;
  std::vector<uint8_t> bad{
      0, 0, 0, 12, 0, 1, 0, 1,
      static_cast<uint8_t>((bad_count >> 24) & 0xff),
      static_cast<uint8_t>((bad_count >> 16) & 0xff),
      static_cast<uint8_t>((bad_count >> 8) & 0xff),
      static_cast<uint8_t>(bad_count & 0xff)};
  write_raw(p.write_fd(), bad);
  p.close_write();

  PrecheckMessage out;
  std::string err;
  EXPECT_TRUE(ReadPrecheckFrame(p.read_fd(), out, err));
  EXPECT_NE(err.find("type_count"), std::string::npos);
}

TEST_F(PrecheckProtocolTest, ReadRejectsTypeNameLengthAboveCap) {
  Pipe p;
  // Build a success body with one type whose declared name_length is
  // kMaxTypeNameBytes + 1. Reader must reject before reading the bytes.
  const uint32_t bad_name_len = kMaxTypeNameBytes + 1;
  // body = u32 type_count(1) + u32 name_length(bad)
  // Don't include name bytes; reader rejects at the length check.
  std::vector<uint8_t> body{
      0, 0, 0, 1,  // type_count = 1
      static_cast<uint8_t>((bad_name_len >> 24) & 0xff),
      static_cast<uint8_t>((bad_name_len >> 16) & 0xff),
      static_cast<uint8_t>((bad_name_len >> 8) & 0xff),
      static_cast<uint8_t>(bad_name_len & 0xff),
  };
  const uint32_t total = 8 + body.size();
  std::vector<uint8_t> frame{
      static_cast<uint8_t>((total >> 24) & 0xff),
      static_cast<uint8_t>((total >> 16) & 0xff),
      static_cast<uint8_t>((total >> 8) & 0xff),
      static_cast<uint8_t>(total & 0xff),
      0, 1, 0, 1};
  frame.insert(frame.end(), body.begin(), body.end());
  write_raw(p.write_fd(), frame);
  p.close_write();

  PrecheckMessage out;
  std::string err;
  EXPECT_TRUE(ReadPrecheckFrame(p.read_fd(), out, err));
  EXPECT_NE(err.find("type_name length"), std::string::npos);
}

TEST_F(PrecheckProtocolTest, ReadRejectsTrailingBytesAfterDeclaredLength) {
  Pipe p;
  // Build a valid empty-success frame, then append a single u32 type_count
  // claiming 0. The total_length header says only the type_count fits in
  // the body; the trailing bytes after that fit but should be rejected.
  // Trick: claim total_length = header(8) + u32(4) = 12, but actually
  // include 4 extra bytes after.
  std::vector<uint8_t> frame{
      0, 0, 0, 12,  // total_length=12 (header + 4 body bytes)
      0, 1,         // version=1
      0, 1,         // kind=kHarvestSuccess
      0, 0, 0, 0    // type_count=0
  };
  // Append junk that the reader should never see (it stops at total_length).
  // To exercise the "trailing bytes" check, build a body that has more
  // bytes than the parser needs. Claim 1 extra body byte over what the
  // success path reads.
  std::vector<uint8_t> bad{
      0, 0, 0, 13,  // total_length=13 (header + 5 body bytes)
      0, 1,         // version=1
      0, 1,         // kind=kHarvestSuccess
      0, 0, 0, 0,   // type_count=0
      0x00          // one extra byte after the parser is done
  };
  write_raw(p.write_fd(), bad);
  p.close_write();

  PrecheckMessage out;
  std::string err;
  EXPECT_TRUE(ReadPrecheckFrame(p.read_fd(), out, err));
  EXPECT_NE(err.find("trailing bytes"), std::string::npos);
}

}  // namespace villagesql_unittest
