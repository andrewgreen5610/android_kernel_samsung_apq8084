/* Copyright (c) 2011-2014, The Linux Foundation. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 and
 * only version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#define pr_fmt(fmt) "AXI: %s(): " fmt, __func__

#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/mutex.h>
#include <linux/radix-tree.h>
#include <linux/clk.h>
#include <linux/msm-bus.h>
#include "msm_bus_core.h"
#include <mach/trace_msm_bus.h>

#define INDEX_MASK 0x0000FFFF
#define PNODE_MASK 0xFFFF0000
#define SHIFT_VAL 16
#define CREATE_PNODE_ID(n, i) (((n) << SHIFT_VAL) | (i))
#define GET_INDEX(n) ((n) & INDEX_MASK)
#define GET_NODE(n) ((n) >> SHIFT_VAL)
#define IS_NODE(n) ((n) % FABRIC_ID_KEY)
#define SEL_FAB_CLK 1
#define SEL_SLAVE_CLK 0
/*
 * To get to BIMC BW convert Hz to bytes by multiplying bus width(8),
 * double-data-rate(2) * ddr-channels(2).
 */
#define GET_BIMC_BW(clk)	(clk * 8 * 2 * 2)

#define BW_TO_CLK_FREQ_HZ(width, bw) \
	msm_bus_div64(width, bw)

#define IS_MASTER_VALID(mas) \
	(((mas >= MSM_BUS_MASTER_FIRST) && (mas <= MSM_BUS_MASTER_LAST)) \
	 ? 1 : 0)

#define IS_SLAVE_VALID(slv) \
	(((slv >= MSM_BUS_SLAVE_FIRST) && (slv <= MSM_BUS_SLAVE_LAST)) ? 1 : 0)

static DEFINE_MUTEX(msm_bus_lock);

/* This function uses shift operations to divide 64 bit value for higher
 * efficiency. The divisor expected are number of ports or bus-width.
 * These are expected to be 1, 2, 4, 8, 16 and 32 in most cases.
 *
 * To account for exception to the above divisor values, the standard
 * do_div function is used.
 * */
uint64_t msm_bus_div64(unsigned int w, uint64_t bw)
{
	uint64_t *b = &bw;

	if ((bw > 0) && (bw < w))
		return 1;

	switch (w) {
	case 0:
		WARN(1, "AXI: Divide by 0 attempted\n");
	case 1: return bw;
	case 2:	return (bw >> 1);
	case 4:	return (bw >> 2);
	case 8:	return (bw >> 3);
	case 16: return (bw >> 4);
	case 32: return (bw >> 5);
	}

	do_div(*b, w);
	return *b;
}

/**
 * add_path_node: Adds the path information to the current node
 * @info: Internal node info structure
 * @next: Combination of the id and index of the next node
 * Function returns: Number of pnodes (path_nodes) on success,
 * error on failure.
 *
 * Every node maintains the list of path nodes. A path node is
 * reached by finding the node-id and index stored at the current
 * node. This makes updating the paths with requested bw and clock
 * values efficient, as it avoids lookup for each update-path request.
 */
static int add_path_node(struct msm_bus_inode_info *info, int next)
{
	struct path_node *pnode;
	int i;
	if (ZERO_OR_NULL_PTR(info)) {
		MSM_BUS_ERR("Cannot find node info!: id :%d\n",
				info->node_info->priv_id);
		return -ENXIO;
	}

	for (i = 0; i <= info->num_pnodes; i++) {
		if (info->pnode[i].next == -2) {
			MSM_BUS_DBG("Reusing pnode for info: %d at index: %d\n",
				info->node_info->priv_id, i);
			info->pnode[i].clk[DUAL_CTX] = 0;
			info->pnode[i].clk[ACTIVE_CTX] = 0;
			info->pnode[i].bw[DUAL_CTX] = 0;
			info->pnode[i].bw[ACTIVE_CTX] = 0;
			info->pnode[i].next = next;
			MSM_BUS_DBG("%d[%d] : (%d, %d)\n",
				info->node_info->priv_id, i, GET_NODE(next),
				GET_INDEX(next));
			return i;
		}
	}

	info->num_pnodes++;
	pnode = krealloc(info->pnode,
		((info->num_pnodes + 1) * sizeof(struct path_node))
		, GFP_KERNEL);
	if (ZERO_OR_NULL_PTR(pnode)) {
		MSM_BUS_ERR("Error creating path node!\n");
		info->num_pnodes--;
		return -ENOMEM;
	}
	info->pnode = pnode;
	info->pnode[info->num_pnodes].clk[DUAL_CTX] = 0;
	info->pnode[info->num_pnodes].clk[ACTIVE_CTX] = 0;
	info->pnode[info->num_pnodes].bw[DUAL_CTX] = 0;
	info->pnode[info->num_pnodes].bw[ACTIVE_CTX] = 0;
	info->pnode[info->num_pnodes].next = next;
	MSM_BUS_DBG("%d[%d] : (%d, %d)\n", info->node_info->priv_id,
		info->num_pnodes, GET_NODE(next), GET_INDEX(next));
	return info->num_pnodes;
}

static int clearvisitedflag(struct device *dev, void *data)
{
	struct msm_bus_fabric_device *fabdev = to_msm_bus_fabric_device(dev);
	fabdev->visited = false;
	return 0;
}

