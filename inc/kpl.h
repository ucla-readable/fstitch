#ifndef __KUDOS_INC_KPL_H
#define __KUDOS_INC_KPL_H

/* these are the KPL calls that do not operate on a file descriptor */

/* these four mirror the JOS calls */
int kpl_open(const char* path, int mode);
int kpl_remove(const char* path);
int kpl_sync(void);
int kpl_shutdown(void);

/* these are other features */
int kpl_link(const char *oldname, const char *newname);
int kpl_rename(const char *oldname, const char *newname);
int kpl_mkdir(const char *name);
int kpl_rmdir(const char *name);

#endif /* __KUDOS_INC_KPL_H */
