/*
 * Copyright (c) 2014, 2015, 2016, 2017 Nicira, Inc.
 * Copyright (c) 2019 Mellanox Technologies, Ltd.
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

#include <rte_flow.h>

#include "cmap.h"
#include "dpif-netdev.h"
#include "netdev-offload-provider.h"
#include "netdev-provider.h"
#include "openvswitch/match.h"
#include "openvswitch/vlog.h"
#include "packets.h"
#include "uuid.h"
#include "netdev-offload-dpdk-private.h"

VLOG_DEFINE_THIS_MODULE(netdev_offload_dpdk);

/* Thread-safety
 * =============
 *
 * Below API is NOT thread safe in following terms:
 *
 *  - The caller must be sure that none of these functions will be called
 *    simultaneously.  Even for different 'netdev's.
 *
 *  - The caller must be sure that 'netdev' will not be destructed/deallocated.
 *
 *  - The caller must be sure that 'netdev' configuration will not be changed.
 *    For example, simultaneous call of 'netdev_reconfigure()' for the same
 *    'netdev' is forbidden.
 *
 * For current implementation all above restrictions could be fulfilled by
 * taking the datapath 'port_mutex' in lib/dpif-netdev.c.  */

/*
 * A mapping from ufid to dpdk rte_flow.
 */
struct ufid_to_rte_flow_data {
    struct cmap_node node;
    ovs_u128 ufid;
    struct rte_flow *rte_flow;
    struct dpif_flow_stats stats;
    struct ovs_refcount refcnt;
};

static struct ufid_to_rte_flow_data * 
ufid_to_flow_data_find(struct netdev *netdev, const ovs_u128 *ufid)
{
    size_t hash = hash_bytes(ufid, sizeof *ufid, 0);
    struct ufid_to_rte_flow_data *data;

    struct cmap *hw_flows = &netdev->hw_info.hw_flows;

    CMAP_FOR_EACH_WITH_HASH (data, node, hash, hw_flows) {
        if (ovs_u128_equals(*ufid, data->ufid)) {
            if (ovs_refcount_try_ref_rcu(&data->refcnt))
                return data;
        }
    }
    return NULL;
}

static inline void
ufid_to_flow_data_insert(struct netdev *netdev, struct ufid_to_rte_flow_data *data)
{
    size_t hash = hash_bytes(&data->ufid, sizeof(data->ufid), 0);
    struct cmap *hw_flows = &netdev->hw_info.hw_flows;
    cmap_insert(hw_flows,
            CONST_CAST(struct cmap_node *, &data->node), hash);
}

static void
ufid_to_flow_data_unref(struct ufid_to_rte_flow_data *fd)
{
    ovs_refcount_unref(&fd->refcnt);
}

static inline void
ufid_to_flow_data_remove(struct netdev *netdev, struct ufid_to_rte_flow_data *data)
{
    size_t hash = hash_bytes(&data->ufid, sizeof(data->ufid), 0);
    struct cmap *hw_flows = &netdev->hw_info.hw_flows;
    cmap_remove(hw_flows,
            CONST_CAST(struct cmap_node *, &data->node), hash);
    ufid_to_flow_data_unref(data);
}

/* Find rte_flow with @ufid. */
static struct rte_flow *
ufid_to_rte_flow_find(struct netdev* netdev, const ovs_u128 *ufid)
{
    struct ufid_to_rte_flow_data *data;
    data = ufid_to_flow_data_find(netdev, ufid);
    if (!data) 
        return NULL;
    return data->rte_flow;
}

static inline void
ufid_to_rte_flow_associate(struct netdev *netdev,
                           const ovs_u128 *ufid,
                           struct rte_flow *rte_flow)
{
    struct ufid_to_rte_flow_data *data = xzalloc(sizeof *data);

    /*
     * We should not simply overwrite an existing rte flow.
     * We should have deleted it first before re-adding it.
     * Thus, if following assert triggers, something is wrong:
     * the rte_flow is not destroyed.
     */
    ovs_assert(ufid_to_rte_flow_find(netdev, ufid) == NULL);

    data->ufid = *ufid;
    data->rte_flow = rte_flow;
    ovs_refcount_init(&data->refcnt);


    ufid_to_flow_data_insert(netdev, data);
}

static int
netdev_offload_dpdk_add_flow(struct netdev *netdev,
                             const struct match *match,
                             struct nlattr *nl_actions,
                             size_t actions_len,
                             const ovs_u128 *ufid,
                             struct offload_info *info)
{
    struct rte_flow_attr flow_attr = {
        .group = 0,
        .priority = 0,
        .ingress = 1,
        .egress = 0,
        .transfer = 1
    };

    struct rte_flow_item *patterns;
    struct flow_actions actions = { .actions = NULL, .cnt = 0 };
    struct rte_flow_error error;
    struct rte_flow *flow;
    int ret = 0;

