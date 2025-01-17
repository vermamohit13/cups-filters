/*

Copyright (c) 2016, Pranjal Bhor
Copyright (c) 2008-2016, Till Kamppeter
Copyright (c) 2011, Tim Waugh
Copyright (c) 2011-2013, Richard Hughes

Permission is hereby granted, free of charge, to any person obtaining
a copy of this software and associated documentation files (the
"Software"), to deal in the Software without restriction, including
without limitation the rights to use, copy, modify, merge, publish,
distribute, sublicense, and/or sell copies of the Software, and to
permit persons to whom the Software is furnished to do so, subject to
the following conditions:

The above copyright notice and this permission notice shall be included
in all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.

MIT Open Source License  -  http://www.opensource.org/

*/


/* PS/PDF to CUPS Raster filter based on mutool */

#include <config.h>
#include <cups/cups.h>
#if (CUPS_VERSION_MAJOR > 1) || (CUPS_VERSION_MINOR > 6)
#define HAVE_CUPS_1_7 1
#endif

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <fcntl.h>
#include <cups/raster.h>
#include <cupsfilters/colormanager.h>
#include <cupsfilters/raster.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <signal.h>
#include <unistd.h>
#include <errno.h>

#define PDF_MAX_CHECK_COMMENT_LINES	20

#define CUPS_IPTEMPFILE "/tmp/ip-XXXXXX"
#define CUPS_OPTEMPFILE "/tmp/op-XXXXXX"

#ifdef CUPS_RASTER_SYNCv1
typedef cups_page_header2_t mupdf_page_header;
#else
typedef cups_page_header_t mupdf_page_header;
#endif /* CUPS_RASTER_SYNCv1 */


int
parse_doc_type(FILE *fp, filter_logfunc_t log, void *ld)
{
  char buf[5];
  char *rc;

  /* get the first few bytes of the file */
  rewind(fp);
  rc = fgets(buf,sizeof(buf),fp);
  /* empty input */
  if (rc == NULL)
    return 1;

  /* is PDF */
  if (strncmp(buf,"%PDF",4) == 0)
    return 0;

  if(log) log(ld, FILTER_LOGLEVEL_DEBUG, "mupdftoraster: input file cannot be identified");
  return -1;
}

static void
parse_pdf_header_options(FILE *fp, mupdf_page_header *h)
{
  char buf[4096];
  int i;

  rewind(fp);
  /* skip until PDF start header */
  while (fgets(buf,sizeof(buf),fp) != 0) {
    if (strncmp(buf,"%PDF",4) == 0) {
      break;
    }
  }
  for (i = 0;i < PDF_MAX_CHECK_COMMENT_LINES;i++) {
    if (fgets(buf,sizeof(buf),fp) == 0) break;
    if (strncmp(buf,"%%PDFTOPDFNumCopies",19) == 0) {
      char *p;

      p = strchr(buf+19,':');
      h->NumCopies = atoi(p+1);
    } else if (strncmp(buf,"%%PDFTOPDFCollate",17) == 0) {
      char *p;

      p = strchr(buf+17,':');
      while (*p == ' ' || *p == '\t') p++;
      if (strncasecmp(p,"true",4) == 0) {
        h->Collate = CUPS_TRUE;
      } else {
        h->Collate = CUPS_FALSE;
      }
    }
  }
}

static void
add_pdf_header_options(mupdf_page_header *h,
		       cups_array_t 	 *mupdf_args)
{
  char tmpstr[1024];

  if ((h->HWResolution[0] != 100) || (h->HWResolution[1] != 100)) {
    snprintf(tmpstr, sizeof(tmpstr), "-r%dx%d", h->HWResolution[0], 
	     h->HWResolution[1]); 
    cupsArrayAdd(mupdf_args, strdup(tmpstr));
  } else {
    snprintf(tmpstr, sizeof(tmpstr), "-r100x100");
    cupsArrayAdd(mupdf_args, strdup(tmpstr));
  }

  snprintf(tmpstr, sizeof(tmpstr), "-w%d", h->cupsWidth);
  cupsArrayAdd(mupdf_args, strdup(tmpstr));

  snprintf(tmpstr, sizeof(tmpstr), "-h%d", h->cupsHeight);
  cupsArrayAdd(mupdf_args, strdup(tmpstr));

  switch (h->cupsColorSpace) {
  case CUPS_CSPACE_RGB:
  case CUPS_CSPACE_CMY:
  case CUPS_CSPACE_SRGB:
  case CUPS_CSPACE_ADOBERGB:
    snprintf(tmpstr, sizeof(tmpstr), "-crgb");
    break;

  case CUPS_CSPACE_CMYK:
    snprintf(tmpstr, sizeof(tmpstr), "-ccmyk");
    break;

  case CUPS_CSPACE_SW:
    snprintf(tmpstr, sizeof(tmpstr), "-cgray");
    break;

  default:
  case CUPS_CSPACE_K:
  case CUPS_CSPACE_W:
    snprintf(tmpstr, sizeof(tmpstr), "-cmono");
    break;
  }
  cupsArrayAdd(mupdf_args, strdup(tmpstr));
}

