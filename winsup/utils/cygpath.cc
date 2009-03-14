/* cygpath.cc -- convert pathnames between Windows and Unix format
   Copyright 1998, 1999, 2000, 2001, 2002, 2003, 2004, 2005,
   2006, 2007, 2008, 2009 Red Hat, Inc.

This file is part of Cygwin.

This software is a copyrighted work licensed under the terms of the
Cygwin license.  Please consult the file "CYGWIN_LICENSE" for
details. */

#define NOCOMATTRIBUTE

#include <shlobj.h>
#include <stdio.h>
#include <string.h>
#include <wchar.h>
#include <locale.h>
#include <stdlib.h>
#include <argz.h>
#include <limits.h>
#include <getopt.h>
#include <windows.h>
#include <io.h>
#include <sys/fcntl.h>
#include <sys/cygwin.h>
#include <ctype.h>
#include <errno.h>
#include <ddk/ntddk.h>
#include <ddk/winddk.h>
#include <ddk/ntifs.h>
#include "wide_path.h"

static const char version[] = "$Revision$";

static char *prog_name;
static char *file_arg, *output_arg;
static int path_flag, unix_flag, windows_flag, absolute_flag;
static int shortname_flag, longname_flag;
static int ignore_flag, allusers_flag, output_flag;
static int mixed_flag, options_from_file_flag, mode_flag;

static const char *format_type_arg;

static struct option long_options[] = {
  {(char *) "absolute", no_argument, NULL, 'a'},
  {(char *) "close", required_argument, NULL, 'c'},
  {(char *) "dos", no_argument, NULL, 'd'},
  {(char *) "file", required_argument, NULL, 'f'},
  {(char *) "help", no_argument, NULL, 'h'},
  {(char *) "ignore", no_argument, NULL, 'i'},
  {(char *) "long-name", no_argument, NULL, 'l'},
  {(char *) "mixed", no_argument, NULL, 'm'},
  {(char *) "mode", no_argument, NULL, 'M'},
  {(char *) "option", no_argument, NULL, 'o'},
  {(char *) "path", no_argument, NULL, 'p'},
  {(char *) "short-name", no_argument, NULL, 's'},
  {(char *) "type", required_argument, NULL, 't'},
  {(char *) "unix", no_argument, NULL, 'u'},
  {(char *) "version", no_argument, NULL, 'v'},
  {(char *) "windows", no_argument, NULL, 'w'},
  {(char *) "allusers", no_argument, NULL, 'A'},
  {(char *) "desktop", no_argument, NULL, 'D'},
  {(char *) "homeroot", no_argument, NULL, 'H'},
  {(char *) "mydocs", no_argument, NULL, 'O'},
  {(char *) "smprograms", no_argument, NULL, 'P'},
  {(char *) "sysdir", no_argument, NULL, 'S'},
  {(char *) "windir", no_argument, NULL, 'W'},
  {(char *) "folder", required_argument, NULL, 'F'},
  {0, no_argument, 0, 0}
};

static char options[] = "ac:df:hilmMopst:uvwADHOPSWF:";

