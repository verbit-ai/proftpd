/*
 * ProFTPD - FTP server daemon
 * Copyright (c) 1997, 1998 Public Flood Software
 * Copyright (c) 1999, 2000 MacGyver aka Habeeb J. Dihu <macgyver@tos.net>
 * Copyright (c) 2001, 2002 The ProFTPD Project team
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307, USA.
 *
 * As a special exemption, Public Flood Software/MacGyver aka Habeeb J. Dihu
 * and other respective copyright holders give permission to link this program
 * with OpenSSL, and distribute the resulting executable, without including
 * the source code for OpenSSL in the source distribution.
 */

/*
 * Authentication module for ProFTPD
 * $Id: mod_auth.c,v 1.116 2002-12-07 21:43:44 jwm Exp $
 */

#include "conf.h"
#include "privs.h"

/* From the core module */
extern int core_display_file(const char *,const char *,const char *);
extern pid_t mpid;

module auth_module;

static int TimeoutLogin = PR_TUNABLE_TIMEOUTLOGIN;
static int logged_in = 0;
static int auth_tries = 0;
static char *auth_pass_resp_code = R_230;
static unsigned int TimeoutSession = 0;

static void auth_scan_scoreboard(void);

/* Perform a chroot or equivalent action to lockdown the process into a
 * particular directory.
 */
static int lockdown(char *newroot) {
  log_debug(DEBUG1, "Preparing to chroot() the environment, path = '%s'",
    newroot);

  PRIVS_ROOT
  if (pr_fsio_chroot(newroot) == -1) {
    PRIVS_RELINQUISH
    log_pri(PR_LOG_ERR, "%s chroot(\"%s\"): %s", session.user, newroot,
      strerror(errno));
    return -1;
  }
  PRIVS_RELINQUISH

  log_debug(DEBUG1, "Environment successfully chroot()ed.");
  return 0;
}

/* auth_cmd_chk_cb() is hooked into the main server's auth_hook function,
 * so that we can deny all commands until authentication is complete.
 */
static int auth_cmd_chk_cb(cmd_rec *cmd) {
  unsigned char *authenticated = get_param_ptr(cmd->server->conf,
    "authenticated", FALSE);

  if (!authenticated || *authenticated == FALSE) {
    send_response(R_530, "Please login with " C_USER " and " C_PASS);
    return FALSE;
  }

  return TRUE;
}

/* As for 1.2.0, timer callbacks are now non-reentrant, so it's
 * safe to call session_exit().
 */

static int auth_login_timeout_cb(CALLBACK_FRAME) {
  /* Is this the proper behavior when timing out? */
  send_response_async(R_421, "Login Timeout (%d seconds): "
    "closing control connection.", TimeoutLogin);

  session_exit(PR_LOG_NOTICE, "FTP login timed out, disconnected", 0, NULL);

  /* Do not restart the timer (should never be reached). */
  return 0;
}

static int auth_session_timeout_cb(CALLBACK_FRAME) {

  send_response_async(R_421, "Session Timeout (%u seconds): "
    "closing control connection", TimeoutSession);

  session_exit(PR_LOG_NOTICE, "FTP session timed out, disconnected", 0, NULL);

  /* no need to restart the timer -- session's over */
  return 0;
}

static int auth_sess_init(void) {
  config_rec *c = NULL;

  /* Check for a server-specific TimeoutLogin */
  if ((c = find_config(main_server->conf, CONF_PARAM, "TimeoutLogin",
      FALSE)) != NULL)
    TimeoutLogin = *((int *) c->argv[0]);

  /* Start the login timer */
  if (TimeoutLogin) {
    remove_timer(TIMER_LOGIN, &auth_module);
    add_timer(TimeoutLogin, TIMER_LOGIN, &auth_module, auth_login_timeout_cb);
  }

  PRIVS_ROOT
  if (pr_open_scoreboard(O_RDWR, NULL) < 0)
    log_debug(DEBUG0, "error opening scoreboard: %s", strerror(errno));
  PRIVS_RELINQUISH

  /* Create an entry in the scoreboard for this session. */
  if (pr_scoreboard_add_entry() < 0)
    log_pri(PR_LOG_NOTICE, "notice: unable to add scoreboard entry: %s",
      strerror(errno));

  pr_scoreboard_update_entry(getpid(),
    PR_SCORE_USER, "(none)",
    PR_SCORE_SERVER_IP, main_server->ipaddr,
    PR_SCORE_SERVER_PORT, main_server->ServerPort,
    PR_SCORE_SERVER_ADDR, main_server->ipaddr, main_server->ServerPort,
    PR_SCORE_SERVER_NAME, main_server->ServerName,
    PR_SCORE_CLIENT_ADDR, session.c->remote_name, session.c->remote_ipaddr,
    PR_SCORE_CLASS, (session.class && session.class->name) ?
      session.class->name: "",
    PR_SCORE_BEGIN_SESSION, time(NULL),
    NULL);

  /* If DisplayConnect is configured, we'll need to scan the scoreboard
   * now, in order to tally up certain values for substituting in any
   * of the DisplayConnect's file variables.
   */
  if (get_param_ptr(main_server->conf, "DisplayConnect", FALSE) != NULL)
    auth_scan_scoreboard();

  return 0;
}

static int auth_init(void) {

  /* By default, enable auth checking */
  set_auth_check(auth_cmd_chk_cb);

  return 0;
}

static int _do_auth(pool *p, xaset_t *conf, char *u, char *pw) {
  char *cpw = NULL;
  config_rec *c;

  if (conf) {
    c = find_config(conf,CONF_PARAM,"UserPassword",FALSE);

    while(c) {
      if (!strcmp(c->argv[0],u)) {
        cpw = (char*)c->argv[1];
        break;
      }

      c = find_config_next(c,c->next,CONF_PARAM,"UserPassword",FALSE);
    }
  }

  if (cpw) {
    if (!auth_getpwnam(p, u))
      return PR_AUTH_NOPWD;

    return auth_check(p, cpw, u, pw);
  }

  return auth_authenticate(p,u,pw);
}

MODRET auth_post_pass(cmd_rec *cmd) {
  config_rec *c = NULL;
  unsigned int ctxt_precedence = 0;
  unsigned char have_user_timeout, have_group_timeout, have_class_timeout,
    have_all_timeout;

  have_user_timeout = have_group_timeout = have_class_timeout =
    have_all_timeout = FALSE;

  if ((c = find_config(TOPLEVEL_CONF, CONF_PARAM, "TimeoutSession",
      FALSE)) != NULL) {

    if (c->argc == 3) {
      if (!strcmp(c->argv[1], "user")) {
        if (user_expression((char **) &c->argv[2])) {

          if (*((unsigned int *) c->argv[1]) > ctxt_precedence) {

            /* Set the context precedence. */
            ctxt_precedence = *((unsigned int *) c->argv[1]);

            TimeoutSession = *((unsigned int *) c->argv[0]);

            have_group_timeout = have_class_timeout = have_all_timeout = FALSE;
            have_user_timeout = TRUE;
          }
        }

      } else if (!strcmp(c->argv[1], "group")) {
        if (group_expression((char **) &c->argv[2])) {

          if (*((unsigned int *) c->argv[1]) > ctxt_precedence) {

            /* Set the context precedence. */
            ctxt_precedence = *((unsigned int *) c->argv[1]);

            TimeoutSession = *((unsigned int *) c->argv[0]);

            have_user_timeout = have_class_timeout = have_all_timeout = FALSE;
            have_group_timeout = TRUE;
          }
        }

      } else if (!strcmp(c->argv[1], "class")) {
        if (session.class && session.class->name &&
            !strcmp(session.class->name, c->argv[2])) {

          if (*((unsigned int *) c->argv[1]) > ctxt_precedence) {

            /* Set the context precedence. */
            ctxt_precedence = *((unsigned int *) c->argv[1]);

            TimeoutSession = *((unsigned int *) c->argv[0]);

            have_user_timeout = have_group_timeout = have_all_timeout = FALSE;
            have_class_timeout = TRUE;
          }
        }
      }

    } else {

      if (*((unsigned int *) c->argv[1]) > ctxt_precedence) {

        /* Set the context precedence. */
        ctxt_precedence = *((unsigned int *) c->argv[1]);

        TimeoutSession = *((unsigned int *) c->argv[0]);

        have_user_timeout = have_group_timeout = have_class_timeout = FALSE;
        have_all_timeout = TRUE;
      }
    }

    c = find_config_next(c, c->next, CONF_PARAM, "TimeoutSession", FALSE);
  }

  /* If configured, start a session timer.  The timer ID value for
   * session timers will not be #defined, as I think that is a bad approach.
   * A better mechanism would be to use the random timer ID generation, and
   * store the returned ID in order to later remove the timer.
   */

  if (have_user_timeout || have_group_timeout ||
      have_class_timeout || have_all_timeout) {
    log_debug(DEBUG4, "setting TimeoutSession of %u seconds for current %s",
      TimeoutSession,
      have_user_timeout ? "user" : have_group_timeout ? "group" :
      have_class_timeout ? "class" : "all");
    add_timer(TimeoutSession, TIMER_SESSION, &auth_module,
      auth_session_timeout_cb);
  }

  return HANDLED(cmd);
}

/* Handle group based authentication, only checked if pw
 * based fails
 */

static config_rec *_auth_group(pool *p, char *user, char **group,
                               char **ournamep, char **anonnamep, char *pass)
{
  config_rec *c;
  char *ourname = NULL,*anonname = NULL;
  char **grmem;
  struct group *grp;

  ourname = (char*)get_param_ptr(main_server->conf,"UserName",FALSE);
  if (ournamep && ourname)
    *ournamep = ourname;

  c = find_config(main_server->conf,CONF_PARAM,"GroupPassword",TRUE);

  if (c) do {
    grp = auth_getgrnam(p,c->argv[0]);

    if (!grp)
      continue;

    for(grmem = grp->gr_mem; *grmem; grmem++)
      if (!strcmp(*grmem,user)) {
        if (auth_check(p,c->argv[1],user,pass) == 0)
          break;
      }

    if (*grmem) {
      if (group)
        *group = c->argv[0];

      if (c->parent)
        c = c->parent;

      if (c->config_type == CONF_ANON)
        anonname = (char*)get_param_ptr(c->subset,"UserName",FALSE);
      if (anonnamep)
        *anonnamep = anonname;
      if (anonnamep && !anonname && ourname)
        *anonnamep = ourname;

      break;
    }
  } while((c = find_config_next(c,c->next,CONF_PARAM,"GroupPassword",TRUE)) != NULL);

  return c;
}

