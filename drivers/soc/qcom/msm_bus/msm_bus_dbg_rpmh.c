// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2019-2020, The Linux Foundation. All rights reserved.
 */

#define pr_fmt(fmt) "AXI: %s(): " fmt, __func__

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/seq_file.h>
#include <linux/debugfs.h>
#include <linux/slab.h>
#include <linux/mutex.h>
#include <linux/rtmutex.h>
#include <linux/string.h>
#include <linux/uaccess.h>
#include <linux/hrtimer.h>
#include <linux/msm-bus-board.h>
#include <linux/msm-bus.h>
#include <linux/msm_bus_rules.h>
#include "msm_bus_core.h"
#include "msm_bus_rpmh.h"

#include <trace/events/trace_msm_bus.h>

#define MAX_BUFF_SIZE 4096
#define FILL_LIMIT 128

static struct dentry *clients;
static struct dentry *dir;
static DEFINE_MUTEX(msm_bus_dbg_fablist_lock);
static DEFINE_RT_MUTEX(msm_bus_dbg_cllist_lock);
static struct msm_bus_dbg_state {
	uint32_t cl;
	uint8_t enable;
	uint8_t current_index;
} clstate;

struct msm_bus_cldata {
	const struct msm_bus_scale_pdata *pdata;
	const struct msm_bus_client_handle *handle;
	int index;
	uint32_t clid;
	int size;
	int vote_count;
	struct dentry *file;
	struct list_head list;
	char buffer[MAX_BUFF_SIZE];
};

struct msm_bus_fab_list {
	const char *name;
	int size;
	struct dentry *file;
	struct list_head list;
	char buffer[MAX_BUFF_SIZE];
};

static char *rules_buf;
static char *dbg_buf;

static LIST_HEAD(fabdata_list);
static LIST_HEAD(cl_list);
static LIST_HEAD(bcm_list);

/**
 * The following structures and functions are used for
 * the test-client which can be created at run-time.
 */

static struct msm_bus_vectors init_vectors[1];
static struct msm_bus_vectors current_vectors[1];
static struct msm_bus_vectors requested_vectors[1];

static struct msm_bus_paths shell_client_usecases[] = {
	{
		.num_paths = ARRAY_SIZE(init_vectors),
		.vectors = init_vectors,
	},
	{
		.num_paths = ARRAY_SIZE(current_vectors),
		.vectors = current_vectors,
	},
	{
		.num_paths = ARRAY_SIZE(requested_vectors),
		.vectors = requested_vectors,
	},
};

static struct msm_bus_scale_pdata shell_client = {
	.usecase = shell_client_usecases,
	.num_usecases = ARRAY_SIZE(shell_client_usecases),
	.name = "test-client",
};

static void msm_bus_dbg_init_vectors(void)
{
	init_vectors[0].src = -1;
	init_vectors[0].dst = -1;
	init_vectors[0].ab = 0;
	init_vectors[0].ib = 0;
	current_vectors[0].src = -1;
	current_vectors[0].dst = -1;
	current_vectors[0].ab = 0;
	current_vectors[0].ib = 0;
	requested_vectors[0].src = -1;
	requested_vectors[0].dst = -1;
	requested_vectors[0].ab = 0;
	requested_vectors[0].ib = 0;
	clstate.enable = 0;
	clstate.current_index = 0;
}

static int msm_bus_dbg_update_cl_request(uint32_t cl)
{
	int ret = 0;

	if (clstate.current_index < 2)
		clstate.current_index = 2;
	else {
		clstate.current_index = 1;
		current_vectors[0].ab = requested_vectors[0].ab;
		current_vectors[0].ib = requested_vectors[0].ib;
	}

	if (clstate.enable) {
		MSM_BUS_DBG("Updating request for shell client, index: %d\n",
			clstate.current_index);
		ret = msm_bus_scale_client_update_request(clstate.cl,
			clstate.current_index);
	} else
		MSM_BUS_DBG("Enable bit not set. Skipping update request\n");

	return ret;
}

