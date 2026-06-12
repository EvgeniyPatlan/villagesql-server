/*****************************************************************************

Copyright (c) 1996, 2026, Oracle and/or its affiliates.
Copyright (c) 2026 VillageSQL Contributors

This program is free software; you can redistribute it and/or modify it under
the terms of the GNU General Public License, version 2.0, as published by the
Free Software Foundation.

This program is designed to work with certain software (including
but not limited to OpenSSL) that is licensed under separate terms,
as designated in a particular file or component or in included license
documentation.  The authors of MySQL hereby grant you an additional
permission to link the program and your derivative works with the
separately licensed software that they have either included with
the program or referenced in the documentation.

This program is distributed in the hope that it will be useful, but WITHOUT
ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
FOR A PARTICULAR PURPOSE. See the GNU General Public License, version 2.0,
for more details.

You should have received a copy of the GNU General Public License along with
this program; if not, write to the Free Software Foundation, Inc.,
51 Franklin St, Fifth Floor, Boston, MA 02110-1301  USA

*****************************************************************************/

/** @file dict/dict0load.cc
 Loads to the memory cache database object definitions
 from dictionary tables

 Created 4/24/1996 Heikki Tuuri
 *******************************************************/

#include "current_thd.h"
#include "ha_prototypes.h"

#include <set>
#include <stack>
#include "dict0load.h"

#include "btr0btr.h"
#include "btr0pcur.h"
#include "dict0boot.h"
#include "dict0crea.h"
#include "dict0dd.h"
#include "dict0dict.h"
#include "dict0mem.h"
#include "dict0priv.h"
#include "dict0stats.h"
#include "fsp0file.h"
#include "fsp0sysspace.h"
#include "fts0priv.h"
#include "mach0data.h"

#include "my_dbug.h"

#include "fil0fil.h"
#include "fts0fts.h"
#include "mysql_version.h"
#include "page0page.h"
#include "rem0cmp.h"
#include "srv0srv.h"
#include "srv0start.h"
#include "ut0math.h"

/* If this flag is true, then we will load the cluster index's (and tables')
metadata even if it is marked as "corrupted". */
bool srv_load_corrupted = false;

/** Using the table->heap, copy the null-terminated filepath into
table->data_dir_path. The data directory path is derived from the
filepath by stripping the the table->name.m_name component suffix.
If the filepath is not of the correct form (".../db/table.ibd"),
then table->data_dir_path will remain nullptr.
@param[in,out]  table           table instance
@param[in]      filepath        filepath of tablespace */
void dict_save_data_dir_path(dict_table_t *table, char *filepath) {
  ut_ad(dict_sys_mutex_own());
  ut_ad(DICT_TF_HAS_DATA_DIR(table->flags));
  ut_ad(table->data_dir_path == nullptr);
  ut_a(Fil_path::has_suffix(IBD, filepath));

  /* Ensure this filepath is not the default filepath. */
  char *default_filepath = Fil_path::make("", table->name.m_name, IBD);

  if (default_filepath == nullptr) {
    /* Memory allocation problem. */
    return;
  }

  if (strcmp(filepath, default_filepath) != 0) {
    size_t pathlen = strlen(filepath);

    ut_a(pathlen < OS_FILE_MAX_PATH);
    ut_a(Fil_path::has_suffix(IBD, filepath));

    char *data_dir_path = mem_heap_strdup(table->heap, filepath);

    Fil_path::make_data_dir_path(data_dir_path);

    if (strlen(data_dir_path)) {
      table->data_dir_path = data_dir_path;
    }
  }

  ut::free(default_filepath);
}

void dict_get_and_save_space_name(dict_table_t *table) {
  /* Do this only for general tablespaces. */
  if (!DICT_TF_HAS_SHARED_SPACE(table->flags)) {
    return;
  }

  if (table->tablespace != nullptr) {
    return;
  }

  fil_space_t *space = fil_space_acquire_silent(table->space);

  if (space != nullptr) {
    /* Use this name unless it is a temporary general
    tablespace name and we can now replace it. */
    if (!srv_sys_tablespaces_open ||
        !dict_table_has_temp_general_tablespace_name(space->name)) {
      /* Use this tablespace name */
      table->tablespace = mem_heap_strdup(table->heap, space->name);

      fil_space_release(space);
      return;
    }
    fil_space_release(space);
  }
<<<<<<< 03d249ddfb1799b24d422eaf31a18170c9b59400

  if (use_cache) {
    fil_space_t *space = fil_space_acquire_silent(table->space);

    if (space != nullptr) {
      /* Use this name unless it is a temporary general
      tablespace name and we can now replace it. */
      if (!srv_sys_tablespaces_open ||
          !dict_table_has_temp_general_tablespace_name(space->name)) {
        /* Use this tablespace name */
        table->tablespace = mem_heap_strdup(table->heap, space->name);

        fil_space_release(space);
        return;
      }
      fil_space_release(space);
    }
  }
}

