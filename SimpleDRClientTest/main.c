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
//#define WINDOWS
//#define X86_64
//#define SHOW_RESULTS
#include <stddef.h>
#include <stdio.h>
#include "dr_api.h"
#include "drmgr.h"
#include <string.h>

// Helium Specific
//#include "dr_defines.h"
#include "defines.h"
#include "drwrap.h"
#include "moduleinfo.h"
#include "profile_global.h"
#include "cpuid.h"
#include "memtrace.h"
#include "inscount.h"
#include "instrace.h"
#include "funcwrap.h"
#include "memdump.h"
#include "funcreplace.h"
#include "misc.h"
//#include "dr_ir_instr.h"
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
static int pass_length = 0;

char logdir[MAX_STRING_LENGTH];
bool debug_mode = false;
bool log_mode = false;
file_t global_logfile;

static char global_logfilename[MAX_STRING_LENGTH];
static char exec[MAX_STRING_LENGTH];

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

void process_global_arguments(){

	int i = 0;

	for (i = 0; i < argument_length; i++){
		if (strcmp(arguments[i].name, "logdir") == 0){
			dr_printf("global logdir - %s\n", arguments[i].arguments);
			strncpy(logdir, arguments[i].arguments, MAX_STRING_LENGTH);
		}
		else if (strcmp(arguments[i].name, "debug") == 0){
			dr_printf("global debug - %s\n", arguments[i].arguments);
			debug_mode = arguments[i].arguments[0] - '0';
		}
		else if (strcmp(arguments[i].name, "log") == 0){
			dr_printf("global log - %s\n", arguments[i].arguments);
			log_mode = arguments[i].arguments[0] - '0';
		}
		else if (strcmp(arguments[i].name, "exec") == 0){
			dr_printf("exec - %s\n", arguments[i].arguments);
			strncpy(exec, arguments[i].arguments, MAX_STRING_LENGTH);
		}
	}
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

//this function is responsible for setting up priorities and the order of the instrumentation passes
static void setupInsPasses(){

	//priority structure template
	drmgr_priority_t priority = {
		sizeof(priority), /* size of struct */
		"default",       /* name of our operation */
		DRMGR_PRIORITY_NAME_DRWRAP,             /* optional name of operation we should precede */
		NULL,             /* optional name of operation we should follow */
		0 };


	//ins pass 1 - bbinfo - most of the time should be executed first
	ins_pass[0].name = "profile";
	ins_pass[0].priority = priority;
	ins_pass[0].priority.name = ins_pass[0].name;
	ins_pass[0].priority.priority = 1;
	ins_pass[0].init_func = bbinfo_init;
	ins_pass[0].app2app_bb = bbinfo_bb_app2app;
	ins_pass[0].analysis_bb = bbinfo_bb_analysis;
	ins_pass[0].instrumentation_bb = bbinfo_bb_instrumentation;
	ins_pass[0].thread_init = bbinfo_thread_init;
	ins_pass[0].thread_exit = bbinfo_thread_exit;
	ins_pass[0].process_exit = bbinfo_exit_event;
	ins_pass[0].module_load = NULL;
	ins_pass[0].module_unload = NULL;


	//ins pass 2 - cpuid
	ins_pass[1].name = "cpuid";
	ins_pass[1].priority = priority;
	ins_pass[1].priority.name = ins_pass[1].name;
	ins_pass[1].priority.priority = 3;
	ins_pass[1].init_func = cpuid_init;
	ins_pass[1].app2app_bb = cpuid_bb_app2app;
	ins_pass[1].analysis_bb = cpuid_bb_analysis;
	ins_pass[1].instrumentation_bb = cpuid_bb_instrumentation;
	ins_pass[1].thread_init = cpuid_thread_init;
	ins_pass[1].thread_exit = cpuid_thread_exit;
	ins_pass[1].process_exit = cpuid_exit_event;
	ins_pass[1].module_load = NULL;
	ins_pass[1].module_unload = NULL;

	//ins pass 3 - memtrace
	ins_pass[2].name = "memtrace";
	ins_pass[2].priority = priority;
	ins_pass[2].priority.name = ins_pass[2].name;
	ins_pass[2].priority.priority = 3;
	ins_pass[2].init_func = memtrace_init;
	ins_pass[2].app2app_bb = memtrace_bb_app2app;
	ins_pass[2].analysis_bb = memtrace_bb_analysis;
	ins_pass[2].instrumentation_bb = memtrace_bb_instrumentation;
	ins_pass[2].thread_init = memtrace_thread_init;
	ins_pass[2].thread_exit = memtrace_thread_exit;
	ins_pass[2].process_exit = memtrace_exit_event;
	ins_pass[2].module_load = NULL;
	ins_pass[2].module_unload = NULL;


	//ins pass 4 - inscount
	ins_pass[3].name = "inscount";
	ins_pass[3].priority = priority;
	ins_pass[3].priority.name = ins_pass[3].name;
	ins_pass[3].priority.priority = 3;
	ins_pass[3].init_func = inscount_init;
	ins_pass[3].app2app_bb = NULL;
	ins_pass[3].analysis_bb = inscount_bb_analysis;
	ins_pass[3].instrumentation_bb = inscount_bb_instrumentation;
	ins_pass[3].thread_init = NULL;
	ins_pass[3].thread_exit = NULL;
	ins_pass[3].process_exit = inscount_exit_event;
	ins_pass[3].module_load = NULL;
	ins_pass[3].module_unload = NULL;

	//ins pass 5 - instrace
	ins_pass[4].name = "instrace";
	ins_pass[4].priority = priority;
	ins_pass[4].priority.name = ins_pass[4].name;
	ins_pass[4].priority.priority = 3;
	ins_pass[4].init_func = instrace_init;
	ins_pass[4].app2app_bb = instrace_bb_app2app;
	ins_pass[4].analysis_bb = instrace_bb_analysis;
	ins_pass[4].instrumentation_bb = instrace_bb_instrumentation;
	ins_pass[4].thread_init = instrace_thread_init;
	ins_pass[4].thread_exit = instrace_thread_exit;
	ins_pass[4].process_exit = instrace_exit_event;
	ins_pass[4].module_load = NULL;
	ins_pass[4].module_unload = NULL;


	//ins pass 6 - functrace - this is a low priority update (should be the last)
	ins_pass[5].name = "functrace";
	ins_pass[5].priority = priority;
	ins_pass[5].priority.name = ins_pass[5].name;
	ins_pass[5].priority.priority = 4;
	ins_pass[5].init_func = functrace_init;
	ins_pass[5].app2app_bb = functrace_bb_app2app;
	ins_pass[5].analysis_bb = functrace_bb_analysis;
	ins_pass[5].instrumentation_bb = functrace_bb_instrumentation;
	ins_pass[5].thread_init = functrace_thread_init;
	ins_pass[5].thread_exit = functrace_thread_exit;
	ins_pass[5].process_exit = functrace_exit_event;
	ins_pass[5].module_load = NULL;
	ins_pass[5].module_unload = NULL;

	//ins pass 7 - funcwrapping - given a high priority
	ins_pass[6].name = "funcwrap";
	ins_pass[6].priority = priority;
	ins_pass[6].priority.name = ins_pass[6].name;
	ins_pass[6].priority.priority = 0;
	ins_pass[6].init_func = funcwrap_init;
	ins_pass[6].app2app_bb = NULL;
	ins_pass[6].analysis_bb = NULL;
	ins_pass[6].instrumentation_bb = funcwrap_bb_instrumentation;
	ins_pass[6].thread_init = funcwrap_thread_init;
	ins_pass[6].thread_exit = funcwrap_thread_exit;
	ins_pass[6].process_exit = funcwrap_exit_event;
	ins_pass[6].module_load = funcwrap_module_load;
	ins_pass[6].module_unload = NULL;


	//ins pass 8 - memdump
	ins_pass[7].name = "memdump";
	ins_pass[7].priority = priority;
	ins_pass[7].priority.name = ins_pass[7].name;
	ins_pass[7].priority.priority = 0;
	ins_pass[7].init_func = memdump_init;
	ins_pass[7].app2app_bb = NULL;
	ins_pass[7].analysis_bb = NULL;
	ins_pass[7].instrumentation_bb = memdump_bb_instrumentation;
	ins_pass[7].thread_init = memdump_thread_init;
	ins_pass[7].thread_exit = memdump_thread_exit;
	ins_pass[7].process_exit = memdump_exit_event;
	ins_pass[7].module_load = memdump_module_load;
	ins_pass[7].module_unload = NULL;

	//ins pass 9 - funcreplace
	ins_pass[8].name = "funcreplace";
	ins_pass[8].priority = priority;
	ins_pass[8].priority.name = ins_pass[8].name;
	ins_pass[8].priority.priority = 0;
	ins_pass[8].init_func = funcreplace_init;
	ins_pass[8].app2app_bb = NULL;
	ins_pass[8].analysis_bb = NULL;
	ins_pass[8].instrumentation_bb = funcreplace_bb_instrumentation;
	ins_pass[8].thread_init = funcreplace_thread_init;
	ins_pass[8].thread_exit = funcreplace_thread_exit;
	ins_pass[8].process_exit = funcreplace_exit_event;
	ins_pass[8].module_load = funcreplace_module_load;
	ins_pass[8].module_unload = NULL;

	//ins pass 10 - misc
	ins_pass[9].name = "misc";
	ins_pass[9].priority = priority;
	ins_pass[9].priority.name = ins_pass[9].name;
	ins_pass[9].priority.priority = 0;
	ins_pass[9].init_func = misc_init;
	ins_pass[9].app2app_bb = NULL;
	ins_pass[9].analysis_bb = NULL;
	ins_pass[9].instrumentation_bb = misc_bb_instrumentation;
	ins_pass[9].thread_init = misc_thread_init;
	ins_pass[9].thread_exit = misc_thread_exit;
	ins_pass[9].process_exit = misc_exit_event;
	ins_pass[9].module_load = NULL;
	ins_pass[9].module_unload = NULL;


	pass_length = 10;

}