static void
usage (FILE * stream, int status)
{
  if (!ignore_flag || !status)
    fprintf (stream, "\
Usage: %s (-d|-m|-u|-w|-t TYPE) [-f FILE] [OPTION]... NAME...\n\
       %s [-c HANDLE] \n\
       %s [-ADHOPSW] \n\
       %s [-F ID] \n\
Convert Unix and Windows format paths, or output system path information\n\
\n\
Output type options:\n\
  -d, --dos             print DOS (short) form of NAMEs (C:\\PROGRA~1\\)\n\
  -m, --mixed           like --windows, but with regular slashes (C:/WINNT)\n\
  -M, --mode            report on mode of file (binmode or textmode)\n\
  -u, --unix            (default) print Unix form of NAMEs (/cygdrive/c/winnt)\n\
  -w, --windows         print Windows form of NAMEs (C:\\WINNT)\n\
  -t, --type TYPE       print TYPE form: 'dos', 'mixed', 'unix', or 'windows'\n\
Path conversion options:\n\
  -a, --absolute        output absolute path\n\
  -l, --long-name       print Windows long form of NAMEs (with -w, -m only)\n\
  -p, --path            NAME is a PATH list (i.e., '/bin:/usr/bin')\n\
  -s, --short-name      print DOS (short) form of NAMEs (with -w, -m only)\n\
System information:\n\
  -A, --allusers        use `All Users' instead of current user for -D, -O, -P\n\
  -D, --desktop         output `Desktop' directory and exit\n\
  -H, --homeroot        output `Profiles' directory (home root) and exit\n\
  -O, --mydocs          output `My Documents' directory and exit\n\
  -P, --smprograms      output Start Menu `Programs' directory and exit\n\
  -S, --sysdir          output system directory and exit\n\
  -W, --windir          output `Windows' directory and exit\n\
  -F, --folder ID       output special folder with numeric ID and exit\n\
", prog_name, prog_name, prog_name, prog_name);
  if (ignore_flag)
    /* nothing to do */;
  else if (stream != stdout)
    fprintf(stream, "Try `%s --help' for more information.\n", prog_name);
  else
    {
      fprintf (stream, "\
Other options:\n\
  -f, --file FILE       read FILE for input; use - to read from STDIN\n\
  -o, --option          read options from FILE as well (for use with --file)\n\
  -c, --close HANDLE    close HANDLE (for use in captured process)\n\
  -i, --ignore          ignore missing argument\n\
  -h, --help            output usage information and exit\n\
  -v, --version         output version information and exit\n\
");
    }
  exit (ignore_flag ? 0 : status);
}

static inline BOOLEAN
RtlAllocateUnicodeString (PUNICODE_STRING uni, ULONG size)
{
  uni->Length = 0;
  uni->MaximumLength = 512;
  uni->Buffer = (WCHAR *) malloc (size);
  return uni->Buffer != NULL;
}

