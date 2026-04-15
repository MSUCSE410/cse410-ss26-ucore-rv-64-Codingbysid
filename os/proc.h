#ifndef PROC_H
#define PROC_H

#include "riscv.h"
#include "types.h"

#define NPROC (512)
#define FD_BUFFER_SIZE (16)

struct file;

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

enum procstate { UNUSED, USED, SLEEPING, RUNNABLE, RUNNING, ZOMBIE };

struct proc {
    enum procstate state; 
    int pid; 
    pagetable_t pagetable; 
    uint64 ustack; 
    uint64 kstack; 
    struct trapframe *trapframe; 
    struct context context; 
    uint64 max_page;
    struct proc *parent; 
    uint64 exit_code;
    struct file *files[FD_BUFFER_SIZE]; 
};

int cpuid();
struct proc *curr_proc();
void exit(int);
void proc_init();
void scheduler() __attribute__((noreturn));
void sched();
void yield();
int fork();
int exec(char *, char **);
int wait(int, int *);
void add_task(struct proc *);
struct proc *pop_task();
struct proc *allocproc();
int fdalloc(struct file *);
int init_stdio(struct proc *);
int push_argv(struct proc *, char **);
void swtch(struct context *, struct context *);

// LAB 4 Additions
int spawn(char *name);

#endif // PROC_H