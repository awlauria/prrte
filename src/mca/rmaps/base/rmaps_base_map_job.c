/*
 * Copyright (c) 2004-2005 The Trustees of Indiana University and Indiana
 *                         University Research and Technology
 *                         Corporation.  All rights reserved.
 * Copyright (c) 2004-2018 The University of Tennessee and The University
 *                         of Tennessee Research Foundation.  All rights
 *                         reserved.
 * Copyright (c) 2004-2005 High Performance Computing Center Stuttgart,
 *                         University of Stuttgart.  All rights reserved.
 * Copyright (c) 2004-2005 The Regents of the University of California.
 *                         All rights reserved.
 * Copyright (c) 2011-2020 Cisco Systems, Inc.  All rights reserved
 * Copyright (c) 2011-2012 Los Alamos National Security, LLC.
 *                         All rights reserved.
 * Copyright (c) 2014-2020 Intel, Inc.  All rights reserved.
 * Copyright (c) 2016      Research Organization for Information Science
 *                         and Technology (RIST). All rights reserved.
 * Copyright (c) 2019      UT-Battelle, LLC. All rights reserved.
 *
 * Copyright (c) 2021      Nanook Consulting.  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#include "prte_config.h"
#include "constants.h"

#include <string.h>

#include "src/mca/mca.h"
#include "src/util/argv.h"
#include "src/util/output.h"
#include "src/util/string_copy.h"
#include "src/mca/base/base.h"
#include "src/hwloc/hwloc-internal.h"

#include "src/mca/errmgr/errmgr.h"
#include "src/mca/ras/base/base.h"
#include "src/runtime/prte_globals.h"
#include "src/util/show_help.h"
#include "src/threads/threads.h"
#include "src/mca/state/state.h"

#include "src/mca/rmaps/base/base.h"
#include "src/mca/rmaps/base/rmaps_private.h"


void prte_rmaps_base_map_job(int fd, short args, void *cbdata)
{
    prte_state_caddy_t *caddy = (prte_state_caddy_t*)cbdata;
    prte_job_t *jdata;
    prte_node_t *node;
    int rc, i;
    bool did_map, pernode = false, perpackage = false;
    prte_rmaps_base_selected_module_t *mod;
    prte_job_t *parent = NULL;
    pmix_rank_t nprocs;
    prte_app_context_t *app;
    bool inherit = false;
    pmix_proc_t name, *nptr;
    char *tmp, *p;
    uint16_t u16 = 0;
    uint16_t *u16ptr = &u16, cpus_per_rank;
    bool use_hwthreads = false;
    bool sequential = false;
    int32_t slots;

    PRTE_ACQUIRE_OBJECT(caddy);
    jdata = caddy->jdata;

    jdata->state = PRTE_JOB_STATE_MAP;

    prte_output_verbose(5, prte_rmaps_base_framework.framework_output,
                        "mca:rmaps: mapping job %s",
                        PRTE_JOBID_PRINT(jdata->nspace));

    /* if this is a dynamic job launch and they didn't explicitly
     * request inheritance, then don't inherit the launch directives */
    nptr = &name;
    if (prte_get_attribute(&jdata->attributes, PRTE_JOB_LAUNCH_PROXY, (void**)&nptr, PMIX_PROC)) {
        /* if the launch proxy is me, then this is the initial launch from
         * a proxy scenario, so we don't really have a parent */
        if (PMIX_CHECK_NSPACE(PRTE_PROC_MY_NAME->nspace, name.nspace)) {
            parent = NULL;
            /* we do allow inheritance of the defaults */
            inherit = true;
        } else if (NULL != (parent = prte_get_job_data_object(name.nspace))) {
            if (prte_get_attribute(&jdata->attributes, PRTE_JOB_INHERIT, NULL, PMIX_BOOL)) {
                inherit = true;
            } else if (prte_get_attribute(&jdata->attributes, PRTE_JOB_NOINHERIT, NULL, PMIX_BOOL)) {
                inherit = false;
                parent = NULL;
            } else if (PRTE_FLAG_TEST(parent, PRTE_JOB_FLAG_TOOL)) {
                /* ensure we inherit the defaults as this is equivalent to an initial launch */
                inherit = true;
                parent = NULL;
            } else {
                inherit = prte_rmaps_base.inherit;
            }
            prte_output_verbose(5, prte_rmaps_base_framework.framework_output,
                                "mca:rmaps: dynamic job %s %s inherit launch directives - parent %s is %s",
                                PRTE_JOBID_PRINT(jdata->nspace),
                                inherit ? "will" : "will not",
                                (NULL == parent) ? "N/A" : PRTE_JOBID_PRINT((parent->nspace)),
                                (NULL == parent) ? "NULL" : ((PRTE_FLAG_TEST(parent, PRTE_JOB_FLAG_TOOL) ? "TOOL" : "NON-TOOL")));
        } else {
            inherit = true;
        }
    } else {
        /* initial launch always takes on default MCA params for non-specified policies */
        inherit = true;
    }

    if (NULL == jdata->map) {
        jdata->map = PRTE_NEW(prte_job_map_t);
    }

    if (inherit && NULL != parent) {
        /* if not already assigned, inherit the parent's ppr */
        if (!prte_get_attribute(&jdata->attributes, PRTE_JOB_PPR, NULL, PMIX_STRING)) {
            /* get the parent job's ppr, if it had one */
            if (prte_get_attribute(&parent->attributes, PRTE_JOB_PPR, (void**)&tmp, PMIX_STRING)) {
                prte_set_attribute(&jdata->attributes, PRTE_ATTR_GLOBAL, PRTE_JOB_PPR, tmp, PMIX_STRING);
                free(tmp);
            }
        }
        /* if not already assigned, inherit the parent's pes/proc */
        if (!prte_get_attribute(&jdata->attributes, PRTE_JOB_PES_PER_PROC, NULL, PMIX_UINT16)) {
            /* get the parent job's pes/proc, if it had one */
            if (prte_get_attribute(&parent->attributes, PRTE_JOB_PES_PER_PROC, (void**)&u16ptr, PMIX_UINT16)) {
                prte_set_attribute(&jdata->attributes, PRTE_ATTR_GLOBAL, PRTE_JOB_PES_PER_PROC, u16ptr, PMIX_UINT16);

            }
        }
        /* if not already assigned, inherit the parent's cpu designation */
        if (!prte_get_attribute(&jdata->attributes, PRTE_JOB_HWT_CPUS, NULL, PMIX_BOOL) &&
            !prte_get_attribute(&jdata->attributes, PRTE_JOB_CORE_CPUS, NULL, PMIX_BOOL)) {
            /* get the parent job's designation, if it had one */
            if (prte_get_attribute(&parent->attributes, PRTE_JOB_HWT_CPUS, NULL, PMIX_BOOL)) {
                prte_set_attribute(&jdata->attributes, PRTE_ATTR_GLOBAL, PRTE_JOB_HWT_CPUS, NULL, PMIX_BOOL);
            } else if (prte_get_attribute(&parent->attributes, PRTE_JOB_CORE_CPUS, NULL, PMIX_BOOL)) {
                prte_set_attribute(&jdata->attributes, PRTE_ATTR_GLOBAL, PRTE_JOB_CORE_CPUS, NULL, PMIX_BOOL);
            } else {
                /* default */
                if (prte_rmaps_base.hwthread_cpus) {
                    prte_set_attribute(&jdata->attributes, PRTE_ATTR_GLOBAL, PRTE_JOB_HWT_CPUS, NULL, PMIX_BOOL);
                } else {
                    prte_set_attribute(&jdata->attributes, PRTE_ATTR_GLOBAL, PRTE_JOB_CORE_CPUS, NULL, PMIX_BOOL);
                }
            }
        }
    }

    /* we always inherit a parent's oversubscribe flag unless the job assigned it */
    if (NULL != parent &&
        !(PRTE_MAPPING_SUBSCRIBE_GIVEN & PRTE_GET_MAPPING_DIRECTIVE(jdata->map->mapping))) {
        if (PRTE_MAPPING_NO_OVERSUBSCRIBE & PRTE_GET_MAPPING_DIRECTIVE(parent->map->mapping)) {
            PRTE_SET_MAPPING_DIRECTIVE(jdata->map->mapping, PRTE_MAPPING_NO_OVERSUBSCRIBE);
        } else {
            PRTE_UNSET_MAPPING_DIRECTIVE(jdata->map->mapping, PRTE_MAPPING_NO_OVERSUBSCRIBE);
            PRTE_SET_MAPPING_DIRECTIVE(jdata->map->mapping, PRTE_MAPPING_SUBSCRIBE_GIVEN);
        }
    }

    if (prte_get_attribute(&jdata->attributes, PRTE_JOB_PPR, (void**)&tmp, PMIX_STRING)) {
        if (NULL != strcasestr(tmp, "node")) {
            pernode = true;
            /* get the ppn */
            if (NULL == (p = strchr(tmp, ':'))) {
                /* should never happen */
                PRTE_ERROR_LOG(PRTE_ERR_BAD_PARAM);
                jdata->exit_code = PRTE_ERR_BAD_PARAM;
                PRTE_ACTIVATE_JOB_STATE(jdata, PRTE_JOB_STATE_MAP_FAILED);
                goto cleanup;
            }
            ++p;  // step over the colon
            u16 = strtoul(p, NULL, 10);
        } else if (NULL != strcasestr(tmp, "package")) {
            perpackage = true;
            /* get the ppn */
            if (NULL == (p = strchr(tmp, ':'))) {
                /* should never happen */
                PRTE_ERROR_LOG(PRTE_ERR_BAD_PARAM);
                jdata->exit_code = PRTE_ERR_BAD_PARAM;
                PRTE_ACTIVATE_JOB_STATE(jdata, PRTE_JOB_STATE_MAP_FAILED);
                goto cleanup;
            }
            ++p;  // step over the colon
            u16 = strtoul(p, NULL, 10);
        }
        free(tmp);
    } else if (PRTE_MAPPING_SEQ == PRTE_GET_MAPPING_POLICY(jdata->map->mapping)) {
        sequential = true;
    } else if (PRTE_MAPPING_BYUSER == PRTE_GET_MAPPING_POLICY(jdata->map->mapping)) {
        /* defer */
        nprocs = 10; // number doesn't matter as long as it is > 2
        goto compute;
    }

    /* estimate the number of procs for assigning default mapping/ranking policies */
    nprocs = 0;
    for (i=0; i < jdata->apps->size; i++) {
        if (NULL != (app = (prte_app_context_t*)prte_pointer_array_get_item(jdata->apps, i))) {
            if (0 == app->num_procs) {
                prte_list_t nodes;
                PRTE_CONSTRUCT(&nodes, prte_list_t);
                prte_rmaps_base_get_target_nodes(&nodes, &slots, app, PRTE_MAPPING_BYNODE, true, true);
                if (pernode) {
                    slots = u16 * prte_list_get_size(&nodes);
                } else if (perpackage) {
                    /* add in #packages for each node */
                    PRTE_LIST_FOREACH(node, &nodes, prte_node_t) {
                        slots += u16 * prte_hwloc_base_get_nbobjs_by_type(node->topology->topo,
                                                                          HWLOC_OBJ_PACKAGE, 0);
                    }
                } else if (sequential) {
                    slots = prte_list_get_size(&nodes);
                }
                PRTE_LIST_DESTRUCT(&nodes);
            } else {
                slots = app->num_procs;
            }
            nprocs += slots;
        }
    }

