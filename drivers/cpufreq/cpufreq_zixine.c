// SPDX-License-Identifier: GPL-2.0
/*
 * Zixine Velocity v1.1 - Smoothness Edition (Fixed & Optimized)
 * Optimasi: Touch Boost Simulation, IO-Wait Sensitivity, & Frame-Sync Sampling
 * Author: zixine
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
static unsigned int target_load_big = 60;
static unsigned int target_load_little = 70; 
static unsigned int fast_ramp_up_load = 80;  
static unsigned int touch_boost_load = 75;
static struct workqueue_struct *zv_wq;

/* Ekspos variabel ke /sys/module/cpufreq_zixine/parameters/ */
module_param(target_load_big, uint, 0644);
MODULE_PARM_DESC(target_load_big, "Target load untuk core Big (default: 60)");

module_param(target_load_little, uint, 0644);
MODULE_PARM_DESC(target_load_little, "Target load untuk core Little (default: 70)");

module_param(fast_ramp_up_load, uint, 0644);
MODULE_PARM_DESC(fast_ramp_up_load, "Batas beban untuk frekuensi maksimal instan (default: 80)");

module_param(touch_boost_load, uint, 0644);
MODULE_PARM_DESC(touch_boost_load, "Simulasi beban saat touch boost (default: 75)");

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

/** CORE LOGIC: VELOCITY-BASED SCALING (FIXED UNIT VERSION) **/
static void zv_eval_freq(struct cpufreq_policy *policy)
{
    struct zv_cpu_info *info = &per_cpu(zv_info, policy->cpu);
    u64 cur_wall, cur_idle, wall_time, idle_time;
    unsigned int load, freq_target, target_load;
    int load_velocity;

    /* Konversi Nanodetik ke Mikrodetik agar sinkron dengan get_cpu_idle_time */
    cur_wall = ktime_get_ns() / 1000;
    cur_idle = get_cpu_idle_time(policy->cpu, &cur_wall, 1); // 1 = IO Wait Aware

    wall_time = cur_wall - info->prev_cpu_wall;
    idle_time = cur_idle - info->prev_cpu_idle;

    info->prev_cpu_wall = cur_wall;
    info->prev_cpu_idle = cur_idle;

    if (unlikely(!wall_time || wall_time <= idle_time)) {
        load = 0;
    } else {
        /* Rumus Beban CPU akurat dalam mikrodetik */
        load = 100 * (wall_time - idle_time) / wall_time;
    }

    if (load > 100) load = 100;

    /* Optimasi: Hitung Akselerasi Beban (Velocity) */
    load_velocity = (int)load - (int)info->prev_load;
    info->prev_load = load;

    /* Optimasi 1: Touch/Interaction Boost Simulation */
    if (load_velocity > 30) {
        load = (load + touch_boost_load) / 2;
        info->hold_counter = 5; // Tahan frekuensi tinggi selama 5 siklus
    }

    /* Tentukan target load berdasarkan cluster (Little < 4, Big >= 4) */
    target_load = (policy->cpu >= 4) ? target_load_big : target_load_little;

    /* Matrix Kalkulasi Frekuensi dengan Proteksi Overflow (u64 casting) */
    if (load >= fast_ramp_up_load) {
        freq_target = policy->max;
    } else if (info->hold_counter > 0) {
        /* Tahan frekuensi saat ini jika dalam masa hold interaction */
        freq_target = (policy->cur > info->target_freq) ? policy->cur : info->target_freq;
        info->hold_counter--;
    } else {
        /* Skala dinamis berdasarkan policy->max agar stabil di semua clock speed */
        freq_target = (unsigned int)(((u64)policy->max * load) / target_load);
    }

    /* Clamp frekuensi agar tetap di dalam batas policy device */
    if (freq_target < policy->min) freq_target = policy->min;
    if (freq_target > policy->max) freq_target = policy->max;

    /* Sinkronisasi & Fast Switching */
    if (freq_target != info->target_freq) {
        info->target_freq = freq_target;
        
        if (policy->fast_switch_enabled) {
            cpufreq_driver_fast_switch(policy, freq_target);
        } else {
            __cpufreq_driver_target(policy, freq_target, CPUFREQ_RELATION_H);
        }
    }

    /* Adaptive Sampling: 8ms (Sync 120Hz) vs 32ms (Power Save) */
    info->next_delay_ms = (load > 20 || info->hold_counter > 0) ? 8 : 32;
}

static void zv_work_handler(struct work_struct *work)
{
    struct zv_policy_info *zpinfo = container_of(work, struct zv_policy_info, work.work);
    struct cpufreq_policy *policy = zpinfo->policy;

    zv_eval_freq(policy);

    /* Menggunakan zv_wq (High-Priority Workqueue) yang sudah dialokasikan di init */
    queue_delayed_work_on(policy->cpu, zv_wq, &zpinfo->work, 
                          msecs_to_jiffies(per_cpu(zv_info, policy->cpu).next_delay_ms));
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
    
    cpufreq_enable_fast_switch(policy);

    for_each_cpu(cpu, policy->cpus) {
        struct zv_cpu_info *info = &per_cpu(zv_info, cpu);
        /* Inisialisasi waktu awal dalam mikrodetik */
        info->prev_cpu_wall = ktime_get_ns() / 1000;
        info->prev_cpu_idle = get_cpu_idle_time(cpu, &info->prev_cpu_wall, 1);
        info->hold_counter = 0;
        info->prev_load = 0;
        info->next_delay_ms = 8;
        info->target_freq = policy->cur;
    }

    /* Panggil work pertama kali lewat High-Priority Workqueue */
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

#ifdef CONFIG_CPU_FREQ_DEFAULT_GOV_ZIXINE_VELOCITY
struct cpufreq_governor *cpufreq_default_governor(void)
{
    return &gov_zixine_velocity;
}
#endif

static int __init zv_gov_init(void)
{
    int err;

    /* 1. Alokasi High-Priority Workqueue khusus Zixine Velocity */
    zv_wq = alloc_workqueue("zv_wq", WQ_HIGHPRI | WQ_FREEZABLE, 0);
    if (!zv_wq) {
        pr_err("Zixine Velocity: Gagal mengalokasikan workqueue!\n");
        return -ENOMEM;
    }

    /* 2. Registrasi Governor ke sistem CPUFreq */
    err = cpufreq_register_governor(&gov_zixine_velocity);
    if (err) {
        pr_err("Zixine Velocity: Gagal mendaftarkan governor!\n");
        destroy_workqueue(zv_wq);
        return err;
    }

    pr_info("Zixine Velocity Governor v1.1 Loaded - Smoothness Edition (Extreme)!\n");
    return 0;
}

static void __exit zv_gov_exit(void)
{
    cpufreq_unregister_governor(&gov_zixine_velocity);
    if (zv_wq) {
        destroy_workqueue(zv_wq);
    }
    pr_info("Zixine Velocity Governor Unloaded.\n");
}

module_init(zv_gov_init);
module_exit(zv_gov_exit);
MODULE_AUTHOR("zixine");
MODULE_DESCRIPTION("Zixine Velocity v1.1 - Smoothness Edition");
MODULE_LICENSE("GPL v2");
