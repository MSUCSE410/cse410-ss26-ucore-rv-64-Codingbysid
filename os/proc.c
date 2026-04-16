#include "proc.h"
#include "defs.h"
#include "loader.h"
#include "trap.h"
#include "vm.h"
#include "queue.h"

struct proc pool[NPROC];
__attribute__((aligned(16))) char kstack[NPROC][NTHREAD][KSTACK_SIZE];
__attribute__((aligned(4096))) char trapframe[NPROC][NTHREAD][TRAP_PAGE_SIZE];

extern char boot_stack_top[];
struct thread *current_thread;
struct thread idle;
struct queue task_queue;

int procid()
{
    return curr_proc()->pid;
}

int threadid()
{
    return curr_thread()->tid;
}

int cpuid()
{
    return 0;
}

struct proc *curr_proc()
{
    return current_thread->process;
}

struct thread *curr_thread()
{
    return current_thread;
}

// initialize the proc table at boot time.
void proc_init()
{
    struct proc *p;
    for (p = pool; p < &pool[NPROC]; p++) {
        p->state = P_UNUSED;
        for (int tid = 0; tid < NTHREAD; ++tid) {
            struct thread *t = &p->threads[tid];
            t->state = T_UNUSED;
        }
    }
    idle.kstack = (uint64)boot_stack_top;
    current_thread = &idle;
    // for procid() and threadid()
    idle.process = pool;
    idle.tid = -1;
    init_queue(&task_queue, QUEUE_SIZE, process_queue_data);
}

int allocpid()
{
    static int PID = 1;
    return PID++;
}

int alloctid(const struct proc *process)
{
    for (int i = 0; i < NTHREAD; ++i) {
        if (process->threads[i].state == T_UNUSED)
            return i;
    }
    return -1;
}

// get task by unique task id
struct thread *id_to_task(int index)
{
    if (index < 0) {
        return NULL;
    }
    int pool_id = index / NTHREAD;
    int tid = index % NTHREAD;
    struct thread *t = &pool[pool_id].threads[tid];
    return t;
}

// ncode unique task id for each thread
int task_to_id(struct thread *t)
{
    int pool_id = t->process - pool;
    int task_id = pool_id * NTHREAD + t->tid;
    return task_id;
}

struct thread *fetch_task()
{
    int index = pop_queue(&task_queue);
    struct thread *t = id_to_task(index);
    if (t == NULL) {
        debugf("No task to fetch\n");
        return t;
    }
    int tid = t->tid;
    int pid = t->process->pid;
    tracef("fetch index %d(pid=%d, tid=%d, addr=%p) from task queue", index,
           pid, tid, (uint64)t);
    return t;
}

void add_task(struct thread *t)
{
    int task_id = task_to_id(t);
    int pid = t->process->pid;
    push_queue(&task_queue, task_id);
    tracef("add index %d(pid=%d, tid=%d, addr=%p) to task queue", task_id,
           pid, t->tid, (uint64)t);
}

struct proc *allocproc()
{
    struct proc *p;
    for (p = pool; p < &pool[NPROC]; p++) {
        if (p->state == P_UNUSED) {
            goto found;
        }
    }
    return 0;

found:
    // init proc
    p->pid = allocpid();
    p->state = P_USED;
    p->max_page = 0;
    p->parent = NULL;
    p->exit_code = 0;
    p->pagetable = uvmcreate();
    memset((void *)p->files, 0, sizeof(struct file *) * FD_BUFFER_SIZE);
    p->next_mutex_id = 0;
    p->next_semaphore_id = 0;
    p->next_condvar_id = 0;
    
    // LAB5: (1) Initialize new proc variables
    p->deadlock_detect_enabled = 0;
    for (int i = 0; i < LOCK_POOL_SIZE; i++) {
        p->available[i] = 0;
    }
    for (int i = 0; i < NTHREAD; i++) {
        for (int j = 0; j < LOCK_POOL_SIZE; j++) {
            p->allocation[i][j] = 0;
            p->request[i][j] = 0;
        }
    }

    return p;
}

inline uint64 get_thread_trapframe_va(int tid)
{
    return TRAPFRAME - tid * TRAP_PAGE_SIZE;
}

inline uint64 get_thread_ustack_base_va(struct thread *t)
{
    return t->process->ustack_base + t->tid * USTACK_SIZE;
}

