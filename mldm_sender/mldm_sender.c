/**
 * Copyright 2017 University Corporation for Atmospheric Research. All rights
 * reserved. See the the file COPYRIGHT in the top-level source-directory for
 * licensing conditions.
 *
 *   @file: mldm_sender.c
 * @author: Steven R. Emmerson
 *
 * This file implements the multicast LDM sender, which is a program for
 * multicasting LDM data-products from the LDM product-queue to a multicast
 * group using FMTP.
 */

#include "config.h"

#include "atofeedt.h"
#include "AuthServer.h"
#include "CidrAddr.h"
#include "globals.h"
#include "inetutil.h"
#include "ldm.h"
#include "ldmprint.h"
#include "log.h"
#include "mcast_info.h"
#include "MldmRpc.h"
#include "OffsetMap.h"
#include "pq.h"
#include "prod_class.h"
#include "prod_index_map.h"
#include "StrBuf.h"
#include "timestamp.h"

#include <arpa/inet.h>
#include <errno.h>
#include <getopt.h>
#include <libgen.h>
#include <limits.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdbool.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>
#include "../mcast_lib/ldm7/fmtp.h"

#ifndef _XOPEN_PATH_MAX
/* For some reason, the following isn't defined by gcc(1) 4.8.3 on Fedora 19 */
#   define _XOPEN_PATH_MAX 1024 // value mandated by XPG6; includes NUL
#endif

#define NELT(a) (sizeof(a)/sizeof((a)[0]))

/**
 * C-callable FMTP sender.
 */
static FmtpSender*          fmtpSender;
/**
 * Information on the multicast group.
 */
static McastInfo             mcastInfo;
/**
 * Termination signals.
 */
static const int             termSigs[] = {SIGINT, SIGTERM};
/**
 * Signal-set for termination signals.
 */
static sigset_t              termSigSet;
/**
 * FMTP product-index to product-queue offset map.
 */
static OffMap*               offMap;
/**
 * Pool of available IP addresses:
 */
static void*                 inAddrPool;
/**
 * Authorizer of remote clients:
 */
static void*                 authorizer;
/**
 * Multicast LDM RPC command-server:
 */
static void*                 mldmCmdSrvr;
/**
 * Multicast LDM RPC server port in host byte-order:
 */
static in_port_t             mldmSrvrPort;
/**
 * Multicast LDM RPC server thread:
 */
static pthread_t             mldmSrvrThread;

/**
 * Blocks termination signals for the current thread.
 */
static inline void
blockTermSigs(void)
{
    (void)pthread_sigmask(SIG_BLOCK, &termSigSet, NULL);
}

/**
 * Unblocks termination signals for the current thread.
 */
static inline void
unblockTermSigs(void)
{
    (void)pthread_sigmask(SIG_UNBLOCK, &termSigSet, NULL);
}

/**
 * Appends a usage message to the pending log messages.
 */
static void
mls_usage(void)
{
    log_add("\
Usage: %s [options] groupId:groupPort FmtpNetPrefix/prefixLen\n\
Options:\n\
    -f feedExpr       Feedtype expression specifying data to send. Default\n\
                      is EXP.\n\
    -l dest           Log to `dest`. One of: \"\" (system logging daemon, \"-\"\n\
                      (standard error), or file `dest`. Default is \"%s\"\n\
    -m mcastIface     IP address of interface to use to send multicast\n\
                      packets. Default is the system's default multicast\n\
                      interface.\n\
    -p serverPort     Port number for FMTP TCP server. Default is chosen by\n\
                      operating system.\n\
    -q prodQueue      Pathname of product-queue. Default is \"%s\".\n\
    -r retxTimeout    FMTP retransmission timeout in minutes. Duration that a\n\
                      product will be held by the FMTP layer before being\n\
                      released. If negative, then the default FMTP timeout is\n\
                      used.\n\
    -s serverIface    IP Address of interface on which FMTP TCP server will\n\
                      listen. Default is all interfaces.\n\
    -t ttl            Time-to-live of outgoing packets (default is 1):\n\
                           0  Restricted to same host. Won't be output by\n\
                              any interface.\n\
                           1  Restricted to same subnet. Won't be\n\
                              forwarded by a router (default).\n\
                         <32  Restricted to same site, organization or\n\
                              department.\n\
                         <64  Restricted to same region.\n\
                        <128  Restricted to same continent.\n\
                        <255  Unrestricted in scope. Global.\n\
    -v                Verbose logging: log INFO level messages.\n\
    -x                Debug logging: log DEBUG level messages.\n\
Operands:\n\
    groupId:groupPort Internet service address of multicast group, where\n\
                      <groupId> is either group-name or dotted-decimal IPv4\n\
                      address and <groupPort> is port number.\n\
    FmtpNetPrefix/prefixLen\n\
                      Prefix of FMTP network in CIDR format (e.g.\n\
                      \"192.168.8.0/21\").\n",
            log_get_id(), log_get_default_destination(), getDefaultQueuePath());
}

