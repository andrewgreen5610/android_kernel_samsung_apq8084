/* Copyright (c) 2014, Linux Foundation. All rights reserved.
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

#include <linux/clk.h>
#include <linux/device.h>
#include <linux/init.h>
#include <linux/io.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <soc/qcom/rpm-smd.h>
#include "msm_bus_core.h"
#include "msm_bus_adhoc.h"
#include "msm_bus_noc.h"
#include "msm_bus_bimc.h"

static int enable_nodeclk(struct nodeclk *nclk)
{
	int ret = 0;

	if (!nclk->enable) {
		ret = clk_prepare_enable(nclk->clk);

		if (ret) {
			MSM_BUS_ERR("%s: failed to enable clk ", __func__);
			nclk->enable = false;
		} else
			nclk->enable = true;
	}
	return ret;
}

static int disable_nodeclk(struct nodeclk *nclk)
{
	int ret = 0;

	if (nclk->enable) {
		clk_disable_unprepare(nclk->clk);
		nclk->enable = false;
	}
	return ret;
}

static int setrate_nodeclk(struct nodeclk *nclk, long rate)
{
	int ret = 0;

	ret = clk_set_rate(nclk->clk, rate);

	if (ret)
		MSM_BUS_ERR("%s: failed to setrate clk", __func__);
	return ret;
}

static int send_rpm_msg(struct device *device)
{
	int ret = 0;
	int ctx;
	int rsc_type;
	struct msm_bus_node_device_type *ndev =
					device->platform_data;
	struct msm_rpm_kvp rpm_kvp;

	if (!ndev) {
		MSM_BUS_ERR("%s: Error getting node info.", __func__);
		ret = -ENODEV;
		goto exit_send_rpm_msg;
	}

	rpm_kvp.length = sizeof(uint64_t);
	rpm_kvp.key = RPM_MASTER_FIELD_BW;

	for (ctx = MSM_RPM_CTX_ACTIVE_SET; ctx <= MSM_RPM_CTX_SLEEP_SET;
					ctx++) {
		if (ctx == MSM_RPM_CTX_ACTIVE_SET)
			rpm_kvp.data =
			(uint8_t *)&ndev->node_ab.ab[MSM_RPM_CTX_ACTIVE_SET];
		else {
			rpm_kvp.data =
			(uint8_t *) &ndev->node_ab.ab[MSM_RPM_CTX_SLEEP_SET];
		}

		if (ndev->node_info->mas_rpm_id != -1) {
			rsc_type = RPM_BUS_MASTER_REQ;
			ret = msm_rpm_send_message(ctx, rsc_type,
				ndev->node_info->mas_rpm_id, &rpm_kvp, 1);
			if (ret) {
				MSM_BUS_ERR("%s: Failed to send RPM message:",
						__func__);
				MSM_BUS_ERR("%s:Node Id %d RPM id %d",
				__func__, ndev->node_info->id,
					 ndev->node_info->mas_rpm_id);
				goto exit_send_rpm_msg;
			}
		}

		if (ndev->node_info->slv_rpm_id != -1) {
			rsc_type = RPM_BUS_SLAVE_REQ;
			ret = msm_rpm_send_message(ctx, rsc_type,
				ndev->node_info->slv_rpm_id, &rpm_kvp, 1);
			if (ret) {
				MSM_BUS_ERR("%s: Failed to send RPM message:",
							__func__);
				MSM_BUS_ERR("%s: Node Id %d RPM id %d",
				__func__, ndev->node_info->id,
					ndev->node_info->slv_rpm_id);
				goto exit_send_rpm_msg;
			}
		}
	}
exit_send_rpm_msg:
	return ret;
}

static int flush_bw_data(struct device *node_device, int ctx)
{
	struct msm_bus_node_device_type *node_info;
	int ret = 0;

	node_info = node_device->platform_data;
	if (!node_info) {
		MSM_BUS_ERR("%s: Unable to find bus device for device %d",
			__func__, node_info->node_info->id);
		ret = -ENODEV;
		goto exit_flush_bw_data;
	}

	if (node_info->node_ab.dirty) {
		if (node_info->ap_owned) {
			struct msm_bus_node_device_type *bus_device =
				node_info->node_info->bus_device->platform_data;
			struct msm_bus_fab_device_type *fabdev =
							bus_device->fabdev;

			if (fabdev)
				ret = fabdev->noc_ops.set_bw(node_info,
							fabdev->qos_base,
							fabdev->base_offset,
							fabdev->qos_off,
							fabdev->qos_freq);
		} else {
			ret = send_rpm_msg(node_device);

			if (ret)
				MSM_BUS_ERR("%s: Failed to send RPM msg for%d",
				__func__, node_info->node_info->id);
		}
	}

exit_flush_bw_data:
	return ret;

}

static int flush_clk_data(struct device *node_device, int ctx)
{
	struct msm_bus_node_device_type *node_info;
	struct nodeclk *nodeclk = NULL;
	int ret = 0;

	node_info = node_device->platform_data;
	if (!node_info) {
		MSM_BUS_ERR("%s: Unable to find bus device for device %d",
			__func__, node_info->node_info->id);
		ret = -ENODEV;
		goto exit_flush_clk_data;
	}

	nodeclk = &node_info->clk[ctx];

	if (nodeclk && nodeclk->clk && nodeclk->dirty) {
		long rounded_rate;

		if (nodeclk->rate) {
			rounded_rate = clk_round_rate(nodeclk->clk,
							nodeclk->rate);
			ret = setrate_nodeclk(nodeclk, rounded_rate);

			if (!ret) {
				MSM_BUS_ERR("%s: Failed to set_rate for %d",
					__func__, node_info->node_info->id);
				ret = -ENODEV;
				goto exit_flush_clk_data;
			}

			ret = enable_nodeclk(nodeclk);
		} else
			ret = disable_nodeclk(nodeclk);

		if (!ret) {
			MSM_BUS_ERR("%s: Failed to enable for %d", __func__,
						node_info->node_info->id);
			ret = -ENODEV;
			goto exit_flush_clk_data;
		}

		MSM_BUS_DBG("%s: Updated %d clk to %llu", __func__,
				node_info->node_info->id, nodeclk->rate);
		nodeclk->dirty = 0;
	}
exit_flush_clk_data:
	return ret;
}

int msm_bus_commit_data(int *dirty_nodes, int ctx, int num_dirty)
{
	int ret = 0;
	int i = 0;

	for (i = 0; i < num_dirty; i++) {
		struct device *node_device =
					bus_find_device(&msm_bus_type, NULL,
						(void *)dirty_nodes[i],
						msm_bus_device_match_adhoc);

		ret = flush_bw_data(node_device, ctx);
		if (ret)
			MSM_BUS_ERR("%s: Error flushing bw data for node %d",
					__func__, dirty_nodes[i]);

		ret = flush_clk_data(node_device, ctx);
		if (ret) {
			MSM_BUS_ERR("%s: Error flushing clk data for node %d",
					__func__, dirty_nodes[i]);
			goto exit_commit_data;
		}
	}
exit_commit_data:
	return ret;
}

static int add_dirty_node(int **dirty_nodes, int id, int *num_dirty)
{
	int i;
	int found = 0;
	int ret = 0;
	int *dnode = NULL;

	for (i = 0; i < *num_dirty; i++) {
		if ((*dirty_nodes)[i] == id) {
			found = 1;
			break;
		}
	}

	/*store num hops statically after get path*/
	/*store pointer to nodeinfo here*/
	if (!found) {
		(*num_dirty)++;
		dnode =
			krealloc(*dirty_nodes, sizeof(int) * (*num_dirty),
								GFP_KERNEL);

		if (ZERO_OR_NULL_PTR(dnode)) {
			MSM_BUS_ERR("%s: Failure allocating dirty nodes array",
								 __func__);
			ret = -ENOMEM;
		} else {
			*dirty_nodes = dnode;
			(*dirty_nodes)[(*num_dirty) - 1] = id;
		}
	}

	return ret;
}

