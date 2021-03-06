/**
 * This file implements the upstream LDM-7. The upstream LDM-7
 *     - Is a child-process of the top-level LDM server;
 *     - Ensures that a multicast LDM sender processes is running for its
 *       associated multicast group;
 *     - Handles one and only one downstream LDM-7;
 *     - Runs a server on its TCP connection that accepts requests for files
 *       missed by the multicast component of its downstream LDM-7; and
 *     - Sends such files to its downstream LDM-7.
 *
 * NB: Using a single TCP connection and having both client-side and server-side
 * transports on both the upstream and downstream LDM-7s only works because,
 * after the initial subscription, all exchanges are asynchronous; consequently,
 * the servers don't interfere with the (non-existent) RPC replies.
 *
 * Copyright 2018 University Corporation for Atmospheric Research. All rights
 * reserved. See the the file COPYRIGHT in the top-level source-directory for
 * licensing conditions.
 *
 *   @file: up7.c
 * @author: Steven R. Emmerson
 */

#include "config.h"

#include "ChildCommand.h"
#include "CidrAddr.h"
#include "globals.h"
#include "inetutil.h"
#include "ldm_config_file.h"
#include "ldm.h"
#include "ldmprint.h"
#include "log.h"
#include "mcast_info.h"
#include "pq.h"
#include "priv.h"
#include "prod_class.h"
#include "prod_index_map.h"
#include "prod_info.h"
#include "rpcutil.h"
#include "timestamp.h"
#include "up7.h"
#include "UpMcastMgr.h"

#include <arpa/inet.h>
#include <errno.h>
#include <libgen.h>
#include <limits.h>
#include <netdb.h>
#include <netinet/in.h>
#include <rpc.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#ifndef _XOPEN_PATH_MAX
/* For some reason, the following isn't defined by gcc(1) 4.8.3 on Fedora 19 */
#   define _XOPEN_PATH_MAX 1024 // value mandated by XPG6; includes NUL
#endif

/******************************************************************************
 * OESS-based submodule for creating an AL2S virtual circuit
 ******************************************************************************/

static const char    python[] = "python"; ///< Name of python executable

/**
 * Creates an AL2S virtual circuit between two end-points.
 *
 * @param[in]  wrkGrpName   Name of the AL2S workgroup
 * @param[in]  desc         Description of virtual circuit
 * @param[in]  end1         One end of the virtual circuit. Switch or port ID
 *                          may start with "dummy", in which case the circuit
 *                          will not be created.
 * @param[in]  end2         Other end of the virtual circuit. Switch or port ID
 *                          may start with "dummy", in which case the circuit
 *                          will not be created.
 * @param[out] circuitId    Identifier of created virtual-circuit. Caller should
 *                          call `free(*circuitId)` when the identifier is no
 *                          longer needed. Will start with "dummy" if a switch
 *                          or port identifier of `end1` or `end2` starts with
 *                          "dummy".
 * @retval     0            Success or a switch or port identifier of `end1` or
 *                          `end2` starts with "dummy"
 * @retval     LDM7_INVAL   Invalid argument. `log_add()` called.
 * @retval     LDM7_SYSTEM  Failure. `log_add()` called.
 */
