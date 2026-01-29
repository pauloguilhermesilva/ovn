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
#include "openvswitch/util.h"
#include "openvswitch/shash.h"
#include "openvswitch/hmap.h"

/* OVN includes. */
#include "ovn-ic.h"
#include "en-port-binding.h"
#include "en-ts.h"
#include "inc-proc-ic.h"
#include "lib/inc-proc-eng.h"
#include "lib/ovn-nb-idl.h"
#include "lib/ovn-sb-idl.h"
#include "lib/ovn-ic-nb-idl.h"
#include "lib/ovn-ic-sb-idl.h"
#include "lib/ovn-util.h"
#include "lib/stopwatch-names.h"
#include "coverage.h"
#include "stopwatch.h"
#include "stopwatch-names.h"

VLOG_DEFINE_THIS_MODULE(en_port_binding);
COVERAGE_DEFINE(port_binding_run);

static void
port_binding_run(const struct engine_context *eng_ctx,
                 struct pb_input *pb_input,
                 struct ed_type_port_binding *pb_data,
                 const struct icnbrec_transit_switch_table *icnb_ts_table,
                 const struct icnbrec_transit_router_table *icnb_tr_table);
static void port_binding_init(struct ed_type_port_binding *data);
static void port_binding_destroy(struct ed_type_port_binding *data);
static void port_binding_clear(struct ed_type_port_binding *data);
static void port_binding_get_input_data(struct engine_node *node,
                                        struct pb_input *input_data);
static const struct nbrec_logical_router *
    find_tr_in_nb(struct pb_input *pb, char *tr_name);
static const struct sbrec_port_binding *
    find_peer_port(struct pb_input *pb,
                   const struct sbrec_port_binding *sb_pb);
static const struct sbrec_port_binding *
    find_crp_from_lrp(struct pb_input *pb,
                      const struct sbrec_port_binding *lrp_pb);
static const struct sbrec_port_binding *
    find_crp_for_sb_pb(struct pb_input *pb,
                       const struct sbrec_port_binding *sb_pb);
static const char *
    get_lp_address_for_sb_pb(struct pb_input *pb,
                             const struct sbrec_port_binding *sb_pb);
static const struct sbrec_chassis *
    find_sb_chassis(struct pb_input *pb, const char *name);
static void sync_lsp_tnl_key(const struct nbrec_logical_switch_port *lsp,
                             int64_t isb_tnl_key);
static inline void
    sync_lrp_tnl_key(const struct nbrec_logical_router_port *lrp,
                     int64_t isb_tnl_key);
static bool
    get_router_uuid_by_sb_pb(struct pb_input *pb,
                             const struct sbrec_port_binding *sb_pb,
                             struct uuid *router_uuid);
static void
    update_isb_pb_external_ids(struct pb_input *pb,
                               const struct sbrec_port_binding *sb_pb,
                               const struct icsbrec_port_binding *isb_pb);
static void
    sync_local_port(struct pb_input *pb,
                    const struct icsbrec_port_binding *isb_pb,
                    const struct sbrec_port_binding *sb_pb,
                    const struct nbrec_logical_switch_port *lsp);
static void
    sync_remote_port(struct pb_input *pb,
                     const struct icsbrec_port_binding *isb_pb,
                     const struct nbrec_logical_switch_port *lsp,
                     const struct sbrec_port_binding *sb_pb);
static void
    sync_router_port(const struct icsbrec_port_binding *isb_pb,
                     const struct icnbrec_transit_router_port *trp,
                     const struct nbrec_logical_router_port *lrp);
static void
    create_nb_lsp(const struct engine_context *ctx,
                  const struct icsbrec_port_binding *isb_pb,
                  const struct nbrec_logical_switch *ls);
static uint32_t allocate_port_key(struct hmap *pb_tnlids);
static const struct icsbrec_port_binding *
    create_isb_pb(const struct engine_context *ctx, const char *logical_port,
                  const struct icsbrec_availability_zone *az,
                  const char *ts_name, const struct uuid *nb_ic_uuid,
                  const char *type, struct hmap *pb_tnlids);
static bool trp_is_remote(struct pb_input *pb, const char *chassis_name);
static struct nbrec_logical_router_port *
    lrp_create(const struct engine_context *ctx,
               const struct nbrec_logical_router *lr,
               const struct icnbrec_transit_router_port *trp);
static void
    sync_ts_isb_pb(struct pb_input *pb, const struct sbrec_port_binding *sb_pb,
                   const struct icsbrec_port_binding *isb_pb);
static const struct sbrec_port_binding *
    find_lsp_in_sb(struct pb_input *pb,
                   const struct nbrec_logical_switch_port *lsp);
static const struct nbrec_logical_switch_port *
    find_nb_lsp_from_sb_pb(struct pb_input *pb,
                           const struct sbrec_port_binding *sb_pb);
struct icsbrec_port_binding *
    find_isb_pb_by_name(struct pb_input *pb, const char *name);
static bool is_pb_associated_with_ts(struct pb_input *pb,
                                     const struct sbrec_port_binding *sb_pb,
                                     struct icnbrec_transit_switch *ts_key);

/* DEBUG */
static void log_lsp(const struct nbrec_logical_switch_port *lsp);
static void log_ls(const struct nbrec_logical_switch *ls);
static void log_ts(const struct icnbrec_transit_switch *ts);
static void log_sb_pb(const struct sbrec_port_binding *pb);
static void log_isb_pb(const struct icsbrec_port_binding *isb_pb);

static void
port_binding_get_input_data(struct engine_node *node,
                            struct pb_input *input_data)
{
    /* Indexes */
    input_data->icsbrec_port_binding_by_az =
        engine_ovsdb_node_get_index(
            engine_get_input("ICSB_port_binding", node),
            "icsbrec_port_binding_by_az");
    input_data->icsbrec_port_binding_by_ts =
        engine_ovsdb_node_get_index(
            engine_get_input("ICSB_port_binding", node),
            "icsbrec_port_binding_by_ts");
    input_data->icsbrec_port_binding_by_name =
        engine_ovsdb_node_get_index(
            engine_get_input("ICSB_port_binding", node),
            "icsbrec_port_binding_by_name");
    input_data->nbrec_ls_by_name =
        engine_ovsdb_node_get_index(
            engine_get_input("NB_logical_switch", node),
            "nbrec_ls_by_name");
    input_data->nbrec_lsp_by_name =
        engine_ovsdb_node_get_index(
            engine_get_input("NB_logical_switch_port", node),
            "nbrec_lsp_by_name");
    input_data->sbrec_port_binding_by_name =
        engine_ovsdb_node_get_index(
            engine_get_input("SB_port_binding", node),
            "sbrec_port_binding_by_name");
    input_data->nbrec_port_by_name =
        engine_ovsdb_node_get_index(
            engine_get_input("NB_logical_switch", node),
            "nbrec_port_by_name");
    input_data->nbrec_lr_by_name =
        engine_ovsdb_node_get_index(
            engine_get_input("NB_logical_router", node),
            "nbrec_lr_by_name");
    input_data->nbrec_ls_by_lsp_port =
        engine_ovsdb_node_get_index(
            engine_get_input("NB_logical_switch", node),
            "nbrec_ls_by_lsp_port");
    input_data->sbrec_chassis_by_name =
        engine_ovsdb_node_get_index(
            engine_get_input("SB_chassis", node),
            "sbrec_chassis_by_name");
    input_data->icnbrec_transit_switch_by_name =
        engine_ovsdb_node_get_index(
            engine_get_input("ICNB_transit_switch", node),
            "icnbrec_transit_switch_by_name");
    
}

