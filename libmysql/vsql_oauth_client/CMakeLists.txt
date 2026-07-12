# Copyright (c) 2026 VillageSQL Contributors
#
# This program is free software; you can redistribute it and/or modify
# it under the terms of the GNU General Public License, version 2.0,
# as published by the Free Software Foundation.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License, version 2.0, for more details.
#
# You should have received a copy of the GNU General Public License
# along with this program; if not, write to the Free Software
# Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301  USA

#
# VillageSQL OAuth client-side authentication plugin (vsql_oauth_client).
# Pairs with the vsql_oauth2 server extension; obtains an OAuth JWT from
# $VSQL_OAUTH_TOKEN_FILE or $VSQL_OAUTH_TOKEN_HELPER and sends it during the handshake.
#

# Gated like the other client authentication plugins (authentication_ldap, oci,
# etc.): part of the community client build, skipped if disabled.
IF(NOT WITH_AUTHENTICATION_CLIENT_PLUGINS)
  MESSAGE(STATUS "Skipping the VillageSQL OAuth authentication client plugin.")
  RETURN()
ENDIF()

# Self-contained: only mysys + the client plugin API. No OpenSSL/curl/JWT libs
# (all OAuth crypto/flow lives in the external token helper, not this plugin).
MYSQL_ADD_PLUGIN(
  vsql_oauth_client

  vsql_oauth_client.c

  LINK_LIBRARIES mysys

  CLIENT_ONLY
  MODULE_ONLY MODULE_OUTPUT_NAME "vsql_oauth_client"
)

IF(LINUX OR SOLARIS)
  SET(PLUGIN_VERSION_FILE
    ${CMAKE_CURRENT_SOURCE_DIR}/vsql_oauth_client.ver)
  IF(SOLARIS)
    TARGET_LINK_OPTIONS(vsql_oauth_client PRIVATE
      LINKER:-z,gnu-version-script-compat)
  ENDIF()
  # Hide all symbols; the one visible symbol is _mysql_client_plugin_declaration_.
  TARGET_LINK_OPTIONS(vsql_oauth_client PRIVATE
    LINKER:--version-script=${PLUGIN_VERSION_FILE}
    )
  SET_TARGET_PROPERTIES(vsql_oauth_client
    PROPERTIES LINK_DEPENDS ${PLUGIN_VERSION_FILE})
ENDIF()
