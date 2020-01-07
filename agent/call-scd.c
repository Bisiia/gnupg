/* call-scd.c - fork of the scdaemon to do SC operations
 * Copyright (C) 2001, 2002, 2005, 2007, 2010,
 *               2011 Free Software Foundation, Inc.
 * Copyright (C) 2013 Werner Koch
 *
 * This file is part of GnuPG.
 *
 * GnuPG is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * GnuPG is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, see <https://www.gnu.org/licenses/>.
 */

#include <config.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <unistd.h>
#ifdef HAVE_SIGNAL_H
# include <signal.h>
#endif
#include <sys/stat.h>
#include <sys/types.h>
#ifndef HAVE_W32_SYSTEM
#include <sys/wait.h>
#endif
#include <npth.h>

#include "agent.h"
#include <assuan.h>
#include "../common/strlist.h"

#ifdef _POSIX_OPEN_MAX
#define MAX_OPEN_FDS _POSIX_OPEN_MAX
#else
#define MAX_OPEN_FDS 20
#endif

/* Definition of module local data of the CTRL structure.  */
struct scd_local_s
{
  /* We keep a list of all allocated context with an anchor at
     SCD_LOCAL_LIST (see below). */
  struct scd_local_s *next_local;

  assuan_context_t ctx;   /* NULL or session context for the SCdaemon
                             used with this connection. */
  unsigned int in_use: 1; /* CTX is in use.  */
  unsigned int invalid:1; /* CTX is invalid, should be released.  */
};


/* Callback parameter for learn card */
struct learn_parm_s
{
  void (*kpinfo_cb)(void*, const char *);
  void *kpinfo_cb_arg;
  void (*certinfo_cb)(void*, const char *);
  void *certinfo_cb_arg;
  void (*sinfo_cb)(void*, const char *, size_t, const char *);
  void *sinfo_cb_arg;
};


/* Callback parameter used by inq_getpin and inq_writekey_parms.  */
struct inq_needpin_parm_s
{
  assuan_context_t ctx;
  int (*getpin_cb)(void *, const char *, const char *, char*, size_t);
  void *getpin_cb_arg;
  const char *getpin_cb_desc;
  assuan_context_t passthru;  /* If not NULL, pass unknown inquiries
                                 up to the caller.  */

  /* The next fields are used by inq_writekey_parm.  */
  const unsigned char *keydata;
  size_t keydatalen;
};


/* To keep track of all active SCD contexts, we keep a linked list
   anchored at this variable. */
static struct scd_local_s *scd_local_list;

/* A Mutex used inside the start_scd function. */
static npth_mutex_t start_scd_lock;

/* A malloced string with the name of the socket to be used for
   additional connections.  May be NULL if not provided by
   SCdaemon. */
static char *socket_name;

/* The context of the primary connection.  This is also used as a flag
   to indicate whether the scdaemon has been started. */
static assuan_context_t primary_scd_ctx;

/* To allow reuse of the primary connection, the following flag is set
   to true if the primary context has been reset and is not in use by
   any connection. */
static int primary_scd_ctx_reusable;



/* Local prototypes.  */




/* This function must be called once to initialize this module.  This
   has to be done before a second thread is spawned.  We can't do the
   static initialization because NPth emulation code might not be able
   to do a static init; in particular, it is not possible for W32. */
void
initialize_module_call_scd (void)
{
  static int initialized;
  int err;

  if (!initialized)
    {
      err = npth_mutex_init (&start_scd_lock, NULL);
      if (err)
        log_fatal ("error initializing mutex: %s\n", strerror (err));
      initialized = 1;
    }
}


/* This function may be called to print information pertaining to the
   current state of this module to the log. */
void
agent_scd_dump_state (void)
{
  log_info ("agent_scd_dump_state: primary_scd_ctx=%p pid=%ld reusable=%d\n",
            primary_scd_ctx,
            (long)assuan_get_pid (primary_scd_ctx),
            primary_scd_ctx_reusable);
  if (socket_name)
    log_info ("agent_scd_dump_state: socket='%s'\n", socket_name);
}


/* The unlock_scd function shall be called after having accessed the
   SCD.  It is currently not very useful but gives an opportunity to
   keep track of connections currently calling SCD.  Note that the
   "lock" operation is done by the start_scd() function which must be
   called and error checked before any SCD operation.  CTRL is the
   usual connection context and RC the error code to be passed trhough
   the function. */
static int
unlock_scd (ctrl_t ctrl, int rc)
{
  int err;

  if (ctrl->scd_local->in_use == 0)
    {
      log_error ("unlock_scd: CTX is not in use\n");
      if (!rc)
        rc = gpg_error (GPG_ERR_INTERNAL);
    }
  err = npth_mutex_lock (&start_scd_lock);
  if (err)
    {
      log_error ("failed to acquire the start_scd lock: %s\n", strerror (err));
      return gpg_error (GPG_ERR_INTERNAL);
    }
  ctrl->scd_local->in_use = 0;
  if (ctrl->scd_local->invalid)
    {
      assuan_release (ctrl->scd_local->ctx);
      ctrl->scd_local->ctx = NULL;
      ctrl->scd_local->invalid = 0;
    }
  err = npth_mutex_unlock (&start_scd_lock);
  if (err)
    {
      log_error ("failed to release the start_scd lock: %s\n", strerror (err));
      return gpg_error (GPG_ERR_INTERNAL);
    }
  return rc;
}

/* To make sure we leave no secrets in our image after forking of the
   scdaemon, we use this callback. */
static void
atfork_cb (void *opaque, int where)
{
  (void)opaque;

  if (!where)
    gcry_control (GCRYCTL_TERM_SECMEM);
}