enum engine_node_state
en_port_binding_run(struct engine_node *node, void *data)
{
    VLOG_INFO("DBG-PG - - %s : %s : %d", __FILE__, __func__, __LINE__);
    const struct engine_context *eng_ctx = engine_get_context();
    struct ed_type_port_binding *pb_data = data;
    struct pb_input pb_input;

    port_binding_clear(pb_data);

    const struct icnbrec_transit_switch_table *icnb_ts_table =
        EN_OVSDB_GET(engine_get_input("ICNB_transit_switch", node));
    const struct icnbrec_transit_router_table *icnb_tr_table =
        EN_OVSDB_GET(engine_get_input("ICNB_transit_router", node));

    port_binding_get_input_data(node, &pb_input);
    pb_input.runned_az = eng_ctx->client_ctx;

    COVERAGE_INC(port_binding_run);
    stopwatch_start(OVN_IC_PORT_BINDING_RUN_STOPWATCH_NAME, time_usec());
    port_binding_run(eng_ctx, &pb_input, pb_data, icnb_ts_table,
                     icnb_tr_table);
    stopwatch_stop(OVN_IC_PORT_BINDING_RUN_STOPWATCH_NAME, time_usec());

    return EN_UPDATED;
}

void *
en_port_binding_init(struct engine_node *node OVS_UNUSED,
                     struct engine_arg *arg OVS_UNUSED)
{
    struct ed_type_port_binding *data = xzalloc(sizeof *data);
    port_binding_init(data);
    return data;
}

void
en_port_binding_cleanup(void *data)
{
    port_binding_destroy(data);
}

static void
port_binding_init(struct ed_type_port_binding *data)
{
    hmap_init(&data->pb_tnlids);
    shash_init(&data->switch_all_local_pbs);
    shash_init(&data->router_all_local_pbs);
}

static void
port_binding_destroy(struct ed_type_port_binding *data)
{
    port_binding_clear(data);
    ovn_destroy_tnlids(&data->pb_tnlids);

    shash_destroy(&data->switch_all_local_pbs);
    shash_destroy(&data->router_all_local_pbs);
}

static void
port_binding_clear(struct ed_type_port_binding *data)
{
    ovn_destroy_tnlids(&data->pb_tnlids);
    hmap_init(&data->pb_tnlids);

    shash_clear(&data->switch_all_local_pbs);
    shash_clear(&data->router_all_local_pbs);
}

static void
port_binding_run(const struct engine_context *eng_ctx,
                 struct pb_input *pb_input,
                 struct ed_type_port_binding *pb_data,
                 const struct icnbrec_transit_switch_table *icnb_ts_table,
                 const struct icnbrec_transit_router_table *icnb_tr_table)
{
    VLOG_INFO("DBG-PG -====================== - %s : %s : %d", __FILE__, __func__, __LINE__);
    VLOG_INFO("DBG-PG - FULL-RECOMPUTE-PB - %s : %s : %d", __FILE__, __func__, __LINE__);
    VLOG_INFO("DBG-PG -====================== - %s : %s : %d", __FILE__, __func__, __LINE__);
    if (!eng_ctx->ovnisb_idl_txn || !eng_ctx->ovnnb_idl_txn
        || !eng_ctx->ovnsb_idl_txn) {
        return;
    }

    struct shash_node *node;
    const struct icsbrec_port_binding *isb_pb;
    const struct icsbrec_port_binding *isb_pb_key =
        icsbrec_port_binding_index_init_row(
            pb_input->icsbrec_port_binding_by_az);
    icsbrec_port_binding_index_set_availability_zone(isb_pb_key,
                                                     pb_input->runned_az);

    ICSBREC_PORT_BINDING_FOR_EACH_EQUAL (isb_pb, isb_pb_key,
        pb_input->icsbrec_port_binding_by_az) {
        ic_pb_get_type(isb_pb) != IC_ROUTER_PORT
            ? shash_add(&pb_data->switch_all_local_pbs, isb_pb->logical_port,
                        isb_pb)
            : shash_add(&pb_data->router_all_local_pbs, isb_pb->logical_port,
                        isb_pb);

        ovn_add_tnlid(&pb_data->pb_tnlids, isb_pb->tunnel_key);
    }
    icsbrec_port_binding_index_destroy_row(isb_pb_key);

    const struct sbrec_port_binding *sb_pb;
    const struct icnbrec_transit_switch *ts;
    ICNBREC_TRANSIT_SWITCH_TABLE_FOR_EACH (ts, icnb_ts_table) {
        const struct nbrec_logical_switch *ls =
            find_ts_in_nb(pb_input->nbrec_ls_by_name,
                          ts->name);
        if (!ls) {
            VLOG_DBG("Transit switch %s not found in NB.", ts->name);
            continue;
        }
        struct shash local_pbs = SHASH_INITIALIZER(&local_pbs);
        struct shash remote_pbs = SHASH_INITIALIZER(&remote_pbs);

        isb_pb_key = icsbrec_port_binding_index_init_row(
            pb_input->icsbrec_port_binding_by_ts);
        icsbrec_port_binding_index_set_transit_switch(isb_pb_key, ts->name);

        ICSBREC_PORT_BINDING_FOR_EACH_EQUAL (isb_pb, isb_pb_key,
            pb_input->icsbrec_port_binding_by_ts) {
            if (isb_pb->availability_zone == pb_input->runned_az) {
                shash_add(&local_pbs, isb_pb->logical_port, isb_pb);
                shash_find_and_delete(&pb_data->switch_all_local_pbs,
                                      isb_pb->logical_port);
            } else {
                shash_add(&remote_pbs, isb_pb->logical_port, isb_pb);
            }
        }
        icsbrec_port_binding_index_destroy_row(isb_pb_key);

        const struct nbrec_logical_switch_port *lsp;
        for (int i = 0; i < ls->n_ports; i++) {
            lsp = ls->ports[i];

            if (!strcmp(lsp->type, "router")
                || !strcmp(lsp->type, "switch")) {
                /* The port is local. */
                sb_pb = find_lsp_in_sb(pb_input, lsp);
                if (!sb_pb) {
                    continue;
                }
                isb_pb = shash_find_and_delete(&local_pbs, lsp->name);
                if (!isb_pb) {
                    isb_pb = create_isb_pb(
                        eng_ctx, sb_pb->logical_port, pb_input->runned_az,
                        ts->name, &ts->header_.uuid, "transit-switch-port",
                        &pb_data->pb_tnlids);
                    sync_ts_isb_pb(pb_input, sb_pb, isb_pb);
                } else {
                    sync_local_port(pb_input, isb_pb, sb_pb, lsp);
                }

                if (isb_pb->type) {
                    icsbrec_port_binding_set_type(isb_pb,
                                                  "transit-switch-port");
                }

                if (isb_pb->nb_ic_uuid) {
                    icsbrec_port_binding_set_nb_ic_uuid(isb_pb,
                                                        &ts->header_.uuid, 1);
                }
            } else if (!strcmp(lsp->type, "remote")) {
                /* The port is remote. */
                isb_pb = shash_find_and_delete(&remote_pbs, lsp->name);
                if (!isb_pb) {
                    nbrec_logical_switch_update_ports_delvalue(ls, lsp);
                } else {
                    sb_pb = find_lsp_in_sb(pb_input, lsp);
                    if (!sb_pb) {
                        continue;
                    }
                    sync_remote_port(pb_input, isb_pb, lsp, sb_pb);
                }
            } else {
                VLOG_DBG("Ignore lsp %s on ts %s with type %s.",
                         lsp->name, ts->name, lsp->type);
            }
        }

        /* Delete extra port-binding from ISB */
        SHASH_FOR_EACH (node, &local_pbs) {
            icsbrec_port_binding_delete(node->data);
        }

        /* Create lsp in NB for remote ports */
        SHASH_FOR_EACH (node, &remote_pbs) {
            create_nb_lsp(eng_ctx, node->data, ls);
        }

        shash_destroy(&local_pbs);
        shash_destroy(&remote_pbs);
    }

    SHASH_FOR_EACH (node, &pb_data->switch_all_local_pbs) {
        icsbrec_port_binding_delete(node->data);
    }

    const struct icnbrec_transit_router *tr;
    ICNBREC_TRANSIT_ROUTER_TABLE_FOR_EACH (tr, icnb_tr_table) {
        const struct nbrec_logical_router *lr = find_tr_in_nb(pb_input,
                                                              tr->name);
        if (!lr) {
            VLOG_DBG("Transit router %s not found in NB.", tr->name);
            continue;
        }

        struct shash nb_ports = SHASH_INITIALIZER(&nb_ports);
        struct shash local_pbs = SHASH_INITIALIZER(&local_pbs);
        struct shash remote_pbs = SHASH_INITIALIZER(&remote_pbs);

        for (size_t i = 0; i < lr->n_ports; i++) {
            const struct nbrec_logical_router_port *lrp = lr->ports[i];
            if (smap_get_def(&lrp->options, "interconn-tr", NULL)) {
                shash_add(&nb_ports, lrp->name, lrp);
            }
        }

        isb_pb_key = icsbrec_port_binding_index_init_row(
            pb_input->icsbrec_port_binding_by_ts);
        icsbrec_port_binding_index_set_transit_switch(isb_pb_key, tr->name);

        ICSBREC_PORT_BINDING_FOR_EACH_EQUAL (isb_pb, isb_pb_key,
            pb_input->icsbrec_port_binding_by_ts) {
            if (isb_pb->availability_zone == pb_input->runned_az) {
                shash_add(&local_pbs, isb_pb->logical_port, isb_pb);
                shash_find_and_delete(&pb_data->router_all_local_pbs,
                                      isb_pb->logical_port);
            } else {
                shash_add(&remote_pbs, isb_pb->logical_port, isb_pb);
            }
        }
        icsbrec_port_binding_index_destroy_row(isb_pb_key);

        for (size_t i = 0; i < tr->n_ports; i++) {
            const struct icnbrec_transit_router_port *trp = tr->ports[i];

            if (trp_is_remote(pb_input, trp->chassis)) {
                isb_pb = shash_find_and_delete(&remote_pbs, trp->name);
            } else {
                isb_pb = shash_find_and_delete(&local_pbs, trp->name);
                if (!isb_pb) {
                    isb_pb = create_isb_pb(eng_ctx, trp->name,
                                           pb_input->runned_az, tr->name,
                                           &tr->header_.uuid,
                                           "transit-router-port",
                                           &pb_data->pb_tnlids);
                    icsbrec_port_binding_set_address(isb_pb, trp->mac);
                }
            }

            /* Don't allow remote ports to create NB LRP until ICSB entry is
             * created in the appropriate AZ. */
            if (isb_pb) {
                const struct nbrec_logical_router_port *lrp =
                    shash_find_and_delete(&nb_ports, trp->name);
                if (!lrp) {
                    lrp = lrp_create(eng_ctx, lr, trp);
                }

                sync_router_port(isb_pb, trp, lrp);
            }
        }

        SHASH_FOR_EACH (node, &nb_ports) {
            nbrec_logical_router_port_delete(node->data);
            nbrec_logical_router_update_ports_delvalue(lr, node->data);
        }

        shash_destroy(&nb_ports);
        shash_destroy(&local_pbs);
        shash_destroy(&remote_pbs);
    }

    SHASH_FOR_EACH (node, &pb_data->router_all_local_pbs) {
        icsbrec_port_binding_delete(node->data);
    }
}

