#include "scheduler.h"
#include "frame.h"
#include "fs_test.h"
#include "gdt.h"
#include "klog.h"
#include "paging.h"
#include "pit.h"
#include "sync.h"
#include "usermode.h"
#include "vfs.h"

#define TASK_KERNEL_STACK_PAGES 2u
#define TASK_MAX_MMAP_REGIONS 8u

typedef enum {
    TASK_UNUSED,
    TASK_STARTING,
    TASK_RUNNABLE,
    TASK_BLOCKED,
    TASK_WAITING,
    TASK_SLEEPING,
    TASK_ZOMBIE,
    TASK_REAP_PENDING
} task_state_t;

typedef struct {
    uint32_t pid;
    uint32_t parent_pid;
    task_state_t state;
    loaded_elf_t program;
    uint32_t kernel_stack;
    regs_t *frame;
    uint32_t exit_status;
    uint32_t wait_pid;
    uint32_t wait_status_user;
    uint32_t wait_rusage_user;
    uint32_t wake_tick;
    uint32_t heap_brk;    /* current program break (0 = not yet used) */
    uint32_t heap_top;    /* highest heap page ever mapped (grow only) */
    uint32_t mmap_region_count;
    uint32_t user_ticks;          /* timer ticks spent in ring 3 */
    uint32_t kernel_ticks;        /* timer ticks spent in ring 0 */
    uint32_t syscall_count;
    uint32_t voluntary_switches;  /* blocked, slept, or waited */
    uint32_t involuntary_switches;/* preempted while runnable */
    struct {
        uint32_t base;
        uint32_t size;
    } mmap_regions[TASK_MAX_MMAP_REGIONS];
} task_t;

#define SCHEDULER_IDLE_INDEX ((int)SCHEDULER_MAX_PROCESSES)
static task_t tasks[SCHEDULER_MAX_PROCESSES];
static task_t idle_task;
static int current_task = -1;
static int scheduler_started;
static int timer_marker_written;
static int waitq_marker_written;

int scheduler_user_tasks_done(void) {
    if (!scheduler_started) return 0;
    for (uint32_t i = 0; i < SCHEDULER_MAX_PROCESSES; i++) {
        if (tasks[i].state == TASK_STARTING ||
            tasks[i].state == TASK_RUNNABLE ||
            tasks[i].state == TASK_BLOCKED ||
            tasks[i].state == TASK_WAITING ||
            tasks[i].state == TASK_SLEEPING)
            return 0;
    }
    return 1;
}

/* Wake every sleeper whose deadline has been reached. Called from the
 * timer IRQ so a sleeping task resumes once its wake tick passes. The
 * unsigned subtraction compares correctly across the 32-bit tick wrap. */
static void wake_expired_sleepers(void) {
    uint32_t now = pit_ticks();
    for (uint32_t i = 0; i < SCHEDULER_MAX_PROCESSES; i++) {
        task_t *task = &tasks[i];
        if (task->state == TASK_SLEEPING &&
            (int32_t)(now - task->wake_tick) >= 0)
            task->state = TASK_RUNNABLE;
    }
}

static void scheduler_idle_loop(void) {
    for (;;) {
        /* Runs only while no user task is runnable, so this is the
         * post-user hook: verify FS error reporting once, then idle.
         * The volume stays usable for the test because every user
         * file I/O check has already finished. */
        fs_error_self_test();
        __asm {
            sti
            hlt
        }
    }
}

static int initialize_stack(task_t *task, int user_mode) {
    uint32_t stack_bytes = TASK_KERNEL_STACK_PAGES * FRAME_SIZE;
    uint32_t stack_top;
    regs_t *frame;

    task->kernel_stack = (uint32_t)frame_alloc_pages(TASK_KERNEL_STACK_PAGES);
    if (!task->kernel_stack ||
        task->kernel_stack + stack_bytes > PAGING_PHYS_LIMIT) {
        if (task->kernel_stack)
            frame_free_pages((void *)task->kernel_stack,
                             TASK_KERNEL_STACK_PAGES);
        task->kernel_stack = 0;
        return 0;
    }

    /* The frame lives on the direct-mapped view; the physical address is
     * what release_task_resources() gives back to the allocator. */
    stack_top = (uint32_t)PHYS_TO_VIRT(task->kernel_stack) + stack_bytes;
    frame = (regs_t *)(stack_top - sizeof(regs_t));
    memset(frame, 0, sizeof(*frame));
    frame->gs = user_mode ? USER_DS : KERNEL_DS;
    frame->fs = user_mode ? USER_DS : KERNEL_DS;
    frame->es = user_mode ? USER_DS : KERNEL_DS;
    frame->ds = user_mode ? USER_DS : KERNEL_DS;
    frame->int_no = 0;
    frame->err_code = 0;
    frame->eip = user_mode ? task->program.entry :
                             (uint32_t)scheduler_idle_loop;
    frame->cs = user_mode ? USER_CS : KERNEL_CS;
    frame->eflags = 0x202u;
    if (user_mode) {
        frame->user_esp = task->program.user_stack;
        frame->user_ss = USER_DS;
    }
    task->frame = frame;
    return 1;
}

