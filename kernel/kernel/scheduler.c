#include "scheduler.h"
#include "console.h"
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

typedef enum {
    TASK_UNUSED,
    TASK_STARTING,
    TASK_RUNNABLE,
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
    uint32_t wake_tick;
    uint32_t heap_brk;    /* current program break (0 = not yet used) */
    uint32_t heap_top;    /* highest heap page ever mapped (grow only) */
    uint32_t mmap_next;   /* next anonymous mmap address (0 = not yet used) */
} task_t;

#define SCHEDULER_IDLE_INDEX ((int)SCHEDULER_MAX_PROCESSES)
static task_t tasks[SCHEDULER_MAX_PROCESSES];
static task_t idle_task;
static int current_task = -1;
static int scheduler_started;
static int timer_marker_written;

int scheduler_user_tasks_done(void) {
    if (!scheduler_started) return 0;
    for (uint32_t i = 0; i < SCHEDULER_MAX_PROCESSES; i++) {
        if (tasks[i].state == TASK_STARTING ||
            tasks[i].state == TASK_RUNNABLE ||
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
        task->kernel_stack + stack_bytes >
            PAGING_IDMAP_MB * 1024u * 1024u) {
        if (task->kernel_stack)
            frame_free_pages((void *)task->kernel_stack,
                             TASK_KERNEL_STACK_PAGES);
        task->kernel_stack = 0;
        return 0;
    }

    stack_top = task->kernel_stack + stack_bytes;
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
    current_task = selected_index;
    if (selected_index == SCHEDULER_IDLE_INDEX) {
        task = &idle_task;
        paging_activate_kernel_space();
    } else {
        task = &tasks[selected_index];
        paging_activate_user_space(&task->program.address_space);
    }
    gdt_set_kernel_stack(task->kernel_stack +
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
    if (next == current_task)
        return frame;

    activate_task(next);
    return next == SCHEDULER_IDLE_INDEX ? idle_task.frame :
                                          tasks[next].frame;
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
            if (parent->wait_status_user &&
                !paging_copy_to_user_space(&parent->program.address_space,
                                           parent->wait_status_user,
                                           &status, sizeof(status)))
                parent->frame->eax = 0u - 14u;
            else
                parent->frame->eax = exiting->pid;
            parent->state = TASK_RUNNABLE;
            parent->wait_pid = 0;
            parent->wait_status_user = 0;
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
    task->program = *replacement;
    replacement->address_space.directory_phys = 0;
    /* A fresh image starts with a fresh, lazily-mapped heap. */
    task->heap_brk = 0;
    task->heap_top = 0;
    task->mmap_next = 0;

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
        if (user_status &&
            !paging_copy_to_user_space(&parent->program.address_space,
                                       user_status, &zombie->exit_status,
                                       sizeof(zombie->exit_status))) {
            frame->eax = 0u - 14u;
            irq_unlock(flags);
            return frame;
        }
        frame->eax = zombie->pid;
        zombie->state = TASK_REAP_PENDING;
        irq_unlock(flags);
        return frame;
    }

    parent->state = TASK_WAITING;
    parent->wait_pid = pid;
    parent->wait_status_user = user_status;
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

/* Map [start, end) as zero-filled, user-writable pages in the given
 * address space (page-aligned range). Caller holds no locks; a partial
 * failure leaves the pages already mapped, which the address-space
 * teardown reclaims when the process exits. */
static int map_user_zero_pages(address_space_t *space, uint32_t start,
                               uint32_t end) {
    for (uint32_t address = start; address < end; address += USER_PAGE_SIZE) {
        void *page = frame_alloc();
        if (!page) return 0;
        memset(page, 0, USER_PAGE_SIZE);
        if (!paging_map_user_page(space, address, (uint32_t)page, 1)) {
            frame_free(page);
            return 0;
        }
    }
    return 1;
}

/* brk(2) with a fixed heap region: grow the per-process break, mapping
 * zero-filled pages as it rises. The break is page-rounded and never
 * returns below USER_HEAP_BASE. A zero request just queries the current
 * break; an invalid size returns -EINVAL/-ENOMEM. */
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
        if (!map_user_zero_pages(space, task->heap_top, rounded))
            return 0u - HAKU_ENOMEM;
        task->heap_top = rounded;
    }
    task->heap_brk = rounded;
    return rounded;
}

/* Limited mmap(2): anonymous private pages from a per-process bump
 * region, always zero-filled and writable. Returns the mapped base
 * address or -errno. */
uint32_t scheduler_mmap(uint32_t length) {
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
    if (task->mmap_next == 0) task->mmap_next = USER_MMAP_BASE;

    rounded = (length + USER_PAGE_SIZE - 1u) & 0xFFFFF000u;
    base = task->mmap_next;
    if (base > USER_MMAP_LIMIT - rounded) return 0u - HAKU_ENOMEM;
    if (!map_user_zero_pages(space, base, base + rounded))
        return 0u - HAKU_ENOMEM;
    task->mmap_next = base + rounded;
    return base;
}
