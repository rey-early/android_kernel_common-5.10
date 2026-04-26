/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * P002 Kernel Extensions Framework
 *
 * Copyright (C) 2026 RapliVx X Mahiro
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/kobject.h>
#include <linux/sysfs.h>
#include <linux/string.h>
#include <linux/sched.h>
#include <linux/sched/signal.h>
#include <linux/slab.h>
#include <linux/rwlock.h>
#include <linux/p002_attributes.h>

/* Global State */
static char p002_bg_blocklist[P002_BLOCKLIST_STRLEN] = 
    "com.shopee.id,com.lazada.android,com.tokopedia.tkpd,com.ss.android.ugc.trill,com.facebook.katana";

static char restricted_apps[P002_MAX_BLOCKED][TASK_COMM_LEN];
static u8   restricted_len[P002_MAX_BLOCKED];
static int  restricted_cnt;

static struct kobject *p002_kobj;
static rwlock_t p002_lock;

/* Helpers */
static bool p002_is_restricted(const char *comm)
{
    int i;
    bool found = false;

    read_lock(&p002_lock);
    for (i = 0; i < restricted_cnt; i++) {
        if (!strncmp(comm, restricted_apps[i], restricted_len[i])) {
            found = true;
            break;
        }
    }
    read_unlock(&p002_lock);
    return found;
}

void p002_background_event(struct task_struct *task, short oom_adj)
{
    if (oom_adj >= 200) {
        if (unlikely(p002_is_restricted(task->comm))) {
            pr_info("P002: Killing restricted app '%s' (PID: %d), OOM score: %d\n", 
                    task->comm, task->pid, oom_adj);
            send_sig(SIGKILL, task, 0);
        }
    }
}
EXPORT_SYMBOL_GPL(p002_background_event);

/* Sysfs Logic */
static void p002_rebuild_blocklist(char *buf)
{
    char *token;
    int count = 0;

    write_lock(&p002_lock);
    
    while ((token = strsep(&buf, ",")) && count < P002_MAX_BLOCKED) {
        if (!*token) continue;
        strscpy(restricted_apps[count], token, TASK_COMM_LEN);
        restricted_len[count] = strlen(restricted_apps[count]);
        count++;
    }
    restricted_cnt = count;
    
    write_unlock(&p002_lock);
    pr_info("P002: Blocklist updated. Total active: %d\n", restricted_cnt);
}

static ssize_t bg_blocklist_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf)
{
    return scnprintf(buf, PAGE_SIZE, "%s\n", p002_bg_blocklist);
}

static ssize_t bg_blocklist_store(struct kobject *kobj, struct kobj_attribute *attr, const char *buf, size_t count)
{
    char tmp[P002_BLOCKLIST_STRLEN];
    
    if (count >= P002_BLOCKLIST_STRLEN)
        return -EINVAL;

    strscpy(tmp, buf, sizeof(tmp));
    strreplace(tmp, '\n', '\0');
    
    strscpy(p002_bg_blocklist, tmp, sizeof(p002_bg_blocklist));
    p002_rebuild_blocklist(tmp);
    
    return count;
}

static struct kobj_attribute bg_blocklist_attr = __ATTR(bg_blocklist, 0664, bg_blocklist_show, bg_blocklist_store);

static struct attribute *p002_attrs[] = {
    &bg_blocklist_attr.attr,
    NULL,
};

static const struct attribute_group p002_attr_group = { .attrs = p002_attrs };

/* Lifecycle */
static int __init p002_init(void)
{
    int ret;
    char tmp[P002_BLOCKLIST_STRLEN];

    pr_info("P002: Initializing Framework v2.3\n");
    rwlock_init(&p002_lock);

    strscpy(tmp, p002_bg_blocklist, sizeof(tmp));
    p002_rebuild_blocklist(tmp);

    p002_kobj = kobject_create_and_add("p002", kernel_kobj);
    if (!p002_kobj)
        return -ENOMEM;

    ret = sysfs_create_group(p002_kobj, &p002_attr_group);
    if (ret) {
        kobject_put(p002_kobj);
        return ret;
    }

    return 0;
}

static void __exit p002_exit(void)
{
    sysfs_remove_group(p002_kobj, &p002_attr_group);
    kobject_put(p002_kobj);
    pr_info("P002: Framework unloaded\n");
}

module_init(p002_init);
module_exit(p002_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("RapliVx X Mahiro");
MODULE_DESCRIPTION("P002 Framework: Universal OOM Killer");
MODULE_VERSION("2.3");
