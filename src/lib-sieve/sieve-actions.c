/* Copyright (c) 2002-2009 Dovecot Sieve authors, see the included COPYING file 
 */

#include "lib.h"
#include "strfuncs.h"
#include "str-sanitize.h"
#include "mail-storage.h"
#include "mail-namespace.h"

#include "sieve-code.h"
#include "sieve-extensions.h"
#include "sieve-binary.h"
#include "sieve-interpreter.h"
#include "sieve-dump.h"
#include "sieve-result.h"
#include "sieve-actions.h"

#include <ctype.h>

/*
 * Action execution environment
 */

const char *sieve_action_get_location(const struct sieve_action_exec_env *aenv)
{
	return t_strdup_printf("msgid=%s", aenv->msgdata->id == NULL ?
		"unspecified" : str_sanitize(aenv->msgdata->id, 80));
}

/*
 * Side-effect operand
 */
 
const struct sieve_operand_class sieve_side_effect_operand_class = 
	{ "SIDE-EFFECT" };

bool sieve_opr_side_effect_read
(const struct sieve_runtime_env *renv, sieve_size_t *address,
	struct sieve_side_effect *seffect)
{
	const struct sieve_side_effect_def *sdef;

	seffect->context = NULL;

	if ( !sieve_opr_object_read
		(renv, &sieve_side_effect_operand_class, address, &seffect->object) )
		return FALSE;

	sdef = seffect->def = 
		(const struct sieve_side_effect_def *) seffect->object.def;

	if ( sdef->read_context != NULL && 
		!sdef->read_context(seffect, renv, address, &seffect->context) ) {
		return FALSE;
	}

	return TRUE;
}

bool sieve_opr_side_effect_dump
(const struct sieve_dumptime_env *denv, sieve_size_t *address)
{
	struct sieve_side_effect seffect;
	const struct sieve_side_effect_def *sdef;
	
	if ( !sieve_opr_object_dump
		(denv, &sieve_side_effect_operand_class, address, &seffect.object) )
		return FALSE;
	
	sdef = seffect.def = 
		(const struct sieve_side_effect_def *) seffect.object.def;

	if ( sdef->dump_context != NULL ) {
		sieve_code_descend(denv);
		if ( !sdef->dump_context(&seffect, denv, address) ) {
			return FALSE;	
		}
		sieve_code_ascend(denv);
	}

	return TRUE;
}

/*
 * Store action
 */
 
/* Forward declarations */

static bool act_store_equals
	(const struct sieve_script_env *senv,
		const struct sieve_action *act1, const struct sieve_action *act2);
	
static int act_store_check_duplicate
	(const struct sieve_runtime_env *renv, 
		const struct sieve_action *act, 
		const struct sieve_action *act_other);
static void act_store_print
	(const struct sieve_action *action, 
		const struct sieve_result_print_env *rpenv, bool *keep);

static bool act_store_start
	(const struct sieve_action *action,
		const struct sieve_action_exec_env *aenv, void **tr_context);
static bool act_store_execute
	(const struct sieve_action *action, 
		const struct sieve_action_exec_env *aenv, void *tr_context);
static bool act_store_commit
	(const struct sieve_action *action, 
		const struct sieve_action_exec_env *aenv, void *tr_context, bool *keep);
static void act_store_rollback
	(const struct sieve_action *action, 
		const struct sieve_action_exec_env *aenv, void *tr_context, bool success);
		
/* Action object */

const struct sieve_action_def act_store = {
	"store",
	SIEVE_ACTFLAG_TRIES_DELIVER,
	act_store_equals,
	act_store_check_duplicate, 
	NULL, 
	act_store_print,
	act_store_start,
	act_store_execute,
	act_store_commit,
	act_store_rollback,
};

/* API */

int sieve_act_store_add_to_result
(const struct sieve_runtime_env *renv, 
	struct sieve_side_effects_list *seffects, const char *mailbox,
	unsigned int source_line)
{
	pool_t pool;
	struct act_store_context *act;
	
	/* Add redirect action to the result */
	pool = sieve_result_pool(renv->result);
	act = p_new(pool, struct act_store_context, 1);
	act->mailbox = p_strdup(pool, mailbox);

	return sieve_result_add_action(renv, NULL, &act_store, seffects, 
		source_line, (void *) act, 0);
}

