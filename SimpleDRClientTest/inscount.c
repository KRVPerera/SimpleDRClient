/* ******************************************************************************
* Copyright (c) 2014-2016 Google, Inc.  All rights reserved.
* Copyright (c) 2011 Massachusetts Institute of Technology  All rights reserved.
* Copyright (c) 2008 VMware, Inc.  All rights reserved.
* ******************************************************************************/

/*
* Redistribution and use in source and binary forms, with or without
* modification, are permitted provided that the following conditions are met:
*
* * Redistributions of source code must retain the above copyright notice,
*   this list of conditions and the following disclaimer.
*
* * Redistributions in binary form must reproduce the above copyright notice,
*   this list of conditions and the following disclaimer in the documentation
*   and/or other materials provided with the distribution.
*
* * Neither the name of VMware, Inc. nor the names of its contributors may be
*   used to endorse or promote products derived from this software without
*   specific prior written permission.
*
* THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
* AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
* IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
* ARE DISCLAIMED. IN NO EVENT SHALL VMWARE, INC. OR CONTRIBUTORS BE LIABLE
* FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
* DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
* SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
* CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
* LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
* OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH
* DAMAGE.
*/

/* Code Manipulation API Sample:
* inscount.c
*
* Reports the dynamic count of the total number of instructions executed.
* Illustrates how to perform performant clean calls.
* Demonstrates effect of clean call optimization and auto-inlining with
* different -opt_cleancall values.
*
* The runtime options for this client include:
* -only_from_app  Do not count instructions in shared libraries
*/
#define WINDOWS
#define X86_64
#define SHOW_RESULTS
#include <stddef.h>
#include <stdio.h>
#include "dr_api.h"
#include "drmgr.h"
#include <string.h>

// Helium Specific
//#include "dr_defines.h"
#include "defines.h"
//#include "dr_ir_instr.h"

#ifdef WINDOWS
# define DISPLAY_STRING(msg) dr_messagebox(msg)
#else
# define DISPLAY_STRING(msg) dr_printf("%s\n", msg);
#endif

#define NULL_TERMINATE(buf) buf[(sizeof(buf)/sizeof(buf[0])) - 1] = '\0'

/* Runtime option: If set, only count instructions in the application itself */
static bool only_from_app = false;
/* Application module */
static app_pc exe_start;
/* we only have a global count */
static uint64 global_count;
/* A simple clean call that will be automatically inlined because it has only
* one argument and contains no calls to other functions.
*/
static void inscount(uint num_instrs) { global_count += num_instrs; }
static void event_exit(void);
static dr_emit_flags_t event_bb_analysis(void *drcontext, void *tag,
	instrlist_t *bb,
	bool for_trace, bool translating,
	void **user_data);
static dr_emit_flags_t event_app_instruction(void *drcontext, void *tag,
	instrlist_t *bb, instr_t *inst,
	bool for_trace, bool translating,
	void *user_data);

// Integrating Helium clients into the simple client
#define ARGUMENT_LENGTH 20

typedef void(*thread_func_t) (void * drcontext);
typedef void(*init_func_t) (client_id_t id, const char * name, const char * arguments);
typedef void(*exit_func_t) (void);
typedef void(*module_load_t) (void * drcontext, const module_data_t * info, bool loaded);
typedef void(*module_unload_t) (void * drcontext, const module_data_t * info);

typedef struct _cmdarguments_t {

	char name[MAX_STRING_LENGTH];
	char arguments[10 * MAX_STRING_LENGTH];

} cmdarguments_t;

typedef struct _instrumentation_pass_t {

	char * name;
	init_func_t init_func;
	drmgr_analysis_cb_t analysis_bb;
	drmgr_insertion_cb_t instrumentation_bb;
	drmgr_xform_cb_t app2app_bb;
	drmgr_priority_t priority;
	thread_func_t thread_init;
	thread_func_t thread_exit;
	exit_func_t process_exit;
	module_load_t module_load;
	module_unload_t module_unload;


} instrumentation_pass_t;

//allocate statically enough space
static cmdarguments_t arguments[ARGUMENT_LENGTH];
static instrumentation_pass_t ins_pass[ARGUMENT_LENGTH];
static int argument_length = 0;