static const struct nbrec_logical_router *
find_tr_in_nb(struct pb_input *pb, char *tr_name)
{
    const struct nbrec_logical_router *key =
        nbrec_logical_router_index_init_row(pb->nbrec_lr_by_name);
    nbrec_logical_router_index_set_name(key, tr_name);

    const struct nbrec_logical_router *lr;
    bool found = false;
    NBREC_LOGICAL_ROUTER_FOR_EACH_EQUAL (lr, key, pb->nbrec_lr_by_name) {
        if (smap_get(&lr->options, "interconn-tr")) {
            found = true;
            break;
        }
    }

    nbrec_logical_router_index_destroy_row(key);
    if (found) {
        return lr;
    }

    return NULL;
}

static const struct sbrec_port_binding *
find_peer_port(struct pb_input *pb,
               const struct sbrec_port_binding *sb_pb)
{
    const char *peer_name = smap_get(&sb_pb->options, "peer");
    if (!peer_name) {
        return NULL;
    }

    return find_sb_pb_by_name(pb->sbrec_port_binding_by_name, peer_name);
}

static const struct sbrec_port_binding *
find_crp_from_lrp(struct pb_input *pb,
                  const struct sbrec_port_binding *lrp_pb)
{
    char *crp_name = ovn_chassis_redirect_name(lrp_pb->logical_port);

    const struct sbrec_port_binding *sb_pb =
        find_sb_pb_by_name(pb->sbrec_port_binding_by_name, crp_name);

    free(crp_name);
    return sb_pb;
}

static const struct sbrec_port_binding *
find_crp_for_sb_pb(struct pb_input *pb,
                   const struct sbrec_port_binding *sb_pb)
{
    const struct sbrec_port_binding *peer = find_peer_port(pb, sb_pb);
    if (!peer) {
        return NULL;
    }

    return find_crp_from_lrp(pb, peer);
}

static const char *
get_lp_address_for_sb_pb(struct pb_input *pb,
                         const struct sbrec_port_binding *sb_pb)
{
    const struct nbrec_logical_switch_port *nb_lsp;

    nb_lsp = get_lsp_by_ts_port_name(pb->nbrec_port_by_name,
                                     sb_pb->logical_port);
    if (!strcmp(nb_lsp->type, "switch")) {
        /* Switches always have implicit "unknown" address, and IC-SB port
         * binding can only have one address specified. */
        return "unknown";
    }

    const struct sbrec_port_binding *peer = find_peer_port(pb, sb_pb);
    if (!peer) {
        return NULL;
    }

    return peer->n_mac ? *peer->mac : NULL;
}

static const struct sbrec_chassis *
find_sb_chassis(struct pb_input *pb, const char *name)
{
    const struct sbrec_chassis *key =
        sbrec_chassis_index_init_row(pb->sbrec_chassis_by_name);
    sbrec_chassis_index_set_name(key, name);

    const struct sbrec_chassis *chassis =
        sbrec_chassis_index_find(pb->sbrec_chassis_by_name, key);
    sbrec_chassis_index_destroy_row(key);

    return chassis;
}

static void
sync_lsp_tnl_key(const struct nbrec_logical_switch_port *lsp,
                 int64_t isb_tnl_key)
{
    int64_t tnl_key = smap_get_int(&lsp->options, "requested-tnl-key", 0);
    if (tnl_key != isb_tnl_key) {
        VLOG_DBG("Set options:requested-tnl-key %"PRId64
                 " for lsp %s in NB.", isb_tnl_key, lsp->name);
        char *tnl_key_str = xasprintf("%"PRId64, isb_tnl_key);
        nbrec_logical_switch_port_update_options_setkey(lsp,
                                                        "requested-tnl-key",
                                                        tnl_key_str);
        free(tnl_key_str);
    }
}

static inline void
sync_lrp_tnl_key(const struct nbrec_logical_router_port *lrp,
                 int64_t isb_tnl_key)
{
    int64_t tnl_key = smap_get_int(&lrp->options, "requested-tnl-key", 0);
    if (tnl_key != isb_tnl_key) {
        VLOG_DBG("Set options:requested-tnl-key %" PRId64 " for lrp %s in NB.",
                 isb_tnl_key, lrp->name);
        char *tnl_key_str = xasprintf("%"PRId64, isb_tnl_key);
        nbrec_logical_router_port_update_options_setkey(
            lrp, "requested-tnl-key", tnl_key_str);
        free(tnl_key_str);
    }
}

static bool
get_router_uuid_by_sb_pb(struct pb_input *pb,
                         const struct sbrec_port_binding *sb_pb,
                         struct uuid *router_uuid)
{
    const struct sbrec_port_binding *router_pb = find_peer_port(pb, sb_pb);
    if (!router_pb || !router_pb->datapath) {
        return NULL;
    }

    return datapath_get_nb_uuid(router_pb->datapath, router_uuid);
}

