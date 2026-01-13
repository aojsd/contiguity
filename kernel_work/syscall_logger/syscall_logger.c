#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/kprobes.h>
#include <linux/ktime.h>
#include <linux/slab.h>
#include <linux/hashtable.h>
#include <linux/spinlock.h>
#include <linux/ptrace.h>

#define PROBE_SYMBOL "x64_sys_call"
#define HASH_TABLE_BITS 10

/* The object we will store in our hash table */
struct syscall_entry {
    pid_t tid;
    long long syscall_id;
    ktime_t start_time;
    struct hlist_node node;
};

/* Globals for the hash table, lock, and slab allocator */
static DEFINE_HASHTABLE(syscall_table, HASH_TABLE_BITS);
static DEFINE_SPINLOCK(syscall_lock);
static struct kmem_cache *entry_cache;


static int entry_handler(struct kretprobe_instance *ri, struct pt_regs *regs)
{
    struct syscall_entry *entry;
    struct pt_regs *syscall_regs;
    pid_t tid = current->pid;

    entry = kmem_cache_alloc(entry_cache, GFP_ATOMIC);
    if (!entry) {
        pr_warn_ratelimited("kmem_cache_alloc failed\n");
        return 1;
    }

    /*
     * CORRECTED: The first argument to the probed function (__x64_sys_call)
     * is a pointer to the actual syscall's pt_regs. This argument is in
     * the RDI register, which corresponds to 'regs->di'.
     */
    syscall_regs = (struct pt_regs *)regs->di;
    entry->syscall_id = syscall_regs->orig_ax;

    entry->tid = tid;
    entry->start_time = ktime_get();

    spin_lock(&syscall_lock);
    hash_add(syscall_table, &entry->node, tid);
    spin_unlock(&syscall_lock);

    return 0;
}

static int ret_handler(struct kretprobe_instance *ri, struct pt_regs *regs)
{
    struct syscall_entry *entry;
    ktime_t end_time;
    s64 duration_ns;
    pid_t tid = current->pid;

    spin_lock(&syscall_lock);

    hash_for_each_possible(syscall_table, entry, node, tid) {
        if (entry->tid == tid) {
            end_time = ktime_get();
            duration_ns = ktime_to_ns(ktime_sub(end_time, entry->start_time));

            pr_info("TID %d: Syscall %lld took %lld ns (Ret=0x%llx)\n",
                    tid, entry->syscall_id, duration_ns, regs->ax);

            hash_del(&entry->node);
            kmem_cache_free(entry_cache, entry);
            break;
        }
    }

    spin_unlock(&syscall_lock);
    return 0;
}

static struct kretprobe my_kretprobe = {
    .kp.symbol_name = PROBE_SYMBOL,
    .entry_handler  = entry_handler,
    .handler        = ret_handler,
    .maxactive      = 2048,
};

static int __init kprobe_init(void)
{
    int ret;

    entry_cache = kmem_cache_create("syscall_entry_cache", sizeof(struct syscall_entry),
                                    0, SLAB_PANIC | SLAB_ACCOUNT, NULL);

    ret = register_kretprobe(&my_kretprobe);
    if (ret < 0) {
        pr_err("register_kretprobe failed, returned %d\n", ret);
        kmem_cache_destroy(entry_cache);
        return ret;
    }
    pr_info("Syscall logger registered for %s\n", my_kretprobe.kp.symbol_name);
    return 0;
}

static void __exit kprobe_exit(void)
{
    struct syscall_entry *entry;
    struct hlist_node *tmp;
    int bkt;

    unregister_kretprobe(&my_kretprobe);

    spin_lock(&syscall_lock);
    hash_for_each_safe(syscall_table, bkt, tmp, entry, node) {
        hash_del(&entry->node);
        kmem_cache_free(entry_cache, entry);
    }
    spin_unlock(&syscall_lock);

    kmem_cache_destroy(entry_cache);

    pr_info("Syscall logger unregistered\n");
}

module_init(kprobe_init);
module_exit(kprobe_exit);
MODULE_LICENSE("GPL");
MODULE_AUTHOR("Your Name");
MODULE_DESCRIPTION("Per-thread syscall timing using a hash table");