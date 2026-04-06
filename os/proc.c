#include "proc.h"
#include "defs.h"
#include "loader.h"
#include "trap.h"
#include "vm.h"
#include "queue.h"

struct proc pool[NPROC];
__attribute__((aligned(16))) char kstack[NPROC][PAGE_SIZE];
__attribute__((aligned(4096))) char trapframe[NPROC][TRAP_PAGE_SIZE];

extern char boot_stack_top[];
struct proc *current_proc;
struct proc idle;
struct queue task_queue;

int threadid()
{
    return curr_proc()->pid;
}

struct proc *curr_proc()
{
    return current_proc;
}

// initialize the proc table at boot time.
void proc_init()
{
    struct proc *p;
    for (p = pool; p < &pool[NPROC]; p++) {
        p->state = UNUSED;
        p->kstack = (uint64)kstack[p - pool];
        p->trapframe = (struct trapframe *)trapframe[p - pool];
    }
    idle.kstack = (uint64)boot_stack_top;
    idle.pid = IDLE_PID;
    current_proc = &idle;
    init_queue(&task_queue);
}

int allocpid()
{
    static int PID = 1;
    return PID++;
}

// Task 2: Stride Scheduling
struct proc *fetch_task()
{
    struct proc *best_p = NULL;
    uint64 min_pass = -1ULL; // Max possible uint64 value

    // Scan pool for RUNNABLE process with the smallest pass value
    for (struct proc *p = pool; p < &pool[NPROC]; p++) {
        if (p->state == RUNNABLE) {
            if (p->pass < min_pass) {
                min_pass = p->pass;
                best_p = p;
            }
        }
    }

    if (best_p != NULL) {
        // Update pass value based on priority
        best_p->stride = BIG_STRIDE / best_p->priority;
        best_p->pass += best_p->stride;
        
        debugf("fetch task %d(pid=%d) with priority %d\n", best_p - pool, best_p->pid, best_p->priority);
        return best_p;
    }
    
    return NULL;
}

void add_task(struct proc *p)
{
    // For stride scheduling, we bypass the round-robin task_queue.
    // fetch_task() will directly scan the pool for RUNNABLE processes.
}

// Look in the process table for an UNUSED proc.
struct proc *allocproc()
{
    struct proc *p;
    for (p = pool; p < &pool[NPROC]; p++) {
        if (p->state == UNUSED) {
            goto found;
        }
    }
    return 0;

found:
    // init proc
    p->pid = allocpid();
    p->state = USED;
    p->ustack = 0;
    p->max_page = 0;
    p->parent = NULL;
    p->exit_code = 0;
    p->pagetable = uvmcreate((uint64)p->trapframe);
    
    // Task 2: Initialize Stride Scheduling variables
    p->priority = 16; 
    p->stride = 0;    
    p->pass = 0;      

    memset(&p->context, 0, sizeof(p->context));
    memset((void *)p->kstack, 0, KSTACK_SIZE);
    memset((void *)p->trapframe, 0, TRAP_PAGE_SIZE);
    p->context.ra = (uint64)usertrapret;
    p->context.sp = p->kstack + KSTACK_SIZE;
    return p;
}

void scheduler()
{
    struct proc *p;
    for (;;) {
        p = fetch_task();
        if (p == NULL) {
            panic("all app are over!\n");
        }
        tracef("swtich to proc %d", p - pool);
        p->state = RUNNING;
        current_proc = p;
        swtch(&idle.context, &p->context);
    }
}

void sched()
{
    struct proc *p = curr_proc();
    if (p->state == RUNNING)
        panic("sched running");
    swtch(&p->context, &idle.context);
}

void yield()
{
    current_proc->state = RUNNABLE;
    add_task(current_proc);
    sched();
}

void freepagetable(pagetable_t pagetable, uint64 max_page)
{
    uvmunmap(pagetable, TRAMPOLINE, 1, 0);
    uvmunmap(pagetable, TRAPFRAME, 1, 0);
    uvmfree(pagetable, max_page);
}

void freeproc(struct proc *p)
{
    if (p->pagetable)
        freepagetable(p->pagetable, p->max_page);
    p->pagetable = 0;
    p->state = UNUSED;
}

int fork()
{
    struct proc *np;
    struct proc *p = curr_proc();
    if ((np = allocproc()) == 0) {
        panic("allocproc\n");
    }
    if (uvmcopy(p->pagetable, np->pagetable, p->max_page) < 0) {
        panic("uvmcopy\n");
    }
    np->max_page = p->max_page;
    *(np->trapframe) = *(p->trapframe);
    np->trapframe->a0 = 0;
    np->parent = p;
    np->state = RUNNABLE;
    add_task(np);
    return np->pid;
}

int exec(char *name)
{
    int id = get_id_by_name(name);
    if (id < 0)
        return -1;
    struct proc *p = curr_proc();
    uvmunmap(p->pagetable, 0, p->max_page, 1);
    p->max_page = 0;
    loader(id, p);
    return 0;
}

// Task 1: Spawn
int spawn(char *name)
{
    int id = get_id_by_name(name);
    if (id < 0) return -1; // Invalid filename

    struct proc *np = allocproc();
    if (np == 0) return -1; // Insufficient memory/process pool full

    loader(id, np);

    np->parent = curr_proc();
    np->state = RUNNABLE;
    
    add_task(np);
    return np->pid; 
}

int wait(int pid, int *code)
{
    struct proc *np;
    int havekids;
    struct proc *p = curr_proc();

    for (;;) {
        havekids = 0;
        for (np = pool; np < &pool[NPROC]; np++) {
            if (np->state != UNUSED && np->parent == p &&
                (pid <= 0 || np->pid == pid)) {
                havekids = 1;
                if (np->state == ZOMBIE) {
                    np->state = UNUSED;
                    pid = np->pid;
                    *code = np->exit_code;
                    return pid;
                }
            }
        }
        if (!havekids) {
            return -1;
        }
        p->state = RUNNABLE;
        add_task(p);
        sched();
    }
}

void exit(int code)
{
    struct proc *p = curr_proc();
    p->exit_code = code;
    debugf("proc %d exit with %d\n", p->pid, code);
    freeproc(p);
    if (p->parent != NULL) {
        p->state = ZOMBIE;
    }
    struct proc *np;
    for (np = pool; np < &pool[NPROC]; np++) {
        if (np->parent == p) {
            np->parent = NULL;
        }
    }
    sched();
}