static char *
get_device_name (char *path)
{
  UNICODE_STRING ntdev, tgtdev, ntdevdir;
  ANSI_STRING ans;
  OBJECT_ATTRIBUTES ntobj;
  NTSTATUS status;
  HANDLE lnk, dir;
  char *ret = strdup (path);
  PDIRECTORY_BASIC_INFORMATION odi = (PDIRECTORY_BASIC_INFORMATION)
				     alloca (4096);
  BOOLEAN restart;
  ULONG cont;

  if (strncasecmp (path, "\\Device\\", 8))
    return ret;

  if (!RtlAllocateUnicodeString (&ntdev, 65536))
    return ret;
  if (!RtlAllocateUnicodeString (&tgtdev, 65536))
    return ret;
  RtlInitAnsiString (&ans, path);
  RtlAnsiStringToUnicodeString (&ntdev, &ans, FALSE);

  /* First check if the given device name is a symbolic link itself.  If so,
     query it and use the new name as actual device name to search for in the
     DOS device name directory.  If not, just use the incoming device name. */
  InitializeObjectAttributes (&ntobj, &ntdev, OBJ_CASE_INSENSITIVE, NULL, NULL);
  status = ZwOpenSymbolicLinkObject (&lnk, SYMBOLIC_LINK_QUERY, &ntobj);
  if (NT_SUCCESS (status))
    {
      status = ZwQuerySymbolicLinkObject (lnk, &tgtdev, NULL);
      ZwClose (lnk);
      if (!NT_SUCCESS (status))
	goto out;
      RtlCopyUnicodeString (&ntdev, &tgtdev);
    }
  else if (status != STATUS_OBJECT_TYPE_MISMATCH)
    goto out;

  for (int i = 0; i < 2; ++i)
    {
      /* There are two DOS device directories, the local and the global dir.
	 Try both, local first. */
      RtlInitUnicodeString (&ntdevdir, i ? L"\\GLOBAL??" : L"\\??");

      /* Open the directory... */
      InitializeObjectAttributes (&ntobj, &ntdevdir, OBJ_CASE_INSENSITIVE,
				  NULL, NULL);
      status = ZwOpenDirectoryObject (&dir, DIRECTORY_QUERY, &ntobj);
      if (!NT_SUCCESS (status))
	break;

      /* ...and scan it. */
      for (restart = TRUE, cont = 0;
	   NT_SUCCESS (ZwQueryDirectoryObject (dir, odi, 4096, TRUE,
					       restart, &cont, NULL));
	   restart = FALSE)
	{
	  /* For each entry check if it's a symbolic link. */
	  InitializeObjectAttributes (&ntobj, &odi->ObjectName,
				      OBJ_CASE_INSENSITIVE, dir, NULL);
	  status = ZwOpenSymbolicLinkObject (&lnk, SYMBOLIC_LINK_QUERY, &ntobj);
	  if (!NT_SUCCESS (status))
	    continue;
	  tgtdev.Length = 0;
	  tgtdev.MaximumLength = 512;
	  /* If so, query it and compare the target of the symlink with the
	     incoming device name. */
	  status = ZwQuerySymbolicLinkObject (lnk, &tgtdev, NULL);
	  ZwClose (lnk);
	  if (!NT_SUCCESS (status))
	    continue;
	  if (RtlEqualUnicodeString (&ntdev, &tgtdev, TRUE))
	    {
	      /* If the comparison succeeds, the name of the directory entry is
		 a valid DOS device name, if prepended with "\\.\".  Return that
		 valid DOS path. */
	      ULONG len = RtlUnicodeStringToAnsiSize (&odi->ObjectName);
	      ret = (char *) malloc (len + 4);
	      strcpy (ret, "\\\\.\\");
	      ans.Length = 0;
	      ans.MaximumLength = len;
	      ans.Buffer = ret + 4;
	      RtlUnicodeStringToAnsiString (&ans, &odi->ObjectName, FALSE);
	      ZwClose (dir);
	      goto out;
	    }
	}
      ZwClose (dir);
    }

out:
  free (tgtdev.Buffer);
  free (ntdev.Buffer);
  return ret;
}

static char *
get_device_paths (char *path)
{
  char *sbuf;
  char *ptr;
  int n = 1;

  ptr = path;
  while ((ptr = strchr (ptr, ';')))
    {
      ptr++;
      n++;
    }

  char *paths[n];
  DWORD acc = 0;
  int i;
  if (!n)
    return strdup ("");

  for (i = 0, ptr = path; ptr; i++)
    {
      char *next = ptr;
      ptr = strchr (ptr, ';');
      if (ptr)
	*ptr++ = 0;
      paths[i] = get_device_name (next);
      acc += strlen (paths[i]) + 1;
    }

  sbuf = (char *) malloc (acc + 1);
  if (sbuf == NULL)
    {
      fprintf (stderr, "%s: out of memory\n", prog_name);
      exit (1);
    }

  sbuf[0] = '\0';
  for (i = 0; i < n; i++)
    {
      strcat (strcat (sbuf, paths[i]), ";");
      free (paths[i]);
    }

  strchr (sbuf, '\0')[-1] = '\0';
  return sbuf;
}

