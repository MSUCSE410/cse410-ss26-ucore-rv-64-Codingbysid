#include "syscall.h"
#include "console.h"
#include "defs.h"
#include "loader.h"
#include "syscall_ids.h"
#include "timer.h"
#include "trap.h"
#include "vm.h"

uint64 get_cycle(); 

uint64 sys_write(int fd, uint64 va, uint len)
{
    debugf("sys_write fd = %d str = %x, len = %d", fd, va, len);
    if (fd != STDOUT)
        return -1;
    struct proc *p = curr_proc();
    char str[MAX_STR_LEN];
    int size = copyinstr(p->pagetable, str, va, MIN(len, MAX_STR_LEN));
    debugf("size = %d", size);
    for (int i = 0; i < size; ++i) {
        console_putchar(str[i]);
    }
    return size;
}

uint64 sys_read(int fd, uint64 va, uint64 len)
{
    debugf("sys_read fd = %d str = %x, len = %d", fd, va, len);
    if (fd != STDIN) return -1;
        
    int c;
    while (1) {
        c = consgetc();
        if (c == 255 || c == -1 || c == 0) {
            yield(); 
            continue;
        }
        break; // Valid character found
    }
    
    char ch = (char)c;
    copyout(curr_proc()->pagetable, va, &ch, 1);
    return 1; 
}

__attribute__((noreturn)) void sys_exit(int code)
{
    exit(code);
    __builtin_unreachable();
}

uint64 sys_sched_yield()
{
    yield();
    return 0;
}

uint64 sys_gettimeofday(uint64 val, int _tz)
{
    struct proc *p = curr_proc();
    if (val == 0) return -1;
    uint64 cycle = get_cycle();
    TimeVal t;
    t.sec = cycle / CPU_FREQ;
    t.usec = (cycle % CPU_FREQ) * 1000000 / CPU_FREQ;
    copyout(p->pagetable, val, (char *)&t, sizeof(TimeVal));
    return 0;
}

uint64 sys_task_info(uint64 ti_va) {
    struct proc *p = curr_proc(); 
    if (ti_va == 0) return -1;

    struct TaskInfo local_ti;
    local_ti.status = 2; 
    for (int i = 0; i < MAX_SYSCALL_NUM; i++) {
        local_ti.syscall_times[i] = p->syscall_times[i];
    }
    
    if (p->start_time == 0 || CPU_FREQ == 0) {
        local_ti.time = 0;
    } else {
        local_ti.time = (int)((get_cycle() - p->start_time) / (CPU_FREQ / 1000));
    }

    copyout(p->pagetable, ti_va, (char *)&local_ti, sizeof(struct TaskInfo));
    return 0;
}

uint64 sys_mmap(uint64 start, uint64 len, int port, int flag, int fd) {
    if (start % 4096 != 0) return -1;
    if (len == 0) return 0; 
    if (len > 1024 * 1024 * 1024) return -1; 
    if ((port & ~0x7) != 0) return -1; 
    if ((port & 0x7) == 0) return -1; 

    uint64 a_len = (len + 4096ULL - 1) & ~(4096ULL - 1);
    struct proc *p = curr_proc();

    for (uint64 va = start; va < start + a_len; va += 4096) {
        if (walkaddr(p->pagetable, va) != 0) {
            return -1; 
        }
    }

    int perm = 1 | 16; 
    if (port & 1) perm |= 2; 
    if (port & 2) perm |= 4; 
    if (port & 4) perm |= 8; 

    for (uint64 va = start; va < start + a_len; va += 4096) {
        void *pa = kalloc();
        if (pa == 0) return -1; 
        
        if (mappages(p->pagetable, va, 4096, (uint64)pa, perm) != 0) {
            kfree(pa);
            return -1;
        }
        
        uint64 current_page = va / 4096;
        if (current_page > p->max_page) {
            p->max_page = current_page;
        }
    }

    return 0; 
}

uint64 sys_munmap(uint64 start, uint64 len) {
    if (start % 4096 != 0) return -1;
    uint64 a_len = (len + 4096ULL - 1) & ~(4096ULL - 1);
    struct proc *p = curr_proc();

    for (uint64 va = start; va < start + a_len; va += 4096) {
        if (walkaddr(p->pagetable, va) == 0) {
            return -1; 
        }
    }

    uvmunmap(p->pagetable, start, a_len / 4096, 1);
    return 0; 
}

uint64 sys_getpid()
{
    return curr_proc()->pid;
}

uint64 sys_getppid()
{
    struct proc *p = curr_proc();
    return p->parent == NULL ? IDLE_PID : p->parent->pid;
}

uint64 sys_clone()
{
    debugf("fork!\n");
    return fork();
}

uint64 sys_exec(uint64 va)
{
    struct proc *p = curr_proc();
    char name[200];
    copyinstr(p->pagetable, name, va, 200);
    debugf("sys_exec %s\n", name);
    return exec(name);
}

uint64 sys_wait(int pid, uint64 va)
{
    return wait(pid, (int*)va);
}

// Project 3: sys_spawn
uint64 sys_spawn(uint64 va)
{
    char name[200];
    struct proc *p = curr_proc();
    
    copyinstr(p->pagetable, name, va, 200);
    extern int spawn(char*); 
    return spawn(name);
}

// Project 3: sys_set_priority
uint64 sys_set_priority(long long prio){
    if (prio < 2) return -1; 
    struct proc *p = curr_proc();
    p->priority = prio;
    p->pass = BIG_STRIDE / p->priority;
    return p->priority; 
}

extern char trap_page[];

void syscall()
{
    struct trapframe *trapframe = curr_proc()->trapframe;
    int id = trapframe->a7, ret;
    uint64 args[6] = { trapframe->a0, trapframe->a1, trapframe->a2,
               trapframe->a3, trapframe->a4, trapframe->a5 };
    tracef("syscall %d args = [%x, %x, %x, %x, %x, %x]", id, args[0],
           args[1], args[2], args[3], args[4], args[5]);
           
    if (id >= 0 && id < MAX_SYSCALL_NUM) {
        curr_proc()->syscall_times[id]++;
    }

    switch (id) {
    case SYS_write:
        ret = sys_write(args[0], args[1], args[2]);
        break;
    case SYS_read:
        ret = sys_read(args[0], args[1], args[2]);
        break;
    case SYS_exit:
        sys_exit(args[0]);
    case SYS_sched_yield:
        ret = sys_sched_yield();
        break;
    case SYS_gettimeofday:
        ret = sys_gettimeofday(args[0], args[1]);
        break;
    case SYS_getpid:
        ret = sys_getpid();
        break;
    case SYS_getppid:
        ret = sys_getppid();
        break;
    case SYS_clone: 
        ret = sys_clone();
        break;
    case SYS_execve:
        ret = sys_exec(args[0]);
        break;
    case SYS_wait4:
        ret = sys_wait(args[0], args[1]);
        break;
    case SYS_spawn:
        ret = sys_spawn(args[0]);
        break;
    case SYS_setpriority: 
        ret = sys_set_priority(args[0]);
        break;
    case SYS_task_info: 
        ret = sys_task_info(args[0]);
        break;
    case SYS_mmap: 
        ret = sys_mmap(args[0], args[1], args[2], args[3], args[4]);
        break;
    case SYS_munmap: 
        ret = sys_munmap(args[0], args[1]);
        break;
    default:
        ret = -1;
        errorf("unknown syscall %d", id);
    }
    trapframe->a0 = ret;
    tracef("syscall ret %d", ret);
}