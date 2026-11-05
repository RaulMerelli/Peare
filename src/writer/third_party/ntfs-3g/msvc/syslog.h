#ifndef PEARE_NTFS_SYSLOG_H
#define PEARE_NTFS_SYSLOG_H

#define LOG_PID 0
#define LOG_USER 0
#define LOG_NOTICE 0
#define openlog(...) ((void)0)
#define closelog(...) ((void)0)
#define syslog(...) ((void)0)
#define vsyslog(...) ((void)0)

#endif
