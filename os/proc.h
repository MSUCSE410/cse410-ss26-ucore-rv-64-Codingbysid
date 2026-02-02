#ifndef PROC_H
#define PROC_H

#include "types.h"

#define NPROC (16)
#define MAX_SYSCALL_NUM (500) // Defined to accommodate syscall ID 410 [cite: 10, 26]

// Saved registers for kernel context switches.
struct context {
    uint64 ra;
    uint64 sp;

    // callee-saved
    uint64 s0;
    uint64 s1;
    uint64 s2;
    uint64 s3;
    uint64 s4;
    uint64 s5;
    uint64 s6;
    uint64 s7;
    uint64 s8;
    uint64 s9;
    uint64 s10;
    uint64 s11;
};

// Process states [cite: 12, 35]
enum procstate { UNUSED, USED, SLEEPING, RUNNABLE, RUNNING, ZOMBIE };

/*
* LAB1: Definition for TaskInfo to be used by sys_task_info 
*/
typedef enum procstate TaskStatus; 

struct TaskInfo {
    TaskStatus status;                             // the task control block (task status) [cite: 12, 24]
    unsigned int syscall_times[MAX_SYSCALL_NUM];   // the number of system calls used by the task [cite: 13, 26]
    int time;                                      // the total running time of the task [cite: 18, 27]
};



// Per-process state
struct proc {
    enum procstate state;        // Process state [cite: 35]
    int pid;                     // Process ID [cite: 35]
    uint64 ustack;               // Virtual address of user stack [cite: 36]
    uint64 kstack;               // Virtual address of kernel stack [cite: 37]
    struct trapframe *trapframe; // data page for trampoline.S [cite: 38]
    struct context context;      // swtch() here to run process [cite: 39]

    /*
    * LAB1: New fields for task information tracking [cite: 41]
    */
    unsigned int syscall_times[MAX_SYSCALL_NUM]; // To track count of each syscall used [cite: 13, 100]
    uint64 start_time;                           // To track total running time [cite: 18, 82]
};

struct proc *curr_proc();
void exit(int);
void proc_init();
void scheduler() __attribute__((noreturn));
void sched();
void yield();
struct proc *allocproc();
// swtch.S
void swtch(struct context *, struct context *);

#endif // PROC_H