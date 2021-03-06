/**
 * Copyright 2018 University Corporation for Atmospheric Research. All rights
 * reserved. See the the file COPYRIGHT in the top-level source-directory for
 * licensing conditions.
 *
 *   @file: UpMcastMgr.h
 * @author: Steven R. Emmerson
 *
 * This file declares the manager for multicasting from the upstream site.
 */

#ifndef UPMCASTMGR_H
#define UPMCASTMGR_H

#include "CidrAddr.h"
#include "fmtp.h"
#include "ldm.h"
#include "pq.h"
#include "VirtualCircuit.h"

#include <sys/types.h>

typedef struct mul               Mul;

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Sets the FMTP retransmission timeout. Calls to `umm_subscribe()` will use
 * this value when creating a new upstream multicast LDM sender.
 * @param[in] minutes  FMTP retransmission timeout. A negative value obtains the
 *                     FMTP default.
 * @see                `umm_subscribe()`
 */
void
umm_setRetxTimeout(const float minutes);

/**
 * Adds a potential multicast LDM sender. The sender is not started. This
 * function should be called for all potential senders before any child
 * process is forked so that all child processes will have this information.
 *
 * @param[in] mcastIface   IPv4 address of interface to use for multicasting.
 *                         "0.0.0.0" obtains the system's default multicast
 *                         interface.
 * @param[in] info         Information on the multicast group. The port number
 *                         of the FMTP TCP server is ignored (it will be chosen
 *                         by the operating sytem). Caller may free.
 * @param[in] ttl          Time-to-live for multicast packets:
 *                                0  Restricted to same host. Won't be output by
 *                                   any interface.
 *                                1  Restricted to same subnet. Won't be
 *                                   forwarded by a router.
 *                              <32  Restricted to same site, organization or
 *                                   department.
 *                              <64  Restricted to same region.
 *                             <128  Restricted to same continent.
 *                             <255  Unrestricted in scope. Global.
 * @param[in] vcEnd        Local virtual-circuit endpoint. Caller may free.
 * @param[in] fmtpSubnet   Subnet for client FMTP TCP connections
 * @param[in] pqPathname   Pathname of product-queue. Caller may free.
 * @retval    0            Success.
 * @retval    LDM7_INVAL   Invalid argument. `log_add()` called.
 * @retval    LDM7_DUP     Multicast group information conflicts with earlier
 *                         addition. Manager not modified. `log_add()` called.
 * @retval    LDM7_SYSTEM  System failure. `log_add()` called.
 */
Ldm7Status
umm_addPotentialSender(
    const struct in_addr              mcastIface,
    const McastInfo* const restrict   info,
    const unsigned short              ttl,
    const VcEndPoint* const restrict  vcEnd,
    const CidrAddr* const restrict    fmtpSubnet,
    const char* const restrict        pqPathname);

/**
 * Returns the response to a multicast subscription request. Doesn't block.
 *
 * @param[in]  feedtype     Multicast group feed-type.
 * @param[out] reply        Reply to the subscription-request. Call should
 *                          destroy when it's no longer needed.
 * @retval     0            Success. The group is being multicast and
 *                          `*reply` is set.
 * @retval     LDM7_LOGIC   Logic error. `log_add()` called.
 * @retval     LDM7_NOENT   No corresponding potential sender was added via
 *                          `mlsm_addPotentialSender()`. `log_add() called`.
 * @retval     LDM7_SYSTEM  System error. `log_add()` called.
 */
Ldm7Status
umm_subscribe(
        const feedtypet          feedtype,
        SubscriptionReply* const reply);

/**
 * Handles the termination of a multicast LDM sender process. This function
 * should be called by the top-level LDM server when it notices that a child
 * process has terminated.
 *
 * @param[in] pid          Process-ID of the terminated multicast LDM sender
 *                         process.
 * @retval    0            Success.
 * @retval    LDM7_NOENT   PID doesn't correspond to known process.
 * @retval    LDM7_SYSTEM  System error. `log_add()` called.
 */
Ldm7Status
umm_terminated(const pid_t pid);

/**
 * Returns the process identifier of the associated multicast LDM sender.
 * @retval 0      Multicast LDM sender doesn't exist
 * @return        PID of multicast LDM sender
 * @threadsafety  Safe
 */
pid_t
umm_getMldmSenderPid(void);

/**
 * Releases the IP address reserved for the FMTP TCP connection in a downstream
 * LDM7.
 * @param[in] feed          LDM feed associated with `downFmtpAddr`
 * @param[in] downFmtpAddr  Address of TCP connection in downstream FMTP layer
 * @retval    LDM7_NOENT    No address pool corresponding to `feed`
 * @retval    LDM7_NOENT    `downFmtpAddr` wasn't reserved
 */
Ldm7Status umm_unsubscribe(
        const feedtypet feed,
        const in_addr_t downFmtpAddr);

/**
 * Clears all entries.
 *
 * @retval    0            Success.
 * @retval    LDM7_SYSTEM  System error. `log_add()` called.
 */
Ldm7Status
umm_clear(void);

#ifdef __cplusplus
}
#endif

#endif