static int find_next_task(void) {
    for (uint32_t offset = 1; offset <= SCHEDULER_MAX_PROCESSES; offset++) {
        uint32_t selected_index;
        if (current_task == SCHEDULER_IDLE_INDEX)
            selected_index = offset - 1u;
        else
            selected_index = ((uint32_t)current_task + offset) %
                             SCHEDULER_MAX_PROCESSES;
        if (tasks[selected_index].state == TASK_RUNNABLE)
            return (int)selected_index;
    }
    if (current_task >= 0 && current_task < SCHEDULER_IDLE_INDEX &&
        tasks[current_task].state == TASK_RUNNABLE)
        return current_task;
    return SCHEDULER_IDLE_INDEX;
}

static void activate_task(int selected_index) {
    task_t *task;
    /* Count the switch out of the previous task here, where it actually
     * happens: a runnable task preempted by the timer is involuntary,
     * while blocking/sleeping/waiting tasks yielded voluntarily. Exiting
     * (zombie) tasks are not context switches. */
    if (current_task >= 0 && current_task < SCHEDULER_IDLE_INDEX) {
        task_t *previous = &tasks[current_task];
        if (previous->state == TASK_RUNNABLE)
            previous->involuntary_switches++;
        else if (previous->state == TASK_BLOCKED ||
                 previous->state == TASK_WAITING ||
                 previous->state == TASK_SLEEPING)
            previous->voluntary_switches++;
    }
    current_task = selected_index;
    if (selected_index == SCHEDULER_IDLE_INDEX) {
        task = &idle_task;
        paging_activate_kernel_space();
    } else {
        task = &tasks[selected_index];
        paging_activate_user_space(&task->program.address_space);
    }
    gdt_set_kernel_stack((uint32_t)PHYS_TO_VIRT(task->kernel_stack) +
                         TASK_KERNEL_STACK_PAGES * FRAME_SIZE);
}

static void release_task_resources(task_t *task) {
    if (task->program.address_space.directory_phys)
        paging_destroy_user_space(&task->program.address_space);
    if (task->kernel_stack) {
        frame_free_pages((void *)task->kernel_stack,
                         TASK_KERNEL_STACK_PAGES);
        task->kernel_stack = 0;
    }
    task->frame = 0;
}

/* Free the slots of children that their parent already reaped. The
 * timer-driven reap does this too, but a new task must not depend on a
 * timer tick landing in the right window: the process table would
 * otherwise run out of slots while a freed one is still pending. */
static void release_reaped_slots(void) {
    for (uint32_t i = 0; i < SCHEDULER_MAX_PROCESSES; i++) {
        task_t *task = &tasks[i];
        if (task->state != TASK_REAP_PENDING || (int)i == current_task)
            continue;
        release_task_resources(task);
        memset(task, 0, sizeof(*task));
    }
}

static void reap_completed_tasks(void) {
    for (uint32_t i = 0; i < SCHEDULER_MAX_PROCESSES; i++) {
        task_t *task = &tasks[i];
        if ((task->state != TASK_ZOMBIE &&
             task->state != TASK_REAP_PENDING) ||
            (int)i == current_task)
            continue;
        release_task_resources(task);
        if (task->state == TASK_REAP_PENDING) {
            klog_prefix();
            klog_write("SCHED-REAP-OK: reaped pid=");
            klog_dec(task->pid);
            klog_write("\n");
            memset(task, 0, sizeof(*task));
        }
    }
}

int scheduler_init(loaded_elf_t *initial_program) {
    if (!initial_program || !initial_program->address_space.directory_phys ||
        !initial_program->entry || !initial_program->user_stack)
        return 0;

    memset(tasks, 0, sizeof(tasks));
    memset(&idle_task, 0, sizeof(idle_task));
    current_task = -1;
    scheduler_started = 0;
    timer_marker_written = 0;
    waitq_marker_written = 0;

    tasks[0].pid = 1;
    tasks[0].parent_pid = 0;
    tasks[0].state = TASK_RUNNABLE;
    tasks[0].program = *initial_program;
    if (!initialize_stack(&tasks[0], 1)) goto fail;
    /* The initial process owns the standard descriptors; every spawned
     * child inherits them from its parent. */
    if (!vfs_open_stdio(tasks[0].pid)) goto fail;
    idle_task.state = TASK_RUNNABLE;
    if (!initialize_stack(&idle_task, 0)) goto fail;
    initial_program->address_space.directory_phys = 0;

    klog_prefix();
    klog_write("sched: SCHED-READY tasks=1");
    klog_write(" kernel-stack=per-task\n");
    klog_prefix();
    klog_write("STDIO-OK: fds 0/1/2 on console\n");
    return 1;

fail:
    vfs_close_all(1);
    if (tasks[0].kernel_stack)
        frame_free_pages((void *)tasks[0].kernel_stack,
                         TASK_KERNEL_STACK_PAGES);
    memset(&tasks[0], 0, sizeof(tasks[0]));
    if (idle_task.kernel_stack)
        frame_free_pages((void *)idle_task.kernel_stack,
                         TASK_KERNEL_STACK_PAGES);
    memset(&idle_task, 0, sizeof(idle_task));
    return 0;
}

void scheduler_start(void) {
    int first = 0;
    (void)irq_lock();
    scheduler_started = 1;
    activate_task(first);
    klog_prefix();
    klog_write("sched: SCHED-START pid=");
    klog_dec(tasks[first].pid);
    klog_write(" (first task)\n");
    scheduler_enter_frame(tasks[first].frame);
    for (;;) __asm hlt
}