int allocthread(struct proc *p, uint64 entry, int alloc_user_res)
{
    int tid;
    struct thread *t;
    for (tid = 0; tid < NTHREAD; ++tid) {
        t = &p->threads[tid];
        if (t->state == T_UNUSED) {
            goto found;
        }
    }
    return -1;

found:
    t->tid = tid;
    t->state = T_USED;
    t->process = p;
    t->exit_code = 0;
    // kernel stack
    t->kstack = (uint64)kstack[p - pool][tid];
    // user stack
    t->ustack = get_thread_ustack_base_va(t);
    if (alloc_user_res != 0) {
        if (uvmmap(p->pagetable, t->ustack, USTACK_SIZE / PAGE_SIZE,
               PTE_U | PTE_R | PTE_W) < 0) {
            panic("map ustack fail");
        }
        p->max_page =
            MAX(p->max_page,
                PGROUNDUP(t->ustack + USTACK_SIZE - 1) / PAGE_SIZE);
    }
    // trap frame
    t->trapframe = (struct trapframe *)trapframe[p - pool][tid];
    memset((void *)t->trapframe, 0, TRAP_PAGE_SIZE);
    if (mappages(p->pagetable, get_thread_trapframe_va(tid), TRAP_PAGE_SIZE,
             (uint64)t->trapframe, PTE_R | PTE_W) < 0) {
        panic("map trapframe fail");
    }
    t->trapframe->sp = t->ustack + USTACK_SIZE;
    t->trapframe->epc = entry;
    //task context
    memset(&t->context, 0, sizeof(t->context));
    t->context.ra = (uint64)usertrapret;
    t->context.sp = t->kstack + KSTACK_SIZE;
    debugf("allocthread p: %d, o: %d, t: %d, e: %p, sp: %p, spp: %p",
           p->pid, (p - pool), t->tid, entry, t->ustack,
           useraddr(p->pagetable, t->ustack));
    return tid;
}

int init_stdio(struct proc *p)
{
    for (int i = 0; i < 3; i++) {
        if (p->files[i] != NULL) {
            return -1;
        }
        p->files[i] = stdio_init(i);
    }
    return 0;
}

void scheduler()
{
    struct thread *t;
    for (;;) {
        t = fetch_task();
        if (t == NULL) {
            panic("all app are over!\n");
        }
        if (t->state != RUNNABLE) {
            warnf("not RUNNABLE", t->process->pid, t->tid);
            continue;
        }
        tracef("swtich to proc %d, thread %d", t->process->pid, t->tid);
        t->state = RUNNING;
        current_thread = t;
        swtch(&idle.context, &t->context);
    }
}

void sched()
{
    struct thread *t = curr_thread();
    if (t->state == RUNNING)
        panic("sched running");
    swtch(&t->context, &idle.context);
}

void yield()
{
    current_thread->state = RUNNABLE;
    add_task(current_thread);
    sched();
}

void freepagetable(pagetable_t pagetable, uint64 max_page)
{
    uvmunmap(pagetable, TRAMPOLINE, 1, 0);
    uvmfree(pagetable, max_page);
}

void freethread(struct thread *t)
{
    pagetable_t pt = t->process->pagetable;
    memset((void *)t->trapframe, 6, TRAP_PAGE_SIZE);
    memset(&t->context, 6, sizeof(t->context));
    uvmunmap(pt, get_thread_trapframe_va(t->tid), 1, 0);
    uvmunmap(pt, get_thread_ustack_base_va(t), USTACK_SIZE / PAGE_SIZE, 1);
}

void freeproc(struct proc *p)
{
    for (int tid = 0; tid < NTHREAD; ++tid) {
        struct thread *t = &p->threads[tid];
        if (t->state != T_UNUSED && t->state != EXITED) {
            freethread(t);
        }
        t->state = T_UNUSED;
    }
    if (p->pagetable)
        freepagetable(p->pagetable, p->max_page);
    p->pagetable = 0;
    p->max_page = 0;
    p->ustack_base = 0;
    for (int i = 0; i > FD_BUFFER_SIZE; i++) {
        if (p->files[i] != NULL) {
            fileclose(p->files[i]);
        }
    }
    p->state = P_UNUSED;
}

