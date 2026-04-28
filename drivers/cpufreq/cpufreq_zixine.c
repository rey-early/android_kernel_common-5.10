// SPDX-License-Identifier: GPL-2.0
/*
 * Zixine Velocity v1.2.7 - GKI Optimized
 * Fierce at work, economical at leisure.
 */

#include <linux/cpufreq.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/workqueue.h>
#include <linux/tick.h>
#include <linux/sched/cpufreq.h>
#include <linux/kernel_stat.h>

/* Parameter Tunable */
static unsigned int target_load = 85;
static unsigned int velocity_kick = 15;

struct zv_cpu_info {
	u64 prev_idle;
	u64 prev_wall;
	unsigned int prev_load;
	unsigned int target_freq;
	unsigned int hold_count;
};

static DEFINE_PER_CPU(struct zv_cpu_info, zv_info);

struct zv_policy_info {
	struct delayed_work work;
	struct cpufreq_policy *policy;
	bool is_active;
};

/* Helper to retrieve idle data without using GKI prohibited symbols */
static u64 get_cpu_idle_time_gki(int cpu)
{
	u64 idle;
	idle = kcpustat_cpu(cpu).cpustat[CPUTIME_IDLE];
	return nsecs_to_usecs(idle);
}

static void zv_eval(struct cpufreq_policy *policy)
{
	struct zv_cpu_info *info = &per_cpu(zv_info, policy->cpu);
	u64 cur_wall, cur_idle, wall_time, idle_time;
	unsigned int load, freq_target;
	int load_velocity;

	cur_wall = ktime_to_us(ktime_get());
	cur_idle = get_cpu_idle_time_gki(policy->cpu);

	if (unlikely(cur_wall <= info->prev_wall))
		return;

	wall_time = cur_wall - info->prev_wall;
	idle_time = (cur_idle > info->prev_idle) ? (cur_idle - info->prev_idle) : 0;

	if (unlikely(wall_time <= idle_time || wall_time == 0)) {
		load = 0;
	} else {
		load = (unsigned int)(100 * (wall_time - idle_time) / wall_time);
	}

	info->prev_wall = cur_wall;
	info->prev_idle = cur_idle;

	/* --- VELOCITY LOGIC (ORIGINAL) --- */
	load_velocity = (int)load - (int)info->prev_load;
	info->prev_load = load;

	if (load_velocity > (int)velocity_kick) {
		freq_target = policy->max;
		info->hold_count = 10; 
	} else if (info->hold_count > 0) {
		freq_target = (policy->max * 70) / 100;
		if (freq_target < (policy->max * load / target_load))
			freq_target = (policy->max * load / target_load);
		info->hold_count--;
	} else {
		freq_target = (unsigned int)(((u64)policy->max * load) / target_load);
	}

	/* Frequency Limiting */
	if (freq_target < policy->min) freq_target = policy->min;
	if (freq_target > policy->max) freq_target = policy->max;

	if (freq_target != info->target_freq) {
		info->target_freq = freq_target;
		if (policy->fast_switch_enabled) {
			cpufreq_driver_fast_switch(policy, freq_target);
		} else {
			__cpufreq_driver_target(policy, freq_target, CPUFREQ_RELATION_L);
		}
	}
}

static void zv_work_handler(struct work_struct *work)
{
	struct zv_policy_info *zpinfo = container_of(work, struct zv_policy_info, work.work);
	
	if (!zpinfo->is_active)
		return;

	zv_eval(zpinfo->policy);
	
	unsigned int delay = (per_cpu(zv_info, zpinfo->policy->cpu).prev_load > 5 || 
						  per_cpu(zv_info, zpinfo->policy->cpu).hold_count > 0) ? 16 : 100;
	
	schedule_delayed_work_on(zpinfo->policy->cpu, &zpinfo->work, msecs_to_jiffies(delay));
}

static int zv_init(struct cpufreq_policy *policy)
{
	struct zv_policy_info *zpinfo = kzalloc(sizeof(*zpinfo), GFP_KERNEL);
	if (!zpinfo) return -ENOMEM;

	zpinfo->policy = policy;
	zpinfo->is_active = false;
	INIT_DELAYED_WORK(&zpinfo->work, zv_work_handler);
	policy->governor_data = zpinfo;
	return 0;
}

static void zv_exit(struct cpufreq_policy *policy)
{
	struct zv_policy_info *zpinfo = policy->governor_data;
	if (zpinfo) {
		zpinfo->is_active = false;
		cancel_delayed_work_sync(&zpinfo->work);
		kfree(zpinfo);
		policy->governor_data = NULL;
	}
}

static int zv_start(struct cpufreq_policy *policy)
{
	struct zv_policy_info *zpinfo = policy->governor_data;
	unsigned int cpu;

	if (!zpinfo) return -EINVAL;
	zpinfo->is_active = true;

	for_each_cpu(cpu, policy->cpus) {
		struct zv_cpu_info *info = &per_cpu(zv_info, cpu);
		info->prev_wall = ktime_to_us(ktime_get());
		info->prev_idle = get_cpu_idle_time_gki(cpu);
		info->hold_count = 0;
		info->prev_load = 0;
		info->target_freq = policy->cur;
	}

	schedule_delayed_work_on(policy->cpu, &zpinfo->work, msecs_to_jiffies(16));
	return 0;
}

static void zv_stop(struct cpufreq_policy *policy)
{
	struct zv_policy_info *zpinfo = policy->governor_data;
	if (zpinfo) {
		zpinfo->is_active = false;
		cancel_delayed_work_sync(&zpinfo->work);
	}
}

static struct cpufreq_governor gov_zixine_velocity = {
	.name		= "zixine_velocity",
	.flags		= CPUFREQ_GOV_DYNAMIC_SWITCHING,
	.owner		= THIS_MODULE,
	.init		= zv_init,
	.exit		= zv_exit,
	.start		= zv_start,
	.stop		= zv_stop,
};

static int __init zv_gov_init(void)
{
	return cpufreq_register_governor(&gov_zixine_velocity);
}

static void __exit zv_gov_exit(void)
{
	cpufreq_unregister_governor(&gov_zixine_velocity);
}

module_init(zv_gov_init);
module_exit(zv_gov_exit);

MODULE_AUTHOR("Zixine");
MODULE_DESCRIPTION("Zixine Velocity Governor v1.2.7 (GKI 12-5.10)");
MODULE_LICENSE("GPL v2");
