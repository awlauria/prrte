/*
 * Copyright (c) 2004-2010 The Trustees of Indiana University and Indiana
 *                         University Research and Technology
 *                         Corporation.  All rights reserved.
 * Copyright (c) 2004-2011 The University of Tennessee and The University
 *                         of Tennessee Research Foundation.  All rights
 *                         reserved.
 * Copyright (c) 2004-2005 High Performance Computing Center Stuttgart,
 *                         University of Stuttgart.  All rights reserved.
 * Copyright (c) 2004-2005 The Regents of the University of California.
 *                         All rights reserved.
 * Copyright (c) 2006-2017 Los Alamos National Security, LLC.
 *                         All rights reserved.
 * Copyright (c) 2009-2018 Cisco Systems, Inc.  All rights reserved
 * Copyright (c) 2011-2017 Oak Ridge National Labs.  All rights reserved.
 * Copyright (c) 2017      UT-Battelle, LLC. All rights reserved.
 * Copyright (c) 2013-2020 Intel, Inc.  All rights reserved.
 * Copyright (c) 2015      Research Organization for Information Science
 *                         and Technology (RIST). All rights reserved.
 * Copyright (c) 2018      IBM Corporation.  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 *
 */

#include "prrte_config.h"
#include "types.h"
#include "types.h"

#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif
#include <ctype.h>

#include "src/util/argv.h"
#include "src/util/prrte_environ.h"
#include "src/util/os_dirpath.h"
#include "src/util/show_help.h"

#include "src/mca/errmgr/errmgr.h"
#include "src/mca/ess/base/base.h"
#include "src/mca/rmaps/rmaps_types.h"
#include "src/prted/prted_submit.h"
#include "src/util/name_fns.h"
#include "src/util/session_dir.h"
#include "src/util/show_help.h"
#include "src/runtime/prrte_globals.h"

#include "src/mca/schizo/base/base.h"

static int define_cli(prrte_cmd_line_t *cli);
static int parse_cli(int argc, int start, char **argv);
static int parse_env(char *path,
                     prrte_cmd_line_t *cmd_line,
                     char **srcenv,
                     char ***dstenv);
static int setup_fork(prrte_job_t *jdata,
                      prrte_app_context_t *context);
static int setup_child(prrte_job_t *jobdat,
                       prrte_proc_t *child,
                       prrte_app_context_t *app,
                       char ***env);

prrte_schizo_base_module_t prrte_schizo_prrte_module = {
    .define_cli = define_cli,
    .parse_cli = parse_cli,
    .parse_env = parse_env,
    .setup_fork = setup_fork,
    .setup_child = setup_child
};