/**
 * getpath() - Finds the path from the topology between src and dest
 * @src: Source. This is the master from which the request originates
 * @dest: Destination. This is the slave to which we're trying to reach
 *
 * Function returns: next_pnode_id. The higher 16 bits of the next_pnode_id
 * represent the src id of the  next node on path. The lower 16 bits of the
 * next_pnode_id represent the "index", which is the next entry in the array
 * of pnodes for that node to fill in clk and bw values. This is created using
 * CREATE_PNODE_ID. The return value is stored in ret_pnode, and this is added
 * to the list of path nodes.
 *
 * This function recursively finds the path by updating the src to the
 * closest possible node to dest.
 */
static int getpath(int src, int dest)
{
	int pnode_num = -1, i;
	struct msm_bus_fabnodeinfo *fabnodeinfo;
	struct msm_bus_fabric_device *fabdev;
	int next_pnode_id = -1;
	struct msm_bus_inode_info *info = NULL;
	int _src = src/FABRIC_ID_KEY;
	int _dst = dest/FABRIC_ID_KEY;
	int ret_pnode = -1;
	int fabid = GET_FABID(src);

	/* Find the location of fabric for the src */
	MSM_BUS_DBG("%d --> %d\n", src, dest);

	fabdev = msm_bus_get_fabric_device(fabid);
	if (!fabdev) {
		MSM_BUS_WARN("Fabric Not yet registered. Try again\n");
		return -ENXIO;
	}

	/* Are we there yet? */
	if (src == dest) {
		info = fabdev->algo->find_node(fabdev, src);
		if (ZERO_OR_NULL_PTR(info)) {
			MSM_BUS_ERR("Node %d not found\n", dest);
			return -ENXIO;
		}

		for (i = 0; i <= info->num_pnodes; i++) {
			if (info->pnode[i].next == -2) {
				MSM_BUS_DBG("src = dst  Reusing pnode for"
				" info: %d at index: %d\n",
				info->node_info->priv_id, i);
				next_pnode_id = CREATE_PNODE_ID(src, i);
				info->pnode[i].clk[DUAL_CTX] = 0;
				info->pnode[i].bw[DUAL_CTX] = 0;
				info->pnode[i].next = next_pnode_id;
				MSM_BUS_DBG("returning: %d, %d\n", GET_NODE
				(next_pnode_id), GET_INDEX(next_pnode_id));
				return next_pnode_id;
			}
		}
		next_pnode_id = CREATE_PNODE_ID(src, (info->num_pnodes + 1));
		pnode_num = add_path_node(info, next_pnode_id);
		if (pnode_num < 0) {
			MSM_BUS_ERR("Error adding path node\n");
			return -ENXIO;
		}
		MSM_BUS_DBG("returning: %d, %d\n", GET_NODE(next_pnode_id),
			GET_INDEX(next_pnode_id));
		return next_pnode_id;
	} else if (_src == _dst) {
		/*
		 * src and dest belong to same fabric, find the destination
		 * from the radix tree
		 */
		info = fabdev->algo->find_node(fabdev, dest);
		if (ZERO_OR_NULL_PTR(info)) {
			MSM_BUS_ERR("Node %d not found\n", dest);
			return -ENXIO;
		}

		ret_pnode = getpath(info->node_info->priv_id, dest);
		next_pnode_id = ret_pnode;
	} else {
		/* find the dest fabric */
		int trynextgw = true;
		struct list_head *gateways = fabdev->algo->get_gw_list(fabdev);
		list_for_each_entry(fabnodeinfo, gateways, list) {
		/* see if the destination is at a connected fabric */
			if (_dst == (fabnodeinfo->info->node_info->priv_id /
				FABRIC_ID_KEY)) {
				/* Found the fab on which the device exists */
				info = fabnodeinfo->info;
				trynextgw = false;
				ret_pnode = getpath(info->node_info->priv_id,
					dest);
				pnode_num = add_path_node(info, ret_pnode);
				if (pnode_num < 0) {
					MSM_BUS_ERR("Error adding path node\n");
					return -ENXIO;
				}
				next_pnode_id = CREATE_PNODE_ID(
					info->node_info->priv_id, pnode_num);
				break;
			}
		}

		/* find the gateway */
		if (trynextgw) {
			gateways = fabdev->algo->get_gw_list(fabdev);
			list_for_each_entry(fabnodeinfo, gateways, list) {
				struct msm_bus_fabric_device *gwfab =
					msm_bus_get_fabric_device(fabnodeinfo->
						info->node_info->priv_id);
				if (!gwfab) {
					MSM_BUS_ERR("Err: No gateway found\n");
					return -ENXIO;
				}

				if (!gwfab->visited) {
					MSM_BUS_DBG("VISITED ID: %d\n",
						gwfab->id);
					gwfab->visited = true;
					info = fabnodeinfo->info;
					ret_pnode = getpath(info->
						node_info->priv_id, dest);
					pnode_num = add_path_node(info,
						ret_pnode);
					if (pnode_num < 0) {
						MSM_BUS_ERR("Malloc failure in"
						" adding path node\n");
						return -ENXIO;
					}
					next_pnode_id = CREATE_PNODE_ID(
					info->node_info->priv_id, pnode_num);
					break;
				}
			}
			if (next_pnode_id < 0)
				return -ENXIO;
		}
	}

	if (!IS_NODE(src)) {
		MSM_BUS_DBG("Returning next_pnode_id:%d[%d]\n", GET_NODE(
			next_pnode_id), GET_INDEX(next_pnode_id));
		return next_pnode_id;
	}
	info = fabdev->algo->find_node(fabdev, src);
	if (!info) {
		MSM_BUS_ERR("Node info not found.\n");
		return -ENXIO;
	}

	pnode_num = add_path_node(info, next_pnode_id);
	MSM_BUS_DBG(" Last: %d[%d] = (%d, %d)\n",
		src, info->num_pnodes, GET_NODE(next_pnode_id),
		GET_INDEX(next_pnode_id));
	MSM_BUS_DBG("returning: %d, %d\n", src, pnode_num);
	return CREATE_PNODE_ID(src, pnode_num);
}

