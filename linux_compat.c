#include <stdint.h>
#include "linux_compat.h"
#include "memory.h"
#include "linux_fs.h"
#include "scheduler.h"

#define TSS_SELECTOR 0x38u
#define LINUX_ENOSYS 38
#define LINUX_EBADF 9
#define LINUX_EXIT_SENTINEL ((int64_t)0x8000000000000000ull)
#define MSR_EFER 0xC0000080u
#define MSR_FS_BASE 0xC0000100u
#define MSR_STAR 0xC0000081u
#define MSR_LSTAR 0xC0000082u
#define MSR_FMASK 0xC0000084u
#define MSR_GS_BASE 0xC0000101u
#define MSR_KERNEL_GS_BASE 0xC0000102u
#define ARCH_SET_FS 0x1002u
#define ARCH_GET_FS 0x1003u

typedef struct { uint64_t user_rsp, kernel_rsp, return_rsp, exit_code, kernel_rflags; } syscall_cpu_t;
#define LINUX_THREAD_LIMIT 8u
#define LINUX_THREAD_STACK_SIZE 65536u
static syscall_cpu_t syscall_cpus[LINUX_THREAD_LIMIT];
static uint8_t linux_thread_stacks[LINUX_THREAD_LIMIT][LINUX_THREAD_STACK_SIZE]
    __attribute__((aligned(16)));
static uint64_t linux_thread_clear_tid[LINUX_THREAD_LIMIT];
static uint32_t linux_timer_ids[16];
static uint32_t linux_timer_count;
static uint64_t linux_signal_wait_key[LINUX_THREAD_LIMIT];
typedef struct {
    uint32_t id, clock_id, notify, signal_number, target_tid;
    uint64_t signal_value, deadline, interval;
    uint8_t active;
} linux_posix_timer_t;
typedef struct {
    uint32_t target_tid, signal_number;
    uint64_t signal_value;
    uint8_t used;
} linux_pending_signal_t;
static linux_posix_timer_t linux_timers[16];
static linux_pending_signal_t linux_pending_signals[16];
static uint64_t process_break;
static uint64_t mmap_cursor;
static uint32_t terminal_lflag = 0x0au; /* ECHO | ICANON */
static uint8_t canonical_line[1024];
static size_t canonical_length, canonical_offset;
static void wrmsr64(uint32_t msr, uint64_t value);
static uint64_t rdmsr64(uint32_t msr);
extern void linux_syscall_entry(void);
extern void maios_linux_console_write(const char *, size_t);
extern int maios_linux_console_read_char(void);
extern void maios_linux_console_set_echo(int enabled);
extern void maios_linux_console_reset(void);

typedef struct __attribute__((packed)) {
    uint32_t reserved0;
    uint64_t rsp0, rsp1, rsp2;
    uint64_t reserved1;
    uint64_t ist1, ist2, ist3, ist4, ist5, ist6, ist7;
    uint64_t reserved2;
    uint16_t reserved3;
    uint16_t iomap_base;
} x86_64_tss_t;

static x86_64_tss_t kernel_tss;
extern uint64_t gdt[];
extern uint8_t kernel_stack_top[];

static syscall_cpu_t *current_syscall_cpu(void)
{
    return (syscall_cpu_t *)(uintptr_t)rdmsr64(MSR_GS_BASE);
}

static uint64_t timespec_to_ticks(int64_t seconds, int64_t nanoseconds)
{
    if (seconds < 0 || nanoseconds < 0 || nanoseconds >= 1000000000ll) return UINT64_MAX;
    if ((uint64_t)seconds > UINT64_MAX / 100u) return UINT64_MAX;
    uint64_t ticks = (uint64_t)seconds * 100u;
    uint64_t fraction = ((uint64_t)nanoseconds + 9999999u) / 10000000u;
    return ticks > UINT64_MAX - fraction ? UINT64_MAX : ticks + fraction;
}

static unsigned int linux_queue_timer_signal(uint32_t tid, uint32_t signal_number,
                                             uint64_t value)
{
    for (unsigned int i = 0; i < 16; ++i) {
        if (linux_pending_signals[i].used) continue;
        linux_pending_signals[i].used = 1;
        linux_pending_signals[i].target_tid = tid;
        linux_pending_signals[i].signal_number = signal_number;
        linux_pending_signals[i].signal_value = value;
        if (tid && tid <= LINUX_THREAD_LIMIT)
            (void)task_wake_key((uint64_t)(uintptr_t)&linux_signal_wait_key[tid - 1], UINT32_MAX);
        return 1;
    }
    return 0;
}

void linux_timer_tick(uint64_t now)
{
    for (unsigned int i = 0; i < 16; ++i) {
        linux_posix_timer_t *timer = &linux_timers[i];
        if (!timer->id || !timer->active || !timer->deadline || now < timer->deadline) continue;
        if (timer->notify != 1 && timer->signal_number)
            (void)linux_queue_timer_signal(timer->target_tid, timer->signal_number,
                                           timer->signal_value);
        if (timer->interval) {
            do { timer->deadline += timer->interval; }
            while (timer->deadline <= now && timer->deadline <= UINT64_MAX - timer->interval);
        } else {
            timer->deadline = 0;
            timer->active = 0;
        }
    }
}