    ret = netdev_dpdk_flow_patterns_add(&patterns, match, info);
    if (ret) {
        VLOG_WARN("Adding rte match patterns for flow ufid"UUID_FMT" failed",
                  UUID_ARGS((struct uuid *)ufid));
        goto out;
    }

    info->actions_offloaded = !netdev_dpdk_flow_actions_add(&actions,
                                                            nl_actions,
                                                            actions_len, info);
    if (!info->actions_offloaded) {
        /* if we failed to offload the rule actions fallback to mark rss
         * actions.
         */
#if 0 /* currently we do not use mark offload */
        netdev_dpdk_flow_actions_free(&actions);
        netdev_dpdk_flow_actions_add_mark_rss(&actions, netdev,
                                              info->flow_mark);
        flow_attr.transfer = 0;
#endif
        ret = -1;
        goto out;
    }

    flow = netdev_dpdk_rte_flow_create(netdev, &flow_attr,
                                       patterns,
                                       actions.actions, &error);

    if (!flow) {
        info->actions_offloaded = 0;
        VLOG_ERR("%s: rte flow create error: %u : message : %s\n",
                 netdev_get_name(netdev), error.type, error.message);
        ret = -1;
        goto out;
    }
    ufid_to_rte_flow_associate(netdev, ufid, flow);
    VLOG_DBG("%s: installed flow %p by ufid "UUID_FMT"\n",
             netdev_get_name(netdev), flow, UUID_ARGS((struct uuid *)ufid));

out:
    netdev_dpdk_flow_actions_free(&actions);
    return ret;
}

/*
 * Check if any unsupported flow patterns are specified.
 */
static int
netdev_offload_dpdk_validate_flow(const struct match *match)
{
    struct match match_zero_wc;
    const struct flow *masks = &match->wc.masks;

    /* Create a wc-zeroed version of flow. */
    match_init(&match_zero_wc, &match->flow, &match->wc);

    if (masks->metadata || masks->skb_priority ||
        masks->pkt_mark || masks->dp_hash) {
        VLOG_ERR("offload failed due to metadata/pkt_mark/hash/prio as match\n");
        goto err;
    }

    /* recirc id must be zero. */
    if (match_zero_wc.flow.recirc_id) {
        VLOG_ERR("offload failed due to recirc_id as match\n");
        goto err;
    }

    if (masks->ct_state || masks->ct_nw_proto ||
        masks->ct_zone  || masks->ct_mark     ||
        !ovs_u128_is_zero(masks->ct_label)) {
        VLOG_ERR("offload failed due to ct as match\n");
        goto err;
    }

    if (masks->conj_id || masks->actset_output) {
        VLOG_ERR("offload failed due to conj_id/actset_output as match\n");
        goto err;
    }

    /* Unsupported L2. */
    if (!is_all_zeros(masks->mpls_lse, sizeof masks->mpls_lse)) {
        VLOG_ERR("offload failed due to L2:mpls_lse as match\n");
        goto err;
    }

    /* Unsupported L3. */
    if (masks->ipv6_label || masks->ct_nw_src || masks->ct_nw_dst     ||
        !is_all_zeros(&masks->ipv6_src,    sizeof masks->ipv6_src)    ||
        !is_all_zeros(&masks->ipv6_dst,    sizeof masks->ipv6_dst)    ||
        !is_all_zeros(&masks->ct_ipv6_src, sizeof masks->ct_ipv6_src) ||
        !is_all_zeros(&masks->ct_ipv6_dst, sizeof masks->ct_ipv6_dst) ||
        !is_all_zeros(&masks->nd_target,   sizeof masks->nd_target)   ||
        !is_all_zeros(&masks->nsh,         sizeof masks->nsh)         ||
        !is_all_zeros(&masks->arp_sha,     sizeof masks->arp_sha)     ||
        !is_all_zeros(&masks->arp_tha,     sizeof masks->arp_tha)) {
        VLOG_ERR("offload failed due to L3:v6/arp as match\n");
        goto err;
    }

    /* If fragmented, then don't HW accelerate - for now. */
    if (match_zero_wc.flow.nw_frag) {
        VLOG_ERR("offload failed due to L3:frag as match\n");
        goto err;
    }

    /* Unsupported L4. */
    if (masks->igmp_group_ip4 || masks->ct_tp_src || masks->ct_tp_dst) {
        VLOG_ERR("offload failed due to L4:igmp/ct as match\n");
        goto err;
    }

    return 0;

err:
    return -1;
}

static int
netdev_offload_dpdk_destroy_flow(struct netdev *netdev,
                                 const ovs_u128 *ufid,
                                 struct ufid_to_rte_flow_data *fd)
{
    ufid_to_flow_data_remove(netdev, fd);

    do {
        /* do nothig */
        /* ovs_pause */
    } while(!__sync_bool_compare_and_swap(&fd->refcnt.count, 1, 0));

    struct rte_flow_error error;
    int ret = netdev_dpdk_rte_flow_destroy(netdev, fd->rte_flow, &error);

