// SPDX-License-Identifier: GPL-2.0
/*
 * Zixine Velocity v1.2.1 - Hybrid Schedutil Edition (Fixed)
 * Author: zixine
 */

#include <linux/cpufreq.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/sched/cpufreq.h>
#include <linux/sched/topology.h>
#include <linux/workqueue.h>
#include <linux/cpumask.h>

/* * FIX: Definisikan enum secara lokal karena drivers/cpufreq 
 * tidak punya akses ke kernel/sched/sched.h
 */
enum schedutil_type {
    FREQUENCY_UTIL,
    ENERGY_UTIL,
};

/* Zixine Velocity Tunables */
static unsigned int target_load_big = 65;
static unsigned int target_load_little = 75; 
static unsigned int touch_boost_util = 250; 
static struct workqueue_struct *zv_wq;

module_param(target_load_big, uint, 0644);
module_param(target_load_little, uint, 0644);
module_param(touch_boost_util, uint, 0644);

struct zv_cpu_info {
    unsigned long prev_util;
    unsigned int target_freq;
    unsigned int hold_counter;
    unsigned int next_delay_ms;
};

static DEFINE_PER_CPU(struct zv_cpu_info, zv_info);

struct zv_policy_info {
    struct delayed_work work;
    struct cpufreq_policy *policy;
};

/* * Ambil fungsi dari symbol table kernel. 
 * Selama kernelmu punya SCHEDUTIL, fungsi ini pasti ada.
 */
extern unsigned long schedutil_cpu_util(int cpu, unsigned long util_cfs,
				 unsigned long max, enum schedutil_type type,
				 struct task_struct *p);

static void zv_eval_freq(struct cpufreq_policy *policy)
{
    struct zv_cpu_info *info = &per_cpu(zv_info, policy->cpu);
    unsigned long util, max_cap;
    unsigned int load, freq_target, target_load;
    int util_velocity;

    /* 1. Ambil kapasitas maksimal CPU */
    max_cap = arch_scale_cpu_capacity(policy->cpu);
    if (!max_cap) max_cap = 1024; // Fallback standar

    /* 2. Ambil Utilitas dari Scheduler (PELT/WALT) */
    util = schedutil_cpu_util(policy->cpu, 0, max_cap, FREQUENCY_UTIL, NULL);

    /* 3. Hitung Velocity */
    util_velocity = (int)util - (int)info->prev_util;
    info->prev_util = util;

    /* 4. Touch Boost: Lonjakan mendadak */
    if (util_velocity > 150) {
        util += touch_boost_util;
        info->hold_counter = 12; 
    }

    /* Skala load 0-100 */
    load = (util * 100) / max_cap;
    if (load > 100) load = 100;

    target_load = (policy->cpu >= 4) ? target_load_big : target_load_little;

    /* 5. Kalkulasi Target Frekuensi */
    if (info->hold_counter > 0) {
        unsigned int boost_floor = (policy->max * 60) / 100;
        freq_target = (unsigned int)(((u64)policy->max * load) / target_load);
        if (freq_target < boost_floor) freq_target = boost_floor;
        info->hold_counter--;
    } else {
        freq_target = (unsigned int)(((u64)policy->max * load) / target_load);
    }

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

    info->next_delay_ms = (util > 100 || info->hold_counter > 0) ? 8 : 40;
}

static void zv_work_handler(struct work_struct *work)
{
    struct zv_policy_info *zpinfo = container_of(work, struct zv_policy_info, work.work);
    struct cpufreq_policy *policy = zpinfo->policy;

    zv_eval_freq(policy);

    queue_delayed_work_on(policy->cpu, zv_wq, &zpinfo->work, 
                          msecs_to_jiffies(per_cpu(zv_info, policy->cpu).next_delay_ms));
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
    cpufreq_enable_fast_switch(policy);
    for_each_cpu(cpu, policy->cpus) {
        struct zv_cpu_info *info = &per_cpu(zv_info, cpu);
        info->prev_util = 0;
        info->hold_counter = 0;
        info->target_freq = policy->cur;
    }
    queue_delayed_work_on(policy->cpu, zv_wq, &((struct zv_policy_info *)policy->governor_data)->work, msecs_to_jiffies(8));
    return 0;
}

static void zv_stop(struct cpufreq_policy *policy)
{
    struct zv_policy_info *zpinfo = policy->governor_data;
    if (zpinfo) cancel_delayed_work_sync(&zpinfo->work);
    cpufreq_disable_fast_switch(policy);
}

static struct cpufreq_governor gov_zixine_velocity = {
    .name       = "zixine_velocity",
    .flags      = CPUFREQ_GOV_DYNAMIC_SWITCHING,
    .owner      = THIS_MODULE,
    .init       = zv_init,
    .exit       = zv_exit,
    .start      = zv_start,
    .stop       = zv_stop,
};

static int __init zv_gov_init(void)
{
    zv_wq = alloc_workqueue("zv_wq", WQ_HIGHPRI | WQ_FREEZABLE, 0);
    if (!zv_wq) return -ENOMEM;
    if (cpufreq_register_governor(&gov_zixine_velocity)) {
        destroy_workqueue(zv_wq);
        return -EINVAL;
    }
    pr_info("Zixine Velocity v1.2.1: Hybrid Schedutil (Fixed) Engaged!\n");
    return 0;
}

static void __exit zv_gov_exit(void)
{
    cpufreq_unregister_governor(&gov_zixine_velocity);
    if (zv_wq) destroy_workqueue(zv_wq);
}

module_init(zv_gov_init);
module_exit(zv_gov_exit);
MODULE_LICENSE("GPL v2");
MODULE_AUTHOR("zixine");
