/* keybox-update.c - keybox update operations
 *	Copyright (C) 2001, 2003 Free Software Foundation, Inc.
 *
 * This file is part of GnuPG.
 *
 * GnuPG is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * GnuPG is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA
 */

#include <config.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>

#include "keybox-defs.h"

#define EXTSEP_S "."


static int
create_tmp_file (const char *template,
                 char **r_bakfname, char **r_tmpfname, FILE **r_fp)
{  
  char *bakfname, *tmpfname;
  
  *r_bakfname = NULL;
  *r_tmpfname = NULL;
  
# ifdef USE_ONLY_8DOT3
  /* Here is another Windoze bug?:
   * you cant rename("pubring.kbx.tmp", "pubring.kbx");
   * but	rename("pubring.kbx.tmp", "pubring.aaa");
   * works.  So we replace .kbx by .bak or .tmp
   */
  if (strlen (template) > 4
      && !strcmp (template+strlen(template)-4, EXTSEP_S "kbx") )
    {
      bakfname = xtrymalloc (strlen (template) + 1);
      if (!bakfname)
        return gpg_error (gpg_err_code_from_errno (errno));
      strcpy (bakfname, template);
      strcpy (bakfname+strlen(template)-4, EXTSEP_S "bak");
      
      tmpfname = xtrymalloc (strlen (template) + 1);
      if (!tmpfname)
        {
          gpg_error_t tmperr = gpg_error (gpg_err_code_from_errno (errno));
          xfree (bakfname);
          return tmperr;
        }
      strcpy (tmpfname,template);
      strcpy (tmpfname + strlen (template)-4, EXTSEP_S "tmp");
    }
  else 
    { /* file does not end with kbx; hmmm */
      bakfname = xtrymalloc ( strlen (template) + 5);
      if (!bakfname)
        return gpg_error (gpg_err_code_from_errno (errno));
      strcpy (stpcpy (bakfname, template), EXTSEP_S "bak");
      
      tmpfname = xtrymalloc ( strlen (template) + 5);
      if (!tmpfname)
        {
          gpg_error_t tmperr = gpg_error (gpg_err_code_from_errno (errno));
          xfree (bakfname);
          return tmperr;
        }
      strcpy (stpcpy (tmpfname, template), EXTSEP_S "tmp");
    }
# else /* Posix file names */
  bakfname = xtrymalloc (strlen (template) + 2);
  if (!bakfname)
    return gpg_error (gpg_err_code_from_errno (errno));
  strcpy (stpcpy (bakfname,template),"~");
  
  tmpfname = xtrymalloc ( strlen (template) + 5);
  if (!tmpfname)
    {
      gpg_error_t tmperr = gpg_error (gpg_err_code_from_errno (errno));
      xfree (bakfname);
      return tmperr;
    }
  strcpy (stpcpy (tmpfname,template), EXTSEP_S "tmp");
# endif /* Posix filename */

  *r_fp = fopen (tmpfname, "wb");
  if (!*r_fp)
    {
      gpg_error_t tmperr = gpg_error (gpg_err_code_from_errno (errno));
      xfree (tmpfname);
      xfree (bakfname);
      return tmperr;
    }
  
  *r_bakfname = bakfname;
  *r_tmpfname = tmpfname;
  return 0;
}


