// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2020 Oplus. All rights reserved.
 */
#include <linux/sched.h>
#include <linux/sysctl.h>
#include <linux/topology.h>
#include <linux/cpufreq.h>
#include "sched.h"
#include <linux/prefer_silver.h>

int sysctl_prefer_silver = 1;
int sysctl_heavy_task_thresh = 50;
int sysctl_cpu_util_thresh = 85;
int sysctl_silver_trigger_freq = 1503000;

bool prefer_silver_check_freq(int cpu)
{
	unsigned int freq = cpufreq_quick_get(cpu);
	return freq < sysctl_silver_trigger_freq;
}

static inline unsigned long ps_task_util(struct task_struct *p)
{
	return p->se.avg.util_avg;
}

unsigned long ps_cpu_util(int cpu)
{
	return cpu_rq(cpu)->cfs.avg.util_avg;
}

bool prefer_silver_check_task_util(struct task_struct *p)
{
	unsigned long util, cap, thresh;

	util = p->se.avg.util_avg;
	cap = capacity_orig_of(task_cpu(p));
	thresh = cap * sysctl_heavy_task_thresh / 100;

	if (util > cap * 80 / 100)
		return false;

	return util < thresh;
}

bool prefer_silver_check_cpu_util(int cpu)
{
	return (capacity_orig_of(cpu) * sysctl_cpu_util_thresh) >
		(ps_cpu_util(cpu) * 100);
}

int find_best_silver_cpu(struct task_struct *p)
{
	int i, best_cpu = -1;
	unsigned long min_util = ULONG_MAX;

	for_each_cpu(i, p->cpus_ptr) {
		unsigned long cur_util;

		if (cpu_topology[i].package_id != 0)
			continue;

		if (!prefer_silver_check_freq(i))
			continue;

		if (!prefer_silver_check_cpu_util(i))
			continue;

		cur_util = ps_cpu_util(i);
		if (cur_util < min_util) {
			min_util = cur_util;
			best_cpu = i;
		}
	}
	return best_cpu;
}