static void msm_bus_dbg_unregister_client(uint32_t cl)
{
	MSM_BUS_DBG("Unregistering shell client\n");
	msm_bus_scale_unregister_client(clstate.cl);
	clstate.cl = 0;
}

static uint32_t msm_bus_dbg_register_client(void)
{
	int ret = 0;

	if (init_vectors[0].src != requested_vectors[0].src) {
		MSM_BUS_DBG("Shell client master changed. Unregistering\n");
		msm_bus_dbg_unregister_client(clstate.cl);
	}
	if (init_vectors[0].dst != requested_vectors[0].dst) {
		MSM_BUS_DBG("Shell client slave changed. Unregistering\n");
		msm_bus_dbg_unregister_client(clstate.cl);
	}

	current_vectors[0].src = init_vectors[0].src;
	requested_vectors[0].src = init_vectors[0].src;
	current_vectors[0].dst = init_vectors[0].dst;
	requested_vectors[0].dst = init_vectors[0].dst;

	if (!clstate.enable) {
		MSM_BUS_DBG("Enable bit not set, skipping registration: cl %d\n"
			, clstate.cl);
		return 0;
	}

	if (clstate.cl) {
		MSM_BUS_DBG("Client  registered, skipping registration\n");
		return clstate.cl;
	}

	MSM_BUS_DBG("Registering shell client\n");
	ret = msm_bus_scale_register_client(&shell_client);
	return ret;
}

static int msm_bus_dbg_mas_get(void  *data, u64 *val)
{
	*val = init_vectors[0].src;
	MSM_BUS_DBG("Get master: %llu\n", *val);
	return 0;
}

static int msm_bus_dbg_mas_set(void  *data, u64 val)
{
	init_vectors[0].src = val;
	MSM_BUS_DBG("Set master: %llu\n", val);
	clstate.cl = msm_bus_dbg_register_client();
	return 0;
}
DEFINE_DEBUGFS_ATTRIBUTE(shell_client_mas_fops, msm_bus_dbg_mas_get,
	msm_bus_dbg_mas_set, "%llu\n");

static int msm_bus_dbg_slv_get(void  *data, u64 *val)
{
	*val = init_vectors[0].dst;
	MSM_BUS_DBG("Get slave: %llu\n", *val);
	return 0;
}

static int msm_bus_dbg_slv_set(void  *data, u64 val)
{
	init_vectors[0].dst = val;
	MSM_BUS_DBG("Set slave: %llu\n", val);
	clstate.cl = msm_bus_dbg_register_client();
	return 0;
}
DEFINE_DEBUGFS_ATTRIBUTE(shell_client_slv_fops, msm_bus_dbg_slv_get,
	msm_bus_dbg_slv_set, "%llu\n");

static int msm_bus_dbg_ab_get(void  *data, u64 *val)
{
	*val = requested_vectors[0].ab;
	MSM_BUS_DBG("Get ab: %llu\n", *val);
	return 0;
}

static int msm_bus_dbg_ab_set(void  *data, u64 val)
{
	requested_vectors[0].ab = val;
	MSM_BUS_DBG("Set ab: %llu\n", val);
	return 0;
}
DEFINE_DEBUGFS_ATTRIBUTE(shell_client_ab_fops, msm_bus_dbg_ab_get,
	msm_bus_dbg_ab_set, "%llu\n");

static int msm_bus_dbg_ib_get(void  *data, u64 *val)
{
	*val = requested_vectors[0].ib;
	MSM_BUS_DBG("Get ib: %llu\n", *val);
	return 0;
}

static int msm_bus_dbg_ib_set(void  *data, u64 val)
{
	requested_vectors[0].ib = val;
	MSM_BUS_DBG("Set ib: %llu\n", val);
	return 0;
}
DEFINE_DEBUGFS_ATTRIBUTE(shell_client_ib_fops, msm_bus_dbg_ib_get,
	msm_bus_dbg_ib_set, "%llu\n");

static int msm_bus_dbg_en_get(void  *data, u64 *val)
{
	*val = clstate.enable;
	MSM_BUS_DBG("Get enable: %llu\n", *val);
	return 0;
}