int msm_bus_update_bw(struct msm_bus_node_device_type *nodedev, int ctx,
			int64_t add_bw, int **dirty_nodes, int *num_dirty)
{
	int ret = 0;
	int i, j;
	uint64_t cur_ab_slp = 0;
	uint64_t cur_ab_act = 0;

	if (nodedev->node_info->virt_dev)
		goto exit_update_bw;

	for (i = 0; i < NUM_CTX; i++) {
		for (j = 0; j < nodedev->num_lnodes; j++) {
			if (i == DUAL_CTX) {
				cur_ab_act +=
					nodedev->lnode_list[j].lnode_ab[i];
				cur_ab_slp +=
					nodedev->lnode_list[j].lnode_ab[i];
			} else
				cur_ab_act +=
					nodedev->lnode_list[j].lnode_ab[i];
		}
	}

	if (nodedev->node_ab.ab[MSM_RPM_CTX_ACTIVE_SET] != cur_ab_act) {
		nodedev->node_ab.ab[MSM_RPM_CTX_ACTIVE_SET] = cur_ab_act;
		nodedev->node_ab.ab[MSM_RPM_CTX_SLEEP_SET] = cur_ab_slp;
		nodedev->node_ab.dirty = true;
		ret = add_dirty_node(dirty_nodes, nodedev->node_info->id,
								num_dirty);

		if (ret) {
			MSM_BUS_ERR("%s: Failed to add dirty node %d", __func__,
						nodedev->node_info->id);
			goto exit_update_bw;
		}
	}

exit_update_bw:
	return ret;
}