static uint64_t get_node_maxib(struct msm_bus_inode_info *info)
{
	int i, ctx;
	uint64_t maxib = 0;

	for (i = 0; i <= info->num_pnodes; i++) {
		for (ctx = 0; ctx < NUM_CTX; ctx++)
			maxib = max(info->pnode[i].clk[ctx], maxib);
	}

	MSM_BUS_DBG("%s: Node %d numpnodes %d maxib %llu", __func__,
		info->num_pnodes, info->node_info->id, maxib);
	return maxib;
}


static uint64_t get_node_sumab(struct msm_bus_inode_info *info)
{
	int i;
	uint64_t maxab = 0;

	for (i = 0; i <= info->num_pnodes; i++)
		maxab += info->pnode[i].bw[DUAL_CTX];

	MSM_BUS_DBG("%s: Node %d numpnodes %d maxib %llu", __func__,
		info->num_pnodes, info->node_info->id, maxab);
	return maxab;
}

static uint64_t get_vfe_bw(void)
{
	int vfe_id = MSM_BUS_MASTER_VFE;
	int iid = msm_bus_board_get_iid(vfe_id);
	int fabid;
	struct msm_bus_fabric_device *fabdev;
	struct msm_bus_inode_info *info;
	uint64_t vfe_bw = 0;

	fabid = GET_FABID(iid);
	fabdev = msm_bus_get_fabric_device(fabid);
	info = fabdev->algo->find_node(fabdev, iid);

	if (!info) {
		MSM_BUS_ERR("%s: Can't find node %d", __func__,
						vfe_id);
		goto exit_get_vfe_bw;
	}

	vfe_bw = get_node_sumab(info);
	MSM_BUS_DBG("vfe_ab %llu", vfe_bw);

exit_get_vfe_bw:
	return vfe_bw;
}

static uint64_t get_mdp_bw(void)
{
	int ids[] = {MSM_BUS_MASTER_MDP_PORT0, MSM_BUS_MASTER_MDP_PORT1};
	int i;
	uint64_t mdp_ab = 0;
	uint32_t ff = 0;

	for (i = 0; i < ARRAY_SIZE(ids); i++) {
		int iid = msm_bus_board_get_iid(ids[i]);
		int fabid;
		struct msm_bus_fabric_device *fabdev;
		struct msm_bus_inode_info *info;

		fabid = GET_FABID(iid);
		fabdev = msm_bus_get_fabric_device(fabid);
		info = fabdev->algo->find_node(fabdev, iid);

		if (!info) {
			MSM_BUS_ERR("%s: Can't find node %d", __func__,
								ids[i]);
			continue;
		}

		mdp_ab += get_node_sumab(info);
		MSM_BUS_DBG("mdp_ab %llu", mdp_ab);
		ff = info->node_info->ff;
	}

	if (ff)
		mdp_ab = msm_bus_div64(2 * ff, 100 * mdp_ab);
	else {
		MSM_BUS_ERR("MDP FF is 0");
		mdp_ab = 0;
	}


	MSM_BUS_DBG("MDP BW %llu\n", mdp_ab);
	return mdp_ab;
}

static uint64_t get_rt_bw(void)
{
	uint64_t rt_bw = 0;

	rt_bw += get_mdp_bw();
	rt_bw += get_vfe_bw();

	return rt_bw;
}

static uint64_t get_avail_bw(struct msm_bus_fabric_device *fabdev)
{
	uint64_t fabclk_rate = 0;
	int i;
	uint64_t avail_bw = 0;
	uint64_t rt_bw = get_rt_bw();
	struct msm_bus_fabric *fabric = to_msm_bus_fabric(fabdev);

	if (!rt_bw)
		goto exit_get_avail_bw;

	for (i = 0; i < NUM_CTX; i++) {
		uint64_t ctx_rate;
		ctx_rate =
			fabric->info.nodeclk[i].rate;
		fabclk_rate = max(ctx_rate, fabclk_rate);
	}

	if (!fabdev->eff_fact || !fabdev->nr_lim_thresh) {
		MSM_BUS_ERR("Error: Eff-fact %d; nr_thresh %llu",
				fabdev->eff_fact, fabdev->nr_lim_thresh);
		return 0;
	}

	avail_bw = msm_bus_div64(100,
				(GET_BIMC_BW(fabclk_rate) * fabdev->eff_fact));

	if (avail_bw >= fabdev->nr_lim_thresh)
		return 0;

	MSM_BUS_DBG("%s: Total_avail_bw %llu, rt_bw %llu\n",
		__func__, avail_bw, rt_bw);
	trace_bus_avail_bw(avail_bw, rt_bw);

	if (avail_bw < rt_bw) {
		MSM_BUS_ERR("\n%s: ERROR avail BW %llu < MDP %llu",
			__func__, avail_bw, rt_bw);
		avail_bw = 0;
		goto exit_get_avail_bw;
	}
	avail_bw -= rt_bw;

exit_get_avail_bw:
	return avail_bw;
}

