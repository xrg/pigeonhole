#include <stdio.h>

#include "lib.h"
#include "compat.h"
#include "mempool.h"
#include "hash.h"
#include "array.h"

#include "sieve-extensions.h"
#include "sieve-commands.h"
#include "sieve-code.h"
#include "sieve-binary.h"
#include "sieve-comparators.h"
#include "sieve-validator.h"
#include "sieve-generator.h"
#include "sieve-interpreter.h"

#include "sieve-match-types.h"

#include <string.h>

/* 
 * Predeclarations 
 */
 
static void opr_match_type_emit
	(struct sieve_binary *sbin, const struct sieve_match_type *mtch);
static void opr_match_type_emit_ext
	(struct sieve_binary *sbin, const struct sieve_match_type *mtch, int ext_id);

/* 
 * Match-type 'extension' 
 */

static int ext_my_id = -1;

static bool mtch_extension_load(int ext_id);
static bool mtch_validator_load(struct sieve_validator *validator);
static bool mtch_interpreter_load(struct sieve_interpreter *interp);

const struct sieve_extension match_type_extension = {
	"@match-type",
	mtch_extension_load,
	mtch_validator_load,
	NULL,
	mtch_interpreter_load,
	NULL,
	NULL
};
	
static bool mtch_extension_load(int ext_id) 
{
	ext_my_id = ext_id;
	return TRUE;
}

/* 
 * Validator context:
 *   name-based match-type registry. 
 *
 * FIXME: This code will be duplicated across all extensions that introduce 
 * a registry of some kind in the validator. 
 */
 
struct mtch_validator_registration {
	int ext_id;
	const struct sieve_match_type *match_type;
};
 
struct mtch_validator_context {
	struct hash_table *registrations;
};

static inline struct mtch_validator_context *
	get_validator_context(struct sieve_validator *validator)
{
	return (struct mtch_validator_context *) 
		sieve_validator_extension_get_context(validator, ext_my_id);
}

static void _sieve_match_type_register
	(pool_t pool, struct mtch_validator_context *ctx, 
	const struct sieve_match_type *mtch, int ext_id) 
{
	struct mtch_validator_registration *reg;
	
	reg = p_new(pool, struct mtch_validator_registration, 1);
	reg->match_type = mtch;
	reg->ext_id = ext_id;
	
	hash_insert(ctx->registrations, (void *) mtch->identifier, (void *) reg);
}
 
void sieve_match_type_register
	(struct sieve_validator *validator, 
	const struct sieve_match_type *mtch, int ext_id) 
{
	pool_t pool = sieve_validator_pool(validator);
	struct mtch_validator_context *ctx = get_validator_context(validator);

	_sieve_match_type_register(pool, ctx, mtch, ext_id);
}

const struct sieve_match_type *sieve_match_type_find
	(struct sieve_validator *validator, const char *identifier,
		int *ext_id) 
{
	struct mtch_validator_context *ctx = get_validator_context(validator);
	struct mtch_validator_registration *reg =
		(struct mtch_validator_registration *) 
			hash_lookup(ctx->registrations, identifier);
			
	if ( reg == NULL ) return NULL;

	if ( ext_id != NULL ) *ext_id = reg->ext_id;

  return reg->match_type;
}

bool mtch_validator_load(struct sieve_validator *validator)
{
	unsigned int i;
	pool_t pool = sieve_validator_pool(validator);
	
	struct mtch_validator_context *ctx = 
		p_new(pool, struct mtch_validator_context, 1);
	
	/* Setup match-type registry */
	ctx->registrations = hash_create
		(pool, pool, 0, str_hash, (hash_cmp_callback_t *)strcmp);

	/* Register core match-types */
	for ( i = 0; i < sieve_core_match_types_count; i++ ) {
		const struct sieve_match_type *mtch = sieve_core_match_types[i];
		
		_sieve_match_type_register(pool, ctx, mtch, -1);
	}

	sieve_validator_extension_set_context(validator, ext_my_id, ctx);

	return TRUE;
}

void sieve_match_types_link_tags
	(struct sieve_validator *validator, 
		struct sieve_command_registration *cmd_reg, unsigned int id_code) 
{	
	sieve_validator_register_tag
		(validator, cmd_reg, &match_type_tag, id_code); 	
}

/*
 * Interpreter context:
 *
 * FIXME: This code will be duplicated across all extensions that introduce 
 * a registry of some kind in the interpreter. 
 */

struct mtch_interpreter_context {
	ARRAY_DEFINE(mtch_extensions, 
		const struct sieve_match_type_extension *); 
};