void sieve_act_store_add_flags
(const struct sieve_action_exec_env *aenv, void *tr_context,
	const char *const *keywords, enum mail_flags flags)
{
	struct act_store_transaction *trans = 
		(struct act_store_transaction *) tr_context;

	/* Assign mail keywords for subsequent mailbox_copy() */
	if ( *keywords != NULL ) {
		const char *const *kw;

		if ( !array_is_created(&trans->keywords) ) {
			pool_t pool = sieve_result_pool(aenv->result); 
			p_array_init(&trans->keywords, pool, 2);
		}
		
		kw = keywords;
		while ( *kw != NULL ) {

			const char *kw_error;

			if ( trans->box != NULL ) {
				if ( mailbox_keyword_is_valid(trans->box, *kw, &kw_error) )
					array_append(&trans->keywords, kw, 1);
				else {
					char *error = "";
					if ( kw_error != NULL && *kw_error != '\0' ) {
						error = t_strdup_noconst(kw_error);
						error[0] = i_tolower(error[0]);
					}
				
					sieve_result_warning(aenv, 
						"specified IMAP keyword '%s' is invalid (ignored): %s", 
						str_sanitize(*kw, 64), error);
				}
			}

			kw++;
		}
	}

	/* Assign mail flags for subsequent mailbox_copy() */
	trans->flags |= flags;

	trans->flags_altered = TRUE;
}

void sieve_act_store_get_storage_error
(const struct sieve_action_exec_env *aenv, struct act_store_transaction *trans)
{
	pool_t pool = sieve_result_pool(aenv->result);
	
	trans->error = p_strdup(pool, 
		mail_storage_get_last_error(trans->namespace->storage, &trans->error_code));
}


/* Equality */

static bool act_store_equals
(const struct sieve_script_env *senv,
	const struct sieve_action *act1, const struct sieve_action *act2)
{
	struct act_store_context *st_ctx1 = 
		( act1 == NULL ? NULL : (struct act_store_context *) act1->context );
	struct act_store_context *st_ctx2 = 
		( act2 == NULL ? NULL : (struct act_store_context *) act2->context );
	const char *mailbox1, *mailbox2;
	
	/* FIXME: consider namespace aliases */

	if ( st_ctx1 == NULL && st_ctx2 == NULL )
		return TRUE;
		
	mailbox1 = ( st_ctx1 == NULL ? 
		SIEVE_SCRIPT_DEFAULT_MAILBOX(senv) : st_ctx1->mailbox );
	mailbox2 = ( st_ctx2 == NULL ? 
		SIEVE_SCRIPT_DEFAULT_MAILBOX(senv) : st_ctx2->mailbox );
	
	if ( strcmp(mailbox1, mailbox2) == 0 ) 
		return TRUE;
		
	return 
		( strcasecmp(mailbox1, "INBOX") == 0 && strcasecmp(mailbox2, "INBOX") == 0 ); 

}

/* Result verification */

static int act_store_check_duplicate
(const struct sieve_runtime_env *renv,
	const struct sieve_action *act, 
	const struct sieve_action *act_other)
{
	return ( act_store_equals(renv->scriptenv, act, act_other) ? 1 : 0 );
}

/* Result printing */

static void act_store_print
(const struct sieve_action *action, 
	const struct sieve_result_print_env *rpenv, bool *keep)	
{
	struct act_store_context *ctx = (struct act_store_context *) action->context;
	const char *mailbox;

	mailbox = ( ctx == NULL ? 
		SIEVE_SCRIPT_DEFAULT_MAILBOX(rpenv->scriptenv) : ctx->mailbox );	

	sieve_result_action_printf(rpenv, "store message in folder: %s", 
		str_sanitize(mailbox, 128));
	
	*keep = FALSE;
}

/* Action implementation */

static struct mailbox *act_store_mailbox_open
(const struct sieve_action_exec_env *aenv, const char **mailbox,
	struct mail_namespace **ns_r, const char **folder_r)
{
	struct mail_storage **storage = &(aenv->exec_status->last_storage);
	enum mailbox_flags flags =
		MAILBOX_FLAG_KEEP_RECENT | MAILBOX_FLAG_SAVEONLY |
		MAILBOX_FLAG_POST_SESSION;
	struct mailbox *box;
	enum mail_error error;

