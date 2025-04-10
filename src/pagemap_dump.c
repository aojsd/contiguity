#define _XOPEN_SOURCE 700
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>
#include <unistd.h>
#include <cassert>

#include "pmap.h"

/* Parse the pagemap entry for the given virtual address.
 *
 * @param[out] entry      the parsed entry
 * @param[in]  pagemap_fd file descriptor to an open /proc/pid/pagemap file
 * @param[in]  vaddr      virtual address to get entry for
 * @return 0 for success, 1 for failure
 */
int pagemap_get_entry(PagemapEntry *entry, uintptr_t vaddr, int pagemap_fd, int kflags_fd)
{
    size_t nread;
    ssize_t ret;
    uint64_t data;

    nread = 0;
    while (nread < sizeof(data)) {
        ret = pread(pagemap_fd, ((uint8_t*)&data) + nread, sizeof(data) - nread,
                (vaddr / sysconf(_SC_PAGE_SIZE)) * sizeof(data) + nread);
        nread += ret;
        if (ret <= 0) {
            return 1;
        }
    }
    entry->pfn = data & (((uint64_t)1 << 55) - 1);
    entry->soft_dirty = (data >> 55) & 1;
    entry->file_page = (data >> 61) & 1;
    entry->swapped = (data >> 62) & 1;
    entry->present = (data >> 63) & 1;

    // Find kpageflags entry
    uint64_t page_flags;
    nread = 0;
    while (nread < sizeof(page_flags)) {
        ret = pread(kflags_fd, ((uint8_t*)&page_flags) + nread, sizeof(page_flags) - nread,
                (entry->pfn * sizeof(page_flags)) + nread);
        nread += ret;
        if (ret <= 0) {
            return 1;
        }
    }
    entry->thp = (page_flags >> 22) & 1;
    entry->hugetlb = (page_flags >> 17) & 1;
    return 0;
}

/* Convert the given virtual address to physical using an already opened /proc/PID/pagemap file descriptor.
 *
 * @param[out] paddr      physical address
 * @param[in]  pagemap_fd file descriptor to an open /proc/PID/pagemap file
 * @param[in]  vaddr      virtual address to get entry for
 * @return 0 for success, 1 for failure
 */
int virt_to_phys_user(uintptr_t *paddr, uintptr_t vaddr, int pagemap_fd, int kflags_fd)
{
    PagemapEntry entry;
    if (pagemap_get_entry(&entry, vaddr, pagemap_fd, kflags_fd)) {
        return 1;
    }

    // Check for mapped virtual page
    if (entry.present == 1) {
        *paddr = (entry.pfn * sysconf(_SC_PAGE_SIZE)) + (vaddr % sysconf(_SC_PAGE_SIZE));
        *paddr += entry.thp;
    }
    else {
        *paddr = 0;
    }

    // Check for hugepage
    if (entry.thp || entry.hugetlb) {
        uint64_t huge_mask = (1 << 9) - 1;
        assert((entry.pfn & huge_mask) == ((vaddr >> 12) & huge_mask));
    }
    return 0;
}

int parse_all(int argc, char **argv)
{
    char buffer[BUFSIZ];
    char maps_file[BUFSIZ];
    char pagemap_file[BUFSIZ];
    char kpageflags_file[BUFSIZ];
    int maps_fd;
    int offset = 0;
    int pagemap_fd;
    int kflags_fd;
    pid_t pid;

    if (argc < 2) {
        printf("Usage: %s pid\n", argv[0]);
        return EXIT_FAILURE;
    }
    pid = strtoull(argv[1], NULL, 0);
    snprintf(maps_file, sizeof(maps_file), "/proc/%ju/maps", (uintmax_t)pid);
    snprintf(pagemap_file, sizeof(pagemap_file), "/proc/%ju/pagemap", (uintmax_t)pid);
    snprintf(kpageflags_file, sizeof(kpageflags_file), "/proc/kpageflags");
    maps_fd = open(maps_file, O_RDONLY);
    if (maps_fd < 0) {
        perror("open maps");
        return EXIT_FAILURE;
    }
    pagemap_fd = open(pagemap_file, O_RDONLY);
    if (pagemap_fd < 0) {
        perror("open pagemap");
        return EXIT_FAILURE;
    }
    kflags_fd = open(kpageflags_file, O_RDONLY);
    if (kflags_fd < 0) {
        perror("open kpageflags");
        return EXIT_FAILURE;
    }
    printf("addr pfn soft-dirty file/shared swapped present library\n");
    for (;;) {
        ssize_t length = read(maps_fd, buffer + offset, sizeof buffer - offset);
        if (length <= 0) break;
        length += offset;
        for (size_t i = offset; i < (size_t)length; i++) {
            uintptr_t low = 0, high = 0;
            if (buffer[i] == '\n' && i) {
                const char *lib_name;
                size_t y;
                /* Parse a line from maps. Each line contains a range that contains many pages. */
                {
                    size_t x = i - 1;
                    while (x && buffer[x] != '\n') x--;
                    if (buffer[x] == '\n') x++;
                    while (buffer[x] != '-' && x < sizeof buffer) {
                        char c = buffer[x++];
                        low *= 16;
                        if (c >= '0' && c <= '9') {
                            low += c - '0';
                        } else if (c >= 'a' && c <= 'f') {
                            low += c - 'a' + 10;
                        } else {
                            break;
                        }
                    }
                    while (buffer[x] != '-' && x < sizeof buffer) x++;
                    if (buffer[x] == '-') x++;
                    while (buffer[x] != ' ' && x < sizeof buffer) {
                        char c = buffer[x++];
                        high *= 16;
                        if (c >= '0' && c <= '9') {
                            high += c - '0';
                        } else if (c >= 'a' && c <= 'f') {
                            high += c - 'a' + 10;
                        } else {
                            break;
                        }
                    }
                    lib_name = 0;
                    for (int field = 0; field < 4; field++) {
                        x++;
                        while(buffer[x] != ' ' && x < sizeof buffer) x++;
                    }
                    while (buffer[x] == ' ' && x < sizeof buffer) x++;
                    y = x;
                    while (buffer[y] != '\n' && y < sizeof buffer) y++;
                    buffer[y] = 0;
                    lib_name = buffer + x;
                }
                /* Get info about all pages in this page range with pagemap. */
                {
                    PagemapEntry entry;
                    for (uintptr_t addr = low; addr < high; addr += sysconf(_SC_PAGE_SIZE)) {
                        /* TODO always fails for the last page (vsyscall), why? pread returns 0. */
                        if (!pagemap_get_entry(&entry, addr, pagemap_fd, kflags_fd)) {
                            printf("%jx %jx %u %u %u %u %s\n",
                                (uintmax_t)addr,
                                (uintmax_t)entry.pfn,
                                entry.soft_dirty,
                                entry.file_page,
                                entry.swapped,
                                entry.present,
                                lib_name
                            );
                        }
                    }
                }
                buffer[y] = '\n';
            }
        }
    }
    close(maps_fd);
    close(pagemap_fd);
    return EXIT_SUCCESS;
}
