/*
 * $Id: stdio_init_exit.c,v 1.22 2005-03-03 14:20:55 obarthel Exp $
 *
 * :ts=4
 *
 * Portable ISO 'C' (1994) runtime library for the Amiga computer
 * Copyright (c) 2002-2005 by Olaf Barthel <olsen@sourcery.han.de>
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 *   - Redistributions of source code must retain the above copyright
 *     notice, this list of conditions and the following disclaimer.
 *
 *   - Neither the name of Olaf Barthel nor the names of contributors
 *     may be used to endorse or promote products derived from this
 *     software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#ifndef _STDLIB_MEM_DEBUG_H
#include "stdlib_mem_debug.h"
#endif /* _STDLIB_MEM_DEBUG_H */

/****************************************************************************/

#ifndef _STDIO_HEADERS_H
#include "stdio_headers.h"
#endif /* _STDIO_HEADERS_H */

#ifndef _UNISTD_HEADERS_H
#include "unistd_headers.h"
#endif /* _UNISTD_HEADERS_H */

/****************************************************************************/

#include "stdlib_protos.h"

/****************************************************************************/

#ifndef ID_RAWCON
#define ID_RAWCON 0x52415700L /* "RAW\0" */
#endif /* ID_RAWCON */

/****************************************************************************/

void
__close_all_files(void)
{
	int i;

	ENTER();

	__check_abort_enabled = FALSE;

	__stdio_lock();

	if(__iob != NULL && __num_iob > 0)
	{
		for(i = 0 ; i < __num_iob ; i++)
		{
			if(FLAG_IS_SET(__iob[i]->iob_Flags,IOBF_IN_USE))
				fclose((FILE *)__iob[i]);
		}

		__num_iob	= 0;
		__iob		= NULL;
	}

	if(__fd != NULL && __num_fd > 0)
	{
		for(i = 0 ; i < __num_fd ; i++)
		{
			if(FLAG_IS_SET(__fd[i]->fd_Flags,FDF_IN_USE))
				close(i);
		}

		__num_fd	= 0;
		__fd		= NULL;
	}

	__stdio_unlock();

	LEAVE();
}

/****************************************************************************/

CLIB_DESTRUCTOR(__stdio_exit)
{
	ENTER();

	__close_all_files();

	__stdio_lock_exit();

	LEAVE();
}

/****************************************************************************/

