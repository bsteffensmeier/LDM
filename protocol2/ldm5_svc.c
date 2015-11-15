/*
 * Please do not edit this file.
 * It was generated using rpcgen.
 */

#include "config.h"

#include <stdlib.h> /* getenv(), exit() */
#include <string.h> /* memset() */

#include "ldm5.h"
#include "ulog.h"
#include "mylog.h"


/* $Id: ldm5_svc.c,v 5.54.18.1 2008/04/15 16:34:11 steve Exp $ */

/*
 * Handles incoming LDM-5 RPC requests.  This method is directly and repeatedly
 * invoked by the RPC layer after svc_run(3NSL) is invoked.
 *
 * rqstp        The RPC request.
 * transp       The server-side RPC transport.
 */
void
ldmprog_5(struct svc_req *rqstp, register SVCXPRT *transp)
{
        union {
                product hereis_5_arg;
                prod_class feedme_5_arg;
                prod_class hiya_5_arg;
                prod_info notification_5_arg;
                prod_class_t notifyme_5_arg;
                comingsoon_args comingsoon_5_arg;
                datapkt blkdata_5_arg;
        } argument;
        char *result;
        xdrproc_t xdr_argument, xdr_result;
        char *(*local)(char *, struct svc_req *);
        const char* procName;

        switch (rqstp->rq_proc) {
        case NULLPROC:
                udebug("%s:%d: NULLPROC", __FILE__, __LINE__);
                (void)svc_sendreply(transp, (xdrproc_t) xdr_void, (char *)NULL);
                return;

        case HEREIS:
                xdr_argument = (xdrproc_t) xdr_product;
                xdr_result = (xdrproc_t) xdr_ldm_replyt;
                local = (char *(*)(char *, struct svc_req *)) hereis_5_svc;
                procName = "HEREIS";
                break;

        case FEEDME:
                xdr_argument = (xdrproc_t) xdr_prod_class;
                xdr_result = (xdrproc_t) xdr_ldm_replyt;
                local = (char *(*)(char *, struct svc_req *)) feedme_5_svc;
                procName = "FEEDME";
                break;

        case HIYA:
                xdr_argument = (xdrproc_t) xdr_prod_class;
                xdr_result = (xdrproc_t) xdr_ldm_replyt;
                local = (char *(*)(char *, struct svc_req *)) hiya_5_svc;
                procName = "HIYA";
                break;

        case NOTIFICATION:
                xdr_argument = (xdrproc_t) xdr_prod_info;
                xdr_result = (xdrproc_t) xdr_ldm_replyt;
                local = (char*(*)(char*, struct svc_req *)) notification_5_svc;
                procName = "NOTIFICATION";
                break;

        case NOTIFYME:
                xdr_argument = (xdrproc_t) xdr_prod_class;
                xdr_result = (xdrproc_t) xdr_ldm_replyt;
                local = (char *(*)(char *, struct svc_req *)) notifyme_5_svc;
                procName = "NOTIFYME";
                break;

        case COMINGSOON:
                xdr_argument = (xdrproc_t) xdr_comingsoon_args;
                xdr_result = (xdrproc_t) xdr_ldm_replyt;
                local = (char *(*)(char *, struct svc_req *)) comingsoon_5_svc;
                procName = "COMINGSOON";
                break;

        case BLKDATA:
                xdr_argument = (xdrproc_t) xdr_datapkt;
                xdr_result = (xdrproc_t) xdr_ldm_replyt;
                local = (char *(*)(char *, struct svc_req *)) blkdata_5_svc;
                procName = "BLKDATA";
                break;

        default:
                svcerr_noproc(transp);
                return;
         }

        udebug("%s:%d: %s", __FILE__, __LINE__, procName);
        (void) memset((void *)&argument, 0, sizeof (argument));
        if (!svc_getargs(transp, xdr_argument, (void*) &argument)) {
                unotice("%s:%d: %s: Couldn't decode RPC-request arguments", 
                        __FILE__, __LINE__, procName);
                svcerr_decode(transp);
                return;
        }
        result = (*local)((char *)&argument, rqstp);
        if (result != NULL && !svc_sendreply(transp, xdr_result, result)) {
                unotice("%s:%d: %s: Couldn't reply to RPC-request", 
                        __FILE__, __LINE__, procName);
                svcerr_systemerr(transp);
        }
        if (!svc_freeargs(transp, xdr_argument, (void*) &argument)) {
                uerror("unable to free arguments");
                exit(1);
        }
        return;
}
