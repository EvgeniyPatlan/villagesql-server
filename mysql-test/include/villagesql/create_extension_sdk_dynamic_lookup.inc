# create_extension_sdk_dynamic_lookup.inc - Build extension with -undefined dynamic_lookup
#
# Same as create_extension_sdk.inc but uses a CMakeLists template that adds
# target_link_options(ext_lib PRIVATE -undefined dynamic_lookup).
#
# Usage:
#   --let $extension_name = my_extension
#   --let $extension_version = 1.0.0
#   --let $extension_source = $MYSQL_TEST_DIR/suite/villagesql/std_data/my_extension.cc
#   --source include/villagesql/create_extension_sdk_dynamic_lookup.inc

if (!$extension_name)
{
  --die ERROR: create_extension_sdk_dynamic_lookup.inc requires $extension_name to be set
}

if (!$extension_source)
{
  --die ERROR: create_extension_sdk_dynamic_lookup.inc requires $extension_source to be set
}

# Default version if not specified
if (!$extension_version)
{
  --let $extension_version = 1.0.0
}

# Get paths
--let $build_dir = `SELECT @@basedir`
--let $veb_dir = `SELECT @@veb_dir`
--let $sdk_version = `SELECT @@villagesql_server_version`
--let $sdk_dir = $build_dir/villagesql-extension-sdk-$sdk_version
--let $ext_build_dir = $MYSQLTEST_VARDIR/tmp/ext_build_$extension_name

# Verify SDK exists
--exec test -d "$sdk_dir" || (echo "ERROR: SDK not found at $sdk_dir. Run 'make sdk' first." && exit 1)

# Create build directory structure
--exec rm -rf $ext_build_dir
--exec mkdir -p $ext_build_dir/src
--exec mkdir -p $ext_build_dir/build

# Copy source file
--exec cp $extension_source $ext_build_dir/src/extension.cc

# Create manifest.json
--exec printf '%s' '{"name": "$extension_name", "version": "$extension_version"}' > $ext_build_dir/manifest.json

# Create CMakeLists.txt from dynamic_lookup template
--let $cmake_template = $MYSQL_TEST_DIR/suite/villagesql/data/extension_creator/CMakeLists.txt.dynamic_lookup_template
--exec sed 's/@EXTENSION_NAME@/$extension_name/g' $cmake_template > $ext_build_dir/CMakeLists.txt

--echo Creating extension $extension_name with -undefined dynamic_lookup...

# Run cmake
--replace_result $MYSQLTEST_VARDIR MYSQLTEST_VARDIR $build_dir BUILD_DIR
--exec cd $ext_build_dir/build && cmake -DVillageSQL_SDK_DIR=$sdk_dir -DCMAKE_MODULE_PATH=$sdk_dir/lib/cmake/VillageSQLExtensionFramework -DVSQL_USE_DEV_ABI=ON .. > cmake_output.txt 2>&1 || (cat cmake_output.txt && exit 1)

# Run make
--replace_result $MYSQLTEST_VARDIR MYSQLTEST_VARDIR
--exec cd $ext_build_dir/build && make > make_output.txt 2>&1 || (cat make_output.txt && exit 1)

# Verify VEB was created
--file_exists $ext_build_dir/build/$extension_name.veb

# Move VEB to server's VEB directory
--exec mkdir -p $veb_dir
--exec cp $ext_build_dir/build/$extension_name.veb $veb_dir/

--echo Created $extension_name.veb

# Cleanup build directory
--exec rm -rf $ext_build_dir

# Reset variables
--let $extension_name =
--let $extension_version =
--let $extension_source =