static config_rec *_auth_anonymous_group(pool *p, char *user)
{
  config_rec *c;
  int ret = 0;

  /* retrieve the session group membership information, so that this check
   * may work properly
   */
  if (!session.gids && !session.groups &&
      (ret = auth_getgroups(p, user, &session.gids, &session.groups)) < 1)
    log_debug(DEBUG2, "no supplemental groups found for user '%s'", user);

  c = find_config(main_server->conf,CONF_PARAM,"AnonymousGroup",FALSE);

  if (c) do {
    ret = group_expression((char**)c->argv);
  } while(!ret && (c = find_config_next(c,c->next,CONF_PARAM,"AnonymousGroup",FALSE)) != NULL);

  return ret ? c : NULL;
}

static config_rec *_auth_resolve_user(pool *p,char **user,
                                      char **ournamep,
                                      char **anonnamep)
{
  config_rec *c = NULL, *topc = NULL;
  char *ourname,*anonname = NULL;
  unsigned char is_alias = FALSE, force_anon = FALSE, *auth_alias_only = NULL;

  /* Precendence rules:
   *   1. Search for UserAlias directive.
   *   2. Search for Anonymous directive.
   *   3. Normal user login
   */

  ourname = (char*)get_param_ptr(main_server->conf,"UserName",FALSE);

  if (ournamep && ourname)
    *ournamep = ourname;

  c = find_config(main_server->conf, CONF_PARAM, "UserAlias", TRUE);

  if (c)
    do {
      if (!strcmp(c->argv[0], "*") ||
          !strcmp(c->argv[0], *user)) {
        is_alias = TRUE;
        break;
      }
    } while ((c = find_config_next(c, c->next, CONF_PARAM, "UserAlias",
      TRUE)) != NULL);

  topc = c;

  while (c && c->parent &&
    (auth_alias_only = get_param_ptr(c->parent->set, "AuthAliasOnly", FALSE))) {

    /* If AuthAliasOnly is on, ignore this one and continue. */
    if (auth_alias_only && *auth_alias_only == TRUE)
      continue;

    is_alias = FALSE;

    find_config_set_top(topc);
    c = find_config_next(c, c->next, CONF_PARAM, "UserAlias", TRUE);

    if (c && (!strcmp(c->argv[0], "*") || !strcmp(c->argv[0], *user)))
      is_alias = TRUE;
  }

  if (c) {
    *user = c->argv[1];

    /* If the alias is applied inside an <Anonymous> context, we have found
     * our anon block
     */

    if (c->parent && c->parent->config_type == CONF_ANON)
      c = c->parent;
    else
      c = NULL;
  }

  /* Next, search for an anonymous entry */

  if (!c)
    c = find_config(main_server->conf, CONF_ANON, NULL, FALSE);
  else
    find_config_set_top(c);

  if (c) do {
    anonname = (char *) get_param_ptr(c->subset, "UserName", FALSE);
    if (!anonname)
      anonname = ourname;

    if (anonname && !strcmp(anonname, *user)) {
       if (anonnamep)
         *anonnamep = anonname;
       break;
    }
  } while((c = find_config_next(c, c->next, CONF_ANON, NULL, FALSE)) != NULL);

  if (!c) {
    c = _auth_anonymous_group(p, *user);

    if (c)
      force_anon = TRUE;
  }

  if (!is_alias && !force_anon) {
    unsigned char *auth_alias_only = get_param_ptr(c ? c->subset :
      main_server->conf, "AuthAliasOnly", FALSE);

    if (auth_alias_only && *auth_alias_only == TRUE) {
      if (c && c->config_type == CONF_ANON)
        c = NULL;
      else
        *user = NULL;

      auth_alias_only = get_param_ptr(main_server->conf, "AuthAliasOnly",
        FALSE);
      if (*user && auth_alias_only && *auth_alias_only == TRUE)
        *user = NULL;

      if ((!user || !c) && anonnamep)
        *anonnamep = NULL;
    }
  }

  return c;
}

static unsigned char auth_check_ftpusers(xaset_t *s, const char *user) {
  unsigned char res = TRUE;
  FILE *ftpusersf = NULL;
  char *u, buf[256] = {'\0'};
  unsigned char *use_ftp_users = get_param_ptr(s, "UseFtpUsers", FALSE);

  if (!use_ftp_users || *use_ftp_users == TRUE) {
    PRIVS_ROOT
    ftpusersf = fopen(FTPUSERS_PATH, "r");
    PRIVS_RELINQUISH

    if (!ftpusersf)
      return res;

    while (fgets(buf, sizeof(buf)-1, ftpusersf)) {
      buf[sizeof(buf)-1] = '\0'; CHOP(buf);

      u = buf; while (isspace((int) *u) && *u) u++;

      if (!*u || *u == '#')
        continue;

      if (!strcmp(u, user)) {
        res = FALSE;
        break;
      }
    }

    fclose(ftpusersf);
  }

  return res;
}

static unsigned char auth_check_shell(xaset_t *s, const char *shell) {
  unsigned char res = TRUE;
  FILE *shellf = NULL;
  char buf[256] = {'\0'};
  unsigned char *require_valid_shell = get_param_ptr(s, "RequireValidShell",
    FALSE);

  if (!require_valid_shell || *require_valid_shell == TRUE) {
    if ((shellf = fopen(VALID_SHELL_PATH, "r")) == NULL)
      return res;

    res = FALSE;

    while (fgets(buf, sizeof(buf)-1, shellf)) {
      buf[sizeof(buf)-1] = '\0'; CHOP(buf);

      if (!strcmp(shell, buf)) {
        res = TRUE;
        break;
      }
    }

    fclose(shellf);
  }

  return res;
}

/* Determine any applicable chdirs
 */

static char *_get_default_chdir(pool *p, xaset_t *conf) {
  config_rec *c;
  char *dir = NULL;
  int ret;

  c = find_config(conf,CONF_PARAM,"DefaultChdir",FALSE);

  while(c) {
    /* Check the groups acl */
    if (c->argc < 2) {
      dir = c->argv[0];
      break;
    }

    ret = group_expression(((char**)c->argv)+1);

    if (ret) {
      dir = c->argv[0];
      break;
    }

    c = find_config_next(c,c->next,CONF_PARAM,"DefaultChdir",FALSE);
  }

  /* if the directory is relative, concatenate w/ session.cwd
   */

  if (dir && *dir != '/' && *dir != '~')
    dir = pdircat(p,session.cwd,dir,NULL);

  return dir;
}

/* Determine if the user (non-anon) needs a default root dir
 * other than /
 */

static char *_get_default_root(pool *p)
{
  config_rec *c;
  char *dir = NULL;
  int ret;

  c = find_config(main_server->conf,CONF_PARAM,"DefaultRoot",FALSE);

  while(c) {
    /* Check the groups acl */
    if (c->argc < 2) {
      dir = c->argv[0];
      break;
    }

    ret = group_expression(((char**)c->argv)+1);

    if (ret) {
      dir = c->argv[0];
      break;
    }

    c = find_config_next(c,c->next,CONF_PARAM,"DefaultRoot",FALSE);
  }

  if (dir) {
    if (!strcmp(dir,"/"))
      dir = NULL;
    else {
      char *realdir;

      /*
      ** We need to be the final user here so that if the user has their home
      ** directory with a mode the user proftpd is running (ie the User
      ** directive) as can not traverse down, we can still have the default
      ** root as ~/public_html/
      */
      PRIVS_USER

      realdir = dir_realpath(p,dir);

      PRIVS_RELINQUISH

      if (realdir)
        dir = realdir;
    }
  }

  return dir;
}

static struct passwd *passwd_dup(pool *p, struct passwd *pw)
{
  struct passwd *npw;

  npw = pcalloc(p,sizeof(struct passwd));

  npw->pw_name = pstrdup(p,pw->pw_name);
  npw->pw_passwd = pstrdup(p,pw->pw_passwd);
  npw->pw_uid = pw->pw_uid;
  npw->pw_gid = pw->pw_gid;
  npw->pw_gecos = pstrdup(p,pw->pw_gecos);
  npw->pw_dir = pstrdup(p,pw->pw_dir);
  npw->pw_shell = pstrdup(p,pw->pw_shell);

  return npw;
}

static void ensure_open_passwd(pool *p)
{
  /* Make sure pass/group is open.
   */
  auth_setpwent(p);
  auth_setgrent(p);

  /* On some unices the following is necessary to ensure the files
   * are open.  (BSDI 3.1)
   */
  auth_getpwent(p);
  auth_getgrent(p);
}

/* Next function (the biggie) handles all authentication, setting
 * up chroot() jail, etc.
 */