static int msm_bus_dbg_en_set(void  *data, u64 val)
{
	int ret = 0;

	clstate.enable = val;
	if (clstate.enable) {
		if (!clstate.cl) {
			MSM_BUS_DBG("client: %u\n", clstate.cl);
			clstate.cl = msm_bus_dbg_register_client();
			if (clstate.cl)
				ret = msm_bus_dbg_update_cl_request(clstate.cl);
		} else {
			MSM_BUS_DBG("update request for cl: %u\n", clstate.cl);
			ret = msm_bus_dbg_update_cl_request(clstate.cl);
		}
	}

	MSM_BUS_DBG("Set enable: %llu\n", val);
	return ret;
}
DEFINE_DEBUGFS_ATTRIBUTE(shell_client_en_fops, msm_bus_dbg_en_get,
	msm_bus_dbg_en_set, "%llu\n");

/**
 * The following functions are used for viewing the client data
 * and changing the client request at run-time
 */

static ssize_t client_data_read(struct file *file, char __user *buf,
	size_t count, loff_t *ppos)
{
	int bsize = 0;
	uint32_t cl = (uint32_t)(uintptr_t)file->private_data;
	struct msm_bus_cldata *cldata = NULL;
	const struct msm_bus_client_handle *handle = file->private_data;
	int found = 0;
	ssize_t ret;

	rt_mutex_lock(&msm_bus_dbg_cllist_lock);
	list_for_each_entry(cldata, &cl_list, list) {
		if ((cldata->clid == cl) ||
			(cldata->handle && (cldata->handle == handle))) {
			found = 1;
			break;
		}
	}

	if (!found) {
		rt_mutex_unlock(&msm_bus_dbg_cllist_lock);
		return 0;
	}

	bsize = cldata->size;
	ret = simple_read_from_buffer(buf, count, ppos,
		cldata->buffer, bsize);
	rt_mutex_unlock(&msm_bus_dbg_cllist_lock);

	return ret;
}

static const struct file_operations client_data_fops = {
	.open		= simple_open,
	.read		= client_data_read,
};

static struct dentry *msm_bus_dbg_create(const char *name, mode_t mode,
	struct dentry *dent, uint32_t clid)
{
	if (dent == NULL) {
		MSM_BUS_DBG("debugfs not ready yet\n");
		return NULL;
	}
	return debugfs_create_file(name, mode, dent, (void *)(uintptr_t)clid,
		&client_data_fops);
}

static ssize_t  msm_bus_dbg_update_request_write(struct file *file,
	const char __user *ubuf, size_t cnt, loff_t *ppos)
{
	struct msm_bus_cldata *cldata;
	unsigned long index = 0;
	int ret = 0;
	char *chid;
	int found = 0;
	uint32_t clid;
	ssize_t res = cnt;

	dbg_buf = kmalloc((sizeof(char) * (cnt + 1)), GFP_KERNEL);
	if (!dbg_buf)
		return -ENOMEM;

	if (cnt == 0) {
		res = 0;
		goto out;
	}
	if (copy_from_user(dbg_buf, ubuf, cnt)) {
		res = -EFAULT;
		goto out;
	}
	dbg_buf[cnt] = '\0';
	chid = dbg_buf;
	MSM_BUS_DBG("buffer: %s\n size: %zu\n", dbg_buf, sizeof(ubuf));

	rt_mutex_lock(&msm_bus_dbg_cllist_lock);
	list_for_each_entry(cldata, &cl_list, list) {
		if (strnstr(chid, cldata->pdata->name, cnt)) {
			found = 1;
			strsep(&chid, " ");
			if (chid) {
				ret = kstrtoul(chid, 10, &index);
				if (ret) {
					MSM_BUS_DBG("Index conversion\n"
						" failed\n");
					rt_mutex_unlock(
						&msm_bus_dbg_cllist_lock);
					res = -EFAULT;
					goto out;
				}
			} else {
				MSM_BUS_DBG("Error parsing input. Index not\n"
					" found\n");
				found = 0;
			}
			if (index > cldata->pdata->num_usecases) {
				MSM_BUS_DBG("Invalid index!\n");
				rt_mutex_unlock(&msm_bus_dbg_cllist_lock);
				res = -EINVAL;
				goto out;
			}
			clid = cldata->clid;
			break;
		}
	}
	rt_mutex_unlock(&msm_bus_dbg_cllist_lock);

	if (found)
		msm_bus_scale_client_update_request(clid, index);

out:
	kfree(dbg_buf);
	return res;
}