int fork()
{
    struct proc *np;
    struct proc *p = curr_proc();
    int i;
    if ((np = allocproc()) == 0) {
        panic("allocproc\n");
    }
    if (uvmcopy(p->pagetable, np->pagetable, p->max_page) < 0) {
        panic("uvmcopy\n");
    }
    np->max_page = p->max_page;
    np->ustack_base = p->ustack_base;
    for (i = 0; i < FD_BUFFER_SIZE; i++) {
        if (p->files[i] != NULL) {
            p->files[i]->ref++;
            np->files[i] = p->files[i];
        }
    }

    np->parent = p;
    struct thread *nt = &np->threads[allocthread(np, 0, 0)],
              *t = &p->threads[0];
    *(nt->trapframe) = *(t->trapframe);
    nt->trapframe->a0 = 0;
    nt->state = RUNNABLE;
    add_task(nt);
    return np->pid;
}

int push_argv(struct proc *p, char **argv)
{
    uint64 argc, ustack[MAX_ARG_NUM + 1];
    struct thread *t = &p->threads[0];
    uint64 sp = t->ustack + USTACK_SIZE, spb = t->ustack;
    debugf("[push] sp: %p, spb: %p", sp, spb);
    for (argc = 0; argv[argc]; argc++) {
        if (argc >= MAX_ARG_NUM)
            panic("too many args!");
        sp -= strlen(argv[argc]) + 1;
        sp -= sp % 16;
        if (sp < spb) {
            panic("uset stack overflow!");
        }
        if (copyout(p->pagetable, sp, argv[argc],
                strlen(argv[argc]) + 1) < 0) {
            panic("copy argv failed!");
        }
        ustack[argc] = sp;
    }
    ustack[argc] = 0;
    sp -= (argc + 1) * sizeof(uint64);
    sp -= sp % 16;
    if (sp < spb) {
        panic("uset stack overflow!");
    }
    if (copyout(p->pagetable, sp, (char *)ustack,
            (argc + 1) * sizeof(uint64)) < 0) {
        panic("copy argc failed!");
    }
    t->trapframe->a1 = sp;
    t->trapframe->sp = sp;
    return argc; 
}

int exec(char *path, char **argv)
{
    infof("exec : %s\n", path);
    struct inode *ip;
    struct proc *p = curr_proc();
    if ((ip = namei(path)) == 0) {
        errorf("invalid file name %s\n", path);
        return -1;
    }
    struct thread *t = curr_thread();
    freethread(t);
    t->state = T_UNUSED;
    uvmunmap(p->pagetable, 0, p->max_page, 1);
    bin_loader(ip, p);
    iput(ip);
    t->state = RUNNING;
    return push_argv(p, argv);
}

int wait(int pid, int *code)
{
    struct proc *np;
    int havekids;
    struct proc *p = curr_proc();
    struct thread *t = curr_thread();

    for (;;) {
        havekids = 0;
        for (np = pool; np < &pool[NPROC]; np++) {
            if (np->state != P_UNUSED && np->parent == p &&
                (pid <= 0 || np->pid == pid)) {
                havekids = 1;
                if (np->state == ZOMBIE) {
                    np->state = P_UNUSED;
                    pid = np->pid;
                    *code = np->exit_code;
                    memset((void *)np->threads[0].kstack, 9,
                           KSTACK_SIZE);
                    return pid;
                }
            }
        }
        if (!havekids) {
            return -1;
        }
        t->state = RUNNABLE;
        add_task(t);
        sched();
    }
}

void exit(int code)
{
    struct proc *p = curr_proc();
    struct thread *t = curr_thread();
    t->exit_code = code;
    t->state = EXITED;
    int tid = t->tid;
    debugf("thread exit with %d", code);
    freethread(t);
    if (tid == 0) {
        p->exit_code = code;
        freeproc(p);
        debugf("proc exit");
        if (p->parent != NULL) {
            p->state = ZOMBIE;
        }
        struct proc *np;
        for (np = pool; np < &pool[NPROC]; np++) {
            if (np->parent == p) {
                np->parent = NULL;
            }
        }
    }
    sched();
}

int fdalloc(struct file *f)
{
    debugf("debugf f = %p, type = %d", f, f->type);
    struct proc *p = curr_proc();
    for (int i = 0; i < FD_BUFFER_SIZE; ++i) {
        if (p->files[i] == NULL) {
            p->files[i] = f;
            debugf("debugf fd = %d, f = %p", i, p->files[i]);
            return i;
        }
    }
    return -1;
}