static int _setup_environment(pool *p, char *user, char *pass)
{
  struct passwd *pw;
  struct stat sbuf;
  config_rec *c;
  char *origuser,*ourname,*anonname = NULL,*anongroup = NULL,*ugroup = NULL;
  char ttyname[20] = {'\0'}, *defaulttransfermode;
  char *defroot = NULL,*defchdir = NULL,*xferlog = NULL;
  int aclp,i,force_anon = 0,res = 0,showsymlinks;
  unsigned char *wtmp_log = NULL, *anon_require_passwd = NULL;

  /********************* Authenticate the user here *********************/

  session.hide_password = TRUE;

  origuser = user;
  c = _auth_resolve_user(p,&user,&ourname,&anonname);
  if (c) session.anon_config = c;

  if (!user) {
    log_auth(PR_LOG_NOTICE, "USER %s: user is not a UserAlias from %s [%s] "
      "to %s:%i", origuser,session.c->remote_name,
      inet_ascii(p, session.c->remote_ipaddr),
      inet_ascii(p, session.c->local_ipaddr), session.c->local_port);
    goto auth_failure;
  }

  if ((pw = auth_getpwnam(p, user)) == NULL) {
    log_auth(PR_LOG_NOTICE, "USER %s: no such user found from %s [%s] to %s:%i",
      user, session.c->remote_name, inet_ascii(p, session.c->remote_ipaddr),
      inet_ascii(p, session.c->local_ipaddr), session.c->local_port);
    goto auth_failure;
  }

  /* security: other functions perform pw lookups, thus we need to make
   * a local copy of the user just looked up
   */

  pw = passwd_dup(p,pw);

  if (pw->pw_uid == 0) {
    unsigned char *root_allow = NULL;

    /* If RootLogin is set to true, we allow this... even though we
     * still log a warning. :)
     */
    if ((root_allow = get_param_ptr(c ? c->subset : main_server->conf,
        "RootLogin", FALSE)) == NULL || *root_allow != TRUE) {
      pr_memscrub(pass, strlen(pass));
      log_auth(PR_LOG_CRIT, "SECURITY VIOLATION: root login attempted.");
      return 0;

    } else {
      log_auth(PR_LOG_WARNING, "ROOT FTP login successful.");
    }
  }

  session.user = pstrdup(p, pw->pw_name);
  session.group = pstrdup(p, auth_gid_name(p, pw->pw_gid));

  /* Set the login_uid and login_uid */
  session.login_uid = pw->pw_uid;
  session.login_gid = pw->pw_gid;

  /* Get the supplemental groups */
  if (!session.gids && !session.groups &&
      (res = auth_getgroups(p, pw->pw_name, &session.gids,
       &session.groups)) < 1)
    log_debug(DEBUG2, "no supplemental groups found for user '%s'",
      pw->pw_name);

  /* If c != NULL from this point on, we have an anonymous login */
  aclp = login_check_limits(main_server->conf,FALSE,TRUE,&i);

  /* set force_anon (for AnonymousGroup) and build a custom
   * anonymous config for this session.
   */
  if (c && c->config_type != CONF_ANON) {
    force_anon = 1;

    defroot = _get_default_root(session.pool);
    if (!defroot)
      defroot = pstrdup(session.pool,pw->pw_dir);

    c = (config_rec*)pcalloc(session.pool,sizeof(config_rec));
    c->config_type = CONF_ANON;
    c->name = defroot;

    anonname = pw->pw_name;

    /* hackery, we trick everything else by pointing the subset
     * at the main server's configuration.  tricky, eh?
     */
     c->subset = main_server->conf;
  }

  if (c) {
    if (!force_anon) {
        anongroup = (char*)get_param_ptr(c->subset,"GroupName",FALSE);
      if (!anongroup)
        anongroup = (char*)get_param_ptr(main_server->conf,"GroupName",FALSE);
    }

    if (!login_check_limits(c->subset,FALSE,TRUE,&i) || (!aclp && !i) ){
      log_auth(PR_LOG_NOTICE, "ANON %s (Login failed): Limit access denies "
        "login.", origuser);
      goto auth_failure;
    }
  }

  if (!c && !aclp) {
    log_auth(PR_LOG_NOTICE, "USER %s (Login failed): Limit access denies login",
      origuser);
    goto auth_failure;
  }

  if (c)
    anon_require_passwd = get_param_ptr(c->subset, "AnonRequirePassword",
      FALSE);

  if (!c || (anon_require_passwd && *anon_require_passwd == TRUE)) {
    int auth_code;
    char *user_name = user;

    if (c && origuser && strcasecmp(user, origuser)) {
      unsigned char *auth_using_alias = get_param_ptr(c->subset,
        "AuthUsingAlias", FALSE);

      /* If 'AuthUsingAlias' set and we're logging in under an alias,
       * then auth using that alias.
       */
      if (auth_using_alias && *auth_using_alias == TRUE) {
        user_name = origuser;
        log_auth(PR_LOG_NOTICE, "ANON AUTH: User %s, Auth Alias %s", user,
          user_name);
      }
    }

    auth_code = _do_auth(p, c ? c->subset : main_server->conf, user_name, pass);

    if (auth_code) {
      /* Normal authentication has failed, see if group authentication
       * passes
       */

      if ((c = _auth_group(p, user, &anongroup, &ourname, &anonname,
          pass)) != NULL) {
        if (c->config_type != CONF_ANON) {
          c = NULL;
          ugroup = anongroup;
          anongroup = NULL;
        }

        auth_code = PR_AUTH_OK;
      }
    }

    pr_memscrub(pass, strlen(pass));

    switch (auth_code) {

      /* Use an RFC2228 response code if authenticated by an RFC2228 module. */
      case PR_AUTH_RFC2228_OK:
        auth_pass_resp_code = R_232;
        break;

      case PR_AUTH_OK:
        auth_pass_resp_code = R_230;
        break;

      case PR_AUTH_NOPWD:
        log_auth(PR_LOG_NOTICE, "USER %s (Login failed): No such user found.",
          user);
        goto auth_failure;

      case PR_AUTH_BADPWD:
        log_auth(PR_LOG_NOTICE, "USER %s (Login failed): Incorrect password.",
          origuser);
        goto auth_failure;

      case PR_AUTH_AGEPWD:
        log_auth(PR_LOG_NOTICE, "USER %s (Login failed): Password expired.",
          user);
        goto auth_failure;

      case PR_AUTH_DISABLEDPWD:
        log_auth(PR_LOG_NOTICE, "USER %s (Login failed): Account disabled.",
          user);
        goto auth_failure;

      default:
        break;
    };

    /* Catch the case where we forgot to handle a bad auth code above. */
    if (auth_code < 0)
      goto auth_failure;

  } else if (c && (!anon_require_passwd || *anon_require_passwd == FALSE)) {
    session.hide_password = FALSE;
  }

  auth_setgrent(p);

  if (!auth_check_shell((c ? c->subset : main_server->conf), pw->pw_shell)) {
    log_auth(PR_LOG_NOTICE, "USER %s (Login failed): Invalid shell: %s", user,
      pw->pw_shell);
    goto auth_failure;
  }

  if (!auth_check_ftpusers((c ? c->subset : main_server->conf), pw->pw_name)) {
    log_auth(PR_LOG_NOTICE, "USER %s (Login failed): User in " FTPUSERS_PATH,
      user);
    goto auth_failure;
  }

  if (c) {
    struct group *grp = NULL;
    unsigned char *add_userdir = NULL;
    char *u = get_param_ptr(main_server->conf, C_USER, FALSE);

    add_userdir = get_param_ptr((c ? c->subset : main_server->conf),
      "UserDirRoot", FALSE);

    /* If resolving an <Anonymous> user, make sure that user's groups
     * are set properly for the check of the home directory path (which
     * depend on those supplemental group memberships).  Additionally,
     * temporarily switch to the new user's uid.
     */

    block_signals();

    PRIVS_ROOT
    if ((res = set_groups(p, pw->pw_gid, session.gids)) < 0)
      log_pri(PR_LOG_ERR, "error: unable to set groups: %s",
        strerror(errno));
#ifdef __hpux
    setresuid(0,0,0);
    setresgid(0,0,0);
#else
    setuid(0);
    setgid(0);
#endif
    PRIVS_SETUP(pw->pw_uid, pw->pw_gid)

    if ((add_userdir && *add_userdir == TRUE) && strcmp(u, user))
      session.chroot_path = dir_realpath(p, pdircat(p, c->name, u, NULL));
    else
      session.chroot_path = dir_realpath(p, c->name);

    /* Check access using access_check() which uses euid instead of ruid,
     * if everything is ok copy it into the session pool. -jss 2/22/2001
     */

    if (session.chroot_path && access_check(session.chroot_path, X_OK) != 0)
      session.chroot_path = NULL;
    else
      session.chroot_path = pstrdup(session.pool, session.chroot_path);

    /* return all privileges back to that of the daemon, for now */
    PRIVS_ROOT
    if ((res = set_groups(p, daemon_gid, daemon_gids)) < 0)
      log_pri(PR_LOG_ERR, "error: unable to set groups: %s",
        strerror(errno));
#ifdef __hpux
    setresuid(0,0,0);
    setresgid(0,0,0);
#else
    setuid(0);
    setgid(0);
#endif
    PRIVS_SETUP(daemon_uid, daemon_gid)

    unblock_signals();

    /* Sanity check, make sure we have daemon_uid and daemon_gid back */
#ifdef HAVE_GETEUID
    if (getegid() != daemon_gid ||
        geteuid() != daemon_uid) {

      PRIVS_RELINQUISH

      log_pri(PR_LOG_ERR, "changing from %s back to daemon uid/gid: %s",
            session.user, strerror(errno));

      end_login(1);
    }
#endif /* HAVE_GETEUID */

    if (anon_require_passwd && *anon_require_passwd == TRUE)
      session.anon_user = pstrdup(session.pool, origuser);
    else
      session.anon_user = pstrdup(session.pool, pass);

    if (!session.chroot_path) {
      log_pri(PR_LOG_ERR, "%s: Directory %s is not accessible.", session.user,
        c->name);
      add_response_err(R_530, "Unable to set anonymous privileges.");
      goto auth_failure;
    }

    sstrncpy(session.cwd, "/", sizeof(session.cwd));
    xferlog = get_param_ptr(c->subset,"TransferLog",FALSE);

    if (anongroup) {
      grp = auth_getgrnam(p,anongroup);
      if (grp) {
        pw->pw_gid = grp->gr_gid;
        session.group = pstrdup(p,grp->gr_name);
      }
    }
  } else {
    struct group *grp;
    char *homedir;

    if (ugroup) {
      grp = auth_getgrnam(p,ugroup);
      if (grp) {
        pw->pw_gid = grp->gr_gid;
        session.group = pstrdup(p,grp->gr_name);
      }
    }

    /* Attempt to resolve any possible symlinks. */
    PRIVS_USER
    homedir = dir_realpath(p, pw->pw_dir);
    PRIVS_RELINQUISH

    if (homedir)
      sstrncpy(session.cwd, homedir, MAX_PATH_LEN);
    else
      sstrncpy(session.cwd, pw->pw_dir, MAX_PATH_LEN);
  }

  /* Get default chdir (if any) */
  defchdir = _get_default_chdir(p,(c ? c->subset : main_server->conf));

  if (defchdir)
    sstrncpy(session.cwd, defchdir, MAX_PATH_LEN);

  /* check limits again to make sure deny/allow directives still permit
   * access.
   */

  if (!login_check_limits((c ? c->subset : main_server->conf), FALSE, TRUE,
      &i)) {
    log_auth(PR_LOG_NOTICE, "%s %s: Limit access denies login.",
      (c != NULL) ? "ANON" : C_USER, origuser);
    goto auth_failure;
  }

  /* Perform a directory fixup. */
  resolve_defered_dirs(main_server);
  fixup_dirs(main_server, CF_DEFER);

  /* If running under an anonymous context, resolve all <Directory>
   * blocks inside it.
   */
  if (c && c->subset)
    resolve_anonymous_dirs(c->subset);

  log_auth(PR_LOG_NOTICE, "%s %s: Login successful.",
    (c != NULL) ? "ANON" : C_USER, origuser);

  /* Write the login to wtmp.  This must be done here because we won't
   * have access after we give up root.  This can result in falsified
   * wtmp entries if an error kicks the user out before we get
   * through with the login process.  Oh well.
   */

#if (defined(BSD) && (BSD >= 199103))
  snprintf(ttyname, sizeof(ttyname), "ftp%ld",(long)getpid());
#else
  snprintf(ttyname, sizeof(ttyname), "ftpd%d",(int)getpid());
#endif

  /* Perform wtmp logging only if not turned off in <Anonymous>
   * or the current server
   */
  if (c)
    wtmp_log = get_param_ptr(c->subset, "WtmpLog", FALSE);

  if (wtmp_log == NULL)
    wtmp_log = get_param_ptr(main_server->conf, "WtmpLog", FALSE);

  PRIVS_ROOT

  if (!wtmp_log || *wtmp_log == TRUE) {
    log_wtmp(ttyname, session.user, session.c->remote_name,
      session.c->remote_ipaddr);
    session.wtmp_log = TRUE;
  }

  /* Open any TransferLogs */
  if (!xferlog) {
    if (c)
      xferlog = get_param_ptr(c->subset, "TransferLog", FALSE);

    if (!xferlog)
      xferlog = get_param_ptr(main_server->conf, "TransferLog", FALSE);

    if (!xferlog)
      xferlog = XFERLOG_PATH;
  }

  if (strcasecmp(xferlog, "NONE") == 0)
    log_open_xfer(NULL);
  else
    log_open_xfer(xferlog);

  if ((res = set_groups(p, pw->pw_gid, session.gids)) < 0)
    log_pri(PR_LOG_ERR, "error: unable to set groups: %s",
      strerror(errno));

  PRIVS_RELINQUISH

  /* Now check to see if the user has an applicable DefaultRoot */
  if (!c && (defroot = _get_default_root(session.pool))) {

    ensure_open_passwd(p);

    if (lockdown(defroot) == -1) {
      log_pri(PR_LOG_ERR, "error: unable to set default root directory");
      send_response(R_530, "Login incorrect.");
      end_login(1);
    }

    /* Re-calc the new cwd based on this root dir.  If not applicable
     * place the user in / (of defroot)
     */

    if (strncmp(session.cwd,defroot,strlen(defroot)) == 0) {
      char *newcwd = &session.cwd[strlen(defroot)];

      if (*newcwd == '/')
        newcwd++;
      session.cwd[0] = '/';
      sstrncpy(&session.cwd[1], newcwd, sizeof(session.cwd));
    }
  }

  if (c)
    ensure_open_passwd(p);

  if (c && lockdown(session.chroot_path) == -1) {
    log_pri(PR_LOG_ERR, "error: unable to set anonymous privileges");
    send_response(R_530, "Login incorrect.");
    end_login(1);
  }

  /* new in 1.1.x, I gave in and we don't give up root permanently..
   * sigh.
   */

#ifndef __hpux
  block_signals();

  PRIVS_ROOT

  setuid(0);
  setgid(0);

  PRIVS_SETUP(pw->pw_uid, pw->pw_gid)

  unblock_signals();
#else
  session.uid = session.ouid = pw->pw_uid;
  session.gid = pw->pw_gid;
  PRIVS_RELINQUISH
#endif

#ifdef HAVE_GETEUID
  if (getegid() != pw->pw_gid ||
     geteuid() != pw->pw_uid) {

    PRIVS_RELINQUISH
    log_pri(PR_LOG_ERR, "error: %s setregid() or setreuid(): %s",
      session.user, strerror(errno));
    send_response(R_530, "Login incorrect.");

    end_login(1);
  }
#endif

  /* If the home directory is NULL or "", reject the login. */
  if (pw->pw_dir == NULL || !strcmp(pw->pw_dir, "")) {
    log_pri(PR_LOG_ERR, "error: user %s home directory is NULL or \"\"",
      session.user);
    send_response(R_530, "Login incorrect.");
    end_login(1);
  }

  {
    unsigned char *show_symlinks = get_param_ptr(
      c ? c->subset : main_server->conf, "ShowSymlinks", FALSE);

    if (show_symlinks && *show_symlinks == TRUE)
      showsymlinks = TRUE;
    else
      showsymlinks = FALSE;
  }

  /* chdir to the proper directory, do this even if anonymous
   * to make sure we aren't outside our chrooted space.
   */

  /* Attempt to change to the correct directory -- use session.cwd first.
   * This will contain the DefaultChdir directory, if configured...
   */
  if (pr_fsio_chdir_canon(session.cwd, !showsymlinks) == -1) {

    /* if we've got DefaultRoot or anonymous login, ignore this error
     * and chdir to /
     */

    if (session.chroot_path != NULL || defroot) {

      log_debug(DEBUG2, "unable to chdir to %s (%s), defaulting to chroot "
        "directory %s", session.cwd, strerror(errno),
        (session.chroot_path ? session.chroot_path : defroot));

      if (pr_fsio_chdir_canon("/", !showsymlinks) == -1) {
        log_pri(PR_LOG_ERR, "%s chdir(\"/\"): %s", session.user,
          strerror(errno));
        send_response(R_530,"Login incorrect.");
        end_login(1);
      }

    } else if (defchdir) {

      /* If we've got defchdir, failure is ok as well, simply switch to
       * user's homedir.
       */
      log_debug(DEBUG2, "unable to chdir to %s (%s), defaulting to home "
        "directory %s", session.cwd, strerror(errno), pw->pw_dir);

      if (pr_fsio_chdir_canon(pw->pw_dir, !showsymlinks) == -1) {
        log_pri(PR_LOG_ERR, "%s chdir(\"%s\"): %s", session.user, session.cwd,
          strerror(errno));
        send_response(R_530, "Login incorrect.");
        end_login(1);
      }

    } else {

      /* Unable to switch to user's real home directory, which is not
       * allowed.
       */
      log_pri(PR_LOG_ERR, "%s chdir(\"%s\"): %s", session.user, session.cwd,
        strerror(errno));
      send_response(R_530, "Login incorrect.");
      end_login(1);
    }
  }

  sstrncpy(session.cwd, pr_fs_getcwd(), sizeof(session.cwd));
  sstrncpy(session.vwd, pr_fs_getvwd(), sizeof(session.vwd));

  /* Make sure session.dir_config is set correctly */
  dir_check_full(p, C_PASS, G_NONE, session.cwd, NULL);

  if (c) {
    if (!session.hide_password)
      session.proc_prefix =
      pstrcat(permanent_pool,session.c->remote_name,
              ": anonymous/",pass,NULL);
    else
      session.proc_prefix =
      pstrcat(permanent_pool,session.c->remote_name,
              ": anonymous",NULL);

    session.sf_flags = SF_ANON;

  } else {

    session.proc_prefix = pstrdup(permanent_pool,session.c->remote_name);
    session.sf_flags = 0;
  }

   /* Check for dynamic configuration.  This check needs to be after the
    * setting of any possible anon_config, as that context may be allowed
    * or denied .ftpaccess-parsing separately from the containing server.
    */
   if (pr_fsio_stat(session.cwd, &sbuf) != -1)
     build_dyn_config(p, session.cwd, &sbuf, TRUE);

  /* While closing the pointer to the password database would avoid any
   * potential attempt to hijack this information, it is unfortunately needed
   * in a chroot()ed environment.  Otherwise, mappings from UIDs to names,
   * among other things, would fail. - MacGyver
   */
  /* auth_endpwent(p); */

  /* Default transfer mode is ASCII */
  defaulttransfermode = (char*)get_param_ptr(main_server->conf,
    "DefaultTransferMode", FALSE);

  if (defaulttransfermode && strcasecmp(defaulttransfermode, "binary") == 0)
        session.sf_flags &= (SF_ALL^SF_ASCII);
  else
        session.sf_flags |= SF_ASCII;

  /* Authentication complete, user logged in, now kill the login
   * timer.
   */

  /* Update the scoreboard entry */
  pr_scoreboard_update_entry(getpid(),
    PR_SCORE_USER, session.user,
    PR_SCORE_CWD, session.cwd,
    NULL);

  session_set_idle();

  remove_timer(TIMER_LOGIN, &auth_module);

  /* These copies are made from the permanent_pool, instead of the more
   * volatile pool used originally, in order that the copied data maintain
   * its integrity for the lifetime of the session.
   */
  session.user = pstrdup(permanent_pool,session.user);

  if (session.group)
    session.group = pstrdup(permanent_pool,session.group);

  if (session.gids)
    session.gids = copy_array(permanent_pool, session.gids);

  /* session.groups is an array of strings, so we must copy the string data
   * as well as the pointers.
   */
  session.groups = copy_array_str(permanent_pool, session.groups);

  /* Resolve any deferred-resolution paths in the FS layer */
  pr_resolve_fs_map();

  return 1;

auth_failure:
  pr_memscrub(pass, strlen(pass));
  session.user = session.group = NULL;
  session.gids = session.groups = NULL;
  session.wtmp_log = FALSE;
  return 0;
}