static inline struct mtch_interpreter_context *
	get_interpreter_context(struct sieve_interpreter *interpreter)
{
	return (struct mtch_interpreter_context *) 
		sieve_interpreter_extension_get_context(interpreter, ext_my_id);
}

static const struct sieve_match_type_extension *sieve_match_type_extension_get
	(struct sieve_interpreter *interpreter, int ext_id)
{
	struct mtch_interpreter_context *ctx = get_interpreter_context(interpreter);
	
	if ( (ctx != NULL) && (ext_id > 0) && (ext_id < (int) array_count(&ctx->mtch_extensions)) ) {
		const struct sieve_match_type_extension * const *ext;

		ext = array_idx(&ctx->mtch_extensions, (unsigned int) ext_id);

		return *ext;
	}
	
	return NULL;
}

void sieve_match_type_extension_set
	(struct sieve_interpreter *interpreter, int ext_id,
		const struct sieve_match_type_extension *ext)
{
	struct mtch_interpreter_context *ctx = get_interpreter_context(interpreter);

	array_idx_set(&ctx->mtch_extensions, (unsigned int) ext_id, &ext);
}

static bool mtch_interpreter_load(struct sieve_interpreter *interpreter)
{
	pool_t pool = sieve_interpreter_pool(interpreter);
	
	struct mtch_interpreter_context *ctx = 
		p_new(pool, struct mtch_interpreter_context, 1);
	
	/* Setup comparator registry */
	p_array_init(&ctx->mtch_extensions, pool, 4);

	sieve_interpreter_extension_set_context(interpreter, ext_my_id, ctx);
	
	return TRUE;
}

/*
 * Address-part operand
 */
 
struct sieve_operand_class match_type_class = 
	{ "match-type", NULL };
struct sieve_operand match_type_operand = 
	{ "match-type", &match_type_class, FALSE };

/* 
 * Address-part tag 
 */
  
static bool tag_match_type_is_instance_of
	(struct sieve_validator *validator, const char *tag)
{
	return sieve_match_type_find(validator, tag, NULL) != NULL;
}
 
static bool tag_match_type_validate
	(struct sieve_validator *validator, 
	struct sieve_ast_argument **arg, 
	struct sieve_command_context *cmd)
{
	int ext_id;
	struct sieve_match_type_context *mtctx;
	const struct sieve_match_type *mtch;

	/* Syntax:   
	 *   ":is" / ":contains" / ":matches" (subject to extension)
   	 */
	
	/* Get match_type from registry */
	mtch = sieve_match_type_find
		(validator, sieve_ast_argument_tag(*arg), &ext_id);
	
	/* In theory, mtch can never be NULL, because we must have found it earlier
	 * to get here.
	 */
	if ( mtch == NULL ) {
		sieve_command_validate_error(validator, cmd, 
			"unknown match-type modifier '%s' "
			"(this error should not occur and is probably a bug)", 
			sieve_ast_argument_strc(*arg));

		return FALSE;	}

	/* Create context */
	mtctx = p_new(sieve_command_pool(cmd), struct sieve_match_type_context, 1);
	mtctx->match_type = mtch;
	mtctx->command_ctx = cmd;
	
	(*arg)->context = (void *) mtctx;
	(*arg)->ext_id = ext_id;
	
	/* Skip tag */
	*arg = sieve_ast_argument_next(*arg);
	
	/* Check whether this match type requires additional validation. 
	 * Additional validation can override the match type recorded in the context 
	 * for later code generation. 
	 */
	if ( mtch->validate != NULL ) {
		return mtch->validate(validator, arg, mtctx);
	}
	
	return TRUE;
}

bool sieve_match_type_validate_argument
(struct sieve_validator *validator, struct sieve_ast_argument *arg,
	struct sieve_ast_argument *key_arg )
{
	struct sieve_match_type_context *mtctx = 
		(struct sieve_match_type_context *) arg->context;

	i_assert(arg->argument == &match_type_tag);

	/* Check whether this match type requires additional validation. 
	 * Additional validation can override the match type recorded in the context 
	 * for later code generation. 
	 */
	if ( mtctx != NULL && mtctx->match_type != NULL &&
		mtctx->match_type->validate_context != NULL ) {
		return mtctx->match_type->validate_context(validator, arg, mtctx, key_arg);
	}
	
	return TRUE;
}

bool sieve_match_type_validate
(struct sieve_validator *validator, struct sieve_command_context *cmd,
	struct sieve_ast_argument *key_arg )
{
	struct sieve_ast_argument *arg = sieve_command_first_argument(cmd);

