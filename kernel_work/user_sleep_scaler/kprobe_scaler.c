#include <linux/init.h>
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/kprobes.h>
#include <linux/time.h>
#include <linux/uaccess.h>
#include <linux/ptrace.h>
#include <linux/kobject.h> // Required for sysfs
#include <linux/sysfs.h>   // Required for sysfs

// The target syscall symbol for x86_64 architecture.
#define SYSCALL_SYMBOL_NAME "__x64_sys_clock_nanosleep"

// The global scaling factor. 1000 = 1.0x (no-op).
static int time_scale_factor = 1000;

/**
 * scale_sleep_duration - Applies the scaling factor to a timespec.
 * @ts: A pointer to the timespec64 structure to be scaled.
 */
static void scale_sleep_duration(struct timespec64 *ts)
{
    u64 total_ns;

    // Convert timespec to total nanoseconds.
    total_ns = (u64)ts->tv_sec * NSEC_PER_SEC + ts->tv_nsec;

    // Apply the scaling factor using 64-bit arithmetic.
    total_ns = total_ns * time_scale_factor;
    do_div(total_ns, 1000);

    // Convert back to timespec.
    ts->tv_nsec = do_div(total_ns, NSEC_PER_SEC);
    ts->tv_sec = total_ns;
}

/**
 * handler_pre - The kprobe pre-handler.
 */
static int handler_pre(struct kprobe *p, struct pt_regs *regs)
{
    struct timespec64 kernel_ts;
    struct timespec __user *user_ts_ptr;
    struct pt_regs *user_regs;
    int flags;

    if (time_scale_factor == 1000)
        return 0;

    user_regs = (struct pt_regs *)regs->di;
    flags = (int)user_regs->si;

    if (flags & TIMER_ABSTIME)
        return 0;

    user_ts_ptr = (struct timespec __user *)user_regs->dx;

    if (copy_from_user(&kernel_ts, user_ts_ptr, sizeof(kernel_ts))) {
        pr_warn("time_scaler: Failed to copy timespec from user space\n");
        return 0;
    }

    scale_sleep_duration(&kernel_ts);

    if (copy_to_user(user_ts_ptr, &kernel_ts, sizeof(kernel_ts))) {
        pr_warn("time_scaler: Failed to write timespec to user space\n");
        return 0;
    }

    // This log can be noisy, so it's commented out by default.
    // pr_info("time_scaler: Scaled sleep for PID %d (%s) by %d/1000\n",
    //         current->pid, current->comm, time_scale_factor);

    return 0;
}

// Define the kprobe structure.
static struct kprobe kp = {
   .symbol_name = SYSCALL_SYMBOL_NAME,
   .pre_handler = handler_pre,
};

// --- sysfs implementation ---

static struct kobject *time_scaler_kobj;

/**
 * sysfs_scale_factor_show - Called when user reads /sys/kernel/time_scaler/scale_factor
 */
static ssize_t sysfs_scale_factor_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf)
{
    return sysfs_emit(buf, "%d\n", time_scale_factor);
}

/**
 * sysfs_scale_factor_store - Called when user writes to /sys/kernel/time_scaler/scale_factor
 */
static ssize_t sysfs_scale_factor_store(struct kobject *kobj, struct kobj_attribute *attr, const char *buf, size_t count)
{
    int res;
    res = kstrtoint(buf, 10, &time_scale_factor);
    if (res < 0)
        return res;

    return count;
}

// Define the attribute, linking the name "scale_factor" to our show/store functions.
static struct kobj_attribute scale_factor_attribute = __ATTR(scale_factor, 0664, sysfs_scale_factor_show, sysfs_scale_factor_store);

// --- End of sysfs implementation ---

// Module initialization function.
static int __init kprobe_scaler_init(void)
{
    int ret;
    pr_info("time_scaler: Initializing module\n");

    // Create the "time_scaler" directory in /sys/kernel/
    time_scaler_kobj = kobject_create_and_add("time_scaler", kernel_kobj);
    if (!time_scaler_kobj) {
        pr_err("time_scaler: Failed to create kobject\n");
        return -ENOMEM;
    }
    pr_info("time_scaler: Created /sys/kernel/time_scaler\n");

    // Create the "scale_factor" file in that directory.
    ret = sysfs_create_file(time_scaler_kobj, &scale_factor_attribute.attr);
    if (ret) {
        pr_err("time_scaler: Failed to create sysfs file\n");
        kobject_put(time_scaler_kobj);
        return ret;
    }
    pr_info("time_scaler: Created /sys/kernel/time_scaler/scale_factor\n");

    // Register the kprobe.
    ret = register_kprobe(&kp);
    if (ret < 0) {
        pr_err("time_scaler: register_kprobe failed, returned %d\n", ret);
        sysfs_remove_file(time_scaler_kobj, &scale_factor_attribute.attr);
        kobject_put(time_scaler_kobj);
        return ret;
    }
    pr_info("time_scaler: Probe planted on %s\n", SYSCALL_SYMBOL_NAME);

    return 0;
}

// Module cleanup function.
static void __exit kprobe_scaler_exit(void)
{
    unregister_kprobe(&kp);
    sysfs_remove_file(time_scaler_kobj, &scale_factor_attribute.attr);
    kobject_put(time_scaler_kobj);
    pr_info("time_scaler: Exiting module\n");
}

module_init(kprobe_scaler_init);
module_exit(kprobe_scaler_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Award-Winning Writer");
MODULE_DESCRIPTION("A kprobe module to scale user-space sleep durations via sysfs.");