/* This function counts the number of connected users. It only fills in the
 * Class-based counters and an estimate for the number of clients. The primary
 * purpose is to make it so that the %N/%y escapes work in a DisplayConnect
 * greeting.
 */
static void auth_scan_scoreboard(void) {
  config_rec *c = NULL;
  pr_scoreboard_entry_t *score = NULL;
  int cur = -1, ccur = -1;
  char config_class_users[128] = {'\0'};
  xaset_t *conf = NULL;
  unsigned char *class_engine = get_param_ptr(main_server->conf,
    "Classes", FALSE);

  if (!class_engine || *class_engine == FALSE)
    return;

  if (!session.class)
    return;

  /* Determine how many users are currently connected */
  pr_rewind_scoreboard();
  while ((score = pr_scoreboard_read_entry()) != NULL) {

    /* Make sure it matches our current server */
    if (score->sce_server_ip->s_addr == main_server->ipaddr->s_addr &&
        score->sce_server_port == main_server->ServerPort) {
        
      cur++;

      /* Note: the class member of the scoreboard entry will never be
       * NULL.  At most, it may be the empty string.
       */
      if (strcasecmp(score->sce_class, session.class->name) == 0)
        ccur++;
    }
  }
  pr_restore_scoreboard();

  /* This silliness is needed to get past the broken HP/UX 11.x compiler.
   */
  conf = CURRENT_CONF;

  remove_config(CURRENT_CONF, "CURRENT-CLIENTS", FALSE);
  c = add_config_param_set(&conf, "CURRENT-CLIENTS", 1, NULL);
  c->argv[0] = pcalloc(c->pool, sizeof(int));
  *((int *) c->argv[0]) = cur;

  remove_config(CURRENT_CONF, "CURRENT-CLASS", FALSE);
  add_config_param_set(&conf, "CURRENT-CLASS", 1, session.class->name);

  snprintf(config_class_users, sizeof(config_class_users),
    "CURRENT-CLIENTS-CLASS-%s", session.class->name);
  remove_config(CURRENT_CONF, config_class_users, FALSE);
  c = add_config_param_set(&conf, config_class_users, 1, NULL);
  c->argv[0] = pcalloc(c->pool, sizeof(int));
  *((int *) c->argv[0]) = ccur;
}