static prrte_cmd_line_init_t cmd_line_init[] = {
    /* Various "obvious" options */
    { NULL, 'h', NULL, "help", 1,
      &prrte_cmd_options.help, PRRTE_CMD_LINE_TYPE_STRING,
      "This help message", PRRTE_CMD_LINE_OTYPE_GENERAL },
    { NULL, 'V', NULL, "version", 0,
      &prrte_cmd_options.version, PRRTE_CMD_LINE_TYPE_BOOL,
      "Print version and exit", PRRTE_CMD_LINE_OTYPE_GENERAL },
    { NULL, 'v', NULL, "verbose", 0,
      &prrte_cmd_options.verbose, PRRTE_CMD_LINE_TYPE_BOOL,
      "Be verbose", PRRTE_CMD_LINE_OTYPE_GENERAL },
    { "prrte_execute_quiet", 'q', NULL, "quiet", 0,
      NULL, PRRTE_CMD_LINE_TYPE_BOOL,
      "Suppress helpful messages", PRRTE_CMD_LINE_OTYPE_GENERAL },
    { NULL, '\0', "report-pid", "report-pid", 1,
      &prrte_cmd_options.report_pid, PRRTE_CMD_LINE_TYPE_STRING,
      "Printout pid on stdout [-], stderr [+], or a file [anything else]",
      PRRTE_CMD_LINE_OTYPE_DEBUG },
    { NULL, '\0', "report-uri", "report-uri", 1,
      &prrte_cmd_options.report_uri, PRRTE_CMD_LINE_TYPE_STRING,
      "Printout URI on stdout [-], stderr [+], or a file [anything else]",
      PRRTE_CMD_LINE_OTYPE_DEBUG },

    /* testing options */
    { NULL, '\0', "timeout", "timeout", 1,
      &prrte_cmd_options.timeout, PRRTE_CMD_LINE_TYPE_INT,
      "Timeout the job after the specified number of seconds",
      PRRTE_CMD_LINE_OTYPE_DEBUG },
    { NULL, '\0', "report-state-on-timeout", "report-state-on-timeout", 0,
      &prrte_cmd_options.report_state_on_timeout, PRRTE_CMD_LINE_TYPE_BOOL,
      "Report all job and process states upon timeout",
      PRRTE_CMD_LINE_OTYPE_DEBUG },
    { NULL, '\0', "get-stack-traces", "get-stack-traces", 0,
      &prrte_cmd_options.get_stack_traces, PRRTE_CMD_LINE_TYPE_BOOL,
      "Get stack traces of all application procs on timeout",
      PRRTE_CMD_LINE_OTYPE_DEBUG },


    /* exit status reporting */
    { "prrte_report_child_jobs_separately", '\0', "report-child-jobs-separately", "report-child-jobs-separately", 0,
      NULL, PRRTE_CMD_LINE_TYPE_BOOL,
      "Return the exit status of the primary job only", PRRTE_CMD_LINE_OTYPE_OUTPUT },

    /* uri of the dvm, or at least where to get it */
    { NULL, '\0', "hnp", "hnp", 1,
      &prrte_cmd_options.hnp, PRRTE_CMD_LINE_TYPE_STRING,
      "Specify the URI of the HNP, or the name of the file (specified as file:filename) that contains that info",
      PRRTE_CMD_LINE_OTYPE_DVM },

    /* select XML output */
    { "prrte_xml_output", '\0', "xml", "xml", 0,
      NULL, PRRTE_CMD_LINE_TYPE_BOOL,
      "Provide all output in XML format", PRRTE_CMD_LINE_OTYPE_OUTPUT },
    { "prrte_xml_file", '\0', "xml-file", "xml-file", 1,
      NULL, PRRTE_CMD_LINE_TYPE_STRING,
      "Provide all output in XML format to the specified file", PRRTE_CMD_LINE_OTYPE_OUTPUT },

    /* tag output */
    { "prrte_tag_output", '\0', "tag-output", "tag-output", 0,
      &prrte_cmd_options.tag_output, PRRTE_CMD_LINE_TYPE_BOOL,
      "Tag all output with [job,rank]", PRRTE_CMD_LINE_OTYPE_OUTPUT },
    { "prrte_timestamp_output", '\0', "timestamp-output", "timestamp-output", 0,
      &prrte_cmd_options.timestamp_output, PRRTE_CMD_LINE_TYPE_BOOL,
      "Timestamp all application process output", PRRTE_CMD_LINE_OTYPE_OUTPUT },
    { "prrte_output_directory", '\0', "output-directory", "output-directory", 1,
      &prrte_cmd_options.output_directory, PRRTE_CMD_LINE_TYPE_STRING,
      "Redirect output from application processes into filename/job/rank/std[out,err,diag]. A relative path value will be converted to an absolute path. The directory name may include a colon followed by a comma-delimited list of optional case-insensitive directives. Supported directives currently include NOJOBID (do not include a job-id directory level) and NOCOPY (do not copy the output to the stdout/err streams)",
      PRRTE_CMD_LINE_OTYPE_OUTPUT },
    { "prrte_output_filename", '\0', "output-filename", "output-filename", 1,
      &prrte_cmd_options.output_filename, PRRTE_CMD_LINE_TYPE_STRING,
      "Redirect output from application processes into filename.rank. A relative path value will be converted to an absolute path. The directory name may include a colon followed by a comma-delimited list of optional case-insensitive directives. Supported directives currently include NOCOPY (do not copy the output to the stdout/err streams)",
      PRRTE_CMD_LINE_OTYPE_OUTPUT },
    { NULL, '\0', "merge-stderr-to-stdout", "merge-stderr-to-stdout", 0,
      &prrte_cmd_options.merge, PRRTE_CMD_LINE_TYPE_BOOL,
      "Merge stderr to stdout for each process", PRRTE_CMD_LINE_OTYPE_OUTPUT },
    { "prrte_xterm", '\0', "xterm", "xterm", 1,
      NULL, PRRTE_CMD_LINE_TYPE_STRING,
      "Create a new xterm window and display output from the specified ranks there",
      PRRTE_CMD_LINE_OTYPE_OUTPUT },

    /* select stdin option */
    { NULL, '\0', "stdin", "stdin", 1,
      &prrte_cmd_options.stdin_target, PRRTE_CMD_LINE_TYPE_STRING,
      "Specify procs to receive stdin [rank, all, none] (default: 0, indicating rank 0)",
      PRRTE_CMD_LINE_OTYPE_INPUT },

    /* request that argv[0] be indexed */
    { NULL, '\0', "index-argv-by-rank", "index-argv-by-rank", 0,
      &prrte_cmd_options.index_argv, PRRTE_CMD_LINE_TYPE_BOOL,
      "Uniquely index argv[0] for each process using its rank",
      PRRTE_CMD_LINE_OTYPE_INPUT },

    /* Specify the launch agent to be used */
    { "prrte_launch_agent", '\0', "launch-agent", "launch-agent", 1,
      NULL, PRRTE_CMD_LINE_TYPE_STRING,
      "Command used to start processes on remote nodes (default: orted)",
      PRRTE_CMD_LINE_OTYPE_LAUNCH },

    /* Preload the binary on the remote machine */
    { NULL, 's', NULL, "preload-binary", 0,
      &prrte_cmd_options.preload_binaries, PRRTE_CMD_LINE_TYPE_BOOL,
      "Preload the binary on the remote machine before starting the remote process.",
      PRRTE_CMD_LINE_OTYPE_LAUNCH },

    /* Preload files on the remote machine */
    { NULL, '\0', NULL, "preload-files", 1,
      &prrte_cmd_options.preload_files, PRRTE_CMD_LINE_TYPE_STRING,
      "Preload the comma separated list of files to the remote machines current working directory before starting the remote process.",
      PRRTE_CMD_LINE_OTYPE_LAUNCH },

    /* Use an appfile */
    { NULL, '\0', NULL, "app", 1,
      &prrte_cmd_options.appfile, PRRTE_CMD_LINE_TYPE_STRING,
      "Provide an appfile; ignore all other command line options",
      PRRTE_CMD_LINE_OTYPE_LAUNCH },

    /* Number of processes; -c, -n, --n, -np, and --np are all
       synonyms */
    { NULL, 'c', "np", "np", 1,
      &prrte_cmd_options.num_procs, PRRTE_CMD_LINE_TYPE_INT,
      "Number of processes to run", PRRTE_CMD_LINE_OTYPE_GENERAL },
    { NULL, '\0', "n", "n", 1,
      &prrte_cmd_options.num_procs, PRRTE_CMD_LINE_TYPE_INT,
      "Number of processes to run", PRRTE_CMD_LINE_OTYPE_GENERAL },

    /* maximum size of VM - typically used to subdivide an allocation */
    { "prrte_max_vm_size", '\0', "max-vm-size", "max-vm-size", 1,
      NULL, PRRTE_CMD_LINE_TYPE_INT,
      "Number of processes to run", PRRTE_CMD_LINE_OTYPE_DVM },

    /* Set a hostfile */
    { NULL, '\0', "hostfile", "hostfile", 1,
      NULL, PRRTE_CMD_LINE_TYPE_STRING,
      "Provide a hostfile", PRRTE_CMD_LINE_OTYPE_LAUNCH },
    { NULL, '\0', "machinefile", "machinefile", 1,
      NULL, PRRTE_CMD_LINE_TYPE_STRING,
      "Provide a hostfile", PRRTE_CMD_LINE_OTYPE_LAUNCH },
    { "prrte_default_hostfile", '\0', "default-hostfile", "default-hostfile", 1,
        NULL, PRRTE_CMD_LINE_TYPE_STRING,
      "Provide a default hostfile", PRRTE_CMD_LINE_OTYPE_LAUNCH },
    { "prrte_if_do_not_resolve", '\0', "do-not-resolve", "do-not-resolve", 0,
      NULL, PRRTE_CMD_LINE_TYPE_BOOL,
      "Do not attempt to resolve interfaces", PRRTE_CMD_LINE_OTYPE_DEVEL },

    /* uri of PMIx publish/lookup server, or at least where to get it */
    { "pmix_server_uri", '\0', "prrte-server", "prrte-server", 1,
      NULL, PRRTE_CMD_LINE_TYPE_STRING,
      "Specify the URI of the publish/lookup server, or the name of the file (specified as file:filename) that contains that info",
      PRRTE_CMD_LINE_OTYPE_DVM },

    { "carto_file_path", '\0', "cf", "cartofile", 1,
      NULL, PRRTE_CMD_LINE_TYPE_STRING,
      "Provide a cartography file", PRRTE_CMD_LINE_OTYPE_MAPPING },

    { "prrte_rankfile", '\0', "rf", "rankfile", 1,
      NULL, PRRTE_CMD_LINE_TYPE_STRING,
      "Provide a rankfile file", PRRTE_CMD_LINE_OTYPE_MAPPING },

    /* Export environment variables; potentially used multiple times,
       so it does not make sense to set into a variable */
    { NULL, 'x', NULL, NULL, 1,
      NULL, PRRTE_CMD_LINE_TYPE_NULL,
      "Export an environment variable, optionally specifying a value (e.g., \"-x foo\" exports the environment variable foo and takes its value from the current environment; \"-x foo=bar\" exports the environment variable name foo and sets its value to \"bar\" in the started processes)", PRRTE_CMD_LINE_OTYPE_LAUNCH },

      /* Mapping controls */
    { "rmaps_base_display_map", '\0', "display-map", "display-map", 0,
      NULL, PRRTE_CMD_LINE_TYPE_BOOL,
      "Display the process map just before launch", PRRTE_CMD_LINE_OTYPE_DEBUG },
    { "rmaps_base_display_devel_map", '\0', "display-devel-map", "display-devel-map", 0,
       NULL, PRRTE_CMD_LINE_TYPE_BOOL,
       "Display a detailed process map (mostly intended for developers) just before launch",
       PRRTE_CMD_LINE_OTYPE_DEVEL },
    { "rmaps_base_display_topo_with_map", '\0', "display-topo", "display-topo", 0,
       NULL, PRRTE_CMD_LINE_TYPE_BOOL,
       "Display the topology as part of the process map (mostly intended for developers) just before launch",
       PRRTE_CMD_LINE_OTYPE_DEVEL },
    { "rmaps_base_display_diffable_map", '\0', "display-diffable-map", "display-diffable-map", 0,
       NULL, PRRTE_CMD_LINE_TYPE_BOOL,
       "Display a diffable process map (mostly intended for developers) just before launch",
       PRRTE_CMD_LINE_OTYPE_DEVEL },
    { NULL, 'H', "host", "host", 1,
      NULL, PRRTE_CMD_LINE_TYPE_STRING,
      "List of hosts to invoke processes on",
      PRRTE_CMD_LINE_OTYPE_MAPPING },
    { "rmaps_base_no_schedule_local", '\0', "nolocal", "nolocal", 0,
      &prrte_cmd_options.nolocal, PRRTE_CMD_LINE_TYPE_BOOL,
      "Do not run any MPI applications on the local node",
      PRRTE_CMD_LINE_OTYPE_MAPPING },
    { "rmaps_base_no_oversubscribe", '\0', "nooversubscribe", "nooversubscribe", 0,
      &prrte_cmd_options.no_oversubscribe, PRRTE_CMD_LINE_TYPE_BOOL,
      "Nodes are not to be oversubscribed, even if the system supports such operation",
      PRRTE_CMD_LINE_OTYPE_MAPPING },
    { "rmaps_base_oversubscribe", '\0', "oversubscribe", "oversubscribe", 0,
      &prrte_cmd_options.oversubscribe, PRRTE_CMD_LINE_TYPE_BOOL,
      "Nodes are allowed to be oversubscribed, even on a managed system, and overloading of processing elements",
      PRRTE_CMD_LINE_OTYPE_MAPPING },
    { "rmaps_base_cpus_per_rank", '\0', "cpus-per-proc", "cpus-per-proc", 1,
      &prrte_cmd_options.cpus_per_proc, PRRTE_CMD_LINE_TYPE_INT,
      "Number of cpus to use for each process [default=1]",
      PRRTE_CMD_LINE_OTYPE_MAPPING },
    { "rmaps_base_cpus_per_rank", '\0', "cpus-per-rank", "cpus-per-rank", 1,
      &prrte_cmd_options.cpus_per_proc, PRRTE_CMD_LINE_TYPE_INT,
      "Synonym for cpus-per-proc", PRRTE_CMD_LINE_OTYPE_MAPPING },

    /* backward compatiblity */
    { "rmaps_base_bycore", '\0', "bycore", "bycore", 0,
      NULL, PRRTE_CMD_LINE_TYPE_BOOL,
      "Whether to map and rank processes round-robin by core",
      PRRTE_CMD_LINE_OTYPE_COMPAT },
    { "rmaps_base_bynode", '\0', "bynode", "bynode", 0,
      NULL, PRRTE_CMD_LINE_TYPE_BOOL,
      "Whether to map and rank processes round-robin by node",
      PRRTE_CMD_LINE_OTYPE_COMPAT },
    { "rmaps_base_byslot", '\0', "byslot", "byslot", 0,
      NULL, PRRTE_CMD_LINE_TYPE_BOOL,
      "Whether to map and rank processes round-robin by slot",
      PRRTE_CMD_LINE_OTYPE_COMPAT },

    /* Nperxxx options that do not require topology and are always
     * available - included for backwards compatibility
     */
    { "rmaps_ppr_pernode", '\0', "pernode", "pernode", 0,
      &prrte_cmd_options.pernode, PRRTE_CMD_LINE_TYPE_BOOL,
      "Launch one process per available node",
      PRRTE_CMD_LINE_OTYPE_COMPAT },
    { "rmaps_ppr_n_pernode", '\0', "npernode", "npernode", 1,
      &prrte_cmd_options.npernode, PRRTE_CMD_LINE_TYPE_INT,
      "Launch n processes per node on all allocated nodes",
      PRRTE_CMD_LINE_OTYPE_COMPAT },
    { "rmaps_ppr_n_pernode", '\0', "N", NULL, 1,
      &prrte_cmd_options.npernode, PRRTE_CMD_LINE_TYPE_INT,
      "Launch n processes per node on all allocated nodes (synonym for 'map-by node')",
      PRRTE_CMD_LINE_OTYPE_MAPPING },

    /* declare hardware threads as independent cpus */
    { "hwloc_base_use_hwthreads_as_cpus", '\0', "use-hwthread-cpus", "use-hwthread-cpus", 0,
      NULL, PRRTE_CMD_LINE_TYPE_BOOL,
      "Use hardware threads as independent cpus", PRRTE_CMD_LINE_OTYPE_MAPPING },

    /* include npersocket for backwards compatibility */
    { "rmaps_ppr_n_persocket", '\0', "npersocket", "npersocket", 1,
      &prrte_cmd_options.npersocket, PRRTE_CMD_LINE_TYPE_INT,
      "Launch n processes per socket on all allocated nodes",
      PRRTE_CMD_LINE_OTYPE_COMPAT },

    /* Mapping options */
    { "rmaps_base_mapping_policy", '\0', NULL, "map-by", 1,
      &prrte_cmd_options.mapping_policy, PRRTE_CMD_LINE_TYPE_STRING,
      "Mapping Policy [slot | hwthread | core | socket (default) | numa | board | node]",
      PRRTE_CMD_LINE_OTYPE_MAPPING },

      /* Ranking options */
    { "rmaps_base_ranking_policy", '\0', NULL, "rank-by", 1,
      &prrte_cmd_options.ranking_policy, PRRTE_CMD_LINE_TYPE_STRING,
      "Ranking Policy [slot (default) | hwthread | core | socket | numa | board | node]",
      PRRTE_CMD_LINE_OTYPE_RANKING },

      /* Binding options */
    { "hwloc_base_binding_policy", '\0', NULL, "bind-to", 1,
      &prrte_cmd_options.binding_policy, PRRTE_CMD_LINE_TYPE_STRING,
      "Policy for binding processes. Allowed values: none, hwthread, core, l1cache, l2cache, l3cache, socket, numa, board, cpu-list (\"none\" is the default when oversubscribed, \"core\" is the default when np<=2, and \"socket\" is the default when np>2). Allowed qualifiers: overload-allowed, if-supported, ordered", PRRTE_CMD_LINE_OTYPE_BINDING },

    /* backward compatiblity */
    { "hwloc_base_bind_to_core", '\0', "bind-to-core", "bind-to-core", 0,
      NULL, PRRTE_CMD_LINE_TYPE_BOOL,
      "Bind processes to cores", PRRTE_CMD_LINE_OTYPE_COMPAT },
    { "hwloc_base_bind_to_socket", '\0', "bind-to-socket", "bind-to-socket", 0,
      NULL, PRRTE_CMD_LINE_TYPE_BOOL,
      "Bind processes to sockets", PRRTE_CMD_LINE_OTYPE_COMPAT },

    { "hwloc_base_report_bindings", '\0', "report-bindings", "report-bindings", 0,
      &prrte_cmd_options.report_bindings, PRRTE_CMD_LINE_TYPE_BOOL,
      "Whether to report process bindings to stderr",
      PRRTE_CMD_LINE_OTYPE_BINDING },

    /* slot list option */
    { "hwloc_base_cpu_list", '\0', "cpu-list", "cpu-list", 1,
      &prrte_cmd_options.cpu_list, PRRTE_CMD_LINE_TYPE_STRING,
      "List of processor IDs to bind processes to [default=NULL]",
      PRRTE_CMD_LINE_OTYPE_BINDING },

    /* generalized pattern mapping option */
    { "rmaps_ppr_pattern", '\0', NULL, "ppr", 1,
      NULL, PRRTE_CMD_LINE_TYPE_STRING,
      "Comma-separated list of number of processes on a given resource type [default: none]",
      PRRTE_CMD_LINE_OTYPE_MAPPING },

    /* Allocation options */
    { "prrte_display_alloc", '\0', "display-allocation", "display-allocation", 0,
      NULL, PRRTE_CMD_LINE_TYPE_BOOL,
      "Display the allocation being used by this job", PRRTE_CMD_LINE_OTYPE_DEBUG },
    { "prrte_display_devel_alloc", '\0', "display-devel-allocation", "display-devel-allocation", 0,
      NULL, PRRTE_CMD_LINE_TYPE_BOOL,
      "Display a detailed list (mostly intended for developers) of the allocation being used by this job",
      PRRTE_CMD_LINE_OTYPE_DEVEL },
    { "hwloc_base_cpu_set", '\0', "cpu-set", "cpu-set", 1,
      NULL, PRRTE_CMD_LINE_TYPE_STRING,
      "Comma-separated list of ranges specifying logical cpus allocated to this job [default: none]",
      PRRTE_CMD_LINE_OTYPE_DEBUG },

    /* mpiexec-like arguments */
    { NULL, '\0', "wdir", "wdir", 1,
      &prrte_cmd_options.wdir, PRRTE_CMD_LINE_TYPE_STRING,
      "Set the working directory of the started processes",
      PRRTE_CMD_LINE_OTYPE_LAUNCH },
    { NULL, '\0', "wd", "wd", 1,
      &prrte_cmd_options.wdir, PRRTE_CMD_LINE_TYPE_STRING,
      "Synonym for --wdir", PRRTE_CMD_LINE_OTYPE_LAUNCH },
    { NULL, '\0', "set-cwd-to-session-dir", "set-cwd-to-session-dir", 0,
      &prrte_cmd_options.set_cwd_to_session_dir, PRRTE_CMD_LINE_TYPE_BOOL,
      "Set the working directory of the started processes to their session directory",
      PRRTE_CMD_LINE_OTYPE_LAUNCH },
    { NULL, '\0', "path", "path", 1,
      &prrte_cmd_options.path, PRRTE_CMD_LINE_TYPE_STRING,
      "PATH to be used to look for executables to start processes",
      PRRTE_CMD_LINE_OTYPE_LAUNCH },

    /* User-level debugger arguments */
    { NULL, '\0', "tv", "tv", 0,
      &prrte_cmd_options.debugger, PRRTE_CMD_LINE_TYPE_BOOL,
      "Deprecated backwards compatibility flag; synonym for \"--debug\"",
      PRRTE_CMD_LINE_OTYPE_DEBUG },
    { NULL, '\0', "debug", "debug", 0,
      &prrte_cmd_options.debugger, PRRTE_CMD_LINE_TYPE_BOOL,
      "Invoke the user-level debugger indicated by the prrte_base_user_debugger MCA parameter",
      PRRTE_CMD_LINE_OTYPE_DEBUG },
    { "prrte_base_user_debugger", '\0', "debugger", "debugger", 1,
      NULL, PRRTE_CMD_LINE_TYPE_STRING,
      "Sequence of debuggers to search for when \"--debug\" is used",
      PRRTE_CMD_LINE_OTYPE_DEBUG },
    { "prrte_output_debugger_proctable", '\0', "output-proctable", "output-proctable", 0,
      NULL, PRRTE_CMD_LINE_TYPE_BOOL,
      "Output the debugger proctable after launch",
      PRRTE_CMD_LINE_OTYPE_DEBUG },

    /* OpenRTE arguments */
    { "prrte_debug", 'd', "debug-devel", "debug-devel", 0,
      NULL, PRRTE_CMD_LINE_TYPE_BOOL,
      "Enable debugging of OpenRTE", PRRTE_CMD_LINE_OTYPE_DEVEL },

    { "prrte_debug_daemons", '\0', "debug-daemons", "debug-daemons", 0,
      NULL, PRRTE_CMD_LINE_TYPE_INT,
      "Enable debugging of any OpenRTE daemons used by this application",
      PRRTE_CMD_LINE_OTYPE_DEVEL },

    { "prrte_debug_daemons_file", '\0', "debug-daemons-file", "debug-daemons-file", 0,
      NULL, PRRTE_CMD_LINE_TYPE_BOOL,
      "Enable debugging of any OpenRTE daemons used by this application, storing output in files",
      PRRTE_CMD_LINE_OTYPE_DEVEL },

    { "prrte_leave_session_attached", '\0', "leave-session-attached", "leave-session-attached", 0,
      NULL, PRRTE_CMD_LINE_TYPE_BOOL,
      "Enable debugging of OpenRTE", PRRTE_CMD_LINE_OTYPE_DEBUG },

    { "prrte_do_not_launch", '\0', "do-not-launch", "do-not-launch", 0,
      NULL, PRRTE_CMD_LINE_TYPE_BOOL,
      "Perform all necessary operations to prepare to launch the application, but do not actually launch it",
      PRRTE_CMD_LINE_OTYPE_DEVEL },

    { NULL, '\0', NULL, "prefix", 1,
      NULL, PRRTE_CMD_LINE_TYPE_STRING,
      "Prefix where Open MPI is installed on remote nodes",
      PRRTE_CMD_LINE_OTYPE_LAUNCH },
    { NULL, '\0', NULL, "noprefix", 0,
      NULL, PRRTE_CMD_LINE_TYPE_STRING,
      "Disable automatic --prefix behavior",
      PRRTE_CMD_LINE_OTYPE_LAUNCH },

    { "prrte_report_launch_progress", '\0', "show-progress", "show-progress", 0,
      NULL, PRRTE_CMD_LINE_TYPE_BOOL,
      "Output a brief periodic report on launch progress",
      PRRTE_CMD_LINE_OTYPE_LAUNCH },

    { "prrte_use_regexp", '\0', "use-regexp", "use-regexp", 0,
      NULL, PRRTE_CMD_LINE_TYPE_BOOL,
      "Use regular expressions for launch", PRRTE_CMD_LINE_OTYPE_LAUNCH },

    { "prrte_report_events", '\0', "report-events", "report-events", 1,
      NULL, PRRTE_CMD_LINE_TYPE_STRING,
      "Report events to a tool listening at the specified URI", PRRTE_CMD_LINE_OTYPE_DEBUG },

    { "prrte_enable_recovery", '\0', "enable-recovery", "enable-recovery", 0,
      NULL, PRRTE_CMD_LINE_TYPE_BOOL,
      "Enable recovery from process failure [Default = disabled]",
      PRRTE_CMD_LINE_OTYPE_UNSUPPORTED },

    { "prrte_max_restarts", '\0', "max-restarts", "max-restarts", 1,
      NULL, PRRTE_CMD_LINE_TYPE_INT,
      "Max number of times to restart a failed process",
      PRRTE_CMD_LINE_OTYPE_UNSUPPORTED },

    { NULL, '\0', "continuous", "continuous", 0,
      &prrte_cmd_options.continuous, PRRTE_CMD_LINE_TYPE_BOOL,
      "Job is to run until explicitly terminated", PRRTE_CMD_LINE_OTYPE_DEBUG },

    { NULL, '\0', "disable-recovery", "disable-recovery", 0,
      &prrte_cmd_options.disable_recovery, PRRTE_CMD_LINE_TYPE_BOOL,
      "Disable recovery (resets all recovery options to off)",
      PRRTE_CMD_LINE_OTYPE_UNSUPPORTED },

    { "prrte_no_vm", '\0', "novm", "novm", 0,
      NULL, PRRTE_CMD_LINE_TYPE_BOOL,
      "Execute without creating an allocation-spanning virtual machine (only start daemons on nodes hosting application procs)",
      PRRTE_CMD_LINE_OTYPE_DVM },

    { NULL, '\0', "allow-run-as-root", "allow-run-as-root", 0,
      &prrte_cmd_options.run_as_root, PRRTE_CMD_LINE_TYPE_BOOL,
      "Allow execution as root (STRONGLY DISCOURAGED)",
      PRRTE_CMD_LINE_OTYPE_LAUNCH },

    { NULL, '\0', "personality", "personality", 1,
      &prrte_cmd_options.personality, PRRTE_CMD_LINE_TYPE_STRING,
      "Comma-separated list of programming model, languages, and containers being used (default=\"prrte\")",
      PRRTE_CMD_LINE_OTYPE_LAUNCH },

    { NULL, '\0', "pset", "pset", 1,
      &prrte_cmd_options.pset, PRRTE_CMD_LINE_TYPE_STRING,
      "User-specified name assigned to the processes in their given application",
      PRRTE_CMD_LINE_OTYPE_LAUNCH },

    { NULL, '\0', "dvm", "dvm", 0,
      &prrte_cmd_options.create_dvm, PRRTE_CMD_LINE_TYPE_BOOL,
      "Create a persistent distributed virtual machine (DVM)",
      PRRTE_CMD_LINE_OTYPE_DVM },

    /* fwd mpirun port */
    { "prrte_fwd_mpirun_port", '\0', "fwd-mpirun-port", "fwd-mpirun-port", 0,
      NULL, PRRTE_CMD_LINE_TYPE_BOOL,
      "Forward mpirun port to compute node daemons so all will use it",
      PRRTE_CMD_LINE_OTYPE_LAUNCH },

    /* End of list */
    { NULL, '\0', NULL, NULL, 0,
      NULL, PRRTE_CMD_LINE_TYPE_NULL, NULL }
};