static char *
get_short_paths (char *path)
{
  wchar_t *sbuf;
  wchar_t *sptr;
  char *next;
  char *ptr = path;
  char *end = strrchr (path, 0);
  DWORD acc = 0;
  DWORD len;

  while (ptr != NULL)
    {
      next = ptr;
      ptr = strchr (ptr, ';');
      if (ptr)
	*ptr++ = 0;
      wide_path wpath (next);
      len = GetShortPathNameW (wpath, NULL, 0);
      if (!len)
	{
	  fprintf (stderr, "%s: cannot create short name of %s\n", prog_name,
		   next);
	  exit (2);
	}
      acc += len + 1;
    }
  sptr = sbuf = (wchar_t *) malloc ((acc + 1) * sizeof (wchar_t));
  if (sbuf == NULL)
    {
      fprintf (stderr, "%s: out of memory\n", prog_name);
      exit (1);
    }
  ptr = path;
  for (;;)
    {
      wide_path wpath (ptr);
      len = GetShortPathNameW (wpath, sptr, acc);
      if (!len)
	{
	  fprintf (stderr, "%s: cannot create short name of %s\n", prog_name,
		   ptr);
	  exit (2);
	}

      ptr = strrchr (ptr, 0);
      sptr = wcsrchr (sptr, 0);
      if (ptr == end)
	break;
      *sptr = L';';
      ++ptr, ++sptr;
      acc -= len + 1;
    }
  len = wcstombs (NULL, sbuf, 0) + 1;
  ptr = (char *) malloc (len);
  if (ptr == NULL)
    {
      fprintf (stderr, "%s: out of memory\n", prog_name);
      exit (1);
    }
  wcstombs (ptr, sbuf, len);
  return ptr;
}

static char *
get_short_name (const char *filename)
{
  wchar_t buf[32768];
  char *sbuf;
  wide_path wpath (filename);
  DWORD len = GetShortPathNameW (wpath, buf, 32768);
  if (!len)
    {
      fprintf (stderr, "%s: cannot create short name of %s\n", prog_name,
	       filename);
      exit (2);
    }
  len = wcstombs (NULL, buf, 0) + 1;
  sbuf = (char *) malloc (len);
  if (sbuf == NULL)
    {
      fprintf (stderr, "%s: out of memory\n", prog_name);
      exit (1);
    }
  wcstombs (sbuf, buf, len);
  return sbuf;
}

static DWORD WINAPI
get_long_path_name_w32impl (LPCWSTR src, LPWSTR sbuf, DWORD)
{
  wchar_t *buf1 = (wchar_t *) malloc (32768);
  wchar_t *buf2 = (wchar_t *) malloc (32768);
  wchar_t *ptr;
  const wchar_t *pelem, *next;
  WIN32_FIND_DATAW w32_fd;
  DWORD len;

  wcscpy (buf1, src);
  *buf2 = L'\0';
  pelem = src;
  ptr = buf2;
  while (pelem)
    {
      next = pelem;
      if (*next == L'\\')
	{
	  wcscat (ptr++, L"\\");
	  pelem++;
	  if (!*pelem)
	    break;
	  continue;
	}
      pelem = wcschr (next, L'\\');
      len = pelem ? (pelem++ - next) : wcslen (next);
      wcsncpy (ptr, next, len);
      ptr[len] = L'\0';
      if (next[1] != L':' && wcscmp(next, L".") && wcscmp(next, L".."))
	{
	  HANDLE h;
	  h = FindFirstFileW (buf2, &w32_fd);
	  if (h != INVALID_HANDLE_VALUE)
	    {
	      wcscpy (ptr, w32_fd.cFileName);
	      FindClose (h);
	    }
	}
      ptr += wcslen (ptr);
      if (pelem)
	{
	  *ptr++ = '\\';
	  *ptr = 0;
	}
    }
  if (sbuf)
    wcscpy (sbuf, buf2);
  SetLastError (0);
  len = wcslen (buf2) + (sbuf ? 0 : 1);
  free (buf1);
  free (buf2);
  return len;
}

