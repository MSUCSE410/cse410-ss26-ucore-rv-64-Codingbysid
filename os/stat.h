#ifndef STAT_H
#define STAT_H

#include "types.h"

typedef struct Stat {
    uint64 dev;     // Device number
    uint64 ino;     // The inode number where the inode file is located
    uint32 mode;    // File Type
    uint32 nlink;   // The number of hard links, initially 1
    uint64 pad[7];  // For compatibility only, can be ignored
} Stat;

#endif // STAT_H