/* The scheduler's own state is protected by the IRQ-save rule below:
 * every task-array mutation runs with interrupts disabled (either in an IRQ
 * handler or under irq_lock()), so no spinlock is needed here. Never take a
 * task-context spinlock (VFS, ATA) with interrupts disabled: a preempted
 * holder would never run again to release it. */
regs_t *scheduler_preempt(regs_t *frame) {
    int next;

    if (!scheduler_started || !frame)
        return frame;
    /* Attribute this timer tick to the interrupted task. The frame's CS
     * tells us whether it was running in ring 3 or in a syscall/IRQ. */
    if (current_task >= 0 && current_task < SCHEDULER_IDLE_INDEX) {
        task_t *current = &tasks[current_task];
        if ((frame->cs & 3u) == 3u)
            current->user_ticks++;
        else
            current->kernel_ticks++;
    }
    if (!timer_marker_written) {
        klog_prefix();
        klog_write("sched: SCHED-TIMER-OK: timer interrupts reach scheduler\n");
        timer_marker_written = 1;
    }

    reap_completed_tasks();
    wake_expired_sleepers();
    if (current_task == SCHEDULER_IDLE_INDEX) {
        idle_task.frame = frame;
    } else if (current_task >= 0 &&
               current_task < SCHEDULER_IDLE_INDEX) {
        tasks[current_task].frame = frame;
    }

    next = find_next_task();
    /* Observable proof that a blocked task is off the run queue: when a
     * task parks on a wait queue and nothing else is runnable, the idle
     * context is selected instead of scheduling that task again. */
    if (!waitq_marker_written && next == SCHEDULER_IDLE_INDEX) {
        for (uint32_t i = 0; i < SCHEDULER_MAX_PROCESSES; i++) {
            if (tasks[i].state != TASK_BLOCKED) continue;
            klog_prefix();
            klog_write("sched: SCHED-WAITQ-OK: pid=");
            klog_dec(tasks[i].pid);
            klog_write(" parked on a wait queue, idle scheduled\n");
            waitq_marker_written = 1;
            break;
        }
    }
    if (next == current_task)
        return frame;

    activate_task(next);
    return next == SCHEDULER_IDLE_INDEX ? idle_task.frame :
                                          tasks[next].frame;
}

/* Hand the wait result to the parent: the exit status and, when the
 * parent asked for one, a zeroed rusage block. The parent's return value
 * is the reaped child pid, or -EFAULT when a user pointer is invalid. */
static int deliver_wait_result(task_t *parent, task_t *child, uint32_t status) {
    haku_rusage_t usage;
    if (parent->wait_status_user &&
        !paging_copy_to_user_space(&parent->program.address_space,
                                   parent->wait_status_user, &status,
                                   sizeof(status)))
        return 0;
    if (parent->wait_rusage_user) {
        memset(&usage, 0, sizeof(usage));
        if (!paging_copy_to_user_space(&parent->program.address_space,
                                       parent->wait_rusage_user, &usage,
                                       sizeof(usage)))
            return 0;
    }
    parent->frame->eax = child->pid;
    return 1;
}

regs_t *scheduler_exit_current(regs_t *frame) {
    int next;
    task_t *exiting;
    uint32_t status;
    if (!scheduler_started || current_task < 0 ||
        current_task >= SCHEDULER_IDLE_INDEX)
        return frame;

    exiting = &tasks[current_task];
    /* Closing descriptors takes the task-context VFS lock, so it must run
     * with interrupts enabled. The task-state transition below is then
     * committed under the scheduler's IRQ-save rule: interrupts stay off
     * through the context switch, and iret restores the next task's IF. */
    vfs_close_all(exiting->pid);
    (void)irq_lock();
    exiting->exit_status = (frame->ebx & 0xFFu) << 8;
    exiting->state = TASK_ZOMBIE;
    exiting->frame = frame;
    status = exiting->exit_status;

    for (uint32_t i = 0; i < SCHEDULER_MAX_PROCESSES; i++) {
        task_t *child = &tasks[i];
        if (child->state == TASK_UNUSED ||
            child->parent_pid != exiting->pid)
            continue;
        child->parent_pid = exiting->pid == 1u ? 0u : 1u;
    }

    if (exiting->parent_pid == 0) {
        exiting->state = TASK_REAP_PENDING;
    } else {
        for (uint32_t i = 0; i < SCHEDULER_MAX_PROCESSES; i++) {
            task_t *parent = &tasks[i];
            int pid_matches;
            if (parent->pid != exiting->parent_pid ||
                parent->state != TASK_WAITING)
                continue;
            pid_matches = parent->wait_pid == 0xFFFFFFFFu ||
                          parent->wait_pid == exiting->pid;
            if (!pid_matches) continue;
            if (!deliver_wait_result(parent, exiting, status))
                parent->frame->eax = 0u - 14u;
            parent->state = TASK_RUNNABLE;
            parent->wait_pid = 0;
            parent->wait_status_user = 0;
            parent->wait_rusage_user = 0;
            exiting->state = TASK_REAP_PENDING;
            break;
        }
    }
    next = find_next_task();
    activate_task(next);
    return next == SCHEDULER_IDLE_INDEX ? idle_task.frame :
                                          tasks[next].frame;
}