/**
 * Decodes the options of the command-line.
 *
 * @pre                      {`openulog()` has already been called.}
 * @param[in]  argc          Number of arguments.
 * @param[in]  argv          Arguments.
 * @param[out] feed          Feedtypes of data to be sent.
 * @param[out] serverIface   Interface on which FMTP TCP server should listen.
 *                           Caller must not free.
 * @param[out] serverPort    Port number for FMTP TCP server.
 * @param[out] ttl           Time-to-live of outgoing packets.
 *                                0  Restricted to same host. Won't be output by
 *                                   any interface.
 *                                1  Restricted to the same subnet. Won't be
 *                                   forwarded by a router (default).
 *                              <32  Restricted to the same site, organization
 *                                   or department.
 *                              <64  Restricted to the same region.
 *                             <128  Restricted to the same continent.
 *                             <255  Unrestricted in scope. Global.
 * @param[out] ifaceAddr     IP address of the interface to use to send
 *                           multicast packets.
 * @param[out] retxTimeout   FMTP retransmission timeout in minutes. Duration
 *                           that a product will be held by the FMTP layer
 *                           before being released. If negative, then the
 *                           default timeout is used.
 * @retval     0             Success. `*serverIface` or `*ttl` might not have
 *                           been set.
 * @retval     1             Invalid options. `log_add()` called.
 */
static int
mls_decodeOptions(
        int                            argc,
        char* const* const restrict    argv,
        feedtypet* const restrict      feed,
        const char** const restrict    serverIface,
        unsigned short* const restrict serverPort,
        unsigned* const restrict       ttl,
        const char** const restrict    ifaceAddr,
        float* const                   retxTimeout)
{
    const char*  pqfname = getQueuePath();
    int          ch;
    extern int   opterr;
    extern int   optopt;
    extern char* optarg;

    opterr = 1; // prevent getopt(3) from trying to print error messages

    while ((ch = getopt(argc, argv, ":F:f:l:m:p:q:r:s:t:vx")) != EOF)
        switch (ch) {
            case 'f': {
                if (strfeedtypet(optarg, feed)) {
                    log_add("Invalid feed expression: \"%s\"", optarg);
                    return 1;
                }
                break;
            }
            case 'l': {
                (void)log_set_destination(optarg);
                break;
            }
            case 'm': {
                *ifaceAddr = optarg;
                break;
            }
            case 'p': {
                unsigned short port;
                int            nbytes;

                if (1 != sscanf(optarg, "%5hu %n", &port, &nbytes) ||
                        0 != optarg[nbytes]) {
                    log_add("Couldn't decode TCP-server port-number option-argument "
                            "\"%s\"", optarg);
                    return 1;
                }
                *serverPort = port;
                break;
            }
            case 'q': {
                pqfname = optarg;
                break;
            }
            case 'r': {
                int   nbytes;
                if (1 != sscanf(optarg, "%f %n", retxTimeout, &nbytes) ||
                        0 != optarg[nbytes]) {
                    log_add("Couldn't decode FMTP retransmission timeout "
                            "option-argument \"%s\"", optarg);
                    return 1;
                }
                break;
            }
            case 's': {
                *serverIface = optarg;
                break;
            }
            case 't': {
                unsigned t;
                int      nbytes;
                if (1 != sscanf(optarg, "%3u %n", &t, &nbytes) ||
                        0 != optarg[nbytes]) {
                    log_add("Couldn't decode time-to-live option-argument \"%s\"",
                            optarg);
                    return 1;
                }
                if (t >= 255) {
                    log_add("Invalid time-to-live option-argument \"%s\"",
                            optarg);
                    return 1;
                }
                *ttl = t;
                break;
            }
            case 'v': {
                if (!log_is_enabled_info)
                    (void)log_set_level(LOG_LEVEL_INFO);
                break;
            }
            case 'x': {
                (void)log_set_level(LOG_LEVEL_DEBUG);
                break;
            }
            case ':': {
                log_add("Option \"%c\" requires an argument", optopt);
                return 1;
            }
            default: {
                log_add("Unknown option: \"%c\"", optopt);
                return 1;
            }
        }
    setQueuePath(pqfname);

    return 0;
}

/**
 * Sets a service address.
 *
 * @param[in]  id           The Internet identifier. Either a name or a
 *                          formatted IP address.
 * @param[in]  port         The port number.
 * @param[out] serviceAddr  The Internet service address to be set.
 * @retval     0            Success. `*serviceAddr` is set.
 * @retval     1            Invalid argument. `log_add()` called.
 * @retval     2            System failure. `log_add()` called.
 */
static int
mls_setServiceAddr(
        const char* const    id,
        const unsigned short port,
        ServiceAddr** const  serviceAddr)
{
    int status = sa_new(serviceAddr, id, port);

    return (0 == status)
            ? 0
            : (EINVAL == status)
              ? 1
              : 2;
}

/**
 * Decodes the Internet service address of the multicast group.
 *
 * @param[in]  arg        Relevant operand.
 * @param[out] groupAddr  Internet service address of the multicast group.
 *                        Caller should free when it's no longer needed.
 * @retval     0          Success. `*groupAddr` is set.
 * @retval     1          Invalid operand. `log_add()` called.
 * @retval     2          System failure. `log_add()` called.
 */