static void *
wait_child_thread (void *arg)
{
  int err;
  struct scd_local_s *sl;

#ifdef HAVE_W32_SYSTEM
  HANDLE pid = (HANDLE)arg;

  npth_unprotect ();
  WaitForSingleObject ((HANDLE)pid, INFINITE);
  npth_protect ();
  log_info ("scdaemon finished\n");
#else
  int wstatus;
  pid_t pid = (pid_t)(uintptr_t)arg;

 again:
  npth_unprotect ();
  err = waitpid (pid, &wstatus, 0);
  npth_protect ();

  if (err < 0)
    {
      if (errno == EINTR)
        goto again;
      log_error ("waitpid failed: %s\n", strerror (errno));
      return NULL;
    }
  else
    {
      if (WIFEXITED (wstatus))
        log_info ("scdaemon finished (status %d)\n", WEXITSTATUS (wstatus));
      else if (WIFSIGNALED (wstatus))
        log_info ("scdaemon killed by signal %d\n", WTERMSIG (wstatus));
      else
        {
          if (WIFSTOPPED (wstatus))
            log_info ("scdaemon stopped by signal %d\n", WSTOPSIG (wstatus));
          goto again;
        }
    }
#endif

  agent_flush_cache (1);  /* Flush the PIN cache.  */

  err = npth_mutex_lock (&start_scd_lock);
  if (err)
    {
      log_error ("failed to acquire the start_scd lock: %s\n",
                 strerror (err));
    }
  else
    {
      assuan_set_flag (primary_scd_ctx, ASSUAN_NO_WAITPID, 1);

      for (sl = scd_local_list; sl; sl = sl->next_local)
        {
          sl->invalid = 1;
          if (!sl->in_use && sl->ctx)
            {
              assuan_release (sl->ctx);
              sl->ctx = NULL;
            }
        }

      primary_scd_ctx = NULL;
      primary_scd_ctx_reusable = 0;

      xfree (socket_name);
      socket_name = NULL;

      err = npth_mutex_unlock (&start_scd_lock);
      if (err)
        log_error ("failed to release the start_scd lock after waitpid: %s\n",
                   strerror (err));
    }

  return NULL;
}


/* Fork off the SCdaemon if this has not already been done.  Lock the
   daemon and make sure that a proper context has been setup in CTRL.
   This function might also lock the daemon, which means that the
   caller must call unlock_scd after this function has returned
   success and the actual Assuan transaction been done. */
static int
start_scd (ctrl_t ctrl)
{
  gpg_error_t err = 0;
  const char *pgmname;
  assuan_context_t ctx = NULL;
  const char *argv[5];
  assuan_fd_t no_close_list[3];
  int i;
  int rc;
  char *abs_homedir = NULL;

  if (opt.disable_scdaemon)
    return gpg_error (GPG_ERR_NOT_SUPPORTED);

  if (ctrl->scd_local && ctrl->scd_local->ctx)
    {
      ctrl->scd_local->in_use = 1;
      return 0; /* Okay, the context is fine.  */
    }

  if (ctrl->scd_local && ctrl->scd_local->in_use)
    {
      log_error ("start_scd: CTX is in use\n");
      return gpg_error (GPG_ERR_INTERNAL);
    }

  /* We need to serialize the access to scd_local_list and primary_scd_ctx. */
  rc = npth_mutex_lock (&start_scd_lock);
  if (rc)
    {
      log_error ("failed to acquire the start_scd lock: %s\n",
                 strerror (rc));
      return gpg_error (GPG_ERR_INTERNAL);
    }

  /* If this is the first call for this session, setup the local data
     structure. */
  if (!ctrl->scd_local)
    {
      ctrl->scd_local = xtrycalloc (1, sizeof *ctrl->scd_local);
      if (!ctrl->scd_local)
        {
          err = gpg_error_from_syserror ();
          rc = npth_mutex_unlock (&start_scd_lock);
          if (rc)
            log_error ("failed to release the start_scd lock: %s\n", strerror (rc));
          return err;
        }
      ctrl->scd_local->next_local = scd_local_list;
      scd_local_list = ctrl->scd_local;
    }

  ctrl->scd_local->in_use = 1;

  /* Check whether the pipe server has already been started and in
     this case either reuse a lingering pipe connection or establish a
     new socket based one. */
  if (primary_scd_ctx && primary_scd_ctx_reusable)
    {
      ctx = primary_scd_ctx;
      primary_scd_ctx_reusable = 0;
      if (opt.verbose)
        log_info ("new connection to SCdaemon established (reusing)\n");
      goto leave;
    }

  rc = assuan_new (&ctx);
  if (rc)
    {
      log_error ("can't allocate assuan context: %s\n", gpg_strerror (rc));
      err = rc;
      goto leave;
    }

  if (socket_name)
    {
      rc = assuan_socket_connect (ctx, socket_name, 0, 0);
      if (rc)
        {
          log_error ("can't connect to socket '%s': %s\n",
                     socket_name, gpg_strerror (rc));
          err = gpg_error (GPG_ERR_NO_SCDAEMON);
          goto leave;
        }

      if (opt.verbose)
        log_info ("new connection to SCdaemon established\n");
      goto leave;
    }

  if (primary_scd_ctx)
    {
      log_info ("SCdaemon is running but won't accept further connections\n");
      err = gpg_error (GPG_ERR_NO_SCDAEMON);
      goto leave;
    }

  /* Nope, it has not been started.  Fire it up now. */
  if (opt.verbose)
    log_info ("no running SCdaemon - starting it\n");

  agent_flush_cache (1);  /* Make sure the PIN cache is flushed.  */

  if (fflush (NULL))
    {
#ifndef HAVE_W32_SYSTEM
      err = gpg_error_from_syserror ();
#endif
      log_error ("error flushing pending output: %s\n", strerror (errno));
      /* At least Windows XP fails here with EBADF.  According to docs
         and Wine an fflush(NULL) is the same as _flushall.  However
         the Wime implementation does not flush stdin,stdout and stderr
         - see above.  Lets try to ignore the error. */
#ifndef HAVE_W32_SYSTEM
      goto leave;
#endif
    }

  if (!opt.scdaemon_program || !*opt.scdaemon_program)
    opt.scdaemon_program = gnupg_module_name (GNUPG_MODULE_NAME_SCDAEMON);
  if ( !(pgmname = strrchr (opt.scdaemon_program, '/')))
    pgmname = opt.scdaemon_program;
  else
    pgmname++;

  argv[0] = pgmname;
  argv[1] = "--multi-server";
  if (gnupg_default_homedir_p ())
    argv[2] = NULL;
  else
    {
      abs_homedir = make_absfilename_try (gnupg_homedir (), NULL);
      if (!abs_homedir)
        {
          log_error ("error building filename: %s\n",
                     gpg_strerror (gpg_error_from_syserror ()));
          goto leave;
        }

      argv[2] = "--homedir";
      argv[3] = abs_homedir;
      argv[4] = NULL;
    }

  i=0;
  if (!opt.running_detached)
    {
      if (log_get_fd () != -1)
        no_close_list[i++] = assuan_fd_from_posix_fd (log_get_fd ());
      no_close_list[i++] = assuan_fd_from_posix_fd (fileno (stderr));
    }
  no_close_list[i] = ASSUAN_INVALID_FD;

  /* Connect to the scdaemon and perform initial handshaking.  Use
     detached flag so that under Windows SCDAEMON does not show up a
     new window.  */
  rc = assuan_pipe_connect (ctx, opt.scdaemon_program, argv,
                            no_close_list, atfork_cb, NULL,
                            ASSUAN_PIPE_CONNECT_DETACHED);
  if (rc)
    {
      log_error ("can't connect to the SCdaemon: %s\n",
                 gpg_strerror (rc));
      err = gpg_error (GPG_ERR_NO_SCDAEMON);
      goto leave;
    }

  if (opt.verbose)
    log_debug ("first connection to SCdaemon established\n");


  /* Get the name of the additional socket opened by scdaemon. */
  {
    membuf_t data;
    unsigned char *databuf;
    size_t datalen;

    xfree (socket_name);
    socket_name = NULL;
    init_membuf (&data, 256);
    assuan_transact (ctx, "GETINFO socket_name",
                     put_membuf_cb, &data, NULL, NULL, NULL, NULL);

    databuf = get_membuf (&data, &datalen);
    if (databuf && datalen)
      {
        socket_name = xtrymalloc (datalen + 1);
        if (!socket_name)
          log_error ("warning: can't store socket name: %s\n",
                     strerror (errno));
        else
          {
            memcpy (socket_name, databuf, datalen);
            socket_name[datalen] = 0;
            if (DBG_IPC)
              log_debug ("additional connections at '%s'\n", socket_name);
          }
      }
    xfree (databuf);
  }

  /* Tell the scdaemon we want him to send us an event signal.  We
     don't support this for W32CE.  */
#ifndef HAVE_W32CE_SYSTEM
  if (opt.sigusr2_enabled)
    {
      char buf[100];

#ifdef HAVE_W32_SYSTEM
      snprintf (buf, sizeof buf, "OPTION event-signal=%p",
                get_agent_scd_notify_event ());
#else
      snprintf (buf, sizeof buf, "OPTION event-signal=%d", SIGUSR2);
#endif
      assuan_transact (ctx, buf, NULL, NULL, NULL, NULL, NULL, NULL);
    }
#endif /*HAVE_W32CE_SYSTEM*/

  primary_scd_ctx = ctx;
  primary_scd_ctx_reusable = 0;

  {
    npth_t thread;
    npth_attr_t tattr;
    pid_t pid;

    pid = assuan_get_pid (primary_scd_ctx);
    err = npth_attr_init (&tattr);
    if (!err)
      {
        npth_attr_setdetachstate (&tattr, NPTH_CREATE_DETACHED);
        err = npth_create (&thread, &tattr, wait_child_thread,
                           (void *)(uintptr_t)pid);
        if (err)
          log_error ("error spawning wait_child_thread: %s\n", strerror (err));
        npth_attr_destroy (&tattr);
      }
  }

 leave:
  rc = npth_mutex_unlock (&start_scd_lock);
  if (rc)
    log_error ("failed to release the start_scd lock: %s\n", strerror (rc));

  xfree (abs_homedir);
  if (err)
    {
      unlock_scd (ctrl, err);
      if (ctx)
        assuan_release (ctx);
    }
  else
    {
      ctrl->scd_local->invalid = 0;
      ctrl->scd_local->ctx = ctx;
    }
  return err;
}