static char *
get_long_name (const char *filename, DWORD& len)
{
  char *sbuf;
  wchar_t buf[32768];
  static HINSTANCE k32 = LoadLibrary ("kernel32.dll");
  static DWORD (WINAPI *GetLongPathName) (LPCWSTR, LPWSTR, DWORD) =
    (DWORD (WINAPI *) (LPCWSTR, LPWSTR, DWORD)) GetProcAddress (k32, "GetLongPathNameW");
  if (!GetLongPathName)
    GetLongPathName = get_long_path_name_w32impl;

  wide_path wpath (filename);
  len = GetLongPathName (wpath, buf, 32768);
  if (len == 0)
    {
      DWORD err = GetLastError ();

      if (err == ERROR_INVALID_PARAMETER)
	{
	  fprintf (stderr, "%s: cannot create long name of %s\n", prog_name,
		   filename);
	  exit (2);
	}
      else if (err == ERROR_FILE_NOT_FOUND)
	get_long_path_name_w32impl (wpath, buf, 32768);
      else
	{
	  buf[0] = L'\0';
	  wcsncat (buf, wpath, 32767);
	}
    }
  len = wcstombs (NULL, buf, 0);
  sbuf = (char *) malloc (len + 1);
  if (!sbuf)
    {
      fprintf (stderr, "%s: out of memory\n", prog_name);
      exit (1);
    }
  wcstombs (sbuf, buf, len + 1);
  return sbuf;
}

static char *
get_long_paths (char *path)
{
  char *sbuf;
  char *ptr;
  int n = 1;

  ptr = path;
  while ((ptr = strchr (ptr, ';')))
    {
      ptr++;
      n++;
    }

  char *paths[n];
  DWORD acc = 0;
  int i;
  if (!n)
    return strdup ("");

  for (i = 0, ptr = path; ptr; i++)
    {
      DWORD len;
      char *next = ptr;
      ptr = strchr (ptr, ';');
      if (ptr)
	*ptr++ = 0;
      paths[i] = get_long_name (next, len);
      acc += len + 1;
    }

  sbuf = (char *) malloc (acc + 1);
  if (sbuf == NULL)
    {
      fprintf (stderr, "%s: out of memory\n", prog_name);
      exit (1);
    }

  sbuf[0] = '\0';
  for (i = 0; i < n; i++)
    {
      strcat (strcat (sbuf, paths[i]), ";");
      free (paths[i]);
    }

  strchr (sbuf, '\0')[-1] = '\0';
  return sbuf;
}

static void
convert_slashes (char* name)
{
  while ((name = strchr (name, '\\')) != NULL)
    {
      if (*name == '\\')
	*name = '/';
       name++;
   }
}

static char *
get_mixed_name (const char* filename)
{
  char* mixed_buf = strdup (filename);

  if (mixed_buf == NULL)
    {
      fprintf (stderr, "%s: out of memory\n", prog_name);
      exit (1);
    }

  convert_slashes (mixed_buf);

  return mixed_buf;
}

static bool
get_special_folder (char* path, int id)
{
  path[0] = 0;
  LPITEMIDLIST pidl = 0;
  if (SHGetSpecialFolderLocation (NULL, id, &pidl) != S_OK)
    return false;
  if (!SHGetPathFromIDList (pidl, path) || !path[0])
    return false;
  return true;
}

static void
get_user_folder (char* path, int id, int allid)
{
  get_special_folder (path, allusers_flag ? allid : id);
}

