#include "fs.h"
#include "bio.h"
#include "defs.h"
#include "file.h"
#include "proc.h"
#include "riscv.h"
#include "types.h"

struct superblock sb;

static void readsb(int dev, struct superblock *sb)
{
    struct buf *bp;
    bp = bread(dev, 1);
    memmove(sb, bp->data, sizeof(*sb));
    brelse(bp);
}

void fsinit()
{
    int dev = ROOTDEV;
    readsb(dev, &sb);
    if (sb.magic != FSMAGIC) {
        panic("invalid file system");
    }
}

static void bzero(int dev, int bno)
{
    struct buf *bp;
    bp = bread(dev, bno);
    memset(bp->data, 0, BSIZE);
    bwrite(bp);
    brelse(bp);
}

static uint balloc(uint dev)
{
    int b, bi, m;
    struct buf *bp;

    bp = 0;
    for (b = 0; b < sb.size; b += BPB) {
        bp = bread(dev, BBLOCK(b, sb));
        for (bi = 0; bi < BPB && b + bi < sb.size; bi++) {
            m = 1 << (bi % 8);
            if ((bp->data[bi / 8] & m) == 0) { 
                bp->data[bi / 8] |= m; 
                bwrite(bp);
                brelse(bp);
                bzero(dev, b + bi);
                return b + bi;
            }
        }
        brelse(bp);
    }
    panic("balloc: out of blocks");
    return 0;
}

static void bfree(int dev, uint b)
{
    struct buf *bp;
    int bi, m;

    bp = bread(dev, BBLOCK(b, sb));
    bi = b % BPB;
    m = 1 << (bi % 8);
    if ((bp->data[bi / 8] & m) == 0)
        panic("freeing free block");
    bp->data[bi / 8] &= ~m;
    bwrite(bp);
    brelse(bp);
}

struct {
    struct inode inode[NINODE];
} itable;

static struct inode *iget(uint dev, uint inum);

struct inode *ialloc(uint dev, short type)
{
    int inum;
    struct buf *bp;
    struct dinode *dip;

    for (inum = 1; inum < sb.ninodes; inum++) {
        bp = bread(dev, IBLOCK(inum, sb));
        dip = (struct dinode *)bp->data + inum % IPB;
        if (dip->type == 0) { 
            memset(dip, 0, sizeof(*dip));
            dip->type = type;
            dip->nlink = 1; // LAB4: Initialize link count
            bwrite(bp);
            brelse(bp);
            return iget(dev, inum);
        }
        brelse(bp);
    }
    panic("ialloc: no inodes");
    return 0;
}

void iupdate(struct inode *ip)
{
    struct buf *bp;
    struct dinode *dip;

    bp = bread(ip->dev, IBLOCK(ip->inum, sb));
    dip = (struct dinode *)bp->data + ip->inum % IPB;
    dip->type = ip->type;
    dip->size = ip->size;
    dip->nlink = ip->nlink; // LAB4: update link count
    memmove(dip->addrs, ip->addrs, sizeof(ip->addrs));
    bwrite(bp);
    brelse(bp);
}

static struct inode *iget(uint dev, uint inum)
{
    struct inode *ip, *empty;
    empty = 0;
    for (ip = &itable.inode[0]; ip < &itable.inode[NINODE]; ip++) {
        if (ip->ref > 0 && ip->dev == dev && ip->inum == inum) {
            ip->ref++;
            return ip;
        }
        if (empty == 0 && ip->ref == 0) 
            empty = ip;
    }

    if (empty == 0)
        panic("iget: no inodes");

    ip = empty;
    ip->dev = dev;
    ip->inum = inum;
    ip->ref = 1;
    ip->valid = 0;
    return ip;
}

struct inode *idup(struct inode *ip)
{
    ip->ref++;
    return ip;
}

void ivalid(struct inode *ip)
{
    struct buf *bp;
    struct dinode *dip;
    if (ip->valid == 0) {
        bp = bread(ip->dev, IBLOCK(ip->inum, sb));
        dip = (struct dinode *)bp->data + ip->inum % IPB;
        ip->type = dip->type;
        ip->size = dip->size;
        ip->nlink = dip->nlink; // LAB4: get link count
        memmove(ip->addrs, dip->addrs, sizeof(ip->addrs));
        brelse(bp);
        ip->valid = 1;
        if (ip->type == 0)
            panic("ivalid: no type");
    }
}

void iput(struct inode *ip)
{
    // LAB4: Delete file content safely when 0 links remain
    if (ip->ref == 1 && ip->valid && ip->nlink == 0) {
        itrunc(ip);
        ip->type = 0;
        iupdate(ip);
        ip->valid = 0;
    }
    ip->ref--;
}