int msm_bus_update_clks(struct msm_bus_node_device_type *nodedev,
		int ctx, int **dirty_nodes, int *num_dirty)
{
	int status = 0;
	struct nodeclk *nodeclk;
	struct nodeclk *busclk;
	struct msm_bus_node_device_type *bus_info = NULL;
	uint64_t req_clk;

	bus_info = nodedev->node_info->bus_device->platform_data;

	if (!bus_info) {
		MSM_BUS_ERR("%s: Unable to find bus device for device %d",
			__func__, nodedev->node_info->id);
		status = -ENODEV;
		goto exit_set_clks;
	}

	req_clk = nodedev->cur_clk_hz[ctx];
	busclk = &bus_info->clk[ctx];

	/* If the busclk rate is not marked "dirty" update the rate.
	 * If the rate is dirty but less than this updated request
	 * then modify (this means within the same update transaction)
	 */
	if (!busclk->dirty || (busclk->dirty && (busclk->rate < req_clk))) {
		busclk->rate = req_clk;
		busclk->dirty = 1;
		MSM_BUS_DBG("%s: Modifying bus clk %d Rate %llu", __func__,
					bus_info->node_info->id, req_clk);
		status = add_dirty_node(dirty_nodes, bus_info->node_info->id,
								num_dirty);

		if (status) {
			MSM_BUS_ERR("%s: Failed to add dirty node %d", __func__,
						bus_info->node_info->id);
			goto exit_set_clks;
		}
	}

	req_clk = nodedev->cur_clk_hz[ctx];
	nodeclk = &nodedev->clk[ctx];

	if (!nodeclk->dirty || (nodeclk->dirty && (nodeclk->rate < req_clk))) {
		nodeclk->rate = req_clk;
		nodeclk->dirty = 1;
		MSM_BUS_DBG("%s: Modifying node clk %d Rate %llu", __func__,
					nodedev->node_info->id, req_clk);
		status = add_dirty_node(dirty_nodes, nodedev->node_info->id,
								num_dirty);
		if (status) {
			MSM_BUS_ERR("%s: Failed to add dirty node %d", __func__,
						nodedev->node_info->id);
			goto exit_set_clks;
		}
	}

exit_set_clks:
	return status;
}