static void tss_descriptor_set(uint64_t *descriptor, uint64_t base, uint32_t limit)
{
    uint64_t low = 0;
    low |= (uint64_t)(limit & 0xffffu);
    low |= (base & 0xffffffu) << 16;
    low |= (uint64_t)0x89u << 40; /* present, available 64-bit TSS */
    low |= (uint64_t)((limit >> 16) & 0x0fu) << 48;
    low |= ((base >> 24) & 0xffu) << 56;
    descriptor[0] = low;
    descriptor[1] = base >> 32;
}

void linux_compat_init(void)
{
    kernel_tss.rsp0 = (uint64_t)(uintptr_t)kernel_stack_top;
    kernel_tss.iomap_base = sizeof(kernel_tss);
    tss_descriptor_set(&gdt[7], (uint64_t)(uintptr_t)&kernel_tss,
                       sizeof(kernel_tss) - 1);
    __asm__ volatile("ltr %0" : : "r"((uint16_t)TSS_SELECTOR) : "memory");
    uint32_t efer_low, efer_high;
    uint64_t star = ((uint64_t)0x23 << 48) | ((uint64_t)0x18 << 32);
    __asm__ volatile("rdmsr" : "=a"(efer_low), "=d"(efer_high) : "c"(MSR_EFER));
    uint64_t efer = (((uint64_t)efer_high << 32) | efer_low) | 1;
    __asm__ volatile("wrmsr" : : "c"(MSR_EFER), "a"((uint32_t)efer), "d"((uint32_t)(efer >> 32)));
    uint64_t lstar = (uint64_t)(uintptr_t)linux_syscall_entry;
    __asm__ volatile("wrmsr" : : "c"(MSR_STAR), "a"((uint32_t)star), "d"((uint32_t)(star >> 32)));
    __asm__ volatile("wrmsr" : : "c"(MSR_LSTAR), "a"((uint32_t)lstar), "d"((uint32_t)(lstar >> 32)));
    __asm__ volatile("wrmsr" : : "c"(MSR_FMASK), "a"(0x4700u), "d"(0));
    __asm__ volatile("wrmsr" : : "c"(MSR_GS_BASE), "a"((uint32_t)(uintptr_t)&syscall_cpus[0]), "d"((uint32_t)((uintptr_t)&syscall_cpus[0] >> 32)));
    __asm__ volatile("wrmsr" : : "c"(MSR_KERNEL_GS_BASE), "a"(0), "d"(0));
}

void linux_scheduler_set_context(uint64_t fs_base, uint64_t gs_base,
                                 uint64_t kernel_gs_base,
                                 uint64_t kernel_stack)
{
    wrmsr64(MSR_FS_BASE, fs_base);
    wrmsr64(MSR_GS_BASE, gs_base);
    wrmsr64(MSR_KERNEL_GS_BASE, kernel_gs_base);
    kernel_tss.rsp0 = kernel_stack ? kernel_stack : (uint64_t)(uintptr_t)kernel_stack_top;
}

void linux_set_kernel_stack(uint64_t stack_top)
{
    syscall_cpus[0].kernel_rsp = stack_top;
    kernel_tss.rsp0 = stack_top;
    task_set_current_kernel_stack(stack_top);
    uint64_t cpu_base = (uint64_t)(uintptr_t)&syscall_cpus[0];
    wrmsr64(MSR_FS_BASE, 0);
    wrmsr64(MSR_GS_BASE, cpu_base);
    wrmsr64(MSR_KERNEL_GS_BASE, 0);
    task_set_current_cpu_context(0, cpu_base, 0);
}

void linux_set_program_break(uint64_t address)
{
    process_break = (address + 4095u) & ~4095ull;
}

void linux_process_reset(void)
{
    uint64_t flags;
    __asm__ volatile("pushfq; popq %0; cli" : "=r"(flags) : : "memory");
    task_kill_linux_threads();
    linux_fs_process_reset();
    process_break = 0;
    mmap_cursor = MAIOS_USER_BASE + 0x20000000ull;
    for (unsigned int i = 0; i < LINUX_THREAD_LIMIT; ++i) {
        syscall_cpus[i].exit_code = 0;
        syscall_cpus[i].user_rsp = 0;
        syscall_cpus[i].kernel_rsp = 0;
        syscall_cpus[i].return_rsp = 0;
        syscall_cpus[i].kernel_rflags = 0;
        linux_thread_clear_tid[i] = 0;
        linux_signal_wait_key[i] = 0;
    }
    linux_timer_count = 0;
    for (unsigned int i = 0; i < 16; ++i) {
        linux_timer_ids[i] = 0;
        linux_timers[i] = (linux_posix_timer_t){0};
        linux_pending_signals[i] = (linux_pending_signal_t){0};
    }
    terminal_lflag = 0x0au;
    canonical_length = canonical_offset = 0;
    maios_linux_console_reset();
    wrmsr64(MSR_FS_BASE, 0);
    __asm__ volatile("pushq %0; popfq" : : "r"(flags) : "memory", "cc");
}

static uint64_t rdmsr64(uint32_t msr)
{
    uint32_t low, high;
    __asm__ volatile("rdmsr" : "=a"(low), "=d"(high) : "c"(msr));
    return ((uint64_t)high << 32) | low;
}

static void wrmsr64(uint32_t msr, uint64_t value)
{
    __asm__ volatile("wrmsr" : : "c"(msr), "a"((uint32_t)value),
                     "d"((uint32_t)(value >> 32)));
}

static void linux_print_hex64(uint64_t value)
{
    static const char digits[] = "0123456789abcdef";
    char text[16];
    for (size_t i = 0; i < sizeof(text); ++i) {
        text[sizeof(text) - 1 - i] = digits[value & 0xfu];
        value >>= 4;
    }
    maios_linux_console_write(text, sizeof(text));
}