static int
rename_tmp_file (const char *bakfname, const char *tmpfname,
                 const char *fname, int secret )
{
  int rc=0;

  /* restrict the permissions for secret keyboxs */
#ifndef HAVE_DOSISH_SYSTEM
/*    if (secret && !opt.preserve_permissions) */
/*      { */
/*        if (chmod (tmpfname, S_IRUSR | S_IWUSR) )  */
/*          { */
/*            log_debug ("chmod of `%s' failed: %s\n", */
/*                       tmpfname, strerror(errno) ); */
/*            return KEYBOX_Write_File; */
/*  	} */
/*      } */
#endif

  /* fixme: invalidate close caches (not used with stdio)*/
/*    iobuf_ioctl (NULL, 2, 0, (char*)tmpfname ); */
/*    iobuf_ioctl (NULL, 2, 0, (char*)bakfname ); */
/*    iobuf_ioctl (NULL, 2, 0, (char*)fname ); */

  /* first make a backup file except for secret keyboxs */
  if (!secret)
    { 
#if defined(HAVE_DOSISH_SYSTEM) || defined(__riscos__)
      remove (bakfname);
#endif
      if (rename (fname, bakfname) )
        {
          return gpg_error (gpg_err_code_from_errno (errno));
	}
    }
  
  /* then rename the file */
#if defined(HAVE_DOSISH_SYSTEM) || defined(__riscos__)
  remove (fname);
#endif
  if (rename (tmpfname, fname) )
    {
      rc = gpg_error (gpg_err_code_from_errno (errno));
      if (secret)
        {
/*            log_info ("WARNING: 2 files with confidential" */
/*                       " information exists.\n"); */
/*            log_info ("%s is the unchanged one\n", fname ); */
/*            log_info ("%s is the new one\n", tmpfname ); */
/*            log_info ("Please fix this possible security flaw\n"); */
	}
      return rc;
    }
  
  return 0;
}



/* Perform insert/delete/update operation.
    mode 1 = insert
 	 2 = delete
 	 3 = update
*/
static int
blob_filecopy (int mode, const char *fname, KEYBOXBLOB blob, 
               int secret, off_t start_offset, unsigned int n_packets )
{
  FILE *fp, *newfp;
  int rc=0;
  char *bakfname = NULL;
  char *tmpfname = NULL;
  char buffer[4096];
  int nread, nbytes;

  /* Open the source file. Because we do a rename, we have to check the 
     permissions of the file */
  if (access (fname, W_OK))
    return gpg_error (gpg_err_code_from_errno (errno));

  fp = fopen (fname, "rb");
  if (mode == 1 && !fp && errno == ENOENT)
    { /* insert mode but file does not exist: create a new keybox file */
      newfp = fopen (fname, "wb");
      if (!newfp )
        {
          return gpg_error (gpg_err_code_from_errno (errno));
	}

      rc = _keybox_write_blob (blob, newfp);
      if (rc)
        {
          return rc;
        }
      if ( fclose (newfp) )
        {
          return gpg_error (gpg_err_code_from_errno (errno));
	}

/*        if (chmod( fname, S_IRUSR | S_IWUSR )) */
/*          { */
/*            log_debug ("%s: chmod failed: %s\n", fname, strerror(errno) ); */
/*            return KEYBOX_File_Error; */
/*          } */
      return 0; /* ready */
    }

  if (!fp)
    {
      rc = gpg_error (gpg_err_code_from_errno (errno));
      goto leave;
    }

  /* create the new file */
  rc = create_tmp_file (fname, &bakfname, &tmpfname, &newfp);
  if (rc)
    {
      fclose(fp);
      goto leave;
    }
  
  /* prepare for insert */
  if (mode == 1)
    { 
      /* copy everything to the new file */
      while ( (nread = fread (buffer, 1, DIM(buffer), fp)) > 0 )
        {
          if (fwrite (buffer, nread, 1, newfp) != 1)
            {
              rc = gpg_error (gpg_err_code_from_errno (errno));
              goto leave;
            }
        }
      if (ferror (fp))
        {
          rc = gpg_error (gpg_err_code_from_errno (errno));
          goto leave;
        }
    }
  
  /* prepare for delete or update */
  if ( mode == 2 || mode == 3 ) 
    { 
      off_t current = 0;
      
      /* copy first part to the new file */
      while ( current < start_offset )
        {
          nbytes = DIM(buffer);
          if (current + nbytes > start_offset)
              nbytes = start_offset - current;
          nread = fread (buffer, 1, nbytes, fp);
          if (!fread)
            break;
          current += nread;
          
          if (fwrite (buffer, nread, 1, newfp) != 1)
            {
              rc = gpg_error (gpg_err_code_from_errno (errno));
              goto leave;
            }
        }
      if (ferror (fp))
        {
          rc = gpg_error (gpg_err_code_from_errno (errno));
          goto leave;
        }
      
      /* skip this blob */
      rc = _keybox_read_blob (NULL, fp);
      if (rc)
        return rc;
    }
  
  /* Do an insert or update */
  if ( mode == 1 || mode == 3 )
    { 
      rc = _keybox_write_blob (blob, newfp);
      if (rc)
          return rc;
    }
  
  /* copy the rest of the packet for an delete or update */
  if (mode == 2 || mode == 3)
    { 
      while ( (nread = fread (buffer, 1, DIM(buffer), fp)) > 0 )
        {
          if (fwrite (buffer, nread, 1, newfp) != 1)
            {
              rc = gpg_error (gpg_err_code_from_errno (errno));
              goto leave;
            }
        }
      if (ferror (fp))
        {
          rc = gpg_error (gpg_err_code_from_errno (errno));
          goto leave;
        }
    }
    
  /* close both files */
  if (fclose(fp))
    {
      rc = gpg_error (gpg_err_code_from_errno (errno));
      fclose (newfp);
      goto leave;
    }
  if (fclose(newfp))
    {
      rc = gpg_error (gpg_err_code_from_errno (errno));
      goto leave;
    }

  rc = rename_tmp_file (bakfname, tmpfname, fname, secret);

 leave:
  xfree(bakfname);
  xfree(tmpfname);
  return rc;
}