static void msm_bus_fab_init_noc_ops(struct msm_bus_node_device_type *bus_dev)
{
	switch (bus_dev->fabdev->bus_type) {
	case MSM_BUS_NOC:
		msm_bus_noc_set_ops(bus_dev);
		break;
	case MSM_BUS_BIMC:
		msm_bus_bimc_set_ops(bus_dev);
		break;
	default:
		MSM_BUS_ERR("%s: Invalid Bus type", __func__);
	}
}

static int msm_bus_qos_disable_clk(struct msm_bus_node_device_type *node)
{
	struct msm_bus_node_device_type *bus_node = NULL;
	int ret = 0;

	if (!node) {
		ret = -ENXIO;
		goto exit_disable_qos_clk;
	}

	bus_node = node->node_info->bus_device->platform_data;

	if (!bus_node) {
		ret = -ENXIO;
		goto exit_disable_qos_clk;
	}

	ret = disable_nodeclk(&bus_node->clk[DUAL_CTX]);

	if (ret) {
		MSM_BUS_ERR("%s: Failed to disable bus clk, node %d",
			__func__, node->node_info->id);
		goto exit_disable_qos_clk;
	}

	if (!IS_ERR_OR_NULL(node->qos_clk.clk)) {
		ret = disable_nodeclk(&node->qos_clk);

		if (ret) {
			MSM_BUS_ERR("%s: Failed to disable mas qos clk,node %d",
				__func__, node->node_info->id);
			goto exit_disable_qos_clk;
		}
	}

exit_disable_qos_clk:
	return ret;
}

static int msm_bus_qos_enable_clk(struct msm_bus_node_device_type *node)
{
	struct msm_bus_node_device_type *bus_node = NULL;
	long rounded_rate;
	int ret = 0;

	if (!node) {
		ret = -ENXIO;
		goto exit_enable_qos_clk;
	}

	bus_node = node->node_info->bus_device->platform_data;

	if (!bus_node) {
		ret = -ENXIO;
		goto exit_enable_qos_clk;
	}

	/* Check if the bus clk is already set before trying to set it
	 * Do this only during
	 *	a. Bootup
	 *	b. Only for bus clks
	 **/
	if (!clk_get_rate(bus_node->clk[DUAL_CTX].clk)) {
		rounded_rate = clk_round_rate(bus_node->clk[DUAL_CTX].clk, 1);
		ret = setrate_nodeclk(&bus_node->clk[DUAL_CTX], rounded_rate);
		if (ret) {
			MSM_BUS_ERR("%s: Failed to set bus clk, node %d",
				__func__, node->node_info->id);
			goto exit_enable_qos_clk;
		}

		ret = enable_nodeclk(&bus_node->clk[DUAL_CTX]);
		if (ret) {
			MSM_BUS_ERR("%s: Failed to enable bus clk, node %d",
				__func__, node->node_info->id);
			goto exit_enable_qos_clk;
		}
	}

	if (!IS_ERR_OR_NULL(node->qos_clk.clk)) {
		rounded_rate = clk_round_rate(node->qos_clk.clk, 1);
		ret = setrate_nodeclk(&node->qos_clk, rounded_rate);
		if (ret) {
			MSM_BUS_ERR("%s: Failed to enable mas qos clk, node %d",
				__func__, node->node_info->id);
			goto exit_enable_qos_clk;
		}

		ret = enable_nodeclk(&node->qos_clk);
		if (ret) {
			MSM_BUS_ERR("%s: Failed to enable mas qos clk, node %d",
				__func__, node->node_info->id);
			goto exit_enable_qos_clk;
		}
	}

exit_enable_qos_clk:
	return ret;
}