static int oess_provision(
        const char* const restrict       wrkGrpName,
        const char* const restrict       desc,
        const VcEndPoint* const restrict end1,
        const VcEndPoint* const restrict end2,
        char** const restrict            circuitId)
{
    int  status;

    if ((end1 && (strncmp(end1->switchId, "dummy", 5) == 0 ||
                  strncmp(end1->portId, "dummy", 5) == 0)) ||
        (end2 && (strncmp(end2->switchId, "dummy", 5) == 0 ||
                  strncmp(end2->portId, "dummy", 5) == 0))) {
        log_notice("Ignoring call to create a dummy AL2S virtual-circuit");
        *circuitId = strdup("dummy_circuitId");
        status = 0;
    }
    else if (wrkGrpName == NULL || desc == NULL || end1 == NULL ||
            end2 == NULL || circuitId == NULL) {
        char* end1Id = end1 ? vcEndPoint_format(end1) : NULL;
        char* end2Id = end2 ? vcEndPoint_format(end2) : NULL;
        log_add("NULL argument: wrkGrpName=%s, desc=%s, end1=%s, end2=%s,"
                "circuitId=%p", wrkGrpName, desc, end1Id, end2Id, circuitId);
        free(end1Id);
        free(end2Id);
        status = LDM7_INVAL;
    }
    else {
        char vlanId1[12]; // More than sufficient for 12-bit VLAN ID
        char vlanId2[12];

        (void)snprintf(vlanId1, sizeof(vlanId1), "%hu", end1->vlanId);
        (void)snprintf(vlanId2, sizeof(vlanId2), "%hu", end2->vlanId);

        const char* const cmdVec[] = {python, "provision.py", wrkGrpName,
                end1->switchId, end1->portId, vlanId1,
                end2->switchId, end2->portId, vlanId2, NULL};

        rootpriv();
            ChildCmd* cmd = childCmd_execvp(cmdVec[0], cmdVec);
        unpriv();

        if (cmd == NULL) {
            status = LDM7_SYSTEM;
        }
        else {
            char*   line = NULL;
            size_t  size = 0;
            ssize_t nbytes = childCmd_getline(cmd, &line, &size);
            int     circuitIdStatus;

            if (nbytes <= 0) {
                log_add("Couldn't get AL2S virtual-circuit ID");

                circuitIdStatus = LDM7_SYSTEM;
            }
            else {
                circuitIdStatus = 0;

                if (line[nbytes-1] == '\n')
                    line[nbytes-1] = 0;
            }

            int childExitStatus;

            status = childCmd_reap(cmd, &childExitStatus);

            if (status) {
                status = LDM7_SYSTEM;
            }
            else {
                if (childExitStatus) {
                    log_add("OESS provisioning process terminated with status "
                            "%d", childExitStatus);

                    status = LDM7_SYSTEM;
                }
                else {
                    if (circuitIdStatus) {
                        status = circuitIdStatus;
                    }
                    else {
                        *circuitId = line;
                    }
                } // Child process terminated unsuccessfully
            } // Child-command was reaped
        } // Couldn't execute child-command

        if (status)
            log_add("Couldn't create AL2S virtual-circuit");
    } // Valid arguments and actual provisioning

    return status;
}

/**
 * Destroys an Al2S virtual circuit.
 *
 * @param[in] wrkGrpName   Name of the AL2S workgroup
 * @param[in] circuitId    Virtual-circuit identifier
 */
static void oess_remove(
        const char* const restrict wrkGrpName,
        const char* const restrict circuitId)
{
    if (strncmp(circuitId, "dummy", 5) == 0) {
        log_notice("Ignoring call to remove a dummy AL2S virtual-circuit");
    }
    else {
        int               status;
        const char* const cmdVec[] = {python, "remove.py", wrkGrpName, circuitId,
                NULL};
        ChildCmd*         cmd = childCmd_execvp(cmdVec[0], cmdVec);

        if (cmd == NULL) {
            status = errno;
        }
        else {
            int exitStatus;

            status = childCmd_reap(cmd, &exitStatus);

            if (status == 0 && exitStatus)
                log_add("Child-process terminated with status %d", exitStatus);
        } // Child-command executing

        if (status) {
            log_add_errno(status, "Couldn't destroy AL2S virtual-circuit");
            log_flush_error();
        }
    } // Not a dummy AL2S virtual-circuit
}

/******************************************************************************
 * Upstream LDM7:
 ******************************************************************************/

/**
 * Module is initialized?
 */
static bool        isInitialized = FALSE;
/**
 * Name of AL2S workgroup
 */
static char*       wrkGrpName = NULL;
/**
 * Local AL2S end-point for virtual circuits
 */
static VcEndPoint* localVcEndPoint = NULL;
/**
 * Identifier of AL2S virtual circuit
 */
static char*       circuitId = NULL;
/**
 * The RPC client-side transport to the downstream LDM-7
 */
static CLIENT*     clnt = NULL;
/**
 * The feedtype of the subscription.
 */
static feedtypet   feedtype = NONE;
/**
 * The IP address of the downstream FMTP layer's TCP connection.
 */
static in_addr_t   downFmtpAddr = INADDR_ANY;
/**
 * Whether or not the product-index map is open.
 */
static bool        pimIsOpen = false;
/**
 * Whether or not this component is done.
 */
static bool        isDone = false;

/**
 * Idempotent.
 */
static void
releaseDownFmtpAddr()
{
    if (feedtype != NONE && downFmtpAddr != INADDR_ANY) {
        if (umm_unsubscribe(feedtype, downFmtpAddr)) {
            log_flush_error();
        }
        else {
            char ipStr[INET_ADDRSTRLEN];
            log_debug("Address %s released", inet_ntop(AF_INET, &downFmtpAddr,
                    ipStr, sizeof(ipStr)));
        }
        downFmtpAddr = INADDR_ANY;
        feedtype = NONE;
    }
}

