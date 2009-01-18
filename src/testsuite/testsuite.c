/* Copyright (c) 2002-2009 Dovecot Sieve authors, see the included COPYING file
 */

#include "lib.h"
#include "lib-signals.h"
#include "ioloop.h"
#include "ostream.h"
#include "hostpid.h"
#include "mail-storage.h"
#include "mail-namespace.h"
#include "env-util.h"

#include "sieve.h"
#include "sieve-extensions.h"
#include "sieve-script.h"
#include "sieve-binary.h"
#include "sieve-result.h"
#include "sieve-interpreter.h"

#include "mail-raw.h"
#include "sieve-tool.h"

#include "testsuite-common.h"
#include "testsuite-result.h"
#include "testsuite-message.h"

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <pwd.h>

/*
 * Configuration
 */

#define DEFAULT_SENDMAIL_PATH "/usr/lib/sendmail"
#define DEFAULT_ENVELOPE_SENDER "MAILER-DAEMON"

/*
 * Testsuite initialization 
 */

static void testsuite_tool_init(void) 
{
	sieve_tool_init();

	(void) sieve_extension_register(&testsuite_extension);
	
	testsuite_init();
}

static void testsuite_tool_deinit(void)
{
	testsuite_deinit();
	
	sieve_tool_deinit();
}

/*
 * Testsuite execution
 */

static void print_help(void)
{
	printf(
"Usage: testsuite [-d <dump filename>] <scriptfile>\n"
	);
}

static int testsuite_run
(struct sieve_binary *sbin, const struct sieve_message_data *msgdata, 
	const struct sieve_script_env *senv, struct sieve_error_handler *ehandler)
{
        struct sieve_interpreter *interp;
	struct sieve_result *result;
        int ret = 0;

        /* Create the interpreter */
        if ( (interp=sieve_interpreter_create(sbin, ehandler)) == NULL )
                return SIEVE_EXEC_BIN_CORRUPT;

        /* Reset execution status */
        if ( senv->exec_status != NULL )
                memset(senv->exec_status, 0, sizeof(*senv->exec_status));

        /* Run the interpreter */
	result = testsuite_result_get();
	sieve_result_ref(result);
        ret = sieve_interpreter_run(interp, msgdata, senv, result);
	sieve_result_unref(&result);

        /* Free the interpreter */
        sieve_interpreter_free(&interp);

        return ret;
}

int main(int argc, char **argv) 
{
	const char *scriptfile, *dumpfile; 
	const char *user;
	int i, ret;
	struct sieve_binary *sbin;
	const char *sieve_dir;
	bool trace = FALSE;

	/* Parse arguments */
	scriptfile = dumpfile =  NULL;
	for (i = 1; i < argc; i++) {
		if (strcmp(argv[i], "-d") == 0) {
			/* dump file */
			i++;
			if (i == argc)
				i_fatal("Missing -d argument");
			dumpfile = argv[i];
#ifdef SIEVE_RUNTIME_TRACE
		} else if (strcmp(argv[i], "-t") == 0) {
			/* runtime trace */
			trace = TRUE;
#endif
		} else if ( scriptfile == NULL ) {
			scriptfile = argv[i];
		} else {
			print_help();
			i_fatal("Unknown argument: %s", argv[i]);
		}
	}
	
	if ( scriptfile == NULL ) {
		print_help();
		i_fatal("Missing <scriptfile> argument");
	}

	printf("Test case: %s:\n\n", scriptfile);

	/* Initialize testsuite */
	testsuite_tool_init();

	/* Initialize environment */
	sieve_dir = strrchr(scriptfile, '/');
	if ( sieve_dir == NULL )
		sieve_dir= "./";
	else
		sieve_dir = t_strdup_until(scriptfile, sieve_dir+1);

	/* Currently needed for include (FIXME) */
	env_put(t_strconcat("SIEVE_DIR=", sieve_dir, "included", NULL));
	env_put(t_strconcat("SIEVE_GLOBAL_DIR=", sieve_dir, "included-global", NULL));
	
	/* Compile sieve script */
	if ( (sbin = sieve_tool_script_compile(scriptfile, NULL)) != NULL ) {
		struct sieve_script_env scriptenv;
		struct sieve_error_handler *ehandler;

		/* Dump script */
		sieve_tool_dump_binary_to(sbin, dumpfile);
	
		/* Initialize mail storages */
		mail_users_init(getenv("AUTH_SOCKET_PATH"), getenv("DEBUG") != NULL);
		mail_storage_init();
		mail_storage_register_all();
		mailbox_list_register_all();

		/* Initialize message environment */
		user = sieve_tool_get_user();
		testsuite_message_init(user);

		memset(&scriptenv, 0, sizeof(scriptenv));
		scriptenv.default_mailbox = "INBOX";
		scriptenv.username = user;
		scriptenv.trace_stream = ( trace ? o_stream_create_fd(1, 0, FALSE) : NULL );

		/* Run the test */
		ehandler = sieve_stderr_ehandler_create(0);
		ret = testsuite_run(sbin, &testsuite_msgdata, &scriptenv, ehandler);
		sieve_error_handler_unref(&ehandler);

		switch ( ret ) {
		case SIEVE_EXEC_OK:
			break;
		case SIEVE_EXEC_FAILURE:
		case SIEVE_EXEC_KEEP_FAILED:
			testsuite_testcase_fail("execution aborted");
			break;
		case SIEVE_EXEC_BIN_CORRUPT:
			testsuite_testcase_fail("binary corrupt");
			break;
		default:
			testsuite_testcase_fail("unknown execution exit code");
		}

		sieve_close(&sbin);

		if ( scriptenv.trace_stream != NULL )
			o_stream_unref(&scriptenv.trace_stream);

		/* De-initialize message environment */
		testsuite_message_deinit();

		/* De-initialize mail storages */
		mail_storage_deinit();
		mail_users_deinit();
	} else {
		testsuite_testcase_fail("failed to compile testcase script");
	}

	/* De-initialize testsuite */
	testsuite_tool_deinit();  

	return testsuite_testcase_result();
}