	if (strcasecmp(*mailbox, "INBOX") == 0) {
		/* Deliveries to INBOX must always succeed, regardless of ACLs */
		flags |= MAILBOX_FLAG_IGNORE_ACLS;
	}

	*folder_r = *mailbox;
	*ns_r = mail_namespace_find(aenv->scriptenv->namespaces, folder_r);
	if ( *ns_r == NULL) {
		*storage = NULL;
		return NULL;
	}
	
	if ( **folder_r == '\0' ) {
		/* delivering to a namespace prefix means we actually want to
		 * deliver to the INBOX instead
		 */
		*folder_r = *mailbox = "INBOX";
		flags |= MAILBOX_FLAG_IGNORE_ACLS;

		*ns_r = mail_namespace_find(aenv->scriptenv->namespaces, folder_r);
		if ( *ns_r == NULL) {
			*storage = NULL;
			return NULL;
		}

		*storage = (*ns_r)->storage;
	}

	/* First attempt at opening the box */
	box = mailbox_alloc((*ns_r)->list, *folder_r, NULL, flags);
	if ( mailbox_open(box) == 0 ) {
		/* Success */
		return box;
	}

	/* Failed */

	*storage = mailbox_get_storage(box);
	(void)mail_storage_get_last_error(*storage, &error);
	
	/* Only continue when the mailbox is missing and when we are allowed to
	 * create it.
	 */
	if ( !aenv->scriptenv->mailbox_autocreate || error != MAIL_ERROR_NOTFOUND ) {
		mailbox_close(&box);
		return NULL;
	}

	/* Try creating it. */
	if ( mailbox_create(box, NULL, FALSE) < 0 ) {
		(void)mail_storage_get_last_error(*storage, &error);
		mailbox_close(&box);
		return NULL;
	}

	/* Subscribe to it if required */
	if ( aenv->scriptenv->mailbox_autosubscribe ) {
		(void)mailbox_list_set_subscribed((*ns_r)->list, *folder_r, TRUE);
	}

	/* Try opening again */
	if ( mailbox_open(box) < 0 || mailbox_sync(box, 0, 0, NULL) < 0 ) {
		/* Failed definitively */
		mailbox_close(&box);
		return NULL;
	}

	return box;
}

static bool act_store_start
(const struct sieve_action *action, 
	const struct sieve_action_exec_env *aenv, void **tr_context)
{  
	struct act_store_context *ctx = (struct act_store_context *) action->context;
	const struct sieve_script_env *senv = aenv->scriptenv;
	const struct sieve_message_data *msgdata = aenv->msgdata;
	struct act_store_transaction *trans;
	struct mail_namespace *ns = NULL;
	struct mailbox *box = NULL;
	const char *folder;
	pool_t pool = sieve_result_pool(aenv->result);
	bool disabled = FALSE, redundant = FALSE;

	/* If context is NULL, the store action is the result of (implicit) keep */	
	if ( ctx == NULL ) {
		ctx = p_new(pool, struct act_store_context, 1);
		ctx->mailbox = p_strdup(pool, SIEVE_SCRIPT_DEFAULT_MAILBOX(senv));
	}

	/* Open the requested mailbox */

	/* NOTE: The caller of the sieve library is allowed to leave namespaces set 
	 * to NULL. This implementation will then skip actually storing the message.
	 */
	if ( senv->namespaces != NULL ) {
		box = act_store_mailbox_open(aenv, &ctx->mailbox, &ns, &folder);

		/* Check whether we are trying to store the message in the folder it
		 * originates from. In that case we skip actually storing it.
		 */
		if ( box != NULL && mailbox_backends_equal(box, msgdata->mail->box) ) {
			mailbox_close(&box);
			box = NULL;
			ns = NULL;
			redundant = TRUE;
		}
	} else {
		disabled = TRUE;
	}
				
	/* Create transaction context */
	trans = p_new(pool, struct act_store_transaction, 1);

	trans->context = ctx;
	trans->namespace = ns;
	trans->folder = folder;
	trans->box = box;
	trans->flags = 0;

	trans->disabled = disabled;
	trans->redundant = redundant;

	if ( ns != NULL && box == NULL )
		sieve_act_store_get_storage_error(aenv, trans);
	
	*tr_context = (void *)trans;

	return ( (box != NULL)
		|| (trans->error_code == MAIL_ERROR_NOTFOUND)
		|| disabled || redundant );
}