/**
 * Creates an AL2S virtual-circuit between two end-points for a given LDM feed.
 *
 * @param[in]  feed              LDM feed
 * @param[in]  remoteVcEndPoint  Remote end of virtual circuit
 * @retval     0                 Success
 * @retval     LDM7_SYSTEM       Failure. `log_add()` called.
 */
static Ldm7Status
up7_createVirtCirc(
        const feedtypet                  feed,
        const VcEndPoint* const restrict remoteVcEndPoint)
{
    int  status;
    char feedStr[128];

    (void)ft_format(feed, feedStr, sizeof(feedStr));
    feedStr[sizeof(feedStr)-1] = 0;

    char* const desc = ldm_format(128, "%s feed", feedStr);

    if (desc == NULL) {
        log_add("Couldn't format description for AL2S virtual-circuit for "
                "feed %s", feedStr);
        status = LDM7_SYSTEM;
    }
    else {
        char* id;

        status = oess_provision(wrkGrpName, desc, localVcEndPoint,
                remoteVcEndPoint, &id);

        if (status) {
            log_add("Couldn't create AL2S virtual circuit for feed %s",
                    feedStr);
        }
        else {
            free(circuitId);
            circuitId = id;
        }

        free(desc);
    } // `desc` allocated

    return status;
}

/**
 * Destroys the virtual circuit if it exists.
 */
static void
up7_destroyVirtCirc()
{
    if (circuitId) {
        oess_remove(wrkGrpName, circuitId);
        free(circuitId);
        circuitId = NULL;
    }
}

/**
 * Opens the product-index map associated with a feedtype.
 *
 * @param[in] feed         The feedtype.
 * @retval    0            Success.
 * @retval    LDM7_LOGIC   The product-index map is already open. `log_add()`
 *                         called.
 * @retval    LDM7_SYSTEM  System error. `log_add()` called. The state of the
 *                         associated file is unspecified.
 */
static int
up7_openProdIndexMap(
        const feedtypet   feed)
{
    char pathname[_XOPEN_PATH_MAX];

    (void)strncpy(pathname, getQueuePath(), sizeof(pathname));
    pathname[sizeof(pathname)-1] = 0;

    int status = pim_openForReading(dirname(pathname), feed);

    if (status == 0)
        pimIsOpen = true;

    return status;
}

/**
 * Closes the open product-index map. Registered by `atexit()`. Idempotent.
 */
static void
up7_closeProdIndexMap()
{
    if (pimIsOpen) {
        if (pim_close()) {
            char feedStr[256];
            int  nbytes = ft_format(feedtype, feedStr, sizeof(feedStr));
            if (nbytes == -1 || nbytes >= sizeof(feedStr)) {
                log_error_q("Couldn't close product-index map for feed %#lx",
                        (unsigned long)feedStr);
            }
            else {
                log_error_q("Couldn't close product-index map for feed %s", feedStr);
            }
        }
        else {
            pimIsOpen = false;
        }
    }
}

/**
 * Idempotent.
 */
static void up7_destroyClient(void)
{
    if (clnt) {
        clnt_destroy(clnt); // Frees `clnt`. Doesn't check for `NULL`.
        clnt = NULL;
    }
}

/**
 * Indicates if this module should no longer be used unless `up7_reset()` is
 * called.
 * @retval `true`  Iff this module should no longer be used
 */
bool up7_isDone()
{
    return isDone;
}

/**
 * Creates a client-side RPC transport on the TCP connection of a server-side
 * RPC transport.
 *
 * @param[in] xprt      Server-side RPC transport.
 * @retval    true      Success.
 * @retval    false     System error. `log_add()` called.
 */
static bool
up7_createClientTransport(
        struct SVCXPRT* const xprt)
{
    bool success;

    /*
     * Create a client-side RPC transport on the TCP connection.
     */
    log_assert(xprt->xp_raddr.sin_port != 0);
    log_assert(xprt->xp_sock >= 0);

    // `xprt->xp_sock >= 0` => socket won't be closed by client-side error
    // TODO: adjust sending buffer size
    CLIENT* client = clnttcp_create(&xprt->xp_raddr, LDMPROG, SEVEN,
            &xprt->xp_sock, MAX_RPC_BUF_NEEDED, 0);

