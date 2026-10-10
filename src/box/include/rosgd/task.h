/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Alban Read */

/* task.h -- tasks, and the baton the Wimp passes between them.
 *
 * RISC OS multitasks cooperatively. A Wimp task owns the machine from the
 * return of one Wimp_Poll to its next, and the Wimp switches tasks by
 * paging the next one's application slot in at &8000, swapping the
 * machine-wide words that are the current program, and resuming it where
 * its own Wimp_Poll stopped. ROSGD does the same with a Linux thread per
 * task:
 *
 *   The baton.  Only the task holding it runs. ros_task_switch() hands it
 *   to another task and blocks until some task hands it back, so the
 *   switching task resumes exactly where it switched, as a task resumes
 *   just after its Wimp_Poll. The Wimp's switching core becomes calls to
 *   this one operation.
 *
 *   What moves with it, the current program:
 *     - the application slot at &8000 (a memfd, mapped; none for task 0);
 *     - the SVC stack, one per task, mapped at the one SVC stack address,
 *       so compiled frames a task leaves inside Wimp_Poll are still there
 *       when it comes back;
 *     - the SVC stack pointer, the SWI depth, and the chain of error
 *       handlers, since a raise never crosses to another task's thread;
 *     - floating point, and DomainId in zero page;
 *     - the environment handlers, in zero page (environment.h).
 *
 * The baton is not the personality lock. The task holding the baton holds
 * the lock while it runs, and releases it to wait, as any code does: then
 * background work runs, but no other task. /init is task 0.
 */
#ifndef ROSGD_TASK_H
#define ROSGD_TASK_H

#include <stddef.h>
#include <stdint.h>

struct ros_task;

/* /init becomes task 0: no application slot, the initial SVC stack. */
int ros_tasks_init(void);

struct ros_task *ros_task_current(void);
extern uint32_t ros_slot_next_size;
void ros_task_slot_next(uint32_t size);
os_error *ros_task_give_slot(void);
/* An application runs in the running task, as the application (module.c),
 * and ends: while none does, space of the task's own is the next one's,
 * which ros_task_give_slot makes the size *WimpSlot -next set (#45). */
void ros_task_app_enter(void);
void ros_task_app_leave(void);
/* The program that was running when the Wimp started, the first task,
 * given application space of its own, becomes a Wimp task with that
 * space as its slot. Its first Wimp_Initialise done, the runtime sets its
 * slot size to what it has (Wimp_SlotSize, as *WimpSlot would). The Wimp
 * asks AMB for a node of no pages (getnullslot) and grows it, and AMB's
 * allocate takes the space over in place (ros_task_take_adopted), so that
 * from then on the Wimp maps that task's memory in and out as any other's,
 * before it copies an event into the task's poll block. RISC OS keeps
 * the same memory through the kernel's application space, and the Wimp
 * there needs no call of this kind (Wimp08s findpages, allocateslot). */
void ros_task_wimp_initialised(void);
int ros_task_take_adopted(struct ros_slot *into);
/* ... and when the Wimp frees that node, the memory goes back to the
 * program: its slot again, which the caller maps if it was mapped in. */
const struct ros_slot *ros_task_restore_adopted(const struct ros_slot *memory);

/* The running program's own application space, outside the desktop where
 * it is not a Wimp node, made size bytes, as the kernel's free pool gives
 * and takes it (*AppSlot). The result is 0, or -1 if it has none of its
 * own or cannot have that. */
int ros_task_resize_space(uint32_t size);
uint32_t ros_task_id(const struct ros_task *t);
/* For a native Wimp (modules/wimp): the value the task's DomainId word
 * holds while it runs, which is the Wimp's internal handle for it. It is
 * written now if it is the running task, and by each switch to it after.
 * Also the size of its application slot, in bytes. */