/**
 * The following functions are used for viewing the commit data
 * for each fabric
 */
static ssize_t fabric_data_read(struct file *file, char __user *buf,
	size_t count, loff_t *ppos)
{
	struct msm_bus_fab_list *fablist = NULL;
	int bsize = 0;
	ssize_t ret;
	const char *name = file->private_data;
	int found = 0;

	mutex_lock(&msm_bus_dbg_fablist_lock);
	list_for_each_entry(fablist, &fabdata_list, list) {
		if (strcmp(fablist->name, name) == 0) {
			found = 1;
			break;
		}
	}
	if (!found) {
		mutex_unlock(&msm_bus_dbg_fablist_lock);
		return -ENOENT;
	}
	bsize = fablist->size;
	ret = simple_read_from_buffer(buf, count, ppos,
		fablist->buffer, bsize);
	mutex_unlock(&msm_bus_dbg_fablist_lock);
	return ret;
}

static const struct file_operations fabric_data_fops = {
	.open		= simple_open,
	.read		= fabric_data_read,
};

static ssize_t rules_dbg_read(struct file *file, char __user *buf,
	size_t count, loff_t *ppos)
{
	ssize_t ret;

	memset(rules_buf, 0, MAX_BUFF_SIZE);
	print_rules_buf(rules_buf, MAX_BUFF_SIZE);
	ret = simple_read_from_buffer(buf, count, ppos,
		rules_buf, MAX_BUFF_SIZE);
	return ret;
}

static const struct file_operations rules_dbg_fops = {
	.open		= simple_open,
	.read		= rules_dbg_read,
};

static const struct file_operations msm_bus_dbg_update_request_fops = {
	.open = simple_open,
	.write = msm_bus_dbg_update_request_write,
};

static ssize_t bcm_client_read(struct file *file,
	char __user *buf, size_t count, loff_t *ppos)
{
	int i, j, cnt;
	char msg[50];
	struct device *dev = NULL;
	struct link_node *lnode_list = NULL;
	struct msm_bus_node_device_type *cur_node = NULL;
	struct msm_bus_node_device_type *cur_bcm = NULL;
	u64 act_ab, act_ib, dual_ab, dual_ib;

	cnt = scnprintf(msg, 64,
		"\nDumping curent BCM client votes to trace log\n");
	if (*ppos)
		goto exit_dump_bcm_clients_read;

	rt_mutex_lock(&msm_bus_dbg_cllist_lock);
	list_for_each_entry(cur_bcm, &bcm_list, dbg_link) {
		for (i = 0; i < cur_bcm->num_lnodes; i++) {
			if (!cur_bcm->lnode_list[i].in_use)
				continue;

			dev = bus_find_device(&msm_bus_type, NULL,
				(void *)&cur_bcm->lnode_list[i].bus_dev_id,
				msm_bus_device_match_adhoc);
			cur_node = to_msm_bus_node(dev);

			for (j = 0; j < cur_node->num_lnodes; j++) {
				if (!cur_node->lnode_list[j].in_use)
					continue;

				lnode_list = &cur_node->lnode_list[j];
				act_ab = lnode_list->lnode_ab[ACTIVE_CTX];
				act_ib = lnode_list->lnode_ib[ACTIVE_CTX];
				dual_ab = lnode_list->lnode_ab[DUAL_CTX];
				dual_ib = lnode_list->lnode_ib[DUAL_CTX];

				if (!act_ab && !act_ib && !dual_ab && !dual_ib)
					continue;

				if (!act_ab && !act_ib) {
					act_ab = dual_ab;
					act_ib = dual_ib;
				}

				trace_bus_bcm_client_status(
					cur_bcm->node_info->name,
					cur_node->lnode_list[j].cl_name,
					act_ab, act_ib, dual_ab, dual_ib);

				MSM_BUS_ERR(
					"bcm=%s client=%s act_ab=%llu act_ib=%llu slp_ab=%llu slp_ib=%llu\n",
					cur_bcm->node_info->name,
					cur_node->lnode_list[j].cl_name,
					(unsigned long long)act_ab,
					(unsigned long long)act_ib,
					(unsigned long long)dual_ab,
					(unsigned long long)dual_ib);
			}
		}
	}
	rt_mutex_unlock(&msm_bus_dbg_cllist_lock);
exit_dump_bcm_clients_read:
	return simple_read_from_buffer(buf, count, ppos, msg, cnt);
}