dict_table_t *dict_load_table(const char *name, bool cached,
                              dict_err_ignore_t ignore_err,
                              const std::string *prev_table) {
  dict_names_t fk_list;
  dict_names_t::iterator i;
  table_name_t table_name;
  dict_table_t *result;

  DBUG_TRACE;
  DBUG_PRINT("dict_load_table", ("loading table: '%s'", name));

  if (prev_table != nullptr && prev_table->compare(name) == 0) {
    return nullptr;
  }

  const std::string cur_table(name);

  ut_ad(dict_sys_mutex_own());

  result = dict_table_check_if_in_cache_low(name);

  table_name.m_name = const_cast<char *>(name);

  if (!result) {
    result = dict_load_table_one(table_name, cached, ignore_err, fk_list,
                                 &cur_table);
    while (!fk_list.empty()) {
      table_name_t fk_table_name;
      dict_table_t *fk_table;

      fk_table_name.m_name = const_cast<char *>(fk_list.front());
      fk_table = dict_table_check_if_in_cache_low(fk_table_name.m_name);
      if (!fk_table) {
        dict_load_table_one(fk_table_name, cached, ignore_err, fk_list,
                            &cur_table);
      }
      fk_list.pop_front();
    }
  }

  return result;
}

void dict_load_tablespace(dict_table_t *table, dict_err_ignore_t ignore_err) {
  ut_ad(!table->is_temporary());

  /* The system and temporary tablespaces are preloaded and always available. */
  if (fsp_is_system_or_temp_tablespace(table->space)) {
    return;
  }

  if (dict_table_is_discarded(table)) {
    ib::warn(ER_IB_MSG_204)
        << "Tablespace for table " << table->name << " is set as discarded.";
    table->ibd_file_missing = true;
    return;
  }

  /* A file-per-table table name is also the tablespace name.
  A general tablespace name is not the same as the table name.
  Use the general tablespace name if it can be read from the
  dictionary, if not use 'innodb_general_##. */
  char *shared_space_name = nullptr;
  std::string tablespace_name;
  const char *space_name;
  const char *tbl_name;

  if (DICT_TF_HAS_SHARED_SPACE(table->flags)) {
    if (table->space == dict_sys_t::s_dict_space_id) {
      shared_space_name = mem_strdup(dict_sys_t::s_dd_space_name);
    } else if (srv_sys_tablespaces_open) {
      shared_space_name = dict_space_get_name(table->space, nullptr);

    } else {
      /* Make the temporary tablespace name. */
      shared_space_name = static_cast<char *>(ut::malloc_withkey(
          UT_NEW_THIS_FILE_PSI_KEY, strlen(general_space_name) + 20));

      if (shared_space_name != nullptr)
        sprintf(shared_space_name, "%s_" ULINTPF, general_space_name,
                static_cast<ulint>(table->space));
    }
    tbl_name = shared_space_name;
    space_name = shared_space_name;

  } else {
    tbl_name = table->name.m_name;

    tablespace_name.assign(tbl_name);
    dict_name::convert_to_space(tablespace_name);
    space_name = tablespace_name.c_str();
  }

  /* The tablespace may already be open. */
  if (fil_space_exists_in_mem(table->space, space_name, false, true)) {
    ut::free(shared_space_name);
    return;
  }

  if (!(ignore_err & DICT_ERR_IGNORE_RECOVER_LOCK) && !srv_is_upgrade_mode) {
    ib::error(ER_IB_MSG_205)
        << "Failed to find tablespace for table " << table->name
        << " in the cache. Attempting"
           " to load the tablespace with space id "
        << table->space;
  }

  /* Use the remote filepath if needed. This parameter is optional
  in the call to fil_ibd_open(). If not supplied, it will be built
  from the space_name. */
  char *filepath = nullptr;
  if (DICT_TF_HAS_DATA_DIR(table->flags)) {
    /* This will set table->data_dir_path from either
    fil_system or SYS_DATAFILES */
    dict_get_and_save_data_dir_path(table, true);

    if (table->data_dir_path != nullptr) {
      filepath = Fil_path::make(table->data_dir_path, table->name.m_name, IBD);
    }

  } else if (DICT_TF_HAS_SHARED_SPACE(table->flags)) {
    /* Set table->tablespace from either
    fil_system or SYS_TABLESPACES */
    dict_get_and_save_space_name(table);

    /* Set the filepath from either
    fil_system or SYS_DATAFILES. */
    filepath = dict_get_first_path(table->space);
    if (filepath == nullptr) {
      ib::warn(ER_IB_MSG_206) << "Could not find the filepath"
                                 " for table "
                              << table->name << ", space ID " << table->space;
    }
  }

  /* Try to open the tablespace.  We set the 2nd param (fix_dict) to
  false because we do not have an x-lock on dict_operation_lock */
  uint32_t fsp_flags = dict_tf_to_fsp_flags(table->flags);
  /* Set tablespace encryption flag */
  if (DICT_TF2_FLAG_IS_SET(table, DICT_TF2_ENCRYPTION_FILE_PER_TABLE)) {
    fsp_flags_set_encryption(fsp_flags);
  }

  /* This dict_load_tablespace() is only used on old 5.7 database during
  upgrade */
  dberr_t err = fil_ibd_open(true, FIL_TYPE_TABLESPACE, table->space, fsp_flags,
                             space_name, filepath, true, true);

  if (err != DB_SUCCESS) {
    /* We failed to find a sensible tablespace file */
    table->ibd_file_missing = true;
  }

  ut::free(shared_space_name);
  ut::free(filepath);
}

