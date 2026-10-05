// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2023-2024, Qualcomm Innovation Center, Inc. All rights reserved.
 */
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/of_address.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>
#include <linux/topology.h>
#include "walt.h"

#define CYCLE_CNTR_OFFSET(core_id, acc_count)		\
				((acc_count) ? (((core_id) + 1) * 4) : 0)

#define CYCLE_CNTR_ENABLE_BIT		BIT(0)
#define CYCLE_CNTR_32BIT_FULL		(1ULL << 32)

struct cpufreq_counter {
	u64 total_cycle_counter;
	u32 prev_cycle_counter;
	spinlock_t lock;
};

static DEFINE_PER_CPU(struct cpufreq_counter, walt_cpufreq_counter);

struct walt_cpufreq_soc_data {
	u32 reg_enable;
	u32 reg_cycle_cntr;
	bool accumulative_counter;
};

struct walt_cpufreq_data {
	void __iomem *base;
	const struct walt_cpufreq_soc_data *soc_data;
};

static struct walt_cpufreq_data cpufreq_data[MAX_CLUSTERS];

u64 walt_cpufreq_get_cpu_cycle_counter(int cpu, u64 wc)
{
	const struct walt_cpufreq_soc_data *soc_data;
	struct cpufreq_counter *cpu_counter;
	struct walt_cpufreq_data *data;
	u64 cycle_counter_ret;
	unsigned long flags;
	u16 offset;
	u32 val;

	if (WARN_ON_ONCE(cpu < 0 || cpu >= nr_cpu_ids))
		return 0;

	data = &cpufreq_data[cpu_cluster(cpu)->id];
	if (WARN_ON_ONCE(!data->base || !data->soc_data))
		return 0;

	soc_data = data->soc_data;

	offset = CYCLE_CNTR_OFFSET(topology_core_id(cpu),
				   soc_data->accumulative_counter);
	val = readl_relaxed(data->base + soc_data->reg_cycle_cntr + offset);

	cpu_counter = per_cpu_ptr(&walt_cpufreq_counter, cpu);

	spin_lock_irqsave(&cpu_counter->lock, flags);

	if (val < cpu_counter->prev_cycle_counter) {
		cpu_counter->total_cycle_counter += CYCLE_CNTR_32BIT_FULL -
			cpu_counter->prev_cycle_counter + val;
	} else {
		cpu_counter->total_cycle_counter += val -
			cpu_counter->prev_cycle_counter;
	}
	cpu_counter->prev_cycle_counter = val;

	cycle_counter_ret = cpu_counter->total_cycle_counter;

	spin_unlock_irqrestore(&cpu_counter->lock, flags);

	pr_debug_once("CPU %u, core-id 0x%x, offset %u cycle_counts=%llu\n",
		      cpu, topology_core_id(cpu), offset, cycle_counter_ret);

	return cycle_counter_ret;
}

static int walt_cpufreq_cycle_cntr_driver_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct walt_sched_cluster *cluster;
	int ret = 0;
	int initialized_clusters = 0;

	for_each_sched_cluster(cluster) {
		struct device_node *cpu_np;
		struct of_phandle_args args;
		struct resource *res;
		void __iomem *base;
		int cpu;

		if (WARN_ON_ONCE(cluster->id >= MAX_CLUSTERS)) {
			ret = -EINVAL;
			goto err_rollback;
		}

		cpu = cluster_first_cpu(cluster);
		cpu_np = of_cpu_device_node_get(cpu);
		if (!cpu_np) {
			dev_err(dev, "failed to get cpu node for cpu %d\n", cpu);
			ret = -EINVAL;
			goto err_rollback;
		}

		ret = of_parse_phandle_with_args(cpu_np, "qcom,freq-domain",
						 "#freq-domain-cells", 0, &args);
		of_node_put(cpu_np);
		if (ret) {
			dev_err(dev, "failed to parse freq-domain for cpu %d: %d\n",
				cpu, ret);
			goto err_rollback;
		}

		res = platform_get_resource(pdev, IORESOURCE_MEM, args.args[0]);
		if (!res) {
			dev_err(dev, "failed to get mem resource %d\n", args.args[0]);
			of_node_put(args.np);
			ret = -ENODEV;
			goto err_rollback;
		}

		base = devm_ioremap(dev, res->start, resource_size(res));
		of_node_put(args.np);
		if (!base) {
			dev_err(dev, "failed to map resource %pR\n", res);
			ret = -ENOMEM;
			goto err_rollback;
		}

		cpufreq_data[cluster->id].soc_data = of_device_get_match_data(dev);
		cpufreq_data[cluster->id].base = base;

		if (!(readl_relaxed(base + cpufreq_data[cluster->id].soc_data->reg_enable)
			    & CYCLE_CNTR_ENABLE_BIT)) {
			dev_err(dev, "Domain-%d cpufreq hardware not enabled\n",
				args.args[0]);
			ret = -ENODEV;
			goto err_rollback;
		}

		initialized_clusters++;
	}

	if (!walt_get_cycle_counts_cb) {
		int cpu;

		for_each_possible_cpu(cpu) {
			struct cpufreq_counter *cnt = per_cpu_ptr(&walt_cpufreq_counter, cpu);
			spin_lock_init(&cnt->lock);
		}

		walt_get_cycle_counts_cb = walt_cpufreq_get_cpu_cycle_counter;
		use_cycle_counter = true;
		complete(&walt_get_cycle_counts_cb_completion);
	}

	return 0;

err_rollback:
	for_each_sched_cluster(cluster) {
		if (initialized_clusters-- <= 0)
			break;
		if (cpufreq_data[cluster->id].base) {
			devm_iounmap(dev, cpufreq_data[cluster->id].base);
			cpufreq_data[cluster->id].base = NULL;
		}
	}
	return ret;
}

static void walt_cpufreq_cycle_cntr_driver_remove(struct platform_device *pdev)
{
	struct walt_sched_cluster *cluster;

	if (walt_get_cycle_counts_cb == walt_cpufreq_get_cpu_cycle_counter) {
		walt_get_cycle_counts_cb = NULL;
		use_cycle_counter = false;
	}

	for_each_sched_cluster(cluster) {
		if (cpufreq_data[cluster->id].base) {
			devm_iounmap(&pdev->dev, cpufreq_data[cluster->id].base);
			cpufreq_data[cluster->id].base = NULL;
		}
	}
}

static const struct walt_cpufreq_soc_data hw_soc_data = {
	.reg_enable = 0x0,
	.reg_cycle_cntr = 0x9c0,
	.accumulative_counter = false,
};

static const struct walt_cpufreq_soc_data epss_soc_data = {
	.reg_enable = 0x0,
	.reg_cycle_cntr = 0x3c4,
	.accumulative_counter = true,
};

static const struct of_device_id walt_cpufreq_cycle_cntr_match[] = {
	{ .compatible = "qcom,cycle-cntr-hw", .data = &hw_soc_data },
	{ .compatible = "qcom,epss", .data = &epss_soc_data },
	{ /* Sentinel */ }
};

static struct platform_driver walt_cpufreq_cycle_cntr_driver = {
	.driver = {
		.name = "walt-cpufreq-cycle-cntr",
		.of_match_table = walt_cpufreq_cycle_cntr_match,
	},
	.probe = walt_cpufreq_cycle_cntr_driver_probe,
	.remove = walt_cpufreq_cycle_cntr_driver_remove,
};

int walt_cpufreq_cycle_cntr_driver_register(void)
{
	return platform_driver_register(&walt_cpufreq_cycle_cntr_driver);
}
