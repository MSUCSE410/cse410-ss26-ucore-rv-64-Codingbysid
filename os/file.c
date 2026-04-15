#include "file.h"
#include "defs.h"
#include "fcntl.h"
#include "fs.h"
#include "proc.h"
#include "stat.h"

struct file filepool[FILEPOOLSIZE];

struct file *stdio_init(int fd)
{
    struct file *f = filealloc();
    f->type = FD_STDIO;
    f->ref = 1;
    f->readable = (fd == STDIN || fd == STDERR);
    f->writable = (fd == STDOUT || fd == STDERR);
    return f;
}

void fileclose(struct file *f)
{
    if (f->ref < 1)
        panic("fileclose");
    if (--f->ref > 0) {
        return;
    }
    switch (f->type) {
    case FD_STDIO:
        break;
    case FD_INODE:
        iput(f->ip);
        break;
    default:
        panic("unknown file type %d\n", f->type);
    }

    f->off = 0;
    f->readable = 0;
    f->writable = 0;
    f->ref = 0;
    f->type = FD_NONE;
}

struct file *filealloc()
{
    for (int i = 0; i < FILEPOOLSIZE; ++i) {
        if (filepool[i].ref == 0) {
            filepool[i].ref = 1;
            return &filepool[i];
        }
    }
    return 0;
}

int show_all_files()
{
    return dirls(root_dir());
}

static struct inode *create(char *path, short type)
{
    struct inode *ip, *dp;
    dp = root_dir(); 
    ivalid(dp);
    if ((ip = dirlookup(dp, path, 0)) != 0) {
        warnf("create a exist file\n");
        iput(dp); 
        ivalid(ip);
        if (type == T_FILE && ip->type == T_FILE)
            return ip;
        iput(ip);
        return 0;
    }
    if ((ip = ialloc(dp->dev, type)) == 0)
        panic("create: ialloc");

    tracef("create dinode and inode type = %d\n", type);

    ivalid(ip);
    iupdate(ip);
    if (dirlink(dp, path, ip->inum) < 0)
        panic("create: dirlink");

    iput(dp);
    return ip;
}

int fileopen(char *path, uint64 omode)
{
    int fd;
    struct file *f;
    struct inode *ip;
    if (omode & O_CREATE) {
        ip = create(path, T_FILE);
        if (ip == 0) {
            return -1;
        }
    } else {
        if ((ip = namei(path)) == 0) {
            return -1;
        }
        ivalid(ip);
    }
    if (ip->type != T_FILE)
        panic("unsupported file inode type\n");
    if ((f = filealloc()) == 0 || (fd = fdalloc(f)) < 0) { 
        if (f) fileclose(f);
        iput(ip);
        return -1;
    }
    f->type = FD_INODE;
    f->off = 0;
    f->ip = ip;
    f->readable = !(omode & O_WRONLY);
    f->writable = (omode & O_WRONLY) || (omode & O_RDWR);
    if ((omode & O_TRUNC) && ip->type == T_FILE) {
        itrunc(ip);
    }
    return fd;
}

uint64 inodewrite(struct file *f, uint64 va, uint64 len)
{
    int r;
    ivalid(f->ip);
    if ((r = writei(f->ip, 1, va, f->off, len)) > 0)
        f->off += r;
    return r;
}

uint64 inoderead(struct file *f, uint64 va, uint64 len)
{
    int r;
    ivalid(f->ip);
    if ((r = readi(f->ip, 1, va, f->off, len)) > 0)
        f->off += r;
    return r;
}

// LAB 4 Additions
struct file *filedup(struct file *f)
{
    if (f->ref < 1)
        panic("filedup");
    f->ref++;
    return f;
}

int filestat(struct file *f, uint64 addr)
{
    struct proc *p = curr_proc();
    Stat st;
    if (f->type == FD_INODE) {
        ivalid(f->ip);
        st.dev = f->ip->dev;
        st.ino = f->ip->inum;
        
        // Map internal kernel types to user-space expected stat modes
        if (f->ip->type == T_FILE) {
            st.mode = 0x100000;
        } else if (f->ip->type == T_DIR) {
            st.mode = 0x040000;
        } else {
            st.mode = 0;
        }
        
        st.nlink = f->ip->nlink;
        memset(st.pad, 0, sizeof(st.pad)); // Clear padding
        
        if (copyout(p->pagetable, addr, (char *)&st, sizeof(st)) < 0)
            return -1;
        return 0;
    }
    return -1;
}