/* Check whether the SCdaemon is active.  This is a fast check without
   any locking and might give a wrong result if another thread is about
   to start the daemon or the daemon is about to be stopped.. */
int
agent_scd_check_running (void)
{
  return !!primary_scd_ctx;
}


/* Reset the SCD if it has been used.  Actually it is not a reset but
   a cleanup of resources used by the current connection. */
int
agent_reset_scd (ctrl_t ctrl)
{
  int err = npth_mutex_lock (&start_scd_lock);

  if (err)
    {
      log_error ("failed to acquire the start_scd lock: %s\n",
                 strerror (err));
    }
  else
    {
      if (ctrl->scd_local)
        {
          if (ctrl->scd_local->ctx)
            {
              /* We send a reset and keep that connection for reuse. */
              if (ctrl->scd_local->ctx == primary_scd_ctx)
                {
                  /* Send a RESTART to the SCD.  This is required for the
                     primary connection as a kind of virtual EOF; we don't
                     have another way to tell it that the next command
                     should be viewed as if a new connection has been
                     made.  For the non-primary connections this is not
                     needed as we simply close the socket.  We don't check
                     for an error here because the RESTART may fail for
                     example if the scdaemon has already been terminated.
                     Anyway, we need to set the reusable flag to make sure
                     that the aliveness check can clean it up. */
                  assuan_transact (primary_scd_ctx, "RESTART",
                                   NULL, NULL, NULL, NULL, NULL, NULL);
                  primary_scd_ctx_reusable = 1;
                }
              else
                assuan_release (ctrl->scd_local->ctx);
              ctrl->scd_local->ctx = NULL;
            }

          /* Remove the local context from our list and release it. */
          if (!scd_local_list)
            BUG ();
          else if (scd_local_list == ctrl->scd_local)
            scd_local_list = ctrl->scd_local->next_local;
          else
            {
              struct scd_local_s *sl;

              for (sl=scd_local_list; sl->next_local; sl = sl->next_local)
                if (sl->next_local == ctrl->scd_local)
                  break;
              if (!sl->next_local)
                BUG ();
              sl->next_local = ctrl->scd_local->next_local;
            }
          xfree (ctrl->scd_local);
          ctrl->scd_local = NULL;
        }

      err = npth_mutex_unlock (&start_scd_lock);
      if (err)
        log_error ("failed to release the start_scd lock: %s\n", strerror (err));
    }

  return 0;
}



/* This handler is a helper for pincache_put_cb but may also be called
 * directly for that status code with ARGS being the arguments after
 * the status keyword (and with white space removed).  */