static int
mutool_spawn (const char *filename,
	      cups_array_t *mutool_args,
	      char **envp,
	      int outputfd,
	      filter_logfunc_t log,
	      void *ld,
	      filter_iscanceledfunc_t iscanceled,
	      void *icd)
{
  char *argument;
  char buf[BUFSIZ];
  char **mutoolargv;
  const char* apos;
  int errfds[2];
  int i;
  int numargs;
  int pid, mutoolpid, errpid;
  cups_file_t *logfp;
  filter_loglevel_t log_level;
  char *msg;
  int status = 65536;
  int wstatus;

  /* Put mutool command line argument into an array for the "exec()"
     call */
  numargs = cupsArrayCount(mutool_args);
  mutoolargv = calloc(numargs + 1, sizeof(char *));
  for (argument = (char *)cupsArrayFirst(mutool_args), i = 0; argument;
       argument = (char *)cupsArrayNext(mutool_args), i++) {
    mutoolargv[i] = argument;
  }
  mutoolargv[i] = NULL;

  if (log) {
    /* Debug output: Full mutool command line and environment variables */
    snprintf(buf, sizeof(buf), "mupdftoraster: mutool command line:");
    for (i = 0; mutoolargv[i]; i ++) {
      if ((strchr(mutoolargv[i],' ')) || (strchr(mutoolargv[i],'\t')))
	apos = "'";
      else
	apos = "";
      snprintf(buf + strlen(buf), sizeof(buf) - strlen(buf),
	       " %s%s%s", apos, mutoolargv[i], apos);
    }
    log(ld, FILTER_LOGLEVEL_DEBUG, "%s", buf);

    for (i = 0; envp[i]; i ++)
      log(ld, FILTER_LOGLEVEL_DEBUG,
	  "mupdftoraster: envp[%d]=\"%s\"", i, envp[i]);
  }

  /* Create a pipe for stderr output of mutool */
  if (pipe(errfds))
  {
    errfds[0] = -1;
    errfds[1] = -1;
    if (log) log(ld, FILTER_LOGLEVEL_ERROR,
		 "mupdftoraster: Unable to establish stderr pipe for mutool "
		 "call");
    goto out;
  }

  /* Set the "close on exec" flag on each end of the pipes... */
  if (fcntl(errfds[0], F_SETFD, fcntl(errfds[0], F_GETFD) | FD_CLOEXEC))
  {
    close(errfds[0]);
    close(errfds[1]);
    errfds[0] = -1;
    errfds[1] = -1;
    if (log) log(ld, FILTER_LOGLEVEL_ERROR,
		 "mupdftoraster: Unable to set \"close on exec\" flag on read "
		 "end of the stderr pipe for mutool call");
    goto out;
  }
  if (fcntl(errfds[1], F_SETFD, fcntl(errfds[1], F_GETFD) | FD_CLOEXEC))
  {
    close(errfds[0]);
    close(errfds[1]);
    if (log) log(ld, FILTER_LOGLEVEL_ERROR,
		 "mupdftoraster: Unable to set \"close on exec\" flag on write "
		 "end of the stderr pipe for mutool call");
    goto out;
  }

  if ((mutoolpid = fork()) == 0)
  {
    /* Couple errfds pipe with stdin of mutool process */
    if (errfds[1] >= 2) {
      if (errfds[1] != 2) {
	if (dup2(errfds[1], 2) < 0) {
	  if (log) log(ld, FILTER_LOGLEVEL_ERROR,
		       "mupdftoraster: Unable to couple pipe with stderr of "
		       "mutool process");
	  exit(1);
	}
	close(errfds[1]);
      }
      close(errfds[0]);
    } else {
      if (log) log(ld, FILTER_LOGLEVEL_ERROR,
		   "mupdftoraster: invalid pipe file descriptor to couple with "
		   "stderr of mutool process");
      exit(1);
    }

    /* Couple stdout of mutool process */
    if (outputfd >= 1) {
      if (outputfd != 1) {
	if (dup2(outputfd, 1) < 0) {
	  if (log) log(ld, FILTER_LOGLEVEL_ERROR,
		       "mupdftoraster: Unable to couple stdout of mutool "
		       "process");
	  exit(1);
	}
	close(outputfd);
      }
    } else {
      if (log) log(ld, FILTER_LOGLEVEL_ERROR,
		   "mupdftoraster: Invalid file descriptor to couple with "
		   "stdout of mutool process");
      exit(1);
    }

    /* Execute mutool command line ... */
    execvpe(filename, mutoolargv, envp);
    if (log) log(ld, FILTER_LOGLEVEL_ERROR,
		 "mupdftoraster: Unable to launch mutool: %s: %s", filename,
		 strerror(errno));
    exit(1);
  }
  if (log) log(ld, FILTER_LOGLEVEL_DEBUG,
	       "mupdftoraster: Started mutool (PID %d)", mutoolpid);

  close(errfds[1]);

  if ((errpid = fork()) == 0)
  {
    logfp = cupsFileOpenFd(errfds[0], "r");
    while (cupsFileGets(logfp, buf, sizeof(buf)))
      if (log) {
	if (strncmp(buf, "DEBUG: ", 7) == 0) {
	  log_level = FILTER_LOGLEVEL_DEBUG;
	  msg = buf + 7;
	} else if (strncmp(buf, "DEBUG2: ", 8) == 0) {
	  log_level = FILTER_LOGLEVEL_DEBUG;
	  msg = buf + 8;
	} else if (strncmp(buf, "INFO: ", 6) == 0) {
	  log_level = FILTER_LOGLEVEL_INFO;
	  msg = buf + 6;
	} else if (strncmp(buf, "WARNING: ", 9) == 0) {
	  log_level = FILTER_LOGLEVEL_WARN;
	  msg = buf + 9;
	} else if (strncmp(buf, "ERROR: ", 7) == 0) {
	  log_level = FILTER_LOGLEVEL_ERROR;
	  msg = buf + 7;
	} else {
	  log_level = FILTER_LOGLEVEL_DEBUG;
	  msg = buf;
	}
	log(ld, log_level, "mupdftoraster: %s", msg);
      }
    cupsFileClose(logfp);
    /* No need to close the fd errfds[0], as cupsFileClose(fp) does this
       already */
    /* Ignore errors of the logging process */
    exit(0);
  }
  if (log) log(ld, FILTER_LOGLEVEL_DEBUG,
	       "mupdftoraster: Started logging (PID %d)", errpid);

  close(errfds[0]);

  while (mutoolpid > 0 || errpid > 0) {
    if ((pid = wait(&wstatus)) < 0) {
      if (errno == EINTR && iscanceled && iscanceled(icd)) {
	if (log) log(ld, FILTER_LOGLEVEL_DEBUG,
		     "mupdftoraster: Job canceled, killing mutool ...");
	kill(mutoolpid, SIGTERM);
	mutoolpid = -1;
	kill(errpid, SIGTERM);
	errpid = -1;
	break;
      } else
	continue;
    }

    /* How did the filter terminate */
    if (wstatus) {
      if (WIFEXITED(wstatus)) {
	/* Via exit() anywhere or return() in the main() function */
	if (log) log(ld, FILTER_LOGLEVEL_ERROR,
		     "mupdftoraster: %s (PID %d) stopped with status %d",
		     (pid == mutoolpid ? "mutool" : "Logging"), pid,
		     WEXITSTATUS(wstatus));
	status = WEXITSTATUS(wstatus);
      } else {
	/* Via signal */
	if (log) log(ld, FILTER_LOGLEVEL_ERROR,
		     "mupdftoraster: %s (PID %d) crashed on signal %d",
		     (pid == mutoolpid ? "mutool" : "Logging"), pid,
		     WTERMSIG(wstatus));
	status = 256 * WTERMSIG(wstatus);
      }
    } else {
      if (log) log(ld, FILTER_LOGLEVEL_DEBUG,
		   "mupdftoraster: %s (PID %d) exited with no errors.",
		   (pid == mutoolpid ? "mutool" : "Logging"), pid);
      status = 0;
    }
    if (pid == mutoolpid)
      mutoolpid = -1;
    else  if (pid == errpid)
      errpid = -1;
  }

out:
  free(mutoolargv);
  return status;
}