int scheduler_exec_current(loaded_elf_t *replacement, regs_t *frame) {
    task_t *task;
    loaded_elf_t previous;
    uint32_t inherited_peak;
    irqflags_t flags;
    if (!scheduler_started || !replacement || !frame ||
        !replacement->address_space.directory_phys ||
        !replacement->entry || !replacement->user_stack ||
        current_task < 0 || current_task >= SCHEDULER_IDLE_INDEX ||
        (frame->cs & 3u) != 3u)
        return 0;

    flags = irq_lock();
    task = &tasks[current_task];
    previous = task->program;
    inherited_peak = previous.address_space.peak_mapped_pages;
    task->program = *replacement;
    /* The process keeps its lifetime peak across an image replacement;
     * the new image's current page counts start from its own mappings. */
    if (task->program.address_space.peak_mapped_pages < inherited_peak)
        task->program.address_space.peak_mapped_pages = inherited_peak;
    replacement->address_space.directory_phys = 0;
    /* A fresh image starts with a fresh, lazily-mapped heap and no
     * anonymous mappings. */
    task->heap_brk = 0;
    task->heap_top = 0;
    task->mmap_region_count = 0;

    frame->gs = USER_DS;
    frame->fs = USER_DS;
    frame->es = USER_DS;
    frame->ds = USER_DS;
    frame->edi = 0;
    frame->esi = 0;
    frame->ebp = 0;
    frame->esp_dummy = 0;
    frame->ebx = 0;
    frame->edx = 0;
    frame->ecx = 0;
    frame->eax = 0;
    frame->eip = task->program.entry;
    frame->cs = USER_CS;
    frame->eflags = 0x202u;
    frame->user_esp = task->program.user_stack;
    frame->user_ss = USER_DS;
    task->frame = frame;

    paging_activate_user_space(&task->program.address_space);
    paging_destroy_user_space(&previous.address_space);
    irq_unlock(flags);

    klog_prefix();
    klog_write("exec: EXECVE-OK pid=");
    klog_dec(task->pid);
    klog_write(" replaced image, descriptors preserved\n");
    return 1;
}

uint32_t scheduler_current_pid(void) {
    if (current_task < 0 || current_task >= SCHEDULER_IDLE_INDEX)
        return 0;
    return tasks[current_task].pid;
}

uint32_t scheduler_parent_pid(void) {
    if (current_task < 0 || current_task >= SCHEDULER_IDLE_INDEX)
        return 0;
    return tasks[current_task].parent_pid;
}

void scheduler_note_syscall(void) {
    if (current_task >= 0 && current_task < SCHEDULER_IDLE_INDEX)
        tasks[current_task].syscall_count++;
}

/* Fill a getusage(0x1001) snapshot for the calling process. Memory counts
 * come from the address space's incremental accounting; resource counts are
 * updated by the timer IRQ, syscall entry, and blocking paths. */
int scheduler_get_usage(haku_usage_t *usage) {
    task_t *task;
    address_space_t *space;
    uint32_t kernel_pages;
    if (!usage || current_task < 0 ||
        current_task >= SCHEDULER_IDLE_INDEX)
        return -(int32_t)HAKU_EINVAL;

    task = &tasks[current_task];
    space = &task->program.address_space;
    memset(usage, 0, sizeof(*usage));
    usage->pid = task->pid;
    usage->user_ticks = task->user_ticks;
    usage->kernel_ticks = task->kernel_ticks;
    usage->syscalls = task->syscall_count;
    usage->voluntary_switches = task->voluntary_switches;
    usage->involuntary_switches = task->involuntary_switches;
    usage->mapped_pages = space->mapped_pages;
    usage->peak_pages = space->peak_mapped_pages;
    usage->page_tables = space->page_tables;
    usage->kernel_stack_pages = TASK_KERNEL_STACK_PAGES;
    if (task->heap_brk > USER_HEAP_BASE)
        usage->heap_bytes = task->heap_brk - USER_HEAP_BASE;
    for (uint32_t i = 0; i < task->mmap_region_count; i++)
        usage->mmap_bytes += task->mmap_regions[i].size;

    usage->resident_bytes = usage->mapped_pages * FRAME_SIZE;
    kernel_pages = usage->page_tables + 1u + usage->kernel_stack_pages;
    usage->kernel_bytes = kernel_pages * FRAME_SIZE;
    usage->total_bytes = usage->resident_bytes + usage->kernel_bytes;
    usage->peak_bytes = usage->peak_pages * FRAME_SIZE;
    return 0;
}

/* ------------------------------------------------------------------ */
/* Wait queues.                                                       */
/*                                                                    */
/* Parking a task is just a state transition: it keeps its kernel     */
/* stack, and when the event fires the scheduler resumes it right      */
/* after the hlt that follows wait_queue_sleep. No frame is crafted    */
/* here; either the waking IRQ returns straight to the halted task or  */
/* the timer IRQ saves that interrupted frame for the next activation. */
/* ------------------------------------------------------------------ */
static int current_slot(void) {
    if (current_task < 0 || current_task >= SCHEDULER_IDLE_INDEX)
        return -1;
    return current_task;
}

int wait_queue_sleep(wait_queue_t *queue) {
    int slot;
    if (!queue) return 0;
    slot = current_slot();
    if (slot < 0) return 0;
    queue->waiters |= 1u << (uint32_t)slot;
    tasks[slot].state = TASK_BLOCKED;
    return 1;
}