static int
mls_decodeGroupAddr(
        char* const restrict         arg,
        ServiceAddr** const restrict groupAddr)
{
    int          status = sa_parse(groupAddr, arg);

    if (ENOMEM == status) {
        status = 2;
    }
    else if (status) {
        log_add("Invalid multicast group specification");
    }

    return status;
}

/**
 * Decodes the operands of the command-line.
 *
 * @param[in]  argc          Number of operands.
 * @param[in]  argv          Operands.
 * @param[out] groupAddr     Internet service address of the multicast group.
 *                           Caller should free when it's no longer needed.
 * @param[out] fmtpSubnet    Subnet for client FMTP TCP connections. Caller
 *                           should call `cidrAddr_delete(*fmtpSubnet)` when
 *                           it's no longer needed.
 * @retval     0             Success. `*groupAddr`, `*serverAddr`, `*feed`, and
 *                           `msgQName` are set.
 * @retval     1             Invalid operands. `log_add()` called.
 * @retval     2             System failure. `log_add()` called.
 */
static int
mls_decodeOperands(
        int                          argc,
        char* const* restrict        argv,
        ServiceAddr** const restrict groupAddr,
        CidrAddr** const restrict    fmtpSubnet)
{
    int status;

    if (argc-- < 1) {
        log_add("Multicast group not specified");
        status = 1;
    }
    else {
        ServiceAddr* mcastAddr;
        status = mls_decodeGroupAddr(*argv++, &mcastAddr);
        if (status == 0) {
            if (argc-- < 1) {
                log_add("FMTP network not specified");
                status = 1;
            }
            else {
                CidrAddr* subnet = cidrAddr_parse(*argv++);
                if (subnet == NULL) {
                    log_add("Invalid FMTP subnet specification: \"%s\"",
                            *--argv);
                    status = 1;
                }
                else {
                    *fmtpSubnet = subnet;
                    *groupAddr = mcastAddr;
                    status = 0;
                }
            }
            if (status)
                sa_free(mcastAddr);
        } // `mcastAddr` set
    }

    return status;
}

/**
 * Sets the multicast group information from command-line arguments.
 *
 * @param[in]  serverIface  Interface on which FMTP TCP server should listen.
 *                          Caller must not free.
 * @param[in]  serverPort   Port number for FMTP TCP server.
 * @param[in]  feed         Feedtype of multicast group.
 * @param[in]  groupAddr    Internet service address of multicast group.
 *                          Caller should free when it's no longer needed.
 * @param[out] mcastInfo    Information on multicast group.
 * @retval     0            Success. `*mcastInfo` is set.
 * @retval     1            Invalid argument. `log_add()` called.
 * @retval     2            System failure. `log_add()` called.
 */
static int
mls_setMcastGroupInfo(
        const char* const restrict        serverIface,
        const unsigned short              serverPort,
        const feedtypet                   feed,
        const ServiceAddr* const restrict groupAddr,
        McastInfo** const restrict        mcastInfo)
{
    ServiceAddr* serverAddr;
    int          status = mls_setServiceAddr(serverIface, serverPort,
            &serverAddr);

    if (0 == status) {
        status = mi_new(mcastInfo, feed, groupAddr, serverAddr) ? 2 : 0;
        sa_free(serverAddr);
    }

    return status;
}

/**
 * Decodes the command line.
 *
 * @param[in]  argc          Number of arguments.
 * @param[in]  argv          Arguments.
 * @param[out] mcastInfo     Multicast group information.
 * @param[out] ttl           Time-to-live of outgoing packets.
 *                                 0  Restricted to same host. Won't be output
 *                                    by any interface.
 *                                 1  Restricted to the same subnet. Won't be
 *                                    forwarded by a router (default).
 *                               <32  Restricted to the same site, organization
 *                                    or department.
 *                               <64  Restricted to the same region.
 *                              <128  Restricted to the same continent.
 *                              <255  Unrestricted in scope. Global.
 * @param[out] ifaceAddr     IP address of the interface from which multicast
 *                           packets should be sent or NULL to have them sent
 *                           from the system's default multicast interface.
 * @param[in]  retxTimeout   FMTP retransmission timeout in minutes. Duration
 *                           that a product will be held by the FMTP layer
 *                           before being released. If negative, then the
 *                           default timeout is used.
 * @param[out] timeoutFactor Ratio of the duration that a data-product will
 *                           be held by the FMTP layer before being released
 *                           after being multicast to the duration to
 *                           multicast the product. If negative, then the
 *                           default timeout factor is used.
 * @param[out] fmtpSubnet    Subnet for client FMTP TCP connections
 * @retval     0             Success. `*mcastInfo` is set. `*ttl` might be set.
 * @retval     1             Invalid command line. `log_add()` called.
 * @retval     2             System failure. `log_add()` called.
 */