static void program_nr_limits(struct msm_bus_fabric_device *fabdev)
{
	int num_nr_lim = 0;
	int i;
	struct msm_bus_inode_info *info[fabdev->num_nr_lim];
	struct msm_bus_fabric *fabric = to_msm_bus_fabric(fabdev);

	num_nr_lim = radix_tree_gang_lookup_tag(&fabric->fab_tree,
			(void **)&info, fabric->fabdev.id, fabdev->num_nr_lim,
			MASTER_NODE);

	for (i = 0; i < num_nr_lim; i++)
		fabdev->algo->config_limiter(fabdev, info[i]);
}

static int msm_bus_commit_limiter(struct device *dev, void *data)
{
	int ret = 0;
	struct msm_bus_fabric_device *fabdev = to_msm_bus_fabric_device(dev);
	MSM_BUS_DBG("fabid: %d\n", fabdev->id);
	program_nr_limits(fabdev);
	return ret;
}

static void compute_nr_limits(struct msm_bus_fabric_device *fabdev, int pnode)
{
	uint64_t total_ib = 0;
	int num_nr_lim = 0;
	uint64_t avail_bw = 0;
	struct msm_bus_inode_info *info[fabdev->num_nr_lim];
	struct msm_bus_fabric *fabric = to_msm_bus_fabric(fabdev);
	int i;

	num_nr_lim = radix_tree_gang_lookup_tag(&fabric->fab_tree,
			(void **)&info, fabric->fabdev.id, fabdev->num_nr_lim,
			MASTER_NODE);

	MSM_BUS_DBG("%s: Found %d NR LIM nodes", __func__, num_nr_lim);
	for (i = 0; i < num_nr_lim; i++)
		total_ib += get_node_maxib(info[i]);

	avail_bw = get_avail_bw(fabdev);
	MSM_BUS_DBG("\n %s: Avail BW %llu", __func__, avail_bw);

	for (i = 0; i < num_nr_lim; i++) {
		uint32_t node_pct = 0;
		uint64_t new_lim_bw = 0;
		uint64_t node_max_ib = 0;
		uint32_t node_max_ib_kB = 0;
		uint32_t total_ib_kB = 0;
		uint64_t bw_node;

		node_max_ib = get_node_maxib(info[i]);
		node_max_ib_kB = msm_bus_div64(1024, node_max_ib);
		total_ib_kB = msm_bus_div64(1024, total_ib);
		node_pct = (node_max_ib_kB * 100) / total_ib_kB;
		bw_node = node_pct * avail_bw;
		new_lim_bw = msm_bus_div64(100, bw_node);

		/*
		 * if limiter bw is more than the requested IB clip to
		 * requested IB.
		 */
		if (new_lim_bw >= node_max_ib)
			new_lim_bw = node_max_ib;

		/*
		 * if there is a floor bw for this nr lim node and
		 *  if there is available bw to divy up among the nr masters
		 *  and if the nr lim masters have a non zero vote and
		 *  if the limited bw is below the floor for this node.
		 *    then limit this node to the floor bw.
		 */
		if (info[i]->node_info->floor_bw && node_max_ib && avail_bw &&
			(new_lim_bw <= info[i]->node_info->floor_bw)) {
			MSM_BUS_ERR("\n ERROR limiting BW %llu < floor %llu",
				new_lim_bw, info[i]->node_info->floor_bw);
			new_lim_bw = info[i]->node_info->floor_bw;
		}

		if (new_lim_bw != info[i]->cur_lim_bw) {
			info[i]->cur_lim_bw = new_lim_bw;
			MSM_BUS_DBG("NodeId %d: Requested IB %llu",
					info[i]->node_info->id, node_max_ib);
			MSM_BUS_DBG("Limited to %llu(%d pct of Avail %llu )\n",
					new_lim_bw, node_pct, avail_bw);
		} else
			MSM_BUS_DBG("NodeId %d: No change Limited to %llu\n",
				info[i]->node_info->id, info[i]->cur_lim_bw);
	}
}

static void setup_nr_limits(int curr, int pnode)
{
	struct msm_bus_fabric_device *fabdev =
		msm_bus_get_fabric_device(GET_FABID(curr));
	struct msm_bus_inode_info *info;

	if (!fabdev) {
		MSM_BUS_WARN("Fabric Not yet registered. Try again\n");
		goto exit_setup_nr_limits;
	}

	/* This logic is currently applicable to BIMC masters only */
	if (fabdev->id != MSM_BUS_FAB_DEFAULT) {
		MSM_BUS_ERR("Static limiting of NR masters only for BIMC");
		goto exit_setup_nr_limits;
	}

	info = fabdev->algo->find_node(fabdev, curr);
	if (!info) {
		MSM_BUS_ERR("Cannot find node info!\n");
		goto exit_setup_nr_limits;
	}

	compute_nr_limits(fabdev, pnode);
exit_setup_nr_limits:
	return;
}