compute:
    /* set some convenience params */
    if (prte_get_attribute(&jdata->attributes, PRTE_JOB_PES_PER_PROC, (void**)&u16ptr, PMIX_UINT16)) {
        cpus_per_rank = u16;
    } else {
        cpus_per_rank = 1;
    }
    if (prte_get_attribute(&jdata->attributes, PRTE_JOB_HWT_CPUS, NULL, PMIX_BOOL)) {
        use_hwthreads = true;
    }

    prte_output_verbose(5, prte_rmaps_base_framework.framework_output,
                        "mca:rmaps: setting mapping policies for job %s nprocs %d",
                        PRTE_JOBID_PRINT(jdata->nspace), (int)nprocs);

    /* set the default mapping policy IFF it wasn't provided */
    if (!PRTE_MAPPING_POLICY_IS_SET(jdata->map->mapping)) {
        did_map = false;
        if (inherit) {
            if (NULL != parent) {
                jdata->map->mapping = parent->map->mapping;
                did_map = true;
            } else if (PRTE_MAPPING_GIVEN & PRTE_GET_MAPPING_DIRECTIVE(prte_rmaps_base.mapping)) {
                prte_output_verbose(5, prte_rmaps_base_framework.framework_output,
                                    "mca:rmaps mapping given by MCA param");
                jdata->map->mapping = prte_rmaps_base.mapping;
                did_map = true;
            }
        }
        if (!did_map) {
            /* default based on number of procs */
            if (nprocs <= 2) {
                if (1 < cpus_per_rank) {
                    /* assigning multiple cpus to a rank requires that we map to
                     * objects that have multiple cpus in them, so default
                     * to byslot if nothing else was specified by the user.
                     */
                    prte_output_verbose(5, prte_rmaps_base_framework.framework_output,
                                        "mca:rmaps[%d] mapping not given - using byslot", __LINE__);
                    PRTE_SET_MAPPING_POLICY(jdata->map->mapping, PRTE_MAPPING_BYSLOT);
                } else if (use_hwthreads) {
                    prte_output_verbose(5, prte_rmaps_base_framework.framework_output,
                                        "mca:rmaps[%d] mapping not given - using byhwthread", __LINE__);
                    PRTE_SET_MAPPING_POLICY(jdata->map->mapping, PRTE_MAPPING_BYHWTHREAD);
                } else {
                    prte_output_verbose(5, prte_rmaps_base_framework.framework_output,
                                        "mca:rmaps[%d] mapping not given - using bycore", __LINE__);
                    PRTE_SET_MAPPING_POLICY(jdata->map->mapping, PRTE_MAPPING_BYCORE);
                }
            } else {
                /* if package is available, map by that */
                if (NULL != hwloc_get_obj_by_type(prte_hwloc_topology, HWLOC_OBJ_PACKAGE, 0)) {
                    prte_output_verbose(5, prte_rmaps_base_framework.framework_output,
                                        "mca:rmaps[%d] mapping not set by user - using bypackage", __LINE__);
                    PRTE_SET_MAPPING_POLICY(jdata->map->mapping, PRTE_MAPPING_BYPACKAGE);
                } else {
                    /* if we have neither, then just do by slot */
                    prte_output_verbose(5, prte_rmaps_base_framework.framework_output,
                                        "mca:rmaps[%d] mapping not given and no packages - using byslot", __LINE__);
                    PRTE_SET_MAPPING_POLICY(jdata->map->mapping, PRTE_MAPPING_BYSLOT);
                }
            }
        }
    }

    /* check for oversubscribe directives */
    if (!(PRTE_MAPPING_SUBSCRIBE_GIVEN & PRTE_GET_MAPPING_DIRECTIVE(jdata->map->mapping))) {
        if (!(PRTE_MAPPING_SUBSCRIBE_GIVEN & PRTE_GET_MAPPING_DIRECTIVE(prte_rmaps_base.mapping))) {
            PRTE_SET_MAPPING_DIRECTIVE(jdata->map->mapping, PRTE_MAPPING_NO_OVERSUBSCRIBE);
        } else if (PRTE_MAPPING_NO_OVERSUBSCRIBE & PRTE_GET_MAPPING_DIRECTIVE(prte_rmaps_base.mapping)) {
            PRTE_SET_MAPPING_DIRECTIVE(jdata->map->mapping, PRTE_MAPPING_NO_OVERSUBSCRIBE);
        } else {
            PRTE_UNSET_MAPPING_DIRECTIVE(jdata->map->mapping, PRTE_MAPPING_NO_OVERSUBSCRIBE);
            PRTE_SET_MAPPING_DIRECTIVE(jdata->map->mapping, PRTE_MAPPING_SUBSCRIBE_GIVEN);
        }
    }

    /* check for no-use-local directive */
    if (prte_ras_base.launch_orted_on_hn) {
        /* must override any setting */
        PRTE_SET_MAPPING_DIRECTIVE(jdata->map->mapping, PRTE_MAPPING_NO_USE_LOCAL);
    } else if (!(PRTE_MAPPING_LOCAL_GIVEN & PRTE_GET_MAPPING_DIRECTIVE(jdata->map->mapping))) {
        if (inherit && (PRTE_MAPPING_NO_USE_LOCAL & PRTE_GET_MAPPING_DIRECTIVE(prte_rmaps_base.mapping))) {
            PRTE_SET_MAPPING_DIRECTIVE(jdata->map->mapping, PRTE_MAPPING_NO_USE_LOCAL);
        }
    }

    /* set the default ranking policy IFF it wasn't provided */
    if (!PRTE_RANKING_POLICY_IS_SET(jdata->map->ranking)) {
        did_map = false;
        if (inherit) {
            if (NULL != parent) {
                jdata->map->ranking = parent->map->ranking;
                did_map = true;
            } else if (PRTE_RANKING_GIVEN & PRTE_GET_RANKING_DIRECTIVE(prte_rmaps_base.ranking)) {
                prte_output_verbose(5, prte_rmaps_base_framework.framework_output,
                                     "mca:rmaps ranking given by MCA param");
                jdata->map->ranking = prte_rmaps_base.ranking;
                did_map = true;
            }
        }
        if (!did_map) {
            PRTE_SET_RANKING_POLICY(jdata->map->ranking, PRTE_RANK_BY_SLOT);
        }
    }

    /* define the binding policy for this job - if the user specified one
     * already (e.g., during the call to comm_spawn), then we don't
     * override it */
    if (!PRTE_BINDING_POLICY_IS_SET(jdata->map->binding)) {
        did_map = false;
        if (inherit) {
            if (NULL != parent) {
                jdata->map->binding = parent->map->binding;
                did_map = true;
            } else if (PRTE_BINDING_POLICY_IS_SET(prte_hwloc_default_binding_policy)) {
                /* if the user specified a default binding policy via
                 * MCA param, then we use it - this can include a directive
                 * to overload */
                prte_output_verbose(5, prte_rmaps_base_framework.framework_output,
                                "mca:rmaps[%d] default binding policy given", __LINE__);
                jdata->map->binding = prte_hwloc_default_binding_policy;
                did_map = true;
            }
        }
        if (!did_map) {
            if (prte_get_attribute(&jdata->attributes, PRTE_JOB_PES_PER_PROC, NULL, PMIX_UINT16)) {
                /* bind to cpus */
                if (use_hwthreads) {
                    /* if we are using hwthread cpus, then bind to those */
                    prte_output_verbose(5, prte_rmaps_base_framework.framework_output,
                                    "mca:rmaps[%d] binding not given - using byhwthread", __LINE__);
                    PRTE_SET_DEFAULT_BINDING_POLICY(jdata->map->binding, PRTE_BIND_TO_HWTHREAD);
                } else {
                    /* bind to core */
                    prte_output_verbose(5, prte_rmaps_base_framework.framework_output,
                                    "mca:rmaps[%d] binding not given - using bycore", __LINE__);
                    PRTE_SET_DEFAULT_BINDING_POLICY(jdata->map->binding, PRTE_BIND_TO_CORE);
                }
            } else {
                /* if the user explicitly mapped-by some object, then we default
                 * to binding to that object */
                prte_mapping_policy_t mpol;
                mpol = PRTE_GET_MAPPING_POLICY(jdata->map->mapping);
                if (PRTE_MAPPING_GIVEN & PRTE_GET_MAPPING_DIRECTIVE(jdata->map->mapping)) {
                    if (PRTE_MAPPING_BYHWTHREAD == mpol) {
                        prte_output_verbose(5, prte_rmaps_base_framework.framework_output,
                                        "mca:rmaps[%d] binding not given - using byhwthread", __LINE__);
                        PRTE_SET_DEFAULT_BINDING_POLICY(jdata->map->binding, PRTE_BIND_TO_HWTHREAD);
                    } else if (PRTE_MAPPING_BYCORE == mpol) {
                        prte_output_verbose(5, prte_rmaps_base_framework.framework_output,
                                        "mca:rmaps[%d] binding not given - using bycore", __LINE__);
                        PRTE_SET_DEFAULT_BINDING_POLICY(jdata->map->binding, PRTE_BIND_TO_CORE);
                    } else if (PRTE_MAPPING_BYL1CACHE == mpol) {
                        prte_output_verbose(5, prte_rmaps_base_framework.framework_output,
                                        "mca:rmaps[%d] binding not given - using byL1", __LINE__);
                        PRTE_SET_DEFAULT_BINDING_POLICY(jdata->map->binding, PRTE_BIND_TO_L1CACHE);
                    } else if (PRTE_MAPPING_BYL2CACHE == mpol) {
                        prte_output_verbose(5, prte_rmaps_base_framework.framework_output,
                                        "mca:rmaps[%d] binding not given - using byL2", __LINE__);
                        PRTE_SET_DEFAULT_BINDING_POLICY(jdata->map->binding, PRTE_BIND_TO_L2CACHE);
                    } else if (PRTE_MAPPING_BYL3CACHE == mpol) {
                        prte_output_verbose(5, prte_rmaps_base_framework.framework_output,
                                        "mca:rmaps[%d] binding not given - using byL3", __LINE__);
                        PRTE_SET_DEFAULT_BINDING_POLICY(jdata->map->binding, PRTE_BIND_TO_L3CACHE);
                    } else if (PRTE_MAPPING_BYPACKAGE == mpol) {
                        prte_output_verbose(5, prte_rmaps_base_framework.framework_output,
                                        "mca:rmaps[%d] binding not given - using bypackage", __LINE__);
                        PRTE_SET_DEFAULT_BINDING_POLICY(jdata->map->binding, PRTE_BIND_TO_PACKAGE);
                    } else {
                        /* we are mapping by node or some other non-object method */
                        if (nprocs <= 2) {
                            if (use_hwthreads) {
                                /* if we are using hwthread cpus, then bind to those */
                                prte_output_verbose(5, prte_rmaps_base_framework.framework_output,
                                                "mca:rmaps[%d] binding not given - using byhwthread", __LINE__);
                                PRTE_SET_DEFAULT_BINDING_POLICY(jdata->map->binding, PRTE_BIND_TO_HWTHREAD);
                            } else {
                                /* for performance, bind to core */
                                prte_output_verbose(5, prte_rmaps_base_framework.framework_output,
                                                "mca:rmaps[%d] binding not given - using bycore", __LINE__);
                                PRTE_SET_DEFAULT_BINDING_POLICY(jdata->map->binding, PRTE_BIND_TO_CORE);
                            }
                        } else {
                            if (NULL != hwloc_get_obj_by_type(prte_hwloc_topology, HWLOC_OBJ_PACKAGE, 0)) {
                                prte_output_verbose(5, prte_rmaps_base_framework.framework_output,
                                                    "mca:rmaps[%d] binding not given - using bypackage", __LINE__);
                                PRTE_SET_DEFAULT_BINDING_POLICY(jdata->map->binding, PRTE_BIND_TO_PACKAGE);
                            } else {
                                /* if we have neither, then just don't bind */
                                prte_output_verbose(5, prte_rmaps_base_framework.framework_output,
                                                    "mca:rmaps[%d] binding not given and no NUMA or packages - not binding", __LINE__);
                                PRTE_SET_BINDING_POLICY(jdata->map->binding, PRTE_BIND_TO_NONE);
                            }
                        }
                    }
                } else if (nprocs <= 2) {
                    if (use_hwthreads) {
                        /* if we are using hwthread cpus, then bind to those */
                        prte_output_verbose(5, prte_rmaps_base_framework.framework_output,
                                        "mca:rmaps[%d] binding not given - using byhwthread", __LINE__);
                        PRTE_SET_DEFAULT_BINDING_POLICY(jdata->map->binding, PRTE_BIND_TO_HWTHREAD);
                    } else {
                        /* for performance, bind to core */
                        prte_output_verbose(5, prte_rmaps_base_framework.framework_output,
                                        "mca:rmaps[%d] binding not given - using bycore", __LINE__);
                        PRTE_SET_DEFAULT_BINDING_POLICY(jdata->map->binding, PRTE_BIND_TO_CORE);
                    }
                } else {
                    /* for performance, bind to package, if available */
                    if (NULL != hwloc_get_obj_by_type(prte_hwloc_topology, HWLOC_OBJ_PACKAGE, 0)) {
                        prte_output_verbose(5, prte_rmaps_base_framework.framework_output,
                                            "mca:rmaps[%d] binding not given - using bypackage", __LINE__);
                        PRTE_SET_DEFAULT_BINDING_POLICY(jdata->map->binding, PRTE_BIND_TO_PACKAGE);
                    } else {
                        /* just don't bind */
                        prte_output_verbose(5, prte_rmaps_base_framework.framework_output,
                                            "mca:rmaps[%d] binding not given and no packages - not binding", __LINE__);
                        PRTE_SET_BINDING_POLICY(jdata->map->binding, PRTE_BIND_TO_NONE);
                    }
                }
            }
            if (PRTE_BIND_OVERLOAD_ALLOWED(prte_hwloc_default_binding_policy)) {
                jdata->map->binding |= PRTE_BIND_ALLOW_OVERLOAD;
            }
        }
    }

    /* if we are not going to launch, then we need to set any
     * undefined topologies to match our own so the mapper
     * can operate
     */
    if (prte_get_attribute(&jdata->attributes, PRTE_JOB_DO_NOT_LAUNCH, NULL, PMIX_BOOL)) {
        prte_node_t *node;
        prte_topology_t *t0;
        int i;
        if (NULL == (node = (prte_node_t*)prte_pointer_array_get_item(prte_node_pool, 0))) {
            PRTE_ERROR_LOG(PRTE_ERR_NOT_FOUND);
            PRTE_RELEASE(caddy);
            jdata->exit_code = PRTE_ERR_NOT_FOUND;
            PRTE_ACTIVATE_JOB_STATE(jdata, PRTE_JOB_STATE_MAP_FAILED);
            return;
        }
        t0 = node->topology;
        for (i=1; i < prte_node_pool->size; i++) {
            if (NULL == (node = (prte_node_t*)prte_pointer_array_get_item(prte_node_pool, i))) {
                continue;
            }
            if (NULL == node->topology) {
                node->topology = t0;
            }
        }
    }

    /* cycle thru the available mappers until one agrees to map
     * the job
     */
    did_map = false;
    if (1 == prte_list_get_size(&prte_rmaps_base.selected_modules)) {
        /* forced selection */
        mod = (prte_rmaps_base_selected_module_t*)prte_list_get_first(&prte_rmaps_base.selected_modules);
        jdata->map->req_mapper = strdup(mod->component->mca_component_name);
    }
    PRTE_LIST_FOREACH(mod, &prte_rmaps_base.selected_modules, prte_rmaps_base_selected_module_t) {
        if (PRTE_SUCCESS == (rc = mod->module->map_job(jdata)) ||
            PRTE_ERR_RESOURCE_BUSY == rc) {
            did_map = true;
            break;
        }
        /* mappers return "next option" if they didn't attempt to
         * map the job. anything else is a true error.
         */
        if (PRTE_ERR_TAKE_NEXT_OPTION != rc) {
            jdata->exit_code = rc;
            PRTE_ACTIVATE_JOB_STATE(jdata, PRTE_JOB_STATE_MAP_FAILED);
            goto cleanup;
        }
    }

    if (did_map && PRTE_ERR_RESOURCE_BUSY == rc) {
        /* the map was done but nothing could be mapped
         * for launch as all the resources were busy
         */
        prte_show_help("help-prte-rmaps-base.txt", "cannot-launch", true);
        jdata->exit_code = rc;
        PRTE_ACTIVATE_JOB_STATE(jdata, PRTE_JOB_STATE_MAP_FAILED);
        goto cleanup;
    }

    /* if we get here without doing the map, or with zero procs in
     * the map, then that's an error
     */
    if (!did_map || 0 == jdata->num_procs || 0 == jdata->map->num_nodes) {
        prte_show_help("help-prte-rmaps-base.txt", "failed-map", true,
                       did_map ? "mapped" : "unmapped",
                       jdata->num_procs, jdata->map->num_nodes);
        jdata->exit_code = -PRTE_JOB_STATE_MAP_FAILED;
        PRTE_ACTIVATE_JOB_STATE(jdata, PRTE_JOB_STATE_MAP_FAILED);
        goto cleanup;
    }

    /* if any node is oversubscribed, then check to see if a binding
     * directive was given - if not, then we want to clear the default
     * binding policy so we don't attempt to bind */
    if (PRTE_FLAG_TEST(jdata, PRTE_JOB_FLAG_OVERSUBSCRIBED)) {
        if (!PRTE_BINDING_POLICY_IS_SET(jdata->map->binding)) {
            /* clear any default binding policy we might have set */
            PRTE_SET_DEFAULT_BINDING_POLICY(jdata->map->binding, PRTE_BIND_TO_NONE);
        }
    }

    /* compute the ranks and add the proc objects
     * to the jdata->procs array */
    if (PRTE_SUCCESS != (rc = prte_rmaps_base_compute_vpids(jdata))) {
        PRTE_ERROR_LOG(rc);
        jdata->exit_code = rc;
        PRTE_ACTIVATE_JOB_STATE(jdata, PRTE_JOB_STATE_MAP_FAILED);
        goto cleanup;
    }

    if (prte_get_attribute(&jdata->attributes, PRTE_JOB_DO_NOT_LAUNCH, NULL, PMIX_BOOL) ||
        prte_get_attribute(&jdata->attributes, PRTE_JOB_DISPLAY_MAP, NULL, PMIX_BOOL) ||
        prte_get_attribute(&jdata->attributes, PRTE_JOB_DISPLAY_DEVEL_MAP, NULL, PMIX_BOOL) ||
        prte_get_attribute(&jdata->attributes, PRTE_JOB_DISPLAY_DIFF, NULL, PMIX_BOOL)) {
        /* compute and save local ranks */
        if (PRTE_SUCCESS != (rc = prte_rmaps_base_compute_local_ranks(jdata))) {
            PRTE_ERROR_LOG(rc);
            jdata->exit_code = rc;
            PRTE_ACTIVATE_JOB_STATE(jdata, PRTE_JOB_STATE_MAP_FAILED);
            goto cleanup;
        }
        /* compute and save bindings */
        if (PRTE_SUCCESS != (rc = prte_rmaps_base_compute_bindings(jdata))) {
            PRTE_ERROR_LOG(rc);
            jdata->exit_code = rc;
            PRTE_ACTIVATE_JOB_STATE(jdata, PRTE_JOB_STATE_MAP_FAILED);
            goto cleanup;
        }
    } else if (prte_get_attribute(&jdata->attributes, PRTE_JOB_FULLY_DESCRIBED, NULL, PMIX_BOOL)) {
        /* compute and save local ranks */
        if (PRTE_SUCCESS != (rc = prte_rmaps_base_compute_local_ranks(jdata))) {
            PRTE_ERROR_LOG(rc);
            jdata->exit_code = rc;
            PRTE_ACTIVATE_JOB_STATE(jdata, PRTE_JOB_STATE_MAP_FAILED);
            goto cleanup;
        }

        /* compute and save bindings */
        if (PRTE_SUCCESS != (rc = prte_rmaps_base_compute_bindings(jdata))) {
            PRTE_ERROR_LOG(rc);
            jdata->exit_code = rc;
            PRTE_ACTIVATE_JOB_STATE(jdata, PRTE_JOB_STATE_MAP_FAILED);
            goto cleanup;
        }
    }

    /* set the offset so shared memory components can potentially
     * connect to any spawned jobs
     */
    jdata->offset = prte_total_procs;
    /* track the total number of procs launched by us */
    prte_total_procs += jdata->num_procs;

    /* if it is a dynamic spawn, save the bookmark on the parent's job too */
    if (!PMIX_NSPACE_INVALID(jdata->originator.nspace)) {
        if (NULL != (parent = prte_get_job_data_object(jdata->originator.nspace))) {
            parent->bookmark = jdata->bookmark;
        }
    }

    if (prte_get_attribute(&jdata->attributes, PRTE_JOB_DISPLAY_MAP, NULL, PMIX_BOOL) ||
        prte_get_attribute(&jdata->attributes, PRTE_JOB_DISPLAY_DEVEL_MAP, NULL, PMIX_BOOL) ||
        prte_get_attribute(&jdata->attributes, PRTE_JOB_DISPLAY_DIFF, NULL, PMIX_BOOL)) {
        /* display the map */
        prte_rmaps_base_display_map(jdata);
    }

    /* set the job state to the next position */
    PRTE_ACTIVATE_JOB_STATE(jdata, PRTE_JOB_STATE_MAP_COMPLETE);

  cleanup:
      /* reset any node map flags we used so the next job will start clean */
       for (i=0; i < jdata->map->nodes->size; i++) {
           if (NULL != (node = (prte_node_t*)prte_pointer_array_get_item(jdata->map->nodes, i))) {
               PRTE_FLAG_UNSET(node, PRTE_NODE_FLAG_MAPPED);
           }
       }

    /* cleanup */
    PRTE_RELEASE(caddy);
}

void prte_rmaps_base_display_map(prte_job_t *jdata)
{
    /* ignore daemon job */
    char *output=NULL;
    int i, j, cnt;
    prte_node_t *node;
    prte_proc_t *proc;
    char *tmp1;
    hwloc_obj_t bd=NULL;;
    prte_hwloc_locality_t locality;
    prte_proc_t *p0;
    char *p0bitmap, *procbitmap;

    /* only have rank=0 output this */
    if (0 != PRTE_PROC_MY_NAME->rank) {
        return;
    }

    if (prte_get_attribute(&jdata->attributes, PRTE_JOB_DISPLAY_DIFF, NULL, PMIX_BOOL)) {
        /* intended solely to test mapping methods, this output
         * can become quite long when testing at scale. Rather
         * than enduring all the malloc/free's required to
         * create an arbitrary-length string, custom-generate
         * the output a line at a time here
         */
        /* display just the procs in a diffable format */
        prte_output(prte_clean_output, "<map>\n");
        fflush(stderr);
        /* loop through nodes */
        cnt = 0;
        for (i=0; i < jdata->map->nodes->size; i++) {
            if (NULL == (node = (prte_node_t*)prte_pointer_array_get_item(jdata->map->nodes, i))) {
                continue;
            }
            prte_output(prte_clean_output, "\t<host num=%d>", cnt);
            fflush(stderr);
            cnt++;
            for (j=0; j < node->procs->size; j++) {
                if (NULL == (proc = (prte_proc_t*)prte_pointer_array_get_item(node->procs, j))) {
                    continue;
                }
                if (proc->job != jdata) {
                    continue;
                }
                if (prte_get_attribute(&proc->attributes, PRTE_PROC_HWLOC_BOUND, (void**)&bd, PMIX_POINTER)) {
                    if (NULL == bd) {
                        tmp1 = strdup("UNBOUND");
                    } else {
                        tmp1 = prte_hwloc_base_cset2str(bd->cpuset, false, node->topology->topo);
                    }
                } else {
                    tmp1 = strdup("UNBOUND");
                }
                prte_output(prte_clean_output, "\t\t<process rank=%s app_idx=%ld local_rank=%lu node_rank=%lu binding=%s>",
                            PRTE_VPID_PRINT(proc->name.rank),  (long)proc->app_idx,
                            (unsigned long)proc->local_rank,
                            (unsigned long)proc->node_rank, tmp1);
                free(tmp1);
            }
            prte_output(prte_clean_output, "\t</host>");
            fflush(stderr);
        }

         /* test locality - for the first node, print the locality of each proc relative to the first one */
        node = (prte_node_t*)prte_pointer_array_get_item(jdata->map->nodes, 0);
        p0 = (prte_proc_t*)prte_pointer_array_get_item(node->procs, 0);
        if (NULL == p0) {
            PRTE_ERROR_LOG(PRTE_ERR_NOT_FOUND);
            return;
        }
        p0bitmap = NULL;
        if (prte_get_attribute(&p0->attributes, PRTE_PROC_CPU_BITMAP, (void**)&p0bitmap, PMIX_STRING) &&
            NULL != p0bitmap) {
            prte_output(prte_clean_output, "\t<locality>");
            for (j=1; j < node->procs->size; j++) {
                if (NULL == (proc = (prte_proc_t*)prte_pointer_array_get_item(node->procs, j))) {
                    continue;
                }
                if (proc->job != jdata) {
                    continue;
                }
                procbitmap = NULL;
                if (prte_get_attribute(&proc->attributes, PRTE_PROC_CPU_BITMAP, (void**)&procbitmap, PMIX_STRING) &&
                    NULL != procbitmap) {
                    locality = prte_hwloc_base_get_relative_locality(node->topology->topo,
                                                                     p0bitmap,
                                                                     procbitmap);
                    prte_output(prte_clean_output, "\t\t<rank=%s rank=%s locality=%s>",
                                PRTE_VPID_PRINT(p0->name.rank),
                                PRTE_VPID_PRINT(proc->name.rank),
                                prte_hwloc_base_print_locality(locality));
                }
            }
            prte_output(prte_clean_output, "\t</locality>\n</map>");
            fflush(stderr);
            if (NULL != p0bitmap) {
                free(p0bitmap);
            }
            if (NULL != procbitmap) {
                free(procbitmap);
            }
        }
    } else {
        prte_map_print(&output, jdata);
        prte_output(prte_clean_output, "%s\n", output);
        free(output);
    }
}