static int define_cli(prrte_cmd_line_t *cli)
{
    int i, rc;
    bool takeus = false;

    prrte_output_verbose(1, prrte_schizo_base_framework.framework_output,
                        "%s schizo:prrte: define_cli",
                        PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME));

    /* protect against bozo error */
    if (NULL == cli) {
        return PRRTE_ERR_BAD_PARAM;
    }

    if (NULL != prrte_schizo_base.personalities) {
        /* if we aren't included, then ignore us */
        for (i=0; NULL != prrte_schizo_base.personalities[i]; i++) {
            if (0 == strcmp(prrte_schizo_base.personalities[i], "prrte")) {
                takeus = true;
                break;
            }
        }
        if (!takeus) {
            return PRRTE_ERR_TAKE_NEXT_OPTION;
        }
    }

    /*
     * Check if a HNP DVM URI is being passed via environment.
     * Note: Place before prrte_cmd_line_parse() so that
     * if user passes both envvar & cmdln, the cmdln wins.
     */
    if (NULL != getenv("PRRTE_HNP_DVM_URI")) {
        prrte_cmd_options.hnp = strdup(getenv("PRRTE_HNP_DVM_URI"));
    }

    /* just add ours to the end */
    rc = prrte_cmd_line_add(cli, cmd_line_init);
    return rc;
}