    if (client == NULL) {
        log_assert(rpc_createerr.cf_stat != RPC_TIMEDOUT);
        log_add("Couldn't create client-side transport to downstream LDM-7 on "
                "%s%s", hostbyaddr(&xprt->xp_raddr), clnt_spcreateerror(""));

        success = false;
    }
    else {
        if (atexit(up7_destroyClient)) {
            log_add_syserr("Couldn't register upstream LDM-7 cleanup function");

            success = false;
        }
        else {
            // `up7_down7_test` calls this function more than once
            up7_destroyClient();
            clnt = client;

            success = true;
        }

        if (!success)
            clnt_destroy(client); // Frees `client`
    } // `client` allocated

    return success;
}

/**
 * Reduces the feed requested by a host according to what it is allowed to
 * receive.
 * @param[in] feed    Feed requested by host
 * @param[in] hostId  Host identifier: either a hostname or an IP address in
 *                    dotted-decimal form
 * @param[in] inAddr  Address of the host
 * @return            Reduced feed. Might be `NONE`.
 */
static feedtypet up7_reduceToAllowed(
        feedtypet                            feed,
        const char* const restrict           hostId,
        const struct in_addr* const restrict inAddr)
{
    static const size_t maxFeeds = 128;
    feedtypet           allowedFeeds[maxFeeds];
    size_t              numFeeds = lcf_getAllowedFeeds(hostId, inAddr, maxFeeds,
            allowedFeeds);
    if (numFeeds > maxFeeds) {
        log_error_q("numFeeds (%u) > maxFeeds (%d)", numFeeds, maxFeeds);
        numFeeds = maxFeeds;
    }
    return lcf_reduceByFeeds(feed, allowedFeeds, numFeeds);
}

/**
 * Ensures that a reply to an RPC service routine has been freed.
 *
 * @param[in] xdrProc  Associated XDR function.
 * @param[in] reply    RPC reply.
 */
static inline void
up7_ensureFree(
        xdrproc_t const      xdrProc,
        void* const restrict reply)
{
    if (reply)
        xdr_free(xdrProc, (char*)reply);
}

/**
 * Sets the subscription of the associated downstream LDM-7. Ensures that the
 * multicast LDM sender process that's associated with the given feedtype is
 * running.
 *
 * @param[in]  request  Subscription request
 * @param[in]  xprt     RPC transport
 * @param[out] reply    Reply.
 * @retval     `true`   Success. `*reply` is set. `feedtype` and
 *                      `downFmtpAddr` are set iff a corresponding multicast
 *                      sender exists.
 * @retval     `false`  Failure. `log_add()` called. Caller should kill the
 *                      connection.
 */
static bool
up7_subscribe(
        const McastSubReq* const restrict request,
        struct SVCXPRT* const restrict    xprt,
        SubscriptionReply* const restrict reply)
{
    bool                replySet = false;
    struct sockaddr_in* sockAddr = svc_getcaller(xprt);
    struct in_addr*     inAddr = &sockAddr->sin_addr;
    const char*         hostId = hostbyaddr(sockAddr);
    feedtypet           reducedFeed = up7_reduceToAllowed(request->feed, hostId,
            inAddr);

    if (reducedFeed == NONE) {
        log_notice("Host %s isn't allowed to receive any part of feed %s",
                hostId, s_feedtypet(request->feed));
        reply->status = LDM7_UNAUTH;
        replySet = true;
    }
    else {
        Ldm7Status status = up7_createVirtCirc(reducedFeed,
                &request->vcEnd);

        if (status != LDM7_OK) {
            log_add("Couldn't create virtual circuit to host %s", hostId);
        }
        else {
            SubscriptionReply rep = {};

            status = umm_subscribe(reducedFeed, &rep);

            if (status) {
                if (LDM7_NOENT == status) {
                    log_notice_q("Allowed feed %s isn't multicasted",
                            s_feedtypet(reducedFeed));
                    reply->status = LDM7_NOENT;
                    replySet = true;
                }
                else {
                    log_add("Couldn't subscribe host %s to feed %s",
                            hostId, s_feedtypet(reducedFeed));
                }
            }
            else {
                in_addr_t addr = cidrAddr_getAddr(
                        &rep.SubscriptionReply_u.info.fmtpAddr);

                status = up7_openProdIndexMap(request->feed);

                if (status) {
                    log_add("Couldn't open product->index map");
                }
                else {
                    feedtype = reducedFeed;
                    downFmtpAddr = addr;
                    *reply = rep;
                    reply->status = LDM7_OK;
                    replySet = true;
                }

                if (status) {
                    (void)umm_unsubscribe(reducedFeed, addr);
                    up7_ensureFree(xdr_SubscriptionReply, &rep);
                }
            } // Multicast LDM sender exists & reply is initialized

            if (status)
                up7_destroyVirtCirc();
        } // Virtual circuit with downstream client created
    } // All or part of subscription is allowed by configuration-file

    return replySet;
}