static int
mls_decodeCommandLine(
        int                         argc,
        char* const* restrict       argv,
        McastInfo** const restrict  mcastInfo,
        unsigned* const restrict    ttl,
        const char** const restrict ifaceAddr,
        float* const                retxTimeout,
        CidrAddr** const restrict   fmtpSubnet)
{
    feedtypet      feed = EXP;
    const char*    serverIface = "0.0.0.0";     // default: all interfaces
    unsigned short serverPort = 0;              // default: chosen by O/S
    const char*    mcastIf = "0.0.0.0";         // default multicast interface
    int            status = mls_decodeOptions(argc, argv, &feed, &serverIface,
            &serverPort, ttl, &mcastIf, retxTimeout);
    extern int     optind;

    if (0 == status) {
        ServiceAddr* groupAddr;

        argc -= optind;
        argv += optind;
        status = mls_decodeOperands(argc, argv, &groupAddr, fmtpSubnet);

        if (0 == status) {
            status = mls_setMcastGroupInfo(serverIface, serverPort, feed,
                    groupAddr, mcastInfo);
            sa_free(groupAddr);

            if (0 == status)
                *ifaceAddr = mcastIf;
        }
    } // options decoded

    return status;
}

/**
 * Handles a signal by rotating the logging level.
 *
 * @param[in] sig  Signal to be handled. Ignored.
 */
static void
mls_rotateLoggingLevel(
        const int sig)
{
    log_roll_level();
}

/**
 * Handles a signal by setting the `done` flag.
 *
 * @param[in] sig  Signal to be handled.
 */
static void
mls_setDoneFlag(
        const int sig)
{
    if (sig == SIGTERM) {
        log_notice_q("SIGTERM");
    }
    else if (sig == SIGINT) {
        log_notice_q("SIGINT");
    }
    else {
        log_notice_q("Signal %d", sig);
    }
    done = 1;
}

/**
 * Sets signal handling.
 */
static void
mls_setSignalHandling(void)
{
    /*
     * Initialize signal-set for termination signals.
     */
    (void)sigemptyset(&termSigSet);
    for (int i = 0; i < NELT(termSigs); i++)
        (void)sigaddset(&termSigSet, termSigs[i]);

    /*
     * Establish signal handlers.
     */
    struct sigaction sigact;

    (void)sigemptyset(&sigact.sa_mask);
    sigact.sa_flags = 0;

    /*
     * Register termination signal-handler.
     */
    sigact.sa_handler = mls_setDoneFlag;
    (void)sigaction(SIGINT, &sigact, NULL);
    (void)sigaction(SIGTERM, &sigact, NULL);

    /*
     * Register logging-level signal-handler. Ensure that it only affects
     * logging by restarting any interrupted system call.
     */
    sigact.sa_flags |= SA_RESTART;
    sigact.sa_handler = mls_rotateLoggingLevel;
    (void)sigaction(SIGUSR2, &sigact, NULL);
}

/**
 * Returns the dotted-decimal IPv4 address of an Internet identifier.
 *
 * @param[in]  inetId       The Internet identifier. May be a name or an IPv4
 *                          address in dotted-decimal form.
 * @param[in]  desc         The description of the entity associated with the
 *                          identifier appropriate for the phrase "Couldn't get
 *                          address of ...".
 * @param[out] buf          The dotted-decimal IPv4 address corresponding to the
 *                          identifier. It's the caller's responsibility to
 *                          ensure that the buffer can hold at least
 *                          INET_ADDRSTRLEN bytes.
 * @retval     0            Success. `buf` is set.
 * @retval     LDM7_INVAL   The identifier cannot be converted to an IPv4
 *                          address because it's invalid or unknown.
 *                          `log_add()` called.
 * @retval     LDM7_SYSTEM  The identifier cannot be converted to an IPv4
 *                          address due to a system error. `log_add()` called.
 */
static Ldm7Status
mls_getIpv4Addr(
        const char* const restrict inetId,
        const char* const restrict desc,
        char* const restrict       buf)
{
    int status = getDottedDecimal(inetId, buf);

    if (status == 0)
        return 0;

    log_add("Couldn't get address of %s", desc);

    return (status == EINVAL || status == ENOENT)
            ? LDM7_INVAL
            : LDM7_SYSTEM;
}

/**
 * Opens the product-index map for updating. Creates the associated file if it
 * doesn't exist. The parent directory of the associated file is the parent
 * directory of the LDM product-queue.
 *
 * @param[in] feed         Feedtype to be multicast.
 * @param[in] maxSigs      Maximum number of signatures that the product-index
 *                         must contain.
 * @retval    0            Success.
 * @retval    LDM7_INVAL   Maximum number of signatures isn't positive.
 *                         `log_add()` called. The file wasn't opened or
 *                         created.
 * @retval    LDM7_SYSTEM  System error. `log_add()` called.
 */
static Ldm7Status
mls_openProdIndexMap(
        const feedtypet feed,
        const size_t    maxSigs)
{
    char pathname[_XOPEN_PATH_MAX];

    (void)strncpy(pathname, getQueuePath(), sizeof(pathname));
    pathname[sizeof(pathname)-1] = 0;

    return pim_openForWriting(dirname(pathname), feed, maxSigs);
}

/**
 * Accepts notification that the FMTP layer is finished with a data-product
 * and releases associated resources.
 *
 * @param[in] prodIndex  Index of the product.
 */
