/* Copyright (c) 2002-2016 Pigeonhole authors, see the included COPYING file
 */

#include "lib.h"
#include "ioloop.h"
#include "array.h"
#include "str.h"
#include "safe-mkstemp.h"
#include "istream-private.h"
#include "istream-seekable.h"
#include "ostream.h"

#include "program-client-private.h"

#include <unistd.h>

#define MAX_OUTPUT_BUFFER_SIZE 16384
#define MAX_OUTPUT_MEMORY_BUFFER (1024*128)

static void program_client_timeout(struct program_client *pclient)
{
	program_client_fail(pclient, PROGRAM_CLIENT_ERROR_RUN_TIMEOUT);
}

static void program_client_connect_timeout(struct program_client *pclient)
{
	program_client_fail(pclient, PROGRAM_CLIENT_ERROR_CONNECT_TIMEOUT);
}

static int program_client_connect(struct program_client *pclient)
{
	int ret;

	if (pclient->set.client_connect_timeout_msecs != 0) {
		pclient->to = timeout_add
			(pclient->set.client_connect_timeout_msecs,
				program_client_connect_timeout, pclient);
	}

	if ( (ret=pclient->connect(pclient)) < 0 ) {
		program_client_fail(pclient, PROGRAM_CLIENT_ERROR_IO);
		return -1;
	}
	return ret;
}

static int program_client_close_output(struct program_client *pclient)
{
	int ret;

	if ( pclient->program_output != NULL )
		o_stream_destroy(&pclient->program_output);
	if ( (ret=pclient->close_output(pclient)) < 0 )
		return -1;
	pclient->program_output = NULL;

	return ret;
}

static void program_client_disconnect_extra_fds
(struct program_client *pclient)
{
	struct program_client_extra_fd *efds;
	unsigned int i, count;
	
	if (!array_is_created(&pclient->extra_fds))
		return;

	efds = array_get_modifiable(&pclient->extra_fds, &count);
	for ( i = 0; i < count; i++ ) {
		if ( efds[i].input != NULL )
			i_stream_unref(&efds[i].input);
		if ( efds[i].io != NULL )
			io_remove(&efds[i].io);
		if ( efds[i].parent_fd != -1 && close(efds[i].parent_fd) < 0 )
			i_error("close(fd=%d) failed: %m", efds[i].parent_fd);
	}
}

static void program_client_disconnect
(struct program_client *pclient, bool force)
{
	int ret, error = FALSE;

	if ( pclient->ioloop != NULL )
		io_loop_stop(pclient->ioloop);

	if ( pclient->disconnected )
		return;

	if ( (ret=program_client_close_output(pclient)) < 0 )
		error = TRUE;

	program_client_disconnect_extra_fds(pclient);
	if ( (ret=pclient->disconnect(pclient, force)) < 0 )
		error = TRUE;

	if ( pclient->program_input != NULL ) {
		if (pclient->output_seekable)
			i_stream_unref(&pclient->program_input);
		else
			i_stream_destroy(&pclient->program_input);
	} 
	if ( pclient->program_output != NULL )
		o_stream_destroy(&pclient->program_output);

	if ( pclient->to != NULL )
		timeout_remove(&pclient->to);
	if ( pclient->io != NULL )
		io_remove(&pclient->io);

	if (pclient->fd_in != -1 && close(pclient->fd_in) < 0)
		i_error("close(%s) failed: %m", pclient->path);
	if (pclient->fd_out != -1 && pclient->fd_out != pclient->fd_in
		&& close(pclient->fd_out) < 0)
		i_error("close(%s/out) failed: %m", pclient->path);
	pclient->fd_in = pclient->fd_out = -1;
	
	pclient->disconnected = TRUE;
	if (error && pclient->error == PROGRAM_CLIENT_ERROR_NONE ) {
		pclient->error = PROGRAM_CLIENT_ERROR_UNKNOWN;
	}
}