static dict_table_t *dict_load_table_one(table_name_t &name, bool cached,
                                         dict_err_ignore_t ignore_err,
                                         dict_names_t &fk_tables,
                                         const std::string *prev_table) {
  dberr_t err;
  dict_table_t *table;
  btr_pcur_t pcur;
  dict_index_t *sys_index;
  dtuple_t *tuple;
  mem_heap_t *heap;
  dfield_t *dfield;
  const rec_t *rec;
  const byte *field;
  ulint len;
  const char *err_msg;
  mtr_t mtr;

  DBUG_TRACE;
  DBUG_PRINT("dict_load_table_one", ("table: %s", name.m_name));

  ut_ad(dict_sys_mutex_own());

  dict_table_t *sys_tables = dict_table_get_low("SYS_TABLES", prev_table);
  if (sys_tables == nullptr) {
    return nullptr;
  }

  heap = mem_heap_create(32000, UT_LOCATION_HERE);

  mtr_start(&mtr);
  sys_index = UT_LIST_GET_FIRST(sys_tables->indexes);
  ut_ad(!dict_table_is_comp(sys_tables));
  ut_ad(name_of_col_is(sys_tables, sys_index, DICT_FLD__SYS_TABLES__ID, "ID"));
  ut_ad(name_of_col_is(sys_tables, sys_index, DICT_FLD__SYS_TABLES__N_COLS,
                       "N_COLS"));
  ut_ad(name_of_col_is(sys_tables, sys_index, DICT_FLD__SYS_TABLES__TYPE,
                       "TYPE"));
  ut_ad(name_of_col_is(sys_tables, sys_index, DICT_FLD__SYS_TABLES__MIX_LEN,
                       "MIX_LEN"));
  ut_ad(name_of_col_is(sys_tables, sys_index, DICT_FLD__SYS_TABLES__SPACE,
                       "SPACE"));

  tuple = dtuple_create(heap, 1);
  dfield = dtuple_get_nth_field(tuple, 0);

  /* We suffix "_backup57" to 5.7 statistics tables/.ibds. This is
  to avoid conflict with 8.0 statistics tables. Since InnoDB dictionary
  refers 5.7 stats tables without the sufix, we strip the suffix and
  search in dictionary. */
  bool is_stats = false;
  if (strcmp(name.m_name, "mysql/innodb_index_stats_backup57") == 0 ||
      strcmp(name.m_name, "mysql/innodb_table_stats_backup57") == 0) {
    is_stats = true;
  }

  std::string orig_name(name.m_name);

  if (is_stats) {
    /* To load 5.7 stats tables, we search the table names
    with "_backup57" suffix. We now strip the suffix before
    searching InnoDB Dictionary */
    std::string substr("_backup57");
    std::size_t found = orig_name.find(substr);
    ut_ad(found != std::string::npos);
    orig_name.erase(found, substr.length());

    dfield_set_data(dfield, orig_name.c_str(), orig_name.length());
  } else {
    dfield_set_data(dfield, name.m_name, ut_strlen(name.m_name));
  }

  dict_index_copy_types(tuple, sys_index, 1);

  pcur.open_on_user_rec(sys_index, tuple, PAGE_CUR_GE, BTR_SEARCH_LEAF, &mtr,
                        UT_LOCATION_HERE);
  rec = pcur.get_rec();

  if (!pcur.is_on_user_rec() || rec_get_deleted_flag(rec, 0)) {
    /* Not found */
  err_exit:
    pcur.close();
    mtr_commit(&mtr);
    mem_heap_free(heap);

    return nullptr;
  }

  field = rec_get_nth_field_old(nullptr, rec, DICT_FLD__SYS_TABLES__NAME, &len);

  /* Check if the table name in record is the searched one */
  if (!is_stats && (len != ut_strlen(name.m_name) ||
                    0 != ut_memcmp(name.m_name, field, len))) {
    goto err_exit;
  }

  err_msg = dict_load_table_low(name, rec, &table);

  if (err_msg) {
    ib::error(ER_IB_MSG_207) << err_msg;
    goto err_exit;
  }

  pcur.close();
  mtr_commit(&mtr);

  dict_load_tablespace(table, ignore_err);

  dict_load_columns(table, heap);

  dict_load_virtual(table, heap);

  dict_table_add_system_columns(table, heap);

  mem_heap_empty(heap);

  /* If there is no tablespace for the table then we only need to
  load the index definitions. So that we can IMPORT the tablespace
  later. When recovering table locks for resurrected incomplete
  transactions, the tablespace should exist, because DDL operations
  were not allowed while the table is being locked by a transaction. */
  dict_err_ignore_t index_load_err =
      !(ignore_err & DICT_ERR_IGNORE_RECOVER_LOCK) && table->ibd_file_missing
          ? DICT_ERR_IGNORE_ALL
          : ignore_err;
  err = dict_load_indexes(table, heap, index_load_err);

  if (err == DB_SUCCESS) {
    if (srv_is_upgrade_mode && !srv_upgrade_old_undo_found &&
        !dict_load_is_system_table(table->name.m_name)) {
      table->id = table->id + DICT_MAX_DD_TABLES;
    }
    if (cached) {
      dict_table_add_to_cache(table, true);
    }
  }

  if (dict_sys->dynamic_metadata != nullptr) {
    dict_table_load_dynamic_metadata(table);
  }

  /* Re-check like we do in dict_load_indexes() */
  if (!srv_load_corrupted && !(index_load_err & DICT_ERR_IGNORE_CORRUPT) &&
      table->is_corrupted()) {
    err = DB_INDEX_CORRUPT;
  }

  if (err == DB_INDEX_CORRUPT) {
    /* Refuse to load the table if the table has a corrupted
    clustered index */
    ut_ad(!srv_load_corrupted);

    ib::error(ER_IB_MSG_208) << "Load table " << table->name
                             << " failed, the table contains a"
                                " corrupted clustered index. Turn on"
                                " 'innodb_force_load_corrupted' to drop it";
    dict_table_remove_from_cache(table);
    table = nullptr;
    goto func_exit;
  }

  /* We don't trust the table->flags2(retrieved from SYS_TABLES.MIX_LEN
  field) if the datafiles are from 3.23.52 version. To identify this
  version, we do the below check and reset the flags. */
  if (!DICT_TF2_FLAG_IS_SET(table, DICT_TF2_FTS_HAS_DOC_ID) &&
      table->space == TRX_SYS_SPACE && table->flags == 0) {
    table->flags2 = 0;
  }

  DBUG_EXECUTE_IF(
      "ib_table_invalid_flags",
      if (strcmp(table->name.m_name, "test/t1") == 0) {
        table->flags2 = 255;
        table->flags = 255;
      });

  if (!dict_tf2_is_valid(table->flags, table->flags2)) {
    ib::error(ER_IB_MSG_209) << "Table " << table->name
                             << " in InnoDB"
                                " data dictionary contains invalid flags."
                                " SYS_TABLES.MIX_LEN="
                             << table->flags2;
    table->flags2 &= ~(DICT_TF2_TEMPORARY | DICT_TF2_INTRINSIC);
    dict_table_remove_from_cache(table);
    table = nullptr;
    err = DB_FAIL;
    goto func_exit;
  }

  /* Initialize table foreign_child value. Its value could be
  changed when dict_load_foreigns() is called below */
  table->fk_max_recusive_level = 0;

  /* If the force recovery flag is set, we open the table irrespective
  of the error condition, since the user may want to dump data from the
  clustered index. However we load the foreign key information only if
  all indexes were loaded. */
  if (!cached || table->ibd_file_missing) {
    /* Don't attempt to load the indexes from disk. */
  } else if (err == DB_SUCCESS) {
    err = dict_load_foreigns(table->name.m_name, nullptr, true, true,
                             ignore_err, fk_tables);

    if (err != DB_SUCCESS) {
      ib::warn(ER_IB_MSG_210) << "Load table " << table->name
                              << " failed, the table has missing"
                                 " foreign key indexes. Turn off"
                                 " 'foreign_key_checks' and try again.";

      dict_table_remove_from_cache(table);
      table = nullptr;
    } else {
      dict_mem_table_free_foreign_vcol_set(table);
      dict_mem_table_fill_foreign_vcol_set(table);
      table->fk_max_recusive_level = 0;
    }
  } else {
    dict_index_t *index;

    /* Make sure that at least the clustered index was loaded.
    Otherwise refuse to load the table */
    index = table->first_index();

    if (!srv_force_recovery || !index || !index->is_clustered()) {
      dict_table_remove_from_cache(table);
      table = nullptr;
    }
  }

func_exit:
  mem_heap_free(heap);

  ut_ad(!table || ignore_err != DICT_ERR_IGNORE_NONE ||
        table->ibd_file_missing || !table->is_corrupted());

  if (table && table->fts) {
    /* We do not add fts tables to optimize thread
    during upgrade because fts tables will be renamed
    as part of upgrade. These tables will be added
    to fts optimize queue when they are opened. */

    if (!(dict_table_has_fts_index(table) ||
          DICT_TF2_FLAG_IS_SET(table, DICT_TF2_FTS_HAS_DOC_ID) ||
          DICT_TF2_FLAG_IS_SET(table, DICT_TF2_FTS_ADD_DOC_ID))) {
      /* the table->fts could be created in dict_load_column
      when a user defined FTS_DOC_ID is present, but no
      FTS */
      fts_optimize_remove_table(table);
      fts_free(table);
    } else if (!srv_is_upgrade_mode) {
      fts_optimize_add_table(table);
    }
  }

  ut_ad(err != DB_SUCCESS || dict_foreign_set_validate(*table));

  return table;
}

