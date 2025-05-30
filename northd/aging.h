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

#ifndef AGING_H
#define AGING_H 1

#include "lib/inc-proc-eng.h"

/* The MAC binding aging node functions. */
enum engine_node_state en_mac_binding_aging_run(struct engine_node *node,
                                                void *data);
void *en_mac_binding_aging_init(struct engine_node *node,
                                struct engine_arg *arg);
void en_mac_binding_aging_cleanup(void *data);

/* The MAC binding aging waker node functions. */
enum engine_node_state en_mac_binding_aging_waker_run(struct engine_node *node,
                                                      void *data);
void *en_mac_binding_aging_waker_init(struct engine_node *node,
                                      struct engine_arg *arg);
void en_mac_binding_aging_waker_cleanup(void *data);

/* The MAC binding aging helper function. */
unsigned int min_mac_binding_age_threshold(const char *opt);

/* The FDB aging node functions. */
enum engine_node_state en_fdb_aging_run(struct engine_node *node, void *data);
void *en_fdb_aging_init(struct engine_node *node, struct engine_arg *arg);
void en_fdb_aging_cleanup(void *data);

/* The FDB aging waker node functions. */
enum engine_node_state en_fdb_aging_waker_run(struct engine_node *node,
                                              void *data);
void *en_fdb_aging_waker_init(struct engine_node *node,
                              struct engine_arg *arg);
void en_fdb_aging_waker_cleanup(void *data);

#endif /* northd/aging.h */