DR_EXPORT void
dr_client_main(client_id_t id, int argc, const char *argv[])
{
	int i;
	dr_set_client_name("DynamoRIO Sample Client 'inscount'",
		"http://dynamorio.org/issues");

	/* Options */
	for (i = 1/*skip client*/; i < argc; i++) {
		if (strcmp(argv[i], "-only_from_app") == 0) {
			only_from_app = true;
		}
		else {
			dr_fprintf(STDERR, "UNRECOGNIZED OPTION: \"%s\"\n", argv[i]);
			DR_ASSERT_MSG(false, "invalid option");
		}
	}


	drmgr_init();

	/* Get main module address */
	if (only_from_app) {
		module_data_t *exe = dr_get_main_module();
		if (exe != NULL)
			dr_fprintf(STDERR, "Application : \"%s\"\n", exe->names.file_name);
			exe_start = exe->start;
		dr_free_module_data(exe);
	}

	/* register events */
	dr_register_exit_event(event_exit);
	drmgr_register_bb_instrumentation_event(event_bb_analysis,
		event_app_instruction,
		NULL);

	/* make it easy to tell, by looking at log file, which client executed */
	dr_log(NULL, LOG_ALL, 1, "Client 'inscount' initializing\n");
#ifdef SHOW_RESULTS
	/* also give notification to stderr */
	if (dr_is_notify_on()) {
# ifdef WINDOWS
		/* ask for best-effort printing to cmd window.  must be called at init. */
		dr_enable_console_printing();
# endif
		dr_fprintf(STDERR, "Client inscount is running\n");
	}
#endif
}

static dr_emit_flags_t
event_bb_analysis(void *drcontext, void *tag, instrlist_t *bb,
bool for_trace, bool translating, void **user_data)
{
	instr_t *instr;
	uint num_instrs;

#ifdef VERBOSE
	dr_printf("in dynamorio_basic_block(tag="PFX")\n", tag);
# ifdef VERBOSE_VERBOSE
	instrlist_disassemble(drcontext, tag, bb, STDOUT);
# endif
#endif
	/* Only count in app BBs */
	if (only_from_app) {
		module_data_t *mod = dr_lookup_module(dr_fragment_app_pc(tag));
		if (mod != NULL) {
			bool from_exe = (mod->start == exe_start);
			dr_free_module_data(mod);
			if (!from_exe) {
				*user_data = NULL;
				return DR_EMIT_DEFAULT;
			}
		}
	}
	/* Count instructions */
	for (instr = instrlist_first_app(bb), num_instrs = 0;
		instr != NULL;
		instr = instr_get_next_app(instr)) {
		num_instrs++;
	}
	*user_data = (void *)(ptr_uint_t)num_instrs;

#if defined(VERBOSE) && defined(VERBOSE_VERBOSE)
	dr_printf("Finished counting for dynamorio_basic_block(tag="PFX")\n", tag);
	instrlist_disassemble(drcontext, tag, bb, STDOUT);
#endif
	return DR_EMIT_DEFAULT;
}

static dr_emit_flags_t
event_app_instruction(void *drcontext, void *tag, instrlist_t *bb, instr_t *instr,
bool for_trace, bool translating, void *user_data)
{
	uint num_instrs;
	if (!drmgr_is_first_instr(drcontext, instr))
		return DR_EMIT_DEFAULT;
	/* Only insert calls for in-app BBs */
	if (user_data == NULL)
		return DR_EMIT_DEFAULT;
	/* Insert clean call */
	num_instrs = (uint)(ptr_uint_t)user_data;
	dr_insert_clean_call(drcontext, bb, instrlist_first_app(bb),
		(void *)inscount, false /* save fpstate */, 1,
		OPND_CREATE_INT32(num_instrs));
	return DR_EMIT_DEFAULT;
}


static void
event_exit(void)
{
#ifdef SHOW_RESULTS
	char msg[512];
	int len;
	len = dr_snprintf(msg, sizeof(msg) / sizeof(msg[0]),
		"Instrumentation results: %llu instructions executed\n",
		global_count);
	DR_ASSERT(len > 0);
	NULL_TERMINATE(msg);
	DISPLAY_STRING(msg);
#endif /* SHOW_RESULTS */
	drmgr_exit();
}

static void doCommandLineArgProcessing(client_id_t id){

	const char * args = dr_get_options(id);

	int i = 0;

	int string_index = 0;
	int index = -1;
	int name_collect_state = 0;
	int arguments_collect_state = 0;

	while (args[i] != '\0'){
		if (args[i] == '-'){
			name_collect_state = 1;
			arguments_collect_state = 0;
			if (index >= 0){
				arguments[index].arguments[string_index - 1] = '\0';
			}
			index++;
			string_index = 0;
			i++;
			continue;
		}

		if (args[i] == ' ' && name_collect_state){
			arguments[index].name[string_index++] = '\0';
			name_collect_state = 0;
			arguments_collect_state = 1;
			string_index = 0;
			i++;
			continue;
		}


		if (name_collect_state){
			arguments[index].name[string_index++] = args[i];
		}
		if (arguments_collect_state){
			arguments[index].arguments[string_index++] = args[i];
		}

		i++;
	}

	//epilog
	arguments[index].arguments[string_index++] = '\0';
	argument_length = index + 1;


	process_global_arguments();

}