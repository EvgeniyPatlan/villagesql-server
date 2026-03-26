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

#include "villagesql/services/background_thread.h"

#include "mysql/psi/mysql_thread.h"
#include "sql/mysqld.h"
#include "sql/mysqld_thd_manager.h"
#include "sql/protocol_classic.h"
#include "sql/sql_class.h"
#include "sql/sql_thd_internal_api.h"
#include "villagesql/include/error.h"

#ifdef HAVE_PSI_INTERFACE
#include "mysql/psi/psi_thread.h"
#endif

namespace villagesql {
namespace services {

// PSI thread key shared by all VEF extension background threads.
// Registered at server startup via init_vef_background_thread_psi_key().
PSI_thread_key key_thread_vef_extension_worker;

#ifdef HAVE_PSI_INTERFACE
static PSI_thread_info vef_extension_worker_thread_info[] = {
    {&key_thread_vef_extension_worker, "vef_extension_worker", "vef_wkr", 0, 0,
     PSI_DOCUMENT_ME}};
#endif

void init_vef_background_thread_psi_key() {
#ifdef HAVE_PSI_INTERFACE
  const char *category = "villagesql";
  int count =
      static_cast<int>(array_elements(vef_extension_worker_thread_info));
  mysql_thread_register(category, vef_extension_worker_thread_info, count);
#endif
}

vef_thread_handle_t *register_vef_background_thread(const char *thread_name) {
  if (my_thread_init()) {
    LogVSQL(ERROR_LEVEL,
            "register_vef_background_thread: my_thread_init() failed");
    return nullptr;
  }

  THD *thd = new (std::nothrow) THD;
  if (thd == nullptr) {
    LogVSQL(ERROR_LEVEL,
            "register_vef_background_thread: failed to allocate THD");
    return nullptr;
  }

  thd->system_thread = SYSTEM_THREAD_BACKGROUND;
  thd->security_context()->skip_grants();
  thd->security_context()->set_host_or_ip_ptr(my_localhost,
                                              strlen(my_localhost));
  thd->security_context()->set_user_ptr(STRING_WITH_LEN("vef_worker"));
  thd->get_protocol_classic()->init_net(nullptr);
  thd->set_new_thread_id();
  thd->set_command(COM_DAEMON);
  thd->set_proc_info(thread_name);
  thd->set_time();
  thd->variables.lock_wait_timeout = LONG_TIMEOUT;

  // thread_stack must be set before calling store_globals().
  // We use a local variable address as an approximation of the thread stack
  // top; this is the same approach used by event_scheduler_thread().
  thd->thread_stack = reinterpret_cast<char *>(&thd);

  mysql_thread_set_psi_id(thd->thread_id());

#ifdef HAVE_PSI_THREAD_INTERFACE
  PSI_thread *psi = PSI_THREAD_CALL(get_thread)();
  thd_set_psi(thd, psi);
  PSI_THREAD_CALL(set_thread_account)
  (thd->security_context()->user().str, thd->security_context()->user().length,
   thd->security_context()->host_or_ip().str,
   thd->security_context()->host_or_ip().length);
  PSI_THREAD_CALL(set_thread_command)(thd->get_command());
  PSI_THREAD_CALL(set_thread_start_time)(thd->query_start_in_secs());
#endif

  thd->store_globals();

  Global_THD_manager *thd_manager = Global_THD_manager::get_instance();
  thd_manager->add_thd(thd);
  thd_manager->inc_thread_running();

  auto *handle = new (std::nothrow) vef_thread_handle_t{thd};
  if (handle == nullptr) {
    thd_manager->remove_thd(thd);
    thd_manager->dec_thread_running();
    thd->release_resources();
    delete thd;
    LogVSQL(ERROR_LEVEL,
            "register_vef_background_thread: failed to allocate handle");
    return nullptr;
  }

  LogVSQL(INFORMATION_LEVEL, "VEF background thread registered: '%s' (id=%lu)",
          thread_name, static_cast<unsigned long>(thd->thread_id()));
  return handle;
}

void unregister_vef_background_thread(vef_thread_handle_t *handle) {
  if (handle == nullptr) return;

  THD *thd = handle->thd;
  delete handle;

  Global_THD_manager *thd_manager = Global_THD_manager::get_instance();
  thd->set_proc_info("Clearing");
  thd->get_protocol_classic()->end_net();
  thd->release_resources();
  thd_manager->remove_thd(thd);
  thd_manager->dec_thread_running();
  delete thd;

  my_thread_end();
}

}  // namespace services
}  // namespace villagesql
