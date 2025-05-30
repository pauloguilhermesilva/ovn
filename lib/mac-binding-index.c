/* Copyright (c) 2022, Red Hat, Inc.
 *
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

#include "lib/mac-binding-index.h"
#include "lib/ovn-sb-idl.h"

struct ovsdb_idl_index *
mac_binding_by_datapath_index_create(struct ovsdb_idl *idl)
{
    return ovsdb_idl_index_create1(idl, &sbrec_mac_binding_col_datapath);
}

struct ovsdb_idl_index *
mac_binding_by_lport_ip_index_create(struct ovsdb_idl *idl)
{
    return ovsdb_idl_index_create2(idl,
                                   &sbrec_mac_binding_col_logical_port,
                                   &sbrec_mac_binding_col_ip);
}

const struct sbrec_mac_binding *
mac_binding_lookup(struct ovsdb_idl_index *sbrec_mac_binding_by_lport_ip,
                   const char *logical_port, const char *ip) {
    struct sbrec_mac_binding *mb =
            sbrec_mac_binding_index_init_row(sbrec_mac_binding_by_lport_ip);
    sbrec_mac_binding_index_set_logical_port(mb, logical_port);
    sbrec_mac_binding_index_set_ip(mb, ip);

    const struct sbrec_mac_binding *retval =
            sbrec_mac_binding_index_find(sbrec_mac_binding_by_lport_ip, mb);

    sbrec_mac_binding_index_destroy_row(mb);

    return retval;
}