static void
do_sysfolders (char option)
{
  char *buf, buf1[MAX_PATH], buf2[PATH_MAX];
  DWORD len = MAX_PATH;
  WIN32_FIND_DATA w32_fd;
  HINSTANCE k32;
  BOOL (*GetProfilesDirectoryAPtr) (LPSTR, LPDWORD) = 0;

  buf = buf1;
  buf[0] = 0;
  switch (option)
    {
    case 'D':
      get_user_folder (buf, CSIDL_DESKTOPDIRECTORY,
			    CSIDL_COMMON_DESKTOPDIRECTORY);
      break;

    case 'P':
      get_user_folder (buf, CSIDL_PROGRAMS, CSIDL_COMMON_PROGRAMS);
      break;

    case 'O':
      get_user_folder (buf, CSIDL_PERSONAL, CSIDL_COMMON_DOCUMENTS);
      break;

    case 'F':
      {
	int val = -1, len = -1;
	if (!(sscanf (output_arg, "%i%n", &val, &len) == 1
	      && len == (int) strlen (output_arg) && val >= 0))
	  {
	    fprintf (stderr, "%s: syntax error in special folder ID %s\n",
		     prog_name, output_arg);
	    exit (1);
	  }
	get_special_folder (buf, val);
      }
      break;

    case 'H':
      k32 = LoadLibrary ("userenv");
      if (k32)
	GetProfilesDirectoryAPtr = (BOOL (*) (LPSTR, LPDWORD))
	  GetProcAddress (k32, "GetProfilesDirectoryA");
      if (GetProfilesDirectoryAPtr)
	(*GetProfilesDirectoryAPtr) (buf, &len);
      else
	{
	  GetWindowsDirectory (buf, MAX_PATH);
	  strcat (buf, "\\Profiles");
	}
      break;

    case 'S':
      GetSystemDirectory (buf, MAX_PATH);
      FindFirstFile (buf, &w32_fd);
      strcpy (strrchr (buf, '\\') + 1, w32_fd.cFileName);
      break;

    case 'W':
      GetWindowsDirectory (buf, MAX_PATH);
      break;

    default:
      usage (stderr, 1);
    }

  if (!buf[0])
    {
      fprintf (stderr, "%s: failed to retrieve special folder path\n", prog_name);
    }
  else if (!windows_flag)
    {
      if (cygwin_conv_path (CCP_WIN_A_TO_POSIX | CCP_RELATIVE, buf, buf2,
	  PATH_MAX))
	fprintf (stderr, "%s: error converting \"%s\" - %s\n",
		 prog_name, buf, strerror (errno));
      else
	buf = buf2;
    }
  else
    {
      if (shortname_flag)
	buf = get_short_name (buf);
      if (mixed_flag)
	buf = get_mixed_name (buf);
    }
  printf ("%s\n", buf);
}

static void
report_mode (char *filename)
{
  switch (cygwin_internal (CW_GET_BINMODE, filename))
    {
    case O_BINARY:
      printf ("%s: binary\n", filename);
      break;
    case O_TEXT:
      printf ("%s: text\n", filename);
      break;
    default:
      fprintf (stderr, "%s: file '%s' - %s\n", prog_name, filename,
	       strerror (errno));
      break;
    }
}

static void
do_pathconv (char *filename)
{
  char *buf;
  wchar_t *buf2;
  DWORD len;
  ssize_t err;
  cygwin_conv_path_t conv_func =
		      (unix_flag ? CCP_WIN_A_TO_POSIX
		      		 : (path_flag ? CCP_POSIX_TO_WIN_A
					      : CCP_POSIX_TO_WIN_W))
		    | (absolute_flag ? CCP_ABSOLUTE : CCP_RELATIVE);

  if (!path_flag)
    {
      len = strlen (filename);
      if (len)
	len = 32768;
      else if (ignore_flag)
	exit (0);
      else
	{
	  fprintf (stderr, "%s: can't convert empty path\n", prog_name);
	  exit (1);
	}
    }
  else
    len = cygwin_conv_path_list (conv_func, filename, NULL, 0);

  buf = (char *) malloc (len);
  if (!unix_flag && !path_flag)
    buf2 = (wchar_t *) malloc (len * sizeof (wchar_t));
  if (buf == NULL)
    {
      fprintf (stderr, "%s: out of memory\n", prog_name);
      exit (1);
    }

  if (path_flag)
    {
      err = cygwin_conv_path_list (conv_func, filename, buf, len);
      if (!unix_flag)
	{
	  if (err)
	    /* oops */;
	  buf = get_device_paths (buf);
	  if (shortname_flag)
	    buf = get_short_paths (buf);
	  if (longname_flag)
	    buf = get_long_paths (buf);
	  if (mixed_flag)
	    buf = get_mixed_name (buf);
	}
      if (err)
	{
	  fprintf (stderr, "%s: error converting \"%s\" - %s\n",
		   prog_name, filename, strerror (errno));
	  exit (1);
	}
    }
  else
    {
      err = cygwin_conv_path (conv_func, filename,
			      unix_flag ? (void *) buf : (void *) buf2, len);
      if (err)
	{
	  fprintf (stderr, "%s: error converting \"%s\" - %s\n",
		   prog_name, filename, strerror (errno));
	  exit (1);
	}
      if (!unix_flag)
	{
	  wcstombs (buf, buf2, 32768);
	  buf = get_device_name (buf);
	  if (shortname_flag)
	    buf = get_short_name (buf);
	  if (longname_flag)
	    buf = get_long_name (buf, len);
	  if (mixed_flag)
	    buf = get_mixed_name (buf);
	  len = 4;
	  if (strncmp (buf, "\\\\?\\UNC\\", 8) == 0)
	    len = 6;
	  if (strlen (buf) < MAX_PATH + len)
	    {
	      buf += len;
	      if (len == 6)
	        *buf = '\\';
	    }
	}
    }

  puts (buf);
}