static gpg_error_t
handle_pincache_put (const char *args)
{
  gpg_error_t err;
  const char *s, *key, *hexwrappedpin;
  char *keybuf = NULL;
  unsigned char *wrappedpin = NULL;
  size_t keylen, hexwrappedpinlen, wrappedpinlen;
  char *value = NULL;
  size_t valuelen;
  gcry_cipher_hd_t cipherhd = NULL;

  key = s = args;
  while (*s && !spacep (s))
    s++;
  keylen = s - key;
  if (keylen < 3)
    {
      /* At least we need 2 slashes and slot number.  */
      log_error ("%s: ignoring invalid key\n", __func__);
      err = 0;
      goto leave;
    }


  keybuf = xtrymalloc (keylen+1);
  if (!keybuf)
    {
      err = gpg_error_from_syserror ();
      goto leave;
    }
  memcpy (keybuf, key, keylen);
  keybuf[keylen] = 0;
  key = keybuf;

  while (spacep (s))
    s++;
  hexwrappedpin = s;
  while (*s && !spacep (s))
    s++;
  hexwrappedpinlen = s - hexwrappedpin;
  if (!hexwrappedpinlen)
    {
      /* Flush the cache.  The cache module knows aboput the structure
       * of the key to flush only parts.  */
      log_debug ("%s: flushing cache '%s'\n", __func__, key);
      agent_put_cache (NULL, key, CACHE_MODE_PIN, NULL, -1);
      err = 0;
      goto leave;
    }

  if (hexwrappedpinlen < 2*24)
    {
      log_error ("%s: ignoring request with too short cryptogram\n", __func__);
      err = 0;
      goto leave;
    }
  wrappedpinlen = hexwrappedpinlen / 2;
  wrappedpin = xtrymalloc (wrappedpinlen);
  if (!wrappedpin)
    {
      err = gpg_error_from_syserror ();
      goto leave;
    }
  if (hex2bin (hexwrappedpin, wrappedpin, wrappedpinlen) == -1)
    {
      log_error ("%s: invalid hex length\n", __func__);
      err = gpg_error (GPG_ERR_INV_LENGTH);
      goto leave;
    }

  valuelen = wrappedpinlen - 8;
  value = xtrymalloc_secure (valuelen+1);
  if (!value)
    {
      err = gpg_error_from_syserror ();
      goto leave;
    }

  err = gcry_cipher_open (&cipherhd, GCRY_CIPHER_AES128,
                          GCRY_CIPHER_MODE_AESWRAP, 0);
  if (!err)
    err = gcry_cipher_setkey (cipherhd, "1234567890123456", 16);
  if (!err)
    err = gcry_cipher_decrypt (cipherhd, value, valuelen,
                               wrappedpin, wrappedpinlen);
  if (err)
    {
      log_error ("%s: error decrypting the cryptogram: %s\n",
                 __func__, gpg_strerror (err));
      goto leave;
    }

  log_debug ("%s: caching '%s'->'%s'\n", __func__, key, value);
  agent_put_cache (NULL, key, CACHE_MODE_PIN, value, -1);

 leave:
  xfree (keybuf);
  xfree (value);
  xfree (wrappedpin);
  gcry_cipher_close (cipherhd);
  return err;
}


/* This status callback is to intercept the PINCACHE_PUT status messages.  */
static gpg_error_t
pincache_put_cb (void *opaque, const char *line)
{
  const char *s;

  (void)opaque;

  s = has_leading_keyword (line, "PINCACHE_PUT");
  if (s)
    return handle_pincache_put (s);
  else
    return 0;
}



static gpg_error_t
learn_status_cb (void *opaque, const char *line)
{
  struct learn_parm_s *parm = opaque;
  gpg_error_t err = 0;
  const char *keyword = line;
  int keywordlen;

  for (keywordlen=0; *line && !spacep (line); line++, keywordlen++)
    ;
  while (spacep (line))
    line++;
  if (keywordlen == 8 && !memcmp (keyword, "CERTINFO", keywordlen))
    {
      parm->certinfo_cb (parm->certinfo_cb_arg, line);
    }
  else if (keywordlen == 11 && !memcmp (keyword, "KEYPAIRINFO", keywordlen))
    {
      parm->kpinfo_cb (parm->kpinfo_cb_arg, line);
    }
  else if (keywordlen == 12 && !memcmp (keyword, "PINCACHE_PUT", keywordlen))
    err = handle_pincache_put (line);
  else if (keywordlen && *line)
    {
      parm->sinfo_cb (parm->sinfo_cb_arg, keyword, keywordlen, line);
    }

  return err;
}

/* Perform the LEARN command and return a list of all private keys
   stored on the card. */
int
agent_card_learn (ctrl_t ctrl,
                  void (*kpinfo_cb)(void*, const char *),
                  void *kpinfo_cb_arg,
                  void (*certinfo_cb)(void*, const char *),
                  void *certinfo_cb_arg,
                  void (*sinfo_cb)(void*, const char *, size_t, const char *),
                  void *sinfo_cb_arg)
{
  int rc;
  struct learn_parm_s parm;

  rc = start_scd (ctrl);
  if (rc)
    return rc;

  memset (&parm, 0, sizeof parm);
  parm.kpinfo_cb = kpinfo_cb;
  parm.kpinfo_cb_arg = kpinfo_cb_arg;
  parm.certinfo_cb = certinfo_cb;
  parm.certinfo_cb_arg = certinfo_cb_arg;
  parm.sinfo_cb = sinfo_cb;
  parm.sinfo_cb_arg = sinfo_cb_arg;
  rc = assuan_transact (ctrl->scd_local->ctx, "LEARN --force",
                        NULL, NULL, NULL, NULL,
                        learn_status_cb, &parm);
  if (rc)
    return unlock_scd (ctrl, rc);

  return unlock_scd (ctrl, 0);
}



static gpg_error_t
get_serialno_cb (void *opaque, const char *line)
{
  gpg_error_t err = 0;
  char **serialno = opaque;
  const char *keyword = line;
  const char *s;
  int keywordlen, n;

  for (keywordlen=0; *line && !spacep (line); line++, keywordlen++)
    ;
  while (spacep (line))
    line++;

  if (keywordlen == 8 && !memcmp (keyword, "SERIALNO", keywordlen))
    {
      if (*serialno)
        return gpg_error (GPG_ERR_CONFLICT); /* Unexpected status line. */
      for (n=0,s=line; hexdigitp (s); s++, n++)
        ;
      if (!n || (n&1)|| !(spacep (s) || !*s) )
        return gpg_error (GPG_ERR_ASS_PARAMETER);
      *serialno = xtrymalloc (n+1);
      if (!*serialno)
        return out_of_core ();
      memcpy (*serialno, line, n);
      (*serialno)[n] = 0;
    }
  else if (keywordlen == 12 && !memcmp (keyword, "PINCACHE_PUT", keywordlen))
    err = handle_pincache_put (line);

  return err;
}