int
__stdio_init(void)
{
	const int num_standard_files = (STDERR_FILENO-STDIN_FILENO+1);

	struct SignalSemaphore * stdio_lock;
	struct SignalSemaphore * fd_lock;
	BPTR default_file;
	ULONG fd_flags,iob_flags;
	int result = ERROR;
	char * buffer;
	char * aligned_buffer;
	int i;

	ENTER();

	if(__stdio_lock_init() < 0)
		goto out;

	__iob = malloc(sizeof(*__iob) * num_standard_files);
	if(__iob == NULL)
		goto out;

	for(i = 0 ; i < num_standard_files ; i++)
	{
		__iob[i] = malloc(sizeof(*__iob[i]));
		if(__iob[i] == NULL)
			goto out;

		memset(__iob[i],0,sizeof(*__iob[i]));
	}

	__num_iob = num_standard_files;

	__fd = malloc(sizeof(*__fd) * num_standard_files);
	if(__fd == NULL)
		goto out;

	for(i = 0 ; i < num_standard_files ; i++)
	{
		__fd[i] = malloc(sizeof(*__fd[i]));
		if(__fd[i] == NULL)
			goto out;

		memset(__fd[i],0,sizeof(*__fd[i]));
	}

	__num_fd = num_standard_files;

	/* Now initialize the standard I/O streams (input, output, error). */
	for(i = STDIN_FILENO ; i <= STDERR_FILENO ; i++)
	{
		PROFILE_OFF();

		switch(i)
		{
			case STDIN_FILENO:

				iob_flags		= IOBF_IN_USE | IOBF_READ | IOBF_NO_NUL | IOBF_BUFFER_MODE_LINE;
				fd_flags		= FDF_IN_USE | FDF_READ | FDF_NO_CLOSE;
				default_file	= Input();
				break;

			case STDOUT_FILENO:

				iob_flags		= IOBF_IN_USE | IOBF_WRITE | IOBF_NO_NUL | IOBF_BUFFER_MODE_LINE;
				fd_flags		= FDF_IN_USE | FDF_WRITE | FDF_NO_CLOSE;
				default_file	= Output();
				break;

			case STDERR_FILENO:
			default:

				iob_flags		= IOBF_IN_USE | IOBF_WRITE | IOBF_NO_NUL | IOBF_BUFFER_MODE_NONE;
				fd_flags		= FDF_IN_USE | FDF_WRITE;
				default_file	= ZERO; /* NOTE: this is really initialized later; see below... */
				break;
		}

		PROFILE_ON();

		/* Allocate a little more memory than necessary. */
		buffer = malloc(BUFSIZ + (CACHE_LINE_SIZE-1));
		if(buffer == NULL)
			goto out;

		#if defined(__THREAD_SAFE)
		{
			/* Allocate memory for an arbitration mechanism, then
			   initialize it. */
			stdio_lock	= __create_semaphore();
			fd_lock		= __create_semaphore();

			if(stdio_lock == NULL || fd_lock == NULL)
			{
				__delete_semaphore(stdio_lock);
				__delete_semaphore(fd_lock);

				goto out;
			}
		}
		#else
		{
			stdio_lock	= NULL;
			fd_lock		= NULL;
		}
		#endif /* __THREAD_SAFE */

		/* Check if this stream is attached to a console window. */
		if(default_file != ZERO)
		{
			PROFILE_OFF();

			if(IsInteractive(default_file))
				SET_FLAG(fd_flags,FDF_IS_INTERACTIVE);

			PROFILE_ON();
		}

		/* Align the buffer start address to a cache line boundary. */
		aligned_buffer = (char *)((ULONG)(buffer + (CACHE_LINE_SIZE-1)) & ~(CACHE_LINE_SIZE-1));

		__initialize_fd(__fd[i],__fd_hook_entry,default_file,fd_flags,fd_lock);

		__initialize_iob(__iob[i],__iob_hook_entry,
			buffer,
			aligned_buffer,BUFSIZ,
			i,
			i,
			iob_flags,
			stdio_lock);
	}

	/* If the program was launched from Workbench, we continue by
	   duplicating the default output stream for use as the
	   standard error stream. */
	if(__WBenchMsg != NULL)
	{
		PROFILE_OFF();
		__fd[STDERR_FILENO]->fd_DefaultFile = Output();
		PROFILE_ON();

		SET_FLAG(__fd[STDERR_FILENO]->fd_Flags,FDF_NO_CLOSE);
	}
	else
	{
		BPTR ces;

		/* Figure out what the default error output stream is. */
		#if defined(__amigaos4__)
		{
			ces = ErrorOutput();
		}
		#else
		{
			struct Process * this_process = (struct Process *)FindTask(NULL);

			ces = this_process->pr_CES;
		}
		#endif /* __amigaos4__ */

		/* Is the standard error stream configured? If so, use it.
		   Otherwise, try to duplicate the standard output stream. */
		if(ces != ZERO)
		{
			__fd[STDERR_FILENO]->fd_DefaultFile = ces;

			SET_FLAG(__fd[STDERR_FILENO]->fd_Flags,FDF_NO_CLOSE);
		}
		else
		{
			__fd[STDERR_FILENO]->fd_DefaultFile = Open("CONSOLE:",MODE_NEWFILE);
		}
	}

	PROFILE_OFF();

	/* Figure out if the standard error stream is bound to a console. */
	if(__fd[STDERR_FILENO]->fd_DefaultFile != ZERO)
	{
		if(IsInteractive(__fd[STDERR_FILENO]->fd_DefaultFile))
			SET_FLAG(__fd[STDERR_FILENO]->fd_Flags,FDF_IS_INTERACTIVE);
	}

	PROFILE_ON();

	result = OK;

 out:

	RETURN(result);
	return(result);
}