/**
 * Delivers a data-product to the associated downstream LDM-7. Called by
 * `pq_processProdBySig()`.
 *
 * @param[in] info         Data-product metadata.
 * @param[in] data         Data-product data.
 * @param[in] xprod        XDR-encoded data-product.
 * @param[in] len          Size of XDR-encoded data-product in bytes.
 * @param[in] optArg       Pointer to associated FMTP product-index.
 * @retval    0            Success.
 * @retval    LDM7_SYSTEM  Failure. `log_add()` called.
 */
static int
up7_deliverProduct(
        const prod_info* const restrict info,
        const void* const restrict      data,
	void* const restrict            xprod,
	const size_t                    len,
	void* const restrict            optArg)
{
    int           status;
    MissedProduct missedProd;

    missedProd.iProd = *(FmtpProdIndex*)optArg;
    missedProd.prod.info = *info;
    missedProd.prod.data = (void*)data; // cast away `const`

    log_debug("Delivering: iProd=%lu, ident=\"%s\"", missedProd.iProd,
            info->ident);
    (void)deliver_missed_product_7(&missedProd, clnt);

    /*
     * The status will be RPC_TIMEDOUT unless an error occurs because the RPC
     * call uses asynchronous message-passing.
     */
    if (clnt_stat(clnt) != RPC_TIMEDOUT) {
        log_add("Couldn't RPC to downstream LDM-7: %s", clnt_errmsg(clnt));
        status = LDM7_SYSTEM;
    }
    else {
        log_info_q("Missed product sent: %s",
                s_prod_info(NULL, 0, &missedProd.prod.info,
                log_is_enabled_debug));
        status = 0;
    }

    return status;
}

/**
 * Sends the data-product corresponding to a multicast product-index to the
 * associated downstream LDM-7.
 *
 * @param[in]  iProd        Product-index.
 * @retval     0            Success.
 * @retval     LDM7_NOENT   No corresponding data-product. `log_add()` called.
 * @retval     LDM7_SYSTEM  System failure. `log_add()` called.
 */
static Ldm7Status
up7_sendProduct(
        FmtpProdIndex iProd)
{
    signaturet sig;
    int        status = pim_get(iProd, &sig);

    if (LDM7_NOENT == status) {
        log_add("No signature in product-index map corresponding to index %lu",
                (unsigned long)iProd);
    }
    else if (0 == status) {
        status = pq_processProduct(pq, sig, up7_deliverProduct, &iProd);

        if (PQ_NOTFOUND == status) {
            char buf[sizeof(signaturet)*2+1];

            (void)sprint_signaturet(buf, sizeof(buf), sig);
            log_add("No data-product corresponding to signature %s: "
                    "prodIndex=%lu", buf, (unsigned long)iProd);
            status = LDM7_NOENT;
        }
        else if (status) {
            status = LDM7_SYSTEM;
        }
    }

    return status;
}

/**
 * Finds a data-product corresponding to a product-index. If found, then it
 * is sent to to the downstream LDM-7 via the client-side transport; otherwise,
 * the downstream LDM-7 is notified that no corresponding data-product exists.
 *
 * @param[in] iProd   Product-index.
 * @retval    true    Success. Either the product or a notice of unavailability
 *                    was sent to the client.
 * @retval    false   Failure. `log_add()` called.
 */
static bool
up7_findAndSendProduct(
    FmtpProdIndex iProd)       // not `cont` because of `no_such_product_7()`
{
    int status = up7_sendProduct(iProd);

    if (LDM7_NOENT == status) {
        log_flush_info();
        (void)no_such_product_7(&iProd, clnt);

        if (clnt_stat(clnt) == RPC_TIMEDOUT) {
            status = 0;
        }
        else {
            /*
             * The status will be RPC_TIMEDOUT unless an error occurs because
             * the RPC call uses asynchronous message-passing.
             */
            log_add("Couldn't RPC to downstream LDM-7: %s", clnt_errmsg(clnt));
        }
    }

    return 0 == status;
}

/**
 * Ensures that the global product-queue is closed at process termination.
 * Referenced by `atexit()`.
 */
static void
closePq(void)
{
    if (pq) {
        if (pq_close(pq)) {
            log_error_q("Couldn't close global product-queue");
        }
        pq = NULL;
    }
}