static void auth_count_scoreboard(cmd_rec *cmd, char *user) {
  pr_scoreboard_entry_t *score = NULL;
  long cur = 0, hcur = 0, ccur = 0, hostsperuser = 1, usersessions = 0;
  config_rec *c, *maxc;
  char *origuser, config_class_users[128] = {'\0'};
  unsigned char classes_enabled = FALSE,
    *class_engine = get_param_ptr(main_server->conf, "Classes", FALSE);

  if (class_engine && *class_engine == TRUE)
    classes_enabled = TRUE;

  /* NOTE: there is an assumption here that if Classes have been enabled,
   * there will be a corresponding Class defined.  This can cause a
   * SIGSEGV if not caught.
   *
   * The catch is this: if Classes are enabled, but find_class() returns
   *  NULL, act as if Classes are disabled.
   */
  if (classes_enabled && session.class == NULL)
      classes_enabled = FALSE;

  /* Determine how many users are currently connected. */
  origuser = user;
  c = _auth_resolve_user(cmd->tmp_pool, &user, NULL, NULL);

  /* Gather our statistics. */
  if (user) {
    pr_rewind_scoreboard();
    while ((score = pr_scoreboard_read_entry()) != NULL) {
      unsigned char same_host = FALSE;

      /* Make sure it matches our current server. */
      if (score->sce_server_ip->s_addr == main_server->ipaddr->s_addr &&
          score->sce_server_port == main_server->ServerPort) {

        if ((c && c->config_type == CONF_ANON &&
            !strcmp(score->sce_user, user)) || !c) {
          char *s, *d, ip[32] = {'\0'};
          int mpos = sizeof(ip) - 1;

          cur++;

          s = strchr(score->sce_client_addr, '[');
          d = ip;
        
          if (s != NULL)
            s++;
        
          while (s && *s && *s != ']' && d < ip + mpos)
            *d++ = *s++;
        
          *d = '\0';
        
          /* Count up sessions on a per-host basis. */

          if (!strcmp(ip, inet_ntoa(*session.c->remote_ipaddr))) {
            same_host = TRUE;

            /* This small hack makes sure that hcur is incremented properly
             * when dealing with anonymous logins (the timing of anonymous
             * login updates to the scoreboard makes this...odd).
             */
            if (c && c->config_type == CONF_ANON && hcur == 0)
              hcur = 1;

            hcur++;
          }
        
          /* Take a per-user count of connections. */
          if (!strcmp(score->sce_user, user)) {
            usersessions++;

            /* Count up unique hosts. */
            if (!same_host)
              hostsperuser++;
          }
        }

        if (classes_enabled &&
            !strcasecmp(score->sce_class, session.class->name))
          ccur++;
      }
    }
    pr_restore_scoreboard();
    PRIVS_RELINQUISH
  }

  remove_config(cmd->server->conf, "CURRENT-CLIENTS", FALSE);
  add_config_param_set(&cmd->server->conf, "CURRENT-CLIENTS", 1, (void *) cur);

  if (classes_enabled) {
    remove_config(cmd->server->conf, "CURRENT-CLASS", FALSE);
    add_config_param_set(&cmd->server->conf, "CURRENT-CLASS", 1,
                         session.class->name);

    snprintf(config_class_users, sizeof(config_class_users), "%s-%s",
             "CURRENT-CLIENTS-CLASS", session.class->name);
    remove_config(cmd->server->conf, config_class_users, FALSE);
    add_config_param_set(&cmd->server->conf, config_class_users, 1, ccur);

    /* Too many users in this class? */
    if (ccur >= session.class->max_connections) {
      char *display = NULL;

      if (session.sf_flags & SF_ANON)
        display = (char*) get_param_ptr(session.anon_config->subset,
                                        "DisplayGoAway",FALSE);

      if (!display)
        display = (char*) get_param_ptr(cmd->server->conf,
                                        "DisplayGoAway",FALSE);

      if (display)
        core_display_file(R_530, display, NULL);
      else
        send_response(R_530, "Too many users in your class, "
                      "please try again later.");

      remove_config(cmd->server->conf, C_USER, FALSE);
      remove_config(cmd->server->conf, C_PASS, FALSE);

      log_auth(PR_LOG_NOTICE, "Connection refused (max clients for class %s).",
        session.class->name);

      end_login(0);
    }
  }

  /* Try to determine what MaxClients/MaxHosts limits apply to this session
   * (if any) and count through the runtime file to see if this limit would
   * be exceeded.
   */

  if ((maxc = find_config((c ? c->subset : cmd->server->conf),
      CONF_PARAM, "MaxClientsPerHost", FALSE)) != NULL) {
    char *maxstr = "Sorry, the maximum number clients (%m) from your host are "
      "already connected.";
    unsigned int *max = maxc->argv[0];
    char maxn[20] = {'\0'};

    if (maxc->argc > 1)
      maxstr = maxc->argv[1];

    if (*max && hcur > *max) {
      snprintf(maxn, sizeof(maxn), "%u", *max);
      send_response(R_530, "%s", sreplace(cmd->tmp_pool, maxstr, "%m", maxn,
        NULL));
      log_auth(PR_LOG_NOTICE, "Connection refused (max clients per host %u).",
        *max);
      end_login(0);
    }
  }

  /* Check for any configured MaxClientsPerUser. */
  if ((maxc = find_config((c ? c->subset : cmd->server->conf),
      CONF_PARAM, "MaxClientsPerUser", FALSE)) != NULL) {
    char *maxstr = "Sorry, maximum number of clients (%m) for this user are "
      "already connected.";
    unsigned int *max = maxc->argv[0];
    char maxn[20] = {'\0'};

    if (maxc->argc > 1)
      maxstr = maxc->argv[1];

    if (*max && usersessions >= *max) {
      snprintf(maxn, sizeof(maxn), "%u", *max);
      send_response(R_530, "%s", sreplace(cmd->tmp_pool, maxstr, "%m", maxn,
        NULL));
      log_auth(PR_LOG_NOTICE, "Connection refused (max clients per user %u).",
        *max);
      end_login(0);
    }
  }

  if ((maxc = find_config((c ? c->subset : cmd->server->conf),
      CONF_PARAM, "MaxClients", FALSE)) != NULL) {
    char *maxstr = "Sorry, the maximum number of allowed clients (%m) are "
      "already connected.";
    unsigned int *max = maxc->argv[0];
    char maxn[20] = {'\0'};

    if (maxc->argc > 1)
      maxstr = maxc->argv[1];

    if (*max && cur > *max) {
      snprintf(maxn, sizeof(maxn), "%u", *max);
      send_response(R_530, "%s", sreplace(cmd->tmp_pool, maxstr, "%m", maxn,
        NULL));
      log_auth(PR_LOG_NOTICE, "Connection refused (max clients %u).", *max);
      end_login(0);
    }
  }

  if ((maxc = find_config((c ? c->subset : cmd->server->conf),
      CONF_PARAM, "MaxHostsPerUser", FALSE)) != NULL) {
    char *maxstr = "Sorry, the maximum number of hosts (%m) for this user are "
      "already connected.";
    unsigned int *max = maxc->argv[0];
    char maxn[20] = {'\0'};

    if (maxc->argc > 1)
      maxstr = maxc->argv[1];

    if (*max && hostsperuser > *max) {
      snprintf(maxn, sizeof(maxn), "%u", *max);
      send_response(R_530, "%s", sreplace(cmd->tmp_pool, maxstr, "%m", maxn,
        NULL));
      log_auth(PR_LOG_NOTICE, "Connection refused (max clients per host %u).",
        *max);
      end_login(0);
    }
  }
}

/* Close the passwd and group databases, because libc won't let us see new
 * entries to these files without this (only in PersistentPasswd mode).
 */
MODRET auth_pre_user(cmd_rec *cmd) {
  auth_endpwent(cmd->tmp_pool);
  auth_endgrent(cmd->tmp_pool);

  return DECLINED(cmd);
}