void ros_task_set_domain(struct ros_task *t, uint32_t id);
uint32_t ros_task_slot_size(const struct ros_task *t);
/* The memory its slot uses: the pages touched (arena.h, ros_slot_used) */
uint32_t ros_task_slot_used(const struct ros_task *t);
/* For a native Wimp: another task's application slot paged in at &8000
 * for a moment, as RISC OS's Wimp pages in a menu's owner to read its
 * blocks. There is no switch, and the baton and the thread stay. What was
 * there is put back. ros_task_page_in returns the slot to hand back. */
struct ros_slot;
const struct ros_slot *ros_task_page_in(const struct ros_task *t);
void ros_task_page_back(const struct ros_slot *was);

/* A task, waiting for the baton: slot_size bytes of application space
 * (0 for none), its own SVC stack, and a thread that runs entry(arg) the
 * first time the baton comes to it.  When entry returns, the task ends
 * and the baton goes back to the task that created it.  NULL if the
 * memory or the thread cannot be had. */
struct ros_task *ros_task_create(uint32_t slot_size, void (*entry)(void *arg), void *arg);

/* Hand the baton to another task, which runs until it hands it on; return
 * when a task hands it back.  Holding the lock, as the running task. */
void ros_task_switch(struct ros_task *to);

/* The Wimp's switch (callback.c): hand the baton over and move only what
 * the thread is: its SVC stack and pointer, SWI depth, error handlers and
 * floating point. The slot, the environment handlers and DomainId are the
 * Wimp's, which pages the next task in itself before it switches. */
void ros_task_switch_thread(struct ros_task *to);
/* End the running task as ros_task_exit does, moving only the thread. */
__attribute__((noreturn)) void ros_task_exit_thread(struct ros_task *next);

/* The desktop's end (callback.c): the running task, woken by another's
 * end, takes over the application space that one had of its own, mapped.
 * If it has one itself, the ended task's is freed instead. */
void ros_task_take_ended_space(void);
/* The size of the space ros_task_take_ended_space gave the running task,
 * once (0 if none). The application the desktop's end returns from
 * (ros_module_run_as_application) leaves the memory limit and application
 * space where that space ends, as RISC OS's CLIEXIT leaves what the Wimp's
 * restorepages set, and not the 1024K the command line had before
 * *Desktop. */
uint32_t ros_task_took_ended_space(void);
/* The running task's program ended by the default handlers (environment.c):
 * ros_task_exit(NULL), marked; and whether the baton came back to the
 * running task through such an end. */
__attribute__((noreturn)) void ros_task_end_program(void);
int ros_task_woken_by_program_end(void);

/* The user context a task goes back to when it runs again: the block of
 * seventeen words its registers were dumped into as it left (the kernel's
 * CallBack buffer).  Setting it for the current task takes it from any
 * other; a task waiting on a block, NULL if none. */
void ros_task_set_user_block(uint32_t block);
uint32_t ros_task_user_block(const struct ros_task *t);
struct ros_task *ros_task_waiting_on(uint32_t block);

/* End the running task, handing the baton to next.  Does not return. */
__attribute__((noreturn)) void ros_task_exit(struct ros_task *next);

/* Call fn(task) as each task's thread ends (ros_task_exit and
 * ros_task_exit_thread), on that thread, under the lock, with its slot
 * still there. This is so whether or not the task closed down as a Wimp
 * task (the Worker module stops its jobs and unmaps its windows here). At
 * most four. */
void ros_task_on_end(void (*fn)(struct ros_task *));

/* A task's virtual display (runtime/vdu/vdisplay.c), NULL for none. Also
 * its real depth: the real sections it is inside (the Wimp's SWIs,
 * callbacks), in which it uses the real display */
struct vdisplay;
struct vdisplay *ros_task_vdisplay(const struct ros_task *t);
void ros_task_set_vdisplay(struct ros_task *t, struct vdisplay *d);
unsigned *ros_task_vdu_real(struct ros_task *t);

/* Whether a task has ended; and, once it has, free what is left of it. */
int ros_task_ended(const struct ros_task *t);
void ros_task_destroy(struct ros_task *t);