void wait_queue_cancel(wait_queue_t *queue) {
    int slot;
    if (!queue) return;
    slot = current_slot();
    if (slot < 0) return;
    queue->waiters &= ~(1u << (uint32_t)slot);
    if (tasks[slot].state == TASK_BLOCKED)
        tasks[slot].state = TASK_RUNNABLE;
}

int wait_queue_wake_all(wait_queue_t *queue) {
    uint32_t mask;
    int woken = 0;
    if (!queue || !queue->waiters) return 0;
    /* Drop the whole bitmap first: a waiter that already cancelled must
     * not be woken again by a stale bit. */
    mask = queue->waiters;
    queue->waiters = 0;
    for (uint32_t i = 0; i < SCHEDULER_MAX_PROCESSES; i++) {
        if (!(mask & (1u << i))) continue;
        if (tasks[i].state != TASK_BLOCKED) continue;
        tasks[i].state = TASK_RUNNABLE;
        woken++;
    }
    return woken;
}

int32_t scheduler_spawn(loaded_elf_t *program) {
    irqflags_t flags;
    uint32_t parent_pid;
    uint32_t slot;
    task_t *child = 0;

    if (!scheduler_started || !program ||
        !program->address_space.directory_phys ||
        !program->entry || !program->user_stack ||
        current_task < 0 || current_task >= SCHEDULER_IDLE_INDEX)
        return -(int32_t)HAKU_ENOMEM;

    /* Reserve a slot without holding interrupts across the VFS inherit:
     * vfs_inherit takes the task-context VFS spinlock, which must never be
     * acquired with interrupts disabled. A STARTING task is ignored by the
     * scheduler until it is fully built. */
    flags = irq_lock();
    release_reaped_slots();
    parent_pid = tasks[current_task].pid;
    for (slot = 1; slot < SCHEDULER_MAX_PROCESSES; slot++) {
        if (tasks[slot].state == TASK_UNUSED) {
            child = &tasks[slot];
            break;
        }
    }
    if (!child) {
        irq_unlock(flags);
        return -(int32_t)HAKU_EAGAIN;
    }
    memset(child, 0, sizeof(*child));
    child->pid = slot + 1u;
    child->parent_pid = parent_pid;
    child->state = TASK_STARTING;
    irq_unlock(flags);

    child->program = *program;
    if (!initialize_stack(child, 1)) {
        flags = irq_lock();
        memset(child, 0, sizeof(*child));
        irq_unlock(flags);
        return -12;
    }
    /* Inherit the parent's descriptors: the child shares the same open
     * file descriptions (refcounted), so its standard handles and any
     * redirections work from the first instruction. They are released
     * together when the child exits. */
    if (vfs_inherit(parent_pid, child->pid) != 1) {
        flags = irq_lock();
        frame_free_pages((void *)child->kernel_stack,
                         TASK_KERNEL_STACK_PAGES);
        memset(child, 0, sizeof(*child));
        irq_unlock(flags);
        return -(int32_t)HAKU_EMFILE;
    }
    program->address_space.directory_phys = 0;

    flags = irq_lock();
    child->state = TASK_RUNNABLE;
    irq_unlock(flags);
    klog_prefix();
    klog_write("sched: SPAWN-OK parent=");
    klog_dec(parent_pid);
    klog_write(" child=");
    klog_dec(child->pid);
    klog_write("\n");
    return (int32_t)child->pid;
}

/* Build the child's kernel stack and register frame for a fork: the frame
 * is a copy of the parent's syscall frame, so the child resumes at the
 * same instruction with eax = 0 (the fork return value). */
static int initialize_fork_stack(task_t *task, const regs_t *parent_frame) {
    uint32_t stack_bytes = TASK_KERNEL_STACK_PAGES * FRAME_SIZE;
    uint32_t stack_top;
    regs_t *frame;

    task->kernel_stack = (uint32_t)frame_alloc_pages(TASK_KERNEL_STACK_PAGES);
    if (!task->kernel_stack ||
        task->kernel_stack + stack_bytes > PAGING_PHYS_LIMIT) {
        if (task->kernel_stack)
            frame_free_pages((void *)task->kernel_stack,
                             TASK_KERNEL_STACK_PAGES);
        task->kernel_stack = 0;
        return 0;
    }
    stack_top = (uint32_t)PHYS_TO_VIRT(task->kernel_stack) + stack_bytes;
    frame = (regs_t *)(stack_top - sizeof(regs_t));
    memcpy(frame, parent_frame, sizeof(*frame));
    frame->eax = 0u;
    task->frame = frame;
    return 1;
}

