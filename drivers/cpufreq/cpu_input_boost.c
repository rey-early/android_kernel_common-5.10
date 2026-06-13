// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2018-2019 Sultan Alsawaf <sultan@kerneltoast.com>.
 * Ported to GKI 5.10 and maintained by rinnsakaguchi.
 *
 * P002 Kernel Extensions Framework
 */

#define pr_fmt(fmt) "cpu_input_boost: %s: " fmt, __func__

#include <linux/cpu.h>
#include <linux/cpufreq.h>
#include <linux/input.h>
#include <linux/kthread.h>
#include <linux/moduleparam.h>
#include <linux/slab.h>
#include <linux/version.h>
#include <linux/pm_qos.h>
#include <linux/suspend.h>
#include <linux/topology.h>
#include <linux/workqueue.h>
#include <uapi/linux/sched/types.h>

static unsigned short input_boost_duration = 200; // Default values for test
static unsigned int wake_boost_duration = 500;
static unsigned int input_boost_freq_lp = 0;
static unsigned int input_boost_freq_hp = 0;
static unsigned int min_freq_lp = 0;
static unsigned int min_freq_hp = 0;

module_param(input_boost_duration, short, 0644);
module_param(wake_boost_duration, uint, 0644);
module_param(input_boost_freq_lp, uint, 0644);
module_param(input_boost_freq_hp, uint, 0644);
module_param(min_freq_lp, uint, 0644);
module_param(min_freq_hp, uint, 0644);

enum {
	SCREEN_OFF,
	INPUT_BOOST,
	MAX_BOOST
};

struct boost_drv {
	struct delayed_work input_unboost;
	struct delayed_work max_unboost;
	struct notifier_block cpu_notif;
	struct notifier_block pm_notif;
	wait_queue_head_t boost_waitq;
	atomic_long_t max_boost_expires;
	unsigned long state;
	unsigned long last_input_jiffies;
};

static DEFINE_PER_CPU(struct freq_qos_request, boost_qos_req);
static DEFINE_PER_CPU(bool, boost_qos_added);

static void input_unboost_worker(struct work_struct *work);
static void max_unboost_worker(struct work_struct *work);

static struct boost_drv boost_drv_g __read_mostly = {
	.input_unboost = __DELAYED_WORK_INITIALIZER(boost_drv_g.input_unboost,
						    input_unboost_worker, 0),
	.max_unboost = __DELAYED_WORK_INITIALIZER(boost_drv_g.max_unboost,
						  max_unboost_worker, 0),
	.boost_waitq = __WAIT_QUEUE_HEAD_INITIALIZER(boost_drv_g.boost_waitq)
};

/* Determine cluster type dynamically based on CPU capacity */
static inline bool is_lp_cpu(unsigned int cpu)
{
	/* CPUs with capacity less than 1024 are typically Little cores */
	return arch_scale_cpu_capacity(cpu) < 1024;
}

static void update_online_cpu_policy(void)
{
	struct boost_drv *b = &boost_drv_g;
	unsigned int cpu, target_freq;

	cpus_read_lock();
	for_each_possible_cpu(cpu) {
		if (!per_cpu(boost_qos_added, cpu))
			continue;

		if (test_bit(SCREEN_OFF, &b->state)) {
			target_freq = 0; /* Let schedutil handle minimums */
		} else if (test_bit(MAX_BOOST, &b->state)) {
			target_freq = INT_MAX; /* Request max available freq */
		} else if (test_bit(INPUT_BOOST, &b->state)) {
			target_freq = is_lp_cpu(cpu) ? input_boost_freq_lp : input_boost_freq_hp;
		} else {
			target_freq = is_lp_cpu(cpu) ? min_freq_lp : min_freq_hp;
		}

		freq_qos_update_request(&per_cpu(boost_qos_req, cpu), target_freq);
	}
	cpus_read_unlock();
}

bool cpu_input_boost_within_input(unsigned long timeout_ms)
{
	struct boost_drv *b = &boost_drv_g;

	return time_before(jiffies, b->last_input_jiffies +
			   msecs_to_jiffies(timeout_ms));
}

static void __cpu_input_boost_kick(struct boost_drv *b)
{
	if (test_bit(SCREEN_OFF, &b->state) || (input_boost_duration == 0))
		return;

	set_bit(INPUT_BOOST, &b->state);
	if (!mod_delayed_work(system_unbound_wq, &b->input_unboost,
			      msecs_to_jiffies(input_boost_duration)))
		wake_up(&b->boost_waitq);
}