MODRET auth_user(cmd_rec *cmd) {
  int nopass = 0;
  config_rec *c;
  char *user, *origuser;
  int failnopwprompt = 0, aclp, i;
  unsigned char *anon_require_passwd = NULL, *login_passwd_prompt = NULL;

  if (logged_in)
    return ERROR_MSG(cmd, R_503, "You are already logged in!");

  if (cmd->argc < 2)
    return ERROR_MSG(cmd, R_500, C_USER ": command requires a parameter.");

  user = cmd->arg;

  remove_config(cmd->server->conf, C_USER, FALSE);
  remove_config(cmd->server->conf, C_PASS, FALSE);

  c = add_config_param_set(&cmd->server->conf, C_USER, 1, NULL);
  c->argv[0] = pstrdup(c->pool, user);

  origuser = user;
  c = _auth_resolve_user(cmd->tmp_pool,&user,NULL,NULL);

  login_passwd_prompt = get_param_ptr(
    (c && c->config_type == CONF_ANON) ? c->subset : main_server->conf,
    "LoginPasswordPrompt", FALSE);

  if (login_passwd_prompt && *login_passwd_prompt == FALSE)
    failnopwprompt = TRUE;
  else
    failnopwprompt = FALSE;

  if (failnopwprompt) {
    if (!user) {
      remove_config(cmd->server->conf, C_USER, FALSE);
      remove_config(cmd->server->conf, C_PASS, FALSE);

      log_pri(PR_LOG_NOTICE, "USER %s (Login failed): Not a UserAlias.",
        origuser);
      send_response(R_530, "Login incorrect.");

      end_login(0);
    }

    aclp = login_check_limits(main_server->conf,FALSE,TRUE,&i);

    if (c && c->config_type != CONF_ANON) {
      c = (config_rec *) pcalloc(session.pool, sizeof(config_rec));
      c->config_type = CONF_ANON;
      c->name = "";	/* don't really need this yet */
      c->subset = main_server->conf;
    }

    if (c) {
      if (!login_check_limits(c->subset,FALSE,TRUE,&i) || (!aclp && !i) ) {
        remove_config(cmd->server->conf, C_USER, FALSE);
        remove_config(cmd->server->conf, C_PASS, FALSE);

        log_auth(PR_LOG_NOTICE, "ANON %s: Limit access denies login.",
          origuser);
        send_response(R_530, "Login incorrect.");

        end_login(0);
      }
    }

    if (!c && !aclp) {
      remove_config(cmd->server->conf, C_USER, FALSE);
      remove_config(cmd->server->conf, C_PASS, FALSE);

      log_auth(PR_LOG_NOTICE, "USER %s: Limit access denies login.", origuser);
      send_response(R_530, "Login incorrect.");

      end_login(0);
    }
  }

  auth_count_scoreboard(cmd, origuser);

  if (c)
    anon_require_passwd = get_param_ptr(c->subset, "AnonRequirePassword",
      FALSE);

  if (c && user && (!anon_require_passwd || *anon_require_passwd == FALSE))
    nopass++;

  if (nopass)
    add_response(R_331, "Anonymous login ok, send your complete email "
                 "address as your password.");
  else
    add_response(R_331, "Password required for %s.", cmd->argv[1]);

  session.gids = NULL;
  session.groups = NULL;
  session.user = NULL;
  session.group = NULL;

  return HANDLED(cmd);
}

/* Close the passwd and group databases, similar to pre_cmd_user().
 */
MODRET auth_pre_pass(cmd_rec *cmd) {
  auth_endpwent(cmd->tmp_pool);
  auth_endgrent(cmd->tmp_pool);
  return DECLINED(cmd);
}

MODRET auth_pass(cmd_rec *cmd) {
  char *user = NULL;
  int res = 0;

  if (logged_in)
    return ERROR_MSG(cmd, R_503, "You are already logged in!");

  user = get_param_ptr(cmd->server->conf, C_USER, FALSE);

  if (!user) {
    remove_config(cmd->server->conf, C_USER, FALSE);
    remove_config(cmd->server->conf, C_PASS, FALSE);

    return ERROR_MSG(cmd, R_503, "Login with " C_USER " first");
  }

  auth_count_scoreboard(cmd, user);

  if ((res = _setup_environment(cmd->tmp_pool, user, cmd->arg)) == 1) {
    char *display = NULL, *grantmsg = NULL;
    config_rec *c = NULL;

    c = add_config_param_set(&cmd->server->conf, "authenticated", 1, NULL);
    c->argv[0] = pcalloc(c->pool, sizeof(unsigned char));
    *((unsigned char *) c->argv[0]) = TRUE;

    set_auth_check(NULL);

    remove_config(cmd->server->conf, C_PASS, FALSE);

    if (session.sf_flags & SF_ANON) {
      add_config_param_set(&cmd->server->conf, C_PASS, 1,
        pstrdup(cmd->server->pool, cmd->arg));
      display = get_param_ptr(session.anon_config->subset, "DisplayLogin",
        FALSE);
    }

    if (!display)
      display = get_param_ptr(cmd->server->conf, "DisplayLogin", FALSE);

    if (display)
      core_display_file(auth_pass_resp_code, display, NULL);

    if ((grantmsg = get_param_ptr((session.anon_config ?
        session.anon_config->subset : cmd->server->conf),
        "AccessGrantMsg", FALSE)) != NULL) {
      grantmsg = sreplace(cmd->tmp_pool, grantmsg, "%u", user, NULL);

      add_response(auth_pass_resp_code, "%s", grantmsg);

    } else {

      if (session.sf_flags & SF_ANON)
        add_response(auth_pass_resp_code,
          "Anonymous access granted, restrictions apply.");
      else
        add_response(auth_pass_resp_code, "User %s logged in.", user);
    }

    logged_in = 1;
    return HANDLED(cmd);
  }

  remove_config(cmd->server->conf, C_USER, FALSE);
  remove_config(cmd->server->conf, C_PASS, FALSE);

  if (res == 0) {
    unsigned int max_logins, *max = NULL;
    char *denymsg = NULL;

    /* check for AccessDenyMsg */
    if ((denymsg = get_param_ptr((session.anon_config ?
        session.anon_config->subset : cmd->server->conf),
        "AccessDenyMsg", FALSE)) != NULL) {
      denymsg = sreplace(cmd->tmp_pool, denymsg, "%u", user, NULL);
    }

    if ((max = get_param_ptr(main_server->conf, "MaxLoginAttempts",
        FALSE)) == NULL)
      max_logins = 3;
    else
      max_logins = *max;

    if (++auth_tries >= max_logins) {
      if (denymsg)
        send_response(R_530, "%s", denymsg);
      else
        send_response(R_530, "Login incorrect.");

      log_auth(PR_LOG_NOTICE, "Maximum login attempts (%u) exceeded",
        max_logins);
      end_login(0);
    }

    return ERROR_MSG(cmd, R_530, denymsg ? denymsg : "Login incorrect.");
  }

  return HANDLED(cmd);
}

MODRET auth_acct(cmd_rec *cmd) {
  add_response(R_502, "ACCT command not implemented.");
  return HANDLED(cmd);
}

MODRET auth_rein(cmd_rec *cmd) {
  add_response(R_502, "REIN command not implemented.");
  return HANDLED(cmd);
}

/* Configuration handlers
 */

MODRET set_accessdenymsg(cmd_rec *cmd) {
  config_rec *c = NULL;

  CHECK_ARGS(cmd, 1);
  CHECK_CONF(cmd, CONF_ROOT|CONF_VIRTUAL|CONF_GLOBAL|CONF_ANON);

  c = add_config_param_str(cmd->argv[0], 1, cmd->argv[1]);
  c->flags |= CF_MERGEDOWN;

  return HANDLED(cmd);
}

MODRET set_accessgrantmsg(cmd_rec *cmd) {
  config_rec *c = NULL;

  CHECK_ARGS(cmd, 1);
  CHECK_CONF(cmd, CONF_ROOT|CONF_VIRTUAL|CONF_GLOBAL|CONF_ANON);

  c = add_config_param_str(cmd->argv[0], 1, cmd->argv[1]);
  c->flags |= CF_MERGEDOWN;

  return HANDLED(cmd);
}

MODRET set_anonrequirepassword(cmd_rec *cmd) {
  int bool = -1;
  config_rec *c = NULL;

  CHECK_ARGS(cmd, 1);
  CHECK_CONF(cmd, CONF_ANON);

  if ((bool = get_boolean(cmd, 1)) == -1)
    CONF_ERROR(cmd, "expected boolean parameter");

  c = add_config_param(cmd->argv[0], 1, NULL);
  c->argv[0] = pcalloc(c->pool, sizeof(unsigned char));
  *((unsigned char *) c->argv[0]) = bool;

  return HANDLED(cmd);
}

MODRET add_anonymousgroup(cmd_rec *cmd) {
  int argc;
  config_rec *c = NULL;
  char **argv = NULL;
  array_header *acl = NULL;

  if (cmd->argc < 2)
    CONF_ERROR(cmd, "wrong number of parameters");

  CHECK_CONF(cmd, CONF_ROOT|CONF_VIRTUAL|CONF_GLOBAL);

  argv = cmd->argv;
  argc = cmd->argc - 1;

  acl = parse_group_expression(cmd->tmp_pool, &argc, argv);

  c = add_config_param(cmd->argv[0], 0);
  c->argc = argc;
  c->argv = pcalloc(c->pool,(argc+1) * sizeof(char*));
  argv = (char **)c->argv;

  if (argc && acl)
    while (argc--) {
      *argv++ = pstrdup(c->pool, *((char **) acl->elts));
      acl->elts = ((char **) acl->elts) + 1;
    }

  *argv = NULL;

  return HANDLED(cmd);
}

MODRET set_authaliasonly(cmd_rec *cmd) {
  int bool = -1;
  config_rec *c = NULL;

  CHECK_ARGS(cmd, 1);
  CHECK_CONF(cmd, CONF_ROOT|CONF_VIRTUAL|CONF_GLOBAL|CONF_ANON);

  if ((bool = get_boolean(cmd, 1)) == -1)
    CONF_ERROR(cmd, "expected Boolean parameter");

  c = add_config_param(cmd->argv[0], 1, NULL);
  c->argv[0] = pcalloc(c->pool, sizeof(unsigned char));
  *((unsigned char *) c->argv[0]) = bool;

  c->flags |= CF_MERGEDOWN;
  return HANDLED(cmd);
}

MODRET set_authusingalias(cmd_rec *cmd) {
  int bool = -1;
  config_rec *c = NULL;

  CHECK_ARGS(cmd, 1);
  CHECK_CONF(cmd, CONF_ANON);

  if ((bool = get_boolean(cmd,1)) == -1)
    CONF_ERROR(cmd, "expected Boolean parameter");

  c = add_config_param(cmd->argv[0], 1, NULL);
  c->argv[0] = pcalloc(c->pool, sizeof(unsigned char));
  *((unsigned char *) c->argv[0]) = bool;

  return HANDLED(cmd);
}

MODRET add_defaultroot(cmd_rec *cmd) {
  config_rec *c;
  char *dir,**argv;
  int argc;
  array_header *acl = NULL;

  CHECK_CONF(cmd, CONF_ROOT | CONF_VIRTUAL | CONF_GLOBAL);

  if (cmd->argc < 2)
    CONF_ERROR(cmd,"syntax: DefaultRoot <directory> [<group-expression>]");

  argv = cmd->argv;
  argc = cmd->argc - 2;

  dir = *++argv;

  /* dir must be / or ~
   */
  if (*dir != '/' && *dir != '~')
    CONF_ERROR(cmd, pstrcat(cmd->tmp_pool, "(", dir, ") absolute pathname "
                            "required.", NULL));

  if (strchr(dir, '*'))
    CONF_ERROR(cmd, pstrcat(cmd->tmp_pool, "(", dir, ") wildcards not allowed "
                            "in pathname.", NULL));

  if (*(dir + strlen(dir) - 1) != '/')
    dir = pstrcat(cmd->tmp_pool, dir, "/", NULL);

  acl = parse_group_expression(cmd->tmp_pool, &argc, argv);

  c = add_config_param(cmd->argv[0], 0);

  c->argc = argc + 1;
  c->argv = pcalloc(c->pool, (argc + 2) * sizeof(char *));
  argv = (char **) c->argv;
  *argv++ = pstrdup(c->pool, dir);

  if (argc && acl)
    while(argc--) {
      *argv++ = pstrdup(c->pool, *((char **) acl->elts));
      acl->elts = ((char **) acl->elts) + 1;
    }

  *argv = NULL;
  return HANDLED(cmd);
}