static bool is_nr_lim(int id)
{
	struct msm_bus_fabric_device *fabdev = msm_bus_get_fabric_device
		(GET_FABID(id));
	struct msm_bus_inode_info *info;
	bool ret = false;

	if (!fabdev) {
		MSM_BUS_ERR("Bus device for bus ID: %d not found!\n",
			GET_FABID(id));
		goto exit_is_nr_lim;
	}

	info = fabdev->algo->find_node(fabdev, id);
	if (!info)
		MSM_BUS_ERR("Cannot find node info %d!\n", id);
	else if ((info->node_info->nr_lim || info->node_info->rt_mas))
		ret = true;
exit_is_nr_lim:
	return ret;
}

/**
 * update_path() - Update the path with the bandwidth and clock values, as
 * requested by the client.
 *
 * @curr: Current source node, as specified in the client vector (master)
 * @pnode: The first-hop node on the path, stored in the internal client struct
 * @req_clk: Requested clock value from the vector
 * @req_bw: Requested bandwidth value from the vector
 * @curr_clk: Current clock frequency
 * @curr_bw: Currently allocated bandwidth
 *
 * This function updates the nodes on the path calculated using getpath(), with
 * clock and bandwidth values. The sum of bandwidths, and the max of clock
 * frequencies is calculated at each node on the path. Commit data to be sent
 * to RPM for each master and slave is also calculated here.
 */
static int update_path(int curr, int pnode, uint64_t req_clk, uint64_t req_bw,
	uint64_t curr_clk, uint64_t curr_bw, unsigned int ctx, unsigned int
	cl_active_flag)
{
	int index, ret = 0;
	struct msm_bus_inode_info *info;
	struct msm_bus_inode_info *src_info;
	int next_pnode;
	int64_t add_bw = req_bw - curr_bw;
	uint64_t bwsum = 0;
	uint64_t req_clk_hz, curr_clk_hz, bwsum_hz;
	int *master_tiers;
	struct msm_bus_fabric_device *fabdev = msm_bus_get_fabric_device
		(GET_FABID(curr));

	if (!fabdev) {
		MSM_BUS_ERR("Bus device for bus ID: %d not found!\n",
			GET_FABID(curr));
		return -ENXIO;
	}

	MSM_BUS_DBG("args: %d %d %d %llu %llu %llu %llu %u\n",
		curr, GET_NODE(pnode), GET_INDEX(pnode), req_clk, req_bw,
		curr_clk, curr_bw, ctx);
	index = GET_INDEX(pnode);
	MSM_BUS_DBG("Client passed index :%d\n", index);
	info = fabdev->algo->find_node(fabdev, curr);
	if (!info) {
		MSM_BUS_ERR("Cannot find node info!\n");
		return -ENXIO;
	}
	src_info = info;

	info->link_info.sel_bw = &info->link_info.bw[ctx];
	info->link_info.sel_clk = &info->link_info.clk[ctx];
	*info->link_info.sel_bw += add_bw;

	info->pnode[index].sel_bw = &info->pnode[index].bw[ctx];

	/**
	 * To select the right clock, AND the context with
	 * client active flag.
	 */
	info->pnode[index].sel_clk = &info->pnode[index].clk[ctx &
		cl_active_flag];
	*info->pnode[index].sel_bw += add_bw;
	*info->pnode[index].sel_clk = req_clk;

	/**
	 * If master supports dual configuration, check if
	 * the configuration needs to be changed based on
	 * incoming requests
	 */
	if (info->node_info->dual_conf) {
		uint64_t node_maxib = 0;
		node_maxib = get_node_maxib(info);
		fabdev->algo->config_master(fabdev, info,
			node_maxib, req_bw);
	}

	info->link_info.num_tiers = info->node_info->num_tiers;
	info->link_info.tier = info->node_info->tier;
	master_tiers = info->node_info->tier;

	do {
		struct msm_bus_inode_info *hop;
		fabdev = msm_bus_get_fabric_device(GET_FABID(curr));
		if (!fabdev) {
			MSM_BUS_ERR("Fabric not found\n");
			return -ENXIO;
		}
		MSM_BUS_DBG("id: %d\n", info->node_info->priv_id);

		/* find next node and index */
		next_pnode = info->pnode[index].next;
		curr = GET_NODE(next_pnode);
		index = GET_INDEX(next_pnode);
		MSM_BUS_DBG("id:%d, next: %d\n", info->
		    node_info->priv_id, curr);

		/* Get hop */
		/* check if we are here as gateway, or does the hop belong to
		 * this fabric */
		if (IS_NODE(curr))
			hop = fabdev->algo->find_node(fabdev, curr);
		else
			hop = fabdev->algo->find_gw_node(fabdev, curr);
		if (!hop) {
			MSM_BUS_ERR("Null Info found for hop\n");
			return -ENXIO;
		}

		hop->link_info.sel_bw = &hop->link_info.bw[ctx];
		hop->link_info.sel_clk = &hop->link_info.clk[ctx];
		*hop->link_info.sel_bw += add_bw;

		hop->pnode[index].sel_bw = &hop->pnode[index].bw[ctx];
		hop->pnode[index].sel_clk = &hop->pnode[index].clk[ctx &
			cl_active_flag];

		if (!hop->node_info->buswidth) {
			MSM_BUS_WARN("No bus width found. Using default\n");
			hop->node_info->buswidth = 8;
		}
		*hop->pnode[index].sel_clk = BW_TO_CLK_FREQ_HZ(hop->node_info->
			buswidth, req_clk);
		*hop->pnode[index].sel_bw += add_bw;
		MSM_BUS_DBG("fabric: %d slave: %d, slave-width: %d info: %d\n",
			fabdev->id, hop->node_info->priv_id, hop->node_info->
			buswidth, info->node_info->priv_id);
		/* Update Bandwidth */
		fabdev->algo->update_bw(fabdev, hop, info, add_bw,
			master_tiers, ctx);
		bwsum = *hop->link_info.sel_bw;
		/* Update Fabric clocks */
		curr_clk_hz = BW_TO_CLK_FREQ_HZ(hop->node_info->buswidth,
			curr_clk);
		req_clk_hz = BW_TO_CLK_FREQ_HZ(hop->node_info->buswidth,
			req_clk);
		bwsum_hz = BW_TO_CLK_FREQ_HZ(hop->node_info->buswidth,
			bwsum);
		/* Account for multiple channels if any */
		if (hop->node_info->num_sports > 1)
			bwsum_hz = msm_bus_div64(hop->node_info->num_sports,
				bwsum_hz);
		MSM_BUS_DBG("AXI: Hop: %d, ports: %d, bwsum_hz: %llu\n",
				hop->node_info->id, hop->node_info->num_sports,
				bwsum_hz);
		MSM_BUS_DBG("up-clk: curr_hz: %llu, req_hz: %llu, bw_hz %llu\n",
			curr_clk, req_clk, bwsum_hz);
		ret = fabdev->algo->update_clks(fabdev, hop, index,
			curr_clk_hz, req_clk_hz, bwsum_hz, SEL_FAB_CLK,
			ctx, cl_active_flag);
		if (ret)
			MSM_BUS_WARN("Failed to update clk\n");
		info = hop;
	} while (GET_NODE(info->pnode[index].next) != info->node_info->priv_id);

	/* Update BW, clk after exiting the loop for the last one */
	if (!info) {
		MSM_BUS_ERR("Cannot find node info!\n");
		return -ENXIO;
	}
	/* Update slave clocks */
	ret = fabdev->algo->update_clks(fabdev, info, index, curr_clk_hz,
	    req_clk_hz, bwsum_hz, SEL_SLAVE_CLK, ctx, cl_active_flag);
	if (ret)
		MSM_BUS_ERR("Failed to update clk\n");

	if ((ctx == cl_active_flag) &&
		((src_info->node_info->nr_lim || src_info->node_info->rt_mas)))
		setup_nr_limits(curr, pnode);

	/* If freq is going down , apply the changes now before
	 * we commit clk data.
	 */
	if ((req_clk < curr_clk) || (req_bw < curr_bw))
		bus_for_each_dev(&msm_bus_type, NULL, NULL,
					msm_bus_commit_limiter);
	return ret;
}