/**
 * Ensures that the product-queue is open for reading.
 *
 * @retval false  Failure.   `log_add()` called.
 * @retval true   Success.
 */
static bool
up7_ensureProductQueueOpen(void)
{
    bool success;

    if (pq) {
        success = true;
    }
    else {
        const char* const pqPath = getQueuePath();
        int               status = pq_open(pqPath, PQ_READONLY, &pq);

        if (status) {
            if (PQ_CORRUPT == status) {
                log_add("The product-queue \"%s\" is corrupt", pqPath);
            }
            else {
                log_add("Couldn't open product-queue \"%s\": %s", pqPath);
            }
            success = false;
        }
        else {
            if (atexit(closePq)) {
                log_add_syserr("Couldn't register product-queue closing function");
                success = false;
            }
            else {
                success = true;
            }
        }
    }

    return success;
}

/**
 * Sets the cursor of the product-queue to just after the data-product with a
 * given signature.
 *
 * @param[in] after        Data-product signature.
 * @retval    0            Success.
 * @retval    LDM7_NOENT   Corresponding data-product not found.
 * @retval    LDM7_SYSTEM  Failure. `log_add()` called.
 */
static Ldm7Status
up7_setCursorFromSignature(
        const signaturet after)
{
    int status;

    switch ((status = pq_setCursorFromSignature(pq, after))) {
    case 0:
        return 0;
    case PQ_NOTFOUND:
        log_info("Data-product with signature %s wasn't found in product-queue",
                s_signaturet(NULL, 0, after));
        return LDM7_NOENT;
    default:
        log_add("Couldn't set product-queue cursor from signature %s: %s",
                s_signaturet(NULL, 0, after), pq_strerror(pq, status));
        return LDM7_SYSTEM;
    }
}

/**
 * Sets the cursor of the product-queue to point a time-offset older than now.
 *
 * @param[in] offset  Time offset in seconds.
 * @retval    true    Success.
 * @retval    false   Failure. `log_add()` called.
 */
static void
up7_setCursorFromTimeOffset(
        const unsigned offset)
{
    timestampt ts;

    (void)set_timestamp(&ts);

    ts.tv_sec = (offset < ts.tv_sec) ? (ts.tv_sec - offset) : 0;

    pq_cset(pq, &ts);
}

/**
 * Sets the cursor of the product-queue from a backlog specification.
 *
 * @param[in] backlog  Backlog specification.
 * @retval    true     Success.
 * @retval    false    Failure. `log_add()` called.
 */
static bool
up7_setProductQueueCursor(
        const BacklogSpec* const restrict backlog)
{
    if (backlog->afterIsSet) {
        switch (up7_setCursorFromSignature(backlog->after)) {
        case 0:
            return true;
        case LDM7_NOENT:
            break;
        default:
            return false;
        }
    }

    up7_setCursorFromTimeOffset(backlog->timeOffset);

    return true;
}

/**
 * Sends a data-product to the downstream LDM-7 if it doesn't have a given
 * signature.
 *
 * Called by `pq_sequence()`.
 *
 * @param[in] info         Data-product's metadata.
 * @param[in] data         Data-product's data.
 * @param[in] xprod        XDR-encoded version of data-product (data and metadata).
 * @param[in] size         Size, in bytes, of XDR-encoded version.
 * @param[in] arg          Signature.
 * @retval    0            Success.
 * @retval    EEXIST       Data-product has given signature. Not sent.
 * @retval    EIO          Couldn't send to downstream LDM-7
 * @return                 `pq_sequence()` return value
 */
static int
up7_sendIfNotSignature(
    const prod_info* const restrict info,
    const void* const restrict      data,
    void* const restrict            xprod,
    const size_t                    size,
    void* const restrict            arg)
{
    int               status;
    const signaturet* sig = (const signaturet*)arg;

    if (0 == memcmp(sig, info->signature, sizeof(signaturet))) {
        status = EEXIST;
    }
    else {
        product prod;
        prod.info = *info;
        prod.data = (void*)data;    // cast away `const`

        deliver_backlog_product_7(&prod, clnt);

        /*
         * The status will be RPC_TIMEDOUT unless an error occurs because the
         * RPC call uses asynchronous message-passing.
         */
        if (clnt_stat(clnt) != RPC_TIMEDOUT) {
            log_add("Couldn't send backlog data-product to downstream LDM-7: "
                    "%s", clnt_errmsg(clnt));
            status = EIO;
        }
        else {
            log_notice_q("Backlog product sent: %s", s_prod_info(NULL, 0, info,
                    log_is_enabled_debug));
            status = 0;
        }
    }

    return status;
}

