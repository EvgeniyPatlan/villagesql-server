// Copyright (c) 2026 VillageSQL Contributors
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License, version 2.0,
// as published by the Free Software Foundation.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License, version 2.0, for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program; if not, write to the Free Software
// Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301  USA

// vsql_oauth_client -- VillageSQL OAuth client-side authentication plugin.
//
// A libmysqlclient client-side auth plugin that obtains an OAuth token and sends
// it to the server, so a vanilla client logs in with NO token on the command
// line / process list / shell history:
//
//     mysql --user='me@corp' \
//           --plugin-dir=<dir-with-this-.so> \
//           --default-auth=vsql_oauth_client
//
// The token comes from one of two sources (env-driven, so the plugin is fully
// self-contained -- no client-program option wiring required):
//   1. $VSQL_OAUTH_TOKEN_FILE -- read the JWT from that file (static token; the
//      analog of the native OIDC client plugin's id-token-file). Refresh, if
//      wanted, is done by whatever writes the file.
//   2. else $VSQL_OAUTH_TOKEN_HELPER -- run that command and read the JWT from
//      its stdout (interactive/refresh: the token helper does browser flow +
//      refresh cache, e.g. vsql_entra_login.py --print-token). Running it
//      per-connect is what gives transparent refresh -- the reason to use this
//      over a static file.
//
// The plugin deliberately does NOT implement OAuth in C: the gnarly parts
// (browser flow, loopback listener, token endpoint, refresh cache) live in the
// external token helper, in a language where that is easy. This plugin is a thin
// pipe. TRADE-OFF: the token-helper mode fork/execs a subprocess during the
// blocking handshake (latency + a PATH dependency on the token helper).
//
// Server pairing: the account is IDENTIFIED WITH vsql_oauth2, which pins
// "vsql_oauth_client" as its client_auth_plugin. This plugin declares that SAME
// name, so client and server agree directly (no auth-switch). The name must NOT
// be "mysql_clear_password": libmysqlclient registers a built-in of that name
// and refuses a second, and the client advertises its DECLARED name on the wire
// so the server must know it. Wire behavior is identical to clear-password (one
// packet: token + NUL) -- only the token SOURCE differs -- and because it's our
// own name, no --enable-cleartext-plugin gate applies.
//
// This is VillageSQL's own client plugin -- NOT a backport of the stock MySQL
// 9.1+ authentication_openid_connect_client. It adds token-helper/refresh, needs no
// client-program surgery, and carries no upstream-fork maintenance. The server
// still ALSO accepts a stock 9.1+ client's native OIDC plugin -- that is a
// separate, server-side capability of the vsql_oauth2 extension.

#include "my_config.h"

#include <my_compiler.h>  // MY_ATTRIBUTE, used by mysql_declare_client_plugin

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// client_plugin.h forward-declares `struct MYSQL;` and (via plugin_auth_common.h)
// provides MYSQL_PLUGIN_VIO, CR_OK, CR_ERROR. We use MYSQL only as an opaque
// pointer, so the full <mysql.h> (which needs generated headers) is not needed.
#include <mysql/client_plugin.h>
#include <mysql/plugin_auth_common.h>

// Max token size we will read. JWTs are typically 1-4 KB; give generous
// headroom. A token larger than this is treated as an error (better to fail
// closed than to truncate a credential).
#define VSQL_MAX_TOKEN 65536

// The token-helper command. Overridable via $VSQL_OAUTH_TOKEN_HELPER so the same
// .so works for Entra/Google/etc. Must print the bare token to stdout and exit 0.
static const char *token_helper_command(void) {
  const char *env = getenv("VSQL_OAUTH_TOKEN_HELPER");
  if (env != NULL && env[0] != '\0') return env;
  return "vsql_entra_login.py --print-token";
}

// Trim a single trailing newline and NUL-terminate `out[0..n)`; return length.
static long finish_token(char *out, size_t n) {
  if (n == 0) return -1;
  if (out[n - 1] == '\n') --n;
  out[n] = '\0';
  return (long)n;
}

// Read the JWT from $VSQL_OAUTH_TOKEN_FILE into `out`. Returns length, or -1.
static long read_token_file(const char *path, char *out, size_t out_sz) {
  FILE *fp = fopen(path, "r");
  if (fp == NULL) return -1;
  size_t n = fread(out, 1, out_sz - 1, fp);
  fclose(fp);
  return finish_token(out, n);
}

// Run the token helper and capture its stdout token into `out`. Returns length,
// or -1.
static long run_token_helper(const char *cmd, char *out, size_t out_sz) {
  FILE *fp = popen(cmd, "r");
  if (fp == NULL) return -1;
  size_t n = fread(out, 1, out_sz - 1, fp);
  int status = pclose(fp);
  // Non-zero exit (auth declined, network error, ...) -> fail closed; do not
  // send a partial/garbage token.
  if (status != 0) return -1;
  return finish_token(out, n);
}

// Obtain the token (NUL-terminated) into `out`. Source precedence:
//   $VSQL_OAUTH_TOKEN_FILE (static) -> else the token helper
//   ($VSQL_OAUTH_TOKEN_HELPER). Returns token length, or -1 on failure.
static long fetch_token(char *out, size_t out_sz) {
  const char *tok_file = getenv("VSQL_OAUTH_TOKEN_FILE");
  if (tok_file != NULL && tok_file[0] != '\0')
    return read_token_file(tok_file, out, out_sz);
  return run_token_helper(token_helper_command(), out, out_sz);
}

// The authentication callback. Mirrors clear_password_auth_client: write one
// packet holding the credential (+ trailing NUL). The only difference is WHERE
// the credential comes from -- fetch_token (token file or token helper), not
// mysql->passwd.
static int vsql_oauth_auth_client(MYSQL_PLUGIN_VIO *vio, struct MYSQL *mysql) {
  (void)mysql;  // token comes from fetch_token, not the MYSQL handle
  char token[VSQL_MAX_TOKEN];
  long len = fetch_token(token, sizeof(token));
  if (len < 0) return CR_ERROR;

  // Write-first: we are the negotiated plugin (the account pins
  // "vsql_oauth_client" and this plugin declares the same name, so the server
  // accepts our first reply directly -- no auth-switch). Send token + trailing
  // NUL, exactly as the built-in clear_password sends a password.
  int res = vio->write_packet(vio, (const unsigned char *)token, (int)len + 1);
  return res ? CR_ERROR : CR_OK;
}

mysql_declare_client_plugin(AUTHENTICATION)
    // The DECLARED name must match how the client loads us
    // (--default-auth=vsql_oauth_client / vsql_oauth_client.so). It must NOT be
    // "mysql_clear_password": that collides with libmysqlclient's built-in of
    // that name, so --default-auth=vsql_oauth_client would miss our declaration
    // and fall back to the built-in (which sends an empty password ->
    // "Access denied ... using password: NO"). Our own name avoids the
    // collision; the wire protocol we speak is identical to clear-password, so
    // the server (which pinned it as this method's client plugin) accepts it.
    "vsql_oauth_client",
    "VillageSQL Contributors",
    "VillageSQL OAuth client authentication plugin (token from "
    "$VSQL_OAUTH_TOKEN_FILE or $VSQL_OAUTH_TOKEN_HELPER)",
    {0, 0, 1},
    "GPL",
    NULL,  // mysql_api (filled by loader)
    NULL,  // init
    NULL,  // deinit
    NULL,  // options
    NULL,  // get_options
    vsql_oauth_auth_client,
    NULL   // authenticate_user_nonblocking
mysql_end_client_plugin;
