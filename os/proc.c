#include "proc.h"
#include "defs.h"
#include "loader.h"
#include "trap.h"
#include "timer.h"

// Declaration to fix the implicit declaration warning and linker error
uint64 get_cycle(); 

struct proc pool[NPROC];
char kstack[NPROC][PAGE_SIZE];
__attribute__((aligned(4096))) char ustack[NPROC][PAGE_SIZE];
__attribute__((aligned(4096))) char trapframe[NPROC][PAGE_SIZE];

extern char boot_stack_top[];
struct proc *current_proc;
struct proc idle;

int threadid()
{
    return curr_proc()->pid;
}

struct proc *curr_proc()
{
    return current_proc;
}

// initialize the proc table at boot time.
void proc_init(void)
{
    struct proc *p;
    for (p = pool; p < &pool[NPROC]; p++) {
        p->state = UNUSED;
        p->kstack = (uint64)kstack[p - pool];
        p->ustack = (uint64)ustack[p - pool];
        p->trapframe = (struct trapframe *)trapframe[p - pool];
        
        /*
        * LAB1: Initialize syscall counters and start time
        */
        for (int i = 0; i < MAX_SYSCALL_NUM; i++) {
            p->syscall_times[i] = 0; // Set all counters to zero
        }
        p->start_time = 0; // Initial start time
    }
    idle.kstack = (uint64)boot_stack_top;
    idle.pid = 0;
    current_proc = &idle;
}

int allocpid()
{
    static int PID = 1;
    return PID++;
}

struct proc *allocproc(void)
{
    struct proc *p;
    for (p = pool; p < &pool[NPROC]; p++) {
        if (p->state == UNUSED) {
            goto found;
        }
    }
    return 0;

found:
    p->pid = allocpid();
    p->state = USED;
    memset(&p->context, 0, sizeof(p->context));
    memset(p->trapframe, 0, PAGE_SIZE);
    memset((void *)p->kstack, 0, PAGE_SIZE);

    /*
    * LAB1: Reset syscall counters for the newly allocated process
    */
    for (int i = 0; i < MAX_SYSCALL_NUM; i++) {
        p->syscall_times[i] = 0;
    }
    p->start_time = 0;

    p->context.ra = (uint64)usertrapret;
    p->context.sp = p->kstack + PAGE_SIZE;
    return p;
}

// Scheduler never returns.
void scheduler(void)
{
    struct proc *p;
    for (;;) {
        for (p = pool; p < &pool[NPROC]; p++) {
            if (p->state == RUNNABLE) {
                /*
                * LAB1: Record the start time when the process first runs
                */
                if (p->start_time == 0) {
                    p->start_time = get_cycle(); // Record hardware clock cycle
                }

                p->state = RUNNING;
                current_proc = p;
                swtch(&idle.context, &p->context);
            }
        }
    }
}

void sched(void)
{
    struct proc *p = curr_proc();
    if (p->state == RUNNING)
        panic("sched running");
    swtch(&p->context, &idle.context);
}

void yield(void)
{
    current_proc->state = RUNNABLE;
    sched();
}

void exit(int code)
{
    struct proc *p = curr_proc();
    infof("proc %d exit with %d", p->pid, code);
    p->state = UNUSED;
    finished();
    sched();
}