static int parse_cli(int argc, int start, char **argv)
{
    int i, j, k;
    bool ignore;
    char *no_dups[] = {
        "grpcomm",
        "odls",
        "rml",
        "routed",
        NULL
    };
    bool takeus = false;

    prrte_output_verbose(1, prrte_schizo_base_framework.framework_output,
                        "%s schizo:prrte: parse_cli",
                        PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME));

    /* if they gave us a list of personalities,
     * see if we are included */
    if (NULL != prrte_schizo_base.personalities) {
        for (i=0; NULL != prrte_schizo_base.personalities[i]; i++) {
            if (0 == strcmp(prrte_schizo_base.personalities[i], "prrte")) {
                takeus = true;
                break;
            }
        }
        if (!takeus) {
            return PRRTE_ERR_TAKE_NEXT_OPTION;
        }
    } else {
        /* attempt to auto-detect CLI options that
         * we recognize */
    }

    for (i = 0; i < (argc-start); ++i) {
        if (0 == strcmp("-mca",  argv[i]) ||
            0 == strcmp("--mca", argv[i]) ) {
            /* ignore this one */
            if (0 == strcmp(argv[i+1], "mca_base_env_list")) {
                i += 2;
                continue;
            }
            /* It would be nice to avoid increasing the length
             * of the orted cmd line by removing any non-PRRTE
             * params. However, this raises a problem since
             * there could be PRRTE directives that we really
             * -do- want the orted to see - it's only the OMPI
             * related directives we could ignore. This becomes
             * a very complicated procedure, however, since
             * the OMPI mca params are not cleanly separated - so
             * filtering them out is nearly impossible.
             *
             * see if this is already present so we at least can
             * avoid growing the cmd line with duplicates
             */
            ignore = false;
            if (NULL != prted_cmd_line) {
                for (j=0; NULL != prted_cmd_line[j]; j++) {
                    if (0 == strcmp(argv[i+1], prted_cmd_line[j])) {
                        /* already here - if the value is the same,
                         * we can quitely ignore the fact that they
                         * provide it more than once. However, some
                         * frameworks are known to have problems if the
                         * value is different. We don't have a good way
                         * to know this, but we at least make a crude
                         * attempt here to protect ourselves.
                         */
                        if (0 == strcmp(argv[i+2], prted_cmd_line[j+1])) {
                            /* values are the same */
                            ignore = true;
                            break;
                        } else {
                            /* values are different - see if this is a problem */
                            for (k=0; NULL != no_dups[k]; k++) {
                                if (0 == strcmp(no_dups[k], argv[i+1])) {
                                    /* print help message
                                     * and abort as we cannot know which one is correct
                                     */
                                    prrte_show_help("help-prrterun.txt", "prrterun:conflicting-params",
                                                   true, prrte_tool_basename, argv[i+1],
                                                   argv[i+2], prted_cmd_line[j+1]);
                                    return PRRTE_ERR_BAD_PARAM;
                                }
                            }
                            /* this passed muster - just ignore it */
                            ignore = true;
                            break;
                        }
                    }
                }
            }
            if (!ignore) {
                prrte_argv_append_nosize(&prted_cmd_line, argv[i]);
                prrte_argv_append_nosize(&prted_cmd_line, argv[i+1]);
                prrte_argv_append_nosize(&prted_cmd_line, argv[i+2]);
            }
            i += 2;
        }
    }
    return PRRTE_SUCCESS;
}