	while ( arg != NULL && arg != cmd->first_positional ) {
		if ( arg->argument == &match_type_tag ) {
			if ( !sieve_match_type_validate_argument(validator, arg, key_arg) ) 
				return FALSE;
		}
		arg = sieve_ast_argument_next(arg);
	}

	return TRUE;	
}

/* Code generation */

static void opr_match_type_emit
	(struct sieve_binary *sbin, const struct sieve_match_type *mtch)
{ 
	(void) sieve_operand_emit_code(sbin, SIEVE_OPERAND_MATCH_TYPE);
	(void) sieve_binary_emit_byte(sbin, mtch->code);
}

static void opr_match_type_emit_ext
	(struct sieve_binary *sbin, const struct sieve_match_type *mtch, int ext_id)
{ 
	unsigned char mtch_code = SIEVE_MATCH_TYPE_CUSTOM + 
		sieve_binary_extension_get_index(sbin, ext_id);
	
	(void) sieve_operand_emit_code(sbin, SIEVE_OPERAND_MATCH_TYPE);	
	(void) sieve_binary_emit_byte(sbin, mtch_code);
	if ( mtch->extension->match_type == NULL )
		(void) sieve_binary_emit_byte(sbin, mtch->ext_code);
}

const struct sieve_match_type *sieve_opr_match_type_read
  (struct sieve_interpreter *interpreter, 
		struct sieve_binary *sbin, sieve_size_t *address)
{
	unsigned int mtch_code;
	const struct sieve_operand *operand = sieve_operand_read(sbin, address);
	
	if ( operand == NULL || operand->class != &match_type_class ) 
		return NULL;
	
	if ( sieve_binary_read_byte(sbin, address, &mtch_code) ) {
		if ( mtch_code < SIEVE_MATCH_TYPE_CUSTOM ) {
			if ( mtch_code < sieve_core_match_types_count )
				return sieve_core_match_types[mtch_code];
			else
				return NULL;
		} else {
			int ext_id = -1;
			const struct sieve_match_type_extension *mtch_ext;

			if ( sieve_binary_extension_get_by_index(sbin,
				mtch_code - SIEVE_MATCH_TYPE_CUSTOM, &ext_id) == NULL )
				return NULL; 

			mtch_ext = sieve_match_type_extension_get(interpreter, ext_id); 
 
			if ( mtch_ext != NULL ) {  	
				unsigned int code;
				if ( mtch_ext->match_type != NULL )
					return mtch_ext->match_type;
		  	
				if ( sieve_binary_read_byte(sbin, address, &code) &&
					mtch_ext->get_match != NULL )
					return mtch_ext->get_match(code);
			} else {
				i_info("Unknown match-type modifier %d.", mtch_code); 
			}
		}
	}		
		
	return NULL; 
}

bool sieve_opr_match_type_dump
	(struct sieve_interpreter *interpreter,
		struct sieve_binary *sbin, sieve_size_t *address)
{
	sieve_size_t pc = *address;
	const struct sieve_match_type *mtch = 
		sieve_opr_match_type_read(interpreter, sbin, address);
	
	if ( mtch == NULL )
		return FALSE;
		
	printf("%08x:   MATCH-TYPE: %s\n", pc, mtch->identifier);
	
	return TRUE;
}

static bool tag_match_type_generate
	(struct sieve_generator *generator, struct sieve_ast_argument *arg, 
	struct sieve_command_context *cmd ATTR_UNUSED)
{
	struct sieve_binary *sbin = sieve_generator_get_binary(generator);
	struct sieve_match_type_context *mtctx =
		(struct sieve_match_type_context *) arg->context;
	
	if ( mtctx->match_type->extension == NULL ) {
		if ( mtctx->match_type->code < SIEVE_MATCH_TYPE_CUSTOM )
			opr_match_type_emit(sbin, mtctx->match_type);
		else
			return FALSE;
	} else {
		opr_match_type_emit_ext(sbin, mtctx->match_type, arg->ext_id);
	} 
			
	return TRUE;
}

/* Match Utility */

struct sieve_match_context *sieve_match_begin
(const struct sieve_match_type *mtch, const struct sieve_comparator *cmp, 
	struct sieve_coded_stringlist *key_list)
{
	struct sieve_match_context *mctx = 
		p_new(pool_datastack_create(), struct sieve_match_context, 1);  

	mctx->match_type = mtch;
	mctx->comparator = cmp;
	mctx->key_list = key_list;

	if ( mtch->match_init != NULL ) {
		mtch->match_init(mctx);
	}

	return mctx;
}