int32_t scheduler_fork(regs_t *frame) {
    irqflags_t flags;
    uint32_t parent_pid;
    uint32_t slot;
    task_t *child = 0;
    task_t *parent;

    if (!scheduler_started || !frame || current_task < 0 ||
        current_task >= SCHEDULER_IDLE_INDEX)
        return -(int32_t)HAKU_EINVAL;
    if ((frame->cs & 3u) != 3u)
        return -(int32_t)HAKU_EINVAL;
    parent = &tasks[current_task];

    /* Reserve a slot first: a STARTING task is ignored by the scheduler
     * until it is fully built. */
    flags = irq_lock();
    release_reaped_slots();
    parent_pid = parent->pid;
    for (slot = 1; slot < SCHEDULER_MAX_PROCESSES; slot++) {
        if (tasks[slot].state == TASK_UNUSED) {
            child = &tasks[slot];
            break;
        }
    }
    if (!child) {
        irq_unlock(flags);
        return -(int32_t)HAKU_EAGAIN;
    }
    memset(child, 0, sizeof(*child));
    child->pid = slot + 1u;
    child->parent_pid = parent_pid;
    child->state = TASK_STARTING;
    irq_unlock(flags);

    /* The child gets its own copy of the user address space: same pages,
     * same contents, separate frames (no copy-on-write yet). */
    if (!paging_create_user_space(&child->program.address_space))
        goto failed;
    if (!paging_copy_user_space(&parent->program.address_space,
                                &child->program.address_space))
        goto failed;
    child->program.entry = parent->program.entry;
    child->program.user_stack = parent->program.user_stack;

    if (!initialize_fork_stack(child, frame))
        goto failed;

    /* The child inherits the parent's descriptors and shares the same open
     * file descriptions (refcounted), exactly like a Linux fork. */
    if (vfs_inherit(parent_pid, child->pid) != 1)
        goto failed;

    /* The heap break and the anonymous mappings belong to the child too. */
    child->heap_brk = parent->heap_brk;
    child->heap_top = parent->heap_top;
    child->mmap_region_count = parent->mmap_region_count;
    memcpy(child->mmap_regions, parent->mmap_regions,
           sizeof(child->mmap_regions));

    flags = irq_lock();
    child->state = TASK_RUNNABLE;
    irq_unlock(flags);
    klog_prefix();
    klog_write("sched: FORK-OK parent=");
    klog_dec(parent_pid);
    klog_write(" child=");
    klog_dec(child->pid);
    klog_write("\n");
    return (int32_t)child->pid;

failed:
    /* The partially built child is discarded whole: its address space and
     * kernel stack are released and the slot returns to the table. It is
     * still TASK_STARTING here, so the scheduler never runs it. */
    release_task_resources(child);
    flags = irq_lock();
    memset(child, 0, sizeof(*child));
    irq_unlock(flags);
    return -(int32_t)HAKU_ENOMEM;
}

regs_t *scheduler_waitpid(uint32_t pid, uint32_t user_status,
                          uint32_t options, regs_t *frame) {
    task_t *parent;
    task_t *zombie = 0;
    int has_child = 0;
    int next;
    irqflags_t flags;

    if (!frame || current_task < 0 ||
        current_task >= SCHEDULER_IDLE_INDEX)
        return frame;
    flags = irq_lock();
    if (options != 0 || pid == 0) {
        frame->eax = 0u - HAKU_EINVAL;
        irq_unlock(flags);
        return frame;
    }
    if (pid != 0xFFFFFFFFu && pid > 0x7FFFFFFFu) {
        frame->eax = 0u - HAKU_EINVAL;
        irq_unlock(flags);
        return frame;
    }
    if (pid != 0xFFFFFFFFu && pid > SCHEDULER_MAX_PROCESSES) {
        frame->eax = 0u - HAKU_ECHILD;
        irq_unlock(flags);
        return frame;
    }

    parent = &tasks[current_task];
    for (uint32_t i = 0; i < SCHEDULER_MAX_PROCESSES; i++) {
        task_t *child = &tasks[i];
        if (child->state == TASK_UNUSED ||
            child->state == TASK_REAP_PENDING ||
            child->parent_pid != parent->pid)
            continue;
        if (pid != 0xFFFFFFFFu && child->pid != pid) continue;
        has_child = 1;
        if (child->state == TASK_ZOMBIE) {
            zombie = child;
            break;
        }
    }
    if (!has_child) {
        frame->eax = 0u - HAKU_ECHILD;
        irq_unlock(flags);
        return frame;
    }

    if (zombie) {
        parent->wait_status_user = user_status;
        parent->wait_rusage_user = 0;
        if (!deliver_wait_result(parent, zombie, zombie->exit_status)) {
            frame->eax = 0u - 14u;
            irq_unlock(flags);
            return frame;
        }
        zombie->state = TASK_REAP_PENDING;
        irq_unlock(flags);
        return frame;
    }

    parent->state = TASK_WAITING;
    parent->wait_pid = pid;
    parent->wait_status_user = user_status;
    parent->wait_rusage_user = 0;
    parent->frame = frame;
    next = find_next_task();
    activate_task(next);
    return next == SCHEDULER_IDLE_INDEX ? idle_task.frame :
                                          tasks[next].frame;
}

/* wait4(2): the Linux form of waitpid. It accepts WNOHANG, treats pid 0
 * as "any child" (Haku has no process groups), and writes a rusage block
 * when the caller passes one. */