static uint bmap(struct inode *ip, uint bn)
{
    uint addr, *a;
    struct buf *bp;

    if (bn < NDIRECT) {
        if ((addr = ip->addrs[bn]) == 0)
            ip->addrs[bn] = addr = balloc(ip->dev);
        return addr;
    }
    bn -= NDIRECT;

    if (bn < NINDIRECT) {
        if ((addr = ip->addrs[NDIRECT]) == 0)
            ip->addrs[NDIRECT] = addr = balloc(ip->dev);
        bp = bread(ip->dev, addr);
        a = (uint *)bp->data;
        if ((addr = a[bn]) == 0) {
            a[bn] = addr = balloc(ip->dev);
            bwrite(bp);
        }
        brelse(bp);
        return addr;
    }

    panic("bmap: out of range");
    return 0;
}

void itrunc(struct inode *ip)
{
    int i, j;
    struct buf *bp;
    uint *a;

    for (i = 0; i < NDIRECT; i++) {
        if (ip->addrs[i]) {
            bfree(ip->dev, ip->addrs[i]);
            ip->addrs[i] = 0;
        }
    }

    if (ip->addrs[NDIRECT]) {
        bp = bread(ip->dev, ip->addrs[NDIRECT]);
        a = (uint *)bp->data;
        for (j = 0; j < NINDIRECT; j++) {
            if (a[j])
                bfree(ip->dev, a[j]);
        }
        brelse(bp);
        bfree(ip->dev, ip->addrs[NDIRECT]);
        ip->addrs[NDIRECT] = 0;
    }

    ip->size = 0;
    iupdate(ip);
}

int readi(struct inode *ip, int user_dst, uint64 dst, uint off, uint n)
{
    uint tot, m;
    struct buf *bp;

    if (off > ip->size || off + n < off)
        return 0;
    if (off + n > ip->size)
        n = ip->size - off;

    for (tot = 0; tot < n; tot += m, off += m, dst += m) {
        bp = bread(ip->dev, bmap(ip, off / BSIZE));
        m = MIN(n - tot, BSIZE - off % BSIZE);
        if (either_copyout(user_dst, dst, (char *)bp->data + (off % BSIZE), m) == -1) {
            brelse(bp);
            tot = -1;
            break;
        }
        brelse(bp);
    }
    return tot;
}

int writei(struct inode *ip, int user_src, uint64 src, uint off, uint n)
{
    uint tot, m;
    struct buf *bp;

    if (off > ip->size || off + n < off)
        return -1;
    if (off + n > MAXFILE * BSIZE)
        return -1;

    for (tot = 0; tot < n; tot += m, off += m, src += m) {
        bp = bread(ip->dev, bmap(ip, off / BSIZE));
        m = MIN(n - tot, BSIZE - off % BSIZE);
        if (either_copyin(user_src, src, (char *)bp->data + (off % BSIZE), m) == -1) {
            brelse(bp);
            break;
        }
        bwrite(bp);
        brelse(bp);
    }

    if (off > ip->size)
        ip->size = off;

    iupdate(ip);
    return tot;
}

struct inode *dirlookup(struct inode *dp, char *name, uint *poff)
{
    uint off, inum;
    struct dirent de;

    if (dp->type != T_DIR)
        panic("dirlookup not DIR");

    for (off = 0; off < dp->size; off += sizeof(de)) {
        if (readi(dp, 0, (uint64)&de, off, sizeof(de)) != sizeof(de))
            panic("dirlookup read");
        if (de.inum == 0)
            continue;
        if (strncmp(name, de.name, DIRSIZ) == 0) {
            if (poff)
                *poff = off;
            inum = de.inum;
            return iget(dp->dev, inum);
        }
    }

    return 0;
}

int dirls(struct inode *dp)
{
    uint64 off, count;
    struct dirent de;

    if (dp->type != T_DIR)
        panic("dirlookup not DIR");

    count = 0;
    for (off = 0; off < dp->size; off += sizeof(de)) {
        if (readi(dp, 0, (uint64)&de, off, sizeof(de)) != sizeof(de))
            panic("dirlookup read");
        if (de.inum == 0)
            continue;
        printf("%s\n", de.name);
        count++;
    }
    return count;
}

int dirlink(struct inode *dp, char *name, uint inum)
{
    int off;
    struct dirent de;
    struct inode *ip;
    if ((ip = dirlookup(dp, name, 0)) != 0) {
        iput(ip);
        return -1;
    }

    for (off = 0; off < dp->size; off += sizeof(de)) {
        if (readi(dp, 0, (uint64)&de, off, sizeof(de)) != sizeof(de))
            panic("dirlink read");
        if (de.inum == 0)
            break;
    }
    strncpy(de.name, name, DIRSIZ);
    de.inum = inum;
    if (writei(dp, 0, (uint64)&de, off, sizeof(de)) != sizeof(de))
        panic("dirlink");
    return 0;
}

struct inode *root_dir()
{
    struct inode *r = iget(ROOTDEV, ROOTINO);
    ivalid(r);
    return r;
}

struct inode *namei(char *path)
{
    int skip = 0;
    struct inode *dp = root_dir();
    if (dp == 0)
        panic("fs dumped.\n");
    return dirlookup(dp, path + skip, 0);
}