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

#include "en-ic.h"
#include "lib/inc-proc-eng.h"
#include "lib/stopwatch-names.h"
#include "ovn-ic.h"
#include "openvswitch/vlog.h"

VLOG_DEFINE_THIS_MODULE(en_ic);

enum engine_node_state
en_ic_run(struct engine_node *node OVS_UNUSED, void *data OVS_UNUSED)
{
    /* 'en_ic' is the engine's output node and only aggregates the per
     * subsystem nodes (gateway, ts, tr, port_binding, route, service_monitor
     * and address_set).  Each of those nodes performs (and gates) its own
     * work, so this node has nothing to compute itself. */
    return EN_UPDATED;
}

void *
en_ic_init(struct engine_node *node OVS_UNUSED,
            struct engine_arg *arg OVS_UNUSED)
{
    return NULL;
}

void
en_ic_cleanup(void *data OVS_UNUSED)
{
}
