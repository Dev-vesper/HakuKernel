#include "scheduler.h"
#include "console.h"
#include "frame.h"
#include "fs_test.h"
#include "gdt.h"
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
static uint32_t switch_count;
static int scheduler_started;
static int preempt_marker_written;

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
        memset(task, 0, sizeof(*task));
        serial_write("SCHED-REAP-OK\n");
    }
}

int scheduler_init(loaded_elf_t *programs, uint32_t count) {
    if (!programs || count == 0 || count > SCHEDULER_MAX_PROCESSES)
        return 0;

    memset(tasks, 0, sizeof(tasks));
    memset(&idle_task, 0, sizeof(idle_task));
    task_count = count;
    current_task = -1;
    switch_count = 0;
    scheduler_started = 0;
    preempt_marker_written = 0;

    for (uint32_t i = 0; i < count; i++) {
        tasks[i].pid = i + 1u;
        tasks[i].state = TASK_RUNNABLE;
        tasks[i].program = programs[i];
        if (!initialize_stack(&tasks[i], 1)) goto fail;
    }
    idle_task.state = TASK_RUNNABLE;
    if (!initialize_stack(&idle_task, 0)) goto fail;

    serial_write("SCHED-READY tasks=");
    serial_dec(count);
    serial_write(" kernel-stack=per-task\n");
    return 1;

fail:
    for (uint32_t i = 0; i < count; i++) {
        if (tasks[i].kernel_stack)
            frame_free_pages((void *)tasks[i].kernel_stack,
                             TASK_KERNEL_STACK_PAGES);
        memset(&tasks[i], 0, sizeof(tasks[i]));
    }
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
    serial_write("SCHED-START pid=");
    serial_dec(tasks[first].pid);
    serial_write("\n");
    scheduler_enter_frame(tasks[first].frame);
    for (;;) __asm hlt
}

regs_t *scheduler_preempt(regs_t *frame) {
    int next;

    if (!scheduler_started || !frame)
        return frame;

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
    switch_count++;
    if (!preempt_marker_written) {
        serial_write("SCHED-PREEMPT-OK switches=");
        serial_dec(switch_count);
        serial_write("\n");
        preempt_marker_written = 1;
    }
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

uint32_t scheduler_current_pid(void) {
    if (current_task < 0 || current_task >= (int)task_count)
        return 0;
    return tasks[current_task].pid;
}