static void linux_report_exception(const char *kind, uint64_t vector, uint64_t error,
                                   uint64_t rip, uint64_t address)
{
    static const char prefix[] = "\nmaiOS: excecao ";
    static const char vector_label[] = " vetor=0x";
    static const char rip_label[] = " RIP=0x";
    static const char address_label[] = " CR2=0x";
    static const char error_label[] = " erro=0x";
    static const char suffix[] = "\n";
    maios_linux_console_write(prefix, sizeof(prefix) - 1);
    maios_linux_console_write(kind, __builtin_strlen(kind));
    maios_linux_console_write(vector_label, sizeof(vector_label) - 1);
    linux_print_hex64(vector);
    maios_linux_console_write(rip_label, sizeof(rip_label) - 1);
    linux_print_hex64(rip);
    maios_linux_console_write(address_label, sizeof(address_label) - 1);
    linux_print_hex64(address);
    maios_linux_console_write(error_label, sizeof(error_label) - 1);
    linux_print_hex64(error);
    maios_linux_console_write(suffix, sizeof(suffix) - 1);
}

void linux_report_user_exception(uint64_t vector, uint64_t error, uint64_t rip, uint64_t address)
{
    linux_report_exception("usuario Linux", vector, error, rip, address);
}

void linux_report_kernel_exception(uint64_t vector, uint64_t error, uint64_t rip, uint64_t address)
{
    linux_report_exception("kernel", vector, error, rip, address);
}

typedef struct { char fields[6][65]; } linux_utsname_t;

static int64_t linux_uname(uint64_t destination)
{
    linux_utsname_t name = {{
        "maiOS", "maiOS", "0.1-x86_64", "#1", "x86_64", "(none)"
    }};
    return memory_copy_to_user(destination, &name, sizeof(name)) ? 0 : -14;
}

typedef struct {
    uint32_t iflag, oflag, cflag, lflag;
    uint8_t line, cc[19];
} linux_kernel_termios_t;

typedef struct { uint16_t rows, columns, xpixel, ypixel; } linux_winsize_t;
typedef struct { uint64_t base, length; } linux_iovec_t;

static int64_t linux_writev(uint64_t fd, uint64_t vector, uint64_t count)
{
    if (count > 64) return -22;
    if (fd < 3 && fd != 1 && fd != 2) return -LINUX_EBADF;
    if (fd >= 3 && !count) return linux_fs_write(fd, 0, 0, 0, 0);
    int64_t total = 0;
    for (uint64_t i = 0; i < count; ++i) {
        linux_iovec_t item;
        if (!memory_copy_from_user(&item, vector + i * sizeof(item), sizeof(item)))
            return total ? total : -14;
        if (item.length > 0x7fffffffffffffffull - (uint64_t)total)
            return total ? total : -22;
        if (fd >= 3) {
            int64_t written = linux_fs_write(fd, item.base, item.length, 0, 0);
            if (written < 0) return total ? total : written;
            total += written;
            if ((uint64_t)written < item.length) return total;
        } else {
            if (fd != 1 && fd != 2) return total ? total : -LINUX_EBADF;
            char buffer[256];
            uint64_t offset = 0;
            while (offset < item.length) {
                size_t length = (size_t)(item.length - offset);
                if (length > sizeof(buffer)) length = sizeof(buffer);
                if (!memory_copy_from_user(buffer, item.base + offset, length))
                    return total ? total : -14;
                maios_linux_console_write(buffer, length);
                offset += length;
                total += (int64_t)length;
            }
        }
    }
    return total;
}

static int64_t linux_select_ready(uint64_t nfds, uint64_t read_set,
                                  uint64_t write_set, uint64_t except_set)
{
    if (nfds > 1024) return -22;
    uint64_t reads[16] = {0}, writes[16] = {0}, excepts[16] = {0};
    if (read_set && !memory_copy_from_user(reads, read_set, sizeof(reads))) return -14;
    if (write_set && !memory_copy_from_user(writes, write_set, sizeof(writes))) return -14;
    if (except_set && !memory_copy_from_user(excepts, except_set, sizeof(excepts))) return -14;
    uint64_t ready = 0;
    for (uint64_t fd = 0; fd < 1024; ++fd) {
        uint64_t bit = 1ull << (fd & 63u);
        uint64_t *read_word = &reads[fd >> 6];
        uint64_t *write_word = &writes[fd >> 6];
        uint64_t *except_word = &excepts[fd >> 6];
        if (fd >= nfds || (fd >= 3 && !linux_fs_fd_valid(fd))) {
            *read_word &= ~bit; *write_word &= ~bit; *except_word &= ~bit;
            continue;
        }
        if (*read_word & bit) {
            if (fd == 0 || fd >= 3) ++ready;
            else *read_word &= ~bit;
        }
        if (*write_word & bit) {
            if (fd == 1 || fd == 2 || fd >= 3) ++ready;
            else *write_word &= ~bit;
        }
        if (*except_word & bit) { *except_word &= ~bit; }
    }
    if ((read_set && !memory_copy_to_user(read_set, reads, sizeof(reads))) ||
        (write_set && !memory_copy_to_user(write_set, writes, sizeof(writes))) ||
        (except_set && !memory_copy_to_user(except_set, excepts, sizeof(excepts)))) return -14;
    return (int64_t)ready;
}

