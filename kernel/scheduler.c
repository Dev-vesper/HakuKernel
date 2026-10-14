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
    TASK_ZOMBIE
} task_state_t;

typedef struct {
    uint32_t pid;
    task_state_t state;
    loaded_elf_t program;
    uint32_t kernel_stack;
    regs_t *frame;
} task_t;

static task_t tasks[SCHEDULER_MAX_PROCESSES];
static task_t idle_task;
static uint32_t task_count;
static int current_task = -1;
static int scheduler_started;
static int timer_marker_written;

int scheduler_user_tasks_done(void) {
    if (task_count == 0) return 0;
    for (uint32_t i = 0; i < task_count; i++)
        if (tasks[i].state == TASK_RUNNABLE) return 0;
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
    if (current_task == (int)task_count) {
        for (uint32_t i = 0; i < task_count; i++)
            if (tasks[i].state == TASK_RUNNABLE)
                return (int)i;
        return (int)task_count;
    }

    for (uint32_t offset = 1; offset < task_count; offset++) {
        uint32_t selected_index =
            ((uint32_t)current_task + offset) % task_count;
        if (tasks[selected_index].state == TASK_RUNNABLE)
            return (int)selected_index;
    }
    if (current_task >= 0 &&
        current_task < (int)task_count &&
        tasks[current_task].state == TASK_RUNNABLE)
        return current_task;
    return (int)task_count;
}

static void activate_task(int selected_index) {
    task_t *task;
    current_task = selected_index;
    if (selected_index == (int)task_count) {
        task = &idle_task;
        paging_activate_kernel_space();
    } else {
        task = &tasks[selected_index];
        paging_activate_user_space(&task->program.address_space);
    }
    gdt_set_kernel_stack(task->kernel_stack +
                         TASK_KERNEL_STACK_PAGES * FRAME_SIZE);
}

static void reap_zombies(void) {
    for (uint32_t i = 0; i < task_count; i++) {
        task_t *task = &tasks[i];
        if (task->state != TASK_ZOMBIE || (int)i == current_task)
            continue;
        paging_destroy_user_space(&task->program.address_space);
        frame_free_pages((void *)task->kernel_stack,
                         TASK_KERNEL_STACK_PAGES);
        klog_prefix();
        klog_write("SCHED-REAP-OK: reaped pid=");
        klog_dec(task->pid);
        klog_write("\n");
        memset(task, 0, sizeof(*task));
    }
}

int scheduler_init(loaded_elf_t *initial_program) {
    if (!initial_program || !initial_program->address_space.directory_phys ||
        !initial_program->entry || !initial_program->user_stack)
        return 0;

    memset(tasks, 0, sizeof(tasks));
    memset(&idle_task, 0, sizeof(idle_task));
    task_count = 1;
    current_task = -1;
    scheduler_started = 0;
    timer_marker_written = 0;

    tasks[0].pid = 1;
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
    task_count = 0;
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

    reap_zombies();
    if (current_task == (int)task_count) {
        idle_task.frame = frame;
    } else if (current_task >= 0) {
        tasks[current_task].frame = frame;
    }

    next = find_next_task();
    if (next == current_task)
        return frame;

    activate_task(next);
    return next == (int)task_count ? idle_task.frame :
                                          tasks[next].frame;
}

regs_t *scheduler_exit_current(regs_t *frame) {
    int next;
    if (!scheduler_started || current_task < 0 ||
        current_task >= (int)task_count)
        return frame;

    vfs_close_all(tasks[current_task].pid);
    tasks[current_task].state = TASK_ZOMBIE;
    tasks[current_task].frame = frame;
    next = find_next_task();
    activate_task(next);
    return next == (int)task_count ? idle_task.frame :
                                          tasks[next].frame;
}

int scheduler_exec_current(loaded_elf_t *replacement, regs_t *frame) {
    task_t *task;
    loaded_elf_t previous;
    uint32_t irq_flags;
    if (!scheduler_started || !replacement || !frame ||
        !replacement->address_space.directory_phys ||
        !replacement->entry || !replacement->user_stack ||
        current_task < 0 || current_task >= (int)task_count ||
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
    if (current_task < 0 || current_task >= (int)task_count)
        return 0;
    return tasks[current_task].pid;
}