/**
 * Sends all data-products of the given feedtype in the product-queue from the
 * current cursor position up to (but not including) the data-product with a
 * given signature.
 *
 * @param[in] before       Signature of data-product at which to stop sending.
 * @retval    0            Success.
 * @retval    LDM7_NOENT   Data-product with given signature not found before
 *                         end of queue reached. `log_info_1()` called.
 * @retval    LDM7_SYSTEM  System failure. `log_add()` called.
 */
static Ldm7Status
up7_sendUpToSignature(
        const signaturet* const before)
{
    // `dup_prod_class()` compiles the patterns
    prod_class_t* const prodClass = dup_prod_class(PQ_CLASS_ALL);

    if (NULL == prodClass)
        return LDM7_SYSTEM;

    prodClass->psa.psa_val->feedtype = feedtype;        // was `ANY`

    int  status;
    for (;;) {
        status = pq_sequence(pq, TV_GT, prodClass, up7_sendIfNotSignature,
                (signaturet*)before);                   // cast away `const`
        if (status == EEXIST) {
            status = 0;
            break;
        }
        if (status == PQUEUE_END) {
            log_info("End-of-backlog product not found before end-of-queue");
            status = LDM7_NOENT;
            break;
        }
        if (status) {
            status = LDM7_SYSTEM;
            break;
        }
    }

    free_prod_class(prodClass);

    return status; // TODO
}

/**
 * Asynchronously sends a backlog of data-products that were missed by a
 * downstream LDM-7 due to a new session being started.
 *
 * @pre                {Client-side transport exists}
 * @pre                {Product-queue is open for reading}
 * @param[in] backlog  Specification of data-product backlog.
 * @param[in] rqstp    RPC service-request.
 * @retval    true     Success.
 * @retval    false    Failure. `log_add()` called.
 */
static bool
up7_sendBacklog(
        const BacklogSpec* const restrict backlog)
{
    if (!up7_setProductQueueCursor(backlog))
        return false;

    return LDM7_SYSTEM != up7_sendUpToSignature(&backlog->before);
}

/**
 * Initializes this module.
 *
 * @param[in] workGroup    Name of AL2S workgroup
 * @param[in] endPoint     Local end-point for AL2S virtual circuits
 * @retval    0            Success
 * @retval    LDM7_LOGIC   Module is already initialized. `log_add()` called.
 * @retval    LDM7_SYSTEM  System error. `log_add()` called.
 */
int
up7_init(
        const char* const restrict       workGroup,
        const VcEndPoint* const restrict endPoint)
{
    int status;

    if (isInitialized) {
        log_add("Upstream LDM7 module is already initialized");
        status = LDM7_LOGIC;
    }
    else {
        char* const name = strdup(workGroup);

        if (name == NULL) {
            log_add("Couldn't duplicate AL2S work-group name");
            status = LDM7_SYSTEM;
        }
        else {
            free(wrkGrpName);
            wrkGrpName = name;

            VcEndPoint* end = vcEndPoint_clone(endPoint);

            if (end == NULL) {
                log_add("Couldn't copy local end-point of AL2S "
                        "virtual-circuit");
                status = LDM7_SYSTEM;
            }
            else {
                vcEndPoint_free(localVcEndPoint);
                localVcEndPoint = end;

                isInitialized = true;
                status = 0;
            } // `localVcEndPoint` allocated
        } // `wrkGrpName` allocated
    }

    return status;
}

/**
 * Destroys this module.
 */
void
up7_destroy(void)
{
    log_debug("up7_destroy() entered");

    if (isInitialized) {
        releaseDownFmtpAddr();
        up7_destroyClient();
        up7_closeProdIndexMap();

        up7_destroyVirtCirc();

        vcEndPoint_free(localVcEndPoint);
        localVcEndPoint = NULL;

        free(wrkGrpName);
        wrkGrpName = NULL;

        isDone = false;
        isInitialized = false;
    }
}

/**
 * Synchronously subscribes a downstream LDM-7 to a feed. Called by the RPC
 * dispatch function `ldmprog_7()`.
 *
 * This function is thread-compatible but not thread-safe.
 *
 * @param[in] request     Subscription request
 * @param[in] rqstp       RPC service-request.
 * @retval    NULL        System error. `log_flush()` and `svcerr_systemerr()`
 *                        called. No reply should be sent to the downstream
 *                        LDM-7.
 * @return                Result of the subscription request.
 */