regs_t *scheduler_wait4(uint32_t pid, uint32_t user_status, uint32_t options,
                        uint32_t user_rusage, regs_t *frame) {
    task_t *parent;
    task_t *zombie = 0;
    int has_child = 0;
    int next;
    irqflags_t flags;

    if (!frame || current_task < 0 ||
        current_task >= SCHEDULER_IDLE_INDEX)
        return frame;
    flags = irq_lock();
    if (options & ~(uint32_t)HAKU_WNOHANG) {
        frame->eax = 0u - HAKU_EINVAL;
        irq_unlock(flags);
        return frame;
    }
    if (pid == 0) pid = 0xFFFFFFFFu;
    if (pid != 0xFFFFFFFFu && pid > 0x7FFFFFFFu) {
        frame->eax = 0u - HAKU_EINVAL;
        irq_unlock(flags);
        return frame;
    }
    if (pid != 0xFFFFFFFFu && pid > SCHEDULER_MAX_PROCESSES) {
        frame->eax = 0u - HAKU_ECHILD;
        irq_unlock(flags);
        return frame;
    }

    parent = &tasks[current_task];
    for (uint32_t i = 0; i < SCHEDULER_MAX_PROCESSES; i++) {
        task_t *child = &tasks[i];
        if (child->state == TASK_UNUSED ||
            child->state == TASK_REAP_PENDING ||
            child->parent_pid != parent->pid)
            continue;
        if (pid != 0xFFFFFFFFu && child->pid != pid) continue;
        has_child = 1;
        if (child->state == TASK_ZOMBIE) {
            zombie = child;
            break;
        }
    }
    if (!has_child) {
        frame->eax = 0u - HAKU_ECHILD;
        irq_unlock(flags);
        return frame;
    }

    if (zombie) {
        parent->wait_status_user = user_status;
        parent->wait_rusage_user = user_rusage;
        if (!deliver_wait_result(parent, zombie, zombie->exit_status)) {
            frame->eax = 0u - 14u;
            parent->wait_rusage_user = 0;
            irq_unlock(flags);
            return frame;
        }
        parent->wait_rusage_user = 0;
        zombie->state = TASK_REAP_PENDING;
        irq_unlock(flags);
        return frame;
    }

    if (options & HAKU_WNOHANG) {
        /* No child has exited yet: report "nothing ready" and stay
         * runnable instead of blocking. */
        frame->eax = 0u;
        irq_unlock(flags);
        return frame;
    }

    parent->state = TASK_WAITING;
    parent->wait_pid = pid;
    parent->wait_status_user = user_status;
    parent->wait_rusage_user = user_rusage;
    parent->frame = frame;
    next = find_next_task();
    activate_task(next);
    return next == SCHEDULER_IDLE_INDEX ? idle_task.frame :
                                          tasks[next].frame;
}

regs_t *scheduler_sleep_ticks(uint32_t ticks, regs_t *frame) {
    task_t *task;
    int next;

    if (!frame || current_task < 0 ||
        current_task >= SCHEDULER_IDLE_INDEX)
        return frame;
    if (ticks == 0) {
        frame->eax = 0;
        return frame;
    }

    (void)irq_lock();
    task = &tasks[current_task];
    frame->eax = 0;
    task->wake_tick = pit_ticks() + ticks;
    task->state = TASK_SLEEPING;
    task->frame = frame;
    next = find_next_task();
    activate_task(next);
    /* IRQs stay masked here; the returned frame's eflags restore them
     * when the isr return path executes iret. */
    return next == SCHEDULER_IDLE_INDEX ? idle_task.frame :
                                          tasks[next].frame;
}

/* Map [start, end) as zero-filled user pages in the given address space
 * (page-aligned range), writable when `writable` is set. On failure the
 * pages mapped by this call are unmapped again, so the caller can report
 * -ENOMEM without leaking address space or frames. */
static int map_user_zero_pages(address_space_t *space, uint32_t start,
                               uint32_t end, int writable) {
    uint32_t mapped = start;
    for (uint32_t address = start; address < end; address += USER_PAGE_SIZE) {
        void *page = frame_alloc();
        uint32_t physical;
        if (!page) break;
        physical = (uint32_t)page;
        memset(PHYS_TO_VIRT(physical), 0, USER_PAGE_SIZE);
        if (!paging_map_user_page(space, address, physical, writable)) {
            frame_free(page);
            break;
        }
        mapped = address + USER_PAGE_SIZE;
    }
    if (mapped == end) return 1;
    while (mapped > start) {
        mapped -= USER_PAGE_SIZE;
        paging_unmap_user_page(space, mapped);
    }
    return 0;
}

/* brk(2) with a fixed heap region: grow or shrink the per-process break,
 * mapping zero-filled pages as it rises and releasing them as it falls.
 * The break is page-rounded and never returns below USER_HEAP_BASE. A zero
 * request just queries the current break; an invalid size returns
 * -EINVAL/-ENOMEM. */
uint32_t scheduler_brk(uint32_t requested) {
    task_t *task;
    address_space_t *space;
    uint32_t current;
    uint32_t rounded;

    if (current_task < 0 || current_task >= SCHEDULER_IDLE_INDEX)
        return 0u - HAKU_EINVAL;
    task = &tasks[current_task];
    space = &task->program.address_space;
    if (!space->directory_phys) return 0u - HAKU_EINVAL;
    if (task->heap_brk == 0) task->heap_brk = USER_HEAP_BASE;
    if (task->heap_top == 0) task->heap_top = USER_HEAP_BASE;
    current = task->heap_brk;
    if (requested == 0) return current;
    if (requested < USER_HEAP_BASE) return 0u - HAKU_EINVAL;
    if (requested > USER_HEAP_LIMIT) return 0u - HAKU_ENOMEM;

    rounded = (requested + USER_PAGE_SIZE - 1u) & 0xFFFFF000u;
    if (rounded > task->heap_top) {
        if (!map_user_zero_pages(space, task->heap_top, rounded, 1))
            return 0u - HAKU_ENOMEM;
        task->heap_top = rounded;
    } else if (rounded < task->heap_top) {
        for (uint32_t address = rounded; address < task->heap_top;
             address += USER_PAGE_SIZE)
            paging_unmap_user_page(space, address);
        task->heap_top = rounded;
    }
    task->heap_brk = rounded;
    return rounded;
}