void program_client_fail
(struct program_client *pclient, enum program_client_error error)
{
	if ( pclient->error != PROGRAM_CLIENT_ERROR_NONE )
		return;

	pclient->error = error;
	program_client_disconnect(pclient, TRUE);

	pclient->failure(pclient, error);
}

static bool program_client_input_pending(struct program_client *pclient)
{
	struct program_client_extra_fd *efds = NULL;
	unsigned int count, i;

	if ( pclient->program_input != NULL &&
		!pclient->program_input->closed &&
		!i_stream_is_eof(pclient->program_input) ) {
		return TRUE;
	}

	if ( array_is_created(&pclient->extra_fds) ) {
		efds = array_get_modifiable(&pclient->extra_fds, &count);
		for ( i = 0; i < count; i++ ) {
			if ( efds[i].input != NULL &&
				!efds[i].input->closed &&
				!i_stream_is_eof(efds[i].input) ) {
				return TRUE;
			}
		}
	}

	return FALSE;
}

static int program_client_program_output(struct program_client *pclient)
{
	struct istream *input = pclient->input;
	struct ostream *output = pclient->program_output;
	const unsigned char *data;
	size_t size;
	int ret = 0;

	if ((ret = o_stream_flush(output)) <= 0) {
		if (ret < 0)
			program_client_fail(pclient, PROGRAM_CLIENT_ERROR_IO);
		return ret;
	}

	if ( input != NULL && output != NULL ) {
		do {
			while ( (data=i_stream_get_data(input, &size)) != NULL ) {
				ssize_t sent;
	
				if ( (sent=o_stream_send(output, data, size)) < 0 ) {
					program_client_fail(pclient, PROGRAM_CLIENT_ERROR_IO);
					return -1;
				}
	
				if ( sent == 0 )
					return 0;
				i_stream_skip(input, sent);
			}
		} while ( (ret=i_stream_read(input)) > 0 );

		if ( ret == 0 )
			return 1;

		if ( ret < 0 ) {
			if ( !input->eof ) {
				program_client_fail(pclient, PROGRAM_CLIENT_ERROR_IO);
				return -1;
			} else if ( !i_stream_have_bytes_left(input) ) {
				i_stream_unref(&pclient->input);
				input = NULL;

				if ( (ret = o_stream_flush(output)) <= 0 ) {
					if ( ret < 0 )
						program_client_fail(pclient, PROGRAM_CLIENT_ERROR_IO);
					return ret;
				}
			} 
		}
	}

	if ( input == NULL ) {
		if ( !program_client_input_pending(pclient) ) {
			program_client_disconnect(pclient, FALSE);
		} else if (program_client_close_output(pclient) < 0) {
			program_client_fail(pclient, PROGRAM_CLIENT_ERROR_IO);
		}
	}
	return 1;
}

static void program_client_program_input(struct program_client *pclient)
{
	struct istream *input = pclient->program_input;
	struct ostream *output = pclient->output;
	const unsigned char *data;
	size_t size;
	int ret = 0;

	if ( input != NULL ) {
		while ( (ret=i_stream_read_data(input, &data, &size, 0)) > 0 ) {
			if ( output != NULL ) {
				ssize_t sent;

				if ( (sent=o_stream_send(output, data, size)) < 0 ) {
					program_client_fail(pclient, PROGRAM_CLIENT_ERROR_IO);
					return;
				}
				size = (size_t)sent;
			}

			i_stream_skip(input, size);
		}

		if ( ret < 0 ) {
			if ( i_stream_is_eof(input) ) {
				if ( !program_client_input_pending(pclient) )
					program_client_disconnect(pclient, FALSE);
				return;
			}
			program_client_fail(pclient, PROGRAM_CLIENT_ERROR_IO);
		}
	}
}

static void program_client_extra_fd_input
(struct program_client_extra_fd *efd)
{
	struct program_client *pclient = efd->pclient;

	i_assert(efd->callback != NULL);
	efd->callback(efd->context, efd->input);

	if (efd->input->closed || i_stream_is_eof(efd->input)) {
		if ( !program_client_input_pending(pclient) )
			program_client_disconnect(pclient, FALSE);
	}
}

