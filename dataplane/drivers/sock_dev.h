#ifndef SOCKDEV_H
#define SOCKDEV_H

#define SOCK_DEV_PREFIX "sock_dev:"

int
sock_dev_create(const char *path, const char *name, int numa_node);

#endif