static struct mail_keywords *act_store_keywords_create
(const struct sieve_action_exec_env *aenv, ARRAY_TYPE(const_string) *keywords, 
	struct mailbox *box)
{
	struct mail_keywords *box_keywords = NULL;
	
	if ( array_is_created(keywords) && array_count(keywords) > 0 ) 
	{
		const char *const *kwds;
		
		(void)array_append_space(keywords);
		kwds = array_idx(keywords, 0);
				
		/* FIXME: Do we need to clear duplicates? */
		if ( mailbox_keywords_create(box, kwds, &box_keywords) < 0) {
			sieve_result_error(aenv, "invalid keywords set for stored message");
			return NULL;
		}
	}

	return box_keywords;	
}

static bool act_store_execute
(const struct sieve_action *action ATTR_UNUSED, 
	const struct sieve_action_exec_env *aenv, void *tr_context)
{   
	struct act_store_transaction *trans = 
		(struct act_store_transaction *) tr_context;
	const struct sieve_message_data *msgdata = aenv->msgdata;
	struct mail_save_context *save_ctx;
	struct mail_keywords *keywords = NULL;
	bool result = TRUE;
	
	/* Verify transaction */
	if ( trans == NULL ) return FALSE;

	/* Check whether we need to do anything */
	if ( trans->disabled ) return TRUE;

	/* If the message originates from the target mailbox, only update the flags 
	 * and keywords 
	 */
	if ( trans->redundant ) {
		if ( trans->flags_altered ) {
			keywords = act_store_keywords_create
				(aenv, &trans->keywords, msgdata->mail->box);

			if ( keywords != NULL ) {
				mail_update_keywords(msgdata->mail, MODIFY_REPLACE, keywords);
				mailbox_keywords_unref(trans->box, &keywords);
			}

			mail_update_flags(msgdata->mail, MODIFY_REPLACE, trans->flags);
		}

		return TRUE;
	}

	/* Exit early if namespace or mailbox are not available */
	if ( trans->namespace == NULL )
		return FALSE;
	else if ( trans->box == NULL ) 
		return FALSE;

	/* Mark attempt to store in default mailbox */
	if ( strcmp(trans->context->mailbox, 
		SIEVE_SCRIPT_DEFAULT_MAILBOX(aenv->scriptenv)) == 0 ) 
		aenv->exec_status->tried_default_save = TRUE;

	/* Mark attempt to use storage. Can only get here when all previous actions
	 * succeeded. 
	 */
	aenv->exec_status->last_storage = mailbox_get_storage(trans->box);
	
	/* Start mail transaction */
	trans->mail_trans = mailbox_transaction_begin
		(trans->box, MAILBOX_TRANSACTION_FLAG_EXTERNAL);

	/* Create mail object for stored message */
	trans->dest_mail = mail_alloc(trans->mail_trans, 0, NULL);
 
	/* Store the message */
	save_ctx = mailbox_save_alloc(trans->mail_trans);
	mailbox_save_set_dest_mail(save_ctx, trans->dest_mail);

	/* Apply keywords and flags that side-effects may have added */
	if ( trans->flags_altered ) {
		keywords = act_store_keywords_create(aenv, &trans->keywords, trans->box);
		
		mailbox_save_set_flags(save_ctx, trans->flags, keywords);
	}

	if ( mailbox_copy(&save_ctx, aenv->msgdata->mail) < 0 ) {
		sieve_act_store_get_storage_error(aenv, trans);
 		result = FALSE;
 	}
 	
	/* Deallocate keywords */
 	if ( keywords != NULL ) {
 		mailbox_keywords_unref(trans->box, &keywords);
 	}
 		 	
	return result;
}