/* Every task there is, one at a time: the first for NULL, then the one
 * after t, NULL after the last (ended ones too, until destroyed). */
struct ros_task *ros_task_next(const struct ros_task *t);

/* The task's x32 application state (capp.h, per-task state): kept in the
 * task, so any thread finds a task's own. Nothing here moves it at a
 * switch. */
struct ros_capp_task;
struct ros_capp_task *ros_task_capp(struct ros_task *t);

/* Give the calling thread a signal stack of its own (sigaltstack, 64K,
 * freed when the thread ends) if it has none, so that a fault on it, a
 * stack overflow above all, still reaches /init's handler (boot/main.c,
 * SA_ONSTACK) and its report, rather than killing init. Each thread the
 * runtime starts calls it first: tasks, the background and pump threads,
 * the ticker, the module workers. A thread with one keeps it. */
void ros_thread_signal_stack(void);

/* ---- the stacks a recursion of commands runs on ----
 *
 * The commands can call themselves without end. An alias can expand to
 * itself. An Obey file can have a line that obeys it again (its last line
 * too, a tail call that gives its level back first). A file's run action,
 * Alias$@RunType_FEB's Obey, can run the file again. A program can start
 * itself as the application (BASIC's OSCLI "BASICVFP -quit" naming its own
 * file). RISC OS goes as deep as its SVC stack allows. FileSwitch refuses
 * to call a filing system with less than 1K of it left, "Not enough stack
 * to call filing system" (&414), which is how RISC OS 5.30 ends an Obey
 * file obeying itself, an application whose !Run runs it again, or an
 * alias and an Obey file calling each other (bigmacfarm). An alias that
 * only expands itself takes a data abort. An application replaces the
 * one that started it, stacks and all, so a program starting itself goes
 * round for ever.
 *
 * Here each level is native frames as well as SVC stack. An application
 * nests inside the one that started it, which it comes back to (module.c,
 * ros_module_run_as_application), and a thread's native stack is fixed:
 * 8 MB for a task's (ros_task_create) and, for the main thread, as exec
 * made it (8 MB in the box). So OS_CLI's alias expansion, *Obey,
 * FileSwitch's run action and its StartApplication (OS_FSControl 2, which
 * every application's start calls) each ask, before going a level
 * deeper, for room on both stacks: the native stack the calling thread
 * has below its frame, and the SVC stack below ros_svc_sp. They refuse
 * with RISC OS's errors when it is not there: ROS_STACK_ALIAS "Expansion
 * too complex" (&1E1), and ROS_STACK_FILE "Not enough stack to call
 * filing system".
 *
 * A task's thread has 8 MB, as the main thread has. At the 1 MB it had,
 * these cycles ran the stack out where the main thread ran the handles
 * out, and anything deeper that the budgets below do not cover, such as a
 * module calling a module calling a module, had a quarter of the room a
 * 64-bit frame wants. The larger stack costs nothing, because a thread
 * stack is address space, paged in as it is touched.
 *
 * Measured (boot/selftest_files.c, the box and hosted): a level of the
 * heaviest cycle, a run action, its alias, *Obey and the line, is 16K of
 * native stack and 48 bytes of SVC stack. An Obey file's tail call is 1.7K
 * to 1.9K, an alias 0.9K, and a BASIC program starting itself 3.9K (and
 * no SVC stack, because each start flattens it). Heavier variants,
 * measured hosted, are the line through *IF at 17.7K and a BASIC program
 * running itself by its file type at 19K. ROS_STACK_LEVEL is twice the
 * heaviest cycle's. A file's room is its alias's and two levels more, so
 * a recursion through a file or an application's start always reaches a
 * filing system's refusal first, as on RISC OS, and one through aliases
 * alone reaches an alias's. What is left at a refusal is at least the
 * alias's room, 160K. The deepest a task's thread goes in the desktop's
 * own use is under 100K (sampled with the edit probe, !Edit's x32 image,
 * task windows and the Filer), so the innermost level's other lines, and
 * the error's way back out, fit with room to spare. The alias chain's own
 * limit is the kernel's circular buffers' (oscli.c: sixteen live levels),
 * well inside all of this.
 *
 * Files kept open: an Obey file's is open while its lines run, as 5.30's
 * Obey reads it (OS_BGet), and a run action's *Obey holds its file so. A
 * recursion through a line that is not the last uses FileSwitch's 255
 * handles up before the stacks, and ends "Too many open files" (&C0)
 * after 255 levels. Examples are an Obey file obeying itself there, an
 * alias and an Obey file calling each other, and a run action on an 8 MB
 * native stack (the main thread's, and a task's since they were raised
 * from 1 MB). On 5.30 the 8K SVC stack gives out first: &414 after 142
 * (an alias and an Obey file), a garbled &124 after 216 (an Obey file's
 * middle line), and &16B "(Number)" after 68 (a run action). (*Obey -c,
 * which closes its file first, ends &414 on both.) It is an error either
 * way and never a fault, but the depth and the error differ. */