/** This function is called when the database is booted. Loads system table
 index definitions except for the clustered index which is added to the
 dictionary cache at booting before calling this function. */
void dict_load_sys_table(dict_table_t *table) /*!< in: system table */
{
  mem_heap_t *heap;

  ut_ad(dict_sys_mutex_own());

  heap = mem_heap_create(100, UT_LOCATION_HERE);

  dict_load_indexes(table, heap, DICT_ERR_IGNORE_NONE);

  mem_heap_free(heap);
}

/** Loads foreign key constraint col names (also for the referenced table).
 Members that must be set (and valid) in foreign:
 foreign->heap
 foreign->n_fields
 foreign->id ('\0'-terminated)
 Members that will be created and set by this function:
 foreign->foreign_col_names[i]
 foreign->referenced_col_names[i]
 (for i=0..foreign->n_fields-1) */
static void dict_load_foreign_cols(
    dict_foreign_t *foreign) /*!< in/out: foreign constraint object */
{
  dict_table_t *sys_foreign_cols;
  dict_index_t *sys_index;
  btr_pcur_t pcur;
  dtuple_t *tuple;
  dfield_t *dfield;
  const rec_t *rec;
  const byte *field;
  ulint len;
  ulint i;
  mtr_t mtr;
  size_t id_len;

  ut_ad(dict_sys_mutex_own());

  id_len = strlen(foreign->id);

  foreign->foreign_col_names = static_cast<const char **>(
      mem_heap_alloc(foreign->heap, foreign->n_fields * sizeof(void *)));

  foreign->referenced_col_names = static_cast<const char **>(
      mem_heap_alloc(foreign->heap, foreign->n_fields * sizeof(void *)));

  mtr_start(&mtr);

  sys_foreign_cols = dict_table_get_low("SYS_FOREIGN_COLS");

  sys_index = UT_LIST_GET_FIRST(sys_foreign_cols->indexes);
  ut_ad(!dict_table_is_comp(sys_foreign_cols));

  tuple = dtuple_create(foreign->heap, 1);
  dfield = dtuple_get_nth_field(tuple, 0);

  dfield_set_data(dfield, foreign->id, id_len);
  dict_index_copy_types(tuple, sys_index, 1);

  pcur.open_on_user_rec(sys_index, tuple, PAGE_CUR_GE, BTR_SEARCH_LEAF, &mtr,
                        UT_LOCATION_HERE);
  for (i = 0; i < foreign->n_fields; i++) {
    rec = pcur.get_rec();

    ut_a(pcur.is_on_user_rec());
    ut_a(!rec_get_deleted_flag(rec, 0));

    field = rec_get_nth_field_old(nullptr, rec, DICT_FLD__SYS_FOREIGN_COLS__ID,
                                  &len);

    if (len != id_len || ut_memcmp(foreign->id, field, len) != 0) {
      const rec_t *pos;
      ulint pos_len;
      const rec_t *for_col_name;
      ulint for_col_name_len;
      const rec_t *ref_col_name;
      ulint ref_col_name_len;

      pos = rec_get_nth_field_old(nullptr, rec, DICT_FLD__SYS_FOREIGN_COLS__POS,
                                  &pos_len);

      for_col_name = rec_get_nth_field_old(
          nullptr, rec, DICT_FLD__SYS_FOREIGN_COLS__FOR_COL_NAME,
          &for_col_name_len);

      ref_col_name = rec_get_nth_field_old(
          nullptr, rec, DICT_FLD__SYS_FOREIGN_COLS__REF_COL_NAME,
          &ref_col_name_len);

      ib::fatal sout(UT_LOCATION_HERE);

      sout << "Unable to load column names for foreign"
              " key '"
           << foreign->id
           << "' because it was not found in"
              " InnoDB internal table SYS_FOREIGN_COLS. The"
              " closest entry we found is:"
              " (ID='";
      sout.write(field, len);
      sout << "', POS=" << mach_read_from_4(pos) << ", FOR_COL_NAME='";
      sout.write(for_col_name, for_col_name_len);
      sout << "', REF_COL_NAME='";
      sout.write(ref_col_name, ref_col_name_len);
      sout << "')";
    }

    field = rec_get_nth_field_old(nullptr, rec, DICT_FLD__SYS_FOREIGN_COLS__POS,
                                  &len);
    ut_a(len == 4);
    ut_a(i == mach_read_from_4(field));

    field = rec_get_nth_field_old(
        nullptr, rec, DICT_FLD__SYS_FOREIGN_COLS__FOR_COL_NAME, &len);
    foreign->foreign_col_names[i] =
        mem_heap_strdupl(foreign->heap, (char *)field, len);

    field = rec_get_nth_field_old(
        nullptr, rec, DICT_FLD__SYS_FOREIGN_COLS__REF_COL_NAME, &len);
    foreign->referenced_col_names[i] =
        mem_heap_strdupl(foreign->heap, (char *)field, len);

    pcur.move_to_next_user_rec(&mtr);
  }

  pcur.close();
  mtr_commit(&mtr);
}