static int msm_bus_dev_init_qos(struct device *dev, void *data)
{
	int ret = 0;
	struct msm_bus_node_device_type *node_dev = NULL;

	node_dev = dev->platform_data;

	if (!node_dev) {
		MSM_BUS_ERR("%s: Unable to get node device info" , __func__);
		ret = -ENXIO;
		goto exit_init_qos;
	}

	MSM_BUS_DBG("Device = %d", node_dev->node_info->id);

	if (node_dev->ap_owned) {
		struct msm_bus_node_device_type *bus_node_info;

		bus_node_info = node_dev->node_info->bus_device->platform_data;

		if (!bus_node_info) {
			MSM_BUS_ERR("%s: Unable to get bus device infofor %d",
				__func__,
				node_dev->node_info->id);
			ret = -ENXIO;
			goto exit_init_qos;
		}

		if (bus_node_info->fabdev &&
			!bus_node_info->fabdev->bypass_qos_prg &&
			bus_node_info->fabdev->noc_ops.qos_init) {

			if (node_dev->ap_owned &&
				(node_dev->node_info->mode) != -1) {

				msm_bus_qos_enable_clk(node_dev);
				bus_node_info->fabdev->noc_ops.qos_init(
					node_dev,
					bus_node_info->fabdev->qos_base,
					bus_node_info->fabdev->base_offset,
					bus_node_info->fabdev->qos_off,
					bus_node_info->fabdev->qos_freq);
				msm_bus_qos_disable_clk(node_dev);
			}
		} else
			MSM_BUS_ERR("%s: Skipping QOS init for %d",
				__func__, node_dev->node_info->id);
	}
exit_init_qos:
	return ret;
}

static int msm_bus_fabric_init(struct device *dev,
			struct msm_bus_node_device_type *pdata)
{
	struct msm_bus_fab_device_type *fabdev;
	struct msm_bus_node_device_type *node_dev = NULL;
	int ret = 0;

	node_dev = dev->platform_data;
	if (!node_dev) {
		MSM_BUS_ERR("%s: Unable to get bus device info" , __func__);
		ret = -ENXIO;
		goto exit_fabric_init;
	}

	if (node_dev->node_info->virt_dev) {
		MSM_BUS_ERR("%s: Skip Fab init for virtual device %d", __func__,
						node_dev->node_info->id);
		goto exit_fabric_init;
	}

	fabdev = devm_kzalloc(dev, sizeof(struct msm_bus_fab_device_type),
								GFP_KERNEL);
	if (!fabdev) {
		MSM_BUS_ERR("Fabric alloc failed\n");
		ret = -ENOMEM;
		goto exit_fabric_init;
	}

	node_dev->fabdev = fabdev;
	fabdev->pqos_base = pdata->fabdev->pqos_base;
	fabdev->qos_range = pdata->fabdev->qos_range;
	fabdev->base_offset = pdata->fabdev->base_offset;
	fabdev->bus_type = pdata->fabdev->bus_type;
	msm_bus_fab_init_noc_ops(node_dev);

	fabdev->qos_base = devm_ioremap(dev,
				fabdev->pqos_base, fabdev->qos_range);
	if (!fabdev->qos_base) {
		MSM_BUS_ERR("%s: Error remapping address 0x%x :bus device %d",
			__func__,
			 fabdev->pqos_base, node_dev->node_info->id);
		ret = -ENOMEM;
		goto exit_fabric_init;
	}

	/*if (msmbus_coresight_init(pdev))
		pr_warn("Coresight support absent for bus: %d\n", pdata->id);*/
exit_fabric_init:
	return ret;
}

static int msm_bus_init_clk(struct device *bus_dev,
				struct msm_bus_node_device_type *pdata)
{
	unsigned int ctx;
	int ret = 0;
	struct msm_bus_node_device_type *node_dev = bus_dev->platform_data;