static void
update_isb_pb_external_ids(struct pb_input *pb,
                           const struct sbrec_port_binding *sb_pb,
                           const struct icsbrec_port_binding *isb_pb)
{
    struct uuid lr_uuid;
    if (!get_router_uuid_by_sb_pb(pb, sb_pb, &lr_uuid)) {
        static struct vlog_rate_limit rl = VLOG_RATE_LIMIT_INIT(5, 1);
        VLOG_WARN_RL(&rl, "Can't get router uuid for transit switch port %s.",
                     isb_pb->logical_port);
        return;
    }

    struct uuid current_lr_uuid;
    if (smap_get_uuid(&isb_pb->external_ids, "router-id", &current_lr_uuid) &&
        uuid_equals(&lr_uuid, &current_lr_uuid)) {
        return;
    }

    char *uuid_s = xasprintf(UUID_FMT, UUID_ARGS(&lr_uuid));
    icsbrec_port_binding_update_external_ids_setkey(isb_pb, "router-id",
                                                    uuid_s);
    free(uuid_s);
}

/* For each local port:
 *   - Sync from NB to ISB.
 *   - Sync gateway from SB to ISB.
 *   - Sync tunnel key from ISB to NB.
 */
static void
sync_local_port(struct pb_input *pb,
                const struct icsbrec_port_binding *isb_pb,
                const struct sbrec_port_binding *sb_pb,
                const struct nbrec_logical_switch_port *lsp)
{
    /* Sync address from NB to ISB */
    const char *address = get_lp_address_for_sb_pb(pb, sb_pb);
    if (!address) {
        VLOG_DBG("Can't get router/switch port address for logical"
                 " switch port %s", sb_pb->logical_port);
        if (isb_pb->address[0]) {
            icsbrec_port_binding_set_address(isb_pb, "");
        }
    } else {
        if (strcmp(address, isb_pb->address)) {
            icsbrec_port_binding_set_address(isb_pb, address);
        }
    }

    /* Sync gateway from SB to ISB */
    const struct sbrec_port_binding *crp = find_crp_for_sb_pb(pb, sb_pb);
    if (crp && crp->chassis) {
        if (strcmp(crp->chassis->name, isb_pb->gateway)) {
            icsbrec_port_binding_set_gateway(isb_pb, crp->chassis->name);
        }
    } else if (!strcmp(lsp->type, "switch") && sb_pb->chassis) {
        if (strcmp(sb_pb->chassis->name, isb_pb->gateway)) {
            icsbrec_port_binding_set_gateway(isb_pb, sb_pb->chassis->name);
        }
    } else {
        if (isb_pb->gateway[0]) {
            icsbrec_port_binding_set_gateway(isb_pb, "");
        }
    }

    /* Sync external_ids:router-id to ISB */
    update_isb_pb_external_ids(pb, sb_pb, isb_pb);

    /* Sync back tunnel key from ISB to NB */
    sync_lsp_tnl_key(lsp, isb_pb->tunnel_key);
}

/* For each remote port:
 *   - Sync from ISB to NB
 *   - Sync gateway from ISB to SB
 */
static void
sync_remote_port(struct pb_input *pb,
                 const struct icsbrec_port_binding *isb_pb,
                 const struct nbrec_logical_switch_port *lsp,
                 const struct sbrec_port_binding *sb_pb)
{
    /* Sync address from ISB to NB */
    if (isb_pb->address[0]) {
        if (lsp->n_addresses != 1 ||
            strcmp(isb_pb->address, lsp->addresses[0])) {
            nbrec_logical_switch_port_set_addresses(
                lsp, (const char **)&isb_pb->address, 1);
        }
    } else {
        if (lsp->n_addresses != 0) {
            nbrec_logical_switch_port_set_addresses(lsp, NULL, 0);
        }
    }

    /* Sync tunnel key from ISB to NB */
    sync_lsp_tnl_key(lsp, isb_pb->tunnel_key);

    /* Skip port binding if it is already requested by the CMS. */
    if (smap_get(&lsp->options, "requested-chassis")) {
        return;
    }

    /* Sync gateway from ISB to SB */
    if (isb_pb->gateway[0]) {
        if (!sb_pb->chassis || strcmp(sb_pb->chassis->name, isb_pb->gateway)) {
            const struct sbrec_chassis *chassis =
                find_sb_chassis(pb, isb_pb->gateway);
            if (!chassis) {
                VLOG_DBG("Chassis %s is not found in SB, syncing from ISB "
                         "to SB skipped for logical port %s.",
                         isb_pb->gateway, lsp->name);
                return;
            }
            sbrec_port_binding_set_chassis(sb_pb, chassis);
        }
    } else {
        if (sb_pb->chassis) {
            sbrec_port_binding_set_chassis(sb_pb, NULL);
        }
    }
}

/* For each remote port:
 *   - Sync from ISB to NB
 */
static void
sync_router_port(const struct icsbrec_port_binding *isb_pb,
                 const struct icnbrec_transit_router_port *trp,
                 const struct nbrec_logical_router_port *lrp)
{
    /* Sync from ICNB to NB */
    if (trp->chassis[0]) {
        const char *chassis_name =
            smap_get_def(&lrp->options, "requested-chassis", "");
        if (strcmp(trp->chassis, chassis_name)) {
            nbrec_logical_router_port_update_options_setkey(
                lrp, "requested-chassis", trp->chassis);
        }
    } else {
        nbrec_logical_router_port_update_options_delkey(
            lrp, "requested-chassis");
    }

    if (strcmp(trp->mac, lrp->mac)) {
        nbrec_logical_router_port_set_mac(lrp, trp->mac);
    }

    bool sync_networks = false;
    if (trp->n_networks != lrp->n_networks) {
        sync_networks = true;
    } else {
        for (size_t i = 0; i < trp->n_networks; i++) {
            if (strcmp(trp->networks[i], lrp->networks[i])) {
                sync_networks |= true;
                break;
            }
        }
    }

    if (sync_networks) {
        nbrec_logical_router_port_set_networks(
            lrp, (const char **) trp->networks, trp->n_networks);
    }

    /* Sync tunnel key from ISB to NB */
    sync_lrp_tnl_key(lrp, isb_pb->tunnel_key);
}

static void
create_nb_lsp(const struct engine_context *ctx,
              const struct icsbrec_port_binding *isb_pb,
              const struct nbrec_logical_switch *ls)
{
    const struct nbrec_logical_switch_port *lsp =
        nbrec_logical_switch_port_insert(ctx->ovnnb_idl_txn);
    nbrec_logical_switch_port_set_name(lsp, isb_pb->logical_port);
    nbrec_logical_switch_port_set_type(lsp, "remote");

    bool up = true;
    nbrec_logical_switch_port_set_up(lsp, &up, 1);

    if (isb_pb->address[0]) {
        nbrec_logical_switch_port_set_addresses(
            lsp, (const char **)&isb_pb->address, 1);
    }
    sync_lsp_tnl_key(lsp, isb_pb->tunnel_key);
    nbrec_logical_switch_update_ports_addvalue(ls, lsp);
}

static uint32_t
allocate_port_key(struct hmap *pb_tnlids)
{
    static uint32_t hint;
    return ovn_allocate_tnlid(pb_tnlids, "transit port",
                              1, (1u << 15) - 1, &hint);
}

static const struct icsbrec_port_binding *
create_isb_pb(const struct engine_context *ctx, const char *logical_port,
              const struct icsbrec_availability_zone *az, const char *ts_name,
              const struct uuid *nb_ic_uuid, const char *type,
              struct hmap *pb_tnlids)
{
    uint32_t pb_tnl_key = allocate_port_key(pb_tnlids);
    if (!pb_tnl_key) {
        return NULL;
    }

    const struct icsbrec_port_binding *isb_pb =
        icsbrec_port_binding_insert(ctx->ovnisb_idl_txn);
    icsbrec_port_binding_set_availability_zone(isb_pb, az);
    icsbrec_port_binding_set_transit_switch(isb_pb, ts_name);
    icsbrec_port_binding_set_logical_port(isb_pb, logical_port);
    icsbrec_port_binding_set_tunnel_key(isb_pb, pb_tnl_key);
    icsbrec_port_binding_set_nb_ic_uuid(isb_pb, nb_ic_uuid, 1);
    icsbrec_port_binding_set_type(isb_pb, type);
    return isb_pb;
}