static void act_store_log_status
(struct act_store_transaction *trans, 
	const struct sieve_action_exec_env *aenv, bool rolled_back, bool status )
{
	const char *mailbox_name;
	
	mailbox_name = str_sanitize(trans->context->mailbox, 128);

	/* Store disabled? */
	if ( trans->disabled ) {
		sieve_result_log(aenv, "store into mailbox '%s' skipped", mailbox_name);
	
	/* Store redundant? */
	} else if ( trans->redundant ) {
		sieve_result_log(aenv, "left message in mailbox '%s'", mailbox_name);

	/* Namespace not set? */
	} else if ( trans->namespace == NULL ) {
		sieve_result_error
			(aenv, "failed to find namespace for mailbox '%s'", mailbox_name);

	/* Store failed? */
	} else if ( !status ) {
		const char *errstr;
		enum mail_error error;

		if ( trans->error != NULL )
			errstr = trans->error;
		else
			errstr = mail_storage_get_last_error(trans->namespace->storage, &error);
	
		sieve_result_error(aenv, "failed to store into mailbox '%s': %s", 
			mailbox_name, errstr);

	/* Store aborted? */
	} else if ( rolled_back ) {
		sieve_result_log(aenv, "store into mailbox '%s' aborted", mailbox_name);

	/* Succeeded */
	} else {
		sieve_result_log(aenv, "stored mail into mailbox '%s'", mailbox_name);

	}
}

static bool act_store_commit
(const struct sieve_action *action ATTR_UNUSED, 
	const struct sieve_action_exec_env *aenv, void *tr_context, bool *keep)
{  
	struct act_store_transaction *trans = 
		(struct act_store_transaction *) tr_context;
	bool status = TRUE;

	/* Verify transaction */
	if ( trans == NULL ) return FALSE;

	/* Check whether we need to do anything */
	if ( trans->disabled ) {
		act_store_log_status(trans, aenv, FALSE, status);
		*keep = FALSE;
		return TRUE;
	} else if ( trans->redundant ) {
		act_store_log_status(trans, aenv, FALSE, status);
		aenv->exec_status->keep_original = TRUE;
		aenv->exec_status->message_saved = TRUE;
		return TRUE;	
	}

	/* Exit early if namespace is not available */
	if ( trans->namespace == NULL )
		return FALSE;
	else if ( trans->box == NULL ) 
		return FALSE;

	/* Mark attempt to use storage. Can only get here when all previous actions
	 * succeeded. 
	 */
	aenv->exec_status->last_storage = trans->namespace->storage;

	/* Free mail object for stored message */
	if ( trans->dest_mail != NULL ) 
		mail_free(&trans->dest_mail);	

	/* Commit mailbox transaction */	
	status = ( mailbox_transaction_commit(&trans->mail_trans) == 0 );

	/* Note the fact that the message was stored at least once */
	if ( status )
		aenv->exec_status->message_saved = TRUE;
	
	/* Log our status */
	act_store_log_status(trans, aenv, FALSE, status);
	
	/* Cancel implicit keep if all went well */
	*keep = !status;
	
	/* Close mailbox */	
	if ( trans->box != NULL )
		mailbox_close(&trans->box);

	return status;
}

static void act_store_rollback
(const struct sieve_action *action ATTR_UNUSED, 
	const struct sieve_action_exec_env *aenv, void *tr_context, bool success)
{
	struct act_store_transaction *trans = 
		(struct act_store_transaction *) tr_context;

	/* Log status */
	act_store_log_status(trans, aenv, TRUE, success);

	/* Free mailobject for stored message */
	if ( trans->dest_mail != NULL ) 
		mail_free(&trans->dest_mail);	

	/* Rollback mailbox transaction */
	if ( trans->mail_trans != NULL )
		mailbox_transaction_rollback(&trans->mail_trans);
  
	/* Close the mailbox */
	if ( trans->box != NULL )  
		mailbox_close(&trans->box);
}

/*
 * Action utility functions
 */

bool sieve_action_duplicate_check_available
(const struct sieve_script_env *senv)
{
	return ( senv->duplicate_check != NULL && senv->duplicate_mark != NULL );
}

int sieve_action_duplicate_check
(const struct sieve_script_env *senv, const void *id, size_t id_size)
{
	if ( senv->duplicate_check == NULL || senv->duplicate_mark == NULL)
		return 0;

	return senv->duplicate_check
		(id, id_size, senv->username); 
}

void sieve_action_duplicate_mark
(const struct sieve_script_env *senv, const void *id, size_t id_size,
	time_t time)
{
	if ( senv->duplicate_check == NULL || senv->duplicate_mark == NULL)
		return;

	senv->duplicate_mark
		(id, id_size, senv->username, time);
}
	