int program_client_connected
(struct program_client *pclient)
{
	int ret = 1;

	pclient->start_time = ioloop_time;
	if (pclient->to != NULL)
		timeout_remove(&pclient->to);
	if ( pclient->set.input_idle_timeout_secs != 0 ) {
		pclient->to = timeout_add(pclient->set.input_idle_timeout_secs*1000,
      program_client_timeout, pclient);
	}

	/* run output */
	if ( pclient->program_output != NULL &&
		(ret=program_client_program_output(pclient)) == 0 ) {
		if ( pclient->program_output != NULL ) {
			o_stream_set_flush_callback
				(pclient->program_output, program_client_program_output, pclient);
		}
	}

	return ret;
}

void program_client_init
(struct program_client *pclient, pool_t pool, const char *path,
	const char *const *args, const struct program_client_settings *set)
{
	pclient->pool = pool;
	pclient->path = p_strdup(pool, path);
	if ( args != NULL )
		pclient->args = p_strarray_dup(pool, args);
	pclient->set = *set;
	pclient->debug = set->debug;
	pclient->fd_in = -1;
	pclient->fd_out = -1;
}

void program_client_set_input
(struct program_client *pclient, struct istream *input)
{
	if ( pclient->input != NULL )
		i_stream_unref(&pclient->input);
	if ( input != NULL )
		i_stream_ref(input);
	pclient->input = input;
}

void program_client_set_output
(struct program_client *pclient, struct ostream *output)
{
	if ( pclient->output != NULL )
		o_stream_unref(&pclient->output);
	if ( output != NULL )
		o_stream_ref(output);
	pclient->output = output;
	pclient->output_seekable = FALSE;
	i_free(pclient->temp_prefix);
}

void program_client_set_output_seekable
(struct program_client *pclient, const char *temp_prefix)
{
	if ( pclient->output != NULL )
		o_stream_unref(&pclient->output);
	pclient->temp_prefix = i_strdup(temp_prefix);
	pclient->output_seekable = TRUE;
}

struct istream *program_client_get_output_seekable
(struct program_client *pclient)
{
	struct istream *input = pclient->seekable_output;
	
	pclient->seekable_output = NULL;

	i_stream_seek(input, 0);
	return input;
}

#undef program_client_set_extra_fd
void program_client_set_extra_fd
(struct program_client *pclient, int fd,
	program_client_fd_callback_t *callback, void *context)
{
	struct program_client_extra_fd *efds;
	struct program_client_extra_fd *efd = NULL;
	unsigned int i, count;
	i_assert(fd > 1);
	
	if ( !array_is_created(&pclient->extra_fds) )
		p_array_init(&pclient->extra_fds, pclient->pool, 2);

	efds = array_get_modifiable(&pclient->extra_fds, &count);
	for ( i = 0; i < count; i++ ) {
		if ( efds[i].child_fd == fd ) {
			efd = &efds[i];
			break;
		}
	}

	if ( efd == NULL ) {
		efd = array_append_space(&pclient->extra_fds);
		efd->pclient = pclient;
		efd->child_fd = fd;
		efd->parent_fd = -1;
	}
	efd->callback = callback;
	efd->context = context;
}

void program_client_set_env
(struct program_client *pclient, const char *name, const char *value)
{
	const char *env;

	if ( !array_is_created(&pclient->envs) )
		p_array_init(&pclient->envs, pclient->pool, 16);

	env = p_strdup_printf(pclient->pool, "%s=%s", name, value);
	array_append(&pclient->envs, &env, 1);
}