/* Return the serial number of the card or an appropriate error.  The
   serial number is returned as a hexstring. */
int
agent_card_serialno (ctrl_t ctrl, char **r_serialno, const char *demand)
{
  int rc;
  char *serialno = NULL;
  char line[ASSUAN_LINELENGTH];

  rc = start_scd (ctrl);
  if (rc)
    return rc;

  if (!demand)
    strcpy (line, "SERIALNO");
  else
    snprintf (line, DIM(line), "SERIALNO --demand=%s", demand);

  rc = assuan_transact (ctrl->scd_local->ctx, line,
                        NULL, NULL, NULL, NULL,
                        get_serialno_cb, &serialno);
  if (rc)
    {
      xfree (serialno);
      return unlock_scd (ctrl, rc);
    }
  *r_serialno = serialno;
  return unlock_scd (ctrl, 0);
}




/* Handle the NEEDPIN inquiry. */
static gpg_error_t
inq_needpin (void *opaque, const char *line)
{
  struct inq_needpin_parm_s *parm = opaque;
  const char *s;
  char *pin;
  size_t pinlen;
  int rc;

  if ((s = has_leading_keyword (line, "NEEDPIN")))
    {
      line = s;
      pinlen = 90;
      pin = gcry_malloc_secure (pinlen);
      if (!pin)
        return out_of_core ();

      rc = parm->getpin_cb (parm->getpin_cb_arg, parm->getpin_cb_desc,
                            line, pin, pinlen);
      if (!rc)
        rc = assuan_send_data (parm->ctx, pin, pinlen);
      xfree (pin);
    }
  else if ((s = has_leading_keyword (line, "POPUPPINPADPROMPT")))
    {
      rc = parm->getpin_cb (parm->getpin_cb_arg, parm->getpin_cb_desc,
                            s, NULL, 1);
    }
  else if ((s = has_leading_keyword (line, "DISMISSPINPADPROMPT")))
    {
      rc = parm->getpin_cb (parm->getpin_cb_arg, parm->getpin_cb_desc,
                            "", NULL, 0);
    }
  else if ((s = has_leading_keyword (line, "PINCACHE_GET")))
    {
      /* rc = parm->getpin_cb (parm->getpin_cb_arg, parm->getpin_cb_desc, */
      /*                       "", NULL, 0); */
      rc = 0;
    }
  else if (parm->passthru)
    {
      unsigned char *value;
      size_t valuelen;
      int rest;
      int needrest = !strncmp (line, "KEYDATA", 8);

      /* Pass the inquiry up to our caller.  We limit the maximum
         amount to an arbitrary value.  As we know that the KEYDATA
         enquiry is pretty sensitive we disable logging then */
      if ((rest = (needrest
                   && !assuan_get_flag (parm->passthru, ASSUAN_CONFIDENTIAL))))
        assuan_begin_confidential (parm->passthru);
      rc = assuan_inquire (parm->passthru, line, &value, &valuelen, 8096);
      if (rest)
        assuan_end_confidential (parm->passthru);
      if (!rc)
        {
          if ((rest = (needrest
                       && !assuan_get_flag (parm->ctx, ASSUAN_CONFIDENTIAL))))
            assuan_begin_confidential (parm->ctx);
          rc = assuan_send_data (parm->ctx, value, valuelen);
          if (rest)
            assuan_end_confidential (parm->ctx);
          xfree (value);
        }
      else
        log_error ("error forwarding inquiry '%s': %s\n",
                   line, gpg_strerror (rc));
    }
  else
    {
      log_error ("unsupported inquiry '%s'\n", line);
      rc = gpg_error (GPG_ERR_ASS_UNKNOWN_INQUIRE);
    }

  return rc;
}


/* Helper returning a command option to describe the used hash
   algorithm.  See scd/command.c:cmd_pksign.  */
static const char *
hash_algo_option (int algo)
{
  switch (algo)
    {
    case GCRY_MD_MD5   : return "--hash=md5";
    case GCRY_MD_RMD160: return "--hash=rmd160";
    case GCRY_MD_SHA1  : return "--hash=sha1";
    case GCRY_MD_SHA224: return "--hash=sha224";
    case GCRY_MD_SHA256: return "--hash=sha256";
    case GCRY_MD_SHA384: return "--hash=sha384";
    case GCRY_MD_SHA512: return "--hash=sha512";
    default:             return "";
    }
}


/* Create a signature using the current card.  MDALGO is either 0 or
 * gives the digest algorithm.  DESC_TEXT is an additional parameter
 * passed to GETPIN_CB. */
int
agent_card_pksign (ctrl_t ctrl,
                   const char *keyid,
                   int (*getpin_cb)(void *, const char *,
                                    const char *, char*, size_t),
                   void *getpin_cb_arg,
                   const char *desc_text,
                   int mdalgo,
                   const unsigned char *indata, size_t indatalen,
                   unsigned char **r_buf, size_t *r_buflen)
{
  int rc;
  char line[ASSUAN_LINELENGTH];
  membuf_t data;
  struct inq_needpin_parm_s inqparm;

  *r_buf = NULL;
  rc = start_scd (ctrl);
  if (rc)
    return rc;

  if (indatalen*2 + 50 > DIM(line))
    return unlock_scd (ctrl, gpg_error (GPG_ERR_GENERAL));

  bin2hex (indata, indatalen, stpcpy (line, "SETDATA "));

  rc = assuan_transact (ctrl->scd_local->ctx, line,
                        NULL, NULL, NULL, NULL, pincache_put_cb, NULL);
  if (rc)
    return unlock_scd (ctrl, rc);

  init_membuf (&data, 1024);
  inqparm.ctx = ctrl->scd_local->ctx;
  inqparm.getpin_cb = getpin_cb;
  inqparm.getpin_cb_arg = getpin_cb_arg;
  inqparm.getpin_cb_desc = desc_text;
  inqparm.passthru = 0;
  inqparm.keydata = NULL;
  inqparm.keydatalen = 0;

  if (ctrl->use_auth_call)
    snprintf (line, sizeof line, "PKAUTH %s", keyid);
  else
    snprintf (line, sizeof line, "PKSIGN %s %s",
              hash_algo_option (mdalgo), keyid);
  rc = assuan_transact (ctrl->scd_local->ctx, line,
                        put_membuf_cb, &data,
                        inq_needpin, &inqparm,
                        pincache_put_cb, NULL);

  if (rc)
    {
      size_t len;

      xfree (get_membuf (&data, &len));
      return unlock_scd (ctrl, rc);
    }

  *r_buf = get_membuf (&data, r_buflen);
  return unlock_scd (ctrl, 0);
}