static void
mls_doneWithProduct(
    const FmtpProdIndex prodIndex)
{
    off_t offset;
    int   status = om_get(offMap, prodIndex, &offset);
    if (status) {
        log_error_q("Couldn't get file-offset corresponding to product-index %lu",
                (unsigned long)prodIndex);
    }
    else {
        status = pq_release(pq, offset);
        if (status) {
            log_error_q("Couldn't release data-product in product-queue "
                    "corresponding to file-offset %ld, product-index %lu",
                    (long)offset, (unsigned long)prodIndex);
        }
    }
}

/**
 * Initializes the resources of this module. Sets `mcastInfo`; in particular,
 * sets `mcastInfo.server.port` to the actual port number used by the FMTP
 * TCP server (in case the number was chosen by the operating-system). Upon
 * return, all FMTP threads have been created -- in particular,  the FMTP TCP
 * server is listening.
 *
 * @param[in] info           Information on the multicast group.
 * @param[in] ttl            Time-to-live of outgoing packets.
 *                                0  Restricted to same host. Won't be output
 *                                   by any interface.
 *                                1  Restricted to the same subnet. Won't be
 *                                   forwarded by a router (default).
 *                              <32  Restricted to the same site, organization
 *                                   or department.
 *                              <64  Restricted to the same region.
 *                             <128  Restricted to the same continent.
 *                             <255  Unrestricted in scope. Global.
 * @param[in] mcastIface     IP address of the interface to use to send
 *                           multicast packets. "0.0.0.0" obtains the default
 *                           multicast interface. Caller may free.
 * @param[in] retxTimeout    FMTP retransmission timeout in minutes. Duration
 *                           that a product will be held by the FMTP layer
 *                           before being released. If negative, then the
 *                           default timeout is used.
 * @param[in] pqPathname     Pathname of product queue from which to obtain
 *                           data-products.
 * @param[in] authorizer     Authorizer of remote clients
 * @retval    0              Success. `*sender` is set.
 * @retval    LDM7_INVAL     An Internet identifier couldn't be converted to an
 *                           IPv4 address because it's invalid or unknown.
 *                           `log_add()` called.
 * @retval    LDM7_MCAST     Failure in FMTP system. `log_add()` called.
 * @retval    LDM7_SYSTEM    System error. `log_add()` called.
 */
static Ldm7Status
mls_init(
    const McastInfo* const restrict info,
    const unsigned                  ttl,
    const char*                     mcastIface,
    const float                     retxTimeout,
    const char* const restrict      pqPathname,
    void*                           authorizer)
{
    char serverInetAddr[INET_ADDRSTRLEN];
    int  status = mls_getIpv4Addr(info->server.inetId, "server",
            serverInetAddr);

    if (status)
        goto return_status;

    char groupInetAddr[INET_ADDRSTRLEN];
    if ((status = mls_getIpv4Addr(info->group.inetId, "multicast-group",
            groupInetAddr)))
        goto return_status;

    offMap = om_new();
    if (offMap == NULL) {
        log_add("Couldn't create prodIndex-to-prodQueueOffset map");
        status = LDM7_SYSTEM;
        goto return_status;
    }

    /*
     * Product-queue is opened thread-safe because `mls_tryMulticast()` and
     * `mls_doneWithProduct()` might be executed on different threads.
     */
    if (pq_open(pqPathname, PQ_READONLY | PQ_THREADSAFE, &pq)) {
        log_add("Couldn't open product-queue \"%s\"", pqPathname);
        status = LDM7_SYSTEM;
        goto free_offMap;
    }

    if ((status = mls_openProdIndexMap(info->feed, pq_getSlotCount(pq))))
        goto close_pq;

    FmtpProdIndex iProd;
    if ((status = pim_getNextProdIndex(&iProd)))
        goto close_prod_index_map;

    if (mi_copy(&mcastInfo, info)) {
        status = LDM7_SYSTEM;
        goto close_prod_index_map;
    }

    if ((status = fmtpSender_create(&fmtpSender, serverInetAddr,
            &mcastInfo.server.port, groupInetAddr, mcastInfo.group.port,
            mcastIface, ttl, iProd, retxTimeout, mls_doneWithProduct,
            authorizer))) {
        log_add("Couldn't create FMTP sender");
        status = (status == 1)
                ? LDM7_INVAL
                : (status == 2)
                  ? LDM7_MCAST
                  : LDM7_SYSTEM;
        goto free_mcastInfo;
    }

    done = 0;

    return 0;

free_mcastInfo:
    xdr_free(xdr_McastInfo, (char*)&mcastInfo);
close_prod_index_map:
    (void)pim_close();
close_pq:
    (void)pq_close(pq);
free_offMap:
    om_free(offMap);
return_status:
    return status;
}

/**
 * Destroys the multicast LDM sender by stopping it and releasing its resources.
 *
 * @retval 0            Success.
 * @retval LDM7_MCAST   Multicast system failure. `log_add()` called.
 * @retval LDM7_SYSTEM  System failure. `log_add()` called.
 */
static inline int       // inlined because small and only called in one place
mls_destroy(void)
{
    int status = fmtpSender_terminate(fmtpSender);

    (void)xdr_free(xdr_McastInfo, (char*)&mcastInfo);
    (void)pim_close();
    (void)pq_close(pq);

    return (status == 0)
            ? 0
            : (status == 2)
              ? LDM7_MCAST
              : LDM7_SYSTEM;
}

