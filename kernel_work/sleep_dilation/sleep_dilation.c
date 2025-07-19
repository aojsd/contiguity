#include <linux/init.h>
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/kprobes.h>
#include <linux/sched.h>
#include <linux/time.h>
#include <linux/kobject.h>
#include <linux/sysfs.h>
#include <linux/hrtimer.h>

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Your Name");
MODULE_DESCRIPTION("A module to dilate sleep timers");
MODULE_VERSION("0.1");

// Global variable for the dilation factor, exposed via sysfs
static int sleep_dilation_factor = 1;

// --- Sysfs setup ---

static struct kobject *sleep_dilation_kobj;

// The 'show' function is called when the sysfs file is read.
static ssize_t sleep_dilation_factor_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf)
{
    return sprintf(buf, "%d\n", sleep_dilation_factor);
}

// The 'store' function is called when the sysfs file is written to.
static ssize_t sleep_dilation_factor_store(struct kobject *kobj, struct kobj_attribute *attr, const char *buf, size_t count)
{
    int res = kstrtoint(buf, 10, &sleep_dilation_factor);
    if (res < 0)
        return res;

    if (sleep_dilation_factor < 1) {
        pr_warn("sleep_dilation_factor cannot be less than 1. Setting to 1.\n");
        sleep_dilation_factor = 1;
    }

    pr_info("sleep_dilation_factor set to %d\n", sleep_dilation_factor);
    return count;
}

// Define the sysfs attribute
static struct kobj_attribute sleep_dilation_factor_attr = __ATTR(sleep_dilation_factor, 0664, sleep_dilation_factor_show, sleep_dilation_factor_store);

// --- kprobe for schedule_timeout ---

static int handler_pre_sched_timeout(struct kprobe *p, struct pt_regs *regs)
{
    long timeout = (long)regs->di; // On x86_64, first arg is in rdi (%di in pt_regs)

    pr_info("schedule_timeout called by %s (PID %d) with timeout %ld jiffies.\n",
            current->comm, current->pid, timeout);

    unsigned int curr_state = get_current_state();
    if (curr_state & TASK_INTERRUPTIBLE) {
        pr_info("INTERRUPTIBLE: schedule_timeout called by %s (PID %d) with timeout %ld jiffies.\n",
            current->comm, current->pid, timeout);

        /*
         * TODO: Enable this block to modify the timeout.
         * if (sleep_dilation_factor > 1) {
         *     long new_timeout = timeout * sleep_dilation_factor;
         *     pr_info("Dilating timeout from %ld to %ld\n", timeout, new_timeout);
         *     regs->di = new_timeout;
         * }
         */
    } else if (curr_state & TASK_UNINTERRUPTIBLE) {
        pr_info("UNINTERRUPTIBLE (not modifying): schedule_timeout called by %s (PID %d) with timeout %ld jiffies.\n",
            current->comm, current->pid, timeout);
    } else {
        pr_info("State %d (not modifying): schedule_timeout called by %s (PID %d) with timeout %ld jiffies.\n",
            curr_state, current->comm, current->pid, timeout);
    }

    return 0; // Always return 0 to continue original function
}

static struct kprobe kp_sched_timeout = {
   .symbol_name = "schedule_timeout",
   .pre_handler = handler_pre_sched_timeout,
};

// --- kprobe for hrtimer_nanosleep ---

static int handler_pre_hrtimer_nanosleep(struct kprobe *p, struct pt_regs *regs)
{
    // On x86_64, the first three arguments are passed via registers:
    // 1st arg (rqtp):    %rdi -> regs->di
    // 2nd arg (mode):    %rsi -> regs->si
    // 3rd arg (clockid): %rdx -> regs->dx
    ktime_t rqtp = (ktime_t)regs->di;
    enum hrtimer_mode mode = (enum hrtimer_mode)regs->si;
    clockid_t clockid = (clockid_t)regs->dx;

    pr_info("User: hrtimer_nanosleep called by %s (PID %d) for %lld ns, mode: %d, clockid: %d\n",
            current->comm, current->pid, (long long)rqtp, mode, clockid);

    /*
     * TODO: Enable this block to modify the timeout.
     * We are directly modifying the register value that holds the ktime_t.
     */
    // if (sleep_dilation_factor > 1) {
    //     ktime_t new_rqtp = rqtp * sleep_dilation_factor;

    //     pr_info("Dilating hrtimer sleep from %lld ns to %lld ns\n",
    //             (long long)rqtp, (long long)new_rqtp);

    //     regs->di = (unsigned long)new_rqtp;
    // }

    return 0; // Always return 0 to continue original function
}

static struct kprobe kp_hrtimer_nanosleep = {
   .symbol_name = "hrtimer_nanosleep",
   .pre_handler = handler_pre_hrtimer_nanosleep,
};

// --- Module Init and Exit ---

static int __init sleep_dilation_init(void)
{
    int ret;

    pr_info("Sleep Dilation Module: Initializing\n");

    // 1. Create sysfs directory /sys/kernel/sleep_dilation
    sleep_dilation_kobj = kobject_create_and_add("sleep_dilation", kernel_kobj);
    if (!sleep_dilation_kobj) {
        pr_err("Failed to create kobject\n");
        return -ENOMEM;
    }

    // 2. Create sysfs file /sys/kernel/sleep_dilation/sleep_dilation_factor
    ret = sysfs_create_file(sleep_dilation_kobj, &sleep_dilation_factor_attr.attr);
    if (ret) {
        pr_err("Failed to create sysfs file\n");
        kobject_put(sleep_dilation_kobj);
        return ret;
    }

    // 3. Register kprobes
    ret = register_kprobe(&kp_sched_timeout);
    if (ret < 0) {
        pr_err("register_kprobe for schedule_timeout failed, returned %d\n", ret);
        sysfs_remove_file(sleep_dilation_kobj, &sleep_dilation_factor_attr.attr);
        kobject_put(sleep_dilation_kobj);
        return ret;
    }
    pr_info("Planted kprobe at %p\n", kp_sched_timeout.addr);

    ret = register_kprobe(&kp_hrtimer_nanosleep);
    if (ret < 0) {
        pr_err("register_kprobe for hrtimer_nanosleep failed, returned %d\n", ret);
        unregister_kprobe(&kp_sched_timeout);
        sysfs_remove_file(sleep_dilation_kobj, &sleep_dilation_factor_attr.attr);
        kobject_put(sleep_dilation_kobj);
        return ret;
    }
    pr_info("Planted kprobe at %p\n", kp_hrtimer_nanosleep.addr);

    return 0;
}

static void __exit sleep_dilation_exit(void)
{
    unregister_kprobe(&kp_hrtimer_nanosleep);
    pr_info("kprobe hrtimer_nanosleep unregistered\n");

    unregister_kprobe(&kp_sched_timeout);
    pr_info("kprobe schedule_timeout unregistered\n");

    sysfs_remove_file(sleep_dilation_kobj, &sleep_dilation_factor_attr.attr);
    kobject_put(sleep_dilation_kobj);
    pr_info("Sysfs components removed\n");

    pr_info("Sleep Dilation Module: Exiting\n");
}

module_init(sleep_dilation_init);
module_exit(sleep_dilation_exit);