/* Check whether there is any padding info from scdaemon.  */
static gpg_error_t
padding_info_cb (void *opaque, const char *line)
{
  gpg_error_t err = 0;
  int *r_padding = opaque;
  const char *s;

  if ((s=has_leading_keyword (line, "PADDING")))
    {
      *r_padding = atoi (s);
    }
  else if ((s=has_leading_keyword (line, "PINCACHE_PUT")))
    err = handle_pincache_put (line);

  return err;
}


/* Decipher INDATA using the current card.  Note that the returned
 * value is not an s-expression but the raw data as returned by
 * scdaemon.  The padding information is stored at R_PADDING with -1
 * for not known.  DESC_TEXT is an additional parameter passed to
 * GETPIN_CB.  */
int
agent_card_pkdecrypt (ctrl_t ctrl,
                      const char *keyid,
                      int (*getpin_cb)(void *, const char *,
                                       const char *, char*, size_t),
                      void *getpin_cb_arg,
                      const char *desc_text,
                      const unsigned char *indata, size_t indatalen,
                      char **r_buf, size_t *r_buflen, int *r_padding)
{
  int rc, i;
  char *p, line[ASSUAN_LINELENGTH];
  membuf_t data;
  struct inq_needpin_parm_s inqparm;
  size_t len;

  *r_buf = NULL;
  *r_padding = -1; /* Unknown.  */
  rc = start_scd (ctrl);
  if (rc)
    return rc;

  /* FIXME: use secure memory where appropriate */

  for (len = 0; len < indatalen;)
    {
      p = stpcpy (line, "SETDATA ");
      if (len)
        p = stpcpy (p, "--append ");
      for (i=0; len < indatalen && (i*2 < DIM(line)-50); i++, len++)
        {
          sprintf (p, "%02X", indata[len]);
          p += 2;
        }
      rc = assuan_transact (ctrl->scd_local->ctx, line,
                            NULL, NULL, NULL, NULL, NULL, NULL);
      if (rc)
        return unlock_scd (ctrl, rc);
    }

  init_membuf (&data, 1024);
  inqparm.ctx = ctrl->scd_local->ctx;
  inqparm.getpin_cb = getpin_cb;
  inqparm.getpin_cb_arg = getpin_cb_arg;
  inqparm.getpin_cb_desc = desc_text;
  inqparm.passthru = 0;
  inqparm.keydata = NULL;
  inqparm.keydatalen = 0;
  snprintf (line, DIM(line), "PKDECRYPT %s", keyid);
  rc = assuan_transact (ctrl->scd_local->ctx, line,
                        put_membuf_cb, &data,
                        inq_needpin, &inqparm,
                        padding_info_cb, r_padding);

  if (rc)
    {
      xfree (get_membuf (&data, &len));
      return unlock_scd (ctrl, rc);
    }
  *r_buf = get_membuf (&data, r_buflen);
  if (!*r_buf)
    return unlock_scd (ctrl, gpg_error (GPG_ERR_ENOMEM));

  return unlock_scd (ctrl, 0);
}



/* Read a certificate with ID into R_BUF and R_BUFLEN. */
int
agent_card_readcert (ctrl_t ctrl,
                     const char *id, char **r_buf, size_t *r_buflen)
{
  int rc;
  char line[ASSUAN_LINELENGTH];
  membuf_t data;
  size_t len;

  *r_buf = NULL;
  rc = start_scd (ctrl);
  if (rc)
    return rc;

  init_membuf (&data, 1024);
  snprintf (line, DIM(line), "READCERT %s", id);
  rc = assuan_transact (ctrl->scd_local->ctx, line,
                        put_membuf_cb, &data,
                        NULL, NULL,
                        pincache_put_cb, NULL);
  if (rc)
    {
      xfree (get_membuf (&data, &len));
      return unlock_scd (ctrl, rc);
    }
  *r_buf = get_membuf (&data, r_buflen);
  if (!*r_buf)
    return unlock_scd (ctrl, gpg_error (GPG_ERR_ENOMEM));

  return unlock_scd (ctrl, 0);
}



/* Read a key with ID and return it in an allocate buffer pointed to
   by r_BUF as a valid S-expression. */
int
agent_card_readkey (ctrl_t ctrl, const char *id, unsigned char **r_buf)
{
  int rc;
  char line[ASSUAN_LINELENGTH];
  membuf_t data;
  size_t len, buflen;

  *r_buf = NULL;
  rc = start_scd (ctrl);
  if (rc)
    return rc;

  init_membuf (&data, 1024);
  snprintf (line, DIM(line), "READKEY %s", id);
  rc = assuan_transact (ctrl->scd_local->ctx, line,
                        put_membuf_cb, &data,
                        NULL, NULL,
                        pincache_put_cb, NULL);
  if (rc)
    {
      xfree (get_membuf (&data, &len));
      return unlock_scd (ctrl, rc);
    }
  *r_buf = get_membuf (&data, &buflen);
  if (!*r_buf)
    return unlock_scd (ctrl, gpg_error (GPG_ERR_ENOMEM));

  if (!gcry_sexp_canon_len (*r_buf, buflen, NULL, NULL))
    {
      xfree (*r_buf); *r_buf = NULL;
      return unlock_scd (ctrl, gpg_error (GPG_ERR_INV_VALUE));
    }

  return unlock_scd (ctrl, 0);
}


/* Handle a KEYDATA inquiry.  Note, we only send the data,
   assuan_transact takes care of flushing and writing the end */
static gpg_error_t
inq_writekey_parms (void *opaque, const char *line)
{
  struct inq_needpin_parm_s *parm = opaque;

  if (has_leading_keyword (line, "KEYDATA"))
    return assuan_send_data (parm->ctx, parm->keydata, parm->keydatalen);
  else
    return inq_needpin (opaque, line);
}


