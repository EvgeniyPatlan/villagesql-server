# update_extension.inc - Update an installed extension to a new version
#
# Usage:
#   --let $extension_name = my_extension
#   --let $extension_version = 2.0.0
#   --source include/villagesql/update_extension.inc
#
# Optional:
#   --let $update_expect_error = ER_VILLAGESQL_GENERIC_ERROR
#     When set, the UPDATE command is expected to fail with this error code.
#     On success, outputs "Updated <name> to <version>".
#     On expected failure, outputs "Attempting to update <name> to <version> (expecting error)".
#
# Outputs generic messages so that result files are not sensitive to changes
# in the SQL syntax of the UPDATE command.

if (!$extension_name)
{
  --die ERROR: update_extension.inc requires $extension_name to be set
}

if (!$extension_version)
{
  --die ERROR: update_extension.inc requires $extension_version to be set
}

if ($update_expect_error)
{
  --echo Attempting to update $extension_name to $extension_version (expecting error)
  --disable_query_log
  --error $update_expect_error
  --eval ALTER EXTENSION $extension_name UPDATE TO '$extension_version'
  --enable_query_log
}

if (!$update_expect_error)
{
  --disable_query_log
  --eval ALTER EXTENSION $extension_name UPDATE TO '$extension_version'
  --enable_query_log
  --echo Updated $extension_name to $extension_version
}

# Reset variables
--let $extension_name =
--let $extension_version =
--let $update_expect_error =
--let $_update_error =