static int parse_env(char *path,
                     prrte_cmd_line_t *cmd_line,
                     char **srcenv,
                     char ***dstenv)
{
    int i, j;
    char *param;
    char *value;
    char *env_set_flag;
    char **vars;
    bool takeus = false;

    prrte_output_verbose(1, prrte_schizo_base_framework.framework_output,
                        "%s schizo:prrte: parse_env",
                        PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME));

    if (NULL != prrte_schizo_base.personalities) {
        /* see if we are included */
        for (i=0; NULL != prrte_schizo_base.personalities[i]; i++) {
            if (0 == strcmp(prrte_schizo_base.personalities[i], "prrte")) {
                takeus = true;
                break;
            }
        }
        if (!takeus) {
            return PRRTE_ERR_TAKE_NEXT_OPTION;
        }
    }

    for (i = 0; NULL != srcenv[i]; ++i) {
        if (0 == strncmp("OMPI_", srcenv[i], 5) ||
            0 == strncmp("PMIX_", srcenv[i], 5)) {
            /* check for duplicate in app->env - this
             * would have been placed there by the
             * cmd line processor. By convention, we
             * always let the cmd line override the
             * environment
             */
            param = strdup(srcenv[i]);
            value = strchr(param, '=');
            *value = '\0';
            value++;
            prrte_setenv(param, value, false, dstenv);
            free(param);
        }
    }

    /* set necessary env variables for external usage from tune conf file*/
    int set_from_file = 0;
    vars = NULL;
    if (PRRTE_SUCCESS == prrte_mca_base_var_process_env_list_from_file(&vars) &&
            NULL != vars) {
        for (i=0; NULL != vars[i]; i++) {
            value = strchr(vars[i], '=');
            /* terminate the name of the param */
            *value = '\0';
            /* step over the equals */
            value++;
            /* overwrite any prior entry */
            prrte_setenv(vars[i], value, true, dstenv);
            /* save it for any comm_spawn'd apps */
            prrte_setenv(vars[i], value, true, &prrte_forwarded_envars);
        }
        set_from_file = 1;
        prrte_argv_free(vars);
    }
    /* Did the user request to export any environment variables on the cmd line? */
    env_set_flag = getenv("OMPI_MCA_mca_base_env_list");
    if (prrte_cmd_line_is_taken(cmd_line, "x")) {
        if (NULL != env_set_flag) {
            prrte_show_help("help-prrterun.txt", "prrterun:conflict-env-set", false);
            return PRRTE_ERR_FATAL;
        }
        j = prrte_cmd_line_get_ninsts(cmd_line, "x");
        for (i = 0; i < j; ++i) {
            param = prrte_cmd_line_get_param(cmd_line, "x", i, 0);

            if (NULL != (value = strchr(param, '='))) {
                /* terminate the name of the param */
                *value = '\0';
                /* step over the equals */
                value++;
                /* overwrite any prior entry */
                prrte_setenv(param, value, true, dstenv);
                /* save it for any comm_spawn'd apps */
                prrte_setenv(param, value, true, &prrte_forwarded_envars);
            } else {
                value = getenv(param);
                if (NULL != value) {
                    /* overwrite any prior entry */
                    prrte_setenv(param, value, true, dstenv);
                    /* save it for any comm_spawn'd apps */
                    prrte_setenv(param, value, true, &prrte_forwarded_envars);
                } else {
                    prrte_output(0, "Warning: could not find environment variable \"%s\"\n", param);
                }
            }
        }
    } else if (NULL != env_set_flag) {
        /* if mca_base_env_list was set, check if some of env vars were set via -x from a conf file.
         * If this is the case, error out.
         */
        if (!set_from_file) {
            /* set necessary env variables for external usage */
            vars = NULL;
            if (PRRTE_SUCCESS == prrte_mca_base_var_process_env_list(env_set_flag, &vars) &&
                    NULL != vars) {
                for (i=0; NULL != vars[i]; i++) {
                    value = strchr(vars[i], '=');
                    /* terminate the name of the param */
                    *value = '\0';
                    /* step over the equals */
                    value++;
                    /* overwrite any prior entry */
                    prrte_setenv(vars[i], value, true, dstenv);
                    /* save it for any comm_spawn'd apps */
                    prrte_setenv(vars[i], value, true, &prrte_forwarded_envars);
                }
                prrte_argv_free(vars);
            }
        } else {
            prrte_show_help("help-prrterun.txt", "prrterun:conflict-env-set", false);
            return PRRTE_ERR_FATAL;
        }
    }

    /* If the user specified --path, store it in the user's app
       environment via the OMPI_exec_path variable. */
    if (NULL != path) {
        prrte_asprintf(&value, "OMPI_exec_path=%s", path);
        prrte_argv_append_nosize(dstenv, value);
        /* save it for any comm_spawn'd apps */
        prrte_argv_append_nosize(&prrte_forwarded_envars, value);
        free(value);
    }

    return PRRTE_SUCCESS;
}