	for (ctx = 0; ctx < NUM_CTX; ctx++) {
		if (!IS_ERR_OR_NULL(pdata->clk[ctx].clk)) {
			node_dev->clk[ctx].clk = pdata->clk[ctx].clk;
			node_dev->clk[ctx].enable = false;
			node_dev->clk[ctx].dirty = false;
			MSM_BUS_ERR("%s: Valid node clk node %d ctx %d",
				__func__, node_dev->node_info->id, ctx);
		}
	}

	if (!IS_ERR_OR_NULL(pdata->qos_clk.clk)) {
		node_dev->qos_clk.clk = pdata->qos_clk.clk;
		node_dev->qos_clk.enable = false;
		MSM_BUS_ERR("%s: Valid Iface clk node %d", __func__,
						node_dev->node_info->id);
	}

	return ret;
}

static int msm_bus_copy_node_info(struct msm_bus_node_device_type *pdata,
				struct device *bus_dev)
{
	int ret = 0;
	struct msm_bus_node_info_type *node_info = NULL;
	struct msm_bus_node_info_type *pdata_node_info = NULL;
	struct msm_bus_node_device_type *bus_node = NULL;

	bus_node = bus_dev->platform_data;

	if (!bus_node || !pdata) {
		ret = -ENXIO;
		MSM_BUS_ERR("%s: Invalid pointers pdata %p, bus_node %p",
			__func__, pdata, bus_node);
		goto exit_copy_node_info;
	}

	node_info = bus_node->node_info;
	pdata_node_info = pdata->node_info;

	node_info->name = pdata_node_info->name;
	node_info->id =  pdata_node_info->id;
	node_info->bus_device_id = pdata_node_info->bus_device_id;
	node_info->mas_rpm_id = pdata_node_info->mas_rpm_id;
	node_info->slv_rpm_id = pdata_node_info->slv_rpm_id;
	node_info->num_connections = pdata_node_info->num_connections;
	node_info->buswidth = pdata_node_info->buswidth;
	node_info->virt_dev = pdata_node_info->virt_dev;
	node_info->is_fab_dev = pdata_node_info->is_fab_dev;
	node_info->mode = pdata_node_info->mode;
	node_info->prio1 = pdata_node_info->prio1;
	node_info->prio0 = pdata_node_info->prio0;

	node_info->dev_connections = devm_kzalloc(bus_dev,
			sizeof(struct device *) *
				pdata_node_info->num_connections,
			GFP_KERNEL);
	if (!node_info->dev_connections) {
		MSM_BUS_ERR("%s:Bus dev connections alloc failed\n", __func__);
		ret = -ENOMEM;
		goto exit_copy_node_info;
	}

	node_info->connections = devm_kzalloc(bus_dev,
			sizeof(int) * pdata_node_info->num_connections,
			GFP_KERNEL);
	if (!node_info->connections) {
		MSM_BUS_ERR("%s:Bus connections alloc failed\n", __func__);
		devm_kfree(bus_dev, node_info->dev_connections);
		ret = -ENOMEM;
		goto exit_copy_node_info;
	}

	memcpy(node_info->connections,
		pdata_node_info->connections,
		sizeof(int) * pdata_node_info->num_connections);

exit_copy_node_info:
	return ret;
}

static struct device *msm_bus_device_init(
			struct msm_bus_node_device_type *pdata)
{
	struct device *bus_dev = NULL;
	struct msm_bus_node_device_type *bus_node = NULL;
	struct msm_bus_node_info_type *node_info = NULL;
	int ret = 0;

	bus_dev = kzalloc(sizeof(struct device), GFP_KERNEL);
	if (!bus_dev) {
		MSM_BUS_ERR("%s:Device alloc failed\n", __func__);
		bus_dev = NULL;
		goto exit_device_init;
	}
	/**
	* Init here so we can use devm calls
	*/
	device_initialize(bus_dev);