/**
 * msm_bus_commit_fn() - Commits the data for fabric to rpm
 * @dev: fabric device
 * @data: NULL
 */
static int msm_bus_commit_fn(struct device *dev, void *data)
{
	int ret = 0;
	struct msm_bus_fabric_device *fabdev = to_msm_bus_fabric_device(dev);
	MSM_BUS_DBG("Committing: fabid: %d\n", fabdev->id);
	ret = fabdev->algo->commit(fabdev);
	return ret;
}

static uint32_t register_client_legacy(struct msm_bus_scale_pdata *pdata)
{
	struct msm_bus_client *client = NULL;
	int i;
	int src, dest, nfab;
	struct msm_bus_fabric_device *deffab;

	deffab = msm_bus_get_fabric_device(MSM_BUS_FAB_DEFAULT);
	if (!deffab) {
		MSM_BUS_ERR("Error finding default fabric\n");
		return 0;
	}

	nfab = msm_bus_get_num_fab();
	if (nfab < deffab->board_algo->board_nfab) {
		MSM_BUS_ERR("Can't register client!\n"
				"Num of fabrics up: %d\n",
				nfab);
		return 0;
	}

	if ((!pdata) || (pdata->usecase->num_paths == 0) || IS_ERR(pdata)) {
		MSM_BUS_ERR("Cannot register client with null data\n");
		return 0;
	}

	client = kzalloc(sizeof(struct msm_bus_client), GFP_KERNEL);
	if (!client) {
		MSM_BUS_ERR("Error allocating client\n");
		return 0;
	}

	mutex_lock(&msm_bus_lock);
	client->pdata = pdata;
	client->curr = -1;
	for (i = 0; i < pdata->usecase->num_paths; i++) {
		int *pnode;
		struct msm_bus_fabric_device *srcfab;
		pnode = krealloc(client->src_pnode, ((i + 1) * sizeof(int)),
			GFP_KERNEL);
		if (ZERO_OR_NULL_PTR(pnode)) {
			MSM_BUS_ERR("Invalid Pnode ptr!\n");
			continue;
		} else
			client->src_pnode = pnode;

		if (!IS_MASTER_VALID(pdata->usecase->vectors[i].src)) {
			MSM_BUS_ERR("Invalid Master ID %d in request!\n",
				pdata->usecase->vectors[i].src);
			goto err;
		}

		if (!IS_SLAVE_VALID(pdata->usecase->vectors[i].dst)) {
			MSM_BUS_ERR("Invalid Slave ID %d in request!\n",
				pdata->usecase->vectors[i].dst);
			goto err;
		}

		src = msm_bus_board_get_iid(pdata->usecase->vectors[i].src);
		if (src == -ENXIO) {
			MSM_BUS_ERR("Master %d not supported. Client cannot be"
				" registered\n",
				pdata->usecase->vectors[i].src);
			goto err;
		}
		dest = msm_bus_board_get_iid(pdata->usecase->vectors[i].dst);
		if (dest == -ENXIO) {
			MSM_BUS_ERR("Slave %d not supported. Client cannot be"
				" registered\n",
				pdata->usecase->vectors[i].dst);
			goto err;
		}
		srcfab = msm_bus_get_fabric_device(GET_FABID(src));
		if (!srcfab) {
			MSM_BUS_ERR("Fabric not found\n");
			goto err;
		}

		srcfab->visited = true;
		pnode[i] = getpath(src, dest);
		bus_for_each_dev(&msm_bus_type, NULL, NULL, clearvisitedflag);
		if (pnode[i] == -ENXIO) {
			MSM_BUS_ERR("Cannot register client now! Try again!\n");
			goto err;
		}
	}
	msm_bus_dbg_client_data(client->pdata, MSM_BUS_DBG_REGISTER,
		(uint32_t)client);
	mutex_unlock(&msm_bus_lock);
	MSM_BUS_DBG("ret: %u num_paths: %d\n", (uint32_t)client,
		pdata->usecase->num_paths);
	return (uint32_t)(client);
err:
	kfree(client->src_pnode);
	kfree(client);
	mutex_unlock(&msm_bus_lock);
	return 0;
}