static const struct file_operations msm_bus_dbg_dump_bcm_clients_fops = {
	.open = simple_open,
	.read = bcm_client_read,
};

static ssize_t msm_bus_dbg_dump_clients_read(struct file *file,
	char __user *buf, size_t count, loff_t *ppos)
{
	int j, cnt;
	char msg[50];
	struct msm_bus_cldata *cldata = NULL;

	cnt = scnprintf(msg, 50,
		"\nDumping curent client votes to trace log\n");
	if (*ppos)
		goto exit_dump_clients_read;

	rt_mutex_lock(&msm_bus_dbg_cllist_lock);
	list_for_each_entry(cldata, &cl_list, list) {
		if (IS_ERR_OR_NULL(cldata->pdata))
			continue;
		for (j = 0; j < cldata->pdata->usecase->num_paths; j++) {
			if (cldata->index == -1)
				continue;
			trace_bus_client_status(
			cldata->pdata->name,
			cldata->pdata->usecase[cldata->index].vectors[j].src,
			cldata->pdata->usecase[cldata->index].vectors[j].dst,
			cldata->pdata->usecase[cldata->index].vectors[j].ab,
			cldata->pdata->usecase[cldata->index].vectors[j].ib,
			cldata->pdata->active_only,
			cldata->vote_count);
		}
	}
	rt_mutex_unlock(&msm_bus_dbg_cllist_lock);
exit_dump_clients_read:
	return simple_read_from_buffer(buf, count, ppos, msg, cnt);
}

static const struct file_operations msm_bus_dbg_dump_clients_fops = {
	.open		= simple_open,
	.read		= msm_bus_dbg_dump_clients_read,
};

