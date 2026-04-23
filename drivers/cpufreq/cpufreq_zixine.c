// SPDX-License-Identifier: GPL-2.0
/*
 * Zixine Velocity v1.2.6 - Ultimate Hybrid Performance
 * Garang saat kerja, irit saat santai.
 */

#include <linux/cpufreq.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/workqueue.h>
#include <linux/tick.h>

/* Parameter Tunable */
static unsigned int target_load = 85;       // Target beban (80-90 seimbang)
static unsigned int velocity_kick = 15;     // Sensitivitas lonjakan (lebih kecil = lebih peka)

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
};

static void zv_eval(struct cpufreq_policy *policy)
{
	struct zv_cpu_info *info = &per_cpu(zv_info, policy->cpu);
	u64 cur_wall, cur_idle, wall_time, idle_time;
	unsigned int load, freq_target;
	int load_velocity;

	cur_wall = ktime_to_us(ktime_get());
	cur_idle = get_cpu_idle_time(policy->cpu, &cur_wall, 1);

	wall_time = cur_wall - info->prev_wall;
	idle_time = cur_idle - info->prev_idle;

	if (unlikely((s64)wall_time <= (s64)idle_time || wall_time == 0)) {
		load = 0;
	} else {
		load = (unsigned int)(100 * (wall_time - idle_time) / wall_time);
	}

	info->prev_wall = cur_wall;
	info->prev_idle = cur_idle;

	/* --- VELOCITY LOGIC --- */
	load_velocity = (int)load - (int)info->prev_load;
	info->prev_load = load;

	if (load_velocity > (int)velocity_kick) {
		/* LONJAKAN TERDETEKSI: Garang! */
		freq_target = policy->max;
		info->hold_count = 10; // Tahan performa tinggi selama 10 siklus
	} else if (info->hold_count > 0) {
		/* MODE SIAGA: Mencegah stutter */
		freq_target = (policy->max * 70) / 100; // Tahan di 70% frekuensi
		if (freq_target < (policy->max * load / target_load))
			freq_target = (policy->max * load / target_load);
		info->hold_count--;
	} else {
		/* MODE HEMAT: Turun sesuai beban aplikasi ringan */
		freq_target = (unsigned int)(((u64)policy->max * load) / target_load);
	}

	/* Proteksi Batas */
	if (freq_target < policy->min) freq_target = policy->min;
	if (freq_target > policy->max) freq_target = policy->max;

	if (freq_target != info->target_freq) {
		info->target_freq = freq_target;
		if (policy->fast_switch_enabled) {
			cpufreq_driver_fast_switch(policy, freq_target);
		} else {
			__cpufreq_driver_target(policy, freq_target, CPUFREQ_RELATION_H);
		}
	}
}

static void zv_work_handler(struct work_struct *work)
{
	struct zv_policy_info *zpinfo = container_of(work, struct zv_policy_info, work.work);
	zv_eval(zpinfo->policy);
	
	/* ADAPTIVE SAMPLING: 16ms (Aktif) vs 100ms (Idle) */
	unsigned int delay = (per_cpu(zv_info, zpinfo->policy->cpu).prev_load > 5 || 
						  per_cpu(zv_info, zpinfo->policy->cpu).hold_count > 0) ? 16 : 100;
	
	schedule_delayed_work_on(zpinfo->policy->cpu, &zpinfo->work, msecs_to_jiffies(delay));
}

static int zv_init(struct cpufreq_policy *policy)
{
	struct zv_policy_info *zpinfo = kzalloc(sizeof(*zpinfo), GFP_KERNEL);
	if (!zpinfo) return -ENOMEM;
	zpinfo->policy = policy;
	INIT_DELAYED_WORK(&zpinfo->work, zv_work_handler);
	policy->governor_data = zpinfo;
	return 0;
}

static void zv_exit(struct cpufreq_policy *policy)
{
	struct zv_policy_info *zpinfo = policy->governor_data;
	if (zpinfo) {
		cancel_delayed_work_sync(&zpinfo->work);
		kfree(zpinfo);
		policy->governor_data = NULL;
	}
}

static int zv_start(struct cpufreq_policy *policy)
{
	unsigned int cpu;
	for_each_cpu(cpu, policy->cpus) {
		struct zv_cpu_info *info = &per_cpu(zv_info, cpu);
		info->prev_wall = ktime_to_us(ktime_get());
		info->prev_idle = get_cpu_idle_time(cpu, &info->prev_wall, 1);
		info->hold_count = 0;
		info->prev_load = 0;
		info->target_freq = policy->cur;
	}
	schedule_delayed_work_on(policy->cpu, &((struct zv_policy_info *)policy->governor_data)->work, msecs_to_jiffies(16));
	return 0;
}

static void zv_stop(struct cpufreq_policy *policy)
{
	struct zv_policy_info *zpinfo = policy->governor_data;
	if (zpinfo) cancel_delayed_work_sync(&zpinfo->work);
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

module_init(zv_gov_init);
MODULE_LICENSE("GPL v2");