int64_t linux_syscall_dispatch(uint64_t *saved_registers)
{
    uint64_t nr = saved_registers[14], a1 = saved_registers[8];
    uint64_t a2 = saved_registers[9], a3 = saved_registers[11];
    uint64_t a4 = saved_registers[5], a5 = saved_registers[7];
    uint64_t a6 = saved_registers[6];
    if (nr == 202) { /* futex wait/wake for threads sharing this address space */
        const uint32_t private_flag = 128u, realtime_flag = 256u;
        uint32_t operation = (uint32_t)a2 & ~(private_flag | realtime_flag);
        if (a1 & 3u) return -22;
        if (operation == 1u || operation == 10u)
            return task_wake_key(a1, a3 > UINT32_MAX ? UINT32_MAX : (uint32_t)a3);
        if (operation == 0u || operation == 9u) { /* WAIT / WAIT_BITSET */
            uint32_t value;
            if (!memory_copy_from_user(&value, a1, sizeof(value))) return -14;
            if (value != (uint32_t)a3) return -11; /* EAGAIN, as on Linux */
            task_block_on(a1);
            return 0;
        }
        return -38;
    }
    if (nr == 222) { /* timer_create, including SIGEV_THREAD_ID used by glibc */
        if (a1 > 7) return -22;
        uint8_t event[64] = {0};
        if (a2 && !memory_copy_from_user(event, a2, sizeof(event))) return -14;
        uint32_t notification = *(uint32_t *)(void *)(event + 12);
        uint32_t signal_number = *(uint32_t *)(void *)(event + 8);
        uint32_t target_tid = *(uint32_t *)(void *)(event + 16);
        uint64_t signal_value = *(uint64_t *)(void *)event;
        if (notification > 4 || notification == 3) return -22;
        if (notification != 1 && (signal_number == 0 || signal_number > 64)) return -22;
        if (notification == 4 && (!target_tid || target_tid > LINUX_THREAD_LIMIT)) return -22;
        uint32_t id;
        for (id = 1; id <= 16; ++id)
            if (!linux_timers[id - 1].id) break;
        if (id > 16) return -11;
        linux_posix_timer_t *timer = &linux_timers[id - 1];
        timer->id = id; timer->clock_id = (uint32_t)a1;
        timer->notify = notification; timer->signal_number = signal_number;
        timer->target_tid = notification == 4 ? target_tid : 1;
        timer->signal_value = signal_value; timer->deadline = timer->interval = 0;
        timer->active = 0;
        linux_timer_ids[id - 1] = id;
        if (id > linux_timer_count) linux_timer_count = id;
        return memory_copy_to_user(a3, &id, sizeof(id)) ? 0 : -14;
    }
    if (nr == 223) { /* timer_settime */
        uint32_t id = (uint32_t)a1;
        struct { int64_t interval_sec, interval_nsec, value_sec, value_nsec; } value, old;
        if (!id || id > 16 || linux_timer_ids[id - 1] != id || !a3) return -22;
        linux_posix_timer_t *timer = &linux_timers[id - 1];
        uint64_t now = scheduler_ticks();
        old.interval_sec = (int64_t)(timer->interval / 100u);
        old.interval_nsec = (int64_t)(timer->interval % 100u) * 10000000ll;
        uint64_t remaining = timer->deadline > now ? timer->deadline - now : 0;
        old.value_sec = (int64_t)(remaining / 100u);
        old.value_nsec = (int64_t)(remaining % 100u) * 10000000ll;
        if (!memory_copy_from_user(&value, a3, sizeof(value))) return -14;
        uint64_t interval = timespec_to_ticks(value.interval_sec, value.interval_nsec);
        uint64_t initial = timespec_to_ticks(value.value_sec, value.value_nsec);
        if (interval == UINT64_MAX || initial == UINT64_MAX) return -22;
        timer->interval = interval;
        timer->active = initial != 0;
        timer->deadline = initial ? now + initial : 0;
        if (a4 && !memory_copy_to_user(a4, &old, sizeof(old))) return -14;
        return 0;
    }
    if (nr == 224) { /* timer_gettime */
        uint32_t id = (uint32_t)a1;
        if (!id || id > 16 || linux_timer_ids[id - 1] != id) return -22;
        linux_posix_timer_t *timer = &linux_timers[id - 1];
        uint64_t now = scheduler_ticks();
        uint64_t remaining = timer->deadline > now ? timer->deadline - now : 0;
        struct { int64_t interval_sec, interval_nsec, value_sec, value_nsec; } value = {
            (int64_t)(timer->interval / 100u), (int64_t)(timer->interval % 100u) * 10000000ll,
            (int64_t)(remaining / 100u), (int64_t)(remaining % 100u) * 10000000ll
        };
        return memory_copy_to_user(a2, &value, sizeof(value)) ? 0 : -14;
    }
    if (nr == 225) return 0; /* timer_getoverrun */
    if (nr == 226) { /* timer_delete */
        uint32_t id = (uint32_t)a1;
        if (!id || id > 16 || linux_timer_ids[id - 1] != id) return -22;
        linux_timer_ids[id - 1] = 0;
        linux_timers[id - 1].id = 0;
        return 0;
    }
    if (nr == 128) { /* rt_sigtimedwait, used by glibc's SIGEV_THREAD helper */
        uint64_t mask;
        unsigned int tid = task_current_id() + 1;
        if (a5 != sizeof(mask) || !memory_copy_from_user(&mask, a1, sizeof(mask))) return -22;
        if (!mask || tid > LINUX_THREAD_LIMIT) return -22;
        for (;;) {
            for (unsigned int i = 0; i < 16; ++i) {
                linux_pending_signal_t *pending = &linux_pending_signals[i];
                if (!pending->used || pending->target_tid != tid ||
                    pending->signal_number > 64 ||
                    !(mask & (1ull << (pending->signal_number - 1)))) continue;
                uint32_t signo = pending->signal_number;
                if (a3) {
                    uint8_t info[128] = {0};
                    int32_t si_signo = (int32_t)signo, si_code = -2, si_tid = (int32_t)tid;
                    int32_t overrun = 0;
                    for (size_t j = 0; j < 4; ++j) info[j] = ((uint8_t *)&si_signo)[j];
                    for (size_t j = 0; j < 4; ++j) info[8 + j] = ((uint8_t *)&si_code)[j];
                    for (size_t j = 0; j < 4; ++j) info[16 + j] = ((uint8_t *)&si_tid)[j];
                    for (size_t j = 0; j < 4; ++j) info[20 + j] = ((uint8_t *)&overrun)[j];
                    for (size_t j = 0; j < 8; ++j) info[24 + j] = ((uint8_t *)&pending->signal_value)[j];
                    if (!memory_copy_to_user(a3, info, sizeof(info))) return -14;
                }
                pending->used = 0;
                return signo;
            }
            if (a4) return -11;
            task_block_on((uint64_t)(uintptr_t)&linux_signal_wait_key[tid - 1]);
        }
    }
    if (nr == 23) /* select */
        return linux_select_ready(a1, a2, a3, a4);
    if (nr == 270) { /* pselect6 */
        if (a6) {
            uint64_t mask[2];
            if (!memory_copy_from_user(mask, a6, sizeof(mask))) return -14;
        }
        return linux_select_ready(a1, a2, a3, a4);
    }
    if (nr == 7 || nr == 271) { /* poll / ppoll */
        typedef struct { int32_t fd; int16_t events, revents; } linux_pollfd_t;
        if (a2 > 64) return -22;
        if (nr == 271 && a3) {
            uint64_t timeout[2];
            if (!memory_copy_from_user(timeout, a3, sizeof(timeout))) return -14;
        }
        uint64_t ready = 0;
        for (uint64_t i = 0; i < a2; ++i) {
            linux_pollfd_t item;
            if (!memory_copy_from_user(&item, a1 + i * sizeof(item), sizeof(item))) return -14;
            item.revents = 0;
            if (item.fd >= 0) {
                if (!linux_fs_fd_valid((uint64_t)item.fd)) item.revents = 0x20; /* POLLNVAL */
                else {
                    if (item.fd == 0 && (item.events & 0x001)) item.revents |= 0x001; /* POLLIN */
                    if ((item.fd == 1 || item.fd == 2) && (item.events & 0x004)) item.revents |= 0x004; /* POLLOUT */
                    if (item.fd >= 3) item.revents |= item.events & (0x001 | 0x004);
                }
            }
            if (item.revents) ++ready;
            if (!memory_copy_to_user(a1 + i * sizeof(item), &item, sizeof(item))) return -14;
        }
        return (int64_t)ready;
    }
    if (nr == 20) return linux_writev(a1, a2, a3);
    if (nr == 217) return linux_fs_getdents64(a1, a2, a3);
    if (nr == 39) return 1; /* getpid */
    if (nr == 110) return 0; /* getppid */
    if (nr == 111) return 1; /* getpgrp */
    if (nr == 112) return 1; /* setsid */
    if (nr == 121) return ((int64_t)a1 == 0 || a1 == 1) ? 1 : -3; /* getpgid */
    if (nr == 124) return ((int64_t)a1 == 0 || a1 == 1) ? 1 : -3; /* getsid */
    if (nr == 109) { /* setpgid for the single foreground process */
        if (((int64_t)a1 != 0 && a1 != 1) || (a2 != 0 && a2 != 1)) return -3;
        return 0;
    }
    if (nr == 102 || nr == 107 || nr == 104 || nr == 108) return 0;
    if (nr == 186) return (int64_t)task_current_id() + 1; /* gettid */
    if (nr == 218) { /* set_tid_address */
        unsigned int id = task_current_id();
        if (id < LINUX_THREAD_LIMIT) linux_thread_clear_tid[id] = a1;
        return (int64_t)id + 1;
    }
    if (nr == 56) { /* clone: shared-VM Linux threads */
        const uint64_t clone_vm = 0x00000100ull, clone_sighand = 0x00000800ull;
        const uint64_t clone_thread = 0x00010000ull, clone_settls = 0x00080000ull;
        const uint64_t clone_parent_settid = 0x00100000ull;
        const uint64_t clone_child_cleartid = 0x00200000ull;
        const uint64_t clone_child_settid = 0x01000000ull;
        const uint64_t allowed = clone_vm | 0x200ull | 0x400ull | clone_sighand |
            clone_thread | 0x40000ull | clone_settls | clone_parent_settid |
            clone_child_cleartid | 0x00400000ull | clone_child_settid;
        if ((a1 & ~allowed) || (a1 & (clone_vm | clone_sighand | clone_thread)) !=
            (clone_vm | clone_sighand | clone_thread) || !a2 ||
            a2 < MAIOS_USER_BASE || a2 >= MAIOS_USER_LIMIT ||
            !memory_user_page_mapped(a2 - (a2 & 4095u))) return -22;
        uint64_t registers[15];
        for (unsigned int i = 0; i < 15; ++i) registers[i] = saved_registers[i];
        registers[14] = 0; /* child returns zero from clone */
        uint64_t parent_fs = rdmsr64(MSR_FS_BASE);
        uint64_t child_fs = (a1 & clone_settls) ? a5 : parent_fs;
        if (child_fs && (child_fs < MAIOS_USER_BASE || child_fs >= MAIOS_USER_LIMIT ||
                         !memory_user_page_mapped(child_fs))) return -14;
        uint64_t child_gs = rdmsr64(MSR_KERNEL_GS_BASE);
        unsigned int slot = 0;
        /* Reserve an unused CPU context; task creation chooses the same free slot. */
        for (slot = 1; slot < LINUX_THREAD_LIMIT; ++slot) {
            if (!syscall_cpus[slot].kernel_rsp && task_slot_available(slot)) break;
        }
        if (slot == LINUX_THREAD_LIMIT) return -11;
        uint64_t child_kstack = (uint64_t)(uintptr_t)
            (linux_thread_stacks[slot] + LINUX_THREAD_STACK_SIZE);
        int tid = task_create_linux_user(registers, saved_registers[12], a2,
            saved_registers[4], child_fs, child_gs,
            (uint64_t)(uintptr_t)&syscall_cpus[slot], child_kstack);
        if (tid < 0) return -11;
        syscall_cpus[tid].kernel_rsp = child_kstack;
        if (a1 & clone_parent_settid) {
            uint32_t value = (uint32_t)tid + 1;
            if (!memory_copy_to_user(a3, &value, sizeof(value))) return -14;
        }
        if (a1 & (clone_child_settid | clone_child_cleartid)) {
            linux_thread_clear_tid[tid] = a4;
            if (a1 & clone_child_settid) {
                uint32_t value = (uint32_t)tid + 1;
                if (!memory_copy_to_user(a4, &value, sizeof(value))) return -14;
            }
        }
        return (int64_t)tid + 1;
    }
    if (nr == 63) return linux_uname(a1);
    if (nr == 158) {
        if (a1 == ARCH_SET_FS) {
            if (a2 && (a2 < MAIOS_USER_BASE || a2 >= MAIOS_USER_LIMIT ||
                       !memory_user_page_mapped(a2))) return -14;
            wrmsr64(MSR_FS_BASE, a2);
            return 0;
        }
        if (a1 == ARCH_GET_FS) {
            uint64_t fs_base = rdmsr64(MSR_FS_BASE);
            return memory_copy_to_user(a2, &fs_base, sizeof(fs_base)) ? 0 : -14;
        }
        return -22;
    }
    if (nr == 273) return 0; /* set_robust_list */
    if (nr == 13) { /* rt_sigaction: table is not delivered yet */
        uint8_t action[32];
        if (a2 && !memory_copy_from_user(action, a2, sizeof(action))) return -14;
        if (a3) {
            for (size_t i = 0; i < sizeof(action); ++i) action[i] = 0;
            if (!memory_copy_to_user(a3, action, sizeof(action))) return -14;
        }
        return 0;
    }
    if (nr == 14) { /* rt_sigprocmask */
        uint64_t new_mask, old_mask = 0;
        if (a2 && !memory_copy_from_user(&new_mask, a2, sizeof(new_mask))) return -14;
        if (a3 && !memory_copy_to_user(a3, &old_mask, sizeof(old_mask))) return -14;
        return 0;
    }
    if (nr == 28) return 0; /* madvise */
    if (nr == 77) return linux_fs_ftruncate(a1, (int64_t)a2);
    if (nr == 82) return linux_fs_rename(a1, a2);
    if (nr == 87) return linux_fs_unlink(a1, 0);
    if (nr == 89) return linux_fs_readlink(a1, a2, a3);
    if (nr == 131) return 0; /* sigaltstack */
    if (nr == 228) { /* clock_gettime from the 100 Hz PIT */
        struct { int64_t seconds, nanoseconds; } time;
        uint64_t ticks = scheduler_ticks();
        time.seconds = (int64_t)(ticks / 100u);
        time.nanoseconds = (int64_t)(ticks % 100u) * 10000000;
        return memory_copy_to_user(a2, &time, sizeof(time)) ? 0 : -14;
    }
    if (nr == 302) { /* prlimit64: report an unlimited resource cap */
        if ((int64_t)a1 != 0 && (int64_t)a1 != 1) return -3;
        if (a3) {
            uint64_t limit[2];
            if (!memory_copy_from_user(limit, a3, sizeof(limit))) return -14;
        }
        if (a4) {
            uint64_t limit[2] = {UINT64_MAX, UINT64_MAX};
            if (!memory_copy_to_user(a4, limit, sizeof(limit))) return -14;
        }
        return 0;
    }
    if (nr == 318) { /* getrandom */
        if (a3 & ~3ull) return -22;
        uint64_t done = 0;
        while (done < a2) {
            uint32_t low, high;
            __asm__ volatile("rdtsc" : "=a"(low), "=d"(high));
            uint64_t value = ((uint64_t)high << 32) | low;
            value ^= (uint64_t)(uintptr_t)&value + done * 0x9e3779b97f4a7c15ull;
            size_t count = (size_t)(a2 - done);
            if (count > sizeof(value)) count = sizeof(value);
            if (!memory_copy_to_user(a1 + done, &value, count))
                return done ? (int64_t)done : -14;
            done += count;
        }
        return (int64_t)done;
    }
    if (nr == 2) return linux_fs_open(a1, a2, a3);
    if (nr == 257) return linux_fs_openat(a1, a2, a3, a4);
    if (nr == 3) return linux_fs_close(a1);
    if (nr == 5) return linux_fs_fstat(a1, a2);
    if (nr == 8) return linux_fs_seek(a1, (int64_t)a2, a3);
    if (nr == 17) return linux_fs_read(a1, a2, a3, a4, 1);
    if (nr == 18) return linux_fs_write(a1, a2, a3, a4, 1);
    if (nr == 21) return linux_fs_access(a1);
    if (nr == 269 || nr == 439) {
        if ((int32_t)a1 != -100) return -LINUX_EBADF;
        if (a3 & ~7ull) return -22;
        if (nr == 439 && (a4 & ~0x1200ull)) return -22;
        return linux_fs_access(a2);
    }
    if (nr == 72) return linux_fs_fcntl(a1, a2, a3);
    if (nr == 74) return linux_fs_fsync(a1);
    if (nr == 79) return linux_fs_getcwd(a1, a2);
    if (nr == 80) return linux_fs_chdir(a1);
    if (nr == 4 || nr == 6) return linux_fs_stat_path(a1, a2);
    if (nr == 262) {
        if ((a4 & 0x1000u) && a2) { /* AT_EMPTY_PATH: fstat on a descriptor */
            char first;
            if (!memory_copy_from_user(&first, a2, sizeof(first))) return -14;
            if (!first) return linux_fs_fstat(a1, a3);
        }
        if ((int32_t)a1 != -100) return -LINUX_EBADF;
        return linux_fs_stat_path(a2, a3);
    }
    if (nr == 332) { /* statx for regular files in the ext4 namespace */
        if ((int32_t)a1 != -100 || (a3 & ~0x1000ull)) return -22;
        return linux_fs_statx_path(a2, a5);
    }
    if (nr == 263) {
        if ((int32_t)a1 != -100) return -LINUX_EBADF;
        return linux_fs_unlink(a2, (a3 & 0x200u) != 0);
    }
    if (nr == 267) {
        if ((int32_t)a1 != -100) return -LINUX_EBADF;
        return linux_fs_readlink(a2, a3, a4);
    }
    if (nr == 316) {
        if ((int32_t)a1 != -100 || (int32_t)a3 != -100 || a5) return -22;
        return linux_fs_rename(a2, a4);
    }
    if (nr == 9) { /* mmap: anonymous or private file-backed mappings */
        const uint64_t map_private = 0x02u, map_anonymous = 0x20u;
        const uint64_t map_fixed = 0x10u, map_fixed_noreplace = 0x100000u;
        int anonymous = (a4 & map_anonymous) != 0;
        if (!a2 || !(a4 & map_private) ||
            (anonymous && ((int32_t)a5 != -1 || a6)) ||
            (!anonymous && ((int32_t)a5 == -1 || (a6 & 4095u)))) return -22;
        if (a2 > SIZE_MAX - 4095u) return -22;
        uint64_t length = (a2 + 4095u) & ~4095ull;
        uint64_t ceiling = MAIOS_USER_LIMIT - 16u * 1024u * 1024u;
        uint64_t address = a1;
        if (address < MAIOS_USER_BASE || address >= ceiling || (address & 4095u)) {
            if (a4 & (map_fixed | map_fixed_noreplace)) return -22;
            address = mmap_cursor;
        }
        if (a4 & (map_fixed | map_fixed_noreplace)) {
            if (address > ceiling || length > ceiling - address) return -12;
            uint64_t page = address;
            while (page < address + length && !memory_user_page_mapped(page)) page += 4096u;
            if (page < address + length) {
                if (a4 & map_fixed_noreplace) return -17;
                memory_unmap_user(address, (size_t)length);
            }
        }
        for (;;) {
            if (address > ceiling || length > ceiling - address) return -12;
            uint64_t page = address;
            while (page < address + length && !memory_user_page_mapped(page))
                page += 4096u;
            if (page == address + length) break;
            address = page + 4096u;
        }
        uint64_t mapped = address;
        for (uint64_t page = address; page < address + length; page += 4096u) {
            uint64_t physical = memory_alloc_page();
            if (!physical || !memory_map_user_page(page, physical, (a3 & 2u) != 0)) {
                uint64_t count = page - mapped;
                if (count) memory_unmap_user(mapped, (size_t)count);
                return -12;
            }
        }
        mmap_cursor = address + length;
        if (!anonymous) {
            int64_t mapped_file = linux_fs_map_file(a5, a6, address, length);
            if (mapped_file < 0) {
                memory_unmap_user(address, (size_t)length);
                return mapped_file;
            }
        }
        return (int64_t)address;
    }
    if (nr == 10) { /* mprotect */
        if (a2 > SIZE_MAX - 4095u || !a2) return -22;
        uint64_t length = (a2 + 4095u) & ~4095ull;
        return memory_protect_user(a1, (size_t)length, (a3 & 2u) != 0) ? 0 : -12;
    }
    if (nr == 11) { /* munmap */
        if (a2 > SIZE_MAX - 4095u || !a2) return -22;
        uint64_t length = (a2 + 4095u) & ~4095ull;
        return memory_unmap_user(a1, (size_t)length) ? 0 : -22;
    }
    if (nr == 12) { /* brk */
        if (!process_break) process_break = MAIOS_USER_BASE + 0x10000000ull;
        if (!a1) return (int64_t)process_break;
        uint64_t ceiling = MAIOS_USER_LIMIT - 16u * 1024u * 1024u;
        if (a1 < process_break || a1 > ceiling) return (int64_t)process_break;
        uint64_t page = (process_break + 4095u) & ~4095ull;
        uint64_t end = (a1 + 4095u) & ~4095ull;
        while (page < end) {
            if (!memory_user_page_mapped(page)) {
                uint64_t physical = memory_alloc_page();
                if (!physical || !memory_map_user_page(page, physical, 1))
                    return (int64_t)process_break;
            }
            page += 4096u;
        }
        process_break = a1;
        return (int64_t)process_break;
    }
    if (nr == 16) { /* basic terminal ioctls */
        if (a1 > 2) return -LINUX_EBADF;
        if (a2 == 0x5401u) { /* TCGETS */
            linux_kernel_termios_t termios = { .iflag = 0x500u, .oflag = 0x5u,
                .cflag = 0xbfu, .lflag = terminal_lflag };
            termios.cc[0] = 3;  /* VINTR */
            termios.cc[1] = 28; /* VQUIT */
            termios.cc[2] = 127;/* VERASE */
            termios.cc[3] = 21; /* VKILL */
            termios.cc[4] = 4;  /* VEOF */
            termios.cc[6] = 1;  /* VMIN */
            termios.cc[8] = 17; /* VSTART */
            termios.cc[9] = 19; /* VSTOP */
            termios.cc[10] = 26;/* VSUSP */
            return memory_copy_to_user(a3, &termios, sizeof(termios)) ? 0 : -14;
        }
        if (a2 == 0x5402u || a2 == 0x5403u || a2 == 0x5404u) { /* TCSETS* */
            linux_kernel_termios_t termios;
            if (!memory_copy_from_user(&termios, a3, sizeof(termios))) return -14;
            terminal_lflag = termios.lflag;
            maios_linux_console_set_echo((terminal_lflag & 0x8u) != 0);
            if (!(terminal_lflag & 0x2u)) canonical_length = canonical_offset = 0;
            return 0;
        }
        if (a2 == 0x5413u) { /* TIOCGWINSZ */
            linux_winsize_t size = {25, 80, 0, 0};
            return memory_copy_to_user(a3, &size, sizeof(size)) ? 0 : -14;
        }
        if (a2 == 0x5414u) return 0; /* TIOCSWINSZ */
        if (a2 == 0x540Fu || a2 == 0x5429u) { /* TIOCGPGRP / TIOCGSID */
            int32_t id = 1;
            return memory_copy_to_user(a3, &id, sizeof(id)) ? 0 : -14;
        }
        if (a2 == 0x5410u) { /* TIOCSPGRP */
            int32_t id;
            if (!memory_copy_from_user(&id, a3, sizeof(id))) return -14;
            return id == 1 ? 0 : -3;
        }
        return -LINUX_ENOSYS;
    }
    if (nr == 1) { /* write */
        if (a1 >= 3) return linux_fs_write(a1, a2, a3, 0, 0);
        if (a1 != 1 && a1 != 2) return -LINUX_EBADF;
        char buffer[256]; size_t done = 0;
        while (done < a3) {
            size_t count = (size_t)(a3 - done); if (count > sizeof(buffer)) count = sizeof(buffer);
            if (!memory_copy_from_user(buffer, a2 + done, count)) return done ? (int64_t)done : -14;
            maios_linux_console_write(buffer, count); done += count;
        }
        return (int64_t)done;
    }
    if (nr == 0) { /* read from console */
        if (a1 >= 3) return linux_fs_read(a1, a2, a3, 0, 0);
        if (a1 != 0) return -LINUX_EBADF;
        if (!a3) return 0;
        if (!(terminal_lflag & 0x2u)) {
            int ch = maios_linux_console_read_char();
            char c = (char)ch;
            return memory_copy_to_user(a2, &c, 1) ? 1 : -14;
        }
        if (canonical_offset >= canonical_length) {
            canonical_length = canonical_offset = 0;
            while (canonical_length < sizeof(canonical_line)) {
                char c = (char)maios_linux_console_read_char();
                if (c == '\b' || c == 0x7f) {
                    if (canonical_length) --canonical_length;
                    continue;
                }
                if (c == 4) break;
                canonical_line[canonical_length++] = (uint8_t)c;
                if (c == '\n') break;
            }
            if (!canonical_length) return 0;
        }
        size_t count = canonical_length - canonical_offset;
        if (count > a3) count = (size_t)a3;
        if (!memory_copy_to_user(a2, canonical_line + canonical_offset, count)) return -14;
        canonical_offset += count;
        if (canonical_offset == canonical_length) canonical_length = canonical_offset = 0;
        return (int64_t)count;
    }
    if (nr == 60 || nr == 231) {
        if (task_is_linux_user()) {
            unsigned int id = task_current_id();
            if (id < LINUX_THREAD_LIMIT && linux_thread_clear_tid[id]) {
                uint32_t zero = 0;
                (void)memory_copy_to_user(linux_thread_clear_tid[id], &zero, sizeof(zero));
                (void)task_wake_key(linux_thread_clear_tid[id], UINT32_MAX);
            }
            if (id < LINUX_THREAD_LIMIT) syscall_cpus[id].kernel_rsp = 0;
            task_exit_current();
        }
        current_syscall_cpu()->exit_code = a1 & 0xffu;
        return LINUX_EXIT_SENTINEL;
    }
    return -LINUX_ENOSYS;
}