static int setup_fork(prrte_job_t *jdata,
                      prrte_app_context_t *app)
{
    int i;
    char *param, *p2, *saveptr;
    bool oversubscribed;
    prrte_node_t *node;
    char **envcpy, **nps, **firstranks;
    char *npstring, *firstrankstring;
    char *num_app_ctx;
    bool takeus = false;
    bool exists;
    prrte_app_context_t* tmp_app;
    prrte_attribute_t *attr;

    prrte_output_verbose(1, prrte_schizo_base_framework.framework_output,
                        "%s schizo:prrte: setup_fork",
                        PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME));

    /* if no personality was specified, then nothing to do */
    if (NULL == jdata->personality) {
        return PRRTE_ERR_TAKE_NEXT_OPTION;
    }

    if (NULL != prrte_schizo_base.personalities) {
    /* see if we are included */
        for (i=0; NULL != jdata->personality[i]; i++) {
            if (0 == strcmp(jdata->personality[i], "prrte")) {
                takeus = true;
                break;
            }
        }
        if (!takeus) {
            return PRRTE_ERR_TAKE_NEXT_OPTION;
        }
    }

    /* see if the mapper thinks we are oversubscribed */
    oversubscribed = false;
    if (NULL == (node = (prrte_node_t*)prrte_pointer_array_get_item(prrte_node_pool, PRRTE_PROC_MY_NAME->vpid))) {
        PRRTE_ERROR_LOG(PRRTE_ERR_NOT_FOUND);
        return PRRTE_ERR_NOT_FOUND;
    }
    if (PRRTE_FLAG_TEST(node, PRRTE_NODE_FLAG_OVERSUBSCRIBED)) {
        oversubscribed = true;
    }

    /* setup base environment: copy the current environ and merge
       in the app context environ */
    if (NULL != app->env) {
        /* manually free original context->env to avoid a memory leak */
        char **tmp = app->env;
        envcpy = prrte_environ_merge(prrte_launch_environ, app->env);
        if (NULL != tmp) {
            prrte_argv_free(tmp);
        }
    } else {
        envcpy = prrte_argv_copy(prrte_launch_environ);
    }
    app->env = envcpy;

    /* special case handling for --prefix: this is somewhat icky,
       but at least some users do this.  :-\ It is possible that
       when using --prefix, the user will also "-x PATH" and/or
       "-x LD_LIBRARY_PATH", which would therefore clobber the
       work that was done in the prior pls to ensure that we have
       the prefix at the beginning of the PATH and
       LD_LIBRARY_PATH.  So examine the context->env and see if we
       find PATH or LD_LIBRARY_PATH.  If found, that means the
       prior work was clobbered, and we need to re-prefix those
       variables. */
    param = NULL;
    prrte_get_attribute(&app->attributes, PRRTE_APP_PREFIX_DIR, (void**)&param, PRRTE_STRING);
    /* grab the parameter from the first app context because the current context does not have a prefix assigned */
    if (NULL == param) {
        tmp_app = (prrte_app_context_t*)prrte_pointer_array_get_item(jdata->apps, 0);
        assert (NULL != tmp_app);
        prrte_get_attribute(&tmp_app->attributes, PRRTE_APP_PREFIX_DIR, (void**)&param, PRRTE_STRING);
    }
    for (i = 0; NULL != param && NULL != app->env && NULL != app->env[i]; ++i) {
        char *newenv;

        /* Reset PATH */
        if (0 == strncmp("PATH=", app->env[i], 5)) {
            prrte_asprintf(&newenv, "%s/bin:%s", param, app->env[i] + 5);
            prrte_setenv("PATH", newenv, true, &app->env);
            free(newenv);
        }

        /* Reset LD_LIBRARY_PATH */
        else if (0 == strncmp("LD_LIBRARY_PATH=", app->env[i], 16)) {
            prrte_asprintf(&newenv, "%s/lib:%s", param, app->env[i] + 16);
            prrte_setenv("LD_LIBRARY_PATH", newenv, true, &app->env);
            free(newenv);
        }
    }
    if (NULL != param) {
        free(param);
    }

    /* pass my contact info to the local proc so we can talk */
    prrte_setenv("OMPI_MCA_prrte_local_daemon_uri", prrte_process_info.my_daemon_uri, true, &app->env);

    /* pass the hnp's contact info to the local proc in case it
     * needs it
     */
    if (NULL != prrte_process_info.my_hnp_uri) {
        prrte_setenv("OMPI_MCA_prrte_hnp_uri", prrte_process_info.my_hnp_uri, true, &app->env);
    }

    /* setup yield schedule */
    if (oversubscribed) {
        prrte_setenv("OMPI_MCA_mpi_oversubscribe", "1", true, &app->env);
    } else {
        prrte_setenv("OMPI_MCA_mpi_oversubscribe", "0", true, &app->env);
    }

    /* set the app_context number into the environment */
    prrte_asprintf(&param, "%ld", (long)app->idx);
    prrte_setenv("OMPI_MCA_prrte_app_num", param, true, &app->env);
    free(param);

    /* although the total_slots_alloc is the universe size, users
     * would appreciate being given a public environmental variable
     * that also represents this value - something MPI specific - so
     * do that here. Also required by the prrte_attributes code!
     *
     * AND YES - THIS BREAKS THE ABSTRACTION BARRIER TO SOME EXTENT.
     * We know - just live with it
     */
    prrte_asprintf(&param, "%ld", (long)jdata->total_slots_alloc);
    prrte_setenv("OMPI_UNIVERSE_SIZE", param, true, &app->env);
    free(param);

    /* pass the number of nodes involved in this job */
    prrte_asprintf(&param, "%ld", (long)(jdata->map->num_nodes));
    prrte_setenv("OMPI_MCA_prrte_num_nodes", param, true, &app->env);
    free(param);

    /* pass a param telling the child what type and model of cpu we are on,
     * if we know it. If hwloc has the value, use what it knows. Otherwise,
     * see if we were explicitly given it and use that value.
     */
    hwloc_obj_t obj;
    char *htmp;
    if (NULL != prrte_hwloc_topology) {
        obj = hwloc_get_root_obj(prrte_hwloc_topology);
        if (NULL != (htmp = (char*)hwloc_obj_get_info_by_name(obj, "CPUType")) ||
            NULL != (htmp = prrte_local_cpu_type)) {
            prrte_setenv("OMPI_MCA_prrte_cpu_type", htmp, true, &app->env);
        }
        if (NULL != (htmp = (char*)hwloc_obj_get_info_by_name(obj, "CPUModel")) ||
            NULL != (htmp = prrte_local_cpu_model)) {
            prrte_setenv("OMPI_MCA_prrte_cpu_model", htmp, true, &app->env);
        }
    } else {
        if (NULL != prrte_local_cpu_type) {
            prrte_setenv("OMPI_MCA_prrte_cpu_type", prrte_local_cpu_type, true, &app->env);
        }
        if (NULL != prrte_local_cpu_model) {
            prrte_setenv("OMPI_MCA_prrte_cpu_model", prrte_local_cpu_model, true, &app->env);
        }
    }

    /* Set an info MCA param that tells the launched processes that
     * any binding policy was applied by us (e.g., so that
     * MPI_INIT doesn't try to bind itself)
     */
    if (PRRTE_BIND_TO_NONE != PRRTE_GET_BINDING_POLICY(jdata->map->binding)) {
        prrte_setenv("OMPI_MCA_prrte_bound_at_launch", "1", true, &app->env);
    }

    /* tell the ESS to avoid the singleton component - but don't override
     * anything that may have been provided elsewhere
     */
    prrte_setenv("OMPI_MCA_ess", "^singleton", false, &app->env);

    /* ensure that the spawned process ignores direct launch components,
     * but do not overrride anything we were given */
    prrte_setenv("OMPI_MCA_pmix", "^s1,s2,cray", false, &app->env);

    /* since we want to pass the name as separate components, make sure
     * that the "name" environmental variable is cleared!
     */
    prrte_unsetenv("OMPI_MCA_prrte_ess_name", &app->env);

    prrte_asprintf(&param, "%ld", (long)jdata->num_procs);
    prrte_setenv("OMPI_MCA_prrte_ess_num_procs", param, true, &app->env);

    /* although the num_procs is the comm_world size, users
     * would appreciate being given a public environmental variable
     * that also represents this value - something MPI specific - so
     * do that here.
     *
     * AND YES - THIS BREAKS THE ABSTRACTION BARRIER TO SOME EXTENT.
     * We know - just live with it
     */
    prrte_setenv("OMPI_COMM_WORLD_SIZE", param, true, &app->env);
    free(param);

    /* users would appreciate being given a public environmental variable
     * that also represents this value - something MPI specific - so
     * do that here.
     *
     * AND YES - THIS BREAKS THE ABSTRACTION BARRIER TO SOME EXTENT.
     * We know - just live with it
     */
    prrte_asprintf(&param, "%ld", (long)jdata->num_local_procs);
    prrte_setenv("OMPI_COMM_WORLD_LOCAL_SIZE", param, true, &app->env);
    free(param);

    /* forcibly set the local tmpdir base and top session dir to match ours */
    prrte_setenv("OMPI_MCA_prrte_tmpdir_base", prrte_process_info.tmpdir_base, true, &app->env);
    /* TODO: should we use PMIx key to pass this data? */
    prrte_setenv("OMPI_MCA_prrte_top_session_dir", prrte_process_info.top_session_dir, true, &app->env);
    prrte_setenv("OMPI_MCA_prrte_jobfam_session_dir", prrte_process_info.jobfam_session_dir, true, &app->env);

    /* MPI-3 requires we provide some further info to the procs,
     * so we pass them as envars to avoid introducing further
     * PRRTE calls in the MPI layer
     */
    prrte_asprintf(&num_app_ctx, "%lu", (unsigned long)jdata->num_apps);

    /* build some common envars we need to pass for MPI-3 compatibility */
    nps = NULL;
    firstranks = NULL;
    for (i=0; i < jdata->apps->size; i++) {
        if (NULL == (tmp_app = (prrte_app_context_t*)prrte_pointer_array_get_item(jdata->apps, i))) {
            continue;
        }
        prrte_argv_append_nosize(&nps, PRRTE_VPID_PRINT(tmp_app->num_procs));
        prrte_argv_append_nosize(&firstranks, PRRTE_VPID_PRINT(tmp_app->first_rank));
    }
    npstring = prrte_argv_join(nps, ' ');
    firstrankstring = prrte_argv_join(firstranks, ' ');
    prrte_argv_free(nps);
    prrte_argv_free(firstranks);

    /* add the MPI-3 envars */
    prrte_setenv("OMPI_NUM_APP_CTX", num_app_ctx, true, &app->env);
    prrte_setenv("OMPI_FIRST_RANKS", firstrankstring, true, &app->env);
    prrte_setenv("OMPI_APP_CTX_NUM_PROCS", npstring, true, &app->env);
    free(num_app_ctx);
    free(firstrankstring);
    free(npstring);

    /* now process any envar attributes - we begin with the job-level
     * ones as the app-specific ones can override them. We have to
     * process them in the order they were given to ensure we wind
     * up in the desired final state */
    PRRTE_LIST_FOREACH(attr, &jdata->attributes, prrte_attribute_t) {
        if (PRRTE_JOB_SET_ENVAR == attr->key) {
            prrte_setenv(attr->data.envar.envar, attr->data.envar.value, true, &app->env);
        } else if (PRRTE_JOB_ADD_ENVAR == attr->key) {
            prrte_setenv(attr->data.envar.envar, attr->data.envar.value, false, &app->env);
        } else if (PRRTE_JOB_UNSET_ENVAR == attr->key) {
            prrte_unsetenv(attr->data.string, &app->env);
        } else if (PRRTE_JOB_PREPEND_ENVAR == attr->key) {
            /* see if the envar already exists */
            exists = false;
            for (i=0; NULL != app->env[i]; i++) {
                saveptr = strchr(app->env[i], '=');   // cannot be NULL
                *saveptr = '\0';
                if (0 == strcmp(app->env[i], attr->data.envar.envar)) {
                    /* we have the var - prepend it */
                    param = saveptr;
                    ++param;  // move past where the '=' sign was
                    prrte_asprintf(&p2, "%s%c%s", attr->data.envar.value,
                                   attr->data.envar.separator, param);
                    *saveptr = '=';  // restore the current envar setting
                    prrte_setenv(attr->data.envar.envar, p2, true, &app->env);
                    free(p2);
                    exists = true;
                    break;
                } else {
                    *saveptr = '=';  // restore the current envar setting
                }
            }
            if (!exists) {
                /* just insert it */
                prrte_setenv(attr->data.envar.envar, attr->data.envar.value, true, &app->env);
            }
        } else if (PRRTE_JOB_APPEND_ENVAR == attr->key) {
            /* see if the envar already exists */
            exists = false;
            for (i=0; NULL != app->env[i]; i++) {
                saveptr = strchr(app->env[i], '=');   // cannot be NULL
                *saveptr = '\0';
                if (0 == strcmp(app->env[i], attr->data.envar.envar)) {
                    /* we have the var - prepend it */
                    param = saveptr;
                    ++param;  // move past where the '=' sign was
                    prrte_asprintf(&p2, "%s%c%s", param, attr->data.envar.separator,
                                   attr->data.envar.value);
                    *saveptr = '=';  // restore the current envar setting
                    prrte_setenv(attr->data.envar.envar, p2, true, &app->env);
                    free(p2);
                    exists = true;
                    break;
                } else {
                    *saveptr = '=';  // restore the current envar setting
                }
            }
            if (!exists) {
                /* just insert it */
                prrte_setenv(attr->data.envar.envar, attr->data.envar.value, true, &app->env);
            }
        }
    }

    /* now do the same thing for any app-level attributes */
    PRRTE_LIST_FOREACH(attr, &app->attributes, prrte_attribute_t) {
        if (PRRTE_APP_SET_ENVAR == attr->key) {
            prrte_setenv(attr->data.envar.envar, attr->data.envar.value, true, &app->env);
        } else if (PRRTE_APP_ADD_ENVAR == attr->key) {
            prrte_setenv(attr->data.envar.envar, attr->data.envar.value, false, &app->env);
        } else if (PRRTE_APP_UNSET_ENVAR == attr->key) {
            prrte_unsetenv(attr->data.string, &app->env);
        } else if (PRRTE_APP_PREPEND_ENVAR == attr->key) {
            /* see if the envar already exists */
            exists = false;
            for (i=0; NULL != app->env[i]; i++) {
                saveptr = strchr(app->env[i], '=');   // cannot be NULL
                *saveptr = '\0';
                if (0 == strcmp(app->env[i], attr->data.envar.envar)) {
                    /* we have the var - prepend it */
                    param = saveptr;
                    ++param;  // move past where the '=' sign was
                    prrte_asprintf(&p2, "%s%c%s", attr->data.envar.value,
                                   attr->data.envar.separator, param);
                    *saveptr = '=';  // restore the current envar setting
                    prrte_setenv(attr->data.envar.envar, p2, true, &app->env);
                    free(p2);
                    exists = true;
                    break;
                } else {
                    *saveptr = '=';  // restore the current envar setting
                }
            }
            if (!exists) {
                /* just insert it */
                prrte_setenv(attr->data.envar.envar, attr->data.envar.value, true, &app->env);
            }
        } else if (PRRTE_APP_APPEND_ENVAR == attr->key) {
            /* see if the envar already exists */
            exists = false;
            for (i=0; NULL != app->env[i]; i++) {
                saveptr = strchr(app->env[i], '=');   // cannot be NULL
                *saveptr = '\0';
                if (0 == strcmp(app->env[i], attr->data.envar.envar)) {
                    /* we have the var - prepend it */
                    param = saveptr;
                    ++param;  // move past where the '=' sign was
                    prrte_asprintf(&p2, "%s%c%s", param, attr->data.envar.separator,
                                   attr->data.envar.value);
                    *saveptr = '=';  // restore the current envar setting
                    prrte_setenv(attr->data.envar.envar, p2, true, &app->env);
                    free(p2);
                    exists = true;
                    break;
                } else {
                    *saveptr = '=';  // restore the current envar setting
                }
            }
            if (!exists) {
                /* just insert it */
                prrte_setenv(attr->data.envar.envar, attr->data.envar.value, true, &app->env);
            }
        }
    }

    return PRRTE_SUCCESS;
}


