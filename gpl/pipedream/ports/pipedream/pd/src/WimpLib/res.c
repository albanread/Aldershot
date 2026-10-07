/* Copyright 1996 Acorn Computers Ltd
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
/************************************************************************/
/* © Acorn Computers Ltd, 1992.                                         */
/*                                                                      */
/* This file forms part of an unsupported source release of RISC_OSLib. */
/*                                                                      */
/* It may be freely used to create executable images for saleable       */
/* products but cannot be sold in source form or as an object library   */
/* without the prior written consent of Acorn Computers Ltd.            */
/*                                                                      */
/* If this file is re-distributed (even if modified) it should retain   */
/* this copyright notice.                                               */
/*                                                                      */
/************************************************************************/

/*
 * Title:   c.res
 * Purpose: access to resources
 * History: IDJ: 06-Feb-92: prepared for source release
 *
 */


#define BOOL int
#define TRUE 1
#define FALSE 0

#include "include.h" /* for SKS_ACW */

#include <stdio.h>
#include <string.h>

#include "os.h"
#include "trace.h"
#include "werr.h"

#include "res.h" /* Ensure consistent interface */

#define GetCountryNumber  240

static const char *programname = 0;

void res_init(const char *a) /* Started up with the program name */
{
  programname = a;
}

#ifdef SKS_ACW /* SKS - PipeDream's RISC_OSLib-type resources now live one level down */
#define RISC_OS_RESOURCE_SUBDIR "RISC_OS"
#endif

#ifndef SKS_ACW

BOOL res_findname(const char *leafname, char *buf /*out*/)
{
  os_filestr str;
  os_error *e;
  const char *progname;

#ifndef UROM
  progname = "Obey";
  strcpy(buf, "<Obey$Dir>.");
  if (programname) {
#endif
      progname = programname;
      strcpy(buf, programname);
      strcat(buf, ":");
#ifndef UROM
  }
#endif
  strcat(buf, leafname);
  str.action = 5;
  str.name = buf;
  e = os_file(&str);
  if (e && e->errnum == 0xCC || !e && str.action == 0) {
    /* File name 'contents of buf' not recognised - could be illegal path,
     * or file not found, so try using $Dir instead.
     */
    sprintf(buf, "<%s$Dir>.%s", progname, leafname);
  }

  return TRUE;
}

#else /* SKS_ACW */

/* simple replacement function which only looks on the path, which must therefore be set */

int res_findname(const char *leafname, char *buf /*out*/)
{
  _kernel_osfile_block fileblk;
  int res;

  res_prefixnamewithpath(leafname, buf);

  if(_kernel_osfile(5, buf, &fileblk) != 1)
      res = -1; /* without error report */
  else
      res = fileblk.start; /* file length */

  reportf("res_findname(%u:%s): length=%u\n", strlen(buf), buf, (size_t) res);

  return(res);
}

#endif /* SKS_ACW */

BOOL res_prefixnamewithpath(const char *leafname, char *buf /*out*/)
{
#ifndef UROM
  if (programname == 0) {
    strcpy(buf, "<Obey$Dir>.");
    strcat(buf, leafname);
  }
  else
#endif
  {
    strcpy(buf, programname);
    strcat(buf, ":");
#ifdef RISC_OS_RESOURCE_SUBDIR
    strcat(buf, RISC_OS_RESOURCE_SUBDIR ".");
#endif
    strcat(buf, leafname);
  }
  return TRUE;
}

BOOL res_prefixnamewithdir(const char *leafname, char *buf /*out*/)
{
#ifndef UROM
  if (programname == 0) {
    strcpy(buf, "<Obey$Dir>.");
    strcat(buf, leafname);
  }
  else
#endif
  {
#ifdef RISC_OS_RESOURCE_SUBDIR
    sprintf(buf, "<%s$Dir>." RISC_OS_RESOURCE_SUBDIR ".%s", programname, leafname);
#else
    sprintf(buf, "<%s$Dir>.%s", programname, leafname);
#endif
  }
  return TRUE;
}

#ifndef SKS_ACW

#ifndef UROM
FILE *res_openfile(const char *leafname, const char *mode)

{ char defaultname [FILENAME_MAX];

  res_findname (leafname, defaultname);

  if (*mode == 'r') /*i e, "r" or "r+"*/
  { char countryname [FILENAME_MAX];
    int r2=255, countrynum=0;
    FILE *fp;

    os_byte(GetCountryNumber, &countrynum, &r2);
    sprintf(countryname, "%s%i", defaultname, countrynum);
    if ((fp = fopen(countryname, mode)) != NULL)
      return fp;
  }

  return fopen (defaultname, mode);
}
#endif

#endif /* SKS_ACW */

/* end of c.res */