/**
 * Multicasts a single data-product. Called by `pq_sequenceLock()`.
 *
 * @param[in] info         Pointer to the data-product's metadata.
 * @param[in] data         Pointer to the data-product's data.
 * @param[in] xprod        Pointer to an XDR-encoded version of the data-product
 *                         (data and metadata).
 * @param[in] size         Size, in bytes, of the XDR-encoded version.
 * @param[in] arg          Pointer to the `off_t` product-queue offset for the
 *                         data-product.
 * @retval    0            Success.
 * @retval    LDM7_MCAST   Multicast layer error. `log_add()` called.
 * @retval    LDM7_SYSTEM  System error. `log_add()` called.
 */
static int
mls_multicastProduct(
        const prod_info* const restrict info,
        const void* const restrict      data,
        void* const restrict            xprod,
        const size_t                    size,
        void* const restrict            arg)
{
    off_t          offset = *(off_t*)arg;
    FmtpProdIndex iProd = fmtpSender_getNextProdIndex(fmtpSender);
    int            status = om_put(offMap, iProd, offset);
    if (status) {
        log_add("Couldn't add product %lu, offset %lu to map",
                (unsigned long)iProd, (unsigned long)offset);
    }
    else {
        /*
         * The signature is added to the product-index map before the product is
         * sent so that it can be found if the receiving LDM-7 immediately
         * requests it.
         */
        status = pim_put(iProd, (const signaturet*)&info->signature);
        if (status) {
            char buf[LDM_INFO_MAX];
            log_add("Couldn't add to product-index map: prodIndex=%lu, "
                    "prodInfo=%s", (unsigned long)iProd,
                    s_prod_info(buf, sizeof(buf), info, 1));
        }
        else {
            status = fmtpSender_send(fmtpSender, xprod, size,
                    (void*)info->signature, sizeof(signaturet), &iProd);

            if (status) {
                off_t off;
                (void)om_get(offMap, iProd, &off);
                status = LDM7_MCAST;
            }
            else {
                if (log_is_enabled_info) {
                    char buf[LDM_INFO_MAX];
                    log_info_q("Sent: prodIndex=%lu, prodInfo=\"%s\"",
                            (unsigned long)iProd,
                            s_prod_info(buf, sizeof(buf), info, 1));
                }
            }
        }
    }

    return status;
}

/**
 * Returns a new product-class for a multicast LDM sender for selecting
 * data-products from the sender's associated product-queue.
 *
 * @param[out] prodClass    Product-class for selecting data-products. Caller
 *                          should call `free_prod_class(*prodClass)` when it's
 *                          no longer needed.
 * @retval     0            Success. `*prodClass` is set.
 * @retval     LDM7_INVAL   Invalid parameter. `log_add()` called.
 * @retval     LDM7_SYSTEM  System error. `log_add()` called.
 */
static int
mls_setProdClass(
        prod_class** const restrict prodClass)
{
    int         status;
    /* PQ_CLASS_ALL has feedtype=ANY, pattern=".*", from=BOT, to=EOT */
    prod_class* pc = dup_prod_class(PQ_CLASS_ALL);      // compiles ERE

    if (pc == NULL) {
        status = LDM7_SYSTEM;
    }
    else {
        (void)set_timestamp(&pc->from); // send products starting now
        pc->psa.psa_val->feedtype = mcastInfo.feed;
        *prodClass = pc;
        status = 0;
    } // `pc` allocated

    return status;
}

/**
 * Tries to multicast the next data-product from a multicast LDM sender's
 * product-queue. Will block for 30 seconds or until a SIGCONT is received if
 * the next data-product doesn't exist.
 *
 * @param[in] prodClass    Class of data-products to multicast.
 * @retval    0            Success.
 * @retval    LDM7_MCAST   Multicast layer error. `log_add()` called.
 * @retval    LDM7_PQ      Product-queue error. `log_add()` called.
 * @retval    LDM7_SYSTEM  System error. `log_add()` called.
 */
static int
mls_tryMulticast(
        prod_class* const restrict prodClass)
{
    // TODO: Keep product locked until FMTP notification, then release

    off_t offset;
    int   status = pq_sequenceLock(pq, TV_GT, prodClass, mls_multicastProduct,
            &offset, &offset);

    if (PQUEUE_END == status) {
        /* No matching data-product. */
        /*
         * The following code ensures that a termination signal isn't delivered
         * between the time that the done flag is checked and the thread is
         * suspended.
         */
        blockTermSigs();

        if (!done) {
            /*
             * Block until a signal handler is called or the timeout occurs. NB:
             * `pq_suspend()` unblocks SIGCONT and SIGALRM.
             *
             * Keep timeout duration consistent with function description.
             */
            (void)pq_suspendAndUnblock(30, termSigs, NELT(termSigs));
        }

        status = 0;           // no problems here
        unblockTermSigs();
    }
    else if (status < 0) {
        log_errno_q(status, "Error in product-queue");
        status = LDM7_PQ;
    }

    return status;
}

/**
 * Blocks signals used by the product-queue for the current thread.
 */