bool sieve_match_value
	(struct sieve_match_context *mctx, const char *value)
{
	const struct sieve_match_type *mtch = mctx->match_type;
	unsigned int key_index;
	string_t *key_item;
	sieve_coded_stringlist_reset(mctx->key_list);
				
	/* Reject unimplemented match-type */
	if ( mtch->match == NULL )
		return FALSE;
				
	/* Match to all key values */
	key_index = 0;
	key_item = NULL;
	while ( sieve_coded_stringlist_next_item(mctx->key_list, &key_item) && 
		key_item != NULL ) 
	{
		if ( mtch->match
			(mctx, value, strlen(value), str_c(key_item), 
				str_len(key_item), key_index) )
			return TRUE;  
	
		key_index++;
	}

	return FALSE;
}

bool sieve_match_end(struct sieve_match_context *mctx)
{
	const struct sieve_match_type *mtch = mctx->match_type;

	if ( mtch->match_deinit != NULL ) {
		return mtch->match_deinit(mctx);
	}

	return FALSE;
}

/*
 * Matching
 */

static bool mtch_is_match
(struct sieve_match_context *mctx ATTR_UNUSED, 
	const char *val1, size_t val1_size, 
	const char *key, size_t key_size, int key_index ATTR_UNUSED)
{
	if ( mctx->comparator->compare != NULL )
		return (mctx->comparator->compare(mctx->comparator, 
			val1, val1_size, key, key_size) == 0);

	return FALSE;
}

/* :contains */

static bool mtch_contains_validate_context
(struct sieve_validator *validator, struct sieve_ast_argument *arg,
    struct sieve_match_type_context *ctx, 
	struct sieve_ast_argument *key_arg ATTR_UNUSED)
{
	struct sieve_ast_argument *carg =
		sieve_command_first_argument(ctx->command_ctx);

    while ( carg != NULL && carg != ctx->command_ctx->first_positional ) {
		if ( carg != arg && carg->argument == &comparator_tag ) {
			const struct sieve_comparator *cmp =
				sieve_comparator_tag_get(carg);
			
			if ( (cmp->flags & SIEVE_COMPARATOR_FLAG_SUBSTRING_MATCH) == 0 ) {
                sieve_command_validate_error(validator, ctx->command_ctx,
                    "the specified %s comparator does not support "
					"sub-string matching as required by the :%s match type",
					cmp->identifier, ctx->match_type->identifier );

				return FALSE;
			}
            return TRUE;
		}

		carg = sieve_ast_argument_next(carg);
	}

	return TRUE;
}

/* FIXME: Naive substring match implementation. Should switch to more 
 * efficient algorithm if large values need to be searched (e.g. message body).
 */
static bool mtch_contains_match
(struct sieve_match_context *mctx, const char *val, size_t val_size, 
	const char *key, size_t key_size, int key_index ATTR_UNUSED)
{
	const char *vend = (const char *) val + val_size;
	const char *kend = (const char *) key + key_size;
	const char *vp = val;
	const char *kp = key;

	if ( mctx->comparator->char_match == NULL ) 
		return FALSE;

	while ( (vp < vend) && (kp < kend) ) {
		if ( !mctx->comparator->char_match(mctx->comparator, &vp, vend, &kp, kend) ) {
			vp = vp - (kp - key) + 1;
			kp = key;
		}
	}
    
  return (kp == kend);
}

static bool mtch_matches_match
(struct sieve_match_context *mctx, const char *val, size_t val_size, 
	const char *key, size_t key_size, int key_index ATTR_UNUSED)
{
	return FALSE;
}
			 
/* 
 * Core match-type modifiers
 */

const struct sieve_argument match_type_tag = { 
	"MATCH-TYPE",
	tag_match_type_is_instance_of, 
	tag_match_type_validate, 
	NULL,
	tag_match_type_generate 
};
 
const struct sieve_match_type is_match_type = {
	"is",
	SIEVE_MATCH_TYPE_IS,
	NULL,
	0,
	NULL, NULL, NULL,
	mtch_is_match,
	NULL
};

const struct sieve_match_type contains_match_type = {
	"contains",
	SIEVE_MATCH_TYPE_CONTAINS,
	NULL,
	0,
	NULL,
	mtch_contains_validate_context, 
	NULL,
	mtch_contains_match,
	NULL
};

const struct sieve_match_type matches_match_type = {
	"matches",
	SIEVE_MATCH_TYPE_MATCHES,
	NULL,
	0,
	NULL,
	mtch_contains_validate_context, 
	NULL,
	mtch_matches_match,
	NULL
};

const struct sieve_match_type *sieve_core_match_types[] = {
	&is_match_type, &contains_match_type, &matches_match_type
};

const unsigned int sieve_core_match_types_count = 
	N_ELEMENTS(sieve_core_match_types);