#define ROS_STACK_LEVEL           (32u << 10)     /* a cycle's native frames, at most */
#define ROS_STACK_ALIAS_NATIVE    (160u << 10)
#define ROS_STACK_FILE_NATIVE     (ROS_STACK_ALIAS_NATIVE + 2 * ROS_STACK_LEVEL)
#define ROS_STACK_ALIAS_SVC       (32u << 10)
#define ROS_STACK_FILE_SVC        (ROS_STACK_ALIAS_SVC + (8u << 10))

/* BASIC's FN and PROC calls (modules/basicvfp/patch-basicasm.py, at
 * Funct.s's FNGOACACHE, where BASIC asks its own stack for 1K above the
 * heap). The translated interpreter recurses in C. A level of the
 * program's recursion takes a few hundred bytes of native stack, where
 * BASIC's own stack takes a few dozen, so a deep recursion ran the
 * thread's stack out long before BASIC's met its heap, and the box took a
 * SIGSEGV in basicvfp_FACTOR (#64). Asked here too, it is BASIC's own "No
 * room for function/procedure call" (37), which the program can trap. The
 * room kept is a file's and two levels more. The handler that traps the
 * error runs from where it was raised (environment.c), and has what an
 * alias, a *command and an application's start need. */
#define ROS_STACK_BASIC_NATIVE    (ROS_STACK_FILE_NATIVE + 2 * ROS_STACK_LEVEL)

enum ros_stack_need { ROS_STACK_ALIAS, ROS_STACK_FILE, ROS_STACK_BASIC };

/* Record the calling thread's native stack: its lowest usable address and
 * its top. In the box a thread's comes from its attributes, and the main
 * thread's from where the kernel put it and RLIMIT_STACK (8 MB at most).
 * Hosted, it is as macOS gives each (the main thread's RLIMIT_STACK:
 * make's is 64 MB). ros_tasks_init does it for the main thread,
 * ros_thread_signal_stack for every thread the runtime starts, and a
 * thread that never did is recorded when it first asks. */
void ros_thread_stack_record(void);

/* The native stack left below the caller's frame; SIZE_MAX if the thread's
 * stack cannot be known, or the caller is not on it. */
size_t ros_native_stack_left(void);

/* 1 if both stacks have the room for need (above), else 0, the refusal's
 * room left recorded for the thread (the self-test's measure). */
int ros_stack_room(enum ros_stack_need need);

struct ros_stack_refusal {
    unsigned count;                 /* refusals on this thread */
    enum ros_stack_need need;       /* the last one's */
    size_t native;                  /* its native stack left, SIZE_MAX if unknown */
    uint32_t svc;                   /* its SVC stack left */
};
const struct ros_stack_refusal *ros_stack_last_refusal(void);

#endif
