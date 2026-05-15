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

// TODO(villagesql-indexing): When CREATE INDEX ... USING EXTENDED is wired
// end-to-end through VEF, add a functional mysql-test
// (custom_index_prevents_extension_uninstall.test) that exercises this
// gating through the SQL surface. Until then, the unit tests below pin the
// predicate directly.

#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "unittest/gunit/test_utils.h"
#include "villagesql/schema/systable/custom_indexes.h"
#include "villagesql/schema/systable/extensions.h"
#include "villagesql/schema/systable/helpers.h"
#include "villagesql/veb/extension_uninstall_checks.h"

namespace villagesql_unittest {

using namespace villagesql;

class ExtensionUninstallChecksTest : public ::testing::Test {
 protected:
  void SetUp() override {
    villagesql::test_set_lower_case_table_names(0);
    system_charset_info = &my_charset_utf8mb4_0900_ai_ci;
  }
};

TEST_F(ExtensionUninstallChecksTest, CustomIndexPreventsExtensionUninstall) {
  ExtensionEntry ext(ExtensionKey("vsql_test"), "1.0.0", /*hash=*/{});

  IndexEntry idx(IndexKey("mydb", "t", "idx"), /*id=*/1, "vsql_test", "1.0.0",
                 "test_idx");
  std::vector<const IndexEntry *> all_indexes = {&idx};

  EXPECT_TRUE(check_for_indexes_of_extension(ext, all_indexes));
}

TEST_F(ExtensionUninstallChecksTest,
       OtherExtensionsIndexDoesNotPreventUninstall) {
  ExtensionEntry ext(ExtensionKey("vsql_test"), "1.0.0", /*hash=*/{});

  IndexEntry idx(IndexKey("mydb", "t", "idx"), /*id=*/1, "other_ext", "1.0.0",
                 "test_idx");
  std::vector<const IndexEntry *> all_indexes = {&idx};

  EXPECT_FALSE(check_for_indexes_of_extension(ext, all_indexes));
}

TEST_F(ExtensionUninstallChecksTest, VersionMismatchDoesNotPreventUninstall) {
  ExtensionEntry ext(ExtensionKey("vsql_test"), "2.0.0", /*hash=*/{});

  IndexEntry idx(IndexKey("mydb", "t", "idx"), /*id=*/1, "vsql_test", "1.0.0",
                 "test_idx");
  std::vector<const IndexEntry *> all_indexes = {&idx};

  EXPECT_FALSE(check_for_indexes_of_extension(ext, all_indexes));
}

TEST_F(ExtensionUninstallChecksTest, NoIndexesIsAllowed) {
  ExtensionEntry ext(ExtensionKey("vsql_test"), "1.0.0", /*hash=*/{});
  std::vector<const IndexEntry *> all_indexes = {};

  EXPECT_FALSE(check_for_indexes_of_extension(ext, all_indexes));
}

}  // namespace villagesql_unittest
