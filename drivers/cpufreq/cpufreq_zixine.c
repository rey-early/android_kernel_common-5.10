// SPDX-License-Identifier: GPL-2.0
/*
 * Zixine Velocity v1.1 - Smoothness Edition
 * Optimasi: Touch Boost Simulation, IO-Wait Sensitivity, & Frame-Sync Sampling
 * Author: zixine & Gemini
 */

#include <linux/cpufreq.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/sched/clock.h>
#include <linux/workqueue.h>
#include <linux/tick.h>

/* Zixine Velocity Parameters */
static unsigned int target_load_big = 60;    /* Lebih agresif dari v1.0 */
static unsigned int target_load_little = 70; 
static unsigned int fast_ramp_up_load = 80;  
static unsigned int touch_boost_load = 75;   /* Simulasi beban saat ada interaksi */

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

/* ========================================================================
 * CORE LOGIC: VELOCITY-BASED SCALING (SMOOTHNESS EDITION)
 * ======================================================================== */
static void zv_eval_freq(struct cpufreq_policy *policy)
{
    struct zv_cpu_info *info = &per_cpu(zv_info, policy->cpu);
    u64 cur_wall, cur_idle, wall_time, idle_time;
    unsigned int load, freq_target, target_load;
    int load_velocity;

    cur_wall = ktime_get_ns();
    cur_idle = get_cpu_idle_time(policy->cpu, &cur_wall, 1); // 1 = IO Wait Aware

    wall_time = cur_wall - info->prev_cpu_wall;
    idle_time = cur_idle - info->prev_cpu_idle;

    info->prev_cpu_wall = cur_wall;
    info->prev_cpu_idle = cur_idle;

    if (unlikely(!wall_time || wall_time < idle_time)) {
        load = 0;
    } else {
        load = 100 * (wall_time - idle_time) / wall_time;
    }

    /* Optimasi: Hitung Akselerasi Beban (Velocity) */
    load_velocity = (int)load - (int)info->prev_load;
    info->prev_load = load;

    /* Optimasi 1: Touch/Interaction Boost Simulation 
     * Jika ada lonjakan mendadak (>30%), asumsikan interaksi UI sedang terjadi 
     */
    if (load_velocity > 30) {
        load = (load + touch_boost_load) / 2;
        info->hold_counter = 5; // Tahan frekuensi tinggi selama 5 siklus
    }

    target_load = (policy->cpu >= 4) ? target_load_big : target_load_little;

    /* Matrix Kalkulasi Frekuensi */
    if (load >= fast_ramp_up_load) {
        freq_target = policy->max;
    } else if (load > target_load) {
        freq_target = (policy->cur * load) / target_load;
    } else if (info->hold_counter > 0) {
        freq_target = policy->cur;
        info->hold_counter--;
    } else {
        freq_target = (policy->cur * load) / target_load;
    }

    /* Clamp frekuensi */
    if (freq_target < policy->min) freq_target = policy->min;
    if (freq_target > policy->max) freq_target = policy->max;

    /* Optimasi 2: Sinkronisasi Frame Rate
     * Gunakan CPUFREQ_RELATION_H agar selalu membulatkan frekuensi ke atas (Smooth)
     */
    if (freq_target != info->target_freq) {
        info->target_freq = freq_target;
        __cpufreq_driver_target(policy, freq_target, CPUFREQ_RELATION_H);
    }

    /* Optimasi 3: Adaptive Sampling 
     * 8ms untuk responsivitas tinggi (Sync 120Hz)
     * 32ms untuk penghematan daya saat tenang
     */
    if (load > 20 || info->hold_counter > 0)
        info->next_delay_ms = 8;
    else
        info->next_delay_ms = 32;
}

static void zv_work_handler(struct work_struct *work)
{
    struct zv_policy_info *zpinfo = container_of(work, struct zv_policy_info, work.work);
    struct cpufreq_policy *policy = zpinfo->policy;

    zv_eval_freq(policy);
    schedule_delayed_work_on(policy->cpu, &zpinfo->work, msecs_to_jiffies(per_cpu(zv_info, policy->cpu).next_delay_ms));
}

static int zv_init(struct cpufreq_policy *policy)
{
    struct zv_policy_info *zpinfo;
    zpinfo = kzalloc(sizeof(*zpinfo), GFP_KERNEL);
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
        info->prev_cpu_wall = ktime_get_ns();
        info->hold_counter = 0;
        info->prev_load = 0;
        info->next_delay_ms = 8;
    }
    schedule_delayed_work_on(policy->cpu, &((struct zv_policy_info *)policy->governor_data)->work, msecs_to_jiffies(8));
    return 0;
}

static void zv_stop(struct cpufreq_policy *policy)
{
    struct zv_policy_info *zpinfo = policy->governor_data;
    if (zpinfo) cancel_delayed_work_sync(&zpinfo->work);
}

static struct cpufreq_governor gov_zixine_velocity = {
    .name		= "zixine_velocity",
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
MODULE_AUTHOR("zixine");
MODULE_DESCRIPTION("Zixine Velocity v1.1 - Smoothness Edition");
MODULE_LICENSE("GPL v2");