	bus_node = devm_kzalloc(bus_dev,
			sizeof(struct msm_bus_node_device_type), GFP_KERNEL);
	if (!bus_node) {
		MSM_BUS_ERR("%s:Bus node alloc failed\n", __func__);
		kfree(bus_dev);
		bus_dev = NULL;
		goto exit_device_init;
	}

	node_info = devm_kzalloc(bus_dev,
			sizeof(struct msm_bus_node_info_type), GFP_KERNEL);
	if (!node_info) {
		MSM_BUS_ERR("%s:Bus node info alloc failed\n", __func__);
		devm_kfree(bus_dev, bus_node);
		kfree(bus_dev);
		bus_dev = NULL;
		goto exit_device_init;
	}

	bus_node->node_info = node_info;
	bus_dev->platform_data = bus_node;

	if (msm_bus_copy_node_info(pdata, bus_dev) < 0) {
		devm_kfree(bus_dev, bus_node);
		devm_kfree(bus_dev, node_info);
		kfree(bus_dev);
		bus_dev = NULL;
		goto exit_device_init;
	}

	bus_dev->bus = &msm_bus_type;
	dev_set_name(bus_dev, bus_node->node_info->name);

	ret = device_add(bus_dev);
	if (ret < 0) {
		MSM_BUS_ERR("%s: Error registering device %d",
				__func__, pdata->node_info->id);
		devm_kfree(bus_dev, bus_node);
		devm_kfree(bus_dev, node_info->dev_connections);
		devm_kfree(bus_dev, node_info->connections);
		devm_kfree(bus_dev, node_info);
		kfree(bus_dev);
		bus_dev = NULL;
		goto exit_device_init;
	}

exit_device_init:
	return bus_dev;
}

static int msm_bus_setup_dev_conn(struct device *bus_dev, void *data)
{
	struct msm_bus_node_device_type *bus_node = NULL;
	int ret = 0;
	int j;

	bus_node = bus_dev->platform_data;
	if (!bus_node) {
		MSM_BUS_ERR("%s: Can't get device info", __func__);
		ret = -ENODEV;
		goto exit_setup_dev_conn;
	}

	/* Setup parent bus device for this node */
	if (!bus_node->node_info->is_fab_dev) {
		struct device *bus_parent_device =
			bus_find_device(&msm_bus_type, NULL,
				(void *)bus_node->node_info->bus_device_id,
				msm_bus_device_match_adhoc);

		if (!bus_parent_device) {
			MSM_BUS_ERR("%s: Error finding parentdev %d parent %d",
				__func__,
				bus_node->node_info->id,
				bus_node->node_info->bus_device_id);
			ret = -ENXIO;
			goto exit_setup_dev_conn;
		}
		bus_node->node_info->bus_device = bus_parent_device;
	}

	for (j = 0; j < bus_node->node_info->num_connections; j++) {
		bus_node->node_info->dev_connections[j] =
			bus_find_device(&msm_bus_type, NULL,
				(void *)bus_node->node_info->connections[j],
				msm_bus_device_match_adhoc);

		if (!bus_node->node_info->dev_connections[j]) {
			MSM_BUS_ERR("%s: Error finding conn %d for device %d",
				__func__, bus_node->node_info->connections[j],
				 bus_node->node_info->id);
			ret = -ENODEV;
			goto exit_setup_dev_conn;
		}
	}

exit_setup_dev_conn:
	return ret;
}

static int msm_bus_node_debug(struct device *bus_dev, void *data)
{
	int j;
	int ret = 0;
	struct msm_bus_node_device_type *bus_node = NULL;

	bus_node = bus_dev->platform_data;
	if (!bus_node) {
		MSM_BUS_ERR("%s: Can't get device info", __func__);
		ret = -ENODEV;
		goto exit_node_debug;
	}

	MSM_BUS_DBG("Device = %d buswidth %u", bus_node->node_info->id,
				bus_node->node_info->buswidth);
	for (j = 0; j < bus_node->node_info->num_connections; j++) {
		struct msm_bus_node_device_type *bdev =
			(struct msm_bus_node_device_type *)
			bus_node->node_info->dev_connections[j]->platform_data;
		MSM_BUS_DBG("\n\t Connection[%d] %d", j, bdev->node_info->id);
	}

exit_node_debug:
	return ret;
}