static bool
trp_is_remote(struct pb_input *pb, const char *chassis_name)
{
    if (chassis_name) {
        const struct sbrec_chassis *chassis =
            find_sb_chassis(pb, chassis_name);
        if (chassis) {
            return smap_get_bool(&chassis->other_config, "is-remote", false);
        } else {
            return true;
        }
    }

    return false;
}

static struct nbrec_logical_router_port *
lrp_create(const struct engine_context *ctx,
           const struct nbrec_logical_router *lr,
           const struct icnbrec_transit_router_port *trp)
{
    struct nbrec_logical_router_port *lrp =
        nbrec_logical_router_port_insert(ctx->ovnnb_idl_txn);
    nbrec_logical_router_port_set_name(lrp, trp->name);

    nbrec_logical_router_port_update_options_setkey(lrp, "interconn-tr",
                                                    trp->name);
    nbrec_logical_router_update_ports_addvalue(lr, lrp);
    return lrp;
}

static void
sync_ts_isb_pb(struct pb_input *pb, const struct sbrec_port_binding *sb_pb,
               const struct icsbrec_port_binding *isb_pb)
{
    const char *address = get_lp_address_for_sb_pb(pb, sb_pb);
    if (address) {
        icsbrec_port_binding_set_address(isb_pb, address);
    }

    const struct sbrec_port_binding *crp = find_crp_for_sb_pb(pb, sb_pb);
    if (crp && crp->chassis) {
        icsbrec_port_binding_set_gateway(isb_pb, crp->chassis->name);
    }

    update_isb_pb_external_ids(pb, sb_pb, isb_pb);

    /* Sync encap so that multiple encaps can be used for the same
     * gateway.  However, it is not needed for now, since we don't yet
     * support specifying encap type/ip for gateway chassis or ha-chassis
     * for logical router port in NB DB, and now encap should always be
     * empty.  The sync can be added if we add such support for gateway
     * chassis/ha-chassis in NB DB. */
}

static const struct sbrec_port_binding *
find_lsp_in_sb(struct pb_input *pb,
               const struct nbrec_logical_switch_port *lsp)
{
    return find_sb_pb_by_name(pb->sbrec_port_binding_by_name, lsp->name);
}

#if 0
static const struct nbrec_logical_switch *
find_ls_from_lsp(struct pb_input *pb,
                 const struct nbrec_logical_switch_port *lsp)
{
    struct nbrec_logical_switch *key =
        nbrec_logical_switch_index_init_row(pb->nbrec_ls_by_lsp_port);
    nbrec_logical_switch_index_set_ports(key,
        (struct nbrec_logical_switch_port **) &lsp, 1);
    const struct nbrec_logical_switch *ls =
        nbrec_logical_switch_index_find(pb->nbrec_ls_by_lsp_port, key);

    nbrec_logical_switch_index_destroy_row(key);
    return ls;
}
#endif

#if 1
static const struct nbrec_logical_switch *
find_ls_from_lsp(const struct nbrec_logical_switch_table *nb_ls_table,
                 const struct nbrec_logical_switch_port *lsp)
{
    const struct nbrec_logical_switch *ls;
    // Percorre os switches monitorados pelo IDL
    NBREC_LOGICAL_SWITCH_FOR_EACH (ls, nb_ls_table) {
        for (size_t i = 0; i < ls->n_ports; i++) {
            if (ls->ports[i] == lsp) {
                return ls; // Encontrou o switch que contém esta porta
            }
        }
    }
    return NULL;
}
#endif

static const struct nbrec_logical_switch_port *
find_nb_lsp_from_sb_pb(struct pb_input *pb,
                       const struct sbrec_port_binding *sb_pb)
{
    struct nbrec_logical_switch_port *key =
        nbrec_logical_switch_port_index_init_row(pb->nbrec_lsp_by_name);
    nbrec_logical_switch_port_index_set_name(key, sb_pb->logical_port);
    const struct nbrec_logical_switch_port *nb_lsp =
        nbrec_logical_switch_port_index_find(pb->nbrec_lsp_by_name, key);

    nbrec_logical_switch_port_index_destroy_row(key);
    return nb_lsp;
}

struct icsbrec_port_binding *
find_isb_pb_by_name(struct pb_input *pb, const char *name)
{
    struct icsbrec_port_binding *key =
        icsbrec_port_binding_index_init_row(pb->icsbrec_port_binding_by_name);
    
    icsbrec_port_binding_index_set_logical_port(key, name);
    struct icsbrec_port_binding *isb_pb =
        icsbrec_port_binding_index_find(pb->icsbrec_port_binding_by_name, key);

    icsbrec_port_binding_index_destroy_row(key);

    return isb_pb;
}

static bool
is_pb_associated_with_ts(struct pb_input *pb,
                         const struct sbrec_port_binding *sb_pb,
                         struct icnbrec_transit_switch *ts_key)
{
    if (!sb_pb->datapath) {
        return false;
    }

    const char *ls_name = smap_get(&sb_pb->datapath->external_ids, "name");
    if (!ls_name) {
        return false;
    }

    icnbrec_transit_switch_index_set_name(ts_key, ls_name);
    
    if (icnbrec_transit_switch_index_find(pb->icnbrec_transit_switch_by_name, 
                                          ts_key)) {
        return true;
    }

    return false;
}

static void
update_or_create_isb_pb(const struct engine_context *eng_ctx,
                        struct pb_input *pb,
                        struct ed_type_port_binding *pb_data,
                        const struct sbrec_port_binding *sb_pb,
                        const struct nbrec_logical_switch_port *nb_lsp,
                        const struct icsbrec_port_binding *isb_pb, 
                        const struct nbrec_logical_switch_table *nb_ls_table)
{
    VLOG_INFO("DBG-PG - - %s : %s : %d", __FILE__, __func__, __LINE__);
    log_lsp(nb_lsp);
    log_sb_pb(sb_pb);
    const struct nbrec_logical_switch *ls = find_ls_from_lsp(nb_ls_table, nb_lsp);
    log_ls(ls);
    /* The port is remote.*/
    if (!ls) {
        return;
    }
    VLOG_INFO("DBG-PG - - %s : %s : %d", __FILE__, __func__, __LINE__);
    const char *ts_name = smap_get(&ls->other_config, "interconn-ts"); //Aqui
    VLOG_INFO("DBG-PG - - %s : %s : %d", __FILE__, __func__, __LINE__);
    const struct icnbrec_transit_switch *t_sw, *t_sw_key;
    t_sw_key = icnbrec_transit_switch_index_init_row(
            pb->icnbrec_transit_switch_by_name);
    icnbrec_transit_switch_index_set_name(t_sw_key, ts_name);
    t_sw = icnbrec_transit_switch_index_find(
            pb->icnbrec_transit_switch_by_name, t_sw_key);
    icnbrec_transit_switch_index_destroy_row(t_sw_key);
    log_ts(t_sw);

    VLOG_INFO("DBG-PG - - %s : %s : %d", __FILE__, __func__, __LINE__);
    if (!strcmp(nb_lsp->type, "router")
                || !strcmp(nb_lsp->type, "switch")) {
        /* The port is local. */
        VLOG_INFO("DBG-PG - - %s : %s : %d", __FILE__, __func__, __LINE__);
        if (!isb_pb) {
            VLOG_INFO("DBG-PG CREATE ISB-PB- - %s : %s : %d", __FILE__, __func__, __LINE__); /*ISB-PB NEW*/
            isb_pb = create_isb_pb(eng_ctx, nb_lsp->name, pb->runned_az,
                                t_sw->name, &t_sw->header_.uuid,
                                "transit-switch-port", &pb_data->pb_tnlids);
            sync_ts_isb_pb(pb, sb_pb, isb_pb);
        } else { /*ISB-PB UPGRADE*/
            sync_local_port(pb, isb_pb, sb_pb, nb_lsp);
        }
#if 0
    } else if (!strcmp(nb_lsp->type, "remote")) {
        /* The port is remote. */
        if (!isb_pb) {
            nbrec_logical_switch_update_ports_delvalue(ls, nb_lsp);
        } else {
            sync_remote_port(pb, isb_pb, nb_lsp, sb_pb);
        }
#endif
    } else {
        VLOG_INFO("Ignore lsp %s on ts %s with type %s.",
                 nb_lsp->name, t_sw->name, nb_lsp->type);
    }
}

