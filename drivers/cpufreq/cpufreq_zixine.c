// SPDX-License-Identifier: GPL-2.0
/*
 * Zixine Velocity v1.2.3 - Final Heat Fix
 * Fokus: Memperbaiki error kompilasi dan masalah suhu (Overheat).
 */

#include <linux/cpufreq.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/workqueue.h>
#include <linux/kernel_stat.h>
#include <linux/sched/cputime.h>
#include <linux/jiffies.h>

/* Parameter yang lebih dingin */
static unsigned int target_load_big = 85;    
static unsigned int target_load_little = 90; 
static unsigned int touch_boost_load = 30;   
static struct workqueue_struct *zv_wq;

module_param(target_load_big, uint, 0644);
module_param(target_load_little, uint, 0644);
module_param(touch_boost_load, uint, 0644);

struct zv_cpu_info {
    u64 prev_cpu_idle;
    u64 prev_cpu_wall;
    unsigned int prev_load;          
    unsigned int target_freq;
    unsigned int hold_counter;       
    unsigned int next_delay_ms;
};

static DEFINE_PER_CPU(struct zv_cpu_info, zv_info);

struct zv_policy_info {
    struct delayed_work work;
    struct cpufreq_policy *policy;
};

/* Helper: Ambil waktu CPU dalam Mikrodetik */
static u64 get_cpu_idle_us(int cpu)
{
    struct kernel_cpustat kcpustat;
    kcpustat_cpu_fetch(&kcpustat, cpu);
    /* CPUTIME di kernel modern biasanya nanodetik, kita bagi 1000 */
    return (kcpustat.cpustat[CPUTIME_IDLE] + kcpustat.cpustat[CPUTIME_IOWAIT]) / 1000;
}

static void zv_eval_freq(struct cpufreq_policy *policy)
{
    struct zv_cpu_info *info = &per_cpu(zv_info, policy->cpu);
    u64 cur_wall, cur_idle, wall_time, idle_time;
    unsigned int load, freq_target, target_load;

    /* Gunakan ktime_get_ns / 1000 sebagai pengganti ktime_to_us */
    cur_wall = ktime_get_ns() / 1000;
    cur_idle = get_cpu_idle_us(policy->cpu);

    wall_time = cur_wall - info->prev_cpu_wall;
    idle_time = cur_idle - info->prev_cpu_idle;

    /* Hindari pembagian nol atau nilai negatif */
    if (unlikely((s64)wall_time <= (s64)idle_time || wall_time == 0)) {
        load = 0;
    } else {
        load = (unsigned int)(100 * (wall_time - idle_time) / wall_time);
    }

    if (load > 100) load = 100;

    /* Boost hanya jika ada lonjakan signifikan */
    if ((int)load - (int)info->prev_load > 25) {
        load += touch_boost_load;
        info->hold_counter = 5; 
    }
    info->prev_load = load;

    target_load = (policy->cpu >= 4) ? target_load_big : target_load_little;

    /* Logika Frekuensi Dinamis */
    if (load > 95) {
        freq_target = policy->max;
    } else if (info->hold_counter > 0) {
        /* Tahan di frekuensi menengah saat boost (tidak mengunci di max) */
        freq_target = (policy->max * 60) / 100; 
        info->hold_counter--;
    } else {
        /* Skala linear murni agar frekuensi bisa turun ke paling bawah */
        freq_target = (unsigned int)(((u64)policy->max * load) / target_load);
    }

    /* Clamp ke batas policy device */
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

    /* Sampling Rate: 16ms (Aktif) vs 100ms (Idle/Deep Sleep) */
    info->next_delay_ms = (load > 15 || info->hold_counter > 0) ? 16 : 100;
}

static void zv_work_handler(struct work_struct *work)
{
    struct zv_policy_info *zpinfo = container_of(work, struct zv_policy_info, work.work);
    zv_eval_freq(zpinfo->policy);
    queue_delayed_work_on(zpinfo->policy->cpu, zv_wq, &zpinfo->work, 
                          msecs_to_jiffies(per_cpu(zv_info, zpinfo->policy->cpu).next_delay_ms));
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
        info->prev_cpu_wall = ktime_get_ns() / 1000;
        info->prev_cpu_idle = get_cpu_idle_us(cpu);
        info->hold_counter = 0;
        info->prev_load = 0;
        info->target_freq = policy->cur;
    }
    queue_delayed_work_on(policy->cpu, zv_wq, &((struct zv_policy_info *)policy->governor_data)->work, msecs_to_jiffies(16));
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
    pr_info("Zixine Velocity v1.2.3: Thermal-Optimized Ready!\n");
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