/** Loads a foreign key constraint to the dictionary cache. If the referenced
 table is not yet loaded, it is added in the output parameter (fk_tables).
 @return DB_SUCCESS or error code */
[[nodiscard]] static dberr_t dict_load_foreign(
    const char *id,
    /*!< in: foreign constraint id, must be
    '\0'-terminated */
    const char **col_names,
    /*!< in: column names, or NULL
    to use foreign->foreign_table->col_names */
    bool check_recursive,
    /*!< in: whether to record the foreign table
    parent count to avoid unlimited recursive
    load of chained foreign tables */
    bool check_charsets,
    /*!< in: whether to check charset
    compatibility */
    dict_err_ignore_t ignore_err,
    /*!< in: error to be ignored */
    dict_names_t &fk_tables)
/*!< out: the foreign key constraint is added
to the dictionary cache only if the referenced
table is already in cache.  Otherwise, the
foreign key constraint is not added to cache,
and the referenced table is added to this
stack. */
{
  dict_foreign_t *foreign;
  dict_table_t *sys_foreign;
  btr_pcur_t pcur;
  dict_index_t *sys_index;
  dtuple_t *tuple;
  mem_heap_t *heap2;
  dfield_t *dfield;
  const rec_t *rec;
  const byte *field;
  ulint len;
  ulint n_fields_and_type;
  mtr_t mtr;
  dict_table_t *for_table;
  dict_table_t *ref_table;
  size_t id_len;

  DBUG_TRACE;
  DBUG_PRINT("dict_load_foreign",
             ("id: '%s', check_recursive: %d", id, check_recursive));

  ut_ad(dict_sys_mutex_own());

  id_len = strlen(id);

  heap2 = mem_heap_create(100, UT_LOCATION_HERE);

  mtr_start(&mtr);

  sys_foreign = dict_table_get_low("SYS_FOREIGN");

  sys_index = UT_LIST_GET_FIRST(sys_foreign->indexes);
  ut_ad(!dict_table_is_comp(sys_foreign));

  tuple = dtuple_create(heap2, 1);
  dfield = dtuple_get_nth_field(tuple, 0);

  dfield_set_data(dfield, id, id_len);
  dict_index_copy_types(tuple, sys_index, 1);

  pcur.open_on_user_rec(sys_index, tuple, PAGE_CUR_GE, BTR_SEARCH_LEAF, &mtr,
                        UT_LOCATION_HERE);
  rec = pcur.get_rec();

  if (!pcur.is_on_user_rec() || rec_get_deleted_flag(rec, 0)) {
    /* Not found */

    ib::error(ER_IB_MSG_211) << "Cannot load foreign constraint " << id
                             << ": could not find the relevant record in "
                             << "SYS_FOREIGN";

    pcur.close();
    mtr_commit(&mtr);
    mem_heap_free(heap2);

    return DB_ERROR;
  }

  field = rec_get_nth_field_old(nullptr, rec, DICT_FLD__SYS_FOREIGN__ID, &len);

  /* Check if the id in record is the searched one */
  if (len != id_len || ut_memcmp(id, field, len) != 0) {
    {
      ib::error err(ER_IB_MSG_1227);
      err << "Cannot load foreign constraint " << id << ": found ";
      err.write(field, len);
      err << " instead in SYS_FOREIGN";
    }

    pcur.close();
    mtr_commit(&mtr);
    mem_heap_free(heap2);

    return DB_ERROR;
  }

  /* Read the table names and the number of columns associated
  with the constraint */

  mem_heap_free(heap2);

  foreign = dict_mem_foreign_create();

  n_fields_and_type = mach_read_from_4(
      rec_get_nth_field_old(nullptr, rec, DICT_FLD__SYS_FOREIGN__N_COLS, &len));

  ut_a(len == 4);

  /* We store the type in the bits 24..29 of n_fields_and_type. */

  foreign->type = (unsigned int)(n_fields_and_type >> 24);
  foreign->n_fields = (unsigned int)(n_fields_and_type & 0x3FFUL);

  foreign->id = mem_heap_strdupl(foreign->heap, id, id_len);

  field = rec_get_nth_field_old(nullptr, rec, DICT_FLD__SYS_FOREIGN__FOR_NAME,
                                &len);

  foreign->foreign_table_name =
      mem_heap_strdupl(foreign->heap, (char *)field, len);
  dict_mem_foreign_table_name_lookup_set(foreign, true);

  const ulint foreign_table_name_len = len;

  field = rec_get_nth_field_old(nullptr, rec, DICT_FLD__SYS_FOREIGN__REF_NAME,
                                &len);
  foreign->referenced_table_name =
      mem_heap_strdupl(foreign->heap, (char *)field, len);
  dict_mem_referenced_table_name_lookup_set(foreign, true);

  pcur.close();
  mtr_commit(&mtr);

  dict_load_foreign_cols(foreign);

  ref_table =
      dict_table_check_if_in_cache_low(foreign->referenced_table_name_lookup);
  for_table =
      dict_table_check_if_in_cache_low(foreign->foreign_table_name_lookup);

  if (!for_table) {
    /* To avoid recursively loading the tables related through
    the foreign key constraints, the child table name is saved
    here.  The child table will be loaded later, along with its
    foreign key constraint. */

    lint old_size = mem_heap_get_size(ref_table->heap);

    ut_a(ref_table != nullptr);
    fk_tables.push_back(mem_heap_strdupl(ref_table->heap,
                                         foreign->foreign_table_name_lookup,
                                         foreign_table_name_len));

    lint new_size = mem_heap_get_size(ref_table->heap);
    dict_sys->size += new_size - old_size;

    dict_foreign_remove_from_cache(foreign);
    return DB_SUCCESS;
  }

  ut_a(for_table || ref_table);

  /* Note that there may already be a foreign constraint object in
  the dictionary cache for this constraint: then the following
  call only sets the pointers in it to point to the appropriate table
  and index objects and frees the newly created object foreign.
  Adding to the cache should always succeed since we are not creating
  a new foreign key constraint but loading one from the data
  dictionary. */

  return dict_foreign_add_to_cache(foreign, col_names, check_charsets, true,
                                   ignore_err);
}

