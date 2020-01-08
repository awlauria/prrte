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

prrte_schizo_base_module_t prrte_schizo_ompi_module = {
    .parse_env = parse_env,
    .setup_fork = setup_fork,
    .setup_child = setup_child
};

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
                        "%s schizo:ompi: parse_env",
                        PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME));

    if (NULL != prrte_schizo_base.personalities) {
        /* see if we are included */
        for (i=0; NULL != prrte_schizo_base.personalities[i]; i++) {
            if (0 == strcmp(prrte_schizo_base.personalities[i], "ompi")) {
                takeus = true;
                break;
            }
        }
        if (!takeus) {
            return PRRTE_ERR_TAKE_NEXT_OPTION;
        }
    }

    for (i = 0; NULL != srcenv[i]; ++i) {
        if (0 == strncmp("OMPI_", srcenv[i], 5)) {
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
                        "%s schizo:ompi: setup_fork",
                        PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME));

    /* if no personality was specified, then nothing to do */
    if (NULL == jdata->personality) {
        return PRRTE_ERR_TAKE_NEXT_OPTION;
    }

    if (NULL != prrte_schizo_base.personalities) {
    /* see if we are included */
        for (i=0; NULL != jdata->personality[i]; i++) {
            if (0 == strcmp(jdata->personality[i], "ompi")) {
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
     * do that here. Also required by the ompi_attributes code!
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
                        "%s schizo:ompi: setup_child",
                        PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME));

    /* if no personality was specified, then nothing to do */
    if (NULL == jdata->personality) {
        return PRRTE_ERR_TAKE_NEXT_OPTION;
    }

    if (NULL != prrte_schizo_base.personalities) {
        /* see if we are included */
        for (i=0; NULL != jdata->personality[i]; i++) {
            if (0 == strcmp(jdata->personality[i], "ompi")) {
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