/* Call scd to write a key to a card under the id KEYREF.  */
gpg_error_t
agent_card_writekey (ctrl_t ctrl,  int force, const char *serialno,
                     const char *keyref,
                     const char *keydata, size_t keydatalen,
                     int (*getpin_cb)(void *, const char *,
                                      const char *, char*, size_t),
                     void *getpin_cb_arg)
{
  gpg_error_t err;
  char line[ASSUAN_LINELENGTH];
  struct inq_needpin_parm_s parms;

  (void)serialno; /* NULL or a number to check for the correct card.
                   * But is is not implemented.  */

  err = start_scd (ctrl);
  if (err)
    return err;

  snprintf (line, DIM(line), "WRITEKEY %s%s", force ? "--force " : "", keyref);
  parms.ctx = ctrl->scd_local->ctx;
  parms.getpin_cb = getpin_cb;
  parms.getpin_cb_arg = getpin_cb_arg;
  parms.getpin_cb_desc= NULL;
  parms.passthru = 0;
  parms.keydata = keydata;
  parms.keydatalen = keydatalen;

  err = assuan_transact (ctrl->scd_local->ctx, line, NULL, NULL,
                         inq_writekey_parms, &parms,
                         pincache_put_cb, NULL);
  return unlock_scd (ctrl, err);
}



/* Type used with the card_getattr_cb.  */
struct card_getattr_parm_s {
  const char *keyword;  /* Keyword to look for.  */
  size_t keywordlen;    /* strlen of KEYWORD.  */
  char *data;           /* Malloced and unescaped data.  */
  int error;            /* ERRNO value or 0 on success. */
};

/* Callback function for agent_card_getattr.  */
static gpg_error_t
card_getattr_cb (void *opaque, const char *line)
{
  gpg_error_t err = 0;
  struct card_getattr_parm_s *parm = opaque;
  const char *keyword = line;
  int keywordlen;

  if (parm->data)
    return 0; /* We want only the first occurrence.  */

  for (keywordlen=0; *line && !spacep (line); line++, keywordlen++)
    ;
  while (spacep (line))
    line++;

  if (keywordlen == parm->keywordlen
      && !memcmp (keyword, parm->keyword, keywordlen))
    {
      parm->data = percent_plus_unescape ((const unsigned char*)line, 0xff);
      if (!parm->data)
        parm->error = errno;
    }
  else if (keywordlen == 12 && !memcmp (keyword, "PINCACHE_PUT", keywordlen))
    err = handle_pincache_put (line);

  return err;
}


/* Call the agent to retrieve a single line data object. On success
   the object is malloced and stored at RESULT; it is guaranteed that
   NULL is never stored in this case.  On error an error code is
   returned and NULL stored at RESULT. */
gpg_error_t
agent_card_getattr (ctrl_t ctrl, const char *name, char **result)
{
  int err;
  struct card_getattr_parm_s parm;
  char line[ASSUAN_LINELENGTH];

  *result = NULL;

  if (!*name)
    return gpg_error (GPG_ERR_INV_VALUE);

  memset (&parm, 0, sizeof parm);
  parm.keyword = name;
  parm.keywordlen = strlen (name);

  /* We assume that NAME does not need escaping. */
  if (8 + strlen (name) > DIM(line)-1)
    return gpg_error (GPG_ERR_TOO_LARGE);
  stpcpy (stpcpy (line, "GETATTR "), name);

  err = start_scd (ctrl);
  if (err)
    return err;

  err = assuan_transact (ctrl->scd_local->ctx, line,
                         NULL, NULL, NULL, NULL,
                         card_getattr_cb, &parm);
  if (!err && parm.error)
    err = gpg_error_from_errno (parm.error);

  if (!err && !parm.data)
    err = gpg_error (GPG_ERR_NO_DATA);

  if (!err)
    *result = parm.data;
  else
    xfree (parm.data);

  return unlock_scd (ctrl, err);
}



struct card_cardlist_parm_s {
  int error;
  strlist_t list;
};

/* Callback function for agent_card_cardlist.  */
static gpg_error_t
card_cardlist_cb (void *opaque, const char *line)
{
  gpg_error_t err = 0;
  struct card_cardlist_parm_s *parm = opaque;
  const char *keyword = line;
  int keywordlen;

  for (keywordlen=0; *line && !spacep (line); line++, keywordlen++)
    ;
  while (spacep (line))
    line++;

  if (keywordlen == 8 && !memcmp (keyword, "SERIALNO", keywordlen))
    {
      const char *s;
      int n;

      for (n=0,s=line; hexdigitp (s); s++, n++)
        ;

      if (!n || (n&1) || *s)
        parm->error = gpg_error (GPG_ERR_ASS_PARAMETER);
      else
        add_to_strlist (&parm->list, line);
    }
  else if (keywordlen == 12 && !memcmp (keyword, "PINCACHE_PUT", keywordlen))
    err = handle_pincache_put (line);

  return err;
}

/* Call the scdaemon to retrieve list of available cards. On success
   the allocated strlist is stored at RESULT.  On error an error code is
   returned and NULL stored at RESULT. */
gpg_error_t
agent_card_cardlist (ctrl_t ctrl, strlist_t *result)
{
  int err;
  struct card_cardlist_parm_s parm;
  char line[ASSUAN_LINELENGTH];

  *result = NULL;

  memset (&parm, 0, sizeof parm);
  strcpy (line, "GETINFO card_list");

  err = start_scd (ctrl);
  if (err)
    return err;

  err = assuan_transact (ctrl->scd_local->ctx, line,
                         NULL, NULL, NULL, NULL,
                         card_cardlist_cb, &parm);
  if (!err && parm.error)
    err = parm.error;

  if (!err)
    *result = parm.list;
  else
    free_strlist (parm.list);

  return unlock_scd (ctrl, err);
}


struct card_keyinfo_parm_s {
  int error;
  struct card_key_info_s *list;
};

