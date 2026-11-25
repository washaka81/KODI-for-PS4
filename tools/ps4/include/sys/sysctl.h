#ifndef _SYS_SYSCTL_H_STUB_
#define _SYS_SYSCTL_H_STUB_
#include <sys/types.h>
#define CTL_KERN 1
#define KERN_PROC 14
#define KERN_PROC_PATHNAME 12
static inline int sysctl(const int* mib, unsigned int namelen, void* oldp, size_t* oldlenp, const void* newp, size_t newlen) {
  (void)mib; (void)namelen; (void)oldp; (void)oldlenp; (void)newp; (void)newlen;
  return -1;
}
#endif
