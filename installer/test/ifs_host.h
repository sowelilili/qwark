/* The host file system's knobs, for test_installer.c. */
#ifndef QWARK_INSTALLER_IFS_HOST_H
#define QWARK_INSTALLER_IFS_HOST_H

#define IFS_OP_OPEN_READ    1
#define IFS_OP_OPEN_WRITE   2
#define IFS_OP_READ         3
#define IFS_OP_WRITE        4
#define IFS_OP_SYNC         5
#define IFS_OP_CLOSE        6
#define IFS_OP_STAT         7
#define IFS_OP_MKDIR        8
#define IFS_OP_RENAME       9   /* matched on the target */
#define IFS_OP_UNLINK       10

/* The folder that stands for the console: root/dev_hdd0 is /dev_hdd0. */
void ifs_host_root(const char *root);

/* No faults, no open files, the call count back to 0. */
void ifs_host_reset(void);

/* The calls made since the reset. */
int ifs_host_calls(void);

/* Call n (1-based) fails. */
void ifs_host_fail_at(int call);

/* Call n and every call after it fail: what a pulled drive looks like. */
void ifs_host_fail_from(int call);

/* Every call of this kind on a path holding path_part fails. */
void ifs_host_fault(int op, const char *path_part);

/* Every write to a path holding path_part succeeds but stores one bit wrong. */
void ifs_host_corrupt(const char *path_part);

#endif