int
mupdftoraster (int inputfd,         /* I - File descriptor input stream */
	       int outputfd,        /* I - File descriptor output stream */
	       int inputseekable,   /* I - Is input stream seekable? (unused)*/
	       filter_data_t *data, /* I - Job and printer data */
	       void *parameters)    /* I - Filter-specific parameters */
{
  char buf[BUFSIZ];
  char *icc_profile = NULL;
  char tmpstr[1024];
  cups_array_t *mupdf_args = NULL;
  cups_option_t *options = NULL;
  FILE *fp = NULL;
  char infilename[1024];
  mupdf_page_header h;
  int fd = -1;
  int cm_disabled;
  int n;
  int num_options;
  int empty = 0;
  int status = 1;
  ppd_file_t *ppd = NULL;
  struct sigaction sa;
  cm_calibration_t cm_calibrate;
  filter_data_t curr_data;
  filter_logfunc_t log = data->logfunc;
  void *ld = data->logdata;
  filter_iscanceledfunc_t iscanceled = data->iscanceledfunc;
  void *icd = data->iscanceleddata;
  char **envp;
  cups_cspace_t cspace = -1;

#ifdef HAVE_CUPS_1_7
  ppd_attr_t *attr;
#endif /* HAVE_CUPS_1_7 */

  if(parameters)
    envp = (char**)(parameters);
  else
    envp = NULL;

  memset(&sa, 0, sizeof(sa));
  /* Ignore SIGPIPE and have write return an error instead */
  sa.sa_handler = SIG_IGN;
  sigaction(SIGPIPE, &sa, NULL);

  num_options = data->num_options;

  ppd = data->ppd;
  if (ppd) {
    ppdMarkOptions (ppd, num_options, options);
  }

  fd = cupsTempFd(infilename, 1024);
    if (fd < 0) {
      if(log) log(ld, FILTER_LOGLEVEL_ERROR, "mupdftoraster: Can't create temporary file");
      goto out;
    }

    /* copy input file to the tmp file */
    while ((n = read(inputfd, buf, BUFSIZ)) > 0) {
      if (write(fd,buf,n) != n) {
        if(log) log(ld, FILTER_LOGLEVEL_ERROR, "mupdftoraster: Can't copy input to temporary file");
        close(fd);
        goto out;
      }
    }

  if (!inputfd) {

    if (lseek(fd,0,SEEK_SET) < 0) {
      if(log) log(ld, FILTER_LOGLEVEL_ERROR, "mupdftoraster: Can't rewind temporary file");
      close(fd);
      goto out;
    }

    if ((fp = fdopen(fd,"rb")) == 0) {
      if(log) log(ld, FILTER_LOGLEVEL_ERROR, "mupdftoraster: Can't open temporary file");
      close(fd);
      goto out;
    }
  } else {
    /* filename is specified */

    if ((fp = fdopen(fd,"rb")) == 0) {
      if(log) log(ld, FILTER_LOGLEVEL_ERROR, "mupdftoraster: Can't open temporary file");
      goto out;
    }
  }

  curr_data.printer = data->printer;
  curr_data.job_id = data->job_id;
  curr_data.job_user = data->job_user;
  curr_data.job_title = data->job_title;
  curr_data.copies = data->copies;
  curr_data.job_attrs = data->job_attrs;        /* We use command line options */
  curr_data.printer_attrs = data->printer_attrs;    /* We use the queue's PPD file */
  curr_data.num_options = data->num_options;
  curr_data.options = data->options;       /* Command line options from 5th arg */
  curr_data.ppdfile = data->ppdfile; /* PPD file name in the "PPD"
					  environment variable. */
  curr_data.ppd = data->ppd;
                                       /* Load PPD file */
  curr_data.logfunc = log;  /* Logging scheme of CUPS */
  curr_data.logdata = data->logdata;
  curr_data.iscanceledfunc = data->iscanceledfunc; /* Job-is-canceled
						       function */
  curr_data.iscanceleddata = data->iscanceleddata;


  /* If doc type is not PDF exit */
  empty = parse_doc_type(fp, log, ld);
  if (empty == -1)
    goto out;

  /*  Check status of color management in CUPS */
  cm_calibrate = cmGetCupsColorCalibrateMode(data, options, num_options);

  if (cm_calibrate == CM_CALIBRATION_ENABLED)
    cm_disabled = 1;
  else 
    cm_disabled = cmIsPrinterCmDisabled(data, getenv("PRINTER"));

  if (!cm_disabled)
    cmGetPrinterIccProfile(data, getenv("PRINTER"), &icc_profile, ppd);

/*  Find print-rendering-intent */

    getPrintRenderIntent(data, &h);
    if(log) log(ld, FILTER_LOGLEVEL_DEBUG,
    	"Print rendering intent = %s", h.cupsRenderingIntent);


  /* mutool parameters */
  mupdf_args = cupsArrayNew(NULL, NULL);
  if (!mupdf_args) {
    if(log) log(ld, FILTER_LOGLEVEL_ERROR, "mupdftoraster: Unable to allocate memory for mutool arguments array");
    goto out;
  }

  if(log) log(ld, FILTER_LOGLEVEL_DEBUG, "mupdftoraster: command: %s",
	      CUPS_MUTOOL);
  snprintf(tmpstr, sizeof(tmpstr), "%s", CUPS_MUTOOL);
  cupsArrayAdd(mupdf_args, strdup(tmpstr));
  cupsArrayAdd(mupdf_args, strdup("draw"));
  cupsArrayAdd(mupdf_args, strdup("-L"));
  cupsArrayAdd(mupdf_args, strdup("-o-"));
  cupsArrayAdd(mupdf_args, strdup("-smtf"));

  /* mutool output parameters */
  cupsArrayAdd(mupdf_args, strdup("-Fpwg"));

  /* Note that MuPDF only creates PWG Raster and never CUPS Raster,
     so we set the PWG Raster flag in the  cupsRasterPrepareHeader() call.
     This function takes care of generating a completely consistent PWG
     Raster header then, no extra manipulation needed.
     From the header h only cupsWidth/cupsHeight (dimensions in pixels),
     resolution, and color space are used here. */
  cupsRasterPrepareHeader(&h, &curr_data, OUTPUT_FORMAT_PWG_RASTER,
			  OUTPUT_FORMAT_PWG_RASTER, 1, &cspace);

  if ((h.HWResolution[0] == 100) && (h.HWResolution[1] == 100)) {
    /* No "Resolution" option */
    if (ppd && (attr = ppdFindAttr(ppd, "DefaultResolution", 0)) != NULL) {
      /* "*DefaultResolution" keyword in the PPD */
      const char *p = attr->value;
      h.HWResolution[0] = atoi(p);
      if ((p = strchr(p, 'x')) != NULL)
	h.HWResolution[1] = atoi(p);
      else
	h.HWResolution[1] = h.HWResolution[0];
      if (h.HWResolution[0] <= 0)
	h.HWResolution[0] = 300;
      if (h.HWResolution[1] <= 0)
	h.HWResolution[1] = h.HWResolution[0];
    } else {
      h.HWResolution[0] = 300;
      h.HWResolution[1] = 300;
    }
    h.cupsWidth = h.HWResolution[0] * h.PageSize[0] / 72;
    h.cupsHeight = h.HWResolution[1] * h.PageSize[1] / 72;
  }

  /* set PDF-specific options */
  parse_pdf_header_options(fp, &h);

  /* fixed other values that pdftopdf handles */
  h.MirrorPrint = CUPS_FALSE;
  h.Orientation = CUPS_ORIENT_0;

  /* get all the data from the header and pass it to mutool */
  add_pdf_header_options (&h, mupdf_args);

  snprintf(tmpstr, sizeof(tmpstr), "%s", infilename);
  cupsArrayAdd(mupdf_args, strdup(tmpstr));

  /* Execute mutool command line ... */
  snprintf(tmpstr, sizeof(tmpstr), "%s", CUPS_MUTOOL);
		
  /* call mutool */
  status = mutool_spawn (tmpstr, mupdf_args, envp, outputfd, log, ld,
			 iscanceled, icd);
  if (status != 0) status = 1;

  if(empty)
  {
    if(log) log(ld, FILTER_LOGLEVEL_ERROR, "mupdftoraster: Input is empty, outputting empty file.");
     status = 0;
  }
out:
  close(outputfd);
  if (fp)
    fclose(fp);
  if (mupdf_args) 
    cupsArrayDelete(mupdf_args);

  free(icc_profile);

  if (fd >= 0)
    unlink(infilename);
  return status;
}