static int msm_bus_device_probe(struct platform_device *pdev)
{
	unsigned int i, ret;
	struct msm_bus_device_node_registration *pdata;

	/* If possible, get pdata from device-tree */
	if (pdev->dev.of_node)
		pdata = msm_bus_of_to_pdata(pdev);
	else {
		pdata = (struct msm_bus_device_node_registration *)pdev->
			dev.platform_data;
	}

	if (IS_ERR_OR_NULL(pdata)) {
		MSM_BUS_ERR("No platform data found");
		ret = -ENODATA;
		goto exit_device_probe;
	}

	for (i = 0; i < pdata->num_devices; i++) {
		struct device *node_dev = NULL;

		node_dev = msm_bus_device_init(&pdata->info[i]);

		if (!node_dev) {
			MSM_BUS_ERR("%s: Error during dev init for %d",
				__func__, pdata->info[i].node_info->id);
			ret = -ENXIO;
			goto exit_device_probe;
		}

		ret = msm_bus_init_clk(node_dev, &pdata->info[i]);
		/*Is this a fabric device ?*/
		if (pdata->info[i].node_info->is_fab_dev) {
			MSM_BUS_DBG("%s: %d is a fab", __func__,
						pdata->info[i].node_info->id);
			ret = msm_bus_fabric_init(node_dev, &pdata->info[i]);
			if (ret) {
				MSM_BUS_ERR("%s: Error intializing fab %d",
					__func__, pdata->info[i].node_info->id);
				goto exit_device_probe;
			}
		}
	}

	ret = bus_for_each_dev(&msm_bus_type, NULL, NULL,
						msm_bus_setup_dev_conn);
	if (ret) {
		MSM_BUS_ERR("%s: Error setting up dev connections", __func__);
		goto exit_device_probe;
	}

	ret = bus_for_each_dev(&msm_bus_type, NULL, NULL, msm_bus_dev_init_qos);
	if (ret) {
		MSM_BUS_ERR("%s: Error during qos init", __func__);
		goto exit_device_probe;
	}

	bus_for_each_dev(&msm_bus_type, NULL, NULL, msm_bus_node_debug);

	/* Register the arb layer ops */
	msm_bus_arb_setops_adhoc(&arb_ops);
exit_device_probe:
	return ret;
}

static int msm_bus_free_dev(struct device *dev, void *data)
{
	struct msm_bus_node_device_type *bus_node = NULL;

	bus_node = dev->platform_data;

	if (bus_node)
		MSM_BUS_ERR("\n%s: Removing device %d", __func__,
						bus_node->node_info->id);
	device_unregister(dev);
	return 0;
}

int msm_bus_device_remove(struct platform_device *pdev)
{
	bus_for_each_dev(&msm_bus_type, NULL, NULL, msm_bus_free_dev);
	return 0;
}

static struct of_device_id fabric_match[] = {
	{.compatible = "qcom,msm-bus-device"},
	{}
};

static struct platform_driver msm_bus_device_driver = {
	.probe = msm_bus_device_probe,
	.remove = msm_bus_device_remove,
	.driver = {
		.name = "msm_bus_device",
		.owner = THIS_MODULE,
		.of_match_table = fabric_match,
	},
};

int __init msm_bus_device_init_driver(void)
{
	MSM_BUS_ERR("msm_bus_fabric_init_driver\n");
	return platform_driver_register(&msm_bus_device_driver);
}
subsys_initcall(msm_bus_device_init_driver);