    if (ret == 0) {
        VLOG_DBG("%s: removed rte flow %p associated with ufid " UUID_FMT "\n",
                 netdev_get_name(netdev), fd->rte_flow,
                 UUID_ARGS((struct uuid *)ufid));
        free(fd);
    } else {
        ovs_refcount_init(&fd->refcnt);
        ufid_to_flow_data_insert(netdev, fd);
        VLOG_ERR("%s: rte flow destroy error: %u : message : %s\n",
                 netdev_get_name(netdev), error.type, error.message);
    }
    return ret;
}

static int
netdev_offload_dpdk_flow_put(struct netdev *netdev, struct match *match,
                             struct nlattr *actions, size_t actions_len,
                             const ovs_u128 *ufid, struct offload_info *info,
                             struct dpif_flow_stats *stats OVS_UNUSED)
{
    struct ufid_to_rte_flow_data *fd;
    int ret;

    /*
     * If an old rte_flow exists, it means it's a flow modification.
     * Here destroy the old rte flow first before adding a new one.
     */
    fd = ufid_to_flow_data_find(netdev, ufid);
    if (fd) {
        ret = netdev_offload_dpdk_destroy_flow(netdev, ufid, fd);
        if (ret < 0) {
            return ret;
        }
    }

    ret = netdev_offload_dpdk_validate_flow(match);
    if (ret < 0) {
        return ret;
    }

    return netdev_offload_dpdk_add_flow(netdev, match, actions,
                                        actions_len, ufid, info);
}

static int
netdev_offload_dpdk_flow_del(struct netdev *netdev, const ovs_u128 *ufid,
                             struct dpif_flow_stats *stats OVS_UNUSED)
{
    struct ufid_to_rte_flow_data *fd = ufid_to_flow_data_find(netdev, ufid);

    if (!fd) {
        return -1;
    }

    return netdev_offload_dpdk_destroy_flow(netdev, ufid, fd);
}

static int
netdev_offload_dpdk_flow_flush(struct netdev *netdev)
{
    struct rte_flow_error error;
    int ret;

    struct ufid_to_rte_flow_data *data;
    CMAP_FOR_EACH(data, node, &netdev->hw_info.hw_flows) {
        ufid_to_flow_data_remove(netdev, data);
        if (ovs_refcount_read(&data->refcnt) == 0)
            free(data);
        else
            ovsrcu_postpone(free, data);
    }

    ret = netdev_dpdk_rte_flow_flush(netdev, &error);
    if (ret) {
        VLOG_ERR("%s: rte flow flush error: %u : message : %s\n",
                 netdev_get_name(netdev), error.type, error.message);
    }

    return ret;
}

static int
netdev_offload_dpdk_flow_stats_get(struct netdev *netdev,
                                   const ovs_u128 *ufid,
                                   struct dpif_flow_stats *stats)
{
    struct rte_flow_query_count query = { .reset = 1 };
    struct rte_flow_error error;
    struct rte_flow *rte_flow;
    int ret;

    struct ufid_to_rte_flow_data *fd = ufid_to_flow_data_find(netdev, ufid);
    if (!fd)
        return -1;
    rte_flow = fd->rte_flow;    

    if (stats->used && fd->stats.used && stats->used == fd->stats.used) {
        stats->n_packets += fd->stats.n_packets;
        stats->n_bytes += fd->stats.n_bytes;
        ufid_to_flow_data_unref(fd);
        return 0;
    }

    ret = netdev_dpdk_rte_flow_query(netdev, rte_flow, &query, &error);
    if (ret) {
        VLOG_DBG("ufid "UUID_FMT
                 " flow %p query for '%s' failed: %u, %s\n",
                 UUID_ARGS((struct uuid *)ufid), rte_flow,
                 netdev_get_name(netdev), error.type, error.message);
        ufid_to_flow_data_unref(fd);
        return -1;
    }

    stats->n_packets += (query.hits_set) ? query.hits : 0;
    stats->n_bytes += (query.bytes_set) ? query.bytes : 0;

    if (stats->used) { 
        fd->stats.used = stats->used;
        fd->stats.n_packets = (query.hits_set) ? query.hits : 0;
        fd->stats.n_bytes = (query.bytes_set) ? query.bytes : 0;
    }
    ufid_to_flow_data_unref(fd);
    return 0;
}

static int
netdev_offload_dpdk_init_flow_api(struct netdev *netdev)
{
    return netdev_dpdk_flow_api_supported(netdev) ? 0 : EOPNOTSUPP;
}

const struct netdev_flow_api netdev_offload_dpdk = {
    .type = "dpdk_flow_api",
    .flow_flush = netdev_offload_dpdk_flow_flush,
    .flow_put = netdev_offload_dpdk_flow_put,
    .flow_del = netdev_offload_dpdk_flow_del,
    .flow_stats_get = netdev_offload_dpdk_flow_stats_get,
    .init_flow_api = netdev_offload_dpdk_init_flow_api,
};