/* Callback function for agent_card_keylist.  */
static gpg_error_t
card_keyinfo_cb (void *opaque, const char *line)
{
  gpg_error_t err = 0;
  struct card_keyinfo_parm_s *parm = opaque;
  const char *keyword = line;
  int keywordlen;

  for (keywordlen=0; *line && !spacep (line); line++, keywordlen++)
    ;
  while (spacep (line))
    line++;

  if (keywordlen == 7 && !memcmp (keyword, "KEYINFO", keywordlen))
    {
      const char *s;
      int n;
      struct card_key_info_s *keyinfo;
      struct card_key_info_s **l_p = &parm->list;

      while ((*l_p))
        l_p = &(*l_p)->next;

      keyinfo = xtrycalloc (1, sizeof *keyinfo);
      if (!keyinfo)
        {
        alloc_error:
          if (!parm->error)
            parm->error = gpg_error_from_syserror ();
          return 0;
        }

      for (n=0,s=line; hexdigitp (s); s++, n++)
        ;

      if (n != 40)
        {
        parm_error:
          if (!parm->error)
            parm->error = gpg_error (GPG_ERR_ASS_PARAMETER);
          return 0;
        }

      memcpy (keyinfo->keygrip, line, 40);

      line = s;

      if (!*line)
        goto parm_error;

      while (spacep (line))
        line++;

      if (*line++ != 'T')
        goto parm_error;

      if (!*line)
        goto parm_error;

      while (spacep (line))
        line++;

      for (n=0,s=line; hexdigitp (s); s++, n++)
        ;

      if (!n)
        goto parm_error;

      keyinfo->serialno = xtrymalloc (n+1);
      if (!keyinfo->serialno)
        goto alloc_error;

      memcpy (keyinfo->serialno, line, n);
      keyinfo->serialno[n] = 0;

      line = s;

      if (!*line)
        goto parm_error;

      while (spacep (line))
        line++;

      if (!*line)
        goto parm_error;

      keyinfo->idstr = xtrystrdup (line);
      if (!keyinfo->idstr)
        goto alloc_error;

      *l_p = keyinfo;
    }
  else if (keywordlen == 12 && !memcmp (keyword, "PINCACHE_PUT", keywordlen))
    err = handle_pincache_put (line);

  return err;
}


void
agent_card_free_keyinfo (struct card_key_info_s *l)
{
  struct card_key_info_s *l_next;

  for (; l; l = l_next)
    {
      l_next = l->next;
      free (l->serialno);
      free (l->idstr);
      free (l);
    }
}

/* Call the scdaemon to check if a key of KEYGRIP is available, or
   retrieve list of available keys on cards. On success the allocated
   structure is stored at RESULT.  On error an error code is returned
   and NULL is stored at RESULT.  */
gpg_error_t
agent_card_keyinfo (ctrl_t ctrl, const char *keygrip,
                    struct card_key_info_s **result)
{
  int err;
  struct card_keyinfo_parm_s parm;
  char line[ASSUAN_LINELENGTH];

  *result = NULL;

  memset (&parm, 0, sizeof parm);
  snprintf (line, sizeof line, "KEYINFO %s", keygrip ? keygrip : "--list");

  err = start_scd (ctrl);
  if (err)
    return err;

  err = assuan_transact (ctrl->scd_local->ctx, line,
                         NULL, NULL, NULL, NULL,
                         card_keyinfo_cb, &parm);
  if (!err && parm.error)
    err = parm.error;

  if (!err)
    *result = parm.list;
  else
    agent_card_free_keyinfo (parm.list);

  return unlock_scd (ctrl, err);
}

static gpg_error_t
pass_status_thru (void *opaque, const char *line)
{
  gpg_error_t err = 0;
  assuan_context_t ctx = opaque;
  char keyword[200];
  int i;

  if (line[0] == '#' && (!line[1] || spacep (line+1)))
    {
      /* We are called in convey comments mode.  Now, if we see a
         comment marker as keyword we forward the line verbatim to the
         the caller.  This way the comment lines from scdaemon won't
         appear as status lines with keyword '#'.  */
      assuan_write_line (ctx, line);
    }
  else
    {
      for (i=0; *line && !spacep (line) && i < DIM(keyword)-1; line++, i++)
        keyword[i] = *line;
      keyword[i] = 0;

      /* Truncate any remaining keyword stuff.  */
      for (; *line && !spacep (line); line++)
        ;
      while (spacep (line))
        line++;

      /* We do not want to pass PINCACHE_PUT through.  */
      if (!strcmp (keyword, "PINCACHE_PUT"))
        err = handle_pincache_put (line);
      else
        assuan_write_status (ctx, keyword, line);
    }
  return err;
}

static gpg_error_t
pass_data_thru (void *opaque, const void *buffer, size_t length)
{
  assuan_context_t ctx = opaque;

  assuan_send_data (ctx, buffer, length);
  return 0;
}


/* Send the line CMDLINE with command for the SCDdaemon to it and send
   all status messages back.  This command is used as a general quoting
   mechanism to pass everything verbatim to SCDAEMON.  The PIN
   inquiry is handled inside gpg-agent.  */
int
agent_card_scd (ctrl_t ctrl, const char *cmdline,
                int (*getpin_cb)(void *, const char *,
                                 const char *, char*, size_t),
                void *getpin_cb_arg, void *assuan_context)
{
  int rc;
  struct inq_needpin_parm_s inqparm;
  int saveflag;

  rc = start_scd (ctrl);
  if (rc)
    return rc;

  inqparm.ctx = ctrl->scd_local->ctx;
  inqparm.getpin_cb = getpin_cb;
  inqparm.getpin_cb_arg = getpin_cb_arg;
  inqparm.getpin_cb_desc = NULL;
  inqparm.passthru = assuan_context;
  inqparm.keydata = NULL;
  inqparm.keydatalen = 0;

  saveflag = assuan_get_flag (ctrl->scd_local->ctx, ASSUAN_CONVEY_COMMENTS);
  assuan_set_flag (ctrl->scd_local->ctx, ASSUAN_CONVEY_COMMENTS, 1);
  rc = assuan_transact (ctrl->scd_local->ctx, cmdline,
                        pass_data_thru, assuan_context,
                        inq_needpin, &inqparm,
                        pass_status_thru, assuan_context);

  assuan_set_flag (ctrl->scd_local->ctx, ASSUAN_CONVEY_COMMENTS, saveflag);
  if (rc)
    {
      return unlock_scd (ctrl, rc);
    }

  return unlock_scd (ctrl, 0);
}

void
agent_card_killscd (void)
{
  if (primary_scd_ctx == NULL)
    return;
  assuan_transact (primary_scd_ctx, "KILLSCD",
                   NULL, NULL, NULL, NULL, NULL, NULL);
  agent_flush_cache (1);  /* Flush the PIN cache.  */
}