static inline void      // inlined because small and only called in one place
mls_blockPqSignals(void)
{
    static sigset_t pqSigSet;

    (void)sigemptyset(&pqSigSet);
    (void)sigaddset(&pqSigSet, SIGCONT);
    (void)sigaddset(&pqSigSet, SIGALRM);

    (void)pthread_sigmask(SIG_BLOCK, &pqSigSet, NULL);
}

/**
 * Starts multicasting data-products.
 *
 * @pre                    `mls_init()` was called.
 * @retval    0            Success.
 * @retval    LDM7_PQ      Product-queue error. `log_add()` called.
 * @retval    LDM7_MCAST   Multicast layer error. `log_add()` called.
 * @retval    LDM7_SYSTEM  System error. `log_add()` called.
 */
static Ldm7Status
mls_startMulticasting(void)
{
    prod_class* prodClass;
    int         status = mls_setProdClass(&prodClass);

    if (status == 0) {
        pq_cset(pq, &prodClass->from);  // sets product-queue cursor

        /*
         * The `done` flag is checked before `mls_tryMulticast()` is called
         * because that function is potentially lengthy and a SIGTERM might
         * have already been received.
         */
        while (0 == status && !done)
            status = mls_tryMulticast(prodClass);

        free_prod_class(prodClass);
    } // `prodClass` allocated

    return status;
}

static void*
runMldmSrvr(void* mldmSrvr)
{
    if (mldmSrvr_run(mldmSrvr))
        log_error_q("Multicast LDM RPC server returned");
    return NULL;
}

static Ldm7Status
startAuthorization(const CidrAddr* const fmtpSubnet)
{
    Ldm7Status status;
    inAddrPool = inAddrPool_new(fmtpSubnet);
    if (inAddrPool == NULL) {
        log_add_syserr("Couldn't create pool of available IP addresses");
        status = LDM7_SYSTEM;
    }
    else {
        authorizer = auth_new(inAddrPool);
        if (authorizer == NULL) {
            log_add_syserr("Couldn't create authorizer of remote clients");
        }
        else {
            mldmCmdSrvr = mldmSrvr_new(inAddrPool);
            if (mldmCmdSrvr == NULL) {
                log_add_syserr("Couldn't create multicast LDM RPC "
                        "command-server");
                status = LDM7_SYSTEM;
            }
            else {
                status = pthread_create(&mldmSrvrThread, NULL, runMldmSrvr,
                        mldmCmdSrvr);
                if (status) {
                    log_add_syserr("Couldn't create multicast LDM RPC "
                            "command-server thread");
                    mldmSrvr_free(mldmCmdSrvr);
                    mldmCmdSrvr = NULL;
                    status = LDM7_SYSTEM;
                }
                else {
                    mldmSrvrPort = mldmSrvr_getPort(mldmCmdSrvr);
                    status = LDM7_OK;
                }
            } // `mldmSrvr` set
            if (status)
                auth_delete(authorizer);
        } // `authorizer` set
        if (status)
            inAddrPool_delete(inAddrPool);
    } // `inAddrPool` set
    return status;
}

static void
stopAuthorization()
{
    int   status = pthread_cancel(mldmSrvrThread);
    if (status) {
        log_add_syserr("Couldn't cancel multicast LDM RPC command-server "
                "thread");
    }
    else {
        void* result;
        status = pthread_join(mldmSrvrThread, &result);
        if (status) {
            log_add_syserr("Couldn't join multicast LDM RPC command-server "
                    "thread");
        }
        else {
            mldmSrvr_free(mldmCmdSrvr);
            auth_delete(authorizer);
            inAddrPool_delete(inAddrPool);
        }
    }
}

/**
 * Executes a multicast LDM. Blocks until termination is requested or
 * an error occurs.
 *
 * @param[in]  info          Information on the multicast group.
 * @param[out] ttl           Time-to-live of outgoing packets.
 *                                0  Restricted to same host. Won't be output
 *                                   by any interface.
 *                                1  Restricted to the same subnet. Won't be
 *                                   forwarded by a router (default).
 *                              <32  Restricted to the same site, organization
 *                                   or department.
 *                              <64  Restricted to the same region.
 *                             <128  Restricted to the same continent.
 *                             <255  Unrestricted in scope. Global.
 * @param[in]  mcastIface    IP address of the interface to use to send
 *                           multicast packets. "0.0.0.0" obtains the default
 *                           multicast interface. Caller may free.
 * @param[in]  retxTimeout   FMTP retransmission timeout in minutes. Duration
 *                           that a product will be held by the FMTP layer being
 *                           released. If negative, then the default timeout is
 *                           used.
 * @param[in]  timeoutFactor Ratio of the duration that a data-product will
 *                           be held by the FMTP layer before being released
 *                           after being multicast to the duration to
 *                           multicast the product. If negative, then the
 *                           default timeout factor is used.
 * @param[in]  pqPathname    Pathname of the product-queue.
 * @param[in]  fmtpSubnet    Subnet for client FMTP TCP connections
 * @retval     0             Success. Termination was requested.
 * @retval     LDM7_INVAL.   Invalid argument. `log_add()` called.
 * @retval     LDM7_MCAST    Multicast sender failure. `log_add()` called.
 * @retval     LDM7_PQ       Product-queue error. `log_add()` called.
 * @retval     LDM7_SYSTEM   System failure. `log_add()` called.
 */
