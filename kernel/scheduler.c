#include "scheduler.h"
#include "console.h"
#include "frame.h"
#include "fs_test.h"
#include "gdt.h"
#include "klog.h"
#include "paging.h"
#include "vfs.h"

#define TASK_KERNEL_STACK_PAGES 2u

typedef enum {
    TASK_UNUSED,
    TASK_RUNNABLE,
    TASK_WAITING,
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
        if (tasks[i].state == TASK_RUNNABLE ||
            tasks[i].state == TASK_WAITING)
            return 0;
    }
    return 1;
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
    idle_task.state = TASK_RUNNABLE;
    if (!initialize_stack(&idle_task, 0)) goto fail;
    initial_program->address_space.directory_phys = 0;

    klog_prefix();
    klog_write("sched: SCHED-READY tasks=1");
    klog_write(" kernel-stack=per-task\n");
    return 1;

fail:
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
    cpu_cli();
    scheduler_started = 1;
    activate_task(first);
    klog_prefix();
    klog_write("sched: SCHED-START pid=");
    klog_dec(tasks[first].pid);
    klog_write(" (first task)\n");
    scheduler_enter_frame(tasks[first].frame);
    for (;;) __asm hlt
}

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
    vfs_close_all(exiting->pid);
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
    uint32_t irq_flags;
    if (!scheduler_started || !replacement || !frame ||
        !replacement->address_space.directory_phys ||
        !replacement->entry || !replacement->user_stack ||
        current_task < 0 || current_task >= SCHEDULER_IDLE_INDEX ||
        (frame->cs & 3u) != 3u)
        return 0;

    irq_flags = cpu_irq_save();
    task = &tasks[current_task];
    previous = task->program;
    task->program = *replacement;
    replacement->address_space.directory_phys = 0;

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
    cpu_irq_restore(irq_flags);

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
    uint32_t irq_flags;
    uint32_t parent_pid;
    uint32_t slot;
    task_t *child = 0;

    if (!scheduler_started || !program ||
        !program->address_space.directory_phys ||
        !program->entry || !program->user_stack ||
        current_task < 0 || current_task >= SCHEDULER_IDLE_INDEX)
        return -(int32_t)HAKU_ENOMEM;

    irq_flags = cpu_irq_save();
    parent_pid = tasks[current_task].pid;
    for (slot = 1; slot < SCHEDULER_MAX_PROCESSES; slot++) {
        if (tasks[slot].state == TASK_UNUSED) {
            child = &tasks[slot];
            break;
        }
    }
    if (!child) {
        cpu_irq_restore(irq_flags);
        return -(int32_t)HAKU_EAGAIN;
    }

    memset(child, 0, sizeof(*child));
    child->pid = slot + 1u;
    child->parent_pid = parent_pid;
    child->state = TASK_RUNNABLE;
    child->program = *program;
    if (!initialize_stack(child, 1)) {
        memset(child, 0, sizeof(*child));
        cpu_irq_restore(irq_flags);
        return -12;
    }
    program->address_space.directory_phys = 0;
    cpu_irq_restore(irq_flags);
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
    uint32_t irq_flags;

    if (!frame || current_task < 0 ||
        current_task >= SCHEDULER_IDLE_INDEX)
        return frame;
    irq_flags = cpu_irq_save();
    if (options != 0 || pid == 0) {
        frame->eax = 0u - HAKU_EINVAL;
        cpu_irq_restore(irq_flags);
        return frame;
    }
    if (pid != 0xFFFFFFFFu && pid > 0x7FFFFFFFu) {
        frame->eax = 0u - HAKU_EINVAL;
        cpu_irq_restore(irq_flags);
        return frame;
    }
    if (pid != 0xFFFFFFFFu && pid > SCHEDULER_MAX_PROCESSES) {
        frame->eax = 0u - HAKU_ECHILD;
        cpu_irq_restore(irq_flags);
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
        cpu_irq_restore(irq_flags);
        return frame;
    }

    if (zombie) {
        if (user_status &&
            !paging_copy_to_user_space(&parent->program.address_space,
                                       user_status, &zombie->exit_status,
                                       sizeof(zombie->exit_status))) {
            frame->eax = 0u - 14u;
            cpu_irq_restore(irq_flags);
            return frame;
        }
        frame->eax = zombie->pid;
        zombie->state = TASK_REAP_PENDING;
        cpu_irq_restore(irq_flags);
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
