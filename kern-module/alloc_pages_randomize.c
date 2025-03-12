#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/mm.h>
#include <linux/gfp.h>
#include <linux/list.h>
#include <linux/slab.h>
#include <linux/init.h>
#include <linux/types.h>

static unsigned long total_gb = 1; // Default: 1GB
static unsigned int immovable_pct = 50; // Default: 50%
module_param(total_gb, ulong, 0644);
module_param(immovable_pct, uint, 0644);
MODULE_PARM_DESC(total_gb, "Total memory to allocate in GB");
MODULE_PARM_DESC(immovable_pct, "Percentage of memory that should be immovable");

struct page_entry {
    struct list_head list;
    struct page *page;
};

static LIST_HEAD(page_list);
static unsigned long allocated_immovable = 0;
static unsigned long allocated_movable = 0;
static unsigned long allocated_pages = 0;

static int __init alloc_pages_test_init(void)
{
    unsigned long total_pages = total_gb << (30 - PAGE_SHIFT); // Convert GB to pages
    unsigned long immovable_pages = (total_pages * immovable_pct) / 100;
    unsigned long movable_pages = total_pages - immovable_pages;
    unsigned long i;
    struct page_entry *entry;

    printk(KERN_INFO "Allocating %lu GB (%lu pages): %lu immovable, %lu movable\n", 
           total_gb, total_pages, immovable_pages, movable_pages);

    // Allocate immovable pages (GFP_KERNEL)
    for (i = 0; i < immovable_pages; i++) {
        struct page *page = alloc_pages(GFP_KERNEL, 0);
        if (!page)
            break;

        entry = kmalloc(sizeof(*entry), GFP_KERNEL);
        if (!entry) {
            __free_page(page);
            break;
        }
        entry->page = page;
        list_add(&entry->list, &page_list);
        allocated_immovable++;
    }

    // Allocate movable pages (GFP_HIGHUSER_MOVABLE)
    for (i = 0; i < movable_pages; i++) {
        struct page *page = alloc_pages(GFP_HIGHUSER_MOVABLE, 0);
        if (!page)
            break;

        entry = kmalloc(sizeof(*entry), GFP_KERNEL);
        if (!entry) {
            __free_page(page);
            break;
        }
        entry->page = page;
        list_add(&entry->list, &page_list);
        allocated_movable++;
    }
    allocated_pages = allocated_immovable + allocated_movable;
    printk(KERN_INFO "Successfully allocated %lu pages (%lu immovable, %lu movable)\n",
            allocated_pages, allocated_immovable, allocated_movable);
    return 0;
}

static void __exit alloc_pages_test_exit(void)
{
    struct page_entry *entry, *tmp;
    list_for_each_entry_safe(entry, tmp, &page_list, list) {
        __free_page(entry->page);
        list_del(&entry->list);
        kfree(entry);
    }
    printk(KERN_INFO "Freed %lu pages\n", allocated_pages);
}

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Michael Wu, ChatGPT");
MODULE_DESCRIPTION("Kernel module to allocate movable and immovable pages");

module_init(alloc_pages_test_init);
module_exit(alloc_pages_test_exit);