static int update_request_legacy(uint32_t cl, unsigned index)
{
	int i, ret = 0;
	struct msm_bus_scale_pdata *pdata;
	int pnode, src, curr, ctx;
	uint64_t req_clk, req_bw, curr_clk, curr_bw;
	struct msm_bus_client *client = (struct msm_bus_client *)cl;
	if (IS_ERR_OR_NULL(client)) {
		MSM_BUS_ERR("msm_bus_scale_client update req error %d\n",
				(uint32_t)client);
		return -ENXIO;
	}

	mutex_lock(&msm_bus_lock);
	if (client->curr == index)
		goto err;

	curr = client->curr;
	pdata = client->pdata;
	if (!pdata) {
		MSM_BUS_ERR("Null pdata passed to update-request\n");
		ret = -ENXIO;
		goto err;
	}

	if (index >= pdata->num_usecases) {
		MSM_BUS_ERR("Client %u passed invalid index: %d\n",
			(uint32_t)client, index);
		ret = -ENXIO;
		goto err;
	}

	MSM_BUS_DBG("cl: %u index: %d curr: %d num_paths: %d\n",
		cl, index, client->curr, client->pdata->usecase->num_paths);

	for (i = 0; i < pdata->usecase->num_paths; i++) {
		src = msm_bus_board_get_iid(client->pdata->usecase[index].
			vectors[i].src);
		if (src == -ENXIO) {
			MSM_BUS_ERR("Master %d not supported. Request cannot"
				" be updated\n", client->pdata->usecase->
				vectors[i].src);
			goto err;
		}

		if (msm_bus_board_get_iid(client->pdata->usecase[index].
			vectors[i].dst) == -ENXIO) {
			MSM_BUS_ERR("Slave %d not supported. Request cannot"
				" be updated\n", client->pdata->usecase->
				vectors[i].dst);
		}

		pnode = client->src_pnode[i];
		req_clk = client->pdata->usecase[index].vectors[i].ib;
		req_bw = client->pdata->usecase[index].vectors[i].ab;
		if (curr < 0) {
			curr_clk = 0;
			curr_bw = 0;
		} else {
			curr_clk = client->pdata->usecase[curr].vectors[i].ib;
			curr_bw = client->pdata->usecase[curr].vectors[i].ab;
			MSM_BUS_DBG("ab: %llu ib: %llu\n", curr_bw, curr_clk);
		}

		if (!pdata->active_only) {
			ret = update_path(src, pnode, req_clk, req_bw,
				curr_clk, curr_bw, 0, pdata->active_only);
			if (ret) {
				MSM_BUS_ERR("Update path failed! %d\n", ret);
				goto err;
			}
		}

		ret = update_path(src, pnode, req_clk, req_bw, curr_clk,
				curr_bw, ACTIVE_CTX, pdata->active_only);
		if (ret) {
			MSM_BUS_ERR("Update Path failed! %d\n", ret);
			goto err;
		}
	}

	client->curr = index;
	ctx = ACTIVE_CTX;
	msm_bus_dbg_client_data(client->pdata, index, cl);
	bus_for_each_dev(&msm_bus_type, NULL, NULL, msm_bus_commit_fn);

	/* For NR/RT limited masters, if freq is going up , apply the changes
	 * after we commit clk data.
	 */
	if (is_nr_lim(src) && ((req_clk > curr_clk) || (req_bw > curr_bw)))
		bus_for_each_dev(&msm_bus_type, NULL, NULL,
					msm_bus_commit_limiter);

err:
	mutex_unlock(&msm_bus_lock);
	return ret;
}

