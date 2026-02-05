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

#include "unittest/gunit/test_utils.h"
#include "villagesql/schema/descriptor/type_descriptor.h"
#include "villagesql/schema/systable/helpers.h"

namespace villagesql_unittest {

// Dummy function pointers for testing
static bool dummy_encode(unsigned char *, size_t, const char *, size_t,
                         size_t *) {
  return false;  // Success
}

static bool dummy_decode(const unsigned char *, size_t, char *, size_t,
                         size_t *) {
  return false;  // Success
}

static int dummy_compare(const unsigned char *, size_t, const unsigned char *,
                         size_t) {
  return 0;  // Equal
}

static size_t dummy_hash(const unsigned char *, size_t) { return 42; }

class TypeDescriptorTest : public ::testing::Test {
 protected:
  void SetUp() override {
    // Reset lower_case_table_names to default for consistent testing
    villagesql::test_set_lower_case_table_names(0);
    // Set system character set
    system_charset_info = &my_charset_utf8mb4_0900_ai_ci;
  }
};

// Test TypeDescriptorKey construction and normalization
TEST_F(TypeDescriptorTest, KeyConstruction) {
  villagesql::TypeDescriptorKey key("COMPLEX", "my_extension", "1.0.0");

  // Key should be normalized (lowercase)
  EXPECT_EQ(key.str(), "complex.my_extension.1.0.0");
}

// Test TypeDescriptorKey comparison
TEST_F(TypeDescriptorTest, KeyComparison) {
  villagesql::TypeDescriptorKey key1("COMPLEX", "ext", "1.0");
  villagesql::TypeDescriptorKey key2("complex", "EXT", "1.0");
  villagesql::TypeDescriptorKey key3("VECTOR", "ext", "1.0");

  // Same normalized key should be equal
  EXPECT_EQ(key1, key2);
  EXPECT_EQ(key1.str(), key2.str());

  // Different keys should not be equal
  EXPECT_NE(key1, key3);
  EXPECT_LT(key1, key3);  // "complex" < "vector"
}

// Test TypeDescriptor construction
TEST_F(TypeDescriptorTest, Construction) {
  villagesql::TypeDescriptor desc(
      villagesql::TypeDescriptorKey("MYTYPE", "test_ext", "2.0.0"),
      1,    // implementation_type
      16,   // persisted_length
      256,  // max_decode_buffer_length
      dummy_encode, dummy_decode, dummy_compare, dummy_hash);

  // Check identity fields
  EXPECT_EQ(desc.type_name(), "MYTYPE");
  EXPECT_EQ(desc.extension_name(), "test_ext");
  EXPECT_EQ(desc.extension_version(), "2.0.0");

  // Check key is correctly constructed
  EXPECT_EQ(desc.key().str(), "mytype.test_ext.2.0.0");

  // Check implementation details
  EXPECT_EQ(desc.implementation_type(), 1);
  EXPECT_EQ(desc.persisted_length(), 16);
  EXPECT_EQ(desc.max_decode_buffer_length(), 256);

  // Check function pointers
  EXPECT_EQ(desc.encode(), dummy_encode);
  EXPECT_EQ(desc.decode(), dummy_decode);
  EXPECT_EQ(desc.compare(), dummy_compare);
  EXPECT_EQ(desc.hash(), dummy_hash);
}

// Test TypeDescriptor with nullptr hash (optional)
TEST_F(TypeDescriptorTest, ConstructionWithNullHash) {
  villagesql::TypeDescriptor desc(
      villagesql::TypeDescriptorKey("NOHASH", "ext", "1.0"), 0, 8, 64,
      dummy_encode, dummy_decode, dummy_compare, nullptr);

  EXPECT_EQ(desc.hash(), nullptr);
  EXPECT_NE(desc.encode(), nullptr);
  EXPECT_NE(desc.decode(), nullptr);
  EXPECT_NE(desc.compare(), nullptr);
}

// Test that TypeDescriptor can be used with SystemTableMap (compile check)
// This verifies the key_type typedef and key() method work correctly
TEST_F(TypeDescriptorTest, KeyTypeCompatibility) {
  villagesql::TypeDescriptor desc(
      villagesql::TypeDescriptorKey("TEST", "ext", "1.0"), 0, 8, 64,
      dummy_encode, dummy_decode, dummy_compare);

  // Verify key_type is TypeDescriptorKey
  static_assert(std::is_same_v<villagesql::TypeDescriptor::key_type,
                               villagesql::TypeDescriptorKey>,
                "key_type should be TypeDescriptorKey");

  // Verify key() returns the right type
  const villagesql::TypeDescriptorKey &key = desc.key();
  EXPECT_EQ(key.str(), "test.ext.1.0");
}

}  // namespace villagesql_unittest