static Ldm7Status
mls_execute(
        const McastInfo* const restrict info,
        const unsigned                  ttl,
        const char* const restrict      mcastIface,
        const float                     retxTimeout,
        const char* const restrict      pqPathname,
        const CidrAddr* const restrict  fmtpSubnet)
{
    int status;
    /*
     * Block signals used by `pq_sequence()` so that they will only be
     * received by a thread that's accessing the product queue. (The product-
     * queue ensures signal reception when necessary.)
     */
    mls_blockPqSignals();
    /*
     * Prevent child threads from receiving a term signal because this thread
     * manages the child threads.
     */
    blockTermSigs();
    /*
     * Sets `inAddrPool, `authorizer`, `mldmSrvr`, `mldmSrvrThread`, and
     * `mldmSrvrPort`.
     */
    status = startAuthorization(fmtpSubnet);
    if (status) {
        log_add("Couldn't initialize authorization of remote clients");
    }
    else {
        // Sets `mcastInfo`
        status = mls_init(info, ttl, mcastIface, retxTimeout, pqPathname,
                authorizer);
        unblockTermSigs(); // Done creating child threads
        if (status) {
            log_add("Couldn't initialize multicast LDM sender");
        }
        else {
            /*
             * Print, to the standard output stream,
             * - The port number of the FMTP TCP server in case it wasn't
             *   specified by the user and was, instead, chosen by the
             *   operating system; and
             * - The port number of the multicast LDM RPC command-server so that
             *   upstream LDM processes can communicate with it to, for
             *   example, reserve IP addresses for remote FMTP clients.
             */
            if (printf("%hu %hu\n", mcastInfo.server.port,
                    mldmSrvr_getPort(mldmCmdSrvr)) < 0) {
                log_add_syserr("Couldn't write port numbers to standard output");
                status = LDM7_SYSTEM;
            }
            else {
                status = fflush(stdout);
                log_assert(status != EOF);
                /*
                 * Data-products are multicast on the current (main) thread so
                 * that the process will automatically terminate if something
                 * goes wrong.
                 */
                char* miStr = mi_format(&mcastInfo);
                char* fmtpSubnetStr = cidrAddr_format(fmtpSubnet);
                log_notice_q("Multicast LDM sender starting up: mcastIface=%s, "
                        "mcastInfo=%s, ttl=%u, " "fmtpSubnet=%s, "
                        "pq=\"%s\", mldmCmdPort=%hu", mcastIface, miStr, ttl,
                        fmtpSubnetStr, pqPathname,
                        mldmSrvr_getPort(mldmCmdSrvr));
                free(fmtpSubnetStr);
                free(miStr);
                status = mls_startMulticasting();

                int msStatus = mls_destroy();
                if (status == 0)
                    status = msStatus;
            } // Port numbers successfully written to standard output stream
        } // Multicast LDM sender initialized
        stopAuthorization();
    } // Multicast LDM RPC server started
    return status;
}

/**
 * Multicasts data-products to a multicast group.
 *
 * @param[in] argc  Number of arguments.
 * @param[in] argv  Arguments. See [mls_usage()](@ref mls_usage)
 * @retval    0     Success.
 * @retval    1     Invalid command line. ERROR-level message logged.
 * @retval    2     System error. ERROR-level message logged.
 * @retval    3     Product-queue error. ERROR-level message logged.
 * @retval    4     Multicast layer error. ERROR-level message logged.
 */
int
main(   const int    argc,
        char** const argv)
{
    /*
     * Initialize logging. Done first in case something happens that needs to
     * be reported.
     */
    log_init(argv[0]);

    /*
     * Decode the command-line.
     */
    McastInfo*  groupInfo;         // multicast group information
    unsigned    ttl = 1;           // Won't be forwarded by any router.
    const char* mcastIface;        // IP address of multicast interface
    // Ratio of product-hold duration to multicast duration
    float       retxTimeout = -1;  // Use default
    CidrAddr*   fmtpSubnet;        // FMTP client subnet
    int         status = mls_decodeCommandLine(argc, argv, &groupInfo, &ttl,
            &mcastIface, &retxTimeout, &fmtpSubnet);

    if (status) {
        log_add("Couldn't decode command-line");
        if (1 == status)
            mls_usage();
        log_flush_error();
    }
    else {
        mls_setSignalHandling();

        status = mls_execute(groupInfo, ttl, mcastIface, retxTimeout,
                getQueuePath(), fmtpSubnet);
        if (status) {
            log_error_q("Couldn't execute multicast LDM sender");
            switch (status) {
                case LDM7_INVAL: status = 1; break;
                case LDM7_PQ:    status = 3; break;
                case LDM7_MCAST: status = 4; break;
                default:         status = 2; break;
            }
        }

        cidrAddr_delete(fmtpSubnet);
        mi_free(groupInfo);
        log_notice_q("Terminating");
    } // `groupInfo` allocated

    log_fini();

    return status;
}
