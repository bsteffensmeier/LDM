/*
 *   Copyright 1993, University Corporation for Atmospheric Research
 *   See ../COPYRIGHT file for copying and redistribution conditions.
 */
/* $Id: ldmping.c,v 1.20.18.3 2007/04/05 21:41:58 steve Exp $ */

/* 
 * pings remote host
 */

#include "config.h"

#include <errno.h>
#include <rpc/rpc.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include "h_clnt.h"
#include "ldm5.h"
#include "ldm5_clnt.h"
#include "log.h"


#define DEFAULT_INTERVAL 25
#define DEFAULT_TIMEO 10


/*
 * ping the remote
 */
static enum clnt_stat
check_hstat(hcp, timeout)
h_clnt *hcp ;
int timeout ;
{
        return nullproc5(hcp, timeout) ;
}


static void
print_label()
{
        log_info_q("%10s %10s %4s   %-21s %s\n",
                        "State",
                        "Elapsed",
                        "Port",
                        "Remote_Host",
                        "rpc_stat"
                        ) ;
        return ;
}


static void
print_hstat(hcp)
h_clnt *hcp ;
{
        if(hcp->state == RESPONDING)
                log_info_q("%10s %3ld.%06ld %4d   %-11s  %s\n",
                        s_remote_state(hcp->state),
                        hcp->elapsed.tv_sec, hcp->elapsed.tv_usec,
                        hcp->port,
                        hcp->remote,
                        s_hclnt_sperrno(hcp)
                        ) ;
        else
                log_error_q("%10s %3ld.%06ld %4d   %-11s  %s\n",
                        s_remote_state(hcp->state),
                        hcp->elapsed.tv_sec, hcp->elapsed.tv_usec,
                        hcp->port,
                        hcp->remote,
                        s_hclnt_sperrno(hcp)
                        ) ;
        return ;
}


static void
usage(av0)
char *av0 ; /*  id string */
{
        (void)fprintf(stderr,
"Usage: %s [options] [remote ...] \t\nOptions:\n", av0);
        (void)fprintf(stderr,
"\t-v           Verbose (default if interactive)\n") ;
        (void)fprintf(stderr,
"\t-q           Quiet (to shut up when interactive)\n") ;
        (void)fprintf(stderr,
"\t-x           Debug mode\n") ;
        (void)fprintf(stderr,
"\t-l dest      Log to `dest`. One of: \"\" (system logging daemon), \"-\"\n"
"\t             (standard error), or file `dest`. Default is \"%s\"\n",
                log_get_default_destination());
        (void)fprintf(stderr,
"\t                 else uses syslogd)\n") ;
        (void)fprintf(stderr,
"\t-t timeout   set RPC timeout to \"timeout\" seconds (default %d)\n",
                        DEFAULT_TIMEO) ;
        fprintf(stderr,
"\t-i interval  Poll after \"interval\" secs (default %d when interactive,\n",
                DEFAULT_INTERVAL) ;
        (void)fprintf(stderr,
"\t                 0 => one trip otherwise)\n") ;
        (void)fprintf(stderr,
"\t-h remote    \"remote\" host to ping (default is localhost)\n") ;
        exit(1);
}


int main(ac,av)
int ac ;
char *av[] ;
{
        int verbose = 0 ;
        int interval = 0 ;
        int timeo = DEFAULT_TIMEO ; 
        int logoptions = (LOG_CONS|LOG_PID) ;
        int nremotes = 0 ;
#define MAX_REMOTES 14 /* 2 * MAX_REMOTES + 3 < max_open_file_descriptors */
        h_clnt stats[MAX_REMOTES + 1] ;
        h_clnt *sp ;
        unsigned        port = 0;

        /*
         * initialize logger
         */
        (void)log_init(av[0]);
        log_set_level(LOG_LEVEL_INFO);

        if(isatty(fileno(stderr)))
        {
                /* set interactive defaults */
                verbose = !0 ;
                interval = DEFAULT_INTERVAL ;
        }

        {
        extern int optind;
        extern int opterr;
        extern char *optarg;
        int ch;

        opterr = 1;

        while ((ch = getopt(ac, av, "vxl:t:h:P:qi:")) != EOF)
                switch (ch) {
                case 'v':
                        if (!log_is_enabled_info)
                            (void)log_set_level(LOG_LEVEL_INFO);
                        verbose = !0 ;
                        break;
                case 'q':
                        verbose = 0 ;
                        break ;
                case 'x':
                        (void)log_set_level(LOG_LEVEL_DEBUG);
                        break;
                case 'l':
                        (void)log_set_destination(optarg);
                        break;
                case 'h':
                        if(nremotes > MAX_REMOTES)
                        {
                                fprintf(stderr,
                                        "Can't handle more than %d remotes\n", MAX_REMOTES) ;
                                break ;
                        }
                        init_h_clnt(&stats[nremotes++], optarg,
                                LDMPROG, FIVE, IPPROTO_TCP) ;
                        break ;
                case 'P': {
                    char*       suffix = "";
                    long        p;

                    errno = 0;
                    p = strtol(optarg, &suffix, 0);

                    if (0 != errno || 0 != *suffix ||
                        0 >= p || 0xffff < p) {

                        (void)fprintf(stderr, "%s: invalid port %s\n",
                             av[0], optarg);
                        usage(av[0]);   
                    }
                    else {
                        port = p;
                    }

                    break;
                }
                case 't':
                        timeo = atoi(optarg) ;
                        if(timeo == 0 && *optarg != '0')
                        {
                                fprintf(stderr, "%s: invalid timeout %s", av[0], optarg) ;
                                usage(av[0]) ;  
                        }
                        break;
                case 'i':
                        interval = atoi(optarg) ;
                        if(interval == 0 && *optarg != '0')
                        {
                                fprintf(stderr, "%s: invalid interval %s", av[0], optarg) ;
                                        usage(av[0]) ;
                        }
                        break ;
                case '?':
                        usage(av[0]);
                        break;
                }

        for(; nremotes < MAX_REMOTES && optind < ac ; nremotes++, optind++)
        {
                init_h_clnt(&stats[nremotes], av[optind],
                        LDMPROG, FIVE, IPPROTO_TCP) ;
        }
        if(ac - optind > 0)
        {
                fprintf(stderr,
                        "Can't handle more than %d remotes\n", MAX_REMOTES) ;
        }
        if(nremotes == 0)
        {
                init_h_clnt(&stats[nremotes++], "localhost",
                        LDMPROG, FIVE, IPPROTO_TCP) ;
        }
        stats[nremotes].state = H_NONE ; /* terminator */
        }

        /*
         * set up signal handlers
         */
        (void) signal(SIGPIPE, SIG_IGN) ;

        if(verbose)
                print_label() ;

        while(1)
        {
                for(sp = stats ; sp->state != H_NONE ; sp++)
                {
                        check_hstat(sp, timeo) ; 
                        /* if not verbose, only report "significant" stuff */
                        if(verbose || sp->elapsed.tv_sec > 1 || sp->state != RESPONDING)
                                print_hstat(sp) ;
                        if(interval == 0 && sp->state != RESPONDING)
                                exit(1) ;
                }
                if(interval == 0)
                        break ; 
                sleep(interval) ;
        }

        exit(0) ;
}