static int setup_child(prrte_job_t *jdata,
                       prrte_proc_t *child,
                       prrte_app_context_t *app,
                       char ***env)
{
    char *param, *value;
    int rc, i;
    int32_t nrestarts=0, *nrptr;
    bool takeus = false;

    prrte_output_verbose(1, prrte_schizo_base_framework.framework_output,
                        "%s schizo:prrte: setup_child",
                        PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME));

    /* if no personality was specified, then nothing to do */
    if (NULL == jdata->personality) {
        return PRRTE_ERR_TAKE_NEXT_OPTION;
    }

    if (NULL != prrte_schizo_base.personalities) {
        /* see if we are included */
        for (i=0; NULL != jdata->personality[i]; i++) {
            if (0 == strcmp(jdata->personality[i], "prrte")) {
                takeus = true;
                break;
            }
        }
        if (!takeus) {
            return PRRTE_ERR_TAKE_NEXT_OPTION;
        }
    }

    /* setup the jobid */
    if (PRRTE_SUCCESS != (rc = prrte_util_convert_jobid_to_string(&value, child->name.jobid))) {
        PRRTE_ERROR_LOG(rc);
        return rc;
    }
    prrte_setenv("OMPI_MCA_ess_base_jobid", value, true, env);
    free(value);

    /* setup the vpid */
    if (PRRTE_SUCCESS != (rc = prrte_util_convert_vpid_to_string(&value, child->name.vpid))) {
        PRRTE_ERROR_LOG(rc);
        return rc;
    }
    prrte_setenv("OMPI_MCA_ess_base_vpid", value, true, env);

    /* although the vpid IS the process' rank within the job, users
     * would appreciate being given a public environmental variable
     * that also represents this value - something MPI specific - so
     * do that here.
     *
     * AND YES - THIS BREAKS THE ABSTRACTION BARRIER TO SOME EXTENT.
     * We know - just live with it
     */
    prrte_setenv("OMPI_COMM_WORLD_RANK", value, true, env);
    free(value);  /* done with this now */

    /* users would appreciate being given a public environmental variable
     * that also represents the local rank value - something MPI specific - so
     * do that here.
     *
     * AND YES - THIS BREAKS THE ABSTRACTION BARRIER TO SOME EXTENT.
     * We know - just live with it
     */
    if (PRRTE_LOCAL_RANK_INVALID == child->local_rank) {
        PRRTE_ERROR_LOG(PRRTE_ERR_VALUE_OUT_OF_BOUNDS);
        rc = PRRTE_ERR_VALUE_OUT_OF_BOUNDS;
        return rc;
    }
    prrte_asprintf(&value, "%lu", (unsigned long) child->local_rank);
    prrte_setenv("OMPI_COMM_WORLD_LOCAL_RANK", value, true, env);
    free(value);

    /* users would appreciate being given a public environmental variable
     * that also represents the node rank value - something MPI specific - so
     * do that here.
     *
     * AND YES - THIS BREAKS THE ABSTRACTION BARRIER TO SOME EXTENT.
     * We know - just live with it
     */
    if (PRRTE_NODE_RANK_INVALID == child->node_rank) {
        PRRTE_ERROR_LOG(PRRTE_ERR_VALUE_OUT_OF_BOUNDS);
        rc = PRRTE_ERR_VALUE_OUT_OF_BOUNDS;
        return rc;
    }
    prrte_asprintf(&value, "%lu", (unsigned long) child->node_rank);
    prrte_setenv("OMPI_COMM_WORLD_NODE_RANK", value, true, env);
    /* set an mca param for it too */
    prrte_setenv("OMPI_MCA_prrte_ess_node_rank", value, true, env);
    free(value);

    /* provide the identifier for the PMIx connection - the
     * PMIx connection is made prior to setting the process
     * name itself. Although in most cases the ID and the
     * process name are the same, it isn't necessarily
     * required */
    prrte_util_convert_process_name_to_string(&value, &child->name);
    prrte_setenv("PMIX_ID", value, true, env);
    free(value);

    nrptr = &nrestarts;
    if (prrte_get_attribute(&child->attributes, PRRTE_PROC_NRESTARTS, (void**)&nrptr, PRRTE_INT32)) {
        /* pass the number of restarts for this proc - will be zero for
         * an initial start, but procs would like to know if they are being
         * restarted so they can take appropriate action
         */
        prrte_asprintf(&value, "%d", nrestarts);
        prrte_setenv("OMPI_MCA_prrte_num_restarts", value, true, env);
        free(value);
    }

    /* if the proc should not barrier in prrte_init, tell it */
    if (prrte_get_attribute(&child->attributes, PRRTE_PROC_NOBARRIER, NULL, PRRTE_BOOL)
        || 0 < nrestarts) {
        prrte_setenv("OMPI_MCA_prrte_do_not_barrier", "1", true, env);
    }

    /* if the proc isn't going to forward IO, then we need to flag that
     * it has "completed" iof termination as otherwise it will never fire
     */
    if (!PRRTE_FLAG_TEST(jdata, PRRTE_JOB_FLAG_FORWARD_OUTPUT)) {
        PRRTE_FLAG_SET(child, PRRTE_PROC_FLAG_IOF_COMPLETE);
    }

    /* pass an envar so the proc can find any files it had prepositioned */
    param = prrte_process_info.proc_session_dir;
    prrte_setenv("OMPI_FILE_LOCATION", param, true, env);

    /* if the user wanted the cwd to be the proc's session dir, then
     * switch to that location now
     */
    if (prrte_get_attribute(&app->attributes, PRRTE_APP_SSNDIR_CWD, NULL, PRRTE_BOOL)) {
        /* create the session dir - may not exist */
        if (PRRTE_SUCCESS != (rc = prrte_os_dirpath_create(param, S_IRWXU))) {
            PRRTE_ERROR_LOG(rc);
            /* doesn't exist with correct permissions, and/or we can't
             * create it - either way, we are done
             */
            return rc;
        }
        /* change to it */
        if (0 != chdir(param)) {
            return PRRTE_ERROR;
        }
        /* It seems that chdir doesn't
         * adjust the $PWD enviro variable when it changes the directory. This
         * can cause a user to get a different response when doing getcwd vs
         * looking at the enviro variable. To keep this consistent, we explicitly
         * ensure that the PWD enviro variable matches the CWD we moved to.
         *
         * NOTE: if a user's program does a chdir(), then $PWD will once
         * again not match getcwd! This is beyond our control - we are only
         * ensuring they start out matching.
         */
        prrte_setenv("PWD", param, true, env);
        /* update the initial wdir value too */
        prrte_setenv("OMPI_MCA_initial_wdir", param, true, env);
    } else if (NULL != app->cwd) {
        /* change to it */
        if (0 != chdir(app->cwd)) {
            return PRRTE_ERROR;
        }
    }
    return PRRTE_SUCCESS;
}