static void
print_version ()
{
  const char *v = strchr (version, ':');
  int len;
  if (!v)
    {
      v = "?";
      len = 1;
    }
  else
    {
      v += 2;
      len = strchr (v, ' ') - v;
    }
  printf ("\
cygpath (cygwin) %.*s\n\
Path Conversion Utility\n\
Copyright 1998, 1999, 2000, 2001, 2002, 2003, 2004, 2005, 2006, \n\
          2007, 2008 Red Hat, Inc.\n\
Compiled on %s\n\
", len, v, __DATE__);
}

static int
do_options (int argc, char **argv, int from_file)
{
  int c, o = 0;
  path_flag = 0;
  unix_flag = 0;
  windows_flag = 0;
  shortname_flag = 0;
  longname_flag = 0;
  mixed_flag = 0;
  ignore_flag = 0;
  allusers_flag = 0;
  output_flag = 0;
  mode_flag = 0;
  if (!from_file)
    options_from_file_flag = 0;
  optind = 0;
  while ((c = getopt_long (argc, argv, options,
			   long_options, (int *) NULL)) != EOF)
    {
      switch (c)
	{
	case 'a':
	  absolute_flag = 1;
	  break;

	case 'c':
	  if (!optarg)
	    usage (stderr, 1);
	  CloseHandle ((HANDLE) strtoul (optarg, NULL, 16));
	  break;

	case 'd':
	  windows_flag = 1;
	  shortname_flag = 1;
	  break;

	case 'f':
	  if (from_file || !optarg)
	    usage (stderr, 1);
	  file_arg = optarg;
	  break;

	case 'M':
	  mode_flag = 1;
	  break;

	case 'o':
	  if (from_file)
	    usage (stderr, 1);
	  options_from_file_flag = 1;
	  break;

	case 'p':
	  path_flag = 1;
	  break;

	case 'u':
	  unix_flag = 1;
	  break;

	case 'w':
	  windows_flag = 1;
	  break;

	 case 'm':
	  windows_flag = 1;
	  mixed_flag = 1;
	  break;

	case 'l':
	  longname_flag = 1;
	  break;

	case 's':
	  shortname_flag = 1;
	  break;

	case 't':
	  if (!optarg)
	    usage (stderr, 1);

	  format_type_arg = (*optarg == '=') ? (optarg + 1) : (optarg);
	  if (strcasecmp (format_type_arg, "dos") == 0)
	    {
	      windows_flag = 1;
	      shortname_flag = 1;
	    }
	  else if (!strcasecmp (format_type_arg, "mixed"))
	    {
	      windows_flag = 1;
	      mixed_flag = 1;
	    }
	  else if (!strcasecmp (format_type_arg, "unix"))
	    unix_flag = 1;
	  else if (!strcasecmp (format_type_arg, "windows"))
	    windows_flag = 1;
	  else
	    usage (stderr, 1);
	  break;

	case 'A':
	  allusers_flag = 1;
	  break;

	case 'D':
	case 'H':
	case 'O':
	case 'P':
	case 'S':
	case 'W':
	  ++output_flag;
	  o = c;
	  break;

	case 'F':
	  if (!optarg)
	    usage (stderr, 1);
	  ++output_flag;
	  output_arg = optarg;
	  o = c;
	  break;

	case 'i':
	  ignore_flag = 1;
	  break;

	case 'h':
	  usage (stdout, 0);
	  break;

	case 'v':
	  print_version ();
	  exit (0);

	default:
	  usage (stderr, 1);
	  break;
	}
    }

  /* If none of the "important" flags are set, -u is default. */
  if (!unix_flag && !windows_flag && !mode_flag
      && (!from_file ? !options_from_file_flag : 1))
    unix_flag = 1;

  /* Only one of ... */
  if (unix_flag + windows_flag + mode_flag > 1
      + (!from_file ? options_from_file_flag : 0))
    usage (stderr, 1);

  /* options_from_file_flag requires a file. */
  if (!from_file && options_from_file_flag && !file_arg)
    usage (stderr, 1);

  /* longname and shortname don't play well together. */
  if (longname_flag && shortname_flag)
    usage (stderr, 1);

  /* longname and shortname only make sense with Windows paths. */
  if ((longname_flag || shortname_flag) && !windows_flag)
    usage (stderr, 1);

  return o;
}

