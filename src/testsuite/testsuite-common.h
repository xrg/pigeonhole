/* Copyright (c) 2002-2009 Dovecot Sieve authors, see the included COPYING file
 */

#ifndef __TESTSUITE_COMMON_H
#define __TESTSUITE_COMMON_H

#include "sieve-common.h"

/*
 * Extension
 */

extern const struct sieve_extension testsuite_extension;

/* 
 * Validator context 
 */

struct testsuite_validator_context {
	struct sieve_validator_object_registry *object_registrations;
};

bool testsuite_validator_context_initialize(struct sieve_validator *valdtr);
struct testsuite_validator_context *testsuite_validator_context_get
	(struct sieve_validator *valdtr);

/* 
 * Generator context 
 */

struct testsuite_generator_context {
	struct sieve_jumplist *exit_jumps;
};

bool testsuite_generator_context_initialize(struct sieve_generator *gentr);

/*
 * Commands
 */

extern const struct sieve_command cmd_test;
extern const struct sieve_command cmd_test_fail;
extern const struct sieve_command cmd_test_set;
extern const struct sieve_command cmd_test_result_print;

/*
 * Tests
 */

extern const struct sieve_command tst_test_script_compile;
extern const struct sieve_command tst_test_script_run;
extern const struct sieve_command tst_test_error;
extern const struct sieve_command tst_test_result;
extern const struct sieve_command tst_test_result_execute;

/* 
 * Operations 
 */

enum testsuite_operation_code {
	TESTSUITE_OPERATION_TEST,
	TESTSUITE_OPERATION_TEST_FINISH,
	TESTSUITE_OPERATION_TEST_FAIL,
	TESTSUITE_OPERATION_TEST_SET,
	TESTSUITE_OPERATION_TEST_SCRIPT_COMPILE,
	TESTSUITE_OPERATION_TEST_SCRIPT_RUN,
	TESTSUITE_OPERATION_TEST_ERROR,
	TESTSUITE_OPERATION_TEST_RESULT,
	TESTSUITE_OPERATION_TEST_RESULT_EXECUTE,
	TESTSUITE_OPERATION_TEST_RESULT_PRINT,
};

extern const struct sieve_operation test_operation;
extern const struct sieve_operation test_finish_operation;
extern const struct sieve_operation test_fail_operation;
extern const struct sieve_operation test_set_operation;
extern const struct sieve_operation test_script_compile_operation;
extern const struct sieve_operation test_script_run_operation;
extern const struct sieve_operation test_error_operation;
extern const struct sieve_operation test_result_operation;
extern const struct sieve_operation test_result_execute_operation;
extern const struct sieve_operation test_result_print_operation;

/* 
 * Operands 
 */

extern const struct sieve_operand testsuite_object_operand;
extern const struct sieve_operand testsuite_substitution_operand;

enum testsuite_operand_code {
	TESTSUITE_OPERAND_OBJECT,
	TESTSUITE_OPERAND_SUBSTITUTION
};

/* 
 * Test context 
 */

void testsuite_test_start(string_t *name);
void testsuite_test_fail(string_t *reason);
void testsuite_test_succeed(string_t *reason);

void testsuite_testcase_fail(const char *reason);
int testsuite_testcase_result(void);

/* 
 * Tested script environment 
 */

bool testsuite_script_compile(const char *script_path);
bool testsuite_script_run(const struct sieve_runtime_env *renv);

void testsuite_script_clear_messages(void);
void testsuite_script_get_error_init(void);
const char *testsuite_script_get_error_next(bool location);

/* 
 * Testsuite init/deinit 
 */

void testsuite_init(void);
void testsuite_deinit(void);

#endif /* __TESTSUITE_COMMON_H */