static int __init msm_bus_debugfs_init(void)
{
	struct dentry *commit, *shell_client, *rules_dbg;
	struct msm_bus_fab_list *fablist;
	struct msm_bus_cldata *cldata = NULL;
	uint64_t val = 0;

	dir = debugfs_create_dir("msm-bus-dbg", NULL);
	if ((!dir) || IS_ERR(dir)) {
		MSM_BUS_ERR("Couldn't create msm-bus-dbg\n");
		goto err;
	}

	clients = debugfs_create_dir("client-data", dir);
	if ((!dir) || IS_ERR(dir)) {
		MSM_BUS_ERR("Couldn't create clients\n");
		goto err;
	}

	shell_client = debugfs_create_dir("shell-client", dir);
	if ((!dir) || IS_ERR(dir)) {
		MSM_BUS_ERR("Couldn't create clients\n");
		goto err;
	}

	commit = debugfs_create_dir("commit-data", dir);
	if ((!dir) || IS_ERR(dir)) {
		MSM_BUS_ERR("Couldn't create commit\n");
		goto err;
	}

	rules_dbg = debugfs_create_dir("rules-dbg", dir);
	if ((!rules_dbg) || IS_ERR(rules_dbg)) {
		MSM_BUS_ERR("Couldn't create rules-dbg\n");
		goto err;
	}

	if (debugfs_create_file("print_rules", 0644,
		rules_dbg, &val, &rules_dbg_fops) == NULL)
		goto err;

	if (debugfs_create_file("update_request", 0644,
		shell_client, &val, &shell_client_en_fops) == NULL)
		goto err;
	if (debugfs_create_file("ib", 0644, shell_client, &val,
		&shell_client_ib_fops) == NULL)
		goto err;
	if (debugfs_create_file("ab", 0644, shell_client, &val,
		&shell_client_ab_fops) == NULL)
		goto err;
	if (debugfs_create_file("slv", 0644, shell_client,
		&val, &shell_client_slv_fops) == NULL)
		goto err;
	if (debugfs_create_file("mas", 0644, shell_client,
		&val, &shell_client_mas_fops) == NULL)
		goto err;
	if (debugfs_create_file("update-request", 0644,
		clients, NULL, &msm_bus_dbg_update_request_fops) == NULL)
		goto err;

	rules_buf = kzalloc(MAX_BUFF_SIZE, GFP_KERNEL);
	if (!rules_buf) {
		MSM_BUS_ERR("Failed to alloc rules_buf");
		goto err;
	}

	rt_mutex_lock(&msm_bus_dbg_cllist_lock);
	list_for_each_entry(cldata, &cl_list, list) {
		if (cldata->pdata) {
			if (cldata->pdata->name == NULL) {
				MSM_BUS_DBG("Client name not found\n");
				continue;
			}
			cldata->file = msm_bus_dbg_create(cldata->pdata->name,
					0444, clients, cldata->clid);
		} else if (cldata->handle) {
			if (cldata->handle->name == NULL) {
				MSM_BUS_DBG("Client doesn't have a name\n");
				continue;
			}
			cldata->file = debugfs_create_file(cldata->handle->name,
							0444, clients,
							(void *)cldata->handle,
							&client_data_fops);
		}
	}
	rt_mutex_unlock(&msm_bus_dbg_cllist_lock);

	if (debugfs_create_file("dump_clients", 0644,
		clients, NULL, &msm_bus_dbg_dump_clients_fops) == NULL)
		goto err;

	if (debugfs_create_file("dump_bcm_clients", 0644,
		clients, NULL, &msm_bus_dbg_dump_bcm_clients_fops) == NULL)
		goto err;

	mutex_lock(&msm_bus_dbg_fablist_lock);
	list_for_each_entry(fablist, &fabdata_list, list) {
		fablist->file = debugfs_create_file(fablist->name, 0444,
			commit, (void *)fablist->name, &fabric_data_fops);
		if (fablist->file == NULL) {
			MSM_BUS_DBG("Cannot create files for commit data\n");
			kfree(rules_buf);
			mutex_unlock(&msm_bus_dbg_fablist_lock);
			goto err;
		}
	}
	mutex_unlock(&msm_bus_dbg_fablist_lock);

	msm_bus_dbg_init_vectors();
	return 0;
err:
	debugfs_remove_recursive(dir);
	return -ENODEV;
}
late_initcall(msm_bus_debugfs_init);

static void __exit msm_bus_dbg_teardown(void)
{
	struct msm_bus_fab_list *fablist = NULL, *fablist_temp;
	struct msm_bus_cldata *cldata = NULL, *cldata_temp;

	debugfs_remove_recursive(dir);

	rt_mutex_lock(&msm_bus_dbg_cllist_lock);
	list_for_each_entry_safe(cldata, cldata_temp, &cl_list, list) {
		list_del(&cldata->list);
		kfree(cldata);
	}
	rt_mutex_unlock(&msm_bus_dbg_cllist_lock);

	mutex_lock(&msm_bus_dbg_fablist_lock);
	list_for_each_entry_safe(fablist, fablist_temp, &fabdata_list, list) {
		list_del(&fablist->list);
		kfree(fablist);
	}
	kfree(rules_buf);
	mutex_unlock(&msm_bus_dbg_fablist_lock);
}
module_exit(msm_bus_dbg_teardown);
MODULE_DESCRIPTION("Debugfs for msm bus scaling client");