void cpu_input_boost_kick(void)
{
	struct boost_drv *b = &boost_drv_g;
	__cpu_input_boost_kick(b);
}

static void __cpu_input_boost_kick_max(struct boost_drv *b,
				       unsigned int duration_ms)
{
	unsigned long boost_jiffies = msecs_to_jiffies(duration_ms);
	unsigned long curr_expires, new_expires;

	if (test_bit(SCREEN_OFF, &b->state))
		return;

	do {
		curr_expires = atomic_long_read(&b->max_boost_expires);
		new_expires = jiffies + boost_jiffies;

		/* Skip this boost if there is a longer boost in effect */
		if (time_after(curr_expires, new_expires))
			return;
	} while (atomic_long_cmpxchg(&b->max_boost_expires, curr_expires,
				     new_expires) != curr_expires);

	set_bit(MAX_BOOST, &b->state);
	if (!mod_delayed_work(system_unbound_wq, &b->max_unboost,
			      boost_jiffies))
		wake_up(&b->boost_waitq);
}

void cpu_input_boost_kick_max(unsigned int duration_ms)
{
	struct boost_drv *b = &boost_drv_g;
	__cpu_input_boost_kick_max(b, duration_ms);
}

static void input_unboost_worker(struct work_struct *work)
{
	struct boost_drv *b = container_of(to_delayed_work(work),
					   typeof(*b), input_unboost);

	clear_bit(INPUT_BOOST, &b->state);
	wake_up(&b->boost_waitq);
}

static void max_unboost_worker(struct work_struct *work)
{
	struct boost_drv *b = container_of(to_delayed_work(work),
					   typeof(*b), max_unboost);

	clear_bit(MAX_BOOST, &b->state);
	wake_up(&b->boost_waitq);
}

static int cpu_boost_thread(void *data)
{
	struct boost_drv *b = data;
	unsigned long old_state = 0;

	/* Set thread to real-time priority */
	sched_set_fifo(current);

	while (1) {
		bool should_stop = false;
		unsigned long curr_state;

		wait_event(b->boost_waitq,
			(curr_state = READ_ONCE(b->state)) != old_state ||
			(should_stop = kthread_should_stop()));

		if (should_stop)
			break;

		old_state = curr_state;
		update_online_cpu_policy();
	}

	return 0;
}

static int cpu_notifier_cb(struct notifier_block *nb, unsigned long action,
			   void *data)
{
	struct cpufreq_policy *policy = data;
	int ret;

	/* Bind Frequency QoS request during policy creation */
	if (action == CPUFREQ_CREATE_POLICY) {
		if (!per_cpu(boost_qos_added, policy->cpu)) {
			ret = freq_qos_add_request(&policy->constraints,
						   &per_cpu(boost_qos_req, policy->cpu),
						   FREQ_QOS_MIN, 0);
			if (ret < 0) {
				pr_err("Failed to add QoS request for CPU%d: %d\n", policy->cpu, ret);
				return NOTIFY_BAD;
			}
			per_cpu(boost_qos_added, policy->cpu) = true;
			pr_debug("QoS request added for CPU%d\n", policy->cpu);
		}
	} else if (action == CPUFREQ_REMOVE_POLICY) {
		if (per_cpu(boost_qos_added, policy->cpu)) {
			freq_qos_remove_request(&per_cpu(boost_qos_req, policy->cpu));
			per_cpu(boost_qos_added, policy->cpu) = false;
			pr_debug("QoS request removed for CPU%d\n", policy->cpu);
		}
	}

	return NOTIFY_OK;
}

static int pm_notifier_cb(struct notifier_block *nb, unsigned long action,
			  void *data)
{
	struct boost_drv *b = container_of(nb, typeof(*b), pm_notif);

	switch (action) {
	case PM_SUSPEND_PREPARE:
		/* Device is going to sleep */
		set_bit(SCREEN_OFF, &b->state);
		wake_up(&b->boost_waitq);
		break;
	case PM_POST_SUSPEND:
		/* Device is waking up */
		clear_bit(SCREEN_OFF, &b->state);
		__cpu_input_boost_kick_max(b, wake_boost_duration);
		break;
	}

	return NOTIFY_OK;
}