static void
action (int argc, char **argv, int opt)
{
  if (output_flag)
    {
      if (argv[optind])
	usage (stderr, 1);

      do_sysfolders (opt);
    }
  else
    {
      if (optind > argc - 1)
	usage (stderr, 1);

      for (int i = optind; argv[i]; i++)
	if (mode_flag)
	  report_mode (argv[i]);
	else
	  do_pathconv (argv[i]);
    }
}

int
main (int argc, char **argv)
{
  int o;

  setlocale (LC_ALL, "");
  prog_name = strrchr (argv[0], '/');
  if (!prog_name)
    prog_name = strrchr (argv[0], '\\');
  if (!prog_name)
    prog_name = argv[0];
  else
    prog_name++;

  o = do_options (argc, argv, 0);

  if (!file_arg)
    action (argc, argv, o);
  else
    {
      FILE *fp;
      char buf[PATH_MAX * 2 + 1];

      if (argv[optind])
	usage (stderr, 1);

      if (strcmp (file_arg, "-"))
	{
	  if (!(fp = fopen (file_arg, "rt")))
	    {
	      perror ("cygpath");
	      exit (1);
	    }
	}
      else
	{
	  fp = stdin;
	  setmode (0, O_TEXT);
	}
      setbuf (stdout, NULL);

      while (fgets (buf, sizeof (buf), fp))
	{
	  size_t azl = 0;
	  int ac;
	  char *az, **av;
	  char *p = strchr (buf, '\n');
	  if (p)
	    *p = '\0';
	  if (argz_create_sep (buf, ' ', &az, &azl))
	    {
	      perror ("cygpath");
	      exit (1);
	    }
	  if (!az)
	    continue;
	  ac = argz_count (az, azl) + 1;
	  av = (char **) malloc ((ac + 1) * sizeof (char *));
	  if (!av)
	    {
	      perror ("cygpath");
	      exit (1);
	    }
	  av[0] = prog_name;
	  argz_extract (az, azl, av + 1);
	  if (options_from_file_flag)
	    o = do_options (ac, av, 1);
	  else
	    optind = 1;
	  action (ac, av, o);
	  free (az);
	  free (av);
	}
    }
  exit (0);
}