#ifdef KEYBOX_WITH_X509 
int
keybox_insert_cert (KEYBOX_HANDLE hd, ksba_cert_t cert,
                    unsigned char *sha1_digest)
{
  int rc;
  const char *fname;
  KEYBOXBLOB blob;

  if (!hd)
    return gpg_error (GPG_ERR_INV_HANDLE); 
  if (!hd->kb)
    return gpg_error (GPG_ERR_INV_HANDLE); 
  fname = hd->kb->fname;
  if (!fname)
    return gpg_error (GPG_ERR_INV_HANDLE); 

  /* close this one otherwise we will mess up the position for a next
     search.  Fixme: it would be better to adjust the position after
     the write opertions.  */
  if (hd->fp)
    {
      fclose (hd->fp);
      hd->fp = NULL;
    }

  rc = _keybox_create_x509_blob (&blob, cert, sha1_digest, hd->ephemeral);
  if (!rc)
    {
      rc = blob_filecopy (1, fname, blob, hd->secret, 0, 0 );
      _keybox_release_blob (blob);
      /*    if (!rc && !hd->secret && kb_offtbl) */
      /*      { */
      /*        update_offset_hash_table_from_kb (kb_offtbl, kb, 0); */
      /*      } */
    }
  return rc;
}

int
keybox_update_cert (KEYBOX_HANDLE hd, ksba_cert_t cert,
                    unsigned char *sha1_digest)
{
  return -1;
}


#endif /*KEYBOX_WITH_X509*/


int
keybox_delete (KEYBOX_HANDLE hd)
{
  off_t off;
  const char *fname;
  FILE *fp;
  int rc;

  if (!hd)
    return gpg_error (GPG_ERR_INV_VALUE);
  if (!hd->found.blob)
    return gpg_error (GPG_ERR_NOTHING_FOUND);
  if (!hd->kb)
    return gpg_error (GPG_ERR_INV_HANDLE); 
  fname = hd->kb->fname;
  if (!fname)
    return gpg_error (GPG_ERR_INV_HANDLE); 

  off = _keybox_get_blob_fileoffset (hd->found.blob);
  if (off == (off_t)-1)
    return gpg_error (GPG_ERR_GENERAL);
  off += 4;

  if (hd->fp)
    {
      fclose (hd->fp);
      hd->fp = NULL;
    }
  
  fp = fopen (hd->kb->fname, "r+b");
  if (!fp)
    return gpg_error (gpg_err_code_from_errno (errno));

  if (fseeko (fp, off, SEEK_SET))
    rc = gpg_error (gpg_err_code_from_errno (errno));
  else if (putc (0, fp) == EOF)
    rc = gpg_error (gpg_err_code_from_errno (errno));
  else
    rc = 0;

  if (fclose (fp))
    {
      if (!rc)
        rc = gpg_error (gpg_err_code_from_errno (errno));
    }

  return rc;
}