static void cpu_input_boost_input_event(struct input_handle *handle,
					unsigned int type, unsigned int code,
					int value)
{
	struct boost_drv *b = handle->handler->private;

	__cpu_input_boost_kick(b);
	b->last_input_jiffies = jiffies;
}

static int cpu_input_boost_input_connect(struct input_handler *handler,
					 struct input_dev *dev,
					 const struct input_device_id *id)
{
	struct input_handle *handle;
	int ret;

	handle = kzalloc(sizeof(*handle), GFP_KERNEL);
	if (!handle)
		return -ENOMEM;

	handle->dev = dev;
	handle->handler = handler;
	handle->name = "cpu_input_boost_handle";

	ret = input_register_handle(handle);
	if (ret)
		goto free_handle;

	ret = input_open_device(handle);
	if (ret)
		goto unregister_handle;

	return 0;

unregister_handle:
	input_unregister_handle(handle);
free_handle:
	kfree(handle);
	return ret;
}

static void cpu_input_boost_input_disconnect(struct input_handle *handle)
{
	input_close_device(handle);
	input_unregister_handle(handle);
	kfree(handle);
}

static const struct input_device_id cpu_input_boost_ids[] = {
	/* Multi-touch touchscreen */
	{
		.flags = INPUT_DEVICE_ID_MATCH_EVBIT | INPUT_DEVICE_ID_MATCH_ABSBIT,
		.evbit = { BIT_MASK(EV_ABS) },
		.absbit = { [BIT_WORD(ABS_MT_POSITION_X)] = BIT_MASK(ABS_MT_POSITION_X) | BIT_MASK(ABS_MT_POSITION_Y) }
	},
	/* Touchpad */
	{
		.flags = INPUT_DEVICE_ID_MATCH_KEYBIT | INPUT_DEVICE_ID_MATCH_ABSBIT,
		.keybit = { [BIT_WORD(BTN_TOUCH)] = BIT_MASK(BTN_TOUCH) },
		.absbit = { [BIT_WORD(ABS_X)] = BIT_MASK(ABS_X) | BIT_MASK(ABS_Y) }
	},
	/* Keypad & Power Key */
	{
		.flags = INPUT_DEVICE_ID_MATCH_EVBIT,
		.evbit = { BIT_MASK(EV_KEY) }
	},
	{ }
};

static struct input_handler cpu_input_boost_input_handler = {
	.event		= cpu_input_boost_input_event,
	.connect	= cpu_input_boost_input_connect,
	.disconnect	= cpu_input_boost_input_disconnect,
	.name		= "cpu_input_boost_handler",
	.id_table	= cpu_input_boost_ids
};

static int __init cpu_input_boost_init(void)
{
	struct boost_drv *b = &boost_drv_g;
	struct task_struct *thread;
	int ret;

	b->cpu_notif.notifier_call = cpu_notifier_cb;
	ret = cpufreq_register_notifier(&b->cpu_notif, CPUFREQ_POLICY_NOTIFIER);
	if (ret) {
		pr_err("Failed to register cpufreq notifier: %d\n", ret);
		return ret;
	}

	cpu_input_boost_input_handler.private = b;
	ret = input_register_handler(&cpu_input_boost_input_handler);
	if (ret) {
		pr_err("Failed to register input handler: %d\n", ret);
		goto unregister_cpu_notif;
	}

	b->last_input_jiffies = jiffies;

	b->pm_notif.notifier_call = pm_notifier_cb;
	ret = register_pm_notifier(&b->pm_notif);
	if (ret) {
		pr_err("Failed to register PM notifier: %d\n", ret);
		goto unregister_handler;
	}

	thread = kthread_run(cpu_boost_thread, b, "cpu_boostd");
	if (IS_ERR(thread)) {
		ret = PTR_ERR(thread);
		pr_err("Failed to start CPU boost thread: %d\n", ret);
		goto unregister_pm_notif;
	}

	pr_debug("Successfully initialized\n");
	return 0;

unregister_pm_notif:
	unregister_pm_notifier(&b->pm_notif);
unregister_handler:
	input_unregister_handler(&cpu_input_boost_input_handler);
unregister_cpu_notif:
	cpufreq_unregister_notifier(&b->cpu_notif, CPUFREQ_POLICY_NOTIFIER);
	return ret;
}
subsys_initcall(cpu_input_boost_init);