/* Handler functions. */
enum engine_input_handler_result
port_binding_en_ts_handler(struct engine_node *node,
                           void *data)
{
    return EN_HANDLED_UPDATED;
}

/* Handler functions. */
enum engine_input_handler_result
port_binding_sb_port_binding_handler(struct engine_node *node,
                                          void *data)
{
    VLOG_INFO("DBG-PG - - %s : %s : %d", __FILE__, __func__, __LINE__);
    const struct engine_context *eng_ctx = engine_get_context();
    struct pb_input pb_input;
    struct ed_type_port_binding *pb_data = data;
    const struct sbrec_port_binding_table *sb_pb_table =
        EN_OVSDB_GET(engine_get_input("SB_port_binding", node));
    const struct nbrec_logical_switch_table *nb_ls_table =
        EN_OVSDB_GET(engine_get_input("NB_logical_switch", node));

    port_binding_get_input_data(node, &pb_input);
    pb_input.runned_az = eng_ctx->client_ctx;

    const struct sbrec_port_binding *sb_pb;

    struct icnbrec_transit_switch *ts_key =
        icnbrec_transit_switch_index_init_row(
            pb_input.icnbrec_transit_switch_by_name);

    SBREC_PORT_BINDING_TABLE_FOR_EACH_TRACKED (sb_pb, sb_pb_table) {
        VLOG_INFO("DBG-PG - - %s : %s : %d", __FILE__, __func__, __LINE__);
        log_sb_pb(sb_pb);
        if (strcmp(sb_pb->type, "patch") != 0) {
            continue;
        }
        VLOG_INFO("DBG-PG - - %s : %s : %d", __FILE__, __func__, __LINE__);     /* sb_pb relationship with lrp */
        if (!is_pb_associated_with_ts(&pb_input, sb_pb, ts_key)) {
            continue;
        }
        VLOG_INFO("DBG-PG - - %s : %s : %d", __FILE__, __func__, __LINE__);
        const struct nbrec_logical_switch_port *nb_lsp =
            find_nb_lsp_from_sb_pb(&pb_input, sb_pb);
        VLOG_INFO("DBG-PG - - %s : %s : %d", __FILE__, __func__, __LINE__);
        if (!nb_lsp) {
            continue;
        }
        VLOG_INFO("DBG-PG - - %s : %s : %d", __FILE__, __func__, __LINE__);

        struct icsbrec_port_binding *isb_key =
            icsbrec_port_binding_index_init_row(
                pb_input.icsbrec_port_binding_by_name);
        icsbrec_port_binding_index_set_logical_port(isb_key,
                                                    sb_pb->logical_port);
        const struct icsbrec_port_binding *isb_pb =
            icsbrec_port_binding_index_find(
                pb_input.icsbrec_port_binding_by_name, isb_key);
        icsbrec_port_binding_index_destroy_row(isb_key);
        VLOG_INFO("DBG-PG - - %s : %s : %d", __FILE__, __func__, __LINE__);
        if (sbrec_port_binding_is_deleted(sb_pb)) {
            VLOG_INFO("DBG-PG - ###### PB-DELETED ###### - %s : %s : %d", __FILE__, __func__, __LINE__);
            struct icsbrec_port_binding *isb_key1 =
                icsbrec_port_binding_index_init_row(pb_input.icsbrec_port_binding_by_name);
            icsbrec_port_binding_index_set_logical_port(isb_key1, sb_pb->logical_port);

            const struct icsbrec_port_binding *isb_pb1 =
                icsbrec_port_binding_index_find(pb_input.icsbrec_port_binding_by_name, isb_key1);

            icsbrec_port_binding_index_destroy_row(isb_key1);
            if (isb_pb1) {
                VLOG_INFO("Deletando porta %s do IC-SB (removida do SB local)", 
                          sb_pb->logical_port);
                icsbrec_port_binding_delete(isb_pb1);
            }
            continue;
        } else {
            VLOG_INFO("DBG-PG ADD|UPDATED - - %s : %s : %d", __FILE__, __func__, __LINE__);

            struct shash_node *node2;

            if (&pb_data->switch_all_local_pbs == NULL) {
                VLOG_INFO("DBG-PG NULLL - - %s : %s : %d", __FILE__, __func__, __LINE__);
            }

            /* Supondo que 'my_shash' seja o seu dicionário de strings */
            SHASH_FOR_EACH (node2, &pb_data->switch_all_local_pbs) {
                VLOG_INFO("DBG-PG - - %s : %s : %d", __FILE__, __func__, __LINE__);
                const char *name = node2->name;    // A chave (string)
                VLOG_INFO("DBG-PG - - %s : %s : %d", __FILE__, __func__, __LINE__);
                const struct icsbrec_port_binding *data1 = node2->data; // O valor (ponteiro)

                VLOG_INFO("KKKKKKK->Porta: %s com tnl_key: %"PRId64, name, data1->tunnel_key);
            // Faça algo com os dados aqui
}
            update_or_create_isb_pb(eng_ctx, &pb_input, pb_data, sb_pb, nb_lsp, isb_pb, nb_ls_table);
            continue;
        }
    }
    icnbrec_transit_switch_index_destroy_row(ts_key);
    VLOG_INFO("DBG-PG - - %s : %s : %d", __FILE__, __func__, __LINE__);
    return EN_HANDLED_UPDATED;
}

enum engine_input_handler_result
port_binding_nb_logical_switch_port_handler(struct engine_node *node,
                                            void *data)
{
    VLOG_INFO("DBG-PG - - %s : %s : %d", __FILE__, __func__, __LINE__);
    const struct engine_context *eng_ctx = engine_get_context();
    struct ed_type_port_binding *pb_data = data;
    struct pb_input pb_input;

    const struct nbrec_logical_switch_port_table *nb_lsp_table =
        EN_OVSDB_GET(engine_get_input("NB_logical_switch_port", node));

    port_binding_get_input_data(node, &pb_input);
    pb_input.runned_az = eng_ctx->client_ctx;

    const struct nbrec_logical_switch_port *nb_lsp;
    NBREC_LOGICAL_SWITCH_PORT_TABLE_FOR_EACH_TRACKED (nb_lsp, nb_lsp_table) {
        if (nbrec_logical_switch_port_is_deleted(nb_lsp)) {
            VLOG_INFO("DBG-PG - - %s : %s : %d", __FILE__, __func__, __LINE__);
            const char *port_name = nb_lsp->name;
            if (!port_name) {
                continue;
            }
            struct icsbrec_port_binding *isb_pb =
                find_isb_pb_by_name(&pb_input, port_name);
            if (isb_pb) {
                VLOG_INFO("DBG-PG - - %s : %s : %d", __FILE__, __func__, __LINE__);
                icsbrec_port_binding_delete(isb_pb);
            }
        } else if (nbrec_logical_switch_port_is_new(nb_lsp)) {
            VLOG_INFO("DBG-PG - - %s : %s : %d", __FILE__, __func__, __LINE__);
            log_lsp(nb_lsp);
        } else {
            VLOG_INFO("DBG-PG LSP-UPGRADE - - %s : %s : %d", __FILE__, __func__, __LINE__);
            log_lsp(nb_lsp);
        }
    }

    return EN_HANDLED_UPDATED;
}

