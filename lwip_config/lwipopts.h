#ifndef LWIP_HDR_LWIPOPTS_H
#define LWIP_HDR_LWIPOPTS_H

#define NO_SYS 1

#define SYS_ARCH_DECL_PROTECT(lev)
#define SYS_ARCH_PROTECT(lev)
#define SYS_ARCH_UNPROTECT(lev)

#define LWIP_SOCKET 0
#define LWIP_NETCONN 0

#define LWIP_DHCP 1

#define MEM_SIZE (1024 * 1024)

#endif