/** Loads foreign key constraints where the table is either the foreign key
 holder or where the table is referenced by a foreign key. Adds these
 constraints to the data dictionary.

 The foreign key constraint is loaded only if the referenced table is also
 in the dictionary cache.  If the referenced table is not in dictionary
 cache, then it is added to the output parameter (fk_tables).

 @return DB_SUCCESS or error code */
dberr_t dict_load_foreigns(
    const char *table_name,       /*!< in: table name */
    const char **col_names,       /*!< in: column names, or NULL
                                  to use table->col_names */
    bool check_recursive,         /*!< in: Whether to check
                                  recursive load of tables
                                  chained by FK */
    bool check_charsets,          /*!< in: whether to check
                                  charset compatibility */
    dict_err_ignore_t ignore_err, /*!< in: error to be ignored */
    dict_names_t &fk_tables)
/*!< out: stack of table
names which must be loaded
subsequently to load all the
foreign key constraints. */
{
  ulint tuple_buf[ut::div_ceil(DTUPLE_EST_ALLOC(1), sizeof(ulint))];
  btr_pcur_t pcur;
  dtuple_t *tuple;
  dfield_t *dfield;
  dict_index_t *sec_index;
  dict_table_t *sys_foreign;
  const rec_t *rec;
  const byte *field;
  ulint len;
  dberr_t err;
  mtr_t mtr;

  DBUG_TRACE;

  ut_ad(dict_sys_mutex_own());

  sys_foreign = dict_table_get_low("SYS_FOREIGN");

  if (sys_foreign == nullptr) {
    /* No foreign keys defined yet in this database */

    ib::info(ER_IB_MSG_212) << "No foreign key system tables in the database";
    return DB_ERROR;
  }

  ut_ad(!dict_table_is_comp(sys_foreign));
  mtr_start(&mtr);

  /* Get the secondary index based on FOR_NAME from table
  SYS_FOREIGN */

  sec_index = sys_foreign->first_index()->next();
  ut_ad(!sec_index->is_clustered());
start_load:

  tuple = dtuple_create_from_mem(tuple_buf, sizeof(tuple_buf), 1, 0);
  dfield = dtuple_get_nth_field(tuple, 0);

  dfield_set_data(dfield, table_name, ut_strlen(table_name));
  dict_index_copy_types(tuple, sec_index, 1);

  pcur.open_on_user_rec(sec_index, tuple, PAGE_CUR_GE, BTR_SEARCH_LEAF, &mtr,
                        UT_LOCATION_HERE);
loop:
  rec = pcur.get_rec();

  if (!pcur.is_on_user_rec()) {
    /* End of index */

    goto load_next_index;
  }

  /* Now we have the record in the secondary index containing a table
  name and a foreign constraint ID */

  field = rec_get_nth_field_old(nullptr, rec,
                                DICT_FLD__SYS_FOREIGN_FOR_NAME__NAME, &len);

  /* Check if the table name in the record is the one searched for; the
  following call does the comparison in the latin1_swedish_ci
  charset-collation, in a case-insensitive way. */

  if (0 != cmp_data_data(dfield_get_type(dfield)->mtype,
                         dfield_get_type(dfield)->prtype, true,
                         static_cast<const byte *>(dfield_get_data(dfield)),
                         dfield_get_len(dfield), field, len, nullptr)) {
    goto load_next_index;
  }

  /* Since table names in SYS_FOREIGN are stored in a case-insensitive
  order, we have to check that the table name matches also in a binary
  string comparison. On Unix, MySQL allows table names that only differ
  in character case.  If lower_case_table_names=2 then what is stored
  may not be the same case, but the previous comparison showed that they
  match with no-case.  */

  if (rec_get_deleted_flag(rec, 0)) {
    goto next_rec;
  }

  if ((innobase_get_lower_case_table_names() != 2) &&
      (0 != ut_memcmp(field, table_name, len))) {
    goto next_rec;
  }

  /* Now we get a foreign key constraint id */
  field = rec_get_nth_field_old(nullptr, rec,
                                DICT_FLD__SYS_FOREIGN_FOR_NAME__ID, &len);

  /* Copy the string because the page may be modified or evicted
  after mtr_commit() below. */
  char fk_id[MAX_TABLE_NAME_LEN + 1];

  ut_a(len <= MAX_TABLE_NAME_LEN);
  memcpy(fk_id, field, len);
  fk_id[len] = '\0';

  pcur.store_position(&mtr);

  mtr_commit(&mtr);

  /* Load the foreign constraint definition to the dictionary cache */

  err = dict_load_foreign(fk_id, col_names, check_recursive, check_charsets,
                          ignore_err, fk_tables);

  if (err != DB_SUCCESS) {
    pcur.close();

    return err;
  }

  mtr_start(&mtr);

  pcur.restore_position(BTR_SEARCH_LEAF, &mtr, UT_LOCATION_HERE);
next_rec:
  pcur.move_to_next_user_rec(&mtr);
  goto loop;

load_next_index:
  pcur.close();
  mtr_commit(&mtr);

  sec_index = sec_index->next();

  if (sec_index != nullptr) {
    mtr_start(&mtr);

    /* Switch to scan index on REF_NAME, fk_max_recusive_level
    already been updated when scanning FOR_NAME index, no need to
    update again */
    check_recursive = false;

    goto start_load;
  }

  return DB_SUCCESS;
}
=======
}
>>>>>>> 845d525d49c8027a4d0cdcc43372c96ba295c857