enum engine_input_handler_result
port_binding_icsb_port_binding_handler(struct engine_node *node,
                                       void *data)
{
    VLOG_INFO("DBG-PG - - %s : %s : %d", __FILE__, __func__, __LINE__);
    const struct engine_context *eng_ctx = engine_get_context();
    struct ed_type_port_binding *pb_data = data;
    struct pb_input pb_input;

    const struct icsbrec_port_binding_table *isb_pb_table =
        EN_OVSDB_GET(engine_get_input("ICSB_port_binding", node));

    port_binding_get_input_data(node, &pb_input);
    pb_input.runned_az = eng_ctx->client_ctx;

    const struct icsbrec_port_binding *isb_pb;
    ICSBREC_PORT_BINDING_TABLE_FOR_EACH_TRACKED (isb_pb, isb_pb_table) {
        if (icsbrec_port_binding_is_deleted(isb_pb)) {
            const struct icsbrec_port_binding *isb_pb_old;
            VLOG_INFO("DBG-PG - - %s : %s : %d", __FILE__, __func__, __LINE__);
            if (ic_pb_get_type(isb_pb) != IC_ROUTER_PORT) {
                isb_pb_old =
                    shash_find_and_delete(&pb_data->switch_all_local_pbs,
                                      isb_pb->logical_port);
                VLOG_INFO("DBG-PG - - %s : %s : %d", __FILE__, __func__, __LINE__);
            } else {
                isb_pb_old =
                    shash_find_and_delete(&pb_data->router_all_local_pbs,
                                      isb_pb->logical_port);
                VLOG_INFO("DBG-PG - - %s : %s : %d", __FILE__, __func__, __LINE__);
            }
            VLOG_INFO("DBG-PG - - %s : %s : %d", __FILE__, __func__, __LINE__);
            ovn_free_tnlid(&pb_data->pb_tnlids, isb_pb->tunnel_key);
            VLOG_INFO("DBG-PG - - %s : %s : %d", __FILE__, __func__, __LINE__);

        } else if (icsbrec_port_binding_is_new(isb_pb)) {
            VLOG_INFO("DBG-PG  AQUIIIIII- - %s : %s : %d", __FILE__, __func__, __LINE__);
            log_isb_pb(isb_pb);
            /* This port is router, not need create lsp for it */
            if (uuid_equals(&isb_pb->availability_zone->header_, &pb_input.runned_az->header_ )) {
                VLOG_INFO("DBG-PG  AQUIIIIII- - %s : %s : %d", __FILE__, __func__, __LINE__);
                return EN_HANDLED_UPDATED;
            }
            VLOG_INFO("DBG-PG  AQUIIIIII- - %s : %s : %d", __FILE__, __func__, __LINE__);
            const struct nbrec_logical_switch *ls =
                find_ts_in_nb(pb_input.nbrec_ls_by_name,
                              isb_pb->transit_switch);
            log_ls(ls);
            VLOG_INFO("DBG-PG CRIANDO LSP REMOTA a PARTIR DO ISB_PB aprendido - - %s : %s : %d", __FILE__, __func__, __LINE__);
            create_nb_lsp(eng_ctx, isb_pb, ls);
            VLOG_INFO("DBG-PG - - %s : %s : %d", __FILE__, __func__, __LINE__);

            ic_pb_get_type(isb_pb) != IC_ROUTER_PORT
            ? shash_add(&pb_data->switch_all_local_pbs, isb_pb->logical_port,
                        isb_pb)
            : shash_add(&pb_data->router_all_local_pbs, isb_pb->logical_port,
                        isb_pb);
            VLOG_INFO("DBG-PG - - %s : %s : %d", __FILE__, __func__, __LINE__);
            ovn_add_tnlid(&pb_data->pb_tnlids, isb_pb->tunnel_key);
            VLOG_INFO("DBG-PG - - %s : %s : %d", __FILE__, __func__, __LINE__);
        } else if (icsbrec_port_binding_is_updated(isb_pb,
            ICSBREC_PORT_BINDING_COL_TUNNEL_KEY)) {
                VLOG_INFO("DBG-PG  AQUIIIIII22222222- - %s : %s : %d", __FILE__, __func__, __LINE__);
                log_isb_pb(isb_pb);
                const struct icsbrec_port_binding *isb_pb_old;
                isb_pb_old = shash_find(&pb_data->switch_all_local_pbs,
                                         isb_pb->tunnel_key);
                VLOG_INFO("DBG-PG - - %s : %s : %d", __FILE__, __func__, __LINE__);
                ovn_free_tnlid(&pb_data->pb_tnlids, isb_pb_old->tunnel_key);
                VLOG_INFO("DBG-PG - - %s : %s : %d", __FILE__, __func__, __LINE__);
                ovn_add_tnlid(&pb_data->pb_tnlids, isb_pb->tunnel_key);
                VLOG_INFO("DBG-PG - - %s : %s : %d", __FILE__, __func__, __LINE__);
        }
    }
    VLOG_INFO("DBG-PG - - %s : %s : %d", __FILE__, __func__, __LINE__);
    return EN_HANDLED_UPDATED;
}

static void
log_lsp(const struct nbrec_logical_switch_port *lsp)
{
    if (!lsp) {
        VLOG_INFO("Logical_Switch_Port: [NULL]");
        return;
    }

    VLOG_INFO("=====================");
    VLOG_INFO("Logical_Switch_Port Dump:");
    VLOG_INFO("  Name: %s", lsp->name);
    VLOG_INFO("  Type: %s", lsp->type);
    VLOG_INFO("  UUID: "UUID_FMT, UUID_ARGS(&lsp->header_.uuid));

    /* 1. Status da Porta */
    if (lsp->n_enabled) {
        VLOG_INFO("  Enabled: %s", *lsp->enabled ? "true" : "false");
    }
    if (lsp->n_up) {
        VLOG_INFO("  Up: %s", *lsp->up ? "true" : "false");
    }

    /* 2. Endereçamento (Addresses & Dynamic) */
    VLOG_INFO("  Addresses Count: %"PRIuSIZE, lsp->n_addresses);
    for (size_t i = 0; i < lsp->n_addresses; i++) {
        VLOG_INFO("    Addr[%"PRIuSIZE"]: %s", i, lsp->addresses[i]);
    }
    if (lsp->dynamic_addresses) {
        VLOG_INFO("  Dynamic Addresses: %s", lsp->dynamic_addresses);
    }

    /* 3. Conexões e Peers */
    if (lsp->peer) {
        VLOG_INFO("  Peer: %s", lsp->peer);
    }
    if (lsp->parent_name) {
        VLOG_INFO("  Parent Name: %s", lsp->parent_name);
    }

    /* 4. Opções (Fundamental para o tipo 'router') */
    struct smap_node *node;
    SMAP_FOR_EACH (node, &lsp->options) {
        VLOG_INFO("  Option: %s = %s", node->key, node->value);
    }

    /* 5. External IDs */
    SMAP_FOR_EACH (node, &lsp->external_ids) {
        VLOG_INFO("  External ID: %s = %s", node->key, node->value);
    }

    /* 6. Port Security e VLAN Tags */
    if (lsp->n_port_security) {
        VLOG_INFO("  Port Security Count: %"PRIuSIZE, lsp->n_port_security);
    }
    if (lsp->n_tag) {
        VLOG_INFO("  Tag: %"PRId64, *lsp->tag);
    }

    /* 7. DHCP e HA Chassis */
    if (lsp->dhcpv4_options) {
        VLOG_INFO("  DHCPv4 Options UUID: "UUID_FMT, 
                  UUID_ARGS(&lsp->dhcpv4_options->header_.uuid));
    }
    if (lsp->ha_chassis_group) {
        VLOG_INFO("  HA Chassis Group: %s", lsp->ha_chassis_group->name);
    }
    VLOG_INFO("=====================");
}