SubscriptionReply*
subscribe_7_svc(
        McastSubReq* const restrict    request,
        struct svc_req* const restrict rqstp)
{
    log_debug("subscribe_7_svc() entered");
    static SubscriptionReply* reply;
    static SubscriptionReply  result;
    struct SVCXPRT* const     xprt = rqstp->rq_xprt;
    const char*               ipv4spec = inet_ntoa(xprt->xp_raddr.sin_addr);
    const char*               hostname = hostbyaddr(&xprt->xp_raddr);
    const char*               feedspec = s_feedtypet(request->feed);

    log_notice_q("Incoming subscription request from %s[%s]:%u for feed %s",
            ipv4spec, hostname, ntohs(xprt->xp_raddr.sin_port), feedspec);
    if (reply) {
        up7_ensureFree(xdr_SubscriptionReply, reply); // free possible prior use
        reply = NULL;
    }

    if (!up7_subscribe(request, xprt, &result)) {
        log_error_q("Subscription failure");
    }
    else {
        if (result.status != LDM7_OK) {
            /*
             * The subscription was unsuccessful for a reason that the
             * downstream LDM7 should understand
             */
            reply = &result;
        }
        else {
            // Subscription was successful

            if (!up7_ensureProductQueueOpen()) {
                log_flush_error();
            }
            else {
                if (!up7_createClientTransport(xprt)) {
                    log_error_q("Couldn't create client-side RPC transport to "
                            " downstream host %s", hostname);
                }
                else {
                    // `clnt` set
                    reply = &result; // Successful reply
                } // Client-side transport to downstream LDM-7 created
            } // Product-queue is open
        } // Successful subscription: `result->status == LDM7_OK`
    } // `result` is set.

    if (reply == NULL) {
        /*
         * A NULL reply will cause the RPC dispatch routine to not reply. This
         * is good because the following `svcerr_systemerr()` replies instead.
         */
        log_flush_error();
        svcerr_systemerr(xprt); // In `svc.c`; Valid for synchronous calls only
        isDone = true;
    }

    return reply;
}

/**
 * Returns the process identifier of the associated multicast LDM sender.
 * @retval 0      Multicast LDM sender doesn't exist
 * @return        PID of multicast LDM sender
 * @threadsafety  Safe
 */
pid_t
getMldmSenderPid(void)
{
    return umm_getMldmSenderPid();
}

/**
 * Asynchronously sends a data-product that the associated downstream LDM-7 did
 * not receive via multicast. Called by the RPC dispatch function `ldmprog_7()`.
 *
 * @param[in] iProd   Index of missed data-product.
 * @param[in] rqstp   RPC service-request.
 * @retval    NULL    Always.
 */
void*
request_product_7_svc(
    FmtpProdIndex* const iProd,
    struct svc_req* const rqstp)
{
    log_debug("Entered: iProd=%lu", (unsigned long)*iProd);
    struct SVCXPRT* const     xprt = rqstp->rq_xprt;

    if (clnt == NULL) {
        log_error_q("Client %s hasn't subscribed yet", rpc_getClientId(rqstp));
        isDone = true;
    }
    else if (!up7_findAndSendProduct(*iProd)) {
        log_flush_error();
        up7_destroyClient();
        isDone = true;
    }

    return NULL;                // don't reply
}

/**
 * Asynchronously sends a backlog of data-products that were missed by a
 * downstream LDM-7 due to a new session being started. Called by the RPC
 * dispatch function `ldmprog_7()`.
 *
 * @param[in] backlog  Specification of data-product backlog.
 * @param[in] rqstp    RPC service-request.
 * @retval    NULL     Always.
 */
void*
request_backlog_7_svc(
    BacklogSpec* const    backlog,
    struct svc_req* const rqstp)
{
    log_debug("Entered");
    struct SVCXPRT* const     xprt = rqstp->rq_xprt;

    if (clnt == NULL) {
        log_error_q("Client %s hasn't subscribed yet", rpc_getClientId(rqstp));
        isDone = true;
    }
    else if (!up7_sendBacklog(backlog)) {
        log_flush_error();
        up7_destroyClient();
        isDone = true;
    }

    return NULL;                // don't reply
}

/**
 * Does nothing. Does not reply.
 *
 * @param[in] rqstp   Pointer to the RPC service-request.
 * @retval    NULL    Always.
 */
void*
test_connection_7_svc(
    void* const           no_op,
    struct svc_req* const rqstp)
{
    log_debug("Entered");
    return NULL;                // don't reply
}