/* First fit for a page-aligned anonymous mapping, scanning the sorted
 * region list so released address space is reused. Returns 0 when the
 * mapping does not fit. */
static uint32_t mmap_find_gap(const task_t *task, uint32_t size) {
    uint32_t candidate = USER_MMAP_BASE;
    for (uint32_t i = 0; i < task->mmap_region_count; i++) {
        uint32_t base = task->mmap_regions[i].base;
        uint32_t end = base + task->mmap_regions[i].size;
        if (candidate + size <= base) return candidate;
        if (candidate < end) candidate = end;
    }
    if (candidate > USER_MMAP_LIMIT ||
        size > USER_MMAP_LIMIT - candidate)
        return 0;
    return candidate;
}

static void mmap_insert_region(task_t *task, uint32_t base, uint32_t size) {
    uint32_t index = 0;
    while (index < task->mmap_region_count &&
           task->mmap_regions[index].base < base)
        index++;
    for (uint32_t i = task->mmap_region_count; i > index; i--)
        task->mmap_regions[i] = task->mmap_regions[i - 1u];
    task->mmap_regions[index].base = base;
    task->mmap_regions[index].size = size;
    task->mmap_region_count++;
}

/* Limited mmap(2): anonymous private pages from the per-process mmap
 * region, always zero-filled; PROT_WRITE decides whether the PTEs carry
 * R/W. Returns the mapped base address or -errno. */
uint32_t scheduler_mmap(uint32_t length, int writable) {
    task_t *task;
    address_space_t *space;
    uint32_t rounded;
    uint32_t base;

    if (current_task < 0 || current_task >= SCHEDULER_IDLE_INDEX)
        return 0u - HAKU_EINVAL;
    if (length == 0 || length > USER_MMAP_LIMIT - USER_MMAP_BASE)
        return 0u - (length == 0 ? HAKU_EINVAL : HAKU_ENOMEM);
    task = &tasks[current_task];
    space = &task->program.address_space;
    if (!space->directory_phys) return 0u - HAKU_EINVAL;
    if (task->mmap_region_count >= TASK_MAX_MMAP_REGIONS)
        return 0u - HAKU_ENOMEM;

    rounded = (length + USER_PAGE_SIZE - 1u) & 0xFFFFF000u;
    base = mmap_find_gap(task, rounded);
    if (!base) return 0u - HAKU_ENOMEM;
    if (!map_user_zero_pages(space, base, base + rounded, writable))
        return 0u - HAKU_ENOMEM;
    mmap_insert_region(task, base, rounded);
    return base;
}

/* munmap(2) for anonymous mappings: the range must lie inside exactly one
 * tracked region. Partial unmap trims the region or splits it, and the
 * released pages (plus empty page tables) return to the frame allocator.
 * Returns 0 or -errno. */
uint32_t scheduler_munmap(uint32_t address, uint32_t length) {
    task_t *task;
    address_space_t *space;
    uint32_t start;
    uint32_t end;
    uint32_t index = TASK_MAX_MMAP_REGIONS;
    uint32_t base = 0;
    uint32_t size = 0;
    int split;

    if (current_task < 0 || current_task >= SCHEDULER_IDLE_INDEX)
        return 0u - HAKU_EINVAL;
    if (length == 0 || address > 0xFFFFFFFFu - length)
        return 0u - HAKU_EINVAL;
    task = &tasks[current_task];
    space = &task->program.address_space;
    if (!space->directory_phys) return 0u - HAKU_EINVAL;

    start = address & 0xFFFFF000u;
    end = address + length;
    if ((end & 0xFFFu) != 0) {
        if (end > 0xFFFFF000u) return 0u - HAKU_EINVAL;
        end = (end + USER_PAGE_SIZE - 1u) & 0xFFFFF000u;
    }
    if (end <= start || start < USER_MMAP_BASE || end > USER_MMAP_LIMIT)
        return 0u - HAKU_EINVAL;

    for (uint32_t i = 0; i < task->mmap_region_count; i++) {
        base = task->mmap_regions[i].base;
        size = task->mmap_regions[i].size;
        if (start >= base && end <= base + size) {
            index = i;
            break;
        }
    }
    if (index == TASK_MAX_MMAP_REGIONS) return 0u - HAKU_EINVAL;

    /* A middle cut becomes two regions: check for a free slot before any
     * page is released so a failure cannot leave the table inconsistent. */
    split = start > base && end < base + size;
    if (split && task->mmap_region_count >= TASK_MAX_MMAP_REGIONS)
        return 0u - HAKU_EINVAL;

    for (uint32_t page = start; page < end; page += USER_PAGE_SIZE)
        paging_unmap_user_page(space, page);

    if (start == base && end == base + size) {
        for (uint32_t i = index; i + 1u < task->mmap_region_count; i++)
            task->mmap_regions[i] = task->mmap_regions[i + 1u];
        task->mmap_region_count--;
    } else if (start == base) {
        task->mmap_regions[index].base = end;
        task->mmap_regions[index].size = base + size - end;
    } else if (end == base + size) {
        task->mmap_regions[index].size = start - base;
    } else {
        uint32_t old_end = base + size;
        task->mmap_regions[index].size = start - base;
        mmap_insert_region(task, end, old_end - end);
    }
    return 0;
}