static void
log_ls(const struct nbrec_logical_switch *ls)
{
    if (!ls) {
        VLOG_INFO("Logical_Switch: [NULL]");
        return;
    }

    /* 1. Identificação Básica */
    VLOG_INFO("=====================");
    VLOG_INFO("Logical_Switch Dump:");
    VLOG_INFO("  Name: %s", ls->name); /* Always nonnull segundo sua struct */
    VLOG_INFO("  UUID: "UUID_FMT, UUID_ARGS(&ls->header_.uuid));

    /* 2. Ports (Relação com LSPs) */
    VLOG_INFO("  Ports Count: %"PRIuSIZE, ls->n_ports);
    for (size_t i = 0; i < ls->n_ports; i++) {
        VLOG_INFO("    Port[%"PRIuSIZE"]: %s", i, ls->ports[i]->name);
    }

    /* 3. External IDs (Onde fica o interconn-ts) */
    struct smap_node *node;
    SMAP_FOR_EACH (node, &ls->external_ids) {
        VLOG_INFO("  External ID: %s = %s", node->key, node->value);
    }

    /* 4. Other Config */
    SMAP_FOR_EACH (node, &ls->other_config) {
        VLOG_INFO("  Other Config: %s = %s", node->key, node->value);
    }

    /* 5. Load Balancers e Grupos */
    VLOG_INFO("  Load Balancers: %"PRIuSIZE" | LB Groups: %"PRIuSIZE, 
              ls->n_load_balancer, ls->n_load_balancer_group);

    /* 6. ACLs e QoS */
    VLOG_INFO("  ACLs: %"PRIuSIZE" | QoS Rules: %"PRIuSIZE, 
              ls->n_acls, ls->n_qos_rules);

    /* 7. DNS e Forwarding Groups */
    VLOG_INFO("  DNS Records: %"PRIuSIZE" | Fwd Groups: %"PRIuSIZE,
              ls->n_dns_records, ls->n_forwarding_groups);

    /* 8. CoPP */
    if (ls->copp) {
        VLOG_INFO("  CoPP: %s", ls->copp->name ? ls->copp->name : "unnamed");
    }
    VLOG_INFO("=====================");
}

static void
log_ts(const struct icnbrec_transit_switch *ts)
{
    if (!ts) {
        VLOG_INFO("Transit_Switch: [NULL]");
        return;
    }

    VLOG_INFO("=====================");
    VLOG_INFO("Transit_Switch Dump:");
    VLOG_INFO("  Name: %s", ts->name); /* Always nonnull */
    VLOG_INFO("  UUID: "UUID_FMT, UUID_ARGS(&ts->header_.uuid));

    /* 1. External IDs */
    struct smap_node *node;
    if (!smap_is_empty(&ts->external_ids)) {
        SMAP_FOR_EACH (node, &ts->external_ids) {
            VLOG_INFO("  External ID: %s = %s", node->key, node->value);
        }
    } else {
        VLOG_INFO("  External IDs: (empty)");
    }

    /* 2. Other Config */
    if (!smap_is_empty(&ts->other_config)) {
        SMAP_FOR_EACH (node, &ts->other_config) {
            VLOG_INFO("  Other Config: %s = %s", node->key, node->value);
        }
    } else {
        VLOG_INFO("  Other Config: (empty)");
    }
    VLOG_INFO("=====================");
}

/* Certifique-se de que esta definição está VISÍVEL antes da função */
struct tnlid_node {
    struct hmap_node hmap_node;
    uint32_t tnlid;
};

static void
log_sb_pb(const struct sbrec_port_binding *pb)
{
    if (!pb) {
        VLOG_INFO("Port_Binding (SB): [NULL]");
        return;
    }

    VLOG_INFO("=====================");
    VLOG_INFO("SB Port_Binding Dump:");
    /* 1. Identificação e Tipo */
    VLOG_INFO("  Logical Port: %s", pb->logical_port);
    VLOG_INFO("  Type: %s", pb->type);
    VLOG_INFO("  Tunnel Key: %"PRId64, pb->tunnel_key);
    VLOG_INFO("  UUID: "UUID_FMT, UUID_ARGS(&pb->header_.uuid));

    /* 2. Datapath (Vínculo com o Switch/Router) */
    if (pb->datapath) {
        VLOG_INFO("  Datapath UUID: "UUID_FMT, 
                  UUID_ARGS(&pb->datapath->header_.uuid));
    }

    /* 3. Rede (MACs e Endereços) */
    for (size_t i = 0; i < pb->n_mac; i++) {
        VLOG_INFO("  MAC[%s]: %s", "PRIuSIZE", pb->mac[i]);
    }
    if (pb->n_tag) {
        VLOG_INFO("  VLAN Tag: %"PRId64, *pb->tag);
    }

    /* 4. Localização (Chassis Binding) */
    if (pb->chassis) {
        VLOG_INFO("  Chassis: %s", pb->chassis->name);
    }
    if (pb->encap) {
        VLOG_INFO("  Encap Type: %s | IP: %s", pb->encap->type, pb->encap->ip);
    }
    if (pb->n_up) {
        VLOG_INFO("  Status UP: %s", *pb->up ? "true" : "false");
    }

    /* 5. Hierarquia (Parent/Virtual) */
    if (pb->parent_port) {
        VLOG_INFO("  Parent Port: %s", pb->parent_port);
    }
    if (pb->virtual_parent) {
        VLOG_INFO("  Virtual Parent: %s", pb->virtual_parent);
    }

    /* 6. Opções e External IDs */
    struct smap_node *node;
    SMAP_FOR_EACH (node, &pb->options) {
        VLOG_INFO("  Option: %s = %s", node->key, node->value);
    }
    SMAP_FOR_EACH (node, &pb->external_ids) {
        VLOG_INFO("  External ID: %s = %s", node->key, node->value);
    }

    /* 7. Chassis Adicionais e HA */
    if (pb->n_additional_chassis) {
        VLOG_INFO("  Additional Chassis Count: %"PRIuSIZE,
                  pb->n_additional_chassis);
    }
    if (pb->ha_chassis_group) {
        VLOG_INFO("  HA Group: %s", pb->ha_chassis_group->name);
    }
    VLOG_INFO("=====================");
}

static void
log_isb_pb(const struct icsbrec_port_binding *isb_pb)
{
    if (!isb_pb) {
        VLOG_INFO("IC-SB Port_Binding: [NULL]");
        return;
    }

    VLOG_INFO("=====================");
    VLOG_INFO("IC-SB Port_Binding Dump:");
    
    /* 1. Identificação de Rede Global */
    VLOG_INFO("  Logical Port: %s", isb_pb->logical_port);
    VLOG_INFO("  Type: %s", isb_pb->type ? isb_pb->type : "<none>");
    VLOG_INFO("  Transit Switch: %s", isb_pb->transit_switch);
    VLOG_INFO("  Tunnel Key: %"PRId64, isb_pb->tunnel_key);
    VLOG_INFO("  UUID: "UUID_FMT, UUID_ARGS(&isb_pb->header_.uuid));

    /* 2. Endereçamento e Roteamento */
    VLOG_INFO("  Address: %s", isb_pb->address);
    VLOG_INFO("  Gateway: %s", isb_pb->gateway);

    /* 3. Localização (AZ e Encapsulamento) */
    if (isb_pb->availability_zone) {
        VLOG_INFO("  AZ: %s (UUID: "UUID_FMT")", 
                  isb_pb->availability_zone->name,
                  UUID_ARGS(&isb_pb->availability_zone->header_.uuid));
    }

    if (isb_pb->encap) {
        VLOG_INFO("  Encap: %s (IP: %s)", isb_pb->encap->type,
                  isb_pb->encap->ip);
    }

    /* 4. Rastreamento NB-IC (Se aplicável) */
    if (isb_pb->n_nb_ic_uuid) {
        VLOG_INFO("  NB-IC UUID: "UUID_FMT, UUID_ARGS(isb_pb->nb_ic_uuid));
    }

    /* 5. Metadados (External IDs) */
    struct smap_node *node;
    if (!smap_is_empty(&isb_pb->external_ids)) {
        SMAP_FOR_EACH (node, &isb_pb->external_ids) {
            VLOG_INFO("  External ID: %s = %s", node->key, node->value);
        }
    }
    VLOG_INFO("=====================");
}