MODRET add_defaultchdir(cmd_rec *cmd) {
  config_rec *c;
  char *dir,**argv;
  int argc;
  array_header *acl = NULL;

  CHECK_CONF(cmd, CONF_ROOT|CONF_VIRTUAL|CONF_GLOBAL|CONF_ANON);

  if (cmd->argc < 2)
    CONF_ERROR(cmd, "syntax: DefaultChdir <directory> [<group-expression>]");

  argv = cmd->argv;
  argc = cmd->argc - 2;

  dir = *++argv;

  if (strchr(dir, '*'))
    CONF_ERROR(cmd, pstrcat(cmd->tmp_pool, "(", dir, ") wildcards not allowed "
                            "in pathname.", NULL));

  if (*(dir + strlen(dir) - 1) != '/')
    dir = pstrcat(cmd->tmp_pool, dir, "/", NULL);

  acl = parse_group_expression(cmd->tmp_pool, &argc, argv);

  c = add_config_param(cmd->argv[0], 0);

  c->argc = argc + 1;
  c->argv = pcalloc(c->pool, (argc + 2) * sizeof(char *));
  argv = (char **) c->argv;
  *argv++ = pstrdup(c->pool, dir);

  if (argc && acl)
    while(argc--) {
      *argv++ = pstrdup(c->pool, *((char **) acl->elts));
      acl->elts = ((char **) acl->elts) + 1;
    }

  *argv = NULL;

  c->flags |= CF_MERGEDOWN;
  return HANDLED(cmd);
}

MODRET set_grouppassword(cmd_rec *cmd) {
  config_rec *c = NULL;

  CHECK_ARGS(cmd, 2);
  CHECK_CONF(cmd, CONF_ROOT|CONF_VIRTUAL|CONF_GLOBAL|CONF_ANON);

  c = add_config_param_str(cmd->argv[0], 2, cmd->argv[1], cmd->argv[2]);
  c->flags |= CF_MERGEDOWN;

  return HANDLED(cmd);
}

MODRET set_loginpasswordprompt(cmd_rec *cmd) {
  int bool = -1;
  config_rec *c = NULL;

  CHECK_ARGS(cmd, 1);
  CHECK_CONF(cmd, CONF_ROOT|CONF_VIRTUAL|CONF_GLOBAL|CONF_ANON);

  if ((bool = get_boolean(cmd, 1)) == -1)
    CONF_ERROR(cmd, "expected Boolean parameter");

  c = add_config_param(cmd->argv[0], 1, NULL);
  c->argv[0] = pcalloc(c->pool, sizeof(unsigned char));
  *((unsigned char *) c->argv[0]) = bool;
  c->flags |= CF_MERGEDOWN;

  return HANDLED(cmd);
}

/* usage: MaxClients max|"none" ["message"] */
MODRET set_maxclients(cmd_rec *cmd) {
  int max;
  config_rec *c = NULL;

  if (cmd->argc < 2 || cmd->argc > 3)
    CONF_ERROR(cmd, "wrong number of parameters");

  CHECK_CONF(cmd, CONF_ROOT|CONF_VIRTUAL|CONF_GLOBAL|CONF_ANON);

  if (!strcasecmp(cmd->argv[1], "none"))
    max = 0;

  else {
    char *endp = NULL;

    max = (int) strtol(cmd->argv[1], &endp, 10);

    if ((endp && *endp) || max < 1)
      CONF_ERROR(cmd, "parameter must be 'none' or a number greater than 0");
  }

  if (cmd->argc == 3) {
    c = add_config_param(cmd->argv[0], 2, NULL, NULL);
    c->argv[0] = pcalloc(c->pool, sizeof(unsigned int));
    *((unsigned int *) c->argv[0]) = max;
    c->argv[1] = pstrdup(c->pool, cmd->argv[2]);

  } else {
    c = add_config_param(cmd->argv[0], 1, NULL);
    c->argv[0] = pcalloc(c->pool, sizeof(unsigned int));
    *((unsigned int *) c->argv[0]) = max;
  }

  c->flags |= CF_MERGEDOWN;

  return HANDLED(cmd);
}

/* usage: MaxClientsPerHost max|"none" ["message"] */
MODRET set_maxhostclients(cmd_rec *cmd) {
  int max;
  config_rec *c = NULL;

  if (cmd->argc < 2 || cmd->argc > 3)
    CONF_ERROR(cmd, "wrong number of parameters");

  CHECK_CONF(cmd, CONF_ROOT|CONF_VIRTUAL|CONF_GLOBAL|CONF_ANON);

  if (!strcasecmp(cmd->argv[1], "none"))
    max = 0;

  else {
    char *endp = NULL;

    max = (int) strtol(cmd->argv[1], &endp, 10);

    if ((endp && *endp) || max < 1)
      CONF_ERROR(cmd, "parameter must be 'none' or a number greater than 0");
  }

  if (cmd->argc == 3) {
    c = add_config_param(cmd->argv[0], 2, NULL, NULL);
    c->argv[0] = pcalloc(c->pool, sizeof(unsigned int));
    *((unsigned int *) c->argv[0]) = max;
    c->argv[1] = pstrdup(c->pool, cmd->argv[2]);

  } else {
    c = add_config_param(cmd->argv[0], 1, NULL);
    c->argv[0] = pcalloc(c->pool, sizeof(unsigned int));
    *((unsigned int *) c->argv[0]) = max;
  }

  c->flags |= CF_MERGEDOWN;

  return HANDLED(cmd);
}


/* usage: MaxClientsPerUser max|"none" ["message"] */
MODRET set_maxuserclients(cmd_rec *cmd) {
  int max;
  config_rec *c = NULL;

  if (cmd->argc < 2 || cmd->argc > 3)
    CONF_ERROR(cmd, "wrong number of parameters");

  CHECK_CONF(cmd, CONF_ROOT|CONF_VIRTUAL|CONF_GLOBAL|CONF_ANON);

  if (!strcasecmp(cmd->argv[1], "none"))
    max = 0;

  else {
    char *endp = NULL;

    max = (int) strtol(cmd->argv[1], &endp, 10);

    if ((endp && *endp) || max < 1)
      CONF_ERROR(cmd, "parameter must be 'none' or a number greater than 0");
  }

  if (cmd->argc == 3) {
    c = add_config_param(cmd->argv[0], 2, NULL, NULL);
    c->argv[0] = pcalloc(c->pool, sizeof(unsigned int));
    *((unsigned int *) c->argv[0]) = max;
    c->argv[1] = pstrdup(c->pool, cmd->argv[2]);

  } else {
    c = add_config_param(cmd->argv[0], 1, NULL);
    c->argv[0] = pcalloc(c->pool, sizeof(unsigned int));
    *((unsigned int *) c->argv[0]) = max;
  }

  c->flags |= CF_MERGEDOWN;

  return HANDLED(cmd);
}

/* usage: MaxHostsPerUser max|"none" ["message"] */
MODRET set_maxhostsperuser(cmd_rec *cmd) {
  int max;
  config_rec *c = NULL;

  CHECK_CONF(cmd, CONF_ROOT|CONF_VIRTUAL|CONF_GLOBAL|CONF_ANON);

  if (cmd->argc < 2 || cmd->argc > 3)
    CONF_ERROR(cmd, "wrong number of parameters");

  if (!strcasecmp(cmd->argv[1], "none"))
    max = 0;

  else {
    char *endp = NULL;

    max = (int) strtol(cmd->argv[1], &endp, 10);

    if ((endp && *endp) || max < 1)
      CONF_ERROR(cmd, "parameter must be 'none' or a number greater than 0");
  }

  if (cmd->argc == 3) {
    c = add_config_param(cmd->argv[0], 2, NULL, NULL);
    c->argv[0] = pcalloc(c->pool, sizeof(unsigned int));
    *((unsigned int *) c->argv[0]) = max;
    c->argv[1] = pstrdup(c->pool, cmd->argv[2]);

  } else {
    c = add_config_param(cmd->argv[0], 1, NULL);
    c->argv[0] = pcalloc(c->pool, sizeof(unsigned int));
    *((unsigned int *) c->argv[0]) = max;
  }

  c->flags |= CF_MERGEDOWN;

  return HANDLED(cmd);
}

MODRET set_maxloginattempts(cmd_rec *cmd) {
  int max;
  config_rec *c = NULL;

  CHECK_ARGS(cmd, 1);
  CHECK_CONF(cmd, CONF_ROOT|CONF_VIRTUAL|CONF_GLOBAL);

  if (!strcasecmp(cmd->argv[1], "none"))
    max = 0;

  else {
    char *endp = NULL;
    max = (int) strtol(cmd->argv[1], &endp, 10);

    if ((endp && *endp) || max < 1)
      CONF_ERROR(cmd, "parameter must be 'none' or a number greater than 0");
  }

  c = add_config_param(cmd->argv[0], 1, NULL);
  c->argv[0] = pcalloc(c->pool, sizeof(unsigned int));
  *((unsigned int *) c->argv[0]) = max;

  return HANDLED(cmd);
}

MODRET set_requirevalidshell(cmd_rec *cmd) {
  int bool = -1;
  config_rec *c = NULL;

  CHECK_ARGS(cmd, 1);
  CHECK_CONF(cmd, CONF_ROOT|CONF_VIRTUAL|CONF_GLOBAL|CONF_ANON);

  if ((bool = get_boolean(cmd, 1)) == -1)
    CONF_ERROR(cmd, "expected boolean argument.");

  c = add_config_param(cmd->argv[0], 1, NULL);
  c->argv[0] = pcalloc(c->pool, sizeof(unsigned char));
  *((unsigned char *) c->argv[0]) = bool;
  c->flags |= CF_MERGEDOWN;

  return HANDLED(cmd);
}