static int program_client_seekable_fd_callback
(const char **path_r, void *context)
{
	struct program_client *pclient = (struct program_client *)context;
	string_t *path;
	int fd;

	path = t_str_new(128);
	str_append(path, pclient->temp_prefix);
	fd = safe_mkstemp(path, 0600, (uid_t)-1, (gid_t)-1);
	if (fd == -1) {
		i_error("safe_mkstemp(%s) failed: %m", str_c(path));
		return -1;
	}

	/* we just want the fd, unlink it */
	if (unlink(str_c(path)) < 0) {
		/* shouldn't happen.. */
		i_error("unlink(%s) failed: %m", str_c(path));
		i_close_fd(&fd);
		return -1;
	}

	*path_r = str_c(path);
	return fd;
}

void program_client_init_streams(struct program_client *pclient)
{
	/* Create streams for normal program I/O */
	if ( pclient->fd_out >= 0 ) {
		pclient->program_output =
			o_stream_create_fd(pclient->fd_out, MAX_OUTPUT_BUFFER_SIZE, FALSE);
	}
	if ( pclient->fd_in >= 0 ) {
		struct istream *input;
		
		input = i_stream_create_fd(pclient->fd_in, (size_t)-1, FALSE);

		if (pclient->output_seekable) {
			struct istream *input2 = input, *input_list[2];
	
			input_list[0] = input2; input_list[1] = NULL;
			input = i_stream_create_seekable(input_list, MAX_OUTPUT_MEMORY_BUFFER,
						 program_client_seekable_fd_callback, pclient);
			i_stream_unref(&input2);
			pclient->seekable_output = input;
			i_stream_ref(pclient->seekable_output);
		}

		pclient->program_input = input;
		pclient->io = io_add
			(pclient->fd_in, IO_READ, program_client_program_input, pclient);
	}

	/* Create streams for additional output through side-channel fds */
	if ( array_is_created(&pclient->extra_fds) ) {
		struct program_client_extra_fd *efds = NULL;
		unsigned int count, i;
		
		efds = array_get_modifiable(&pclient->extra_fds, &count);
		for ( i = 0; i < count; i++ ) {
			i_assert( efds[i].parent_fd >= 0 );
			efds[i].input = i_stream_create_fd
				(efds[i].parent_fd, (size_t)-1, FALSE);
			efds[i].io = io_add
				(efds[i].parent_fd, IO_READ, program_client_extra_fd_input, &efds[i]);
		}
	}
}

void program_client_destroy(struct program_client **_pclient)
{
	struct program_client *pclient = *_pclient;

	program_client_disconnect(pclient, TRUE);

	if ( pclient->input != NULL )
		i_stream_unref(&pclient->input);
	if ( pclient->output != NULL )
		o_stream_unref(&pclient->output);
	if ( pclient->seekable_output != NULL )
		i_stream_unref(&pclient->seekable_output);
	if ( pclient->io != NULL )
		io_remove(&pclient->io);
	if ( pclient->ioloop != NULL )
		io_loop_destroy(&pclient->ioloop);
	i_free(pclient->temp_prefix);
	pool_unref(&pclient->pool);
	*_pclient = NULL;
}

int program_client_run(struct program_client *pclient)
{
	int ret;

	/* reset */
	pclient->disconnected = FALSE;
	pclient->exit_code = 1;
	pclient->error = PROGRAM_CLIENT_ERROR_NONE;

	pclient->ioloop = io_loop_create();

	if ( (ret=program_client_connect(pclient)) >= 0 ) {
		/* run output */
		if ( ret > 0 && pclient->program_output != NULL &&
			(ret=o_stream_flush(pclient->program_output)) == 0 ) {
			o_stream_set_flush_callback
				(pclient->program_output, program_client_program_output, pclient);
		}

		/* run i/o event loop */
		if ( ret < 0 ) {
			pclient->error = PROGRAM_CLIENT_ERROR_IO;
		} else if ( !pclient->disconnected &&
			(ret == 0 || program_client_input_pending(pclient)) ) {
			io_loop_run(pclient->ioloop);
		}

		/* finished */
		program_client_disconnect(pclient, FALSE);
	}

	io_loop_destroy(&pclient->ioloop);

	if ( pclient->error != PROGRAM_CLIENT_ERROR_NONE )
		return -1;

	return pclient->exit_code;
}

