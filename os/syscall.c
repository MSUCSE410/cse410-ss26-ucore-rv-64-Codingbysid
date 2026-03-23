#include "syscall.h"
#include "defs.h"
#include "loader.h"
#include "syscall_ids.h"
#include "timer.h"
#include "trap.h"

// External references for virtual memory management
extern uint64 useraddr(pagetable_t pagetable, uint64 va);
extern uint64 walkaddr(pagetable_t pagetable, uint64 va);
extern void* kalloc(void);
extern void kfree(void *pa);
extern int mappages(pagetable_t pagetable, uint64 va, uint64 size, uint64 pa, int perm);
extern void uvmunmap(pagetable_t pagetable, uint64 va, uint64 npages, int do_free);

uint64 get_cycle(); 

uint64 sys_write(int fd, char *str, uint len) {
    if (fd != STDOUT) return -1;
    struct proc *p = curr_proc();
    
    for (int i = 0; i < len; ++i) {
        // Translate the user virtual address of each character to a physical address
        uint64 va = (uint64)str + i;
        uint64 pa = useraddr(p->pagetable, va);
        if (pa == 0) return -1; 
        
        console_putchar(*(char *)pa);
    }
    return len;
}

__attribute__((noreturn)) void sys_exit(int code) {
    exit(code);
    __builtin_unreachable();
}

uint64 sys_sched_yield() {
    yield();
    return 0;
}

uint64 sys_gettimeofday(uint64 val_va, int _tz) {
    struct proc *p = curr_proc();
    if (val_va == 0) return -1;

    // Create a local copy to gather the timing data
    TimeVal local_val;
    uint64 cycle = get_cycle();
    local_val.sec = cycle / CPU_FREQ;
    local_val.usec = (cycle % CPU_FREQ) * 1000000 / CPU_FREQ;

    // Safely copy the data byte-by-byte into the translated physical memory
    uint8 *src = (uint8 *)&local_val;
    for (int i = 0; i < sizeof(TimeVal); ++i) {
        uint64 pa = useraddr(p->pagetable, val_va + i);
        if (pa == 0) return -1;
        *(uint8 *)pa = src[i];
    }
    return 0;
}

uint64 sys_task_info(uint64 ti_va) {
    struct proc *p = curr_proc(); 
    if (ti_va == 0) return -1;

    // Create a local copy to gather the task data
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

    // Safely copy the data byte-by-byte into the translated physical memory
    uint8 *src = (uint8 *)&local_ti;
    for(int i = 0; i < sizeof(struct TaskInfo); i++){
        uint64 pa = useraddr(p->pagetable, ti_va + i);
        if (pa == 0) return -1;
        *(uint8 *)pa = src[i];
    }
    return 0;
}

// TASK 2: mmap Implementation
uint64 sys_mmap(uint64 start, uint64 len, int port, int flag, int fd) {
    // 1. Strict Parameter Checking: Reject unaligned addresses
    if (start % 4096 != 0) return -1;
    if ((port & ~0x7) != 0) return -1; 
    if ((port & 0x7) == 0) return -1; 

    uint64 a_len = (len + 4096ULL - 1) & ~(4096ULL - 1);
    struct proc *p = curr_proc();

    // 2. Check if ANY page in the requested range is already mapped
    for (uint64 va = start; va < start + a_len; va += 4096) {
        if (walkaddr(p->pagetable, va) != 0) {
            return -1; 
        }
    }

    // 3. Set up permissions
    int perm = 1 | 16; // PTE_V | PTE_U
    if (port & 1) perm |= 2; // PTE_R
    if (port & 2) perm |= 4; // PTE_W
    if (port & 4) perm |= 8; // PTE_X

    // 4. Map physical memory
    for (uint64 va = start; va < start + a_len; va += 4096) {
        void *pa = kalloc();
        if (pa == 0) return -1; // Out of memory
        
        if (mappages(p->pagetable, va, 4096, (uint64)pa, perm) != 0) {
            kfree(pa);
            return -1;
        }
        
        // Track the highest mapped page
        uint64 current_page = va / 4096;
        if (current_page > p->max_page) {
            p->max_page = current_page;
        }
    }

    return 0; // Return 0 on success
}

// TASK 2: munmap Implementation
uint64 sys_munmap(uint64 start, uint64 len) {
    // 1. Strict Parameter Checking: Reject unaligned addresses
    if (start % 4096 != 0) return -1;
    uint64 a_len = (len + 4096ULL - 1) & ~(4096ULL - 1);
    struct proc *p = curr_proc();

    // 2. Check if ANY page in the range is NOT mapped
    for (uint64 va = start; va < start + a_len; va += 4096) {
        if (walkaddr(p->pagetable, va) == 0) {
            return -1; 
        }
    }

    // 3. Unmap the memory safely
    uvmunmap(p->pagetable, start, a_len / 4096, 1);
    return 0; // Return 0 on success
}

void syscall() {
    struct proc *p = curr_proc();
    struct trapframe *trapframe = p->trapframe;
    int id = trapframe->a7, ret;
    uint64 args[6] = { trapframe->a0, trapframe->a1, trapframe->a2,
                       trapframe->a3, trapframe->a4, trapframe->a5 };
    
    if (id >= 0 && id < MAX_SYSCALL_NUM) {
        p->syscall_times[id]++;
    }

    switch (id) {
    case SYS_write:
        ret = sys_write(args[0], (char *)args[1], args[2]);
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
        ret = p->pid; 
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
}