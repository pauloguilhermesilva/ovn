/*
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at:
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
*/

#include <config.h>

#include <getopt.h>
#include <stdlib.h>
#include <stdio.h>

/* OVS includes. */
#include "openvswitch/vlog.h"

/* OVN includes. */
#include "ovn-ic.h"
#include "en-tr.h"
#include "en-enum-datapaths.h"
#include "inc-proc-ic.h"
#include "lib/inc-proc-eng.h"
#include "lib/ovn-nb-idl.h"
#include "lib/ovn-ic-nb-idl.h"
#include "lib/ovn-ic-sb-idl.h"
#include "lib/ovn-util.h"
#include "lib/stopwatch-names.h"
#include "coverage.h"
#include "smap.h"
#include "stopwatch.h"
#include "stopwatch-names.h"

VLOG_DEFINE_THIS_MODULE(en_transit_router);
COVERAGE_DEFINE(tr_run);

static void
tr_run(const struct engine_context *eng_ctx,
       struct ed_type_transit_router *tr_data,
       struct ed_type_enum_datapaths *dp_node_data,
       const struct nbrec_logical_router_table *nbrec_lr_table,
       const struct icnbrec_transit_router_table *icnbrec_tr_table);
static void tr_init(struct ed_type_transit_router *data);
static void tr_destroy(struct ed_type_transit_router *data);
static const struct nbrec_logical_router *
find_nb_lr_by_name(struct ovsdb_idl_index *nbrec_lr_by_name, const char *name);

enum engine_node_state
en_tr_run(struct engine_node *node, void *data)
{
    const struct engine_context *eng_ctx = engine_get_context();
    struct ed_type_transit_router *tr_data = data;

    struct ed_type_enum_datapaths *dp_node_data =
        engine_get_input_data("enum_datapaths", node);

    const struct nbrec_logical_router_table *nbrec_lr_table =
        EN_OVSDB_GET(engine_get_input("NB_logical_router", node));
    const struct icnbrec_transit_router_table *icnbrec_tr_table =
        EN_OVSDB_GET(engine_get_input("ICNB_transit_router", node));

    COVERAGE_INC(tr_run);
    stopwatch_start(OVN_IC_TRANSIT_ROUTER_RUN_STOPWATCH_NAME, time_usec());
    tr_run(eng_ctx, tr_data, dp_node_data, nbrec_lr_table, icnbrec_tr_table);
    stopwatch_stop(OVN_IC_TRANSIT_ROUTER_RUN_STOPWATCH_NAME, time_usec());

    return EN_UPDATED;
}

void *
en_tr_init(struct engine_node *node OVS_UNUSED,
           struct engine_arg *arg OVS_UNUSED)
{
    struct ed_type_transit_router *data = xzalloc(sizeof *data);
    tr_init(data);
    return data;
}

void
en_tr_cleanup(void *data)
{
    tr_destroy(data);
}

static void
tr_init(struct ed_type_transit_router *data)
{
    shash_init(&data->isb_tr_dps);
    hmap_init(&data->dp_tnlids);
}

static void
tr_destroy(struct ed_type_transit_router *data)
{
    shash_destroy(&data->isb_tr_dps);
    ovn_destroy_tnlids(&data->dp_tnlids);
}

static void
tr_run(const struct engine_context *eng_ctx,
       struct ed_type_transit_router *tr_data OVS_UNUSED,
       struct ed_type_enum_datapaths *dp_node_data,
       const struct nbrec_logical_router_table *nbrec_lr_table,
       const struct icnbrec_transit_router_table *icnbrec_tr_table)
{
    const struct nbrec_logical_router *lr;
    if (eng_ctx->ovnnb_idl_txn) {
        struct shash nb_tres = SHASH_INITIALIZER(&nb_tres);
        NBREC_LOGICAL_ROUTER_TABLE_FOR_EACH (lr, nbrec_lr_table) {
            const char *tr_name = smap_get(&lr->options, "interconn-tr");
            if (tr_name) {
                shash_add(&nb_tres, tr_name, lr);
            }
        }

        const struct icnbrec_transit_router *tr;
        ICNBREC_TRANSIT_ROUTER_TABLE_FOR_EACH (tr, icnbrec_tr_table) {
            lr = shash_find_and_delete(&nb_tres, tr->name);
            if (!lr) {
                lr = nbrec_logical_router_insert(eng_ctx->ovnnb_idl_txn);
                nbrec_logical_router_set_name(lr, tr->name);
                nbrec_logical_router_update_options_setkey(
                    lr, "interconn-tr", tr->name);
            }
            char *uuid_str = uuid_to_string(&tr->header_.uuid);
            struct icsbrec_datapath_binding *isb_dp = shash_find_data(
                &dp_node_data->isb_tr_dps, uuid_str);
            free(uuid_str);

            if (isb_dp) {
                char *tnl_key_str = xasprintf("%"PRId64, isb_dp->tunnel_key);
                nbrec_logical_router_update_options_setkey(
                    lr, "requested-tnl-key", tnl_key_str);
                free(tnl_key_str);
            }
        }

        struct shash_node *node;
        SHASH_FOR_EACH (node, &nb_tres) {
            nbrec_logical_router_delete(node->data);
        }
        shash_destroy(&nb_tres);
    }

    /* Sync TR between INB and ISB.  This is performed after syncing with AZ
     * SB, to avoid uncommitted ISB datapath tunnel key to be synced back to
     * AZ. */
    if (eng_ctx->ovnisb_idl_txn) {
        /* Create ISB Datapath_Binding */
        const struct icnbrec_transit_router *tr;
        ICNBREC_TRANSIT_ROUTER_TABLE_FOR_EACH (tr, icnbrec_tr_table) {
            char *uuid_str = uuid_to_string(&tr->header_.uuid);
            struct icsbrec_datapath_binding *isb_dp =
                shash_find_and_delete(&dp_node_data->isb_tr_dps, uuid_str);
            free(uuid_str);

            if (!isb_dp) {
                int dp_key = allocate_dp_key(&dp_node_data->dp_tnlids, false,
                                             "transit router datapath");
                if (!dp_key) {
                    continue;
                }

                isb_dp = icsbrec_datapath_binding_insert(
                    eng_ctx->ovnisb_idl_txn);
                icsbrec_datapath_binding_set_tunnel_key(isb_dp, dp_key);
                icsbrec_datapath_binding_set_nb_ic_uuid(isb_dp,
                                                        &tr->header_.uuid, 1);
                icsbrec_datapath_binding_set_type(isb_dp, "transit-router");
            }
        }

        struct shash_node *node;
        SHASH_FOR_EACH (node, &dp_node_data->isb_tr_dps) {
            icsbrec_datapath_binding_delete(node->data);
        }
    }
}

static const struct nbrec_logical_router *
find_nb_lr_by_name(struct ovsdb_idl_index *nbrec_lr_by_name, const char *name)
{
    const struct nbrec_logical_router *key =
        nbrec_logical_router_index_init_row(nbrec_lr_by_name);
    nbrec_logical_router_index_set_name(key, name);
    const struct nbrec_logical_router *lr =
        nbrec_logical_router_index_find(nbrec_lr_by_name, key);
    nbrec_logical_router_index_destroy_row(key);
    return lr;
}

/* Incremental handler for ICNB transit router changes.
 * Handles add/delete of transit routers without a full recompute.
 * Falls back (EN_UNHANDLED) for updates, which are uncommon. */
enum engine_input_handler_result
tr_icnb_transit_router_handler(struct engine_node *node,
                               void *data OVS_UNUSED)
{
    const struct engine_context *eng_ctx = engine_get_context();

    struct ed_type_enum_datapaths *dp_data =
        engine_get_input_data("enum_datapaths", node);

    const struct icnbrec_transit_router_table *icnbrec_tr_table =
        EN_OVSDB_GET(engine_get_input("ICNB_transit_router", node));

    struct ovsdb_idl_index *nbrec_lr_by_name =
        engine_ovsdb_node_get_index(
            engine_get_input("NB_logical_router", node),
            "nbrec_lr_by_name");

    bool changed = false;
    const struct icnbrec_transit_router *tr;
    ICNBREC_TRANSIT_ROUTER_TABLE_FOR_EACH_TRACKED (tr, icnbrec_tr_table) {
        changed = true;

        if (icnbrec_transit_router_is_deleted(tr)) {
            if (eng_ctx->ovnnb_idl_txn) {
                const struct nbrec_logical_router *lr =
                    find_nb_lr_by_name(nbrec_lr_by_name, tr->name);
                if (lr && smap_get(&lr->options, "interconn-tr")) {
                    nbrec_logical_router_delete(lr);
                }
            }
            if (eng_ctx->ovnisb_idl_txn) {
                char *uuid_str = uuid_to_string(&tr->header_.uuid);
                struct icsbrec_datapath_binding *isb_dp =
                    shash_find_and_delete(&dp_data->isb_tr_dps, uuid_str);
                free(uuid_str);
                if (isb_dp) {
                    ovn_free_tnlid(&dp_data->dp_tnlids, isb_dp->tunnel_key);
                    icsbrec_datapath_binding_delete(isb_dp);
                }
            }
        } else if (icnbrec_transit_router_is_new(tr)) {
            const struct nbrec_logical_router *lr = NULL;
            if (eng_ctx->ovnnb_idl_txn) {
                lr = find_nb_lr_by_name(nbrec_lr_by_name, tr->name);
                if (!lr) {
                    lr = nbrec_logical_router_insert(eng_ctx->ovnnb_idl_txn);
                    nbrec_logical_router_set_name(lr, tr->name);
                    nbrec_logical_router_update_options_setkey(
                        lr, "interconn-tr", tr->name);
                }
            }
            if (eng_ctx->ovnisb_idl_txn) {
                char *uuid_str = uuid_to_string(&tr->header_.uuid);
                bool already_exists =
                    shash_find(&dp_data->isb_tr_dps, uuid_str) != NULL;
                free(uuid_str);
                if (!already_exists) {
                    int dp_key = allocate_dp_key(&dp_data->dp_tnlids, false,
                                                 "transit router datapath");
                    if (dp_key) {
                        struct icsbrec_datapath_binding *isb_dp =
                            icsbrec_datapath_binding_insert(
                                eng_ctx->ovnisb_idl_txn);
                        icsbrec_datapath_binding_set_tunnel_key(isb_dp,
                                                                dp_key);
                        icsbrec_datapath_binding_set_nb_ic_uuid(
                            isb_dp, &tr->header_.uuid, 1);
                        icsbrec_datapath_binding_set_type(isb_dp,
                                                          "transit-router");
                        if (lr) {
                            char *tnl_key_str = xasprintf("%d", dp_key);
                            nbrec_logical_router_update_options_setkey(
                                lr, "requested-tnl-key", tnl_key_str);
                            free(tnl_key_str);
                        }
                    }
                }
            }
        } else {
            /* Updated TR: attributes of transit routers rarely change.
             * Fall back to full recompute to be safe. */
            return EN_UNHANDLED;
        }
    }

    if (changed) {
        return EN_HANDLED_UPDATED;
    }
    return EN_HANDLED_UNCHANGED;
}

/* Incremental handler for NB logical router changes.
 * Only reacts to deleted interconnect routers (self-healing): if a router
 * with interconn-tr option is deleted but the corresponding ICNB transit
 * router still exists, recreate it. */
enum engine_input_handler_result
tr_nb_logical_router_handler(struct engine_node *node,
                             void *data OVS_UNUSED)
{
    const struct engine_context *eng_ctx = engine_get_context();

    const struct nbrec_logical_router_table *nbrec_lr_table =
        EN_OVSDB_GET(engine_get_input("NB_logical_router", node));
    const struct icnbrec_transit_router_table *icnbrec_tr_table =
        EN_OVSDB_GET(engine_get_input("ICNB_transit_router", node));

    bool changed = false;
    const struct nbrec_logical_router *lr;
    NBREC_LOGICAL_ROUTER_TABLE_FOR_EACH_TRACKED (lr, nbrec_lr_table) {
        const char *tr_name = smap_get(&lr->options, "interconn-tr");
        if (!tr_name) {
            continue;
        }

        changed = true;

        if (!nbrec_logical_router_is_deleted(lr)) {
            /* Updates to interconnect routers are not our concern here. */
            continue;
        }

        if (!eng_ctx->ovnnb_idl_txn) {
            return EN_UNHANDLED;
        }

        /* Self-healing: recreate the LR if its ICNB transit router still
         * exists. */
        bool tr_exists = false;
        const struct icnbrec_transit_router *tr;
        ICNBREC_TRANSIT_ROUTER_TABLE_FOR_EACH (tr, icnbrec_tr_table) {
            if (!strcmp(tr->name, tr_name)) {
                tr_exists = true;
                break;
            }
        }

        if (tr_exists) {
            const struct nbrec_logical_router *new_lr =
                nbrec_logical_router_insert(eng_ctx->ovnnb_idl_txn);
            nbrec_logical_router_set_name(new_lr, tr->name);
            nbrec_logical_router_update_options_setkey(
                new_lr, "interconn-tr", tr->name);
            return EN_HANDLED_UPDATED;
        }
    }

    if (changed) {
        return EN_HANDLED_UPDATED;
    }
    return EN_HANDLED_UNCHANGED;
}