static int reset_pnodes(int curr, int pnode)
{
	struct msm_bus_inode_info *info;
	struct msm_bus_fabric_device *fabdev;
	int index, next_pnode;
	fabdev = msm_bus_get_fabric_device(GET_FABID(curr));
	if (!fabdev) {
		MSM_BUS_ERR("Fabric not found for: %d\n",
			(GET_FABID(curr)));
			return -ENXIO;
	}

	index = GET_INDEX(pnode);
	info = fabdev->algo->find_node(fabdev, curr);
	if (!info) {
		MSM_BUS_ERR("Cannot find node info!\n");
		return -ENXIO;
	}

	MSM_BUS_DBG("Starting the loop--remove\n");
	do {
		struct msm_bus_inode_info *hop;
		fabdev = msm_bus_get_fabric_device(GET_FABID(curr));
		if (!fabdev) {
			MSM_BUS_ERR("Fabric not found\n");
				return -ENXIO;
		}

		next_pnode = info->pnode[index].next;
		info->pnode[index].next = -2;
		curr = GET_NODE(next_pnode);
		index = GET_INDEX(next_pnode);
		if (IS_NODE(curr))
			hop = fabdev->algo->find_node(fabdev, curr);
		else
			hop = fabdev->algo->find_gw_node(fabdev, curr);
		if (!hop) {
			MSM_BUS_ERR("Null Info found for hop\n");
			return -ENXIO;
		}

		MSM_BUS_DBG("%d[%d] = %d\n", info->node_info->priv_id, index,
			info->pnode[index].next);
		MSM_BUS_DBG("num_pnodes: %d: %d\n", info->node_info->priv_id,
			info->num_pnodes);
		info = hop;
	} while (GET_NODE(info->pnode[index].next) != info->node_info->priv_id);

	info->pnode[index].next = -2;
	MSM_BUS_DBG("%d[%d] = %d\n", info->node_info->priv_id, index,
		info->pnode[index].next);
	MSM_BUS_DBG("num_pnodes: %d: %d\n", info->node_info->priv_id,
		info->num_pnodes);
	return 0;
}

int msm_bus_board_get_iid(int id)
{
	struct msm_bus_fabric_device *deffab;

	deffab = msm_bus_get_fabric_device(MSM_BUS_FAB_DEFAULT);
	if (!deffab) {
		MSM_BUS_ERR("Error finding default fabric\n");
		return -ENXIO;
	}

	return deffab->board_algo->get_iid(id);
}

void msm_bus_scale_client_reset_pnodes(uint32_t cl)
{
	int i, src, pnode, index;
	struct msm_bus_client *client = (struct msm_bus_client *)(cl);
	if (IS_ERR_OR_NULL(client)) {
		MSM_BUS_ERR("msm_bus_scale_reset_pnodes error\n");
		return;
	}
	index = 0;
	for (i = 0; i < client->pdata->usecase->num_paths; i++) {
		src = msm_bus_board_get_iid(
			client->pdata->usecase[index].vectors[i].src);
		pnode = client->src_pnode[i];
		MSM_BUS_DBG("(%d, %d)\n", GET_NODE(pnode), GET_INDEX(pnode));
		reset_pnodes(src, pnode);
	}
}

static void unregister_client_legacy(uint32_t cl)
{
	int i;
	struct msm_bus_client *client = (struct msm_bus_client *)(cl);
	bool warn = false;
	if (IS_ERR_OR_NULL(client))
		return;

	for (i = 0; i < client->pdata->usecase->num_paths; i++) {
		if ((client->pdata->usecase[0].vectors[i].ab) ||
			(client->pdata->usecase[0].vectors[i].ib)) {
			warn = true;
			break;
		}
	}

	if (warn) {
		int num_paths = client->pdata->usecase->num_paths;
		int ab[num_paths], ib[num_paths];
		WARN(1, "%s called unregister with non-zero vectors\n",
			client->pdata->name);

		/*
		 * Save client values and zero them out to
		 * cleanly unregister
		 */
		for (i = 0; i < num_paths; i++) {
			ab[i] = client->pdata->usecase[0].vectors[i].ab;
			ib[i] = client->pdata->usecase[0].vectors[i].ib;
			client->pdata->usecase[0].vectors[i].ab = 0;
			client->pdata->usecase[0].vectors[i].ib = 0;
		}

		msm_bus_scale_client_update_request(cl, 0);

		/* Restore client vectors if required for re-registering. */
		for (i = 0; i < num_paths; i++) {
			client->pdata->usecase[0].vectors[i].ab = ab[i];
			client->pdata->usecase[0].vectors[i].ib = ib[i];
		}
	} else if (client->curr != 0)
		msm_bus_scale_client_update_request(cl, 0);

	MSM_BUS_DBG("Unregistering client %d\n", cl);
	mutex_lock(&msm_bus_lock);
	msm_bus_scale_client_reset_pnodes(cl);
	msm_bus_dbg_client_data(client->pdata, MSM_BUS_DBG_UNREGISTER, cl);
	mutex_unlock(&msm_bus_lock);
	kfree(client->src_pnode);
	kfree(client);
}

void msm_bus_arb_setops_legacy(struct msm_bus_arb_ops *arb_ops)
{
	arb_ops->register_client = register_client_legacy;
	arb_ops->update_request = update_request_legacy;
	arb_ops->unregister_client = unregister_client_legacy;
}