MODRET set_rootlogin(cmd_rec *cmd) {
  int bool = -1;
  config_rec *c = NULL;

  CHECK_ARGS(cmd,1);
  CHECK_CONF(cmd, CONF_ROOT|CONF_VIRTUAL|CONF_GLOBAL|CONF_ANON);

  if ((bool = get_boolean(cmd, 1)) == -1)
    CONF_ERROR(cmd, "expected boolean argument.");

  c = add_config_param(cmd->argv[0], 1, NULL);
  c->argv[0] = pcalloc(c->pool, sizeof(unsigned char));
  *((unsigned char *) c->argv[0]) = (unsigned char) bool;
  c->flags |= CF_MERGEDOWN;

  return HANDLED(cmd);
}

MODRET set_timeoutlogin(cmd_rec *cmd) {
  int timeout = -1;
  char *endp = NULL;
  config_rec *c = NULL;

  CHECK_ARGS(cmd, 1);
  CHECK_CONF(cmd, CONF_ROOT|CONF_VIRTUAL|CONF_GLOBAL);

  timeout = (int) strtol(cmd->argv[1], &endp, 10);

  if ((endp && *endp) || timeout < 0 || timeout > 65535)
    CONF_ERROR(cmd, "timeout values must be between 0 and 65535");

  c = add_config_param(cmd->argv[0], 1, NULL);
  c->argv[0] = pcalloc(c->pool, sizeof(int));
  *((int *) c->argv[0]) = timeout;

  return HANDLED(cmd);
}

MODRET set_timeoutsession(cmd_rec *cmd) {
  int seconds = 0, precedence = 0;
  config_rec *c = NULL;

  int ctxt = (cmd->config && cmd->config->config_type != CONF_PARAM ?
     cmd->config->config_type : cmd->server->config_type ?
     cmd->server->config_type : CONF_ROOT);

  /* this directive must have either 1 or 3 arguments */
  if (cmd->argc-1 != 1 && cmd->argc-1 != 3)
    CONF_ERROR(cmd, "missing arguments");

  CHECK_CONF(cmd, CONF_ROOT|CONF_VIRTUAL|CONF_GLOBAL|CONF_ANON);

  /* Set the precedence for this config_rec based on its configuration
   * context.
   */
  if (ctxt & CONF_GLOBAL)
    precedence = 1;

  /* these will never appear simultaneously */
  else if (ctxt & CONF_ROOT || ctxt & CONF_VIRTUAL)
    precedence = 2;

  else if (ctxt & CONF_ANON)
    precedence = 3;

  if ((seconds = atoi(cmd->argv[1])) < 0) {
    CONF_ERROR(cmd, pstrcat(cmd->tmp_pool,
      "seconds must be greater than or equal to 0", NULL));

  } else if (seconds == 0) {

    /* do nothing */
    return HANDLED(cmd);
  }

  if (cmd->argc-1 == 3) {
    if (!strcmp(cmd->argv[2], "user") ||
        !strcmp(cmd->argv[2], "group") ||
        !strcmp(cmd->argv[2], "class")) {

       /* no op */

     } else
       CONF_ERROR(cmd, pstrcat(cmd->tmp_pool, cmd->argv[0],
         ": unknown classifier used: '", cmd->argv[2], "'", NULL));
  }

  if (cmd->argc-1 == 1) {
    c = add_config_param(cmd->argv[0], 2, NULL);
    c->argv[0] = pcalloc(c->pool, sizeof(unsigned int));
    *((unsigned int *) c->argv[0]) = seconds;
    c->argv[1] = pcalloc(c->pool, sizeof(unsigned int));
    *((unsigned int *) c->argv[1]) = precedence;

  } else if (cmd->argc-1 == 3) {
    array_header *acl = NULL;
    int argc = cmd->argc - 3;
    char **argv = cmd->argv + 2;

    acl = parse_group_expression(cmd->tmp_pool, &argc, argv);

    c = add_config_param(cmd->argv[0], 0);
    c->argc = argc + 2;

    /* add 3 to argc for the argv of the config_rec: one for the
     * seconds value, one for the precedence, one for the classifier,
     * and one for the terminating NULL
     */
    c->argv = pcalloc(c->pool, ((argc + 4) * sizeof(char *)));

    /* capture the config_rec's argv pointer for doing the by-hand
     * population
     */
    argv = (char **) c->argv;

    /* Copy in the seconds. */
    *argv = pcalloc(c->pool, sizeof(unsigned int));
    *((unsigned int *) *argv++) = seconds;

    /* Copy in the precedence. */
    *argv = pcalloc(c->pool, sizeof(unsigned int));
    *((unsigned int *) *argv++) = precedence;

    /* Copy in the classifier. */
    *argv++ = pstrdup(c->pool, cmd->argv[2]);

    /* now, copy in the expression arguments */
    if (argc && acl) {
      while (argc--) {
        *argv++ = pstrdup(c->pool, *((char **) acl->elts));
        acl->elts = ((char **) acl->elts) + 1;
      }
    }

    /* don't forget the terminating NULL */
    *argv = NULL;
  }

  c->flags |= CF_MERGEDOWN_MULTI;
  return HANDLED(cmd);
}

MODRET set_useftpusers(cmd_rec *cmd) {
  int bool = -1;
  config_rec *c = NULL;

  CHECK_ARGS(cmd, 1);
  CHECK_CONF(cmd, CONF_ROOT|CONF_VIRTUAL|CONF_GLOBAL|CONF_ANON);

  if ((bool = get_boolean(cmd, 1)) == -1)
    CONF_ERROR(cmd, "expected boolean argument.");

  c = add_config_param(cmd->argv[0], 1, NULL);
  c->argv[0] = pcalloc(c->pool, sizeof(unsigned char));
  *((unsigned char *) c->argv[0]) = bool;
  c->flags |= CF_MERGEDOWN;

  return HANDLED(cmd);
}

MODRET set_useralias(cmd_rec *cmd) {
  config_rec *c = NULL;

  CHECK_ARGS(cmd, 2);
  CHECK_CONF(cmd, CONF_ROOT|CONF_VIRTUAL|CONF_GLOBAL|CONF_ANON);

  c = add_config_param_str(cmd->argv[0], 2, cmd->argv[1], cmd->argv[2]);

  /* Note: only merge this directive down if it is not appearing in an
   * <Anonymous> context.
   */
  if (!check_context(cmd, CONF_ANON))
    c->flags |= CF_MERGEDOWN;

  return HANDLED(cmd);
}

MODRET set_userdirroot(cmd_rec *cmd) {
  int bool = -1;
  config_rec *c = NULL;

  CHECK_ARGS(cmd, 1);
  CHECK_CONF(cmd, CONF_ANON);

  if ((bool = get_boolean(cmd, 1)) == -1)
    CONF_ERROR(cmd, "expected Boolean parameter");

  c = add_config_param(cmd->argv[0], 1, NULL);
  c->argv[0] = pcalloc(c->pool, sizeof(unsigned char));
  *((unsigned char *) c->argv[0]) = bool;

  return HANDLED(cmd);
}

MODRET set_userpassword(cmd_rec *cmd) {
  config_rec *c = NULL;

  CHECK_ARGS(cmd, 2);
  CHECK_CONF(cmd, CONF_ROOT|CONF_VIRTUAL|CONF_GLOBAL|CONF_ANON);

  c = add_config_param_str(cmd->argv[0], 2, cmd->argv[1], cmd->argv[2]);
  c->flags |= CF_MERGEDOWN;

  return HANDLED(cmd);
}

/* Module API tables
 */

static conftable auth_conftab[] = {
  { "AccessDenyMsg",		set_accessdenymsg,		NULL },
  { "AccessGrantMsg",		set_accessgrantmsg,		NULL },
  { "AnonRequirePassword",	set_anonrequirepassword,	NULL },
  { "AnonymousGroup",		add_anonymousgroup,		NULL },
  { "AuthAliasOnly",		set_authaliasonly,		NULL },
  { "AuthUsingAlias",		set_authusingalias,		NULL },
  { "DefaultChdir",		add_defaultchdir,		NULL },
  { "DefaultRoot",		add_defaultroot,		NULL },
  { "GroupPassword",		set_grouppassword,		NULL },
  { "LoginPasswordPrompt",	set_loginpasswordprompt,	NULL },
  { "MaxClients",		set_maxclients,			NULL },
  { "MaxClientsPerHost",	set_maxhostclients,		NULL },
  { "MaxClientsPerUser",	set_maxuserclients,		NULL },
  { "MaxHostsPerUser",		set_maxhostsperuser,		NULL },
  { "MaxLoginAttempts",		set_maxloginattempts,		NULL },
  { "RequireValidShell",	set_requirevalidshell,		NULL },
  { "RootLogin",		set_rootlogin,			NULL },
  { "TimeoutLogin",		set_timeoutlogin,		NULL },
  { "TimeoutSession",		set_timeoutsession,		NULL },
  { "UseFtpUsers",		set_useftpusers,		NULL },
  { "UserAlias",		set_useralias,			NULL },
  { "UserDirRoot",		set_userdirroot,		NULL },
  { "UserPassword",		set_userpassword,		NULL },
  { NULL,			NULL,				NULL }
};

static cmdtable auth_cmdtab[] = {
  { PRE_CMD,	C_USER,	G_NONE,	auth_pre_user,	FALSE,	FALSE,	CL_AUTH },
  { CMD,	C_USER,	G_NONE,	auth_user,	FALSE,	FALSE,	CL_AUTH },
  { PRE_CMD,	C_PASS,	G_NONE,	auth_pre_pass,	FALSE,	FALSE,	CL_AUTH },
  { CMD,	C_PASS,	G_NONE,	auth_pass,	FALSE,	FALSE,	CL_AUTH },
  { POST_CMD,	C_PASS,	G_NONE,	auth_post_pass,	FALSE,	FALSE,	CL_AUTH },
  { CMD,	C_ACCT,	G_NONE,	auth_acct,	FALSE,	FALSE,	CL_AUTH },
  { CMD,	C_REIN,	G_NONE,	auth_rein,	FALSE,	FALSE,	CL_AUTH },
  { 0, NULL }
};

/* Module interface */

module auth_module = {
  NULL, NULL,

  /* Module API version */
  0x20,

  /* Module name */
  "auth",

  /* Module configuration directive table */
  auth_conftab,	

  /* Module command handler table */
  auth_cmdtab,

  /* Module authentication handler table */
  NULL,

  /* Module initialization function */
  auth_init,

  /* Session initialization function */
  auth_sess_init
};

