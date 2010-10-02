/*
 * Copyright 2000, International Business Machines Corporation and others.
 * All Rights Reserved.
 *
 * This software has been released under the terms of the IBM Public
 * License.  For details, see the LICENSE file in the top-level source
 * directory or online at http://www.openafs.org/dl/license10.html
 */

/* RX:  Extended Remote Procedure Call */

#include <afsconfig.h>
#ifdef	KERNEL
#include "afs/param.h"
#else
#include <afs/param.h>
#endif


#ifdef KERNEL
#include "afs/sysincludes.h"
#include "afsincludes.h"
#ifndef UKERNEL
#include "h/types.h"
#include "h/time.h"
#include "h/stat.h"
#ifdef	AFS_OSF_ENV
#include <net/net_globals.h>
#endif /* AFS_OSF_ENV */
#ifdef AFS_LINUX20_ENV
#include "h/socket.h"
#endif
#include "netinet/in.h"
#ifdef AFS_SUN57_ENV
#include "inet/common.h"
#include "inet/ip.h"
#include "inet/ip_ire.h"
#endif
#include "afs/afs_args.h"
#include "afs/afs_osi.h"
#ifdef RX_KERNEL_TRACE
#include "rx_kcommon.h"
#endif
#if	(defined(AFS_AUX_ENV) || defined(AFS_AIX_ENV))
#include "h/systm.h"
#endif
#ifdef RXDEBUG
#undef RXDEBUG			/* turn off debugging */
#endif /* RXDEBUG */
#if defined(AFS_SGI_ENV)
#include "sys/debug.h"
#endif
#include "afsint.h"
#ifdef	AFS_OSF_ENV
#undef kmem_alloc
#undef kmem_free
#undef mem_alloc
#undef mem_free
#endif /* AFS_OSF_ENV */
#else /* !UKERNEL */
#include "afs/sysincludes.h"
#include "afsincludes.h"
#endif /* !UKERNEL */
#include "afs/lock.h"
#include "rx_kmutex.h"
#include "rx_kernel.h"
#include "rx_clock.h"
#include "rx_queue.h"
#include "rx.h"
#include "rx_globals.h"
#include "rx_trace.h"
#include "rx_atomic.h"
#include "rx_internal.h"
#include "rx_stats.h"
#define	AFSOP_STOP_RXCALLBACK	210	/* Stop CALLBACK process */
#define	AFSOP_STOP_AFS		211	/* Stop AFS process */
#define	AFSOP_STOP_BKG		212	/* Stop BKG process */
#include "afsint.h"
extern afs_int32 afs_termState;
#ifdef AFS_AIX41_ENV
#include "sys/lockl.h"
#include "sys/lock_def.h"
#endif /* AFS_AIX41_ENV */
# include "afs/rxgen_consts.h"
#else /* KERNEL */
# include <sys/types.h>
# include <string.h>
# include <stdarg.h>
# include <errno.h>
# ifdef HAVE_STDINT_H
#  include <stdint.h>
# endif
#ifdef AFS_NT40_ENV
# include <stdlib.h>
# include <fcntl.h>
# include <afs/afsutil.h>
# include <WINNT\afsreg.h>
#else
# include <sys/socket.h>
# include <sys/file.h>
# include <netdb.h>
# include <sys/stat.h>
# include <netinet/in.h>
# include <sys/time.h>
#endif
# include "rx.h"
# include "rx_user.h"
# include "rx_clock.h"
# include "rx_queue.h"
# include "rx_atomic.h"
# include "rx_globals.h"
# include "rx_trace.h"
# include "rx_internal.h"
# include "rx_stats.h"
# include <afs/rxgen_consts.h>
#endif /* KERNEL */

#ifndef KERNEL
#ifdef AFS_PTHREAD_ENV
#ifndef AFS_NT40_ENV
int (*registerProgram) (pid_t, char *) = 0;
int (*swapNameProgram) (pid_t, const char *, char *) = 0;
#endif
#else
int (*registerProgram) (PROCESS, char *) = 0;
int (*swapNameProgram) (PROCESS, const char *, char *) = 0;
#endif
#endif

/* Local static routines */
static void rxi_DestroyConnectionNoLock(struct rx_connection *conn);
#ifdef RX_ENABLE_LOCKS
static void rxi_SetAcksInTransmitQueue(struct rx_call *call);
#endif

#ifdef	AFS_GLOBAL_RXLOCK_KERNEL
struct rx_tq_debug {
    rx_atomic_t rxi_start_aborted; /* rxi_start awoke after rxi_Send in error.*/
    rx_atomic_t rxi_start_in_error;
} rx_tq_debug;
#endif /* AFS_GLOBAL_RXLOCK_KERNEL */

/*
 * rxi_rpc_peer_stat_cnt counts the total number of peer stat structures
 * currently allocated within rx.  This number is used to allocate the
 * memory required to return the statistics when queried.
 * Protected by the rx_rpc_stats mutex.
 */

static unsigned int rxi_rpc_peer_stat_cnt;

/*
 * rxi_rpc_process_stat_cnt counts the total number of local process stat
 * structures currently allocated within rx.  The number is used to allocate
 * the memory required to return the statistics when queried.
 * Protected by the rx_rpc_stats mutex.
 */

static unsigned int rxi_rpc_process_stat_cnt;

rx_atomic_t rx_nWaiting = RX_ATOMIC_INIT(0);
rx_atomic_t rx_nWaited = RX_ATOMIC_INIT(0);

#if !defined(offsetof)
#include <stddef.h>		/* for definition of offsetof() */
#endif

#ifdef RX_ENABLE_LOCKS
afs_kmutex_t rx_atomic_mutex;
#endif

#ifdef AFS_PTHREAD_ENV
#include <assert.h>

/*
 * Use procedural initialization of mutexes/condition variables
 * to ease NT porting
 */

extern afs_kmutex_t rx_quota_mutex;
extern afs_kmutex_t rx_pthread_mutex;
extern afs_kmutex_t rx_packets_mutex;
extern afs_kmutex_t rx_refcnt_mutex;
extern afs_kmutex_t des_init_mutex;
extern afs_kmutex_t des_random_mutex;
extern afs_kmutex_t rx_clock_mutex;
extern afs_kmutex_t rxi_connCacheMutex;
extern afs_kmutex_t rx_event_mutex;
extern afs_kmutex_t osi_malloc_mutex;
extern afs_kmutex_t event_handler_mutex;
extern afs_kmutex_t listener_mutex;
extern afs_kmutex_t rx_if_init_mutex;
extern afs_kmutex_t rx_if_mutex;
extern afs_kmutex_t rxkad_client_uid_mutex;
extern afs_kmutex_t rxkad_random_mutex;

extern afs_kcondvar_t rx_event_handler_cond;
extern afs_kcondvar_t rx_listener_cond;

static afs_kmutex_t epoch_mutex;
static afs_kmutex_t rx_init_mutex;
static afs_kmutex_t rx_debug_mutex;
static afs_kmutex_t rx_rpc_stats;

static void
rxi_InitPthread(void)
{
    MUTEX_INIT(&rx_clock_mutex, "clock", MUTEX_DEFAULT, 0);
    MUTEX_INIT(&rx_stats_mutex, "stats", MUTEX_DEFAULT, 0);
    MUTEX_INIT(&rx_atomic_mutex, "atomic", MUTEX_DEFAULT, 0);
    MUTEX_INIT(&rx_quota_mutex, "quota", MUTEX_DEFAULT, 0);
    MUTEX_INIT(&rx_pthread_mutex, "pthread", MUTEX_DEFAULT, 0);
    MUTEX_INIT(&rx_packets_mutex, "packets", MUTEX_DEFAULT, 0);
    MUTEX_INIT(&rx_refcnt_mutex, "refcnts", MUTEX_DEFAULT, 0);
    MUTEX_INIT(&epoch_mutex, "epoch", MUTEX_DEFAULT, 0);
    MUTEX_INIT(&rx_init_mutex, "init", MUTEX_DEFAULT, 0);
    MUTEX_INIT(&rx_event_mutex, "event", MUTEX_DEFAULT, 0);
    MUTEX_INIT(&des_init_mutex, "des", MUTEX_DEFAULT, 0);
    MUTEX_INIT(&des_random_mutex, "random", MUTEX_DEFAULT, 0);
    MUTEX_INIT(&osi_malloc_mutex, "malloc", MUTEX_DEFAULT, 0);
    MUTEX_INIT(&event_handler_mutex, "event handler", MUTEX_DEFAULT, 0);
    MUTEX_INIT(&rxi_connCacheMutex, "conn cache", MUTEX_DEFAULT, 0);
    MUTEX_INIT(&listener_mutex, "listener", MUTEX_DEFAULT, 0);
    MUTEX_INIT(&rx_if_init_mutex, "if init", MUTEX_DEFAULT, 0);
    MUTEX_INIT(&rx_if_mutex, "if", MUTEX_DEFAULT, 0);
    MUTEX_INIT(&rxkad_client_uid_mutex, "uid", MUTEX_DEFAULT, 0);
    MUTEX_INIT(&rxkad_random_mutex, "rxkad random", MUTEX_DEFAULT, 0);
    MUTEX_INIT(&rx_debug_mutex, "debug", MUTEX_DEFAULT, 0);

    assert(pthread_cond_init
	   (&rx_event_handler_cond, (const pthread_condattr_t *)0) == 0);
    assert(pthread_cond_init(&rx_listener_cond, (const pthread_condattr_t *)0)
	   == 0);
    assert(pthread_key_create(&rx_thread_id_key, NULL) == 0);
    assert(pthread_key_create(&rx_ts_info_key, NULL) == 0);

    rxkad_global_stats_init();

    MUTEX_INIT(&rx_rpc_stats, "rx_rpc_stats", MUTEX_DEFAULT, 0);
    MUTEX_INIT(&rx_freePktQ_lock, "rx_freePktQ_lock", MUTEX_DEFAULT, 0);
#ifdef	RX_ENABLE_LOCKS
#ifdef RX_LOCKS_DB
    rxdb_init();
#endif /* RX_LOCKS_DB */
    MUTEX_INIT(&freeSQEList_lock, "freeSQEList lock", MUTEX_DEFAULT, 0);
    MUTEX_INIT(&rx_freeCallQueue_lock, "rx_freeCallQueue_lock", MUTEX_DEFAULT,
	       0);
    CV_INIT(&rx_waitingForPackets_cv, "rx_waitingForPackets_cv", CV_DEFAULT,
	    0);
    MUTEX_INIT(&rx_peerHashTable_lock, "rx_peerHashTable_lock", MUTEX_DEFAULT,
	       0);
    MUTEX_INIT(&rx_connHashTable_lock, "rx_connHashTable_lock", MUTEX_DEFAULT,
	       0);
    MUTEX_INIT(&rx_serverPool_lock, "rx_serverPool_lock", MUTEX_DEFAULT, 0);
    MUTEX_INIT(&rxi_keyCreate_lock, "rxi_keyCreate_lock", MUTEX_DEFAULT, 0);
#endif /* RX_ENABLE_LOCKS */
}

pthread_once_t rx_once_init = PTHREAD_ONCE_INIT;
#define INIT_PTHREAD_LOCKS \
assert(pthread_once(&rx_once_init, rxi_InitPthread)==0)
/*
 * The rx_stats_mutex mutex protects the following global variables:
 * rxi_lowConnRefCount
 * rxi_lowPeerRefCount
 * rxi_nCalls
 * rxi_Alloccnt
 * rxi_Allocsize
 * rx_tq_debug
 * rx_stats
 */

/*
 * The rx_quota_mutex mutex protects the following global variables:
 * rxi_dataQuota
 * rxi_minDeficit
 * rxi_availProcs
 * rxi_totalMin
 */

/*
 * The rx_freePktQ_lock protects the following global variables:
 * rx_nFreePackets
 */

/*
 * The rx_packets_mutex mutex protects the following global variables:
 * rx_nPackets
 * rx_TSFPQLocalMax
 * rx_TSFPQGlobSize
 * rx_TSFPQMaxProcs
 */

/*
 * The rx_pthread_mutex mutex protects the following global variables:
 * rxi_fcfs_thread_num
 */
#else
#define INIT_PTHREAD_LOCKS
#endif


/* Variables for handling the minProcs implementation.  availProcs gives the
 * number of threads available in the pool at this moment (not counting dudes
 * executing right now).  totalMin gives the total number of procs required
 * for handling all minProcs requests.  minDeficit is a dynamic variable
 * tracking the # of procs required to satisfy all of the remaining minProcs
 * demands.
 * For fine grain locking to work, the quota check and the reservation of
 * a server thread has to come while rxi_availProcs and rxi_minDeficit
 * are locked. To this end, the code has been modified under #ifdef
 * RX_ENABLE_LOCKS so that quota checks and reservation occur at the
 * same time. A new function, ReturnToServerPool() returns the allocation.
 *
 * A call can be on several queue's (but only one at a time). When
 * rxi_ResetCall wants to remove the call from a queue, it has to ensure
 * that no one else is touching the queue. To this end, we store the address
 * of the queue lock in the call structure (under the call lock) when we
 * put the call on a queue, and we clear the call_queue_lock when the
 * call is removed from a queue (once the call lock has been obtained).
 * This allows rxi_ResetCall to safely synchronize with others wishing
 * to manipulate the queue.
 */

#if defined(RX_ENABLE_LOCKS) && defined(KERNEL)
static afs_kmutex_t rx_rpc_stats;
void rxi_StartUnlocked(struct rxevent *event, void *call,
                       void *arg1, int istack);
#endif

/* We keep a "last conn pointer" in rxi_FindConnection. The odds are
** pretty good that the next packet coming in is from the same connection
** as the last packet, since we're send multiple packets in a transmit window.
*/
struct rx_connection *rxLastConn = 0;

#ifdef RX_ENABLE_LOCKS
/* The locking hierarchy for rx fine grain locking is composed of these
 * tiers:
 *
 * rx_connHashTable_lock - synchronizes conn creation, rx_connHashTable access
 * conn_call_lock - used to synchonize rx_EndCall and rx_NewCall
 * call->lock - locks call data fields.
 * These are independent of each other:
 *	rx_freeCallQueue_lock
 *	rxi_keyCreate_lock
 * rx_serverPool_lock
 * freeSQEList_lock
 *
 * serverQueueEntry->lock
 * rx_peerHashTable_lock - locked under rx_connHashTable_lock
 * rx_rpc_stats
 * peer->lock - locks peer data fields.
 * conn_data_lock - that more than one thread is not updating a conn data
 *		    field at the same time.
 * rx_freePktQ_lock
 *
 * lowest level:
 *	multi_handle->lock
 *	rxevent_lock
 *      rx_packets_mutex
 *	rx_stats_mutex
 *      rx_refcnt_mutex
 *	rx_atomic_mutex
 *
 * Do we need a lock to protect the peer field in the conn structure?
 *      conn->peer was previously a constant for all intents and so has no
 *      lock protecting this field. The multihomed client delta introduced
 *      a RX code change : change the peer field in the connection structure
 *      to that remote interface from which the last packet for this
 *      connection was sent out. This may become an issue if further changes
 *      are made.
 */
#define SET_CALL_QUEUE_LOCK(C, L) (C)->call_queue_lock = (L)
#define CLEAR_CALL_QUEUE_LOCK(C) (C)->call_queue_lock = NULL
#ifdef RX_LOCKS_DB
/* rxdb_fileID is used to identify the lock location, along with line#. */
static int rxdb_fileID = RXDB_FILE_RX;
#endif /* RX_LOCKS_DB */
#else /* RX_ENABLE_LOCKS */
#define SET_CALL_QUEUE_LOCK(C, L)
#define CLEAR_CALL_QUEUE_LOCK(C)
#endif /* RX_ENABLE_LOCKS */
struct rx_serverQueueEntry *rx_waitForPacket = 0;
struct rx_serverQueueEntry *rx_waitingForPacket = 0;

/* ------------Exported Interfaces------------- */

/* This function allows rxkad to set the epoch to a suitably random number
 * which rx_NewConnection will use in the future.  The principle purpose is to
 * get rxnull connections to use the same epoch as the rxkad connections do, at
 * least once the first rxkad connection is established.  This is important now
 * that the host/port addresses aren't used in FindConnection: the uniqueness
 * of epoch/cid matters and the start time won't do. */

#ifdef AFS_PTHREAD_ENV
/*
 * This mutex protects the following global variables:
 * rx_epoch
 */

#define LOCK_EPOCH MUTEX_ENTER(&epoch_mutex)
#define UNLOCK_EPOCH MUTEX_EXIT(&epoch_mutex)
#else
#define LOCK_EPOCH
#define UNLOCK_EPOCH
#endif /* AFS_PTHREAD_ENV */

void
rx_SetEpoch(afs_uint32 epoch)
{
    LOCK_EPOCH;
    rx_epoch = epoch;
    UNLOCK_EPOCH;
}

/* Initialize rx.  A port number may be mentioned, in which case this
 * becomes the default port number for any service installed later.
 * If 0 is provided for the port number, a random port will be chosen
 * by the kernel.  Whether this will ever overlap anything in
 * /etc/services is anybody's guess...  Returns 0 on success, -1 on
 * error. */
#ifndef AFS_NT40_ENV
static
#endif
int rxinit_status = 1;
#ifdef AFS_PTHREAD_ENV
/*
 * This mutex protects the following global variables:
 * rxinit_status
 */

#define LOCK_RX_INIT MUTEX_ENTER(&rx_init_mutex)
#define UNLOCK_RX_INIT MUTEX_EXIT(&rx_init_mutex)
#else
#define LOCK_RX_INIT
#define UNLOCK_RX_INIT
#endif

int
rx_InitHost(u_int host, u_int port)
{
#ifdef KERNEL
    osi_timeval_t tv;
#else /* KERNEL */
    struct timeval tv;
#endif /* KERNEL */
    char *htable, *ptable;
    int tmp_status;

    SPLVAR;

    INIT_PTHREAD_LOCKS;
    LOCK_RX_INIT;
    if (rxinit_status == 0) {
	tmp_status = rxinit_status;
	UNLOCK_RX_INIT;
	return tmp_status;	/* Already started; return previous error code. */
    }
#ifdef RXDEBUG
    rxi_DebugInit();
#endif
#ifdef AFS_NT40_ENV
    if (afs_winsockInit() < 0)
	return -1;
#endif

#ifndef KERNEL
    /*
     * Initialize anything necessary to provide a non-premptive threading
     * environment.
     */
    rxi_InitializeThreadSupport();
#endif

    /* Allocate and initialize a socket for client and perhaps server
     * connections. */

    rx_socket = rxi_GetHostUDPSocket(host, (u_short) port);
    if (rx_socket == OSI_NULLSOCKET) {
	UNLOCK_RX_INIT;
	return RX_ADDRINUSE;
    }
#if defined(RX_ENABLE_LOCKS) && defined(KERNEL)
#ifdef RX_LOCKS_DB
    rxdb_init();
#endif /* RX_LOCKS_DB */
    MUTEX_INIT(&rx_stats_mutex, "rx_stats_mutex", MUTEX_DEFAULT, 0);
    MUTEX_INIT(&rx_quota_mutex, "rx_quota_mutex", MUTEX_DEFAULT, 0);
    MUTEX_INIT(&rx_pthread_mutex, "rx_pthread_mutex", MUTEX_DEFAULT, 0);
    MUTEX_INIT(&rx_packets_mutex, "rx_packets_mutex", MUTEX_DEFAULT, 0);
    MUTEX_INIT(&rx_refcnt_mutex, "rx_refcnt_mutex", MUTEX_DEFAULT, 0);
    MUTEX_INIT(&rx_rpc_stats, "rx_rpc_stats", MUTEX_DEFAULT, 0);
    MUTEX_INIT(&rx_freePktQ_lock, "rx_freePktQ_lock", MUTEX_DEFAULT, 0);
    MUTEX_INIT(&freeSQEList_lock, "freeSQEList lock", MUTEX_DEFAULT, 0);
    MUTEX_INIT(&rx_freeCallQueue_lock, "rx_freeCallQueue_lock", MUTEX_DEFAULT,
	       0);
    CV_INIT(&rx_waitingForPackets_cv, "rx_waitingForPackets_cv", CV_DEFAULT,
	    0);
    MUTEX_INIT(&rx_peerHashTable_lock, "rx_peerHashTable_lock", MUTEX_DEFAULT,
	       0);
    MUTEX_INIT(&rx_connHashTable_lock, "rx_connHashTable_lock", MUTEX_DEFAULT,
	       0);
    MUTEX_INIT(&rx_serverPool_lock, "rx_serverPool_lock", MUTEX_DEFAULT, 0);
#if defined(AFS_HPUX110_ENV)
    if (!uniprocessor)
	rx_sleepLock = alloc_spinlock(LAST_HELD_ORDER - 10, "rx_sleepLock");
#endif /* AFS_HPUX110_ENV */
#endif /* RX_ENABLE_LOCKS && KERNEL */

    rxi_nCalls = 0;
    rx_connDeadTime = 12;
    rx_tranquil = 0;		/* reset flag */
    rxi_ResetStatistics();
    htable = (char *)
	osi_Alloc(rx_hashTableSize * sizeof(struct rx_connection *));
    PIN(htable, rx_hashTableSize * sizeof(struct rx_connection *));	/* XXXXX */
    memset(htable, 0, rx_hashTableSize * sizeof(struct rx_connection *));
    ptable = (char *)osi_Alloc(rx_hashTableSize * sizeof(struct rx_peer *));
    PIN(ptable, rx_hashTableSize * sizeof(struct rx_peer *));	/* XXXXX */
    memset(ptable, 0, rx_hashTableSize * sizeof(struct rx_peer *));

    /* Malloc up a bunch of packets & buffers */
    rx_nFreePackets = 0;
    queue_Init(&rx_freePacketQueue);
    rxi_NeedMorePackets = FALSE;
    rx_nPackets = 0;	/* rx_nPackets is managed by rxi_MorePackets* */

    /* enforce a minimum number of allocated packets */
    if (rx_extraPackets < rxi_nSendFrags * rx_maxSendWindow)
        rx_extraPackets = rxi_nSendFrags * rx_maxSendWindow;

    /* allocate the initial free packet pool */
#ifdef RX_ENABLE_TSFPQ
    rxi_MorePacketsTSFPQ(rx_extraPackets + RX_MAX_QUOTA + 2, RX_TS_FPQ_FLUSH_GLOBAL, 0);
#else /* RX_ENABLE_TSFPQ */
    rxi_MorePackets(rx_extraPackets + RX_MAX_QUOTA + 2);        /* fudge */
#endif /* RX_ENABLE_TSFPQ */
    rx_CheckPackets();

    NETPRI;

    clock_Init();

#if defined(AFS_NT40_ENV) && !defined(AFS_PTHREAD_ENV)
    tv.tv_sec = clock_now.sec;
    tv.tv_usec = clock_now.usec;
    srand((unsigned int)tv.tv_usec);
#else
    osi_GetTime(&tv);
#endif
    if (port) {
	rx_port = port;
    } else {
#if defined(KERNEL) && !defined(UKERNEL)
	/* Really, this should never happen in a real kernel */
	rx_port = 0;
#else
	struct sockaddr_in addr;
#ifdef AFS_NT40_ENV
        int addrlen = sizeof(addr);
#else
	socklen_t addrlen = sizeof(addr);
#endif
	if (getsockname((intptr_t)rx_socket, (struct sockaddr *)&addr, &addrlen)) {
	    rx_Finalize();
	    return -1;
	}
	rx_port = addr.sin_port;
#endif
    }
    rx_stats.minRtt.sec = 9999999;
#ifdef	KERNEL
    rx_SetEpoch(tv.tv_sec | 0x80000000);
#else
    rx_SetEpoch(tv.tv_sec);	/* Start time of this package, rxkad
				 * will provide a randomer value. */
#endif
    MUTEX_ENTER(&rx_quota_mutex);
    rxi_dataQuota += rx_extraQuota; /* + extra pkts caller asked to rsrv */
    MUTEX_EXIT(&rx_quota_mutex);
    /* *Slightly* random start time for the cid.  This is just to help
     * out with the hashing function at the peer */
    rx_nextCid = ((tv.tv_sec ^ tv.tv_usec) << RX_CIDSHIFT);
    rx_connHashTable = (struct rx_connection **)htable;
    rx_peerHashTable = (struct rx_peer **)ptable;

    rx_lastAckDelay.sec = 0;
    rx_lastAckDelay.usec = 400000;	/* 400 milliseconds */
    rx_hardAckDelay.sec = 0;
    rx_hardAckDelay.usec = 100000;	/* 100 milliseconds */
    rx_softAckDelay.sec = 0;
    rx_softAckDelay.usec = 100000;	/* 100 milliseconds */

    rxevent_Init(20, rxi_ReScheduleEvents);

    /* Initialize various global queues */
    queue_Init(&rx_idleServerQueue);
    queue_Init(&rx_incomingCallQueue);
    queue_Init(&rx_freeCallQueue);

#if defined(AFS_NT40_ENV) && !defined(KERNEL)
    /* Initialize our list of usable IP addresses. */
    rx_GetIFInfo();
#endif

    /* Start listener process (exact function is dependent on the
     * implementation environment--kernel or user space) */
    rxi_StartListener();

    USERPRI;
    tmp_status = rxinit_status = 0;
    UNLOCK_RX_INIT;
    return tmp_status;
}

int
rx_Init(u_int port)
{
    return rx_InitHost(htonl(INADDR_ANY), port);
}

/* called with unincremented nRequestsRunning to see if it is OK to start
 * a new thread in this service.  Could be "no" for two reasons: over the
 * max quota, or would prevent others from reaching their min quota.
 */
#ifdef RX_ENABLE_LOCKS
/* This verion of QuotaOK reserves quota if it's ok while the
 * rx_serverPool_lock is held.  Return quota using ReturnToServerPool().
 */
static int
QuotaOK(struct rx_service *aservice)
{
    /* check if over max quota */
    if (aservice->nRequestsRunning >= aservice->maxProcs) {
	return 0;
    }

    /* under min quota, we're OK */
    /* otherwise, can use only if there are enough to allow everyone
     * to go to their min quota after this guy starts.
     */

    MUTEX_ENTER(&rx_quota_mutex);
    if ((aservice->nRequestsRunning < aservice->minProcs)
	|| (rxi_availProcs > rxi_minDeficit)) {
	aservice->nRequestsRunning++;
	/* just started call in minProcs pool, need fewer to maintain
	 * guarantee */
	if (aservice->nRequestsRunning <= aservice->minProcs)
	    rxi_minDeficit--;
	rxi_availProcs--;
	MUTEX_EXIT(&rx_quota_mutex);
	return 1;
    }
    MUTEX_EXIT(&rx_quota_mutex);

    return 0;
}

static void
ReturnToServerPool(struct rx_service *aservice)
{
    aservice->nRequestsRunning--;
    MUTEX_ENTER(&rx_quota_mutex);
    if (aservice->nRequestsRunning < aservice->minProcs)
	rxi_minDeficit++;
    rxi_availProcs++;
    MUTEX_EXIT(&rx_quota_mutex);
}

#else /* RX_ENABLE_LOCKS */
static int
QuotaOK(struct rx_service *aservice)
{
    int rc = 0;
    /* under min quota, we're OK */
    if (aservice->nRequestsRunning < aservice->minProcs)
	return 1;

    /* check if over max quota */
    if (aservice->nRequestsRunning >= aservice->maxProcs)
	return 0;

    /* otherwise, can use only if there are enough to allow everyone
     * to go to their min quota after this guy starts.
     */
    MUTEX_ENTER(&rx_quota_mutex);
    if (rxi_availProcs > rxi_minDeficit)
	rc = 1;
    MUTEX_EXIT(&rx_quota_mutex);
    return rc;
}
#endif /* RX_ENABLE_LOCKS */

#ifndef KERNEL
/* Called by rx_StartServer to start up lwp's to service calls.
   NExistingProcs gives the number of procs already existing, and which
   therefore needn't be created. */
void
rxi_StartServerProcs(int nExistingProcs)
{
    struct rx_service *service;
    int i;
    int maxdiff = 0;
    int nProcs = 0;

    /* For each service, reserve N processes, where N is the "minimum"
     * number of processes that MUST be able to execute a request in parallel,
     * at any time, for that process.  Also compute the maximum difference
     * between any service's maximum number of processes that can run
     * (i.e. the maximum number that ever will be run, and a guarantee
     * that this number will run if other services aren't running), and its
     * minimum number.  The result is the extra number of processes that
     * we need in order to provide the latter guarantee */
    for (i = 0; i < RX_MAX_SERVICES; i++) {
	int diff;
	service = rx_services[i];
	if (service == (struct rx_service *)0)
	    break;
	nProcs += service->minProcs;
	diff = service->maxProcs - service->minProcs;
	if (diff > maxdiff)
	    maxdiff = diff;
    }
    nProcs += maxdiff;		/* Extra processes needed to allow max number requested to run in any given service, under good conditions */
    nProcs -= nExistingProcs;	/* Subtract the number of procs that were previously created for use as server procs */
    for (i = 0; i < nProcs; i++) {
	rxi_StartServerProc(rx_ServerProc, rx_stackSize);
    }
}
#endif /* KERNEL */

#ifdef AFS_NT40_ENV
/* This routine is only required on Windows */
void
rx_StartClientThread(void)
{
#ifdef AFS_PTHREAD_ENV
    pthread_t pid;
    pid = pthread_self();
#endif /* AFS_PTHREAD_ENV */
}
#endif /* AFS_NT40_ENV */

/* This routine must be called if any services are exported.  If the
 * donateMe flag is set, the calling process is donated to the server
 * process pool */
void
rx_StartServer(int donateMe)
{
    struct rx_service *service;
    int i;
    SPLVAR;
    clock_NewTime();

    NETPRI;
    /* Start server processes, if necessary (exact function is dependent
     * on the implementation environment--kernel or user space).  DonateMe
     * will be 1 if there is 1 pre-existing proc, i.e. this one.  In this
     * case, one less new proc will be created rx_StartServerProcs.
     */
    rxi_StartServerProcs(donateMe);

    /* count up the # of threads in minProcs, and add set the min deficit to
     * be that value, too.
     */
    for (i = 0; i < RX_MAX_SERVICES; i++) {
	service = rx_services[i];
	if (service == (struct rx_service *)0)
	    break;
	MUTEX_ENTER(&rx_quota_mutex);
	rxi_totalMin += service->minProcs;
	/* below works even if a thread is running, since minDeficit would
	 * still have been decremented and later re-incremented.
	 */
	rxi_minDeficit += service->minProcs;
	MUTEX_EXIT(&rx_quota_mutex);
    }

    /* Turn on reaping of idle server connections */
    rxi_ReapConnections(NULL, NULL, NULL);

    USERPRI;

    if (donateMe) {
#ifndef AFS_NT40_ENV
#ifndef KERNEL
	char name[32];
	static int nProcs;
#ifdef AFS_PTHREAD_ENV
	pid_t pid;
	pid = afs_pointer_to_int(pthread_self());
#else /* AFS_PTHREAD_ENV */
	PROCESS pid;
	LWP_CurrentProcess(&pid);
#endif /* AFS_PTHREAD_ENV */

	sprintf(name, "srv_%d", ++nProcs);
	if (registerProgram)
	    (*registerProgram) (pid, name);
#endif /* KERNEL */
#endif /* AFS_NT40_ENV */
	rx_ServerProc(NULL);	/* Never returns */
    }
#ifdef RX_ENABLE_TSFPQ
    /* no use leaving packets around in this thread's local queue if
     * it isn't getting donated to the server thread pool.
     */
    rxi_FlushLocalPacketsTSFPQ();
#endif /* RX_ENABLE_TSFPQ */
    return;
}

/* Create a new client connection to the specified service, using the
 * specified security object to implement the security model for this
 * connection. */
struct rx_connection *
rx_NewConnection(afs_uint32 shost, u_short sport, u_short sservice,
		 struct rx_securityClass *securityObject,
		 int serviceSecurityIndex)
{
    int hashindex, i;
    afs_int32 cid;
    struct rx_connection *conn;

    SPLVAR;

    clock_NewTime();
    dpf(("rx_NewConnection(host %x, port %u, service %u, securityObject %p, "
	 "serviceSecurityIndex %d)\n",
         ntohl(shost), ntohs(sport), sservice, securityObject,
	 serviceSecurityIndex));

    /* Vasilsi said: "NETPRI protects Cid and Alloc", but can this be true in
     * the case of kmem_alloc? */
    conn = rxi_AllocConnection();
#ifdef	RX_ENABLE_LOCKS
    MUTEX_INIT(&conn->conn_call_lock, "conn call lock", MUTEX_DEFAULT, 0);
    MUTEX_INIT(&conn->conn_data_lock, "conn data lock", MUTEX_DEFAULT, 0);
    CV_INIT(&conn->conn_call_cv, "conn call cv", CV_DEFAULT, 0);
#endif
    NETPRI;
    MUTEX_ENTER(&rx_connHashTable_lock);
    cid = (rx_nextCid += RX_MAXCALLS);
    conn->type = RX_CLIENT_CONNECTION;
    conn->cid = cid;
    conn->epoch = rx_epoch;
    conn->peer = rxi_FindPeer(shost, sport, 0, 1);
    conn->serviceId = sservice;
    conn->securityObject = securityObject;
    conn->securityData = (void *) 0;
    conn->securityIndex = serviceSecurityIndex;
    rx_SetConnDeadTime(conn, rx_connDeadTime);
    rx_SetConnSecondsUntilNatPing(conn, 0);
    conn->ackRate = RX_FAST_ACK_RATE;
    conn->nSpecific = 0;
    conn->specific = NULL;
    conn->challengeEvent = NULL;
    conn->delayedAbortEvent = NULL;
    conn->abortCount = 0;
    conn->error = 0;
    for (i = 0; i < RX_MAXCALLS; i++) {
	conn->twind[i] = rx_initSendWindow;
	conn->rwind[i] = rx_initReceiveWindow;
    }

    RXS_NewConnection(securityObject, conn);
    hashindex =
	CONN_HASH(shost, sport, conn->cid, conn->epoch, RX_CLIENT_CONNECTION);

    conn->refCount++;		/* no lock required since only this thread knows... */
    conn->next = rx_connHashTable[hashindex];
    rx_connHashTable[hashindex] = conn;
    if (rx_stats_active)
	rx_atomic_inc(&rx_stats.nClientConns);
    MUTEX_EXIT(&rx_connHashTable_lock);
    USERPRI;
    return conn;
}

void
rx_SetConnDeadTime(struct rx_connection *conn, int seconds)
{
    /* The idea is to set the dead time to a value that allows several
     * keepalives to be dropped without timing out the connection. */
    conn->secondsUntilDead = MAX(seconds, 6);
    conn->secondsUntilPing = conn->secondsUntilDead / 6;
}

int rxi_lowPeerRefCount = 0;
int rxi_lowConnRefCount = 0;

/*
 * Cleanup a connection that was destroyed in rxi_DestroyConnectioNoLock.
 * NOTE: must not be called with rx_connHashTable_lock held.
 */
void
rxi_CleanupConnection(struct rx_connection *conn)
{
    /* Notify the service exporter, if requested, that this connection
     * is being destroyed */
    if (conn->type == RX_SERVER_CONNECTION && conn->service->destroyConnProc)
	(*conn->service->destroyConnProc) (conn);

    /* Notify the security module that this connection is being destroyed */
    RXS_DestroyConnection(conn->securityObject, conn);

    /* If this is the last connection using the rx_peer struct, set its
     * idle time to now. rxi_ReapConnections will reap it if it's still
     * idle (refCount == 0) after rx_idlePeerTime (60 seconds) have passed.
     */
    MUTEX_ENTER(&rx_peerHashTable_lock);
    if (conn->peer->refCount < 2) {
	conn->peer->idleWhen = clock_Sec();
	if (conn->peer->refCount < 1) {
	    conn->peer->refCount = 1;
            if (rx_stats_active) {
                MUTEX_ENTER(&rx_stats_mutex);
                rxi_lowPeerRefCount++;
                MUTEX_EXIT(&rx_stats_mutex);
            }
	}
    }
    conn->peer->refCount--;
    MUTEX_EXIT(&rx_peerHashTable_lock);

    if (rx_stats_active)
    {
        if (conn->type == RX_SERVER_CONNECTION)
	    rx_atomic_dec(&rx_stats.nServerConns);
        else
	    rx_atomic_dec(&rx_stats.nClientConns);
    }
#ifndef KERNEL
    if (conn->specific) {
	int i;
	for (i = 0; i < conn->nSpecific; i++) {
	    if (conn->specific[i] && rxi_keyCreate_destructor[i])
		(*rxi_keyCreate_destructor[i]) (conn->specific[i]);
	    conn->specific[i] = NULL;
	}
	free(conn->specific);
    }
    conn->specific = NULL;
    conn->nSpecific = 0;
#endif /* !KERNEL */

    MUTEX_DESTROY(&conn->conn_call_lock);
    MUTEX_DESTROY(&conn->conn_data_lock);
    CV_DESTROY(&conn->conn_call_cv);

    rxi_FreeConnection(conn);
}

/* Destroy the specified connection */
void
rxi_DestroyConnection(struct rx_connection *conn)
{
    MUTEX_ENTER(&rx_connHashTable_lock);
    rxi_DestroyConnectionNoLock(conn);
    /* conn should be at the head of the cleanup list */
    if (conn == rx_connCleanup_list) {
	rx_connCleanup_list = rx_connCleanup_list->next;
	MUTEX_EXIT(&rx_connHashTable_lock);
	rxi_CleanupConnection(conn);
    }
#ifdef RX_ENABLE_LOCKS
    else {
	MUTEX_EXIT(&rx_connHashTable_lock);
    }
#endif /* RX_ENABLE_LOCKS */
}

static void
rxi_DestroyConnectionNoLock(struct rx_connection *conn)
{
    struct rx_connection **conn_ptr;
    int havecalls = 0;
    struct rx_packet *packet;
    int i;
    SPLVAR;

    clock_NewTime();

    NETPRI;
    MUTEX_ENTER(&conn->conn_data_lock);
    MUTEX_ENTER(&rx_refcnt_mutex);
    if (conn->refCount > 0)
	conn->refCount--;
    else {
        if (rx_stats_active) {
            MUTEX_ENTER(&rx_stats_mutex);
            rxi_lowConnRefCount++;
            MUTEX_EXIT(&rx_stats_mutex);
        }
    }

    if ((conn->refCount > 0) || (conn->flags & RX_CONN_BUSY)) {
	/* Busy; wait till the last guy before proceeding */
        MUTEX_EXIT(&rx_refcnt_mutex);
	MUTEX_EXIT(&conn->conn_data_lock);
	USERPRI;
	return;
    }

    /* If the client previously called rx_NewCall, but it is still
     * waiting, treat this as a running call, and wait to destroy the
     * connection later when the call completes. */
    if ((conn->type == RX_CLIENT_CONNECTION)
	&& (conn->flags & (RX_CONN_MAKECALL_WAITING|RX_CONN_MAKECALL_ACTIVE))) {
	conn->flags |= RX_CONN_DESTROY_ME;
	MUTEX_EXIT(&conn->conn_data_lock);
	USERPRI;
	return;
    }
    MUTEX_EXIT(&rx_refcnt_mutex);
    MUTEX_EXIT(&conn->conn_data_lock);

    /* Check for extant references to this connection */
    for (i = 0; i < RX_MAXCALLS; i++) {
	struct rx_call *call = conn->call[i];
	if (call) {
	    havecalls = 1;
	    if (conn->type == RX_CLIENT_CONNECTION) {
		MUTEX_ENTER(&call->lock);
		if (call->delayedAckEvent) {
		    /* Push the final acknowledgment out now--there
		     * won't be a subsequent call to acknowledge the
		     * last reply packets */
		    rxevent_Cancel(call->delayedAckEvent, call,
				   RX_CALL_REFCOUNT_DELAY);
		    if (call->state == RX_STATE_PRECALL
			|| call->state == RX_STATE_ACTIVE) {
			rxi_SendAck(call, 0, 0, RX_ACK_DELAY, 0);
		    } else {
			rxi_AckAll(NULL, call, 0);
		    }
		}
		MUTEX_EXIT(&call->lock);
	    }
	}
    }
#ifdef RX_ENABLE_LOCKS
    if (!havecalls) {
	if (MUTEX_TRYENTER(&conn->conn_data_lock)) {
	    MUTEX_EXIT(&conn->conn_data_lock);
	} else {
	    /* Someone is accessing a packet right now. */
	    havecalls = 1;
	}
    }
#endif /* RX_ENABLE_LOCKS */

    if (havecalls) {
	/* Don't destroy the connection if there are any call
	 * structures still in use */
	MUTEX_ENTER(&conn->conn_data_lock);
	conn->flags |= RX_CONN_DESTROY_ME;
	MUTEX_EXIT(&conn->conn_data_lock);
	USERPRI;
	return;
    }

    if (conn->natKeepAliveEvent) {
	rxi_NatKeepAliveOff(conn);
    }

    if (conn->delayedAbortEvent) {
	rxevent_Cancel(conn->delayedAbortEvent, (struct rx_call *)0, 0);
	packet = rxi_AllocPacket(RX_PACKET_CLASS_SPECIAL);
	if (packet) {
	    MUTEX_ENTER(&conn->conn_data_lock);
	    rxi_SendConnectionAbort(conn, packet, 0, 1);
	    MUTEX_EXIT(&conn->conn_data_lock);
	    rxi_FreePacket(packet);
	}
    }

    /* Remove from connection hash table before proceeding */
    conn_ptr =
	&rx_connHashTable[CONN_HASH
			  (peer->host, peer->port, conn->cid, conn->epoch,
			   conn->type)];
    for (; *conn_ptr; conn_ptr = &(*conn_ptr)->next) {
	if (*conn_ptr == conn) {
	    *conn_ptr = conn->next;
	    break;
	}
    }
    /* if the conn that we are destroying was the last connection, then we
     * clear rxLastConn as well */
    if (rxLastConn == conn)
	rxLastConn = 0;

    /* Make sure the connection is completely reset before deleting it. */
    /* get rid of pending events that could zap us later */
    if (conn->challengeEvent)
	rxevent_Cancel(conn->challengeEvent, (struct rx_call *)0, 0);
    if (conn->checkReachEvent)
	rxevent_Cancel(conn->checkReachEvent, (struct rx_call *)0, 0);
    if (conn->natKeepAliveEvent)
	rxevent_Cancel(conn->natKeepAliveEvent, (struct rx_call *)0, 0);

    /* Add the connection to the list of destroyed connections that
     * need to be cleaned up. This is necessary to avoid deadlocks
     * in the routines we call to inform others that this connection is
     * being destroyed. */
    conn->next = rx_connCleanup_list;
    rx_connCleanup_list = conn;
}

/* Externally available version */
void
rx_DestroyConnection(struct rx_connection *conn)
{
    SPLVAR;

    NETPRI;
    rxi_DestroyConnection(conn);
    USERPRI;
}

void
rx_GetConnection(struct rx_connection *conn)
{
    SPLVAR;

    NETPRI;
    MUTEX_ENTER(&rx_refcnt_mutex);
    conn->refCount++;
    MUTEX_EXIT(&rx_refcnt_mutex);
    USERPRI;
}

#ifdef  AFS_GLOBAL_RXLOCK_KERNEL
/* Wait for the transmit queue to no longer be busy.
 * requires the call->lock to be held */
static void rxi_WaitforTQBusy(struct rx_call *call) {
    while (call->flags & RX_CALL_TQ_BUSY) {
	call->flags |= RX_CALL_TQ_WAIT;
	call->tqWaiters++;
#ifdef RX_ENABLE_LOCKS
	osirx_AssertMine(&call->lock, "rxi_WaitforTQ lock");
	CV_WAIT(&call->cv_tq, &call->lock);
#else /* RX_ENABLE_LOCKS */
	osi_rxSleep(&call->tq);
#endif /* RX_ENABLE_LOCKS */
	call->tqWaiters--;
	if (call->tqWaiters == 0) {
	    call->flags &= ~RX_CALL_TQ_WAIT;
	}
    }
}
#endif

/* Start a new rx remote procedure call, on the specified connection.
 * If wait is set to 1, wait for a free call channel; otherwise return
 * 0.  Maxtime gives the maximum number of seconds this call may take,
 * after rx_NewCall returns.  After this time interval, a call to any
 * of rx_SendData, rx_ReadData, etc. will fail with RX_CALL_TIMEOUT.
 * For fine grain locking, we hold the conn_call_lock in order to
 * to ensure that we don't get signalle after we found a call in an active
 * state and before we go to sleep.
 */
struct rx_call *
rx_NewCall(struct rx_connection *conn)
{
    int i, wait;
    struct rx_call *call;
    struct clock queueTime;
    SPLVAR;

    clock_NewTime();
    dpf(("rx_NewCall(conn %"AFS_PTR_FMT")\n", conn));

    NETPRI;
    clock_GetTime(&queueTime);
    /*
     * Check if there are others waiting for a new call.
     * If so, let them go first to avoid starving them.
     * This is a fairly simple scheme, and might not be
     * a complete solution for large numbers of waiters.
     *
     * makeCallWaiters keeps track of the number of
     * threads waiting to make calls and the
     * RX_CONN_MAKECALL_WAITING flag bit is used to
     * indicate that there are indeed calls waiting.
     * The flag is set when the waiter is incremented.
     * It is only cleared when makeCallWaiters is 0.
     * This prevents us from accidently destroying the
     * connection while it is potentially about to be used.
     */
    MUTEX_ENTER(&conn->conn_call_lock);
    MUTEX_ENTER(&conn->conn_data_lock);
    while (conn->flags & RX_CONN_MAKECALL_ACTIVE) {
        conn->flags |= RX_CONN_MAKECALL_WAITING;
	conn->makeCallWaiters++;
        MUTEX_EXIT(&conn->conn_data_lock);

#ifdef	RX_ENABLE_LOCKS
        CV_WAIT(&conn->conn_call_cv, &conn->conn_call_lock);
#else
        osi_rxSleep(conn);
#endif
	MUTEX_ENTER(&conn->conn_data_lock);
	conn->makeCallWaiters--;
        if (conn->makeCallWaiters == 0)
            conn->flags &= ~RX_CONN_MAKECALL_WAITING;
    }

    /* We are now the active thread in rx_NewCall */
    conn->flags |= RX_CONN_MAKECALL_ACTIVE;
    MUTEX_EXIT(&conn->conn_data_lock);

    for (;;) {
        wait = 1;

	for (i = 0; i < RX_MAXCALLS; i++) {
	    call = conn->call[i];
	    if (call) {
		if (call->state == RX_STATE_DALLY) {
                    MUTEX_ENTER(&call->lock);
                    if (call->state == RX_STATE_DALLY) {
                        /*
                         * We are setting the state to RX_STATE_RESET to
                         * ensure that no one else will attempt to use this
                         * call once we drop the conn->conn_call_lock and
                         * call->lock.  We must drop the conn->conn_call_lock
                         * before calling rxi_ResetCall because the process
                         * of clearing the transmit queue can block for an
                         * extended period of time.  If we block while holding
                         * the conn->conn_call_lock, then all rx_EndCall
                         * processing will block as well.  This has a detrimental
                         * effect on overall system performance.
                         */
                        call->state = RX_STATE_RESET;
                        MUTEX_EXIT(&conn->conn_call_lock);
                        MUTEX_ENTER(&rx_refcnt_mutex);
                        CALL_HOLD(call, RX_CALL_REFCOUNT_BEGIN);
                        MUTEX_EXIT(&rx_refcnt_mutex);
                        rxi_ResetCall(call, 0);
                        (*call->callNumber)++;
                        if (MUTEX_TRYENTER(&conn->conn_call_lock))
                            break;

                        /*
                         * If we failed to be able to safely obtain the
                         * conn->conn_call_lock we will have to drop the
                         * call->lock to avoid a deadlock.  When the call->lock
                         * is released the state of the call can change.  If it
                         * is no longer RX_STATE_RESET then some other thread is
                         * using the call.
                         */
                        MUTEX_EXIT(&call->lock);
                        MUTEX_ENTER(&conn->conn_call_lock);
                        MUTEX_ENTER(&call->lock);

                        if (call->state == RX_STATE_RESET)
                            break;

                        /*
                         * If we get here it means that after dropping
                         * the conn->conn_call_lock and call->lock that
                         * the call is no longer ours.  If we can't find
                         * a free call in the remaining slots we should
                         * not go immediately to RX_CONN_MAKECALL_WAITING
                         * because by dropping the conn->conn_call_lock
                         * we have given up synchronization with rx_EndCall.
                         * Instead, cycle through one more time to see if
                         * we can find a call that can call our own.
                         */
                        MUTEX_ENTER(&rx_refcnt_mutex);
                        CALL_RELE(call, RX_CALL_REFCOUNT_BEGIN);
                        MUTEX_EXIT(&rx_refcnt_mutex);
                        wait = 0;
                    }
                    MUTEX_EXIT(&call->lock);
                }
	    } else {
                /* rxi_NewCall returns with mutex locked */
		call = rxi_NewCall(conn, i);
                MUTEX_ENTER(&rx_refcnt_mutex);
                CALL_HOLD(call, RX_CALL_REFCOUNT_BEGIN);
                MUTEX_EXIT(&rx_refcnt_mutex);
		break;
	    }
	}
	if (i < RX_MAXCALLS) {
	    break;
	}
        if (!wait)
            continue;

	MUTEX_ENTER(&conn->conn_data_lock);
	conn->flags |= RX_CONN_MAKECALL_WAITING;
	conn->makeCallWaiters++;
	MUTEX_EXIT(&conn->conn_data_lock);

#ifdef	RX_ENABLE_LOCKS
	CV_WAIT(&conn->conn_call_cv, &conn->conn_call_lock);
#else
	osi_rxSleep(conn);
#endif
	MUTEX_ENTER(&conn->conn_data_lock);
	conn->makeCallWaiters--;
        if (conn->makeCallWaiters == 0)
            conn->flags &= ~RX_CONN_MAKECALL_WAITING;
	MUTEX_EXIT(&conn->conn_data_lock);
    }
    /* Client is initially in send mode */
    call->state = RX_STATE_ACTIVE;
    call->error = conn->error;
    if (call->error)
	call->mode = RX_MODE_ERROR;
    else
	call->mode = RX_MODE_SENDING;

    /* remember start time for call in case we have hard dead time limit */
    call->queueTime = queueTime;
    clock_GetTime(&call->startTime);
    hzero(call->bytesSent);
    hzero(call->bytesRcvd);

    /* Turn on busy protocol. */
    rxi_KeepAliveOn(call);

    /* Attempt MTU discovery */
    rxi_GrowMTUOn(call);

    /*
     * We are no longer the active thread in rx_NewCall
     */
    MUTEX_ENTER(&conn->conn_data_lock);
    conn->flags &= ~RX_CONN_MAKECALL_ACTIVE;
    MUTEX_EXIT(&conn->conn_data_lock);

    /*
     * Wake up anyone else who might be giving us a chance to
     * run (see code above that avoids resource starvation).
     */
#ifdef	RX_ENABLE_LOCKS
    CV_BROADCAST(&conn->conn_call_cv);
#else
    osi_rxWakeup(conn);
#endif
    MUTEX_EXIT(&conn->conn_call_lock);

#ifdef	AFS_GLOBAL_RXLOCK_KERNEL
    if (call->flags & (RX_CALL_TQ_BUSY | RX_CALL_TQ_CLEARME)) {
        osi_Panic("rx_NewCall call about to be used without an empty tq");
    }
#endif /* AFS_GLOBAL_RXLOCK_KERNEL */

    MUTEX_EXIT(&call->lock);
    USERPRI;

    dpf(("rx_NewCall(call %"AFS_PTR_FMT")\n", call));
    return call;
}

int
rxi_HasActiveCalls(struct rx_connection *aconn)
{
    int i;
    struct rx_call *tcall;
    SPLVAR;

    NETPRI;
    for (i = 0; i < RX_MAXCALLS; i++) {
	if ((tcall = aconn->call[i])) {
	    if ((tcall->state == RX_STATE_ACTIVE)
		|| (tcall->state == RX_STATE_PRECALL)) {
		USERPRI;
		return 1;
	    }
	}
    }
    USERPRI;
    return 0;
}

int
rxi_GetCallNumberVector(struct rx_connection *aconn,
			afs_int32 * aint32s)
{
    int i;
    struct rx_call *tcall;
    SPLVAR;

    NETPRI;
    for (i = 0; i < RX_MAXCALLS; i++) {
	if ((tcall = aconn->call[i]) && (tcall->state == RX_STATE_DALLY))
	    aint32s[i] = aconn->callNumber[i] + 1;
	else
	    aint32s[i] = aconn->callNumber[i];
    }
    USERPRI;
    return 0;
}

int
rxi_SetCallNumberVector(struct rx_connection *aconn,
			afs_int32 * aint32s)
{
    int i;
    struct rx_call *tcall;
    SPLVAR;

    NETPRI;
    for (i = 0; i < RX_MAXCALLS; i++) {
	if ((tcall = aconn->call[i]) && (tcall->state == RX_STATE_DALLY))
	    aconn->callNumber[i] = aint32s[i] - 1;
	else
	    aconn->callNumber[i] = aint32s[i];
    }
    USERPRI;
    return 0;
}

/* Advertise a new service.  A service is named locally by a UDP port
 * number plus a 16-bit service id.  Returns (struct rx_service *) 0
 * on a failure.
 *
     char *serviceName;	 Name for identification purposes (e.g. the
                         service name might be used for probing for
                         statistics) */
struct rx_service *
rx_NewServiceHost(afs_uint32 host, u_short port, u_short serviceId,
		  char *serviceName, struct rx_securityClass **securityObjects,
		  int nSecurityObjects,
		  afs_int32(*serviceProc) (struct rx_call * acall))
{
    osi_socket socket = OSI_NULLSOCKET;
    struct rx_service *tservice;
    int i;
    SPLVAR;

    clock_NewTime();

    if (serviceId == 0) {
	(osi_Msg
	 "rx_NewService:  service id for service %s is not non-zero.\n",
	 serviceName);
	return 0;
    }
    if (port == 0) {
	if (rx_port == 0) {
	    (osi_Msg
	     "rx_NewService: A non-zero port must be specified on this call if a non-zero port was not provided at Rx initialization (service %s).\n",
	     serviceName);
	    return 0;
	}
	port = rx_port;
	socket = rx_socket;
    }

    tservice = rxi_AllocService();
    NETPRI;

#ifdef	RX_ENABLE_LOCKS
    MUTEX_INIT(&tservice->svc_data_lock, "svc data lock", MUTEX_DEFAULT, 0);
#endif

    for (i = 0; i < RX_MAX_SERVICES; i++) {
	struct rx_service *service = rx_services[i];
	if (service) {
	    if (port == service->servicePort && host == service->serviceHost) {
		if (service->serviceId == serviceId) {
		    /* The identical service has already been
		     * installed; if the caller was intending to
		     * change the security classes used by this
		     * service, he/she loses. */
		    (osi_Msg
		     "rx_NewService: tried to install service %s with service id %d, which is already in use for service %s\n",
		     serviceName, serviceId, service->serviceName);
		    USERPRI;
		    rxi_FreeService(tservice);
		    return service;
		}
		/* Different service, same port: re-use the socket
		 * which is bound to the same port */
		socket = service->socket;
	    }
	} else {
	    if (socket == OSI_NULLSOCKET) {
		/* If we don't already have a socket (from another
		 * service on same port) get a new one */
		socket = rxi_GetHostUDPSocket(host, port);
		if (socket == OSI_NULLSOCKET) {
		    USERPRI;
		    rxi_FreeService(tservice);
		    return 0;
		}
	    }
	    service = tservice;
	    service->socket = socket;
	    service->serviceHost = host;
	    service->servicePort = port;
	    service->serviceId = serviceId;
	    service->serviceName = serviceName;
	    service->nSecurityObjects = nSecurityObjects;
	    service->securityObjects = securityObjects;
	    service->minProcs = 0;
	    service->maxProcs = 1;
	    service->idleDeadTime = 60;
	    service->idleDeadErr = 0;
	    service->connDeadTime = rx_connDeadTime;
	    service->executeRequestProc = serviceProc;
	    service->checkReach = 0;
	    service->nSpecific = 0;
	    service->specific = NULL;
	    rx_services[i] = service;	/* not visible until now */
	    USERPRI;
	    return service;
	}
    }
    USERPRI;
    rxi_FreeService(tservice);
    (osi_Msg "rx_NewService: cannot support > %d services\n",
     RX_MAX_SERVICES);
    return 0;
}

/* Set configuration options for all of a service's security objects */

afs_int32
rx_SetSecurityConfiguration(struct rx_service *service,
			    rx_securityConfigVariables type,
			    void *value)
{
    int i;
    for (i = 0; i<service->nSecurityObjects; i++) {
	if (service->securityObjects[i]) {
	    RXS_SetConfiguration(service->securityObjects[i], NULL, type,
				 value, NULL);
	}
    }
    return 0;
}

struct rx_service *
rx_NewService(u_short port, u_short serviceId, char *serviceName,
	      struct rx_securityClass **securityObjects, int nSecurityObjects,
	      afs_int32(*serviceProc) (struct rx_call * acall))
{
    return rx_NewServiceHost(htonl(INADDR_ANY), port, serviceId, serviceName, securityObjects, nSecurityObjects, serviceProc);
}

/* Generic request processing loop. This routine should be called
 * by the implementation dependent rx_ServerProc. If socketp is
 * non-null, it will be set to the file descriptor that this thread
 * is now listening on. If socketp is null, this routine will never
 * returns. */
void
rxi_ServerProc(int threadID, struct rx_call *newcall, osi_socket * socketp)
{
    struct rx_call *call;
    afs_int32 code;
    struct rx_service *tservice = NULL;

    for (;;) {
	if (newcall) {
	    call = newcall;
	    newcall = NULL;
	} else {
	    call = rx_GetCall(threadID, tservice, socketp);
	    if (socketp && *socketp != OSI_NULLSOCKET) {
		/* We are now a listener thread */
		return;
	    }
	}

	/* if server is restarting( typically smooth shutdown) then do not
	 * allow any new calls.
	 */

	if (rx_tranquil && (call != NULL)) {
	    SPLVAR;

	    NETPRI;
	    MUTEX_ENTER(&call->lock);

	    rxi_CallError(call, RX_RESTARTING);
	    rxi_SendCallAbort(call, (struct rx_packet *)0, 0, 0);

	    MUTEX_EXIT(&call->lock);
	    USERPRI;
	}
#ifdef	KERNEL
	if (afs_termState == AFSOP_STOP_RXCALLBACK) {
#ifdef RX_ENABLE_LOCKS
	    AFS_GLOCK();
#endif /* RX_ENABLE_LOCKS */
	    afs_termState = AFSOP_STOP_AFS;
	    afs_osi_Wakeup(&afs_termState);
#ifdef RX_ENABLE_LOCKS
	    AFS_GUNLOCK();
#endif /* RX_ENABLE_LOCKS */
	    return;
	}
#endif

	tservice = call->conn->service;

	if (tservice->beforeProc)
	    (*tservice->beforeProc) (call);

	code = tservice->executeRequestProc(call);

	if (tservice->afterProc)
	    (*tservice->afterProc) (call, code);

	rx_EndCall(call, code);
        if (rx_stats_active) {
            MUTEX_ENTER(&rx_stats_mutex);
            rxi_nCalls++;
            MUTEX_EXIT(&rx_stats_mutex);
        }
    }
}


void
rx_WakeupServerProcs(void)
{
    struct rx_serverQueueEntry *np, *tqp;
    SPLVAR;

    NETPRI;
    MUTEX_ENTER(&rx_serverPool_lock);

#ifdef RX_ENABLE_LOCKS
    if (rx_waitForPacket)
	CV_BROADCAST(&rx_waitForPacket->cv);
#else /* RX_ENABLE_LOCKS */
    if (rx_waitForPacket)
	osi_rxWakeup(rx_waitForPacket);
#endif /* RX_ENABLE_LOCKS */
    MUTEX_ENTER(&freeSQEList_lock);
    for (np = rx_FreeSQEList; np; np = tqp) {
	tqp = *(struct rx_serverQueueEntry **)np;
#ifdef RX_ENABLE_LOCKS
	CV_BROADCAST(&np->cv);
#else /* RX_ENABLE_LOCKS */
	osi_rxWakeup(np);
#endif /* RX_ENABLE_LOCKS */
    }
    MUTEX_EXIT(&freeSQEList_lock);
    for (queue_Scan(&rx_idleServerQueue, np, tqp, rx_serverQueueEntry)) {
#ifdef RX_ENABLE_LOCKS
	CV_BROADCAST(&np->cv);
#else /* RX_ENABLE_LOCKS */
	osi_rxWakeup(np);
#endif /* RX_ENABLE_LOCKS */
    }
    MUTEX_EXIT(&rx_serverPool_lock);
    USERPRI;
}

/* meltdown:
 * One thing that seems to happen is that all the server threads get
 * tied up on some empty or slow call, and then a whole bunch of calls
 * arrive at once, using up the packet pool, so now there are more
 * empty calls.  The most critical resources here are server threads
 * and the free packet pool.  The "doreclaim" code seems to help in
 * general.  I think that eventually we arrive in this state: there
 * are lots of pending calls which do have all their packets present,
 * so they won't be reclaimed, are multi-packet calls, so they won't
 * be scheduled until later, and thus are tying up most of the free
 * packet pool for a very long time.
 * future options:
 * 1.  schedule multi-packet calls if all the packets are present.
 * Probably CPU-bound operation, useful to return packets to pool.
 * Do what if there is a full window, but the last packet isn't here?
 * 3.  preserve one thread which *only* runs "best" calls, otherwise
 * it sleeps and waits for that type of call.
 * 4.  Don't necessarily reserve a whole window for each thread.  In fact,
 * the current dataquota business is badly broken.  The quota isn't adjusted
 * to reflect how many packets are presently queued for a running call.
 * So, when we schedule a queued call with a full window of packets queued
 * up for it, that *should* free up a window full of packets for other 2d-class
 * calls to be able to use from the packet pool.  But it doesn't.
 *
 * NB.  Most of the time, this code doesn't run -- since idle server threads
 * sit on the idle server queue and are assigned by "...ReceivePacket" as soon
 * as a new call arrives.
 */
/* Sleep until a call arrives.  Returns a pointer to the call, ready
 * for an rx_Read. */
#ifdef RX_ENABLE_LOCKS
struct rx_call *
rx_GetCall(int tno, struct rx_service *cur_service, osi_socket * socketp)
{
    struct rx_serverQueueEntry *sq;
    struct rx_call *call = (struct rx_call *)0;
    struct rx_service *service = NULL;
    SPLVAR;

    MUTEX_ENTER(&freeSQEList_lock);

    if ((sq = rx_FreeSQEList)) {
	rx_FreeSQEList = *(struct rx_serverQueueEntry **)sq;
	MUTEX_EXIT(&freeSQEList_lock);
    } else {			/* otherwise allocate a new one and return that */
	MUTEX_EXIT(&freeSQEList_lock);
	sq = rxi_Alloc(sizeof(struct rx_serverQueueEntry));
	MUTEX_INIT(&sq->lock, "server Queue lock", MUTEX_DEFAULT, 0);
	CV_INIT(&sq->cv, "server Queue lock", CV_DEFAULT, 0);
    }

    MUTEX_ENTER(&rx_serverPool_lock);
    if (cur_service != NULL) {
	ReturnToServerPool(cur_service);
    }
    while (1) {
	if (queue_IsNotEmpty(&rx_incomingCallQueue)) {
	    struct rx_call *tcall, *ncall, *choice2 = NULL;

	    /* Scan for eligible incoming calls.  A call is not eligible
	     * if the maximum number of calls for its service type are
	     * already executing */
	    /* One thread will process calls FCFS (to prevent starvation),
	     * while the other threads may run ahead looking for calls which
	     * have all their input data available immediately.  This helps
	     * keep threads from blocking, waiting for data from the client. */
	    for (queue_Scan(&rx_incomingCallQueue, tcall, ncall, rx_call)) {
		service = tcall->conn->service;
		if (!QuotaOK(service)) {
		    continue;
		}
		MUTEX_ENTER(&rx_pthread_mutex);
		if (tno == rxi_fcfs_thread_num
		    || !tcall->queue_item_header.next) {
		    MUTEX_EXIT(&rx_pthread_mutex);
		    /* If we're the fcfs thread , then  we'll just use
		     * this call. If we haven't been able to find an optimal
		     * choice, and we're at the end of the list, then use a
		     * 2d choice if one has been identified.  Otherwise... */
		    call = (choice2 ? choice2 : tcall);
		    service = call->conn->service;
		} else {
		    MUTEX_EXIT(&rx_pthread_mutex);
		    if (!queue_IsEmpty(&tcall->rq)) {
			struct rx_packet *rp;
			rp = queue_First(&tcall->rq, rx_packet);
			if (rp->header.seq == 1) {
			    if (!meltdown_1pkt
				|| (rp->header.flags & RX_LAST_PACKET)) {
				call = tcall;
			    } else if (rxi_2dchoice && !choice2
				       && !(tcall->flags & RX_CALL_CLEARED)
				       && (tcall->rprev > rxi_HardAckRate)) {
				choice2 = tcall;
			    } else
				rxi_md2cnt++;
			}
		    }
		}
		if (call) {
		    break;
		} else {
		    ReturnToServerPool(service);
		}
	    }
	}

	if (call) {
	    queue_Remove(call);
	    MUTEX_EXIT(&rx_serverPool_lock);
	    MUTEX_ENTER(&call->lock);

	    if (call->flags & RX_CALL_WAIT_PROC) {
		call->flags &= ~RX_CALL_WAIT_PROC;
		rx_atomic_dec(&rx_nWaiting);
	    }

	    if (call->state != RX_STATE_PRECALL || call->error) {
		MUTEX_EXIT(&call->lock);
		MUTEX_ENTER(&rx_serverPool_lock);
		ReturnToServerPool(service);
		call = NULL;
		continue;
	    }

	    if (queue_IsEmpty(&call->rq)
		|| queue_First(&call->rq, rx_packet)->header.seq != 1)
		rxi_SendAck(call, 0, 0, RX_ACK_DELAY, 0);

	    CLEAR_CALL_QUEUE_LOCK(call);
	    break;
	} else {
	    /* If there are no eligible incoming calls, add this process
	     * to the idle server queue, to wait for one */
	    sq->newcall = 0;
	    sq->tno = tno;
	    if (socketp) {
		*socketp = OSI_NULLSOCKET;
	    }
	    sq->socketp = socketp;
	    queue_Append(&rx_idleServerQueue, sq);
#ifndef AFS_AIX41_ENV
	    rx_waitForPacket = sq;
#else
	    rx_waitingForPacket = sq;
#endif /* AFS_AIX41_ENV */
	    do {
		CV_WAIT(&sq->cv, &rx_serverPool_lock);
#ifdef	KERNEL
		if (afs_termState == AFSOP_STOP_RXCALLBACK) {
		    MUTEX_EXIT(&rx_serverPool_lock);
		    return (struct rx_call *)0;
		}
#endif
	    } while (!(call = sq->newcall)
		     && !(socketp && *socketp != OSI_NULLSOCKET));
	    MUTEX_EXIT(&rx_serverPool_lock);
	    if (call) {
		MUTEX_ENTER(&call->lock);
	    }
	    break;
	}
    }

    MUTEX_ENTER(&freeSQEList_lock);
    *(struct rx_serverQueueEntry **)sq = rx_FreeSQEList;
    rx_FreeSQEList = sq;
    MUTEX_EXIT(&freeSQEList_lock);

    if (call) {
	clock_GetTime(&call->startTime);
	call->state = RX_STATE_ACTIVE;
	call->mode = RX_MODE_RECEIVING;
#ifdef RX_KERNEL_TRACE
	if (ICL_SETACTIVE(afs_iclSetp)) {
	    int glockOwner = ISAFS_GLOCK();
	    if (!glockOwner)
		AFS_GLOCK();
	    afs_Trace3(afs_iclSetp, CM_TRACE_WASHERE, ICL_TYPE_STRING,
		       __FILE__, ICL_TYPE_INT32, __LINE__, ICL_TYPE_POINTER,
		       call);
	    if (!glockOwner)
		AFS_GUNLOCK();
	}
#endif

	rxi_calltrace(RX_CALL_START, call);
	dpf(("rx_GetCall(port=%d, service=%d) ==> call %"AFS_PTR_FMT"\n",
	     call->conn->service->servicePort, call->conn->service->serviceId,
	     call));

	MUTEX_EXIT(&call->lock);
        MUTEX_ENTER(&rx_refcnt_mutex);
	CALL_HOLD(call, RX_CALL_REFCOUNT_BEGIN);
        MUTEX_EXIT(&rx_refcnt_mutex);
    } else {
	dpf(("rx_GetCall(socketp=%p, *socketp=0x%x)\n", socketp, *socketp));
    }

    return call;
}
#else /* RX_ENABLE_LOCKS */
struct rx_call *
rx_GetCall(int tno, struct rx_service *cur_service, osi_socket * socketp)
{
    struct rx_serverQueueEntry *sq;
    struct rx_call *call = (struct rx_call *)0, *choice2;
    struct rx_service *service = NULL;
    SPLVAR;

    NETPRI;
    MUTEX_ENTER(&freeSQEList_lock);

    if ((sq = rx_FreeSQEList)) {
	rx_FreeSQEList = *(struct rx_serverQueueEntry **)sq;
	MUTEX_EXIT(&freeSQEList_lock);
    } else {			/* otherwise allocate a new one and return that */
	MUTEX_EXIT(&freeSQEList_lock);
	sq = rxi_Alloc(sizeof(struct rx_serverQueueEntry));
	MUTEX_INIT(&sq->lock, "server Queue lock", MUTEX_DEFAULT, 0);
	CV_INIT(&sq->cv, "server Queue lock", CV_DEFAULT, 0);
    }
    MUTEX_ENTER(&sq->lock);

    if (cur_service != NULL) {
	cur_service->nRequestsRunning--;
        MUTEX_ENTER(&rx_quota_mutex);
	if (cur_service->nRequestsRunning < cur_service->minProcs)
	    rxi_minDeficit++;
	rxi_availProcs++;
        MUTEX_EXIT(&rx_quota_mutex);
    }
    if (queue_IsNotEmpty(&rx_incomingCallQueue)) {
	struct rx_call *tcall, *ncall;
	/* Scan for eligible incoming calls.  A call is not eligible
	 * if the maximum number of calls for its service type are
	 * already executing */
	/* One thread will process calls FCFS (to prevent starvation),
	 * while the other threads may run ahead looking for calls which
	 * have all their input data available immediately.  This helps
	 * keep threads from blocking, waiting for data from the client. */
	choice2 = (struct rx_call *)0;
	for (queue_Scan(&rx_incomingCallQueue, tcall, ncall, rx_call)) {
	    service = tcall->conn->service;
	    if (QuotaOK(service)) {
		MUTEX_ENTER(&rx_pthread_mutex);
		if (tno == rxi_fcfs_thread_num
		    || !tcall->queue_item_header.next) {
		    MUTEX_EXIT(&rx_pthread_mutex);
		    /* If we're the fcfs thread, then  we'll just use
		     * this call. If we haven't been able to find an optimal
		     * choice, and we're at the end of the list, then use a
		     * 2d choice if one has been identified.  Otherwise... */
		    call = (choice2 ? choice2 : tcall);
		    service = call->conn->service;
		} else {
		    MUTEX_EXIT(&rx_pthread_mutex);
		    if (!queue_IsEmpty(&tcall->rq)) {
			struct rx_packet *rp;
			rp = queue_First(&tcall->rq, rx_packet);
			if (rp->header.seq == 1
			    && (!meltdown_1pkt
				|| (rp->header.flags & RX_LAST_PACKET))) {
			    call = tcall;
			} else if (rxi_2dchoice && !choice2
				   && !(tcall->flags & RX_CALL_CLEARED)
				   && (tcall->rprev > rxi_HardAckRate)) {
			    choice2 = tcall;
			} else
			    rxi_md2cnt++;
		    }
		}
	    }
	    if (call)
		break;
	}
    }

    if (call) {
	queue_Remove(call);
	/* we can't schedule a call if there's no data!!! */
	/* send an ack if there's no data, if we're missing the
	 * first packet, or we're missing something between first
	 * and last -- there's a "hole" in the incoming data. */
	if (queue_IsEmpty(&call->rq)
	    || queue_First(&call->rq, rx_packet)->header.seq != 1
	    || call->rprev != queue_Last(&call->rq, rx_packet)->header.seq)
	    rxi_SendAck(call, 0, 0, RX_ACK_DELAY, 0);

	call->flags &= (~RX_CALL_WAIT_PROC);
	service->nRequestsRunning++;
	/* just started call in minProcs pool, need fewer to maintain
	 * guarantee */
        MUTEX_ENTER(&rx_quota_mutex);
	if (service->nRequestsRunning <= service->minProcs)
	    rxi_minDeficit--;
	rxi_availProcs--;
        MUTEX_EXIT(&rx_quota_mutex);
	rx_atomic_dec(&rx_nWaiting);
	/* MUTEX_EXIT(&call->lock); */
    } else {
	/* If there are no eligible incoming calls, add this process
	 * to the idle server queue, to wait for one */
	sq->newcall = 0;
	if (socketp) {
	    *socketp = OSI_NULLSOCKET;
	}
	sq->socketp = socketp;
	queue_Append(&rx_idleServerQueue, sq);
	do {
	    osi_rxSleep(sq);
#ifdef	KERNEL
	    if (afs_termState == AFSOP_STOP_RXCALLBACK) {
		USERPRI;
		rxi_Free(sq, sizeof(struct rx_serverQueueEntry));
		return (struct rx_call *)0;
	    }
#endif
	} while (!(call = sq->newcall)
		 && !(socketp && *socketp != OSI_NULLSOCKET));
    }
    MUTEX_EXIT(&sq->lock);

    MUTEX_ENTER(&freeSQEList_lock);
    *(struct rx_serverQueueEntry **)sq = rx_FreeSQEList;
    rx_FreeSQEList = sq;
    MUTEX_EXIT(&freeSQEList_lock);

    if (call) {
	clock_GetTime(&call->startTime);
	call->state = RX_STATE_ACTIVE;
	call->mode = RX_MODE_RECEIVING;
#ifdef RX_KERNEL_TRACE
	if (ICL_SETACTIVE(afs_iclSetp)) {
	    int glockOwner = ISAFS_GLOCK();
	    if (!glockOwner)
		AFS_GLOCK();
	    afs_Trace3(afs_iclSetp, CM_TRACE_WASHERE, ICL_TYPE_STRING,
		       __FILE__, ICL_TYPE_INT32, __LINE__, ICL_TYPE_POINTER,
		       call);
	    if (!glockOwner)
		AFS_GUNLOCK();
	}
#endif

	rxi_calltrace(RX_CALL_START, call);
	dpf(("rx_GetCall(port=%d, service=%d) ==> call %p\n",
	     call->conn->service->servicePort, call->conn->service->serviceId,
	     call));
    } else {
	dpf(("rx_GetCall(socketp=%p, *socketp=0x%x)\n", socketp, *socketp));
    }

    USERPRI;

    return call;
}
#endif /* RX_ENABLE_LOCKS */



/* Establish a procedure to be called when a packet arrives for a
 * call.  This routine will be called at most once after each call,
 * and will also be called if there is an error condition on the or
 * the call is complete.  Used by multi rx to build a selection
 * function which determines which of several calls is likely to be a
 * good one to read from.
 * NOTE: the way this is currently implemented it is probably only a
 * good idea to (1) use it immediately after a newcall (clients only)
 * and (2) only use it once.  Other uses currently void your warranty
 */
void
rx_SetArrivalProc(struct rx_call *call,
		  void (*proc) (struct rx_call * call,
					void * mh,
					int index),
		  void * handle, int arg)
{
    call->arrivalProc = proc;
    call->arrivalProcHandle = handle;
    call->arrivalProcArg = arg;
}

/* Call is finished (possibly prematurely).  Return rc to the peer, if
 * appropriate, and return the final error code from the conversation
 * to the caller */

afs_int32
rx_EndCall(struct rx_call *call, afs_int32 rc)
{
    struct rx_connection *conn = call->conn;
    afs_int32 error;
    SPLVAR;

    dpf(("rx_EndCall(call %"AFS_PTR_FMT" rc %d error %d abortCode %d)\n",
          call, rc, call->error, call->abortCode));

    NETPRI;
    MUTEX_ENTER(&call->lock);

    if (rc == 0 && call->error == 0) {
	call->abortCode = 0;
	call->abortCount = 0;
    }

    call->arrivalProc = (void (*)())0;
    if (rc && call->error == 0) {
	rxi_CallError(call, rc);
	/* Send an abort message to the peer if this error code has
	 * only just been set.  If it was set previously, assume the
	 * peer has already been sent the error code or will request it
	 */
	rxi_SendCallAbort(call, (struct rx_packet *)0, 0, 0);
    }
    if (conn->type == RX_SERVER_CONNECTION) {
	/* Make sure reply or at least dummy reply is sent */
	if (call->mode == RX_MODE_RECEIVING) {
	    rxi_WriteProc(call, 0, 0);
	}
	if (call->mode == RX_MODE_SENDING) {
	    rxi_FlushWrite(call);
	}
	rxi_calltrace(RX_CALL_END, call);
	/* Call goes to hold state until reply packets are acknowledged */
	if (call->tfirst + call->nSoftAcked < call->tnext) {
	    call->state = RX_STATE_HOLD;
	} else {
	    call->state = RX_STATE_DALLY;
	    rxi_ClearTransmitQueue(call, 0);
	    rxevent_Cancel(call->resendEvent, call, RX_CALL_REFCOUNT_RESEND);
	    rxevent_Cancel(call->keepAliveEvent, call,
			   RX_CALL_REFCOUNT_ALIVE);
	}
    } else {			/* Client connection */
	char dummy;
	/* Make sure server receives input packets, in the case where
	 * no reply arguments are expected */
	if ((call->mode == RX_MODE_SENDING)
	    || (call->mode == RX_MODE_RECEIVING && call->rnext == 1)) {
	    (void)rxi_ReadProc(call, &dummy, 1);
	}

	/* If we had an outstanding delayed ack, be nice to the server
	 * and force-send it now.
	 */
	if (call->delayedAckEvent) {
	    rxevent_Cancel(call->delayedAckEvent, call,
			   RX_CALL_REFCOUNT_DELAY);
	    call->delayedAckEvent = NULL;
	    rxi_SendDelayedAck(NULL, call, NULL);
	}

	/* We need to release the call lock since it's lower than the
	 * conn_call_lock and we don't want to hold the conn_call_lock
	 * over the rx_ReadProc call. The conn_call_lock needs to be held
	 * here for the case where rx_NewCall is perusing the calls on
	 * the connection structure. We don't want to signal until
	 * rx_NewCall is in a stable state. Otherwise, rx_NewCall may
	 * have checked this call, found it active and by the time it
	 * goes to sleep, will have missed the signal.
	 */
        MUTEX_EXIT(&call->lock);
        MUTEX_ENTER(&conn->conn_call_lock);
        MUTEX_ENTER(&call->lock);
	MUTEX_ENTER(&conn->conn_data_lock);
	conn->flags |= RX_CONN_BUSY;
	if (conn->flags & RX_CONN_MAKECALL_WAITING) {
	    MUTEX_EXIT(&conn->conn_data_lock);
#ifdef	RX_ENABLE_LOCKS
	    CV_BROADCAST(&conn->conn_call_cv);
#else
	    osi_rxWakeup(conn);
#endif
	}
#ifdef RX_ENABLE_LOCKS
	else {
	    MUTEX_EXIT(&conn->conn_data_lock);
	}
#endif /* RX_ENABLE_LOCKS */
	call->state = RX_STATE_DALLY;
    }
    error = call->error;

    /* currentPacket, nLeft, and NFree must be zeroed here, because
     * ResetCall cannot: ResetCall may be called at splnet(), in the
     * kernel version, and may interrupt the macros rx_Read or
     * rx_Write, which run at normal priority for efficiency. */
    if (call->currentPacket) {
#ifdef RX_TRACK_PACKETS
        call->currentPacket->flags &= ~RX_PKTFLAG_CP;
#endif
	rxi_FreePacket(call->currentPacket);
	call->currentPacket = (struct rx_packet *)0;
    }

    call->nLeft = call->nFree = call->curlen = 0;

    /* Free any packets from the last call to ReadvProc/WritevProc */
#ifdef RXDEBUG_PACKET
    call->iovqc -=
#endif /* RXDEBUG_PACKET */
        rxi_FreePackets(0, &call->iovq);
    MUTEX_EXIT(&call->lock);

    MUTEX_ENTER(&rx_refcnt_mutex);
    CALL_RELE(call, RX_CALL_REFCOUNT_BEGIN);
    MUTEX_EXIT(&rx_refcnt_mutex);
    if (conn->type == RX_CLIENT_CONNECTION) {
	MUTEX_ENTER(&conn->conn_data_lock);
	conn->flags &= ~RX_CONN_BUSY;
	MUTEX_EXIT(&conn->conn_data_lock);
        MUTEX_EXIT(&conn->conn_call_lock);
    }
    USERPRI;
    /*
     * Map errors to the local host's errno.h format.
     */
    error = ntoh_syserr_conv(error);
    return error;
}

#if !defined(KERNEL)

/* Call this routine when shutting down a server or client (especially
 * clients).  This will allow Rx to gracefully garbage collect server
 * connections, and reduce the number of retries that a server might
 * make to a dead client.
 * This is not quite right, since some calls may still be ongoing and
 * we can't lock them to destroy them. */
void
rx_Finalize(void)
{
    struct rx_connection **conn_ptr, **conn_end;

    INIT_PTHREAD_LOCKS;
    LOCK_RX_INIT;
    if (rxinit_status == 1) {
	UNLOCK_RX_INIT;
	return;			/* Already shutdown. */
    }
    rxi_DeleteCachedConnections();
    if (rx_connHashTable) {
	MUTEX_ENTER(&rx_connHashTable_lock);
	for (conn_ptr = &rx_connHashTable[0], conn_end =
	     &rx_connHashTable[rx_hashTableSize]; conn_ptr < conn_end;
	     conn_ptr++) {
	    struct rx_connection *conn, *next;
	    for (conn = *conn_ptr; conn; conn = next) {
		next = conn->next;
		if (conn->type == RX_CLIENT_CONNECTION) {
                    MUTEX_ENTER(&rx_refcnt_mutex);
		    conn->refCount++;
                    MUTEX_EXIT(&rx_refcnt_mutex);
#ifdef RX_ENABLE_LOCKS
		    rxi_DestroyConnectionNoLock(conn);
#else /* RX_ENABLE_LOCKS */
		    rxi_DestroyConnection(conn);
#endif /* RX_ENABLE_LOCKS */
		}
	    }
	}
#ifdef RX_ENABLE_LOCKS
	while (rx_connCleanup_list) {
	    struct rx_connection *conn;
	    conn = rx_connCleanup_list;
	    rx_connCleanup_list = rx_connCleanup_list->next;
	    MUTEX_EXIT(&rx_connHashTable_lock);
	    rxi_CleanupConnection(conn);
	    MUTEX_ENTER(&rx_connHashTable_lock);
	}
	MUTEX_EXIT(&rx_connHashTable_lock);
#endif /* RX_ENABLE_LOCKS */
    }
    rxi_flushtrace();

#ifdef AFS_NT40_ENV
    afs_winsockCleanup();
#endif

    rxinit_status = 1;
    UNLOCK_RX_INIT;
}
#endif

/* if we wakeup packet waiter too often, can get in loop with two
    AllocSendPackets each waking each other up (from ReclaimPacket calls) */
void
rxi_PacketsUnWait(void)
{
    if (!rx_waitingForPackets) {
	return;
    }
#ifdef KERNEL
    if (rxi_OverQuota(RX_PACKET_CLASS_SEND)) {
	return;			/* still over quota */
    }
#endif /* KERNEL */
    rx_waitingForPackets = 0;
#ifdef	RX_ENABLE_LOCKS
    CV_BROADCAST(&rx_waitingForPackets_cv);
#else
    osi_rxWakeup(&rx_waitingForPackets);
#endif
    return;
}


/* ------------------Internal interfaces------------------------- */

/* Return this process's service structure for the
 * specified socket and service */
struct rx_service *
rxi_FindService(osi_socket socket, u_short serviceId)
{
    struct rx_service **sp;
    for (sp = &rx_services[0]; *sp; sp++) {
	if ((*sp)->serviceId == serviceId && (*sp)->socket == socket)
	    return *sp;
    }
    return 0;
}

#ifdef RXDEBUG_PACKET
#ifdef KDUMP_RX_LOCK
static struct rx_call_rx_lock *rx_allCallsp = 0;
#else
static struct rx_call *rx_allCallsp = 0;
#endif
#endif /* RXDEBUG_PACKET */

/* Allocate a call structure, for the indicated channel of the
 * supplied connection.  The mode and state of the call must be set by
 * the caller. Returns the call with mutex locked. */
struct rx_call *
rxi_NewCall(struct rx_connection *conn, int channel)
{
    struct rx_call *call;
#ifdef	AFS_GLOBAL_RXLOCK_KERNEL
    struct rx_call *cp;	/* Call pointer temp */
    struct rx_call *nxp;	/* Next call pointer, for queue_Scan */
#endif /* AFS_GLOBAL_RXLOCK_KERNEL */

    dpf(("rxi_NewCall(conn %"AFS_PTR_FMT", channel %d)\n", conn, channel));

    /* Grab an existing call structure, or allocate a new one.
     * Existing call structures are assumed to have been left reset by
     * rxi_FreeCall */
    MUTEX_ENTER(&rx_freeCallQueue_lock);

#ifdef	AFS_GLOBAL_RXLOCK_KERNEL
    /*
     * EXCEPT that the TQ might not yet be cleared out.
     * Skip over those with in-use TQs.
     */
    call = NULL;
    for (queue_Scan(&rx_freeCallQueue, cp, nxp, rx_call)) {
	if (!(cp->flags & RX_CALL_TQ_BUSY)) {
	    call = cp;
	    break;
	}
    }
    if (call) {
#else /* AFS_GLOBAL_RXLOCK_KERNEL */
    if (queue_IsNotEmpty(&rx_freeCallQueue)) {
	call = queue_First(&rx_freeCallQueue, rx_call);
#endif /* AFS_GLOBAL_RXLOCK_KERNEL */
	queue_Remove(call);
        if (rx_stats_active)
	    rx_atomic_dec(&rx_stats.nFreeCallStructs);
	MUTEX_EXIT(&rx_freeCallQueue_lock);
	MUTEX_ENTER(&call->lock);
	CLEAR_CALL_QUEUE_LOCK(call);
#ifdef	AFS_GLOBAL_RXLOCK_KERNEL
	/* Now, if TQ wasn't cleared earlier, do it now. */
	rxi_WaitforTQBusy(call);
	if (call->flags & RX_CALL_TQ_CLEARME) {
	    rxi_ClearTransmitQueue(call, 1);
	    /*queue_Init(&call->tq);*/
	}
#endif /* AFS_GLOBAL_RXLOCK_KERNEL */
	/* Bind the call to its connection structure */
	call->conn = conn;
	rxi_ResetCall(call, 1);
    } else {

	call = rxi_Alloc(sizeof(struct rx_call));
#ifdef RXDEBUG_PACKET
        call->allNextp = rx_allCallsp;
        rx_allCallsp = call;
        call->call_id =
	    rx_atomic_inc_and_read(&rx_stats.nCallStructs);
#else /* RXDEBUG_PACKET */
        rx_atomic_inc(&rx_stats.nCallStructs);
#endif /* RXDEBUG_PACKET */

        MUTEX_EXIT(&rx_freeCallQueue_lock);
	MUTEX_INIT(&call->lock, "call lock", MUTEX_DEFAULT, NULL);
	MUTEX_ENTER(&call->lock);
	CV_INIT(&call->cv_twind, "call twind", CV_DEFAULT, 0);
	CV_INIT(&call->cv_rq, "call rq", CV_DEFAULT, 0);
	CV_INIT(&call->cv_tq, "call tq", CV_DEFAULT, 0);

	/* Initialize once-only items */
	queue_Init(&call->tq);
	queue_Init(&call->rq);
	queue_Init(&call->iovq);
#ifdef RXDEBUG_PACKET
        call->rqc = call->tqc = call->iovqc = 0;
#endif /* RXDEBUG_PACKET */
	/* Bind the call to its connection structure (prereq for reset) */
	call->conn = conn;
	rxi_ResetCall(call, 1);
    }
    call->channel = channel;
    call->callNumber = &conn->callNumber[channel];
    call->rwind = conn->rwind[channel];
    call->twind = conn->twind[channel];
    /* Note that the next expected call number is retained (in
     * conn->callNumber[i]), even if we reallocate the call structure
     */
    conn->call[channel] = call;
    /* if the channel's never been used (== 0), we should start at 1, otherwise
     * the call number is valid from the last time this channel was used */
    if (*call->callNumber == 0)
	*call->callNumber = 1;

    return call;
}

/* A call has been inactive long enough that so we can throw away
 * state, including the call structure, which is placed on the call
 * free list.
 *
 * call->lock amd rx_refcnt_mutex are held upon entry.
 * haveCTLock is set when called from rxi_ReapConnections.
 */
void
rxi_FreeCall(struct rx_call *call, int haveCTLock)
{
    int channel = call->channel;
    struct rx_connection *conn = call->conn;


    if (call->state == RX_STATE_DALLY || call->state == RX_STATE_HOLD)
	(*call->callNumber)++;
    rxi_ResetCall(call, 0);
    call->conn->call[channel] = (struct rx_call *)0;
    MUTEX_EXIT(&rx_refcnt_mutex);

    MUTEX_ENTER(&rx_freeCallQueue_lock);
    SET_CALL_QUEUE_LOCK(call, &rx_freeCallQueue_lock);
#ifdef AFS_GLOBAL_RXLOCK_KERNEL
    /* A call may be free even though its transmit queue is still in use.
     * Since we search the call list from head to tail, put busy calls at
     * the head of the list, and idle calls at the tail.
     */
    if (call->flags & RX_CALL_TQ_BUSY)
	queue_Prepend(&rx_freeCallQueue, call);
    else
	queue_Append(&rx_freeCallQueue, call);
#else /* AFS_GLOBAL_RXLOCK_KERNEL */
    queue_Append(&rx_freeCallQueue, call);
#endif /* AFS_GLOBAL_RXLOCK_KERNEL */
    if (rx_stats_active)
	rx_atomic_inc(&rx_stats.nFreeCallStructs);
    MUTEX_EXIT(&rx_freeCallQueue_lock);

    /* Destroy the connection if it was previously slated for
     * destruction, i.e. the Rx client code previously called
     * rx_DestroyConnection (client connections), or
     * rxi_ReapConnections called the same routine (server
     * connections).  Only do this, however, if there are no
     * outstanding calls. Note that for fine grain locking, there appears
     * to be a deadlock in that rxi_FreeCall has a call locked and
     * DestroyConnectionNoLock locks each call in the conn. But note a
     * few lines up where we have removed this call from the conn.
     * If someone else destroys a connection, they either have no
     * call lock held or are going through this section of code.
     */
    MUTEX_ENTER(&conn->conn_data_lock);
    if (conn->flags & RX_CONN_DESTROY_ME && !(conn->flags & RX_CONN_MAKECALL_WAITING)) {
        MUTEX_ENTER(&rx_refcnt_mutex);
	conn->refCount++;
        MUTEX_EXIT(&rx_refcnt_mutex);
	MUTEX_EXIT(&conn->conn_data_lock);
#ifdef RX_ENABLE_LOCKS
	if (haveCTLock)
	    rxi_DestroyConnectionNoLock(conn);
	else
	    rxi_DestroyConnection(conn);
#else /* RX_ENABLE_LOCKS */
	rxi_DestroyConnection(conn);
#endif /* RX_ENABLE_LOCKS */
    } else {
	MUTEX_EXIT(&conn->conn_data_lock);
    }
    MUTEX_ENTER(&rx_refcnt_mutex);
}

rx_atomic_t rxi_Allocsize = RX_ATOMIC_INIT(0);
rx_atomic_t rxi_Alloccnt = RX_ATOMIC_INIT(0);

void *
rxi_Alloc(size_t size)
{
    char *p;

    if (rx_stats_active) {
	rx_atomic_add(&rxi_Allocsize, (int) size);
	rx_atomic_inc(&rxi_Alloccnt);
    }

p = (char *)
#if defined(KERNEL) && !defined(UKERNEL) && defined(AFS_FBSD80_ENV)
  afs_osi_Alloc_NoSleep(size);
#else
  osi_Alloc(size);
#endif
    if (!p)
	osi_Panic("rxi_Alloc error");
    memset(p, 0, size);
    return p;
}

void
rxi_Free(void *addr, size_t size)
{
    if (rx_stats_active) {
	rx_atomic_sub(&rxi_Allocsize, (int) size);
        rx_atomic_dec(&rxi_Alloccnt);
    }
    osi_Free(addr, size);
}

void
rxi_SetPeerMtu(struct rx_peer *peer, afs_uint32 host, afs_uint32 port, int mtu)
{
    struct rx_peer **peer_ptr = NULL, **peer_end = NULL;
    struct rx_peer *next = NULL;
    int hashIndex;

    if (!peer) {
	MUTEX_ENTER(&rx_peerHashTable_lock);
	if (port == 0) {
	    peer_ptr = &rx_peerHashTable[0];
	    peer_end = &rx_peerHashTable[rx_hashTableSize];
	    next = NULL;
	resume:
	    for ( ; peer_ptr < peer_end; peer_ptr++) {
		if (!peer)
		    peer = *peer_ptr;
		for ( ; peer; peer = next) {
		    next = peer->next;
		    if (host == peer->host)
			break;
		}
	    }
	} else {
	    hashIndex = PEER_HASH(host, port);
	    for (peer = rx_peerHashTable[hashIndex]; peer; peer = peer->next) {
		if ((peer->host == host) && (peer->port == port))
		    break;
	    }
	}
    } else {
	MUTEX_ENTER(&rx_peerHashTable_lock);
    }

    if (peer) {
        peer->refCount++;
        MUTEX_EXIT(&rx_peerHashTable_lock);

        MUTEX_ENTER(&peer->peer_lock);
	/* We don't handle dropping below min, so don't */
	mtu = MAX(mtu, RX_MIN_PACKET_SIZE);
        peer->ifMTU=MIN(mtu, peer->ifMTU);
        peer->natMTU = rxi_AdjustIfMTU(peer->ifMTU);
	/* if we tweaked this down, need to tune our peer MTU too */
	peer->MTU = MIN(peer->MTU, peer->natMTU);
	/* if we discovered a sub-1500 mtu, degrade */
	if (peer->ifMTU < OLD_MAX_PACKET_SIZE)
	    peer->maxDgramPackets = 1;
	/* We no longer have valid peer packet information */
	if (peer->maxPacketSize-RX_IPUDP_SIZE > peer->ifMTU)
	    peer->maxPacketSize = 0;
        MUTEX_EXIT(&peer->peer_lock);

        MUTEX_ENTER(&rx_peerHashTable_lock);
        peer->refCount--;
        if (host && !port) {
            peer = next;
	    /* pick up where we left off */
            goto resume;
        }
    }
    MUTEX_EXIT(&rx_peerHashTable_lock);
}

/* Find the peer process represented by the supplied (host,port)
 * combination.  If there is no appropriate active peer structure, a
 * new one will be allocated and initialized
 * The origPeer, if set, is a pointer to a peer structure on which the
 * refcount will be be decremented. This is used to replace the peer
 * structure hanging off a connection structure */
struct rx_peer *
rxi_FindPeer(afs_uint32 host, u_short port,
	     struct rx_peer *origPeer, int create)
{
    struct rx_peer *pp;
    int hashIndex;
    hashIndex = PEER_HASH(host, port);
    MUTEX_ENTER(&rx_peerHashTable_lock);
    for (pp = rx_peerHashTable[hashIndex]; pp; pp = pp->next) {
	if ((pp->host == host) && (pp->port == port))
	    break;
    }
    if (!pp) {
	if (create) {
	    pp = rxi_AllocPeer();	/* This bzero's *pp */
	    pp->host = host;	/* set here or in InitPeerParams is zero */
	    pp->port = port;
	    MUTEX_INIT(&pp->peer_lock, "peer_lock", MUTEX_DEFAULT, 0);
	    queue_Init(&pp->congestionQueue);
	    queue_Init(&pp->rpcStats);
	    pp->next = rx_peerHashTable[hashIndex];
	    rx_peerHashTable[hashIndex] = pp;
	    rxi_InitPeerParams(pp);
            if (rx_stats_active)
		rx_atomic_inc(&rx_stats.nPeerStructs);
	}
    }
    if (pp && create) {
	pp->refCount++;
    }
    if (origPeer)
	origPeer->refCount--;
    MUTEX_EXIT(&rx_peerHashTable_lock);
    return pp;
}


/* Find the connection at (host, port) started at epoch, and with the
 * given connection id.  Creates the server connection if necessary.
 * The type specifies whether a client connection or a server
 * connection is desired.  In both cases, (host, port) specify the
 * peer's (host, pair) pair.  Client connections are not made
 * automatically by this routine.  The parameter socket gives the
 * socket descriptor on which the packet was received.  This is used,
 * in the case of server connections, to check that *new* connections
 * come via a valid (port, serviceId).  Finally, the securityIndex
 * parameter must match the existing index for the connection.  If a
 * server connection is created, it will be created using the supplied
 * index, if the index is valid for this service */
struct rx_connection *
rxi_FindConnection(osi_socket socket, afs_uint32 host,
		   u_short port, u_short serviceId, afs_uint32 cid,
		   afs_uint32 epoch, int type, u_int securityIndex)
{
    int hashindex, flag, i;
    struct rx_connection *conn;
    hashindex = CONN_HASH(host, port, cid, epoch, type);
    MUTEX_ENTER(&rx_connHashTable_lock);
    rxLastConn ? (conn = rxLastConn, flag = 0) : (conn =
						  rx_connHashTable[hashindex],
						  flag = 1);
    for (; conn;) {
	if ((conn->type == type) && ((cid & RX_CIDMASK) == conn->cid)
	    && (epoch == conn->epoch)) {
	    struct rx_peer *pp = conn->peer;
	    if (securityIndex != conn->securityIndex) {
		/* this isn't supposed to happen, but someone could forge a packet
		 * like this, and there seems to be some CM bug that makes this
		 * happen from time to time -- in which case, the fileserver
		 * asserts. */
		MUTEX_EXIT(&rx_connHashTable_lock);
		return (struct rx_connection *)0;
	    }
	    if (pp->host == host && pp->port == port)
		break;
	    if (type == RX_CLIENT_CONNECTION && pp->port == port)
		break;
	    /* So what happens when it's a callback connection? */
	    if (		/*type == RX_CLIENT_CONNECTION && */
		   (conn->epoch & 0x80000000))
		break;
	}
	if (!flag) {
	    /* the connection rxLastConn that was used the last time is not the
	     ** one we are looking for now. Hence, start searching in the hash */
	    flag = 1;
	    conn = rx_connHashTable[hashindex];
	} else
	    conn = conn->next;
    }
    if (!conn) {
	struct rx_service *service;
	if (type == RX_CLIENT_CONNECTION) {
	    MUTEX_EXIT(&rx_connHashTable_lock);
	    return (struct rx_connection *)0;
	}
	service = rxi_FindService(socket, serviceId);
	if (!service || (securityIndex >= service->nSecurityObjects)
	    || (service->securityObjects[securityIndex] == 0)) {
	    MUTEX_EXIT(&rx_connHashTable_lock);
	    return (struct rx_connection *)0;
	}
	conn = rxi_AllocConnection();	/* This bzero's the connection */
	MUTEX_INIT(&conn->conn_call_lock, "conn call lock", MUTEX_DEFAULT, 0);
	MUTEX_INIT(&conn->conn_data_lock, "conn data lock", MUTEX_DEFAULT, 0);
	CV_INIT(&conn->conn_call_cv, "conn call cv", CV_DEFAULT, 0);
	conn->next = rx_connHashTable[hashindex];
	rx_connHashTable[hashindex] = conn;
	conn->peer = rxi_FindPeer(host, port, 0, 1);
	conn->type = RX_SERVER_CONNECTION;
	conn->lastSendTime = clock_Sec();	/* don't GC immediately */
	conn->epoch = epoch;
	conn->cid = cid & RX_CIDMASK;
	/* conn->serial = conn->lastSerial = 0; */
	/* conn->timeout = 0; */
	conn->ackRate = RX_FAST_ACK_RATE;
	conn->service = service;
	conn->serviceId = serviceId;
	conn->securityIndex = securityIndex;
	conn->securityObject = service->securityObjects[securityIndex];
	conn->nSpecific = 0;
	conn->specific = NULL;
	rx_SetConnDeadTime(conn, service->connDeadTime);
	rx_SetConnIdleDeadTime(conn, service->idleDeadTime);
	rx_SetServerConnIdleDeadErr(conn, service->idleDeadErr);
	for (i = 0; i < RX_MAXCALLS; i++) {
	    conn->twind[i] = rx_initSendWindow;
	    conn->rwind[i] = rx_initReceiveWindow;
	}
	/* Notify security object of the new connection */
	RXS_NewConnection(conn->securityObject, conn);
	/* XXXX Connection timeout? */
	if (service->newConnProc)
	    (*service->newConnProc) (conn);
        if (rx_stats_active)
            rx_atomic_inc(&rx_stats.nServerConns);
    }

    MUTEX_ENTER(&rx_refcnt_mutex);
    conn->refCount++;
    MUTEX_EXIT(&rx_refcnt_mutex);

    rxLastConn = conn;		/* store this connection as the last conn used */
    MUTEX_EXIT(&rx_connHashTable_lock);
    return conn;
}

/* There are two packet tracing routines available for testing and monitoring
 * Rx.  One is called just after every packet is received and the other is
 * called just before every packet is sent.  Received packets, have had their
 * headers decoded, and packets to be sent have not yet had their headers
 * encoded.  Both take two parameters: a pointer to the packet and a sockaddr
 * containing the network address.  Both can be modified.  The return value, if
 * non-zero, indicates that the packet should be dropped.  */

int (*rx_justReceived) (struct rx_packet *, struct sockaddr_in *) = 0;
int (*rx_almostSent) (struct rx_packet *, struct sockaddr_in *) = 0;

/* A packet has been received off the interface.  Np is the packet, socket is
 * the socket number it was received from (useful in determining which service
 * this packet corresponds to), and (host, port) reflect the host,port of the
 * sender.  This call returns the packet to the caller if it is finished with
 * it, rather than de-allocating it, just as a small performance hack */

struct rx_packet *
rxi_ReceivePacket(struct rx_packet *np, osi_socket socket,
		  afs_uint32 host, u_short port, int *tnop,
		  struct rx_call **newcallp)
{
    struct rx_call *call;
    struct rx_connection *conn;
    int channel;
    afs_uint32 currentCallNumber;
    int type;
    int skew;
#ifdef RXDEBUG
    char *packetType;
#endif
    struct rx_packet *tnp;

#ifdef RXDEBUG
/* We don't print out the packet until now because (1) the time may not be
 * accurate enough until now in the lwp implementation (rx_Listener only gets
 * the time after the packet is read) and (2) from a protocol point of view,
 * this is the first time the packet has been seen */
    packetType = (np->header.type > 0 && np->header.type < RX_N_PACKET_TYPES)
	? rx_packetTypes[np->header.type - 1] : "*UNKNOWN*";
    dpf(("R %d %s: %x.%d.%d.%d.%d.%d.%d flags %d, packet %"AFS_PTR_FMT,
	 np->header.serial, packetType, ntohl(host), ntohs(port), np->header.serviceId,
	 np->header.epoch, np->header.cid, np->header.callNumber,
	 np->header.seq, np->header.flags, np));
#endif

    if (np->header.type == RX_PACKET_TYPE_VERSION) {
	return rxi_ReceiveVersionPacket(np, socket, host, port, 1);
    }

    if (np->header.type == RX_PACKET_TYPE_DEBUG) {
	return rxi_ReceiveDebugPacket(np, socket, host, port, 1);
    }
#ifdef RXDEBUG
    /* If an input tracer function is defined, call it with the packet and
     * network address.  Note this function may modify its arguments. */
    if (rx_justReceived) {
	struct sockaddr_in addr;
	int drop;
	addr.sin_family = AF_INET;
	addr.sin_port = port;
	addr.sin_addr.s_addr = host;
#ifdef STRUCT_SOCKADDR_HAS_SA_LEN
	addr.sin_len = sizeof(addr);
#endif /* AFS_OSF_ENV */
	drop = (*rx_justReceived) (np, &addr);
	/* drop packet if return value is non-zero */
	if (drop)
	    return np;
	port = addr.sin_port;	/* in case fcn changed addr */
	host = addr.sin_addr.s_addr;
    }
#endif

    /* If packet was not sent by the client, then *we* must be the client */
    type = ((np->header.flags & RX_CLIENT_INITIATED) != RX_CLIENT_INITIATED)
	? RX_CLIENT_CONNECTION : RX_SERVER_CONNECTION;

    /* Find the connection (or fabricate one, if we're the server & if
     * necessary) associated with this packet */
    conn =
	rxi_FindConnection(socket, host, port, np->header.serviceId,
			   np->header.cid, np->header.epoch, type,
			   np->header.securityIndex);

    if (!conn) {
	/* If no connection found or fabricated, just ignore the packet.
	 * (An argument could be made for sending an abort packet for
	 * the conn) */
	return np;
    }

    MUTEX_ENTER(&conn->conn_data_lock);
    if (conn->maxSerial < np->header.serial)
	conn->maxSerial = np->header.serial;
    MUTEX_EXIT(&conn->conn_data_lock);

    /* If the connection is in an error state, send an abort packet and ignore
     * the incoming packet */
    if (conn->error) {
	/* Don't respond to an abort packet--we don't want loops! */
	MUTEX_ENTER(&conn->conn_data_lock);
	if (np->header.type != RX_PACKET_TYPE_ABORT)
	    np = rxi_SendConnectionAbort(conn, np, 1, 0);
        MUTEX_ENTER(&rx_refcnt_mutex);
	conn->refCount--;
        MUTEX_EXIT(&rx_refcnt_mutex);
	MUTEX_EXIT(&conn->conn_data_lock);
	return np;
    }

    /* Check for connection-only requests (i.e. not call specific). */
    if (np->header.callNumber == 0) {
	switch (np->header.type) {
	case RX_PACKET_TYPE_ABORT: {
	    /* What if the supplied error is zero? */
	    afs_int32 errcode = ntohl(rx_GetInt32(np, 0));
	    dpf(("rxi_ReceivePacket ABORT rx_GetInt32 = %d", errcode));
	    rxi_ConnectionError(conn, errcode);
            MUTEX_ENTER(&rx_refcnt_mutex);
	    conn->refCount--;
            MUTEX_EXIT(&rx_refcnt_mutex);
	    return np;
	}
	case RX_PACKET_TYPE_CHALLENGE:
	    tnp = rxi_ReceiveChallengePacket(conn, np, 1);
            MUTEX_ENTER(&rx_refcnt_mutex);
	    conn->refCount--;
            MUTEX_EXIT(&rx_refcnt_mutex);
	    return tnp;
	case RX_PACKET_TYPE_RESPONSE:
	    tnp = rxi_ReceiveResponsePacket(conn, np, 1);
            MUTEX_ENTER(&rx_refcnt_mutex);
	    conn->refCount--;
            MUTEX_EXIT(&rx_refcnt_mutex);
	    return tnp;
	case RX_PACKET_TYPE_PARAMS:
	case RX_PACKET_TYPE_PARAMS + 1:
	case RX_PACKET_TYPE_PARAMS + 2:
	    /* ignore these packet types for now */
            MUTEX_ENTER(&rx_refcnt_mutex);
	    conn->refCount--;
            MUTEX_EXIT(&rx_refcnt_mutex);
	    return np;


	default:
	    /* Should not reach here, unless the peer is broken: send an
	     * abort packet */
	    rxi_ConnectionError(conn, RX_PROTOCOL_ERROR);
	    MUTEX_ENTER(&conn->conn_data_lock);
	    tnp = rxi_SendConnectionAbort(conn, np, 1, 0);
            MUTEX_ENTER(&rx_refcnt_mutex);
	    conn->refCount--;
            MUTEX_EXIT(&rx_refcnt_mutex);
	    MUTEX_EXIT(&conn->conn_data_lock);
	    return tnp;
	}
    }

    channel = np->header.cid & RX_CHANNELMASK;
    call = conn->call[channel];
#ifdef	RX_ENABLE_LOCKS
    if (call)
	MUTEX_ENTER(&call->lock);
    /* Test to see if call struct is still attached to conn. */
    if (call != conn->call[channel]) {
	if (call)
	    MUTEX_EXIT(&call->lock);
	if (type == RX_SERVER_CONNECTION) {
	    call = conn->call[channel];
	    /* If we started with no call attached and there is one now,
	     * another thread is also running this routine and has gotten
	     * the connection channel. We should drop this packet in the tests
	     * below. If there was a call on this connection and it's now
	     * gone, then we'll be making a new call below.
	     * If there was previously a call and it's now different then
	     * the old call was freed and another thread running this routine
	     * has created a call on this channel. One of these two threads
	     * has a packet for the old call and the code below handles those
	     * cases.
	     */
	    if (call)
		MUTEX_ENTER(&call->lock);
	} else {
	    /* This packet can't be for this call. If the new call address is
	     * 0 then no call is running on this channel. If there is a call
	     * then, since this is a client connection we're getting data for
	     * it must be for the previous call.
	     */
            if (rx_stats_active)
		rx_atomic_inc(&rx_stats.spuriousPacketsRead);
            MUTEX_ENTER(&rx_refcnt_mutex);
	    conn->refCount--;
            MUTEX_EXIT(&rx_refcnt_mutex);
	    return np;
	}
    }
#endif
    currentCallNumber = conn->callNumber[channel];

    if (type == RX_SERVER_CONNECTION) {	/* We're the server */
	if (np->header.callNumber < currentCallNumber) {
            if (rx_stats_active)
		rx_atomic_inc(&rx_stats.spuriousPacketsRead);
#ifdef	RX_ENABLE_LOCKS
	    if (call)
		MUTEX_EXIT(&call->lock);
#endif
            MUTEX_ENTER(&rx_refcnt_mutex);
	    conn->refCount--;
            MUTEX_EXIT(&rx_refcnt_mutex);
	    return np;
	}
	if (!call) {
	    MUTEX_ENTER(&conn->conn_call_lock);
	    call = rxi_NewCall(conn, channel);
	    MUTEX_EXIT(&conn->conn_call_lock);
	    *call->callNumber = np->header.callNumber;
#ifdef RXDEBUG
	    if (np->header.callNumber == 0)
		dpf(("RecPacket call 0 %d %s: %x.%u.%u.%u.%u.%u.%u flags %d, packet %"AFS_PTR_FMT" resend %d.%.06d len %d",
                      np->header.serial, rx_packetTypes[np->header.type - 1], ntohl(conn->peer->host), ntohs(conn->peer->port),
                      np->header.serial, np->header.epoch, np->header.cid, np->header.callNumber, np->header.seq,
                      np->header.flags, np, np->retryTime.sec, np->retryTime.usec / 1000, np->length));
#endif
	    call->state = RX_STATE_PRECALL;
	    clock_GetTime(&call->queueTime);
	    hzero(call->bytesSent);
	    hzero(call->bytesRcvd);
	    /*
	     * If the number of queued calls exceeds the overload
	     * threshold then abort this call.
	     */
	    if ((rx_BusyThreshold > 0) &&
		(rx_atomic_read(&rx_nWaiting) > rx_BusyThreshold)) {
		struct rx_packet *tp;

		rxi_CallError(call, rx_BusyError);
		tp = rxi_SendCallAbort(call, np, 1, 0);
		MUTEX_EXIT(&call->lock);
                MUTEX_ENTER(&rx_refcnt_mutex);
		conn->refCount--;
                MUTEX_EXIT(&rx_refcnt_mutex);
                if (rx_stats_active)
                    rx_atomic_inc(&rx_stats.nBusies);
		return tp;
	    }
	    rxi_KeepAliveOn(call);
	} else if (np->header.callNumber != currentCallNumber) {
	    /* Wait until the transmit queue is idle before deciding
	     * whether to reset the current call. Chances are that the
	     * call will be in ether DALLY or HOLD state once the TQ_BUSY
	     * flag is cleared.
	     */
#ifdef AFS_GLOBAL_RXLOCK_KERNEL
	    while ((call->state == RX_STATE_ACTIVE)
		   && (call->flags & RX_CALL_TQ_BUSY)) {
		call->flags |= RX_CALL_TQ_WAIT;
		call->tqWaiters++;
#ifdef RX_ENABLE_LOCKS
		osirx_AssertMine(&call->lock, "rxi_Start lock3");
		CV_WAIT(&call->cv_tq, &call->lock);
#else /* RX_ENABLE_LOCKS */
		osi_rxSleep(&call->tq);
#endif /* RX_ENABLE_LOCKS */
		call->tqWaiters--;
		if (call->tqWaiters == 0)
		    call->flags &= ~RX_CALL_TQ_WAIT;
	    }
#endif /* AFS_GLOBAL_RXLOCK_KERNEL */
	    /* If the new call cannot be taken right now send a busy and set
	     * the error condition in this call, so that it terminates as
	     * quickly as possible */
	    if (call->state == RX_STATE_ACTIVE) {
		struct rx_packet *tp;

		rxi_CallError(call, RX_CALL_DEAD);
		tp = rxi_SendSpecial(call, conn, np, RX_PACKET_TYPE_BUSY,
				     NULL, 0, 1);
		MUTEX_EXIT(&call->lock);
                MUTEX_ENTER(&rx_refcnt_mutex);
		conn->refCount--;
                MUTEX_EXIT(&rx_refcnt_mutex);
		return tp;
	    }
	    rxi_ResetCall(call, 0);
	    *call->callNumber = np->header.callNumber;
#ifdef RXDEBUG
	    if (np->header.callNumber == 0)
		dpf(("RecPacket call 0 %d %s: %x.%u.%u.%u.%u.%u.%u flags %d, packet %"AFS_PTR_FMT" resend %d.%06d len %d",
                      np->header.serial, rx_packetTypes[np->header.type - 1], ntohl(conn->peer->host), ntohs(conn->peer->port),
                      np->header.serial, np->header.epoch, np->header.cid, np->header.callNumber, np->header.seq,
                      np->header.flags, np, np->retryTime.sec, np->retryTime.usec, np->length));
#endif
	    call->state = RX_STATE_PRECALL;
	    clock_GetTime(&call->queueTime);
	    hzero(call->bytesSent);
	    hzero(call->bytesRcvd);
	    /*
	     * If the number of queued calls exceeds the overload
	     * threshold then abort this call.
	     */
	    if ((rx_BusyThreshold > 0) &&
	        (rx_atomic_read(&rx_nWaiting) > rx_BusyThreshold)) {
		struct rx_packet *tp;

		rxi_CallError(call, rx_BusyError);
		tp = rxi_SendCallAbort(call, np, 1, 0);
		MUTEX_EXIT(&call->lock);
                MUTEX_ENTER(&rx_refcnt_mutex);
		conn->refCount--;
                MUTEX_EXIT(&rx_refcnt_mutex);
                if (rx_stats_active)
                    rx_atomic_inc(&rx_stats.nBusies);
		return tp;
	    }
	    rxi_KeepAliveOn(call);
	} else {
	    /* Continuing call; do nothing here. */
	}
    } else {			/* we're the client */
	/* Ignore all incoming acknowledgements for calls in DALLY state */
	if (call && (call->state == RX_STATE_DALLY)
	    && (np->header.type == RX_PACKET_TYPE_ACK)) {
            if (rx_stats_active)
                rx_atomic_inc(&rx_stats.ignorePacketDally);
#ifdef  RX_ENABLE_LOCKS
	    if (call) {
		MUTEX_EXIT(&call->lock);
	    }
#endif
            MUTEX_ENTER(&rx_refcnt_mutex);
	    conn->refCount--;
            MUTEX_EXIT(&rx_refcnt_mutex);
	    return np;
	}

	/* Ignore anything that's not relevant to the current call.  If there
	 * isn't a current call, then no packet is relevant. */
	if (!call || (np->header.callNumber != currentCallNumber)) {
            if (rx_stats_active)
                rx_atomic_inc(&rx_stats.spuriousPacketsRead);
#ifdef	RX_ENABLE_LOCKS
	    if (call) {
		MUTEX_EXIT(&call->lock);
	    }
#endif
            MUTEX_ENTER(&rx_refcnt_mutex);
	    conn->refCount--;
            MUTEX_EXIT(&rx_refcnt_mutex);
	    return np;
	}
	/* If the service security object index stamped in the packet does not
	 * match the connection's security index, ignore the packet */
	if (np->header.securityIndex != conn->securityIndex) {
#ifdef	RX_ENABLE_LOCKS
	    MUTEX_EXIT(&call->lock);
#endif
            MUTEX_ENTER(&rx_refcnt_mutex);
	    conn->refCount--;
            MUTEX_EXIT(&rx_refcnt_mutex);
	    return np;
	}

	/* If we're receiving the response, then all transmit packets are
	 * implicitly acknowledged.  Get rid of them. */
	if (np->header.type == RX_PACKET_TYPE_DATA) {
#ifdef	AFS_GLOBAL_RXLOCK_KERNEL
	    /* XXX Hack. Because we must release the global rx lock when
	     * sending packets (osi_NetSend) we drop all acks while we're
	     * traversing the tq in rxi_Start sending packets out because
	     * packets may move to the freePacketQueue as result of being here!
	     * So we drop these packets until we're safely out of the
	     * traversing. Really ugly!
	     * For fine grain RX locking, we set the acked field in the
	     * packets and let rxi_Start remove them from the transmit queue.
	     */
	    if (call->flags & RX_CALL_TQ_BUSY) {
#ifdef	RX_ENABLE_LOCKS
		rxi_SetAcksInTransmitQueue(call);
#else
                MUTEX_ENTER(&rx_refcnt_mutex);
		conn->refCount--;
                MUTEX_EXIT(&rx_refcnt_mutex);
		return np;	/* xmitting; drop packet */
#endif
	    } else {
		rxi_ClearTransmitQueue(call, 0);
	    }
#else /* AFS_GLOBAL_RXLOCK_KERNEL */
	    rxi_ClearTransmitQueue(call, 0);
#endif /* AFS_GLOBAL_RXLOCK_KERNEL */
	} else {
	    if (np->header.type == RX_PACKET_TYPE_ACK) {
		/* now check to see if this is an ack packet acknowledging that the
		 * server actually *lost* some hard-acked data.  If this happens we
		 * ignore this packet, as it may indicate that the server restarted in
		 * the middle of a call.  It is also possible that this is an old ack
		 * packet.  We don't abort the connection in this case, because this
		 * *might* just be an old ack packet.  The right way to detect a server
		 * restart in the midst of a call is to notice that the server epoch
		 * changed, btw.  */
		/* XXX I'm not sure this is exactly right, since tfirst **IS**
		 * XXX unacknowledged.  I think that this is off-by-one, but
		 * XXX I don't dare change it just yet, since it will
		 * XXX interact badly with the server-restart detection
		 * XXX code in receiveackpacket.  */
		if (ntohl(rx_GetInt32(np, FIRSTACKOFFSET)) < call->tfirst) {
                    if (rx_stats_active)
                        rx_atomic_inc(&rx_stats.spuriousPacketsRead);
		    MUTEX_EXIT(&call->lock);
                    MUTEX_ENTER(&rx_refcnt_mutex);
		    conn->refCount--;
                    MUTEX_EXIT(&rx_refcnt_mutex);
		    return np;
		}
	    }
	}			/* else not a data packet */
    }

    osirx_AssertMine(&call->lock, "rxi_ReceivePacket middle");
    /* Set remote user defined status from packet */
    call->remoteStatus = np->header.userStatus;

    /* Note the gap between the expected next packet and the actual
     * packet that arrived, when the new packet has a smaller serial number
     * than expected.  Rioses frequently reorder packets all by themselves,
     * so this will be quite important with very large window sizes.
     * Skew is checked against 0 here to avoid any dependence on the type of
     * inPacketSkew (which may be unsigned).  In C, -1 > (unsigned) 0 is always
     * true!
     * The inPacketSkew should be a smoothed running value, not just a maximum.  MTUXXX
     * see CalculateRoundTripTime for an example of how to keep smoothed values.
     * I think using a beta of 1/8 is probably appropriate.  93.04.21
     */
    MUTEX_ENTER(&conn->conn_data_lock);
    skew = conn->lastSerial - np->header.serial;
    conn->lastSerial = np->header.serial;
    MUTEX_EXIT(&conn->conn_data_lock);
    if (skew > 0) {
	struct rx_peer *peer;
	peer = conn->peer;
	if (skew > peer->inPacketSkew) {
	    dpf(("*** In skew changed from %d to %d\n",
                  peer->inPacketSkew, skew));
	    peer->inPacketSkew = skew;
	}
    }

    /* Now do packet type-specific processing */
    switch (np->header.type) {
    case RX_PACKET_TYPE_DATA:
	np = rxi_ReceiveDataPacket(call, np, 1, socket, host, port, tnop,
				   newcallp);
	break;
    case RX_PACKET_TYPE_ACK:
	/* Respond immediately to ack packets requesting acknowledgement
	 * (ping packets) */
	if (np->header.flags & RX_REQUEST_ACK) {
	    if (call->error)
		(void)rxi_SendCallAbort(call, 0, 1, 0);
	    else
		(void)rxi_SendAck(call, 0, np->header.serial,
				  RX_ACK_PING_RESPONSE, 1);
	}
	np = rxi_ReceiveAckPacket(call, np, 1);
	break;
    case RX_PACKET_TYPE_ABORT: {
	/* An abort packet: reset the call, passing the error up to the user. */
	/* What if error is zero? */
	/* What if the error is -1? the application will treat it as a timeout. */
	afs_int32 errdata = ntohl(*(afs_int32 *) rx_DataOf(np));
	dpf(("rxi_ReceivePacket ABORT rx_DataOf = %d", errdata));
	rxi_CallError(call, errdata);
	MUTEX_EXIT(&call->lock);
        MUTEX_ENTER(&rx_refcnt_mutex);
	conn->refCount--;
        MUTEX_EXIT(&rx_refcnt_mutex);
	return np;		/* xmitting; drop packet */
    }
    case RX_PACKET_TYPE_BUSY:
	/* XXXX */
	break;
    case RX_PACKET_TYPE_ACKALL:
	/* All packets acknowledged, so we can drop all packets previously
	 * readied for sending */
#ifdef	AFS_GLOBAL_RXLOCK_KERNEL
	/* XXX Hack. We because we can't release the global rx lock when
	 * sending packets (osi_NetSend) we drop all ack pkts while we're
	 * traversing the tq in rxi_Start sending packets out because
	 * packets may move to the freePacketQueue as result of being
	 * here! So we drop these packets until we're safely out of the
	 * traversing. Really ugly!
	 * For fine grain RX locking, we set the acked field in the packets
	 * and let rxi_Start remove the packets from the transmit queue.
	 */
	if (call->flags & RX_CALL_TQ_BUSY) {
#ifdef	RX_ENABLE_LOCKS
	    rxi_SetAcksInTransmitQueue(call);
	    break;
#else /* RX_ENABLE_LOCKS */
	    MUTEX_EXIT(&call->lock);
            MUTEX_ENTER(&rx_refcnt_mutex);
	    conn->refCount--;
            MUTEX_EXIT(&rx_refcnt_mutex);
	    return np;		/* xmitting; drop packet */
#endif /* RX_ENABLE_LOCKS */
	}
#endif /* AFS_GLOBAL_RXLOCK_KERNEL */
	rxi_ClearTransmitQueue(call, 0);
	rxevent_Cancel(call->keepAliveEvent, call, RX_CALL_REFCOUNT_ALIVE);
	break;
    default:
	/* Should not reach here, unless the peer is broken: send an abort
	 * packet */
	rxi_CallError(call, RX_PROTOCOL_ERROR);
	np = rxi_SendCallAbort(call, np, 1, 0);
	break;
    };
    /* Note when this last legitimate packet was received, for keep-alive
     * processing.  Note, we delay getting the time until now in the hope that
     * the packet will be delivered to the user before any get time is required
     * (if not, then the time won't actually be re-evaluated here). */
    call->lastReceiveTime = clock_Sec();
    MUTEX_EXIT(&call->lock);
    MUTEX_ENTER(&rx_refcnt_mutex);
    conn->refCount--;
    MUTEX_EXIT(&rx_refcnt_mutex);
    return np;
}

/* return true if this is an "interesting" connection from the point of view
    of someone trying to debug the system */
int
rxi_IsConnInteresting(struct rx_connection *aconn)
{
    int i;
    struct rx_call *tcall;

    if (aconn->flags & (RX_CONN_MAKECALL_WAITING | RX_CONN_DESTROY_ME))
	return 1;

    for (i = 0; i < RX_MAXCALLS; i++) {
	tcall = aconn->call[i];
	if (tcall) {
	    if ((tcall->state == RX_STATE_PRECALL)
		|| (tcall->state == RX_STATE_ACTIVE))
		return 1;
	    if ((tcall->mode == RX_MODE_SENDING)
		|| (tcall->mode == RX_MODE_RECEIVING))
		return 1;
	}
    }
    return 0;
}

#ifdef KERNEL
/* if this is one of the last few packets AND it wouldn't be used by the
   receiving call to immediately satisfy a read request, then drop it on
   the floor, since accepting it might prevent a lock-holding thread from
   making progress in its reading. If a call has been cleared while in
   the precall state then ignore all subsequent packets until the call
   is assigned to a thread. */

static int
TooLow(struct rx_packet *ap, struct rx_call *acall)
{
    int rc = 0;

    MUTEX_ENTER(&rx_quota_mutex);
    if (((ap->header.seq != 1) && (acall->flags & RX_CALL_CLEARED)
	 && (acall->state == RX_STATE_PRECALL))
	|| ((rx_nFreePackets < rxi_dataQuota + 2)
	    && !((ap->header.seq < acall->rnext + rx_initSendWindow)
		 && (acall->flags & RX_CALL_READER_WAIT)))) {
	rc = 1;
    }
    MUTEX_EXIT(&rx_quota_mutex);
    return rc;
}
#endif /* KERNEL */

static void
rxi_CheckReachEvent(struct rxevent *event, void *arg1, void *arg2)
{
    struct rx_connection *conn = arg1;
    struct rx_call *acall = arg2;
    struct rx_call *call = acall;
    struct clock when, now;
    int i, waiting;

    MUTEX_ENTER(&conn->conn_data_lock);
    conn->checkReachEvent = NULL;
    waiting = conn->flags & RX_CONN_ATTACHWAIT;
    if (event) {
        MUTEX_ENTER(&rx_refcnt_mutex);
	conn->refCount--;
        MUTEX_EXIT(&rx_refcnt_mutex);
    }
    MUTEX_EXIT(&conn->conn_data_lock);

    if (waiting) {
	if (!call) {
	    MUTEX_ENTER(&conn->conn_call_lock);
	    MUTEX_ENTER(&conn->conn_data_lock);
	    for (i = 0; i < RX_MAXCALLS; i++) {
		struct rx_call *tc = conn->call[i];
		if (tc && tc->state == RX_STATE_PRECALL) {
		    call = tc;
		    break;
		}
	    }
	    if (!call)
		/* Indicate that rxi_CheckReachEvent is no longer running by
		 * clearing the flag.  Must be atomic under conn_data_lock to
		 * avoid a new call slipping by: rxi_CheckConnReach holds
		 * conn_data_lock while checking RX_CONN_ATTACHWAIT.
		 */
		conn->flags &= ~RX_CONN_ATTACHWAIT;
	    MUTEX_EXIT(&conn->conn_data_lock);
	    MUTEX_EXIT(&conn->conn_call_lock);
	}

	if (call) {
	    if (call != acall)
		MUTEX_ENTER(&call->lock);
	    rxi_SendAck(call, NULL, 0, RX_ACK_PING, 0);
	    if (call != acall)
		MUTEX_EXIT(&call->lock);

	    clock_GetTime(&now);
	    when = now;
	    when.sec += RX_CHECKREACH_TIMEOUT;
	    MUTEX_ENTER(&conn->conn_data_lock);
	    if (!conn->checkReachEvent) {
                MUTEX_ENTER(&rx_refcnt_mutex);
		conn->refCount++;
                MUTEX_EXIT(&rx_refcnt_mutex);
		conn->checkReachEvent =
		    rxevent_PostNow(&when, &now, rxi_CheckReachEvent, conn,
				    NULL);
	    }
	    MUTEX_EXIT(&conn->conn_data_lock);
	}
    }
}

static int
rxi_CheckConnReach(struct rx_connection *conn, struct rx_call *call)
{
    struct rx_service *service = conn->service;
    struct rx_peer *peer = conn->peer;
    afs_uint32 now, lastReach;

    if (service->checkReach == 0)
	return 0;

    now = clock_Sec();
    MUTEX_ENTER(&peer->peer_lock);
    lastReach = peer->lastReachTime;
    MUTEX_EXIT(&peer->peer_lock);
    if (now - lastReach < RX_CHECKREACH_TTL)
	return 0;

    MUTEX_ENTER(&conn->conn_data_lock);
    if (conn->flags & RX_CONN_ATTACHWAIT) {
	MUTEX_EXIT(&conn->conn_data_lock);
	return 1;
    }
    conn->flags |= RX_CONN_ATTACHWAIT;
    MUTEX_EXIT(&conn->conn_data_lock);
    if (!conn->checkReachEvent)
	rxi_CheckReachEvent(NULL, conn, call);

    return 1;
}

/* try to attach call, if authentication is complete */
static void
TryAttach(struct rx_call *acall, osi_socket socket,
	  int *tnop, struct rx_call **newcallp,
	  int reachOverride)
{
    struct rx_connection *conn = acall->conn;

    if (conn->type == RX_SERVER_CONNECTION
	&& acall->state == RX_STATE_PRECALL) {
	/* Don't attach until we have any req'd. authentication. */
	if (RXS_CheckAuthentication(conn->securityObject, conn) == 0) {
	    if (reachOverride || rxi_CheckConnReach(conn, acall) == 0)
		rxi_AttachServerProc(acall, socket, tnop, newcallp);
	    /* Note:  this does not necessarily succeed; there
	     * may not any proc available
	     */
	} else {
	    rxi_ChallengeOn(acall->conn);
	}
    }
}

/* A data packet has been received off the interface.  This packet is
 * appropriate to the call (the call is in the right state, etc.).  This
 * routine can return a packet to the caller, for re-use */

struct rx_packet *
rxi_ReceiveDataPacket(struct rx_call *call,
		      struct rx_packet *np, int istack,
		      osi_socket socket, afs_uint32 host, u_short port,
		      int *tnop, struct rx_call **newcallp)
{
    int ackNeeded = 0;		/* 0 means no, otherwise ack_reason */
    int newPackets = 0;
    int didHardAck = 0;
    int haveLast = 0;
    afs_uint32 seq;
    afs_uint32 serial=0, flags=0;
    int isFirst;
    struct rx_packet *tnp;
    struct clock when, now;
    if (rx_stats_active)
        rx_atomic_inc(&rx_stats.dataPacketsRead);

#ifdef KERNEL
    /* If there are no packet buffers, drop this new packet, unless we can find
     * packet buffers from inactive calls */
    if (!call->error
	&& (rxi_OverQuota(RX_PACKET_CLASS_RECEIVE) || TooLow(np, call))) {
	MUTEX_ENTER(&rx_freePktQ_lock);
	rxi_NeedMorePackets = TRUE;
	MUTEX_EXIT(&rx_freePktQ_lock);
        if (rx_stats_active)
            rx_atomic_inc(&rx_stats.noPacketBuffersOnRead);
	call->rprev = np->header.serial;
	rxi_calltrace(RX_TRACE_DROP, call);
	dpf(("packet %"AFS_PTR_FMT" dropped on receipt - quota problems", np));
	if (rxi_doreclaim)
	    rxi_ClearReceiveQueue(call);
	clock_GetTime(&now);
	when = now;
	clock_Add(&when, &rx_softAckDelay);
	if (!call->delayedAckEvent
	    || clock_Gt(&call->delayedAckEvent->eventTime, &when)) {
	    rxevent_Cancel(call->delayedAckEvent, call,
			   RX_CALL_REFCOUNT_DELAY);
            MUTEX_ENTER(&rx_refcnt_mutex);
	    CALL_HOLD(call, RX_CALL_REFCOUNT_DELAY);
            MUTEX_EXIT(&rx_refcnt_mutex);

	    call->delayedAckEvent =
		rxevent_PostNow(&when, &now, rxi_SendDelayedAck, call, 0);
	}
	/* we've damaged this call already, might as well do it in. */
	return np;
    }
#endif /* KERNEL */

    /*
     * New in AFS 3.5, if the RX_JUMBO_PACKET flag is set then this
     * packet is one of several packets transmitted as a single
     * datagram. Do not send any soft or hard acks until all packets
     * in a jumbogram have been processed. Send negative acks right away.
     */
    for (isFirst = 1, tnp = NULL; isFirst || tnp; isFirst = 0) {
	/* tnp is non-null when there are more packets in the
	 * current jumbo gram */
	if (tnp) {
	    if (np)
		rxi_FreePacket(np);
	    np = tnp;
	}

	seq = np->header.seq;
	serial = np->header.serial;
	flags = np->header.flags;

	/* If the call is in an error state, send an abort message */
	if (call->error)
	    return rxi_SendCallAbort(call, np, istack, 0);

	/* The RX_JUMBO_PACKET is set in all but the last packet in each
	 * AFS 3.5 jumbogram. */
	if (flags & RX_JUMBO_PACKET) {
	    tnp = rxi_SplitJumboPacket(np, host, port, isFirst);
	} else {
	    tnp = NULL;
	}

	if (np->header.spare != 0) {
	    MUTEX_ENTER(&call->conn->conn_data_lock);
	    call->conn->flags |= RX_CONN_USING_PACKET_CKSUM;
	    MUTEX_EXIT(&call->conn->conn_data_lock);
	}

	/* The usual case is that this is the expected next packet */
	if (seq == call->rnext) {

	    /* Check to make sure it is not a duplicate of one already queued */
	    if (queue_IsNotEmpty(&call->rq)
		&& queue_First(&call->rq, rx_packet)->header.seq == seq) {
                if (rx_stats_active)
                    rx_atomic_inc(&rx_stats.dupPacketsRead);
		dpf(("packet %"AFS_PTR_FMT" dropped on receipt - duplicate", np));
		rxevent_Cancel(call->delayedAckEvent, call,
			       RX_CALL_REFCOUNT_DELAY);
		np = rxi_SendAck(call, np, serial, RX_ACK_DUPLICATE, istack);
		ackNeeded = 0;
		call->rprev = seq;
		continue;
	    }

	    /* It's the next packet. Stick it on the receive queue
	     * for this call. Set newPackets to make sure we wake
	     * the reader once all packets have been processed */
#ifdef RX_TRACK_PACKETS
	    np->flags |= RX_PKTFLAG_RQ;
#endif
	    queue_Prepend(&call->rq, np);
#ifdef RXDEBUG_PACKET
            call->rqc++;
#endif /* RXDEBUG_PACKET */
	    call->nSoftAcks++;
	    np = NULL;		/* We can't use this anymore */
	    newPackets = 1;

	    /* If an ack is requested then set a flag to make sure we
	     * send an acknowledgement for this packet */
	    if (flags & RX_REQUEST_ACK) {
		ackNeeded = RX_ACK_REQUESTED;
	    }

	    /* Keep track of whether we have received the last packet */
	    if (flags & RX_LAST_PACKET) {
		call->flags |= RX_CALL_HAVE_LAST;
		haveLast = 1;
	    }

	    /* Check whether we have all of the packets for this call */
	    if (call->flags & RX_CALL_HAVE_LAST) {
		afs_uint32 tseq;	/* temporary sequence number */
		struct rx_packet *tp;	/* Temporary packet pointer */
		struct rx_packet *nxp;	/* Next pointer, for queue_Scan */

		for (tseq = seq, queue_Scan(&call->rq, tp, nxp, rx_packet)) {
		    if (tseq != tp->header.seq)
			break;
		    if (tp->header.flags & RX_LAST_PACKET) {
			call->flags |= RX_CALL_RECEIVE_DONE;
			break;
		    }
		    tseq++;
		}
	    }

	    /* Provide asynchronous notification for those who want it
	     * (e.g. multi rx) */
	    if (call->arrivalProc) {
		(*call->arrivalProc) (call, call->arrivalProcHandle,
				      call->arrivalProcArg);
		call->arrivalProc = (void (*)())0;
	    }

	    /* Update last packet received */
	    call->rprev = seq;

	    /* If there is no server process serving this call, grab
	     * one, if available. We only need to do this once. If a
	     * server thread is available, this thread becomes a server
	     * thread and the server thread becomes a listener thread. */
	    if (isFirst) {
		TryAttach(call, socket, tnop, newcallp, 0);
	    }
	}
	/* This is not the expected next packet. */
	else {
	    /* Determine whether this is a new or old packet, and if it's
	     * a new one, whether it fits into the current receive window.
	     * Also figure out whether the packet was delivered in sequence.
	     * We use the prev variable to determine whether the new packet
	     * is the successor of its immediate predecessor in the
	     * receive queue, and the missing flag to determine whether
	     * any of this packets predecessors are missing.  */

	    afs_uint32 prev;	/* "Previous packet" sequence number */
	    struct rx_packet *tp;	/* Temporary packet pointer */
	    struct rx_packet *nxp;	/* Next pointer, for queue_Scan */
	    int missing;	/* Are any predecessors missing? */

	    /* If the new packet's sequence number has been sent to the
	     * application already, then this is a duplicate */
	    if (seq < call->rnext) {
                if (rx_stats_active)
                    rx_atomic_inc(&rx_stats.dupPacketsRead);
		rxevent_Cancel(call->delayedAckEvent, call,
			       RX_CALL_REFCOUNT_DELAY);
		np = rxi_SendAck(call, np, serial, RX_ACK_DUPLICATE, istack);
		ackNeeded = 0;
		call->rprev = seq;
		continue;
	    }

	    /* If the sequence number is greater than what can be
	     * accomodated by the current window, then send a negative
	     * acknowledge and drop the packet */
	    if ((call->rnext + call->rwind) <= seq) {
		rxevent_Cancel(call->delayedAckEvent, call,
			       RX_CALL_REFCOUNT_DELAY);
		np = rxi_SendAck(call, np, serial, RX_ACK_EXCEEDS_WINDOW,
				 istack);
		ackNeeded = 0;
		call->rprev = seq;
		continue;
	    }

	    /* Look for the packet in the queue of old received packets */
	    for (prev = call->rnext - 1, missing =
		 0, queue_Scan(&call->rq, tp, nxp, rx_packet)) {
		/*Check for duplicate packet */
		if (seq == tp->header.seq) {
                    if (rx_stats_active)
                        rx_atomic_inc(&rx_stats.dupPacketsRead);
		    rxevent_Cancel(call->delayedAckEvent, call,
				   RX_CALL_REFCOUNT_DELAY);
		    np = rxi_SendAck(call, np, serial, RX_ACK_DUPLICATE,
				     istack);
		    ackNeeded = 0;
		    call->rprev = seq;
		    goto nextloop;
		}
		/* If we find a higher sequence packet, break out and
		 * insert the new packet here. */
		if (seq < tp->header.seq)
		    break;
		/* Check for missing packet */
		if (tp->header.seq != prev + 1) {
		    missing = 1;
		}

		prev = tp->header.seq;
	    }

	    /* Keep track of whether we have received the last packet. */
	    if (flags & RX_LAST_PACKET) {
		call->flags |= RX_CALL_HAVE_LAST;
	    }

	    /* It's within the window: add it to the the receive queue.
	     * tp is left by the previous loop either pointing at the
	     * packet before which to insert the new packet, or at the
	     * queue head if the queue is empty or the packet should be
	     * appended. */
#ifdef RX_TRACK_PACKETS
            np->flags |= RX_PKTFLAG_RQ;
#endif
#ifdef RXDEBUG_PACKET
            call->rqc++;
#endif /* RXDEBUG_PACKET */
	    queue_InsertBefore(tp, np);
	    call->nSoftAcks++;
	    np = NULL;

	    /* Check whether we have all of the packets for this call */
	    if ((call->flags & RX_CALL_HAVE_LAST)
		&& !(call->flags & RX_CALL_RECEIVE_DONE)) {
		afs_uint32 tseq;	/* temporary sequence number */

		for (tseq =
		     call->rnext, queue_Scan(&call->rq, tp, nxp, rx_packet)) {
		    if (tseq != tp->header.seq)
			break;
		    if (tp->header.flags & RX_LAST_PACKET) {
			call->flags |= RX_CALL_RECEIVE_DONE;
			break;
		    }
		    tseq++;
		}
	    }

	    /* We need to send an ack of the packet is out of sequence,
	     * or if an ack was requested by the peer. */
	    if (seq != prev + 1 || missing) {
		ackNeeded = RX_ACK_OUT_OF_SEQUENCE;
	    } else if (flags & RX_REQUEST_ACK) {
		ackNeeded = RX_ACK_REQUESTED;
            }

	    /* Acknowledge the last packet for each call */
	    if (flags & RX_LAST_PACKET) {
		haveLast = 1;
	    }

	    call->rprev = seq;
	}
      nextloop:;
    }

    if (newPackets) {
	/*
	 * If the receiver is waiting for an iovec, fill the iovec
	 * using the data from the receive queue */
	if (call->flags & RX_CALL_IOVEC_WAIT) {
	    didHardAck = rxi_FillReadVec(call, serial);
	    /* the call may have been aborted */
	    if (call->error) {
		return NULL;
	    }
	    if (didHardAck) {
		ackNeeded = 0;
	    }
	}

	/* Wakeup the reader if any */
	if ((call->flags & RX_CALL_READER_WAIT)
	    && (!(call->flags & RX_CALL_IOVEC_WAIT) || !(call->iovNBytes)
		|| (call->iovNext >= call->iovMax)
		|| (call->flags & RX_CALL_RECEIVE_DONE))) {
	    call->flags &= ~RX_CALL_READER_WAIT;
#ifdef	RX_ENABLE_LOCKS
	    CV_BROADCAST(&call->cv_rq);
#else
	    osi_rxWakeup(&call->rq);
#endif
	}
    }

    /*
     * Send an ack when requested by the peer, or once every
     * rxi_SoftAckRate packets until the last packet has been
     * received. Always send a soft ack for the last packet in
     * the server's reply.
     *
     * If we have received all of the packets for the call
     * immediately send an RX_PACKET_TYPE_ACKALL packet so that
     * the peer can empty its packet queue and cancel all resend
     * events.
     */
    if (call->flags & RX_CALL_RECEIVE_DONE) {
        rxevent_Cancel(call->delayedAckEvent, call, RX_CALL_REFCOUNT_DELAY);
        rxi_AckAll(NULL, call, 0);
    } else if (ackNeeded) {
	rxevent_Cancel(call->delayedAckEvent, call, RX_CALL_REFCOUNT_DELAY);
	np = rxi_SendAck(call, np, serial, ackNeeded, istack);
    } else if (call->nSoftAcks > (u_short) rxi_SoftAckRate) {
	rxevent_Cancel(call->delayedAckEvent, call, RX_CALL_REFCOUNT_DELAY);
	np = rxi_SendAck(call, np, serial, RX_ACK_IDLE, istack);
    } else if (call->nSoftAcks) {
	clock_GetTime(&now);
	when = now;
	if (haveLast && !(flags & RX_CLIENT_INITIATED)) {
	    clock_Add(&when, &rx_lastAckDelay);
	} else {
	    clock_Add(&when, &rx_softAckDelay);
	}
	if (!call->delayedAckEvent
	    || clock_Gt(&call->delayedAckEvent->eventTime, &when)) {
	    rxevent_Cancel(call->delayedAckEvent, call,
			   RX_CALL_REFCOUNT_DELAY);
            MUTEX_ENTER(&rx_refcnt_mutex);
	    CALL_HOLD(call, RX_CALL_REFCOUNT_DELAY);
            MUTEX_EXIT(&rx_refcnt_mutex);
	    call->delayedAckEvent =
		rxevent_PostNow(&when, &now, rxi_SendDelayedAck, call, 0);
	}
    }

    return np;
}

#ifdef	ADAPT_WINDOW
static void rxi_ComputeRate();
#endif

static void
rxi_UpdatePeerReach(struct rx_connection *conn, struct rx_call *acall)
{
    struct rx_peer *peer = conn->peer;

    MUTEX_ENTER(&peer->peer_lock);
    peer->lastReachTime = clock_Sec();
    MUTEX_EXIT(&peer->peer_lock);

    MUTEX_ENTER(&conn->conn_data_lock);
    if (conn->flags & RX_CONN_ATTACHWAIT) {
	int i;

	conn->flags &= ~RX_CONN_ATTACHWAIT;
	MUTEX_EXIT(&conn->conn_data_lock);

	for (i = 0; i < RX_MAXCALLS; i++) {
	    struct rx_call *call = conn->call[i];
	    if (call) {
		if (call != acall)
		    MUTEX_ENTER(&call->lock);
		/* tnop can be null if newcallp is null */
		TryAttach(call, (osi_socket) - 1, NULL, NULL, 1);
		if (call != acall)
		    MUTEX_EXIT(&call->lock);
	    }
	}
    } else
	MUTEX_EXIT(&conn->conn_data_lock);
}

#if defined(RXDEBUG) && defined(AFS_NT40_ENV)
static const char *
rx_ack_reason(int reason)
{
    switch (reason) {
    case RX_ACK_REQUESTED:
	return "requested";
    case RX_ACK_DUPLICATE:
	return "duplicate";
    case RX_ACK_OUT_OF_SEQUENCE:
	return "sequence";
    case RX_ACK_EXCEEDS_WINDOW:
	return "window";
    case RX_ACK_NOSPACE:
	return "nospace";
    case RX_ACK_PING:
	return "ping";
    case RX_ACK_PING_RESPONSE:
	return "response";
    case RX_ACK_DELAY:
	return "delay";
    case RX_ACK_IDLE:
	return "idle";
    default:
	return "unknown!!";
    }
}
#endif


/* rxi_ComputePeerNetStats
 *
 * Called exclusively by rxi_ReceiveAckPacket to compute network link
 * estimates (like RTT and throughput) based on ack packets.  Caller
 * must ensure that the packet in question is the right one (i.e.
 * serial number matches).
 */
static void
rxi_ComputePeerNetStats(struct rx_call *call, struct rx_packet *p,
			struct rx_ackPacket *ap, struct rx_packet *np)
{
    struct rx_peer *peer = call->conn->peer;

    /* Use RTT if not delayed by client and
     * ignore packets that were retransmitted. */
    if (!(p->flags & RX_PKTFLAG_ACKED) &&
        ap->reason != RX_ACK_DELAY &&
        clock_Eq(&p->timeSent, &p->firstSent))
	rxi_ComputeRoundTripTime(p, &p->timeSent, peer);
#ifdef ADAPT_WINDOW
    rxi_ComputeRate(peer, call, p, np, ap->reason);
#endif
}

/* The real smarts of the whole thing.  */
struct rx_packet *
rxi_ReceiveAckPacket(struct rx_call *call, struct rx_packet *np,
		     int istack)
{
    struct rx_ackPacket *ap;
    int nAcks;
    struct rx_packet *tp;
    struct rx_packet *nxp;	/* Next packet pointer for queue_Scan */
    struct rx_connection *conn = call->conn;
    struct rx_peer *peer = conn->peer;
    afs_uint32 first;
    afs_uint32 serial;
    /* because there are CM's that are bogus, sending weird values for this. */
    afs_uint32 skew = 0;
    int nbytes;
    int missing;
    int acked;
    int nNacked = 0;
    int newAckCount = 0;
    int maxDgramPackets = 0;	/* Set if peer supports AFS 3.5 jumbo datagrams */
    int pktsize = 0;            /* Set if we need to update the peer mtu */
    int conn_data_locked = 0;

    if (rx_stats_active)
        rx_atomic_inc(&rx_stats.ackPacketsRead);
    ap = (struct rx_ackPacket *)rx_DataOf(np);
    nbytes = rx_Contiguous(np) - (int)((ap->acks) - (u_char *) ap);
    if (nbytes < 0)
	return np;		/* truncated ack packet */

    /* depends on ack packet struct */
    nAcks = MIN((unsigned)nbytes, (unsigned)ap->nAcks);
    first = ntohl(ap->firstPacket);
    serial = ntohl(ap->serial);
    /* temporarily disabled -- needs to degrade over time
     * skew = ntohs(ap->maxSkew); */

    /* Ignore ack packets received out of order */
    if (first < call->tfirst) {
	return np;
    }

    if (np->header.flags & RX_SLOW_START_OK) {
	call->flags |= RX_CALL_SLOW_START_OK;
    }

    if (ap->reason == RX_ACK_PING_RESPONSE)
	rxi_UpdatePeerReach(conn, call);

    if (conn->lastPacketSizeSeq) {
	MUTEX_ENTER(&conn->conn_data_lock);
        conn_data_locked = 1;
	if ((first > conn->lastPacketSizeSeq) && (conn->lastPacketSize)) {
	    pktsize = conn->lastPacketSize;
	    conn->lastPacketSize = conn->lastPacketSizeSeq = 0;
	}
    }
    if ((ap->reason == RX_ACK_PING_RESPONSE) && (conn->lastPingSizeSer)) {
        if (!conn_data_locked) {
            MUTEX_ENTER(&conn->conn_data_lock);
            conn_data_locked = 1;
        }
	if ((conn->lastPingSizeSer == serial) && (conn->lastPingSize)) {
	    /* process mtu ping ack */
	    pktsize = conn->lastPingSize;
	    conn->lastPingSizeSer = conn->lastPingSize = 0;
	}
    }

    if (conn_data_locked) {
	MUTEX_EXIT(&conn->conn_data_lock);
        conn_data_locked = 0;
    }
#ifdef RXDEBUG
#ifdef AFS_NT40_ENV
    if (rxdebug_active) {
	char msg[512];
	size_t len;

	len = _snprintf(msg, sizeof(msg),
			"tid[%d] RACK: reason %s serial %u previous %u seq %u skew %d first %u acks %u space %u ",
			 GetCurrentThreadId(), rx_ack_reason(ap->reason),
			 ntohl(ap->serial), ntohl(ap->previousPacket),
			 (unsigned int)np->header.seq, (unsigned int)skew,
			 ntohl(ap->firstPacket), ap->nAcks, ntohs(ap->bufferSpace) );
	if (nAcks) {
	    int offset;

	    for (offset = 0; offset < nAcks && len < sizeof(msg); offset++)
		msg[len++] = (ap->acks[offset] == RX_ACK_TYPE_NACK ? '-' : '*');
	}
	msg[len++]='\n';
	msg[len] = '\0';
	OutputDebugString(msg);
    }
#else /* AFS_NT40_ENV */
    if (rx_Log) {
	fprintf(rx_Log,
		"RACK: reason %x previous %u seq %u serial %u skew %d first %u",
		ap->reason, ntohl(ap->previousPacket),
		(unsigned int)np->header.seq, (unsigned int)serial,
		(unsigned int)skew, ntohl(ap->firstPacket));
	if (nAcks) {
	    int offset;
	    for (offset = 0; offset < nAcks; offset++)
		putc(ap->acks[offset] == RX_ACK_TYPE_NACK ? '-' : '*',
		     rx_Log);
	}
	putc('\n', rx_Log);
    }
#endif /* AFS_NT40_ENV */
#endif

    MUTEX_ENTER(&peer->peer_lock);
    if (pktsize) {
	/*
	 * Start somewhere. Can't assume we can send what we can receive,
	 * but we are clearly receiving.
	 */
	if (!peer->maxPacketSize)
	    peer->maxPacketSize = RX_MIN_PACKET_SIZE+RX_IPUDP_SIZE;

	if (pktsize > peer->maxPacketSize) {
	    peer->maxPacketSize = pktsize;
	    if ((pktsize-RX_IPUDP_SIZE > peer->ifMTU)) {
		peer->ifMTU=pktsize-RX_IPUDP_SIZE;
		peer->natMTU = rxi_AdjustIfMTU(peer->ifMTU);
		rxi_ScheduleGrowMTUEvent(call, 1);
	    }
	}
    }

    /* Update the outgoing packet skew value to the latest value of
     * the peer's incoming packet skew value.  The ack packet, of
     * course, could arrive out of order, but that won't affect things
     * much */
    peer->outPacketSkew = skew;

    /* Check for packets that no longer need to be transmitted, and
     * discard them.  This only applies to packets positively
     * acknowledged as having been sent to the peer's upper level.
     * All other packets must be retained.  So only packets with
     * sequence numbers < ap->firstPacket are candidates. */
    for (queue_Scan(&call->tq, tp, nxp, rx_packet)) {
	if (tp->header.seq >= first)
	    break;
	call->tfirst = tp->header.seq + 1;
        rxi_ComputePeerNetStats(call, tp, ap, np);
	if (!(tp->flags & RX_PKTFLAG_ACKED)) {
	    newAckCount++;
	}
#ifdef	AFS_GLOBAL_RXLOCK_KERNEL
	/* XXX Hack. Because we have to release the global rx lock when sending
	 * packets (osi_NetSend) we drop all acks while we're traversing the tq
	 * in rxi_Start sending packets out because packets may move to the
	 * freePacketQueue as result of being here! So we drop these packets until
	 * we're safely out of the traversing. Really ugly!
	 * To make it even uglier, if we're using fine grain locking, we can
	 * set the ack bits in the packets and have rxi_Start remove the packets
	 * when it's done transmitting.
	 */
	if (call->flags & RX_CALL_TQ_BUSY) {
#ifdef RX_ENABLE_LOCKS
	    tp->flags |= RX_PKTFLAG_ACKED;
	    call->flags |= RX_CALL_TQ_SOME_ACKED;
#else /* RX_ENABLE_LOCKS */
	    break;
#endif /* RX_ENABLE_LOCKS */
	} else
#endif /* AFS_GLOBAL_RXLOCK_KERNEL */
	{
	    queue_Remove(tp);
#ifdef RX_TRACK_PACKETS
	    tp->flags &= ~RX_PKTFLAG_TQ;
#endif
#ifdef RXDEBUG_PACKET
            call->tqc--;
#endif /* RXDEBUG_PACKET */
	    rxi_FreePacket(tp);	/* rxi_FreePacket mustn't wake up anyone, preemptively. */
	}
    }

#ifdef ADAPT_WINDOW
    /* Give rate detector a chance to respond to ping requests */
    if (ap->reason == RX_ACK_PING_RESPONSE) {
	rxi_ComputeRate(peer, call, 0, np, ap->reason);
    }
#endif

    /* N.B. we don't turn off any timers here.  They'll go away by themselves, anyway */

    /* Now go through explicit acks/nacks and record the results in
     * the waiting packets.  These are packets that can't be released
     * yet, even with a positive acknowledge.  This positive
     * acknowledge only means the packet has been received by the
     * peer, not that it will be retained long enough to be sent to
     * the peer's upper level.  In addition, reset the transmit timers
     * of any missing packets (those packets that must be missing
     * because this packet was out of sequence) */

    call->nSoftAcked = 0;
    for (missing = 0, queue_Scan(&call->tq, tp, nxp, rx_packet)) {
	/* Update round trip time if the ack was stimulated on receipt
	 * of this packet */
#ifdef AFS_GLOBAL_RXLOCK_KERNEL
#ifdef RX_ENABLE_LOCKS
	if (tp->header.seq >= first)
#endif /* RX_ENABLE_LOCKS */
#endif /* AFS_GLOBAL_RXLOCK_KERNEL */
            rxi_ComputePeerNetStats(call, tp, ap, np);

	/* Set the acknowledge flag per packet based on the
	 * information in the ack packet. An acknowlegded packet can
	 * be downgraded when the server has discarded a packet it
	 * soacked previously, or when an ack packet is received
	 * out of sequence. */
	if (tp->header.seq < first) {
	    /* Implicit ack information */
	    if (!(tp->flags & RX_PKTFLAG_ACKED)) {
		newAckCount++;
	    }
	    tp->flags |= RX_PKTFLAG_ACKED;
	} else if (tp->header.seq < first + nAcks) {
	    /* Explicit ack information:  set it in the packet appropriately */
	    if (ap->acks[tp->header.seq - first] == RX_ACK_TYPE_ACK) {
		if (!(tp->flags & RX_PKTFLAG_ACKED)) {
		    newAckCount++;
		    tp->flags |= RX_PKTFLAG_ACKED;
		}
		if (missing) {
		    nNacked++;
		} else {
		    call->nSoftAcked++;
		}
	    } else /* RX_ACK_TYPE_NACK */ {
		tp->flags &= ~RX_PKTFLAG_ACKED;
		missing = 1;
	    }
	} else {
	    tp->flags &= ~RX_PKTFLAG_ACKED;
	    missing = 1;
	}

        /*
         * Following the suggestion of Phil Kern, we back off the peer's
         * timeout value for future packets until a successful response
         * is received for an initial transmission.
         */
        if (missing && !peer->backedOff) {
            struct clock c = peer->timeout;
            struct clock max_to = {3, 0};

            clock_Add(&peer->timeout, &c);
            if (clock_Gt(&peer->timeout, &max_to))
                peer->timeout = max_to;
            peer->backedOff = 1;
        }

	/* If packet isn't yet acked, and it has been transmitted at least
	 * once, reset retransmit time using latest timeout
	 * ie, this should readjust the retransmit timer for all outstanding
	 * packets...  So we don't just retransmit when we should know better*/

	if (!(tp->flags & RX_PKTFLAG_ACKED) && !clock_IsZero(&tp->retryTime)) {
            tp->retryTime = tp->timeSent;
	    clock_Add(&tp->retryTime, &peer->timeout);
	    /* shift by eight because one quarter-sec ~ 256 milliseconds */
	    clock_Addmsec(&(tp->retryTime), ((afs_uint32) tp->backoff) << 8);
	}
    }

    /* If the window has been extended by this acknowledge packet,
     * then wakeup a sender waiting in alloc for window space, or try
     * sending packets now, if he's been sitting on packets due to
     * lack of window space */
    if (call->tnext < (call->tfirst + call->twind)) {
#ifdef	RX_ENABLE_LOCKS
	CV_SIGNAL(&call->cv_twind);
#else
	if (call->flags & RX_CALL_WAIT_WINDOW_ALLOC) {
	    call->flags &= ~RX_CALL_WAIT_WINDOW_ALLOC;
	    osi_rxWakeup(&call->twind);
	}
#endif
	if (call->flags & RX_CALL_WAIT_WINDOW_SEND) {
	    call->flags &= ~RX_CALL_WAIT_WINDOW_SEND;
	}
    }

    /* if the ack packet has a receivelen field hanging off it,
     * update our state */
    if (np->length >= rx_AckDataSize(ap->nAcks) + 2 * sizeof(afs_int32)) {
	afs_uint32 tSize;

	/* If the ack packet has a "recommended" size that is less than
	 * what I am using now, reduce my size to match */
	rx_packetread(np, rx_AckDataSize(ap->nAcks) + (int)sizeof(afs_int32),
		      (int)sizeof(afs_int32), &tSize);
	tSize = (afs_uint32) ntohl(tSize);
	peer->natMTU = rxi_AdjustIfMTU(MIN(tSize, peer->ifMTU));

	/* Get the maximum packet size to send to this peer */
	rx_packetread(np, rx_AckDataSize(ap->nAcks), (int)sizeof(afs_int32),
		      &tSize);
	tSize = (afs_uint32) ntohl(tSize);
	tSize = (afs_uint32) MIN(tSize, rx_MyMaxSendSize);
	tSize = rxi_AdjustMaxMTU(peer->natMTU, tSize);

	/* sanity check - peer might have restarted with different params.
	 * If peer says "send less", dammit, send less...  Peer should never
	 * be unable to accept packets of the size that prior AFS versions would
	 * send without asking.  */
	if (peer->maxMTU != tSize) {
	    if (peer->maxMTU > tSize) /* possible cong., maxMTU decreased */
		peer->congestSeq++;
	    peer->maxMTU = tSize;
	    peer->MTU = MIN(tSize, peer->MTU);
	    call->MTU = MIN(call->MTU, tSize);
	}

	if (np->length == rx_AckDataSize(ap->nAcks) + 3 * sizeof(afs_int32)) {
	    /* AFS 3.4a */
	    rx_packetread(np,
			  rx_AckDataSize(ap->nAcks) + 2 * (int)sizeof(afs_int32),
			  (int)sizeof(afs_int32), &tSize);
	    tSize = (afs_uint32) ntohl(tSize);	/* peer's receive window, if it's */
	    if (tSize < call->twind) {	/* smaller than our send */
		call->twind = tSize;	/* window, we must send less... */
		call->ssthresh = MIN(call->twind, call->ssthresh);
		call->conn->twind[call->channel] = call->twind;
	    }

	    /* Only send jumbograms to 3.4a fileservers. 3.3a RX gets the
	     * network MTU confused with the loopback MTU. Calculate the
	     * maximum MTU here for use in the slow start code below.
	     */
	    /* Did peer restart with older RX version? */
	    if (peer->maxDgramPackets > 1) {
		peer->maxDgramPackets = 1;
	    }
	} else if (np->length >=
		   rx_AckDataSize(ap->nAcks) + 4 * sizeof(afs_int32)) {
	    /* AFS 3.5 */
	    rx_packetread(np,
			  rx_AckDataSize(ap->nAcks) + 2 * (int)sizeof(afs_int32),
			  sizeof(afs_int32), &tSize);
	    tSize = (afs_uint32) ntohl(tSize);
	    /*
	     * As of AFS 3.5 we set the send window to match the receive window.
	     */
	    if (tSize < call->twind) {
		call->twind = tSize;
		call->conn->twind[call->channel] = call->twind;
		call->ssthresh = MIN(call->twind, call->ssthresh);
	    } else if (tSize > call->twind) {
		call->twind = tSize;
		call->conn->twind[call->channel] = call->twind;
	    }

	    /*
	     * As of AFS 3.5, a jumbogram is more than one fixed size
	     * packet transmitted in a single UDP datagram. If the remote
	     * MTU is smaller than our local MTU then never send a datagram
	     * larger than the natural MTU.
	     */
	    rx_packetread(np,
			  rx_AckDataSize(ap->nAcks) + 3 * (int)sizeof(afs_int32),
			  (int)sizeof(afs_int32), &tSize);
	    maxDgramPackets = (afs_uint32) ntohl(tSize);
	    maxDgramPackets = MIN(maxDgramPackets, rxi_nDgramPackets);
	    maxDgramPackets =
		MIN(maxDgramPackets, (int)(peer->ifDgramPackets));
	    maxDgramPackets = MIN(maxDgramPackets, tSize);
	    if (maxDgramPackets > 1) {
		peer->maxDgramPackets = maxDgramPackets;
		call->MTU = RX_JUMBOBUFFERSIZE + RX_HEADER_SIZE;
	    } else {
		peer->maxDgramPackets = 1;
		call->MTU = peer->natMTU;
	    }
	} else if (peer->maxDgramPackets > 1) {
	    /* Restarted with lower version of RX */
	    peer->maxDgramPackets = 1;
	}
    } else if (peer->maxDgramPackets > 1
	       || peer->maxMTU != OLD_MAX_PACKET_SIZE) {
	/* Restarted with lower version of RX */
	peer->maxMTU = OLD_MAX_PACKET_SIZE;
	peer->natMTU = OLD_MAX_PACKET_SIZE;
	peer->MTU = OLD_MAX_PACKET_SIZE;
	peer->maxDgramPackets = 1;
	peer->nDgramPackets = 1;
	peer->congestSeq++;
	call->MTU = OLD_MAX_PACKET_SIZE;
    }

    if (nNacked) {
	/*
	 * Calculate how many datagrams were successfully received after
	 * the first missing packet and adjust the negative ack counter
	 * accordingly.
	 */
	call->nAcks = 0;
	call->nNacks++;
	nNacked = (nNacked + call->nDgramPackets - 1) / call->nDgramPackets;
	if (call->nNacks < nNacked) {
	    call->nNacks = nNacked;
	}
    } else {
	call->nAcks += newAckCount;
	call->nNacks = 0;
    }

    if (call->flags & RX_CALL_FAST_RECOVER) {
	if (nNacked) {
	    call->cwind = MIN((int)(call->cwind + 1), rx_maxSendWindow);
	} else {
	    call->flags &= ~RX_CALL_FAST_RECOVER;
	    call->cwind = call->nextCwind;
	    call->nextCwind = 0;
	    call->nAcks = 0;
	}
	call->nCwindAcks = 0;
    } else if (nNacked && call->nNacks >= (u_short) rx_nackThreshold) {
	/* Three negative acks in a row trigger congestion recovery */
#ifdef  AFS_GLOBAL_RXLOCK_KERNEL
	MUTEX_EXIT(&peer->peer_lock);
	if (call->flags & RX_CALL_FAST_RECOVER_WAIT) {
	    /* someone else is waiting to start recovery */
	    return np;
	}
	call->flags |= RX_CALL_FAST_RECOVER_WAIT;
	rxi_WaitforTQBusy(call);
	MUTEX_ENTER(&peer->peer_lock);
#endif /* AFS_GLOBAL_RXLOCK_KERNEL */
	call->flags &= ~RX_CALL_FAST_RECOVER_WAIT;
	call->flags |= RX_CALL_FAST_RECOVER;
	call->ssthresh = MAX(4, MIN((int)call->cwind, (int)call->twind)) >> 1;
	call->cwind =
	    MIN((int)(call->ssthresh + rx_nackThreshold), rx_maxSendWindow);
	call->nDgramPackets = MAX(2, (int)call->nDgramPackets) >> 1;
	call->nextCwind = call->ssthresh;
	call->nAcks = 0;
	call->nNacks = 0;
	peer->MTU = call->MTU;
	peer->cwind = call->nextCwind;
	peer->nDgramPackets = call->nDgramPackets;
	peer->congestSeq++;
	call->congestSeq = peer->congestSeq;
	/* Reset the resend times on the packets that were nacked
	 * so we will retransmit as soon as the window permits*/
	for (acked = 0, queue_ScanBackwards(&call->tq, tp, nxp, rx_packet)) {
	    if (acked) {
		if (!(tp->flags & RX_PKTFLAG_ACKED)) {
		    clock_Zero(&tp->retryTime);
		}
	    } else if (tp->flags & RX_PKTFLAG_ACKED) {
		acked = 1;
	    }
	}
    } else {
	/* If cwind is smaller than ssthresh, then increase
	 * the window one packet for each ack we receive (exponential
	 * growth).
	 * If cwind is greater than or equal to ssthresh then increase
	 * the congestion window by one packet for each cwind acks we
	 * receive (linear growth).  */
	if (call->cwind < call->ssthresh) {
	    call->cwind =
		MIN((int)call->ssthresh, (int)(call->cwind + newAckCount));
	    call->nCwindAcks = 0;
	} else {
	    call->nCwindAcks += newAckCount;
	    if (call->nCwindAcks >= call->cwind) {
		call->nCwindAcks = 0;
		call->cwind = MIN((int)(call->cwind + 1), rx_maxSendWindow);
	    }
	}
	/*
	 * If we have received several acknowledgements in a row then
	 * it is time to increase the size of our datagrams
	 */
	if ((int)call->nAcks > rx_nDgramThreshold) {
	    if (peer->maxDgramPackets > 1) {
		if (call->nDgramPackets < peer->maxDgramPackets) {
		    call->nDgramPackets++;
		}
		call->MTU = RX_HEADER_SIZE + RX_JUMBOBUFFERSIZE;
	    } else if (call->MTU < peer->maxMTU) {
		/* don't upgrade if we can't handle it */
		if ((call->nDgramPackets == 1) && (call->MTU >= peer->ifMTU))
		    call->MTU = peer->ifMTU;
		else {
		    call->MTU += peer->natMTU;
		    call->MTU = MIN(call->MTU, peer->maxMTU);
		}
	    }
	    call->nAcks = 0;
	}
    }

    MUTEX_EXIT(&peer->peer_lock);	/* rxi_Start will lock peer. */

    /* Servers need to hold the call until all response packets have
     * been acknowledged. Soft acks are good enough since clients
     * are not allowed to clear their receive queues. */
    if (call->state == RX_STATE_HOLD
	&& call->tfirst + call->nSoftAcked >= call->tnext) {
	call->state = RX_STATE_DALLY;
	rxi_ClearTransmitQueue(call, 0);
        rxevent_Cancel(call->keepAliveEvent, call, RX_CALL_REFCOUNT_ALIVE);
    } else if (!queue_IsEmpty(&call->tq)) {
	rxi_Start(0, call, 0, istack);
    }
    return np;
}

/* Received a response to a challenge packet */
struct rx_packet *
rxi_ReceiveResponsePacket(struct rx_connection *conn,
			  struct rx_packet *np, int istack)
{
    int error;

    /* Ignore the packet if we're the client */
    if (conn->type == RX_CLIENT_CONNECTION)
	return np;

    /* If already authenticated, ignore the packet (it's probably a retry) */
    if (RXS_CheckAuthentication(conn->securityObject, conn) == 0)
	return np;

    /* Otherwise, have the security object evaluate the response packet */
    error = RXS_CheckResponse(conn->securityObject, conn, np);
    if (error) {
	/* If the response is invalid, reset the connection, sending
	 * an abort to the peer */
#ifndef KERNEL
	rxi_Delay(1);
#endif
	rxi_ConnectionError(conn, error);
	MUTEX_ENTER(&conn->conn_data_lock);
	np = rxi_SendConnectionAbort(conn, np, istack, 0);
	MUTEX_EXIT(&conn->conn_data_lock);
	return np;
    } else {
	/* If the response is valid, any calls waiting to attach
	 * servers can now do so */
	int i;

	for (i = 0; i < RX_MAXCALLS; i++) {
	    struct rx_call *call = conn->call[i];
	    if (call) {
		MUTEX_ENTER(&call->lock);
		if (call->state == RX_STATE_PRECALL)
		    rxi_AttachServerProc(call, (osi_socket) - 1, NULL, NULL);
		/* tnop can be null if newcallp is null */
		MUTEX_EXIT(&call->lock);
	    }
	}

	/* Update the peer reachability information, just in case
	 * some calls went into attach-wait while we were waiting
	 * for authentication..
	 */
	rxi_UpdatePeerReach(conn, NULL);
    }
    return np;
}

/* A client has received an authentication challenge: the security
 * object is asked to cough up a respectable response packet to send
 * back to the server.  The server is responsible for retrying the
 * challenge if it fails to get a response. */

struct rx_packet *
rxi_ReceiveChallengePacket(struct rx_connection *conn,
			   struct rx_packet *np, int istack)
{
    int error;

    /* Ignore the challenge if we're the server */
    if (conn->type == RX_SERVER_CONNECTION)
	return np;

    /* Ignore the challenge if the connection is otherwise idle; someone's
     * trying to use us as an oracle. */
    if (!rxi_HasActiveCalls(conn))
	return np;

    /* Send the security object the challenge packet.  It is expected to fill
     * in the response. */
    error = RXS_GetResponse(conn->securityObject, conn, np);

    /* If the security object is unable to return a valid response, reset the
     * connection and send an abort to the peer.  Otherwise send the response
     * packet to the peer connection. */
    if (error) {
	rxi_ConnectionError(conn, error);
	MUTEX_ENTER(&conn->conn_data_lock);
	np = rxi_SendConnectionAbort(conn, np, istack, 0);
	MUTEX_EXIT(&conn->conn_data_lock);
    } else {
	np = rxi_SendSpecial((struct rx_call *)0, conn, np,
			     RX_PACKET_TYPE_RESPONSE, NULL, -1, istack);
    }
    return np;
}


/* Find an available server process to service the current request in
 * the given call structure.  If one isn't available, queue up this
 * call so it eventually gets one */
void
rxi_AttachServerProc(struct rx_call *call,
		     osi_socket socket, int *tnop,
		     struct rx_call **newcallp)
{
    struct rx_serverQueueEntry *sq;
    struct rx_service *service = call->conn->service;
    int haveQuota = 0;

    /* May already be attached */
    if (call->state == RX_STATE_ACTIVE)
	return;

    MUTEX_ENTER(&rx_serverPool_lock);

    haveQuota = QuotaOK(service);
    if ((!haveQuota) || queue_IsEmpty(&rx_idleServerQueue)) {
	/* If there are no processes available to service this call,
	 * put the call on the incoming call queue (unless it's
	 * already on the queue).
	 */
#ifdef RX_ENABLE_LOCKS
	if (haveQuota)
	    ReturnToServerPool(service);
#endif /* RX_ENABLE_LOCKS */

	if (!(call->flags & RX_CALL_WAIT_PROC)) {
	    call->flags |= RX_CALL_WAIT_PROC;
	    rx_atomic_inc(&rx_nWaiting);
	    rx_atomic_inc(&rx_nWaited);
	    rxi_calltrace(RX_CALL_ARRIVAL, call);
	    SET_CALL_QUEUE_LOCK(call, &rx_serverPool_lock);
	    queue_Append(&rx_incomingCallQueue, call);
	}
    } else {
	sq = queue_First(&rx_idleServerQueue, rx_serverQueueEntry);

	/* If hot threads are enabled, and both newcallp and sq->socketp
	 * are non-null, then this thread will process the call, and the
	 * idle server thread will start listening on this threads socket.
	 */
	queue_Remove(sq);
	if (rx_enable_hot_thread && newcallp && sq->socketp) {
	    *newcallp = call;
	    *tnop = sq->tno;
	    *sq->socketp = socket;
	    clock_GetTime(&call->startTime);
            MUTEX_ENTER(&rx_refcnt_mutex);
	    CALL_HOLD(call, RX_CALL_REFCOUNT_BEGIN);
            MUTEX_EXIT(&rx_refcnt_mutex);
	} else {
	    sq->newcall = call;
	}
	if (call->flags & RX_CALL_WAIT_PROC) {
	    /* Conservative:  I don't think this should happen */
	    call->flags &= ~RX_CALL_WAIT_PROC;
	    if (queue_IsOnQueue(call)) {
		queue_Remove(call);

		rx_atomic_dec(&rx_nWaiting);
	    }
	}
	call->state = RX_STATE_ACTIVE;
	call->mode = RX_MODE_RECEIVING;
#ifdef RX_KERNEL_TRACE
	{
	    int glockOwner = ISAFS_GLOCK();
	    if (!glockOwner)
		AFS_GLOCK();
	    afs_Trace3(afs_iclSetp, CM_TRACE_WASHERE, ICL_TYPE_STRING,
		       __FILE__, ICL_TYPE_INT32, __LINE__, ICL_TYPE_POINTER,
		       call);
	    if (!glockOwner)
		AFS_GUNLOCK();
	}
#endif
	if (call->flags & RX_CALL_CLEARED) {
	    /* send an ack now to start the packet flow up again */
	    call->flags &= ~RX_CALL_CLEARED;
	    rxi_SendAck(call, 0, 0, RX_ACK_DELAY, 0);
	}
#ifdef	RX_ENABLE_LOCKS
	CV_SIGNAL(&sq->cv);
#else
	service->nRequestsRunning++;
        MUTEX_ENTER(&rx_quota_mutex);
	if (service->nRequestsRunning <= service->minProcs)
	    rxi_minDeficit--;
	rxi_availProcs--;
        MUTEX_EXIT(&rx_quota_mutex);
	osi_rxWakeup(sq);
#endif
    }
    MUTEX_EXIT(&rx_serverPool_lock);
}

/* Delay the sending of an acknowledge event for a short while, while
 * a new call is being prepared (in the case of a client) or a reply
 * is being prepared (in the case of a server).  Rather than sending
 * an ack packet, an ACKALL packet is sent. */
void
rxi_AckAll(struct rxevent *event, struct rx_call *call, char *dummy)
{
#ifdef RX_ENABLE_LOCKS
    if (event) {
	MUTEX_ENTER(&call->lock);
	call->delayedAckEvent = NULL;
        MUTEX_ENTER(&rx_refcnt_mutex);
	CALL_RELE(call, RX_CALL_REFCOUNT_ACKALL);
        MUTEX_EXIT(&rx_refcnt_mutex);
    }
    rxi_SendSpecial(call, call->conn, (struct rx_packet *)0,
		    RX_PACKET_TYPE_ACKALL, NULL, 0, 0);
    if (event)
	MUTEX_EXIT(&call->lock);
#else /* RX_ENABLE_LOCKS */
    if (event)
	call->delayedAckEvent = NULL;
    rxi_SendSpecial(call, call->conn, (struct rx_packet *)0,
		    RX_PACKET_TYPE_ACKALL, NULL, 0, 0);
#endif /* RX_ENABLE_LOCKS */
}

void
rxi_SendDelayedAck(struct rxevent *event, void *arg1, void *unused)
{
    struct rx_call *call = arg1;
#ifdef RX_ENABLE_LOCKS
    if (event) {
	MUTEX_ENTER(&call->lock);
	if (event == call->delayedAckEvent)
	    call->delayedAckEvent = NULL;
        MUTEX_ENTER(&rx_refcnt_mutex);
	CALL_RELE(call, RX_CALL_REFCOUNT_DELAY);
        MUTEX_EXIT(&rx_refcnt_mutex);
    }
    (void)rxi_SendAck(call, 0, 0, RX_ACK_DELAY, 0);
    if (event)
	MUTEX_EXIT(&call->lock);
#else /* RX_ENABLE_LOCKS */
    if (event)
	call->delayedAckEvent = NULL;
    (void)rxi_SendAck(call, 0, 0, RX_ACK_DELAY, 0);
#endif /* RX_ENABLE_LOCKS */
}


#ifdef RX_ENABLE_LOCKS
/* Set ack in all packets in transmit queue. rxi_Start will deal with
 * clearing them out.
 */
static void
rxi_SetAcksInTransmitQueue(struct rx_call *call)
{
    struct rx_packet *p, *tp;
    int someAcked = 0;

    for (queue_Scan(&call->tq, p, tp, rx_packet)) {
	p->flags |= RX_PKTFLAG_ACKED;
	someAcked = 1;
    }
    if (someAcked) {
	call->flags |= RX_CALL_TQ_CLEARME;
	call->flags |= RX_CALL_TQ_SOME_ACKED;
    }

    rxevent_Cancel(call->resendEvent, call, RX_CALL_REFCOUNT_RESEND);
    call->tfirst = call->tnext;
    call->nSoftAcked = 0;

    if (call->flags & RX_CALL_FAST_RECOVER) {
	call->flags &= ~RX_CALL_FAST_RECOVER;
	call->cwind = call->nextCwind;
	call->nextCwind = 0;
    }

    CV_SIGNAL(&call->cv_twind);
}
#endif /* RX_ENABLE_LOCKS */

/* Clear out the transmit queue for the current call (all packets have
 * been received by peer) */
void
rxi_ClearTransmitQueue(struct rx_call *call, int force)
{
#ifdef	AFS_GLOBAL_RXLOCK_KERNEL
    struct rx_packet *p, *tp;

    if (!force && (call->flags & RX_CALL_TQ_BUSY)) {
	int someAcked = 0;
	for (queue_Scan(&call->tq, p, tp, rx_packet)) {
	    p->flags |= RX_PKTFLAG_ACKED;
	    someAcked = 1;
	}
	if (someAcked) {
	    call->flags |= RX_CALL_TQ_CLEARME;
	    call->flags |= RX_CALL_TQ_SOME_ACKED;
	}
    } else {
#endif /* AFS_GLOBAL_RXLOCK_KERNEL */
#ifdef RXDEBUG_PACKET
        call->tqc -=
#endif /* RXDEBUG_PACKET */
            rxi_FreePackets(0, &call->tq);
	if (call->tqWaiters || (call->flags & RX_CALL_TQ_WAIT)) {
#ifdef RX_ENABLE_LOCKS
	    CV_BROADCAST(&call->cv_tq);
#else /* RX_ENABLE_LOCKS */
	    osi_rxWakeup(&call->tq);
#endif /* RX_ENABLE_LOCKS */
	}
#ifdef	AFS_GLOBAL_RXLOCK_KERNEL
	call->flags &= ~RX_CALL_TQ_CLEARME;
    }
#endif /* AFS_GLOBAL_RXLOCK_KERNEL */

    rxevent_Cancel(call->resendEvent, call, RX_CALL_REFCOUNT_RESEND);
    call->tfirst = call->tnext;	/* implicitly acknowledge all data already sent */
    call->nSoftAcked = 0;

    if (call->flags & RX_CALL_FAST_RECOVER) {
	call->flags &= ~RX_CALL_FAST_RECOVER;
	call->cwind = call->nextCwind;
    }
#ifdef	RX_ENABLE_LOCKS
    CV_SIGNAL(&call->cv_twind);
#else
    osi_rxWakeup(&call->twind);
#endif
}

void
rxi_ClearReceiveQueue(struct rx_call *call)
{
    if (queue_IsNotEmpty(&call->rq)) {
        u_short count;

        count = rxi_FreePackets(0, &call->rq);
	rx_packetReclaims += count;
#ifdef RXDEBUG_PACKET
        call->rqc -= count;
        if ( call->rqc != 0 )
            dpf(("rxi_ClearReceiveQueue call %"AFS_PTR_FMT" rqc %u != 0", call, call->rqc));
#endif
	call->flags &= ~(RX_CALL_RECEIVE_DONE | RX_CALL_HAVE_LAST);
    }
    if (call->state == RX_STATE_PRECALL) {
	call->flags |= RX_CALL_CLEARED;
    }
}

/* Send an abort packet for the specified call */
struct rx_packet *
rxi_SendCallAbort(struct rx_call *call, struct rx_packet *packet,
		  int istack, int force)
{
    afs_int32 error;
    struct clock when, now;

    if (!call->error)
	return packet;

    /* Clients should never delay abort messages */
    if (rx_IsClientConn(call->conn))
	force = 1;

    if (call->abortCode != call->error) {
	call->abortCode = call->error;
	call->abortCount = 0;
    }

    if (force || rxi_callAbortThreshhold == 0
	|| call->abortCount < rxi_callAbortThreshhold) {
	if (call->delayedAbortEvent) {
	    rxevent_Cancel(call->delayedAbortEvent, call,
			   RX_CALL_REFCOUNT_ABORT);
	}
	error = htonl(call->error);
	call->abortCount++;
	packet =
	    rxi_SendSpecial(call, call->conn, packet, RX_PACKET_TYPE_ABORT,
			    (char *)&error, sizeof(error), istack);
    } else if (!call->delayedAbortEvent) {
	clock_GetTime(&now);
	when = now;
	clock_Addmsec(&when, rxi_callAbortDelay);
        MUTEX_ENTER(&rx_refcnt_mutex);
	CALL_HOLD(call, RX_CALL_REFCOUNT_ABORT);
        MUTEX_EXIT(&rx_refcnt_mutex);
	call->delayedAbortEvent =
	    rxevent_PostNow(&when, &now, rxi_SendDelayedCallAbort, call, 0);
    }
    return packet;
}

/* Send an abort packet for the specified connection.  Packet is an
 * optional pointer to a packet that can be used to send the abort.
 * Once the number of abort messages reaches the threshhold, an
 * event is scheduled to send the abort. Setting the force flag
 * overrides sending delayed abort messages.
 *
 * NOTE: Called with conn_data_lock held. conn_data_lock is dropped
 *       to send the abort packet.
 */
struct rx_packet *
rxi_SendConnectionAbort(struct rx_connection *conn,
			struct rx_packet *packet, int istack, int force)
{
    afs_int32 error;
    struct clock when, now;

    if (!conn->error)
	return packet;

    /* Clients should never delay abort messages */
    if (rx_IsClientConn(conn))
	force = 1;

    if (force || rxi_connAbortThreshhold == 0
	|| conn->abortCount < rxi_connAbortThreshhold) {
	if (conn->delayedAbortEvent) {
	    rxevent_Cancel(conn->delayedAbortEvent, (struct rx_call *)0, 0);
	}
	error = htonl(conn->error);
	conn->abortCount++;
	MUTEX_EXIT(&conn->conn_data_lock);
	packet =
	    rxi_SendSpecial((struct rx_call *)0, conn, packet,
			    RX_PACKET_TYPE_ABORT, (char *)&error,
			    sizeof(error), istack);
	MUTEX_ENTER(&conn->conn_data_lock);
    } else if (!conn->delayedAbortEvent) {
	clock_GetTime(&now);
	when = now;
	clock_Addmsec(&when, rxi_connAbortDelay);
	conn->delayedAbortEvent =
	    rxevent_PostNow(&when, &now, rxi_SendDelayedConnAbort, conn, 0);
    }
    return packet;
}

/* Associate an error all of the calls owned by a connection.  Called
 * with error non-zero.  This is only for really fatal things, like
 * bad authentication responses.  The connection itself is set in
 * error at this point, so that future packets received will be
 * rejected. */
void
rxi_ConnectionError(struct rx_connection *conn,
		    afs_int32 error)
{
    if (error) {
	int i;

	dpf(("rxi_ConnectionError conn %"AFS_PTR_FMT" error %d", conn, error));

	MUTEX_ENTER(&conn->conn_data_lock);
	if (conn->challengeEvent)
	    rxevent_Cancel(conn->challengeEvent, (struct rx_call *)0, 0);
	if (conn->natKeepAliveEvent)
	    rxevent_Cancel(conn->natKeepAliveEvent, (struct rx_call *)0, 0);
	if (conn->checkReachEvent) {
	    rxevent_Cancel(conn->checkReachEvent, (struct rx_call *)0, 0);
	    conn->checkReachEvent = 0;
	    conn->flags &= ~RX_CONN_ATTACHWAIT;
            MUTEX_ENTER(&rx_refcnt_mutex);
	    conn->refCount--;
            MUTEX_EXIT(&rx_refcnt_mutex);
	}
	MUTEX_EXIT(&conn->conn_data_lock);
	for (i = 0; i < RX_MAXCALLS; i++) {
	    struct rx_call *call = conn->call[i];
	    if (call) {
		MUTEX_ENTER(&call->lock);
		rxi_CallError(call, error);
		MUTEX_EXIT(&call->lock);
	    }
	}
	conn->error = error;
        if (rx_stats_active)
            rx_atomic_inc(&rx_stats.fatalErrors);
    }
}

void
rxi_CallError(struct rx_call *call, afs_int32 error)
{
#ifdef DEBUG
    osirx_AssertMine(&call->lock, "rxi_CallError");
#endif
    dpf(("rxi_CallError call %"AFS_PTR_FMT" error %d call->error %d", call, error, call->error));
    if (call->error)
	error = call->error;

#ifdef AFS_GLOBAL_RXLOCK_KERNEL
    if (!((call->flags & RX_CALL_TQ_BUSY) || (call->tqWaiters > 0))) {
	rxi_ResetCall(call, 0);
    }
#else
    rxi_ResetCall(call, 0);
#endif
    call->error = error;
    call->mode = RX_MODE_ERROR;
}

/* Reset various fields in a call structure, and wakeup waiting
 * processes.  Some fields aren't changed: state & mode are not
 * touched (these must be set by the caller), and bufptr, nLeft, and
 * nFree are not reset, since these fields are manipulated by
 * unprotected macros, and may only be reset by non-interrupting code.
 */
#ifdef ADAPT_WINDOW
/* this code requires that call->conn be set properly as a pre-condition. */
#endif /* ADAPT_WINDOW */

void
rxi_ResetCall(struct rx_call *call, int newcall)
{
    int flags;
    struct rx_peer *peer;
    struct rx_packet *packet;
#ifdef DEBUG
    osirx_AssertMine(&call->lock, "rxi_ResetCall");
#endif
    dpf(("rxi_ResetCall(call %"AFS_PTR_FMT", newcall %d)\n", call, newcall));

    /* Notify anyone who is waiting for asynchronous packet arrival */
    if (call->arrivalProc) {
	(*call->arrivalProc) (call, call->arrivalProcHandle,
			      call->arrivalProcArg);
	call->arrivalProc = (void (*)())0;
    }

    if (call->delayedAbortEvent) {
	rxevent_Cancel(call->delayedAbortEvent, call, RX_CALL_REFCOUNT_ABORT);
	packet = rxi_AllocPacket(RX_PACKET_CLASS_SPECIAL);
	if (packet) {
	    rxi_SendCallAbort(call, packet, 0, 1);
	    rxi_FreePacket(packet);
	}
    }

    /*
     * Update the peer with the congestion information in this call
     * so other calls on this connection can pick up where this call
     * left off. If the congestion sequence numbers don't match then
     * another call experienced a retransmission.
     */
    peer = call->conn->peer;
    MUTEX_ENTER(&peer->peer_lock);
    if (!newcall) {
	if (call->congestSeq == peer->congestSeq) {
	    peer->cwind = MAX(peer->cwind, call->cwind);
	    peer->MTU = MAX(peer->MTU, call->MTU);
	    peer->nDgramPackets =
		MAX(peer->nDgramPackets, call->nDgramPackets);
	}
    } else {
	call->abortCode = 0;
	call->abortCount = 0;
    }
    if (peer->maxDgramPackets > 1) {
	call->MTU = RX_HEADER_SIZE + RX_JUMBOBUFFERSIZE;
    } else {
	call->MTU = peer->MTU;
    }
    call->cwind = MIN((int)peer->cwind, (int)peer->nDgramPackets);
    call->ssthresh = rx_maxSendWindow;
    call->nDgramPackets = peer->nDgramPackets;
    call->congestSeq = peer->congestSeq;
    MUTEX_EXIT(&peer->peer_lock);

    flags = call->flags;
#ifdef	AFS_GLOBAL_RXLOCK_KERNEL
    rxi_WaitforTQBusy(call);
#endif /* AFS_GLOBAL_RXLOCK_KERNEL */

    rxi_ClearTransmitQueue(call, 1);
    if (call->tqWaiters || (flags & RX_CALL_TQ_WAIT)) {
        dpf(("rcall %"AFS_PTR_FMT" has %d waiters and flags %d\n", call, call->tqWaiters, call->flags));
    }
    call->flags = 0;

    rxi_ClearReceiveQueue(call);
    /* why init the queue if you just emptied it? queue_Init(&call->rq); */


    call->error = 0;
    call->twind = call->conn->twind[call->channel];
    call->rwind = call->conn->rwind[call->channel];
    call->nSoftAcked = 0;
    call->nextCwind = 0;
    call->nAcks = 0;
    call->nNacks = 0;
    call->nCwindAcks = 0;
    call->nSoftAcks = 0;
    call->nHardAcks = 0;

    call->tfirst = call->rnext = call->tnext = 1;
    call->rprev = 0;
    call->lastAcked = 0;
    call->localStatus = call->remoteStatus = 0;

    if (flags & RX_CALL_READER_WAIT) {
#ifdef	RX_ENABLE_LOCKS
	CV_BROADCAST(&call->cv_rq);
#else
	osi_rxWakeup(&call->rq);
#endif
    }
    if (flags & RX_CALL_WAIT_PACKETS) {
	MUTEX_ENTER(&rx_freePktQ_lock);
	rxi_PacketsUnWait();	/* XXX */
	MUTEX_EXIT(&rx_freePktQ_lock);
    }
#ifdef	RX_ENABLE_LOCKS
    CV_SIGNAL(&call->cv_twind);
#else
    if (flags & RX_CALL_WAIT_WINDOW_ALLOC)
	osi_rxWakeup(&call->twind);
#endif

#ifdef RX_ENABLE_LOCKS
    /* The following ensures that we don't mess with any queue while some
     * other thread might also be doing so. The call_queue_lock field is
     * is only modified under the call lock. If the call is in the process
     * of being removed from a queue, the call is not locked until the
     * the queue lock is dropped and only then is the call_queue_lock field
     * zero'd out. So it's safe to lock the queue if call_queue_lock is set.
     * Note that any other routine which removes a call from a queue has to
     * obtain the queue lock before examing the queue and removing the call.
     */
    if (call->call_queue_lock) {
	MUTEX_ENTER(call->call_queue_lock);
	if (queue_IsOnQueue(call)) {
	    queue_Remove(call);
	    if (flags & RX_CALL_WAIT_PROC) {
		rx_atomic_dec(&rx_nWaiting);
	    }
	}
	MUTEX_EXIT(call->call_queue_lock);
	CLEAR_CALL_QUEUE_LOCK(call);
    }
#else /* RX_ENABLE_LOCKS */
    if (queue_IsOnQueue(call)) {
	queue_Remove(call);
	if (flags & RX_CALL_WAIT_PROC)
	    rx_atomic_dec(&rx_nWaiting);
    }
#endif /* RX_ENABLE_LOCKS */

    rxi_KeepAliveOff(call);
    rxevent_Cancel(call->delayedAckEvent, call, RX_CALL_REFCOUNT_DELAY);
}

/* Send an acknowledge for the indicated packet (seq,serial) of the
 * indicated call, for the indicated reason (reason).  This
 * acknowledge will specifically acknowledge receiving the packet, and
 * will also specify which other packets for this call have been
 * received.  This routine returns the packet that was used to the
 * caller.  The caller is responsible for freeing it or re-using it.
 * This acknowledgement also returns the highest sequence number
 * actually read out by the higher level to the sender; the sender
 * promises to keep around packets that have not been read by the
 * higher level yet (unless, of course, the sender decides to abort
 * the call altogether).  Any of p, seq, serial, pflags, or reason may
 * be set to zero without ill effect.  That is, if they are zero, they
 * will not convey any information.
 * NOW there is a trailer field, after the ack where it will safely be
 * ignored by mundanes, which indicates the maximum size packet this
 * host can swallow.  */
/*
    struct rx_packet *optionalPacket;  use to send ack (or null)
    int	seq;			 Sequence number of the packet we are acking
    int	serial;			 Serial number of the packet
    int	pflags;			 Flags field from packet header
    int	reason;			 Reason an acknowledge was prompted
*/

struct rx_packet *
rxi_SendAck(struct rx_call *call,
	    struct rx_packet *optionalPacket, int serial, int reason,
	    int istack)
{
    struct rx_ackPacket *ap;
    struct rx_packet *rqp;
    struct rx_packet *nxp;	/* For queue_Scan */
    struct rx_packet *p;
    u_char offset;
    afs_int32 templ;
    afs_uint32 padbytes = 0;
#ifdef RX_ENABLE_TSFPQ
    struct rx_ts_info_t * rx_ts_info;
#endif

    /*
     * Open the receive window once a thread starts reading packets
     */
    if (call->rnext > 1) {
	call->conn->rwind[call->channel] = call->rwind = rx_maxReceiveWindow;
    }

    /* Don't attempt to grow MTU if this is a critical ping */
    if (reason == RX_ACK_MTU) {
	/* keep track of per-call attempts, if we're over max, do in small
	 * otherwise in larger? set a size to increment by, decrease
	 * on failure, here?
	 */
	if (call->conn->peer->maxPacketSize &&
	    (call->conn->peer->maxPacketSize < OLD_MAX_PACKET_SIZE
	     +RX_IPUDP_SIZE))
	    padbytes = call->conn->peer->maxPacketSize+16;
	else
	    padbytes = call->conn->peer->maxMTU + 128;

	/* do always try a minimum size ping */
	padbytes = MAX(padbytes, RX_MIN_PACKET_SIZE+RX_IPUDP_SIZE+4);

	/* subtract the ack payload */
	padbytes -= (rx_AckDataSize(call->rwind) + 4 * sizeof(afs_int32));
	reason = RX_ACK_PING;
    }

    call->nHardAcks = 0;
    call->nSoftAcks = 0;
    if (call->rnext > call->lastAcked)
	call->lastAcked = call->rnext;
    p = optionalPacket;

    if (p) {
	rx_computelen(p, p->length);	/* reset length, you never know */
    } /* where that's been...         */
#ifdef RX_ENABLE_TSFPQ
    else {
        RX_TS_INFO_GET(rx_ts_info);
        if ((p = rx_ts_info->local_special_packet)) {
            rx_computelen(p, p->length);
        } else if ((p = rxi_AllocPacket(RX_PACKET_CLASS_SPECIAL))) {
            rx_ts_info->local_special_packet = p;
        } else { /* We won't send the ack, but don't panic. */
            return optionalPacket;
        }
    }
#else
    else if (!(p = rxi_AllocPacket(RX_PACKET_CLASS_SPECIAL))) {
	/* We won't send the ack, but don't panic. */
	return optionalPacket;
    }
#endif

    templ = padbytes +
	rx_AckDataSize(call->rwind) + 4 * sizeof(afs_int32) -
	rx_GetDataSize(p);
    if (templ > 0) {
	if (rxi_AllocDataBuf(p, templ, RX_PACKET_CLASS_SPECIAL) > 0) {
#ifndef RX_ENABLE_TSFPQ
	    if (!optionalPacket)
		rxi_FreePacket(p);
#endif
	    return optionalPacket;
	}
	templ = rx_AckDataSize(call->rwind) + 2 * sizeof(afs_int32);
	if (rx_Contiguous(p) < templ) {
#ifndef RX_ENABLE_TSFPQ
	    if (!optionalPacket)
		rxi_FreePacket(p);
#endif
	    return optionalPacket;
	}
    }


    /* MTUXXX failing to send an ack is very serious.  We should */
    /* try as hard as possible to send even a partial ack; it's */
    /* better than nothing. */
    ap = (struct rx_ackPacket *)rx_DataOf(p);
    ap->bufferSpace = htonl(0);	/* Something should go here, sometime */
    ap->reason = reason;

    /* The skew computation used to be bogus, I think it's better now. */
    /* We should start paying attention to skew.    XXX  */
    ap->serial = htonl(serial);
    ap->maxSkew = 0;		/* used to be peer->inPacketSkew */

    ap->firstPacket = htonl(call->rnext);	/* First packet not yet forwarded to reader */
    ap->previousPacket = htonl(call->rprev);	/* Previous packet received */

    /* No fear of running out of ack packet here because there can only be at most
     * one window full of unacknowledged packets.  The window size must be constrained
     * to be less than the maximum ack size, of course.  Also, an ack should always
     * fit into a single packet -- it should not ever be fragmented.  */
    for (offset = 0, queue_Scan(&call->rq, rqp, nxp, rx_packet)) {
	if (!rqp || !call->rq.next
	    || (rqp->header.seq > (call->rnext + call->rwind))) {
#ifndef RX_ENABLE_TSFPQ
	    if (!optionalPacket)
		rxi_FreePacket(p);
#endif
	    rxi_CallError(call, RX_CALL_DEAD);
	    return optionalPacket;
	}

	while (rqp->header.seq > call->rnext + offset)
	    ap->acks[offset++] = RX_ACK_TYPE_NACK;
	ap->acks[offset++] = RX_ACK_TYPE_ACK;

	if ((offset > (u_char) rx_maxReceiveWindow) || (offset > call->rwind)) {
#ifndef RX_ENABLE_TSFPQ
	    if (!optionalPacket)
		rxi_FreePacket(p);
#endif
	    rxi_CallError(call, RX_CALL_DEAD);
	    return optionalPacket;
	}
    }

    ap->nAcks = offset;
    p->length = rx_AckDataSize(offset) + 4 * sizeof(afs_int32);

    /* these are new for AFS 3.3 */
    templ = rxi_AdjustMaxMTU(call->conn->peer->ifMTU, rx_maxReceiveSize);
    templ = htonl(templ);
    rx_packetwrite(p, rx_AckDataSize(offset), sizeof(afs_int32), &templ);
    templ = htonl(call->conn->peer->ifMTU);
    rx_packetwrite(p, rx_AckDataSize(offset) + sizeof(afs_int32),
		   sizeof(afs_int32), &templ);

    /* new for AFS 3.4 */
    templ = htonl(call->rwind);
    rx_packetwrite(p, rx_AckDataSize(offset) + 2 * sizeof(afs_int32),
		   sizeof(afs_int32), &templ);

    /* new for AFS 3.5 */
    templ = htonl(call->conn->peer->ifDgramPackets);
    rx_packetwrite(p, rx_AckDataSize(offset) + 3 * sizeof(afs_int32),
		   sizeof(afs_int32), &templ);

    p->header.serviceId = call->conn->serviceId;
    p->header.cid = (call->conn->cid | call->channel);
    p->header.callNumber = *call->callNumber;
    p->header.seq = 0;
    p->header.securityIndex = call->conn->securityIndex;
    p->header.epoch = call->conn->epoch;
    p->header.type = RX_PACKET_TYPE_ACK;
    p->header.flags = RX_SLOW_START_OK;
    if (reason == RX_ACK_PING) {
	p->header.flags |= RX_REQUEST_ACK;
#ifdef ADAPT_WINDOW
	clock_GetTime(&call->pingRequestTime);
#endif
	if (padbytes) {
	    p->length = padbytes +
		rx_AckDataSize(call->rwind) + 4 * sizeof(afs_int32);

	    while (padbytes--)
		/* not fast but we can potentially use this if truncated
		 * fragments are delivered to figure out the mtu.
		 */
		rx_packetwrite(p, rx_AckDataSize(offset) + 4 *
			       sizeof(afs_int32), sizeof(afs_int32),
			       &padbytes);
	}
    }
    if (call->conn->type == RX_CLIENT_CONNECTION)
	p->header.flags |= RX_CLIENT_INITIATED;

#ifdef RXDEBUG
#ifdef AFS_NT40_ENV
    if (rxdebug_active) {
	char msg[512];
	size_t len;

	len = _snprintf(msg, sizeof(msg),
			"tid[%d] SACK: reason %s serial %u previous %u seq %u first %u acks %u space %u ",
			 GetCurrentThreadId(), rx_ack_reason(ap->reason),
			 ntohl(ap->serial), ntohl(ap->previousPacket),
			 (unsigned int)p->header.seq, ntohl(ap->firstPacket),
			 ap->nAcks, ntohs(ap->bufferSpace) );
	if (ap->nAcks) {
	    int offset;

	    for (offset = 0; offset < ap->nAcks && len < sizeof(msg); offset++)
		msg[len++] = (ap->acks[offset] == RX_ACK_TYPE_NACK ? '-' : '*');
	}
	msg[len++]='\n';
	msg[len] = '\0';
	OutputDebugString(msg);
    }
#else /* AFS_NT40_ENV */
    if (rx_Log) {
	fprintf(rx_Log, "SACK: reason %x previous %u seq %u first %u ",
		ap->reason, ntohl(ap->previousPacket),
		(unsigned int)p->header.seq, ntohl(ap->firstPacket));
	if (ap->nAcks) {
	    for (offset = 0; offset < ap->nAcks; offset++)
		putc(ap->acks[offset] == RX_ACK_TYPE_NACK ? '-' : '*',
		     rx_Log);
	}
	putc('\n', rx_Log);
    }
#endif /* AFS_NT40_ENV */
#endif
    {
	int i, nbytes = p->length;

	for (i = 1; i < p->niovecs; i++) {	/* vec 0 is ALWAYS header */
	    if (nbytes <= p->wirevec[i].iov_len) {
		int savelen, saven;

		savelen = p->wirevec[i].iov_len;
		saven = p->niovecs;
		p->wirevec[i].iov_len = nbytes;
		p->niovecs = i + 1;
		rxi_Send(call, p, istack);
		p->wirevec[i].iov_len = savelen;
		p->niovecs = saven;
		break;
	    } else
		nbytes -= p->wirevec[i].iov_len;
	}
    }
    if (rx_stats_active)
        rx_atomic_inc(&rx_stats.ackPacketsSent);
#ifndef RX_ENABLE_TSFPQ
    if (!optionalPacket)
	rxi_FreePacket(p);
#endif
    return optionalPacket;	/* Return packet for re-use by caller */
}

/* Send all of the packets in the list in single datagram */
static void
rxi_SendList(struct rx_call *call, struct rx_packet **list, int len,
	     int istack, int moreFlag, struct clock *now,
	     struct clock *retryTime, int resending)
{
    int i;
    int requestAck = 0;
    int lastPacket = 0;
    struct rx_connection *conn = call->conn;
    struct rx_peer *peer = conn->peer;

    MUTEX_ENTER(&peer->peer_lock);
    peer->nSent += len;
    if (resending)
	peer->reSends += len;
    MUTEX_EXIT(&peer->peer_lock);

    if (rx_stats_active) {
        if (resending)
            rx_atomic_add(&rx_stats.dataPacketsReSent, len);
        else
            rx_atomic_add(&rx_stats.dataPacketsSent, len);
    }

    if (list[len - 1]->header.flags & RX_LAST_PACKET) {
	lastPacket = 1;
    }

    /* Set the packet flags and schedule the resend events */
    /* Only request an ack for the last packet in the list */
    for (i = 0; i < len; i++) {
	list[i]->retryTime = *retryTime;
	if (list[i]->header.serial) {
	    /* Exponentially backoff retry times */
	    if (list[i]->backoff < MAXBACKOFF) {
		/* so it can't stay == 0 */
		list[i]->backoff = (list[i]->backoff << 1) + 1;
	    } else
		list[i]->backoff++;
	    clock_Addmsec(&(list[i]->retryTime),
			  ((afs_uint32) list[i]->backoff) << 8);
	}

	/* Wait a little extra for the ack on the last packet */
	if (lastPacket && !(list[i]->header.flags & RX_CLIENT_INITIATED)) {
	    clock_Addmsec(&(list[i]->retryTime), 400);
	}

	/* Record the time sent */
	list[i]->timeSent = *now;

	/* Ask for an ack on retransmitted packets,  on every other packet
	 * if the peer doesn't support slow start. Ask for an ack on every
	 * packet until the congestion window reaches the ack rate. */
	if (list[i]->header.serial) {
	    requestAck = 1;
	} else {
	    /* improved RTO calculation- not Karn */
	    list[i]->firstSent = *now;
	    if (!lastPacket && (call->cwind <= (u_short) (conn->ackRate + 1)
				|| (!(call->flags & RX_CALL_SLOW_START_OK)
				    && (list[i]->header.seq & 1)))) {
		requestAck = 1;
	    }
	}

	/* Tag this packet as not being the last in this group,
	 * for the receiver's benefit */
	if (i < len - 1 || moreFlag) {
	    list[i]->header.flags |= RX_MORE_PACKETS;
	}

	/* Install the new retransmit time for the packet, and
	 * record the time sent */
	list[i]->timeSent = *now;
    }

    if (requestAck) {
	list[len - 1]->header.flags |= RX_REQUEST_ACK;
    }

    /* Since we're about to send a data packet to the peer, it's
     * safe to nuke any scheduled end-of-packets ack */
    rxevent_Cancel(call->delayedAckEvent, call, RX_CALL_REFCOUNT_DELAY);

    MUTEX_EXIT(&call->lock);
    MUTEX_ENTER(&rx_refcnt_mutex);
    CALL_HOLD(call, RX_CALL_REFCOUNT_SEND);
    MUTEX_EXIT(&rx_refcnt_mutex);
    if (len > 1) {
	rxi_SendPacketList(call, conn, list, len, istack);
    } else {
	rxi_SendPacket(call, conn, list[0], istack);
    }
    MUTEX_ENTER(&call->lock);
    MUTEX_ENTER(&rx_refcnt_mutex);
    CALL_RELE(call, RX_CALL_REFCOUNT_SEND);
    MUTEX_EXIT(&rx_refcnt_mutex);

    /* Update last send time for this call (for keep-alive
     * processing), and for the connection (so that we can discover
     * idle connections) */
    conn->lastSendTime = call->lastSendTime = clock_Sec();
    /* Let a set of retransmits trigger an idle timeout */
    if (!resending)
	call->lastSendData = call->lastSendTime;
}

/* When sending packets we need to follow these rules:
 * 1. Never send more than maxDgramPackets in a jumbogram.
 * 2. Never send a packet with more than two iovecs in a jumbogram.
 * 3. Never send a retransmitted packet in a jumbogram.
 * 4. Never send more than cwind/4 packets in a jumbogram
 * We always keep the last list we should have sent so we
 * can set the RX_MORE_PACKETS flags correctly.
 */
static void
rxi_SendXmitList(struct rx_call *call, struct rx_packet **list, int len,
		 int istack, struct clock *now, struct clock *retryTime,
		 int resending)
{
    int i, cnt, lastCnt = 0;
    struct rx_packet **listP, **lastP = 0;
    struct rx_peer *peer = call->conn->peer;
    int morePackets = 0;

    for (cnt = 0, listP = &list[0], i = 0; i < len; i++) {
	/* Does the current packet force us to flush the current list? */
	if (cnt > 0
	    && (list[i]->header.serial || (list[i]->flags & RX_PKTFLAG_ACKED)
		|| list[i]->length > RX_JUMBOBUFFERSIZE)) {
	    if (lastCnt > 0) {
		rxi_SendList(call, lastP, lastCnt, istack, 1, now, retryTime,
			     resending);
		/* If the call enters an error state stop sending, or if
		 * we entered congestion recovery mode, stop sending */
		if (call->error || (call->flags & RX_CALL_FAST_RECOVER_WAIT))
		    return;
	    }
	    lastP = listP;
	    lastCnt = cnt;
	    listP = &list[i];
	    cnt = 0;
	}
	/* Add the current packet to the list if it hasn't been acked.
	 * Otherwise adjust the list pointer to skip the current packet.  */
	if (!(list[i]->flags & RX_PKTFLAG_ACKED)) {
	    cnt++;
	    /* Do we need to flush the list? */
	    if (cnt >= (int)peer->maxDgramPackets
		|| cnt >= (int)call->nDgramPackets || cnt >= (int)call->cwind
		|| list[i]->header.serial
		|| list[i]->length != RX_JUMBOBUFFERSIZE) {
		if (lastCnt > 0) {
		    rxi_SendList(call, lastP, lastCnt, istack, 1, now,
				 retryTime, resending);
		    /* If the call enters an error state stop sending, or if
		     * we entered congestion recovery mode, stop sending */
		    if (call->error
			|| (call->flags & RX_CALL_FAST_RECOVER_WAIT))
			return;
		}
		lastP = listP;
		lastCnt = cnt;
		listP = &list[i + 1];
		cnt = 0;
	    }
	} else {
	    if (cnt != 0) {
		osi_Panic("rxi_SendList error");
	    }
	    listP = &list[i + 1];
	}
    }

    /* Send the whole list when the call is in receive mode, when
     * the call is in eof mode, when we are in fast recovery mode,
     * and when we have the last packet */
    if ((list[len - 1]->header.flags & RX_LAST_PACKET)
	|| call->mode == RX_MODE_RECEIVING || call->mode == RX_MODE_EOF
	|| (call->flags & RX_CALL_FAST_RECOVER)) {
	/* Check for the case where the current list contains
	 * an acked packet. Since we always send retransmissions
	 * in a separate packet, we only need to check the first
	 * packet in the list */
	if (cnt > 0 && !(listP[0]->flags & RX_PKTFLAG_ACKED)) {
	    morePackets = 1;
	}
	if (lastCnt > 0) {
	    rxi_SendList(call, lastP, lastCnt, istack, morePackets, now,
			 retryTime, resending);
	    /* If the call enters an error state stop sending, or if
	     * we entered congestion recovery mode, stop sending */
	    if (call->error || (call->flags & RX_CALL_FAST_RECOVER_WAIT))
		return;
	}
	if (morePackets) {
	    rxi_SendList(call, listP, cnt, istack, 0, now, retryTime,
			 resending);
	}
    } else if (lastCnt > 0) {
	rxi_SendList(call, lastP, lastCnt, istack, 0, now, retryTime,
		     resending);
    }
}

#ifdef	RX_ENABLE_LOCKS
/* Call rxi_Start, below, but with the call lock held. */
void
rxi_StartUnlocked(struct rxevent *event,
		  void *arg0, void *arg1, int istack)
{
    struct rx_call *call = arg0;

    MUTEX_ENTER(&call->lock);
    rxi_Start(event, call, arg1, istack);
    MUTEX_EXIT(&call->lock);
}
#endif /* RX_ENABLE_LOCKS */

/* This routine is called when new packets are readied for
 * transmission and when retransmission may be necessary, or when the
 * transmission window or burst count are favourable.  This should be
 * better optimized for new packets, the usual case, now that we've
 * got rid of queues of send packets. XXXXXXXXXXX */
void
rxi_Start(struct rxevent *event,
          void *arg0, void *arg1, int istack)
{
    struct rx_call *call = arg0;

    struct rx_packet *p;
    struct rx_packet *nxp;	/* Next pointer for queue_Scan */
    struct rx_peer *peer = call->conn->peer;
    struct clock now, usenow, retryTime;
    int haveEvent;
    int nXmitPackets;
    int maxXmitPackets;
    struct rx_packet **xmitList;
    int resending = 0;

    /* If rxi_Start is being called as a result of a resend event,
     * then make sure that the event pointer is removed from the call
     * structure, since there is no longer a per-call retransmission
     * event pending. */
    if (event && event == call->resendEvent) {
        MUTEX_ENTER(&rx_refcnt_mutex);
	CALL_RELE(call, RX_CALL_REFCOUNT_RESEND);
        MUTEX_EXIT(&rx_refcnt_mutex);
	call->resendEvent = NULL;
	resending = 1;
	if (queue_IsEmpty(&call->tq)) {
	    /* Nothing to do */
	    return;
	}
	/* Timeouts trigger congestion recovery */
#ifdef  AFS_GLOBAL_RXLOCK_KERNEL
	if (call->flags & RX_CALL_FAST_RECOVER_WAIT) {
	    /* someone else is waiting to start recovery */
	    return;
	}
	call->flags |= RX_CALL_FAST_RECOVER_WAIT;
	rxi_WaitforTQBusy(call);
#endif /* AFS_GLOBAL_RXLOCK_KERNEL */
	call->flags &= ~RX_CALL_FAST_RECOVER_WAIT;
	call->flags |= RX_CALL_FAST_RECOVER;
	if (peer->maxDgramPackets > 1) {
	    call->MTU = RX_JUMBOBUFFERSIZE + RX_HEADER_SIZE;
	} else {
	    call->MTU = MIN(peer->natMTU, peer->maxMTU);
	}
	call->ssthresh = MAX(4, MIN((int)call->cwind, (int)call->twind)) >> 1;
	call->nDgramPackets = 1;
	call->cwind = 1;
	call->nextCwind = 1;
	call->nAcks = 0;
	call->nNacks = 0;
	MUTEX_ENTER(&peer->peer_lock);
	peer->MTU = call->MTU;
	peer->cwind = call->cwind;
	peer->nDgramPackets = 1;
	peer->congestSeq++;
	call->congestSeq = peer->congestSeq;
	MUTEX_EXIT(&peer->peer_lock);
	/* Clear retry times on packets. Otherwise, it's possible for
	 * some packets in the queue to force resends at rates faster
	 * than recovery rates.
	 */
	for (queue_Scan(&call->tq, p, nxp, rx_packet)) {
	    if (!(p->flags & RX_PKTFLAG_ACKED)) {
		clock_Zero(&p->retryTime);
	    }
	}
    }
    if (call->error) {
#ifdef	AFS_GLOBAL_RXLOCK_KERNEL
        if (rx_stats_active)
            rx_atomic_inc(&rx_tq_debug.rxi_start_in_error);
#endif
	return;
    }

    if (queue_IsNotEmpty(&call->tq)) {	/* If we have anything to send */
	/* Get clock to compute the re-transmit time for any packets
	 * in this burst.  Note, if we back off, it's reasonable to
	 * back off all of the packets in the same manner, even if
	 * some of them have been retransmitted more times than more
	 * recent additions.
	 * Do a dance to avoid blocking after setting now. */
	MUTEX_ENTER(&peer->peer_lock);
        retryTime = peer->timeout;
	MUTEX_EXIT(&peer->peer_lock);

	clock_GetTime(&now);
	clock_Add(&retryTime, &now);
	usenow = now;
	/* Send (or resend) any packets that need it, subject to
	 * window restrictions and congestion burst control
	 * restrictions.  Ask for an ack on the last packet sent in
	 * this burst.  For now, we're relying upon the window being
	 * considerably bigger than the largest number of packets that
	 * are typically sent at once by one initial call to
	 * rxi_Start.  This is probably bogus (perhaps we should ask
	 * for an ack when we're half way through the current
	 * window?).  Also, for non file transfer applications, this
	 * may end up asking for an ack for every packet.  Bogus. XXXX
	 */
	/*
	 * But check whether we're here recursively, and let the other guy
	 * do the work.
	 */
#ifdef	AFS_GLOBAL_RXLOCK_KERNEL
	if (!(call->flags & RX_CALL_TQ_BUSY)) {
	    call->flags |= RX_CALL_TQ_BUSY;
	    do {
#endif /* AFS_GLOBAL_RXLOCK_KERNEL */
	    restart:
#ifdef	AFS_GLOBAL_RXLOCK_KERNEL
		call->flags &= ~RX_CALL_NEED_START;
#endif /* AFS_GLOBAL_RXLOCK_KERNEL */
		nXmitPackets = 0;
		maxXmitPackets = MIN(call->twind, call->cwind);
		xmitList = (struct rx_packet **)
#if defined(KERNEL) && !defined(UKERNEL) && defined(AFS_FBSD80_ENV)
		    /* XXXX else we must drop any mtx we hold */
		    afs_osi_Alloc_NoSleep(maxXmitPackets * sizeof(struct rx_packet *));
#else
		osi_Alloc(maxXmitPackets * sizeof(struct rx_packet *));
#endif
		if (xmitList == NULL)
		    osi_Panic("rxi_Start, failed to allocate xmit list");
		for (queue_Scan(&call->tq, p, nxp, rx_packet)) {
		    if (call->flags & RX_CALL_FAST_RECOVER_WAIT) {
			/* We shouldn't be sending packets if a thread is waiting
			 * to initiate congestion recovery */
			dpf(("call %d waiting to initiate fast recovery\n",
			     *(call->callNumber)));
			break;
		    }
		    if ((nXmitPackets)
			&& (call->flags & RX_CALL_FAST_RECOVER)) {
			/* Only send one packet during fast recovery */
			dpf(("call %d restricted to one packet per send during fast recovery\n",
			     *(call->callNumber)));
			break;
		    }
#ifdef RX_TRACK_PACKETS
		    if ((p->flags & RX_PKTFLAG_FREE)
			|| (!queue_IsEnd(&call->tq, nxp)
			    && (nxp->flags & RX_PKTFLAG_FREE))
			|| (p == (struct rx_packet *)&rx_freePacketQueue)
			|| (nxp == (struct rx_packet *)&rx_freePacketQueue)) {
			osi_Panic("rxi_Start: xmit queue clobbered");
		    }
#endif
		    if (p->flags & RX_PKTFLAG_ACKED) {
			/* Since we may block, don't trust this */
			usenow.sec = usenow.usec = 0;
                        if (rx_stats_active)
                            rx_atomic_inc(&rx_stats.ignoreAckedPacket);
			continue;	/* Ignore this packet if it has been acknowledged */
		    }

		    /* Turn off all flags except these ones, which are the same
		     * on each transmission */
		    p->header.flags &= RX_PRESET_FLAGS;

		    if (p->header.seq >=
			call->tfirst + MIN((int)call->twind,
					   (int)(call->nSoftAcked +
						 call->cwind))) {
			call->flags |= RX_CALL_WAIT_WINDOW_SEND;	/* Wait for transmit window */
			/* Note: if we're waiting for more window space, we can
			 * still send retransmits; hence we don't return here, but
			 * break out to schedule a retransmit event */
			dpf(("call %d waiting for window (seq %d, twind %d, nSoftAcked %d, cwind %d)\n",
			     *(call->callNumber), p->header.seq, call->twind, call->nSoftAcked,
                             call->cwind));
			break;
		    }

		    /* Transmit the packet if it needs to be sent. */
		    if (!clock_Lt(&now, &p->retryTime)) {
			if (nXmitPackets == maxXmitPackets) {
			    rxi_SendXmitList(call, xmitList, nXmitPackets,
					     istack, &now, &retryTime,
					     resending);
			    osi_Free(xmitList, maxXmitPackets *
				     sizeof(struct rx_packet *));
			    goto restart;
			}
                        dpf(("call %d xmit packet %"AFS_PTR_FMT" now %u.%06u retryTime %u.%06u nextRetry %u.%06u\n",
                              *(call->callNumber), p,
                              now.sec, now.usec,
                              p->retryTime.sec, p->retryTime.usec,
                              retryTime.sec, retryTime.usec));
			xmitList[nXmitPackets++] = p;
		    }
		}

		/* xmitList now hold pointers to all of the packets that are
		 * ready to send. Now we loop to send the packets */
		if (nXmitPackets > 0) {
		    rxi_SendXmitList(call, xmitList, nXmitPackets, istack,
				     &now, &retryTime, resending);
		}
		osi_Free(xmitList,
			 maxXmitPackets * sizeof(struct rx_packet *));

#ifdef	AFS_GLOBAL_RXLOCK_KERNEL
		/*
		 * TQ references no longer protected by this flag; they must remain
		 * protected by the global lock.
		 */
		if (call->flags & RX_CALL_FAST_RECOVER_WAIT) {
		    call->flags &= ~RX_CALL_TQ_BUSY;
		    if (call->tqWaiters || (call->flags & RX_CALL_TQ_WAIT)) {
			dpf(("call %"AFS_PTR_FMT" has %d waiters and flags %d\n",
                             call, call->tqWaiters, call->flags));
#ifdef RX_ENABLE_LOCKS
			osirx_AssertMine(&call->lock, "rxi_Start start");
			CV_BROADCAST(&call->cv_tq);
#else /* RX_ENABLE_LOCKS */
			osi_rxWakeup(&call->tq);
#endif /* RX_ENABLE_LOCKS */
		    }
		    return;
		}
		if (call->error) {
		    /* We went into the error state while sending packets. Now is
		     * the time to reset the call. This will also inform the using
		     * process that the call is in an error state.
		     */
                    if (rx_stats_active)
                        rx_atomic_inc(&rx_tq_debug.rxi_start_aborted);
		    call->flags &= ~RX_CALL_TQ_BUSY;
		    if (call->tqWaiters || (call->flags & RX_CALL_TQ_WAIT)) {
			dpf(("call error %d while xmit %p has %d waiters and flags %d\n",
                             call->error, call, call->tqWaiters, call->flags));
#ifdef RX_ENABLE_LOCKS
			osirx_AssertMine(&call->lock, "rxi_Start middle");
			CV_BROADCAST(&call->cv_tq);
#else /* RX_ENABLE_LOCKS */
			osi_rxWakeup(&call->tq);
#endif /* RX_ENABLE_LOCKS */
		    }
		    rxi_CallError(call, call->error);
		    return;
		}
#ifdef RX_ENABLE_LOCKS
		if (call->flags & RX_CALL_TQ_SOME_ACKED) {
		    int missing;
		    call->flags &= ~RX_CALL_TQ_SOME_ACKED;
		    /* Some packets have received acks. If they all have, we can clear
		     * the transmit queue.
		     */
		    for (missing =
			 0, queue_Scan(&call->tq, p, nxp, rx_packet)) {
			if (p->header.seq < call->tfirst
			    && (p->flags & RX_PKTFLAG_ACKED)) {
			    queue_Remove(p);
#ifdef RX_TRACK_PACKETS
			    p->flags &= ~RX_PKTFLAG_TQ;
#endif
#ifdef RXDEBUG_PACKET
                            call->tqc--;
#endif
			    rxi_FreePacket(p);
			} else
			    missing = 1;
		    }
		    if (!missing)
			call->flags |= RX_CALL_TQ_CLEARME;
		}
#endif /* RX_ENABLE_LOCKS */
		/* Don't bother doing retransmits if the TQ is cleared. */
		if (call->flags & RX_CALL_TQ_CLEARME) {
		    rxi_ClearTransmitQueue(call, 1);
		} else
#endif /* AFS_GLOBAL_RXLOCK_KERNEL */
		{

		    /* Always post a resend event, if there is anything in the
		     * queue, and resend is possible.  There should be at least
		     * one unacknowledged packet in the queue ... otherwise none
		     * of these packets should be on the queue in the first place.
		     */
		    if (call->resendEvent) {
			/* Cancel the existing event and post a new one */
			rxevent_Cancel(call->resendEvent, call,
				       RX_CALL_REFCOUNT_RESEND);
		    }

		    /* The retry time is the retry time on the first unacknowledged
		     * packet inside the current window */
		    for (haveEvent =
			 0, queue_Scan(&call->tq, p, nxp, rx_packet)) {
			/* Don't set timers for packets outside the window */
			if (p->header.seq >= call->tfirst + call->twind) {
			    break;
			}

			if (!(p->flags & RX_PKTFLAG_ACKED)
			    && !clock_IsZero(&p->retryTime)) {
			    haveEvent = 1;
			    retryTime = p->retryTime;
			    break;
			}
		    }

		    /* Post a new event to re-run rxi_Start when retries may be needed */
		    if (haveEvent && !(call->flags & RX_CALL_NEED_START)) {
#ifdef RX_ENABLE_LOCKS
                        MUTEX_ENTER(&rx_refcnt_mutex);
			CALL_HOLD(call, RX_CALL_REFCOUNT_RESEND);
                        MUTEX_EXIT(&rx_refcnt_mutex);
			call->resendEvent =
			    rxevent_PostNow2(&retryTime, &usenow,
					     rxi_StartUnlocked,
					     (void *)call, 0, istack);
#else /* RX_ENABLE_LOCKS */
			call->resendEvent =
			    rxevent_PostNow2(&retryTime, &usenow, rxi_Start,
					     (void *)call, 0, istack);
#endif /* RX_ENABLE_LOCKS */
		    }
		}
#ifdef	AFS_GLOBAL_RXLOCK_KERNEL
	    } while (call->flags & RX_CALL_NEED_START);
	    /*
	     * TQ references no longer protected by this flag; they must remain
	     * protected by the global lock.
	     */
	    call->flags &= ~RX_CALL_TQ_BUSY;
	    if (call->tqWaiters || (call->flags & RX_CALL_TQ_WAIT)) {
		dpf(("call %"AFS_PTR_FMT" has %d waiters and flags %d\n",
                      call, call->tqWaiters, call->flags));
#ifdef RX_ENABLE_LOCKS
		osirx_AssertMine(&call->lock, "rxi_Start end");
		CV_BROADCAST(&call->cv_tq);
#else /* RX_ENABLE_LOCKS */
		osi_rxWakeup(&call->tq);
#endif /* RX_ENABLE_LOCKS */
	    }
	} else {
	    call->flags |= RX_CALL_NEED_START;
	}
#endif /* AFS_GLOBAL_RXLOCK_KERNEL */
    } else {
	if (call->resendEvent) {
	    rxevent_Cancel(call->resendEvent, call, RX_CALL_REFCOUNT_RESEND);
	}
    }
}

/* Also adjusts the keep alive parameters for the call, to reflect
 * that we have just sent a packet (so keep alives aren't sent
 * immediately) */
void
rxi_Send(struct rx_call *call, struct rx_packet *p,
	 int istack)
{
    struct rx_connection *conn = call->conn;

    /* Stamp each packet with the user supplied status */
    p->header.userStatus = call->localStatus;

    /* Allow the security object controlling this call's security to
     * make any last-minute changes to the packet */
    RXS_SendPacket(conn->securityObject, call, p);

    /* Since we're about to send SOME sort of packet to the peer, it's
     * safe to nuke any scheduled end-of-packets ack */
    rxevent_Cancel(call->delayedAckEvent, call, RX_CALL_REFCOUNT_DELAY);

    /* Actually send the packet, filling in more connection-specific fields */
    MUTEX_EXIT(&call->lock);
    MUTEX_ENTER(&rx_refcnt_mutex);
    CALL_HOLD(call, RX_CALL_REFCOUNT_SEND);
    MUTEX_EXIT(&rx_refcnt_mutex);
    rxi_SendPacket(call, conn, p, istack);
    MUTEX_ENTER(&rx_refcnt_mutex);
    CALL_RELE(call, RX_CALL_REFCOUNT_SEND);
    MUTEX_EXIT(&rx_refcnt_mutex);
    MUTEX_ENTER(&call->lock);

    /* Update last send time for this call (for keep-alive
     * processing), and for the connection (so that we can discover
     * idle connections) */
    if ((p->header.type != RX_PACKET_TYPE_ACK) ||
	(((struct rx_ackPacket *)rx_DataOf(p))->reason == RX_ACK_PING) ||
	(p->length <= (rx_AckDataSize(call->rwind) + 4 * sizeof(afs_int32))))
    {
	conn->lastSendTime = call->lastSendTime = clock_Sec();
	/* Don't count keepalive ping/acks here, so idleness can be tracked. */
	if ((p->header.type != RX_PACKET_TYPE_ACK) ||
	    ((((struct rx_ackPacket *)rx_DataOf(p))->reason != RX_ACK_PING) &&
	     (((struct rx_ackPacket *)rx_DataOf(p))->reason !=
	      RX_ACK_PING_RESPONSE)))
	    call->lastSendData = call->lastSendTime;
    }
}

/* Check if a call needs to be destroyed.  Called by keep-alive code to ensure
 * that things are fine.  Also called periodically to guarantee that nothing
 * falls through the cracks (e.g. (error + dally) connections have keepalive
 * turned off.  Returns 0 if conn is well, -1 otherwise.  If otherwise, call
 *  may be freed!
 * haveCTLock Set if calling from rxi_ReapConnections
 */
#ifdef RX_ENABLE_LOCKS
int
rxi_CheckCall(struct rx_call *call, int haveCTLock)
#else /* RX_ENABLE_LOCKS */
int
rxi_CheckCall(struct rx_call *call)
#endif				/* RX_ENABLE_LOCKS */
{
    struct rx_connection *conn = call->conn;
    afs_uint32 now;
    afs_uint32 deadTime;
    int cerror = 0;
    int newmtu = 0;

#ifdef AFS_GLOBAL_RXLOCK_KERNEL
    if (call->flags & RX_CALL_TQ_BUSY) {
	/* Call is active and will be reset by rxi_Start if it's
	 * in an error state.
	 */
	return 0;
    }
#endif
    /* dead time + RTT + 8*MDEV, rounded up to next second. */
    deadTime =
	(((afs_uint32) conn->secondsUntilDead << 10) +
	 ((afs_uint32) conn->peer->rtt >> 3) +
	 ((afs_uint32) conn->peer->rtt_dev << 1) + 1023) >> 10;
    now = clock_Sec();
    /* These are computed to the second (+- 1 second).  But that's
     * good enough for these values, which should be a significant
     * number of seconds. */
    if (now > (call->lastReceiveTime + deadTime)) {
	if (call->state == RX_STATE_ACTIVE) {
#ifdef ADAPT_PMTU
#if defined(KERNEL) && defined(AFS_SUN57_ENV)
	    ire_t *ire;
#if defined(AFS_SUN510_ENV) && defined(GLOBAL_NETSTACKID)
	    netstack_t *ns =  netstack_find_by_stackid(GLOBAL_NETSTACKID);
	    ip_stack_t *ipst = ns->netstack_ip;
#endif
	    ire = ire_cache_lookup(conn->peer->host
#if defined(AFS_SUN510_ENV) && defined(ALL_ZONES)
				   , ALL_ZONES
#if defined(AFS_SUN510_ENV) && (defined(ICL_3_ARG) || defined(GLOBAL_NETSTACKID))
				   , NULL
#if defined(AFS_SUN510_ENV) && defined(GLOBAL_NETSTACKID)
				   , ipst
#endif
#endif
#endif
		);

	    if (ire && ire->ire_max_frag > 0)
		rxi_SetPeerMtu(NULL, conn->peer->host, 0,
			       ire->ire_max_frag);
#if defined(GLOBAL_NETSTACKID)
	    netstack_rele(ns);
#endif
#endif
#endif /* ADAPT_PMTU */
	    cerror = RX_CALL_DEAD;
	    goto mtuout;
	} else {
#ifdef RX_ENABLE_LOCKS
	    /* Cancel pending events */
	    rxevent_Cancel(call->delayedAckEvent, call,
			   RX_CALL_REFCOUNT_DELAY);
	    rxevent_Cancel(call->resendEvent, call, RX_CALL_REFCOUNT_RESEND);
	    rxevent_Cancel(call->keepAliveEvent, call,
			   RX_CALL_REFCOUNT_ALIVE);
            MUTEX_ENTER(&rx_refcnt_mutex);
	    if (call->refCount == 0) {
		rxi_FreeCall(call, haveCTLock);
                MUTEX_EXIT(&rx_refcnt_mutex);
		return -2;
	    }
            MUTEX_EXIT(&rx_refcnt_mutex);
	    return -1;
#else /* RX_ENABLE_LOCKS */
	    rxi_FreeCall(call, 0);
	    return -2;
#endif /* RX_ENABLE_LOCKS */
	}
	/* Non-active calls are destroyed if they are not responding
	 * to pings; active calls are simply flagged in error, so the
	 * attached process can die reasonably gracefully. */
    }
    /* see if we have a non-activity timeout */
    if (call->startWait && conn->idleDeadTime
	&& ((call->startWait + conn->idleDeadTime) < now) &&
	(call->flags & RX_CALL_READER_WAIT)) {
	if (call->state == RX_STATE_ACTIVE) {
	    cerror = RX_CALL_TIMEOUT;
	    goto mtuout;
	}
    }
    if (call->lastSendData && conn->idleDeadTime && (conn->idleDeadErr != 0)
        && ((call->lastSendData + conn->idleDeadTime) < now)) {
	if (call->state == RX_STATE_ACTIVE) {
	    cerror = conn->idleDeadErr;
	    goto mtuout;
	}
    }
    /* see if we have a hard timeout */
    if (conn->hardDeadTime
	&& (now > (conn->hardDeadTime + call->startTime.sec))) {
	if (call->state == RX_STATE_ACTIVE)
	    rxi_CallError(call, RX_CALL_TIMEOUT);
	return -1;
    }
    return 0;
mtuout:
    if (conn->msgsizeRetryErr && cerror != RX_CALL_TIMEOUT
	&& call->lastReceiveTime) {
	int oldMTU = conn->peer->ifMTU;

	/* if we thought we could send more, perhaps things got worse */
	if (conn->peer->maxPacketSize > conn->lastPacketSize)
	    /* maxpacketsize will be cleared in rxi_SetPeerMtu */
	    newmtu = MAX(conn->peer->maxPacketSize-RX_IPUDP_SIZE,
			 conn->lastPacketSize-(128+RX_IPUDP_SIZE));
	else
	    newmtu = conn->lastPacketSize-(128+RX_IPUDP_SIZE);

	/* minimum capped in SetPeerMtu */
	rxi_SetPeerMtu(conn->peer, 0, 0, newmtu);

	/* clean up */
	conn->lastPacketSize = 0;

	/* needed so ResetCall doesn't clobber us. */
	call->MTU = conn->peer->ifMTU;

	/* if we never succeeded, let the error pass out as-is */
	if (conn->peer->maxPacketSize && oldMTU != conn->peer->ifMTU)
	    cerror = conn->msgsizeRetryErr;

    }
    rxi_CallError(call, cerror);
    return -1;
}

void
rxi_NatKeepAliveEvent(struct rxevent *event, void *arg1, void *dummy)
{
    struct rx_connection *conn = arg1;
    struct rx_header theader;
    char tbuffer[1500];
    struct sockaddr_in taddr;
    char *tp;
    char a[1] = { 0 };
    struct iovec tmpiov[2];
    osi_socket socket =
        (conn->type ==
         RX_CLIENT_CONNECTION ? rx_socket : conn->service->socket);


    tp = &tbuffer[sizeof(struct rx_header)];
    taddr.sin_family = AF_INET;
    taddr.sin_port = rx_PortOf(rx_PeerOf(conn));
    taddr.sin_addr.s_addr = rx_HostOf(rx_PeerOf(conn));
#ifdef STRUCT_SOCKADDR_HAS_SA_LEN
    taddr.sin_len = sizeof(struct sockaddr_in);
#endif
    memset(&theader, 0, sizeof(theader));
    theader.epoch = htonl(999);
    theader.cid = 0;
    theader.callNumber = 0;
    theader.seq = 0;
    theader.serial = 0;
    theader.type = RX_PACKET_TYPE_VERSION;
    theader.flags = RX_LAST_PACKET;
    theader.serviceId = 0;

    memcpy(tbuffer, &theader, sizeof(theader));
    memcpy(tp, &a, sizeof(a));
    tmpiov[0].iov_base = tbuffer;
    tmpiov[0].iov_len = 1 + sizeof(struct rx_header);

    osi_NetSend(socket, &taddr, tmpiov, 1, 1 + sizeof(struct rx_header), 1);

    MUTEX_ENTER(&conn->conn_data_lock);
    MUTEX_ENTER(&rx_refcnt_mutex);
    /* Only reschedule ourselves if the connection would not be destroyed */
    if (conn->refCount <= 1) {
	conn->natKeepAliveEvent = NULL;
        MUTEX_EXIT(&rx_refcnt_mutex);
	MUTEX_EXIT(&conn->conn_data_lock);
	rx_DestroyConnection(conn); /* drop the reference for this */
    } else {
	conn->refCount--; /* drop the reference for this */
        MUTEX_EXIT(&rx_refcnt_mutex);
	conn->natKeepAliveEvent = NULL;
	rxi_ScheduleNatKeepAliveEvent(conn);
	MUTEX_EXIT(&conn->conn_data_lock);
    }
}

void
rxi_ScheduleNatKeepAliveEvent(struct rx_connection *conn)
{
    if (!conn->natKeepAliveEvent && conn->secondsUntilNatPing) {
	struct clock when, now;
	clock_GetTime(&now);
	when = now;
	when.sec += conn->secondsUntilNatPing;
        MUTEX_ENTER(&rx_refcnt_mutex);
	conn->refCount++; /* hold a reference for this */
        MUTEX_EXIT(&rx_refcnt_mutex);
	conn->natKeepAliveEvent =
	    rxevent_PostNow(&when, &now, rxi_NatKeepAliveEvent, conn, 0);
    }
}

void
rx_SetConnSecondsUntilNatPing(struct rx_connection *conn, afs_int32 seconds)
{
    MUTEX_ENTER(&conn->conn_data_lock);
    conn->secondsUntilNatPing = seconds;
    if (seconds != 0)
	rxi_ScheduleNatKeepAliveEvent(conn);
    MUTEX_EXIT(&conn->conn_data_lock);
}

void
rxi_NatKeepAliveOn(struct rx_connection *conn)
{
    MUTEX_ENTER(&conn->conn_data_lock);
    rxi_ScheduleNatKeepAliveEvent(conn);
    MUTEX_EXIT(&conn->conn_data_lock);
}

/* When a call is in progress, this routine is called occasionally to
 * make sure that some traffic has arrived (or been sent to) the peer.
 * If nothing has arrived in a reasonable amount of time, the call is
 * declared dead; if nothing has been sent for a while, we send a
 * keep-alive packet (if we're actually trying to keep the call alive)
 */
void
rxi_KeepAliveEvent(struct rxevent *event, void *arg1, void *dummy)
{
    struct rx_call *call = arg1;
    struct rx_connection *conn;
    afs_uint32 now;

    MUTEX_ENTER(&rx_refcnt_mutex);
    CALL_RELE(call, RX_CALL_REFCOUNT_ALIVE);
    MUTEX_EXIT(&rx_refcnt_mutex);
    MUTEX_ENTER(&call->lock);
    if (event == call->keepAliveEvent)
	call->keepAliveEvent = NULL;
    now = clock_Sec();

#ifdef RX_ENABLE_LOCKS
    if (rxi_CheckCall(call, 0)) {
	MUTEX_EXIT(&call->lock);
	return;
    }
#else /* RX_ENABLE_LOCKS */
    if (rxi_CheckCall(call))
	return;
#endif /* RX_ENABLE_LOCKS */

    /* Don't try to keep alive dallying calls */
    if (call->state == RX_STATE_DALLY) {
	MUTEX_EXIT(&call->lock);
	return;
    }

    conn = call->conn;
    if ((now - call->lastSendTime) > conn->secondsUntilPing) {
	/* Don't try to send keepalives if there is unacknowledged data */
	/* the rexmit code should be good enough, this little hack
	 * doesn't quite work XXX */
	(void)rxi_SendAck(call, NULL, 0, RX_ACK_PING, 0);
    }
    rxi_ScheduleKeepAliveEvent(call);
    MUTEX_EXIT(&call->lock);
}

/* Does what's on the nameplate. */
void
rxi_GrowMTUEvent(struct rxevent *event, void *arg1, void *dummy)
{
    struct rx_call *call = arg1;
    struct rx_connection *conn;

    MUTEX_ENTER(&rx_refcnt_mutex);
    CALL_RELE(call, RX_CALL_REFCOUNT_ALIVE);
    MUTEX_EXIT(&rx_refcnt_mutex);
    MUTEX_ENTER(&call->lock);

    if (event == call->growMTUEvent)
	call->growMTUEvent = NULL;

#ifdef RX_ENABLE_LOCKS
    if (rxi_CheckCall(call, 0)) {
	MUTEX_EXIT(&call->lock);
	return;
    }
#else /* RX_ENABLE_LOCKS */
    if (rxi_CheckCall(call))
	return;
#endif /* RX_ENABLE_LOCKS */

    /* Don't bother with dallying calls */
    if (call->state == RX_STATE_DALLY) {
	MUTEX_EXIT(&call->lock);
	return;
    }

    conn = call->conn;

    /*
     * keep being scheduled, just don't do anything if we're at peak,
     * or we're not set up to be properly handled (idle timeout required)
     */
    if ((conn->peer->maxPacketSize != 0) &&
	(conn->peer->natMTU < RX_MAX_PACKET_SIZE) &&
	(conn->idleDeadErr))
	(void)rxi_SendAck(call, NULL, 0, RX_ACK_MTU, 0);
    rxi_ScheduleGrowMTUEvent(call, 0);
    MUTEX_EXIT(&call->lock);
}

void
rxi_ScheduleKeepAliveEvent(struct rx_call *call)
{
    if (!call->keepAliveEvent) {
	struct clock when, now;
	clock_GetTime(&now);
	when = now;
	when.sec += call->conn->secondsUntilPing;
        MUTEX_ENTER(&rx_refcnt_mutex);
	CALL_HOLD(call, RX_CALL_REFCOUNT_ALIVE);
        MUTEX_EXIT(&rx_refcnt_mutex);
	call->keepAliveEvent =
	    rxevent_PostNow(&when, &now, rxi_KeepAliveEvent, call, 0);
    }
}

void
rxi_ScheduleGrowMTUEvent(struct rx_call *call, int secs)
{
    if (!call->growMTUEvent) {
	struct clock when, now;

	clock_GetTime(&now);
	when = now;
	if (!secs) {
	    if (call->conn->secondsUntilPing)
		secs = (6*call->conn->secondsUntilPing)-1;

	    if (call->conn->secondsUntilDead)
		secs = MIN(secs, (call->conn->secondsUntilDead-1));
	}

	when.sec += secs;
        MUTEX_ENTER(&rx_refcnt_mutex);
	CALL_HOLD(call, RX_CALL_REFCOUNT_ALIVE);
        MUTEX_EXIT(&rx_refcnt_mutex);
	call->growMTUEvent =
	    rxevent_PostNow(&when, &now, rxi_GrowMTUEvent, call, 0);
    }
}

/* N.B. rxi_KeepAliveOff:  is defined earlier as a macro */
void
rxi_KeepAliveOn(struct rx_call *call)
{
    /* Pretend last packet received was received now--i.e. if another
     * packet isn't received within the keep alive time, then the call
     * will die; Initialize last send time to the current time--even
     * if a packet hasn't been sent yet.  This will guarantee that a
     * keep-alive is sent within the ping time */
    call->lastReceiveTime = call->lastSendTime = clock_Sec();
    rxi_ScheduleKeepAliveEvent(call);
}

void
rxi_GrowMTUOn(struct rx_call *call)
{
    struct rx_connection *conn = call->conn;
    MUTEX_ENTER(&conn->conn_data_lock);
    conn->lastPingSizeSer = conn->lastPingSize = 0;
    MUTEX_EXIT(&conn->conn_data_lock);
    rxi_ScheduleGrowMTUEvent(call, 1);
}

/* This routine is called to send connection abort messages
 * that have been delayed to throttle looping clients. */
void
rxi_SendDelayedConnAbort(struct rxevent *event,
			 void *arg1, void *unused)
{
    struct rx_connection *conn = arg1;

    afs_int32 error;
    struct rx_packet *packet;

    MUTEX_ENTER(&conn->conn_data_lock);
    conn->delayedAbortEvent = NULL;
    error = htonl(conn->error);
    conn->abortCount++;
    MUTEX_EXIT(&conn->conn_data_lock);
    packet = rxi_AllocPacket(RX_PACKET_CLASS_SPECIAL);
    if (packet) {
	packet =
	    rxi_SendSpecial((struct rx_call *)0, conn, packet,
			    RX_PACKET_TYPE_ABORT, (char *)&error,
			    sizeof(error), 0);
	rxi_FreePacket(packet);
    }
}

/* This routine is called to send call abort messages
 * that have been delayed to throttle looping clients. */
void
rxi_SendDelayedCallAbort(struct rxevent *event,
			 void *arg1, void *dummy)
{
    struct rx_call *call = arg1;

    afs_int32 error;
    struct rx_packet *packet;

    MUTEX_ENTER(&call->lock);
    call->delayedAbortEvent = NULL;
    error = htonl(call->error);
    call->abortCount++;
    packet = rxi_AllocPacket(RX_PACKET_CLASS_SPECIAL);
    if (packet) {
	packet =
	    rxi_SendSpecial(call, call->conn, packet, RX_PACKET_TYPE_ABORT,
			    (char *)&error, sizeof(error), 0);
	rxi_FreePacket(packet);
    }
    MUTEX_EXIT(&call->lock);
    MUTEX_ENTER(&rx_refcnt_mutex);
    CALL_RELE(call, RX_CALL_REFCOUNT_ABORT);
    MUTEX_EXIT(&rx_refcnt_mutex);
}

/* This routine is called periodically (every RX_AUTH_REQUEST_TIMEOUT
 * seconds) to ask the client to authenticate itself.  The routine
 * issues a challenge to the client, which is obtained from the
 * security object associated with the connection */
void
rxi_ChallengeEvent(struct rxevent *event,
		   void *arg0, void *arg1, int tries)
{
    struct rx_connection *conn = arg0;

    conn->challengeEvent = NULL;
    if (RXS_CheckAuthentication(conn->securityObject, conn) != 0) {
	struct rx_packet *packet;
	struct clock when, now;

	if (tries <= 0) {
	    /* We've failed to authenticate for too long.
	     * Reset any calls waiting for authentication;
	     * they are all in RX_STATE_PRECALL.
	     */
	    int i;

	    MUTEX_ENTER(&conn->conn_call_lock);
	    for (i = 0; i < RX_MAXCALLS; i++) {
		struct rx_call *call = conn->call[i];
		if (call) {
		    MUTEX_ENTER(&call->lock);
		    if (call->state == RX_STATE_PRECALL) {
			rxi_CallError(call, RX_CALL_DEAD);
			rxi_SendCallAbort(call, NULL, 0, 0);
		    }
		    MUTEX_EXIT(&call->lock);
		}
	    }
	    MUTEX_EXIT(&conn->conn_call_lock);
	    return;
	}

	packet = rxi_AllocPacket(RX_PACKET_CLASS_SPECIAL);
	if (packet) {
	    /* If there's no packet available, do this later. */
	    RXS_GetChallenge(conn->securityObject, conn, packet);
	    rxi_SendSpecial((struct rx_call *)0, conn, packet,
			    RX_PACKET_TYPE_CHALLENGE, NULL, -1, 0);
	    rxi_FreePacket(packet);
	}
	clock_GetTime(&now);
	when = now;
	when.sec += RX_CHALLENGE_TIMEOUT;
	conn->challengeEvent =
	    rxevent_PostNow2(&when, &now, rxi_ChallengeEvent, conn, 0,
			 (tries - 1));
    }
}

/* Call this routine to start requesting the client to authenticate
 * itself.  This will continue until authentication is established,
 * the call times out, or an invalid response is returned.  The
 * security object associated with the connection is asked to create
 * the challenge at this time.  N.B.  rxi_ChallengeOff is a macro,
 * defined earlier. */
void
rxi_ChallengeOn(struct rx_connection *conn)
{
    if (!conn->challengeEvent) {
	RXS_CreateChallenge(conn->securityObject, conn);
	rxi_ChallengeEvent(NULL, conn, 0, RX_CHALLENGE_MAXTRIES);
    };
}


/* Compute round trip time of the packet provided, in *rttp.
 */

/* rxi_ComputeRoundTripTime is called with peer locked. */
/* sentp and/or peer may be null */
void
rxi_ComputeRoundTripTime(struct rx_packet *p,
			 struct clock *sentp,
			 struct rx_peer *peer)
{
    struct clock thisRtt, *rttp = &thisRtt;

    int rtt_timeout;

    clock_GetTime(rttp);

    if (clock_Lt(rttp, sentp)) {
	clock_Zero(rttp);
	return;			/* somebody set the clock back, don't count this time. */
    }
    clock_Sub(rttp, sentp);
    dpf(("rxi_ComputeRoundTripTime(call=%d packet=%"AFS_PTR_FMT" rttp=%d.%06d sec)\n",
          p->header.callNumber, p, rttp->sec, rttp->usec));

    if (rttp->sec == 0 && rttp->usec == 0) {
        /*
         * The actual round trip time is shorter than the
         * clock_GetTime resolution.  It is most likely 1ms or 100ns.
         * Since we can't tell which at the moment we will assume 1ms.
         */
        rttp->usec = 1000;
    }

    if (rx_stats_active) {
        MUTEX_ENTER(&rx_stats_mutex);
        if (clock_Lt(rttp, &rx_stats.minRtt))
            rx_stats.minRtt = *rttp;
        if (clock_Gt(rttp, &rx_stats.maxRtt)) {
            if (rttp->sec > 60) {
                MUTEX_EXIT(&rx_stats_mutex);
                return;		/* somebody set the clock ahead */
            }
            rx_stats.maxRtt = *rttp;
        }
        clock_Add(&rx_stats.totalRtt, rttp);
        rx_atomic_inc(&rx_stats.nRttSamples);
        MUTEX_EXIT(&rx_stats_mutex);
    }

    /* better rtt calculation courtesy of UMich crew (dave,larry,peter,?) */

    /* Apply VanJacobson round-trip estimations */
    if (peer->rtt) {
	int delta;

	/*
	 * srtt (peer->rtt) is in units of one-eighth-milliseconds.
	 * srtt is stored as fixed point with 3 bits after the binary
	 * point (i.e., scaled by 8). The following magic is
	 * equivalent to the smoothing algorithm in rfc793 with an
	 * alpha of .875 (srtt' = rtt/8 + srtt*7/8 in fixed point).
         * srtt'*8 = rtt + srtt*7
	 * srtt'*8 = srtt*8 + rtt - srtt
	 * srtt' = srtt + rtt/8 - srtt/8
         * srtt' = srtt + (rtt - srtt)/8
	 */

	delta = _8THMSEC(rttp) - peer->rtt;
	peer->rtt += (delta >> 3);

	/*
	 * We accumulate a smoothed rtt variance (actually, a smoothed
	 * mean difference), then set the retransmit timer to smoothed
	 * rtt + 4 times the smoothed variance (was 2x in van's original
	 * paper, but 4x works better for me, and apparently for him as
	 * well).
	 * rttvar is stored as
	 * fixed point with 2 bits after the binary point (scaled by
	 * 4).  The following is equivalent to rfc793 smoothing with
	 * an alpha of .75 (rttvar' = rttvar*3/4 + |delta| / 4).
         *   rttvar'*4 = rttvar*3 + |delta|
         *   rttvar'*4 = rttvar*4 + |delta| - rttvar
         *   rttvar' = rttvar + |delta|/4 - rttvar/4
         *   rttvar' = rttvar + (|delta| - rttvar)/4
	 * This replaces rfc793's wired-in beta.
	 * dev*4 = dev*4 + (|actual - expected| - dev)
	 */

	if (delta < 0)
	    delta = -delta;

	delta -= (peer->rtt_dev << 1);
	peer->rtt_dev += (delta >> 3);
    } else {
	/* I don't have a stored RTT so I start with this value.  Since I'm
	 * probably just starting a call, and will be pushing more data down
	 * this, I expect congestion to increase rapidly.  So I fudge a
	 * little, and I set deviance to half the rtt.  In practice,
	 * deviance tends to approach something a little less than
	 * half the smoothed rtt. */
	peer->rtt = _8THMSEC(rttp) + 8;
	peer->rtt_dev = peer->rtt >> 2;	/* rtt/2: they're scaled differently */
    }
    /* the timeout is RTT + 4*MDEV but no less than rx_minPeerTimeout msec.
     * This is because one end or the other of these connections is usually
     * in a user process, and can be switched and/or swapped out.  So on fast,
     * reliable networks, the timeout would otherwise be too short. */
    rtt_timeout = MAX(((peer->rtt >> 3) + peer->rtt_dev), rx_minPeerTimeout);
    clock_Zero(&(peer->timeout));
    clock_Addmsec(&(peer->timeout), rtt_timeout);

    /* Reset the backedOff flag since we just computed a new timeout value */
    peer->backedOff = 0;

    dpf(("rxi_ComputeRoundTripTime(call=%d packet=%"AFS_PTR_FMT" rtt=%d ms, srtt=%d ms, rtt_dev=%d ms, timeout=%d.%06d sec)\n",
          p->header.callNumber, p, MSEC(rttp), peer->rtt >> 3, peer->rtt_dev >> 2, (peer->timeout.sec), (peer->timeout.usec)));
}


/* Find all server connections that have not been active for a long time, and
 * toss them */
void
rxi_ReapConnections(struct rxevent *unused, void *unused1, void *unused2)
{
    struct clock now, when;
    clock_GetTime(&now);

    /* Find server connection structures that haven't been used for
     * greater than rx_idleConnectionTime */
    {
	struct rx_connection **conn_ptr, **conn_end;
	int i, havecalls = 0;
	MUTEX_ENTER(&rx_connHashTable_lock);
	for (conn_ptr = &rx_connHashTable[0], conn_end =
	     &rx_connHashTable[rx_hashTableSize]; conn_ptr < conn_end;
	     conn_ptr++) {
	    struct rx_connection *conn, *next;
	    struct rx_call *call;
	    int result;

	  rereap:
	    for (conn = *conn_ptr; conn; conn = next) {
		/* XXX -- Shouldn't the connection be locked? */
		next = conn->next;
		havecalls = 0;
		for (i = 0; i < RX_MAXCALLS; i++) {
		    call = conn->call[i];
		    if (call) {
			int code;
			havecalls = 1;
			code = MUTEX_TRYENTER(&call->lock);
			if (!code)
			    continue;
#ifdef RX_ENABLE_LOCKS
			result = rxi_CheckCall(call, 1);
#else /* RX_ENABLE_LOCKS */
			result = rxi_CheckCall(call);
#endif /* RX_ENABLE_LOCKS */
			MUTEX_EXIT(&call->lock);
			if (result == -2) {
			    /* If CheckCall freed the call, it might
			     * have destroyed  the connection as well,
			     * which screws up the linked lists.
			     */
			    goto rereap;
			}
		    }
		}
		if (conn->type == RX_SERVER_CONNECTION) {
		    /* This only actually destroys the connection if
		     * there are no outstanding calls */
		    MUTEX_ENTER(&conn->conn_data_lock);
                    MUTEX_ENTER(&rx_refcnt_mutex);
		    if (!havecalls && !conn->refCount
			&& ((conn->lastSendTime + rx_idleConnectionTime) <
			    now.sec)) {
			conn->refCount++;	/* it will be decr in rx_DestroyConn */
                        MUTEX_EXIT(&rx_refcnt_mutex);
			MUTEX_EXIT(&conn->conn_data_lock);
#ifdef RX_ENABLE_LOCKS
			rxi_DestroyConnectionNoLock(conn);
#else /* RX_ENABLE_LOCKS */
			rxi_DestroyConnection(conn);
#endif /* RX_ENABLE_LOCKS */
		    }
#ifdef RX_ENABLE_LOCKS
		    else {
                        MUTEX_EXIT(&rx_refcnt_mutex);
			MUTEX_EXIT(&conn->conn_data_lock);
		    }
#endif /* RX_ENABLE_LOCKS */
		}
	    }
	}
#ifdef RX_ENABLE_LOCKS
	while (rx_connCleanup_list) {
	    struct rx_connection *conn;
	    conn = rx_connCleanup_list;
	    rx_connCleanup_list = rx_connCleanup_list->next;
	    MUTEX_EXIT(&rx_connHashTable_lock);
	    rxi_CleanupConnection(conn);
	    MUTEX_ENTER(&rx_connHashTable_lock);
	}
	MUTEX_EXIT(&rx_connHashTable_lock);
#endif /* RX_ENABLE_LOCKS */
    }

    /* Find any peer structures that haven't been used (haven't had an
     * associated connection) for greater than rx_idlePeerTime */
    {
	struct rx_peer **peer_ptr, **peer_end;
	int code;

        /*
         * Why do we need to hold the rx_peerHashTable_lock across
         * the incrementing of peer_ptr since the rx_peerHashTable
         * array is not changing?  We don't.
         *
         * By dropping the lock periodically we can permit other
         * activities to be performed while a rxi_ReapConnections
         * call is in progress.  The goal of reap connections
         * is to clean up quickly without causing large amounts
         * of contention.  Therefore, it is important that global
         * mutexes not be held for extended periods of time.
         */
	for (peer_ptr = &rx_peerHashTable[0], peer_end =
	     &rx_peerHashTable[rx_hashTableSize]; peer_ptr < peer_end;
	     peer_ptr++) {
	    struct rx_peer *peer, *next, *prev;

            MUTEX_ENTER(&rx_peerHashTable_lock);
            for (prev = peer = *peer_ptr; peer; peer = next) {
		next = peer->next;
		code = MUTEX_TRYENTER(&peer->peer_lock);
		if ((code) && (peer->refCount == 0)
		    && ((peer->idleWhen + rx_idlePeerTime) < now.sec)) {
		    rx_interface_stat_p rpc_stat, nrpc_stat;
		    size_t space;

                    /*
                     * now know that this peer object is one to be
                     * removed from the hash table.  Once it is removed
                     * it can't be referenced by other threads.
                     * Lets remove it first and decrement the struct
                     * nPeerStructs count.
                     */
		    if (peer == *peer_ptr) {
			*peer_ptr = next;
			prev = next;
		    } else
			prev->next = next;

                    if (rx_stats_active)
                        rx_atomic_dec(&rx_stats.nPeerStructs);

                    /*
                     * Now if we hold references on 'prev' and 'next'
                     * we can safely drop the rx_peerHashTable_lock
                     * while we destroy this 'peer' object.
                     */
                    if (next)
                        next->refCount++;
                    if (prev)
                        prev->refCount++;
                    MUTEX_EXIT(&rx_peerHashTable_lock);

		    MUTEX_EXIT(&peer->peer_lock);
		    MUTEX_DESTROY(&peer->peer_lock);
		    for (queue_Scan
			 (&peer->rpcStats, rpc_stat, nrpc_stat,
			  rx_interface_stat)) {
			unsigned int num_funcs;
			if (!rpc_stat)
			    break;
			queue_Remove(&rpc_stat->queue_header);
			queue_Remove(&rpc_stat->all_peers);
			num_funcs = rpc_stat->stats[0].func_total;
			space =
			    sizeof(rx_interface_stat_t) +
			    rpc_stat->stats[0].func_total *
			    sizeof(rx_function_entry_v1_t);

			rxi_Free(rpc_stat, space);

                        MUTEX_ENTER(&rx_rpc_stats);
			rxi_rpc_peer_stat_cnt -= num_funcs;
                        MUTEX_EXIT(&rx_rpc_stats);
		    }
		    rxi_FreePeer(peer);

                    /*
                     * Regain the rx_peerHashTable_lock and
                     * decrement the reference count on 'prev'
                     * and 'next'.
                     */
                    MUTEX_ENTER(&rx_peerHashTable_lock);
                    if (next)
                        next->refCount--;
                    if (prev)
                        prev->refCount--;
		} else {
		    if (code) {
			MUTEX_EXIT(&peer->peer_lock);
		    }
		    prev = peer;
		}
	    }
            MUTEX_EXIT(&rx_peerHashTable_lock);
	}
    }

    /* THIS HACK IS A TEMPORARY HACK.  The idea is that the race condition in
     * rxi_AllocSendPacket, if it hits, will be handled at the next conn
     * GC, just below.  Really, we shouldn't have to keep moving packets from
     * one place to another, but instead ought to always know if we can
     * afford to hold onto a packet in its particular use.  */
    MUTEX_ENTER(&rx_freePktQ_lock);
    if (rx_waitingForPackets) {
	rx_waitingForPackets = 0;
#ifdef	RX_ENABLE_LOCKS
	CV_BROADCAST(&rx_waitingForPackets_cv);
#else
	osi_rxWakeup(&rx_waitingForPackets);
#endif
    }
    MUTEX_EXIT(&rx_freePktQ_lock);

    when = now;
    when.sec += RX_REAP_TIME;	/* Check every RX_REAP_TIME seconds */
    rxevent_Post(&when, rxi_ReapConnections, 0, 0);
}


/* rxs_Release - This isn't strictly necessary but, since the macro name from
 * rx.h is sort of strange this is better.  This is called with a security
 * object before it is discarded.  Each connection using a security object has
 * its own refcount to the object so it won't actually be freed until the last
 * connection is destroyed.
 *
 * This is the only rxs module call.  A hold could also be written but no one
 * needs it. */

int
rxs_Release(struct rx_securityClass *aobj)
{
    return RXS_Close(aobj);
}

#ifdef ADAPT_WINDOW
#define	RXRATE_PKT_OH	(RX_HEADER_SIZE + RX_IPUDP_SIZE)
#define	RXRATE_SMALL_PKT    (RXRATE_PKT_OH + sizeof(struct rx_ackPacket))
#define	RXRATE_AVG_SMALL_PKT	(RXRATE_PKT_OH + (sizeof(struct rx_ackPacket)/2))
#define	RXRATE_LARGE_PKT    (RXRATE_SMALL_PKT + 256)

/* Adjust our estimate of the transmission rate to this peer, given
 * that the packet p was just acked. We can adjust peer->timeout and
 * call->twind. Pragmatically, this is called
 * only with packets of maximal length.
 * Called with peer and call locked.
 */

static void
rxi_ComputeRate(struct rx_peer *peer, struct rx_call *call,
		struct rx_packet *p, struct rx_packet *ackp, u_char ackReason)
{
    afs_int32 xferSize, xferMs;
    afs_int32 minTime;
    struct clock newTO;

    /* Count down packets */
    if (peer->rateFlag > 0)
	peer->rateFlag--;
    /* Do nothing until we're enabled */
    if (peer->rateFlag != 0)
	return;
    if (!call->conn)
	return;

    /* Count only when the ack seems legitimate */
    switch (ackReason) {
    case RX_ACK_REQUESTED:
	xferSize =
	    p->length + RX_HEADER_SIZE + call->conn->securityMaxTrailerSize;
	xferMs = peer->rtt;
	break;

    case RX_ACK_PING_RESPONSE:
	if (p)			/* want the response to ping-request, not data send */
	    return;
	clock_GetTime(&newTO);
	if (clock_Gt(&newTO, &call->pingRequestTime)) {
	    clock_Sub(&newTO, &call->pingRequestTime);
	    xferMs = (newTO.sec * 1000) + (newTO.usec / 1000);
	} else {
	    return;
	}
	xferSize = rx_AckDataSize(rx_maxSendWindow) + RX_HEADER_SIZE;
	break;

    default:
	return;
    }

    dpf(("CONG peer %lx/%u: sample (%s) size %ld, %ld ms (to %d.%06d, rtt %u, ps %u)",
          ntohl(peer->host), ntohs(peer->port), (ackReason == RX_ACK_REQUESTED ? "dataack" : "pingack"),
          xferSize, xferMs, peer->timeout.sec, peer->timeout.usec, peer->smRtt, peer->ifMTU));

    /* Track only packets that are big enough. */
    if ((p->length + RX_HEADER_SIZE + call->conn->securityMaxTrailerSize) <
	peer->ifMTU)
	return;

    /* absorb RTT data (in milliseconds) for these big packets */
    if (peer->smRtt == 0) {
	peer->smRtt = xferMs;
    } else {
	peer->smRtt = ((peer->smRtt * 15) + xferMs + 4) >> 4;
	if (!peer->smRtt)
	    peer->smRtt = 1;
    }

    if (peer->countDown) {
	peer->countDown--;
	return;
    }
    peer->countDown = 10;	/* recalculate only every so often */

    /* In practice, we can measure only the RTT for full packets,
     * because of the way Rx acks the data that it receives.  (If it's
     * smaller than a full packet, it often gets implicitly acked
     * either by the call response (from a server) or by the next call
     * (from a client), and either case confuses transmission times
     * with processing times.)  Therefore, replace the above
     * more-sophisticated processing with a simpler version, where the
     * smoothed RTT is kept for full-size packets, and the time to
     * transmit a windowful of full-size packets is simply RTT *
     * windowSize. Again, we take two steps:
     - ensure the timeout is large enough for a single packet's RTT;
     - ensure that the window is small enough to fit in the desired timeout.*/

    /* First, the timeout check. */
    minTime = peer->smRtt;
    /* Get a reasonable estimate for a timeout period */
    minTime += minTime;
    newTO.sec = minTime / 1000;
    newTO.usec = (minTime - (newTO.sec * 1000)) * 1000;

    /* Increase the timeout period so that we can always do at least
     * one packet exchange */
    if (clock_Gt(&newTO, &peer->timeout)) {

	dpf(("CONG peer %lx/%u: timeout %d.%06d ==> %ld.%06d (rtt %u)",
              ntohl(peer->host), ntohs(peer->port), peer->timeout.sec, peer->timeout.usec,
              newTO.sec, newTO.usec, peer->smRtt));

	peer->timeout = newTO;
    }

    /* Now, get an estimate for the transmit window size. */
    minTime = peer->timeout.sec * 1000 + (peer->timeout.usec / 1000);
    /* Now, convert to the number of full packets that could fit in a
     * reasonable fraction of that interval */
    minTime /= (peer->smRtt << 1);
    minTime = MAX(minTime, rx_minPeerTimeout);
    xferSize = minTime;		/* (make a copy) */

    /* Now clamp the size to reasonable bounds. */
    if (minTime <= 1)
	minTime = 1;
    else if (minTime > rx_maxSendWindow)
	minTime = rx_maxSendWindow;
/*    if (minTime != peer->maxWindow) {
      dpf(("CONG peer %lx/%u: windowsize %lu ==> %lu (to %lu.%06lu, rtt %u)",
	     ntohl(peer->host), ntohs(peer->port), peer->maxWindow, minTime,
	     peer->timeout.sec, peer->timeout.usec, peer->smRtt));
      peer->maxWindow = minTime;
	elide... call->twind = minTime;
    }
*/

    /* Cut back on the peer timeout if it had earlier grown unreasonably.
     * Discern this by calculating the timeout necessary for rx_Window
     * packets. */
    if ((xferSize > rx_maxSendWindow) && (peer->timeout.sec >= 3)) {
	/* calculate estimate for transmission interval in milliseconds */
	minTime = rx_maxSendWindow * peer->smRtt;
	if (minTime < 1000) {
	    dpf(("CONG peer %lx/%u: cut TO %d.%06d by 0.5 (rtt %u)",
		 ntohl(peer->host), ntohs(peer->port), peer->timeout.sec,
		 peer->timeout.usec, peer->smRtt));

	    newTO.sec = 0;	/* cut back on timeout by half a second */
	    newTO.usec = 500000;
	    clock_Sub(&peer->timeout, &newTO);
	}
    }

    return;
}				/* end of rxi_ComputeRate */
#endif /* ADAPT_WINDOW */


void
rxi_DebugInit(void)
{
#ifdef RXDEBUG
#ifdef AFS_NT40_ENV
#define TRACE_OPTION_RX_DEBUG 16
    HKEY parmKey;
    DWORD dummyLen;
    DWORD TraceOption;
    long code;

    rxdebug_active = 0;

    code = RegOpenKeyEx(HKEY_LOCAL_MACHINE, AFSREG_CLT_SVC_PARAM_SUBKEY,
                         0, KEY_QUERY_VALUE, &parmKey);
    if (code != ERROR_SUCCESS)
	return;

    dummyLen = sizeof(TraceOption);
    code = RegQueryValueEx(parmKey, "TraceOption", NULL, NULL,
			   (BYTE *) &TraceOption, &dummyLen);
    if (code == ERROR_SUCCESS) {
	rxdebug_active = (TraceOption & TRACE_OPTION_RX_DEBUG) ? 1 : 0;
    }
    RegCloseKey (parmKey);
#endif /* AFS_NT40_ENV */
#endif
}

void
rx_DebugOnOff(int on)
{
#ifdef RXDEBUG
#ifdef AFS_NT40_ENV
    rxdebug_active = on;
#endif
#endif
}

void
rx_StatsOnOff(int on)
{
#ifdef RXDEBUG
    rx_stats_active = on;
#endif
}


/* Don't call this debugging routine directly; use dpf */
void
rxi_DebugPrint(char *format, ...)
{
#ifdef RXDEBUG
    va_list ap;
#ifdef AFS_NT40_ENV
    char msg[512];
    char tformat[256];
    size_t len;

    va_start(ap, format);

    len = _snprintf(tformat, sizeof(tformat), "tid[%d] %s", GetCurrentThreadId(), format);

    if (len > 0) {
	len = _vsnprintf(msg, sizeof(msg)-2, tformat, ap);
	if (len > 0) {
	    if (msg[len-1] != '\n') {
		msg[len] = '\n';
		msg[len+1] = '\0';
	    }
	    OutputDebugString(msg);
	}
    }
    va_end(ap);
#else
    struct clock now;

    va_start(ap, format);

    clock_GetTime(&now);
    fprintf(rx_Log, " %d.%06d:", (unsigned int)now.sec,
	    (unsigned int)now.usec);
    vfprintf(rx_Log, format, ap);
    putc('\n', rx_Log);
    va_end(ap);
#endif
#endif
}

#ifndef KERNEL
/*
 * This function is used to process the rx_stats structure that is local
 * to a process as well as an rx_stats structure received from a remote
 * process (via rxdebug).  Therefore, it needs to do minimal version
 * checking.
 */
void
rx_PrintTheseStats(FILE * file, struct rx_statistics *s, int size,
		   afs_int32 freePackets, char version)
{
    int i;

    if (size != sizeof(struct rx_statistics)) {
	fprintf(file,
		"Unexpected size of stats structure: was %d, expected %" AFS_SIZET_FMT "\n",
		size, sizeof(struct rx_statistics));
    }

    fprintf(file, "rx stats: free packets %d, allocs %d, ", (int)freePackets,
	    s->packetRequests);

    if (version >= RX_DEBUGI_VERSION_W_NEWPACKETTYPES) {
	fprintf(file, "alloc-failures(rcv %u/%u,send %u/%u,ack %u)\n",
		s->receivePktAllocFailures, s->receiveCbufPktAllocFailures,
		s->sendPktAllocFailures, s->sendCbufPktAllocFailures,
		s->specialPktAllocFailures);
    } else {
	fprintf(file, "alloc-failures(rcv %u,send %u,ack %u)\n",
		s->receivePktAllocFailures, s->sendPktAllocFailures,
		s->specialPktAllocFailures);
    }

    fprintf(file,
	    "   greedy %u, " "bogusReads %u (last from host %x), "
	    "noPackets %u, " "noBuffers %u, " "selects %u, "
	    "sendSelects %u\n", s->socketGreedy, s->bogusPacketOnRead,
	    s->bogusHost, s->noPacketOnRead, s->noPacketBuffersOnRead,
	    s->selects, s->sendSelects);

    fprintf(file, "   packets read: ");
    for (i = 0; i < RX_N_PACKET_TYPES; i++) {
	fprintf(file, "%s %u ", rx_packetTypes[i], s->packetsRead[i]);
    }
    fprintf(file, "\n");

    fprintf(file,
	    "   other read counters: data %u, " "ack %u, " "dup %u "
	    "spurious %u " "dally %u\n", s->dataPacketsRead,
	    s->ackPacketsRead, s->dupPacketsRead, s->spuriousPacketsRead,
	    s->ignorePacketDally);

    fprintf(file, "   packets sent: ");
    for (i = 0; i < RX_N_PACKET_TYPES; i++) {
	fprintf(file, "%s %u ", rx_packetTypes[i], s->packetsSent[i]);
    }
    fprintf(file, "\n");

    fprintf(file,
	    "   other send counters: ack %u, " "data %u (not resends), "
	    "resends %u, " "pushed %u, " "acked&ignored %u\n",
	    s->ackPacketsSent, s->dataPacketsSent, s->dataPacketsReSent,
	    s->dataPacketsPushed, s->ignoreAckedPacket);

    fprintf(file,
	    "   \t(these should be small) sendFailed %u, " "fatalErrors %u\n",
	    s->netSendFailures, (int)s->fatalErrors);

    if (s->nRttSamples) {
	fprintf(file, "   Average rtt is %0.3f, with %d samples\n",
		clock_Float(&s->totalRtt) / s->nRttSamples, s->nRttSamples);

	fprintf(file, "   Minimum rtt is %0.3f, maximum is %0.3f\n",
		clock_Float(&s->minRtt), clock_Float(&s->maxRtt));
    }

    fprintf(file,
	    "   %d server connections, " "%d client connections, "
	    "%d peer structs, " "%d call structs, " "%d free call structs\n",
	    s->nServerConns, s->nClientConns, s->nPeerStructs,
	    s->nCallStructs, s->nFreeCallStructs);

#if	!defined(AFS_PTHREAD_ENV) && !defined(AFS_USE_GETTIMEOFDAY)
    fprintf(file, "   %d clock updates\n", clock_nUpdates);
#endif
}

/* for backward compatibility */
void
rx_PrintStats(FILE * file)
{
    MUTEX_ENTER(&rx_stats_mutex);
    rx_PrintTheseStats(file, (struct rx_statistics *) &rx_stats,
		       sizeof(rx_stats), rx_nFreePackets,
		       RX_DEBUGI_VERSION);
    MUTEX_EXIT(&rx_stats_mutex);
}

void
rx_PrintPeerStats(FILE * file, struct rx_peer *peer)
{
    fprintf(file, "Peer %x.%d.  " "Burst size %d, " "burst wait %d.%06d.\n",
	    ntohl(peer->host), (int)ntohs(peer->port), (int)peer->burstSize,
	    (int)peer->burstWait.sec, (int)peer->burstWait.usec);

    fprintf(file,
	    "   Rtt %d, " "retry time %u.%06d, " "total sent %d, "
	    "resent %d\n", peer->rtt, (int)peer->timeout.sec,
	    (int)peer->timeout.usec, peer->nSent, peer->reSends);

    fprintf(file,
	    "   Packet size %d, " "max in packet skew %d, "
	    "max out packet skew %d\n", peer->ifMTU, (int)peer->inPacketSkew,
	    (int)peer->outPacketSkew);
}
#endif

#if defined(AFS_PTHREAD_ENV) && defined(RXDEBUG)
/*
 * This mutex protects the following static variables:
 * counter
 */

#define LOCK_RX_DEBUG MUTEX_ENTER(&rx_debug_mutex)
#define UNLOCK_RX_DEBUG MUTEX_EXIT(&rx_debug_mutex)
#else
#define LOCK_RX_DEBUG
#define UNLOCK_RX_DEBUG
#endif /* AFS_PTHREAD_ENV */

#if defined(RXDEBUG) || defined(MAKEDEBUGCALL)
static int
MakeDebugCall(osi_socket socket, afs_uint32 remoteAddr, afs_uint16 remotePort,
	      u_char type, void *inputData, size_t inputLength,
	      void *outputData, size_t outputLength)
{
    static afs_int32 counter = 100;
    time_t waitTime, waitCount;
    struct rx_header theader;
    char tbuffer[1500];
    afs_int32 code;
    struct timeval tv_now, tv_wake, tv_delta;
    struct sockaddr_in taddr, faddr;
#ifdef AFS_NT40_ENV
    int faddrLen;
#else
    socklen_t faddrLen;
#endif
    fd_set imask;
    char *tp;

    waitTime = 1;
    waitCount = 5;
    LOCK_RX_DEBUG;
    counter++;
    UNLOCK_RX_DEBUG;
    tp = &tbuffer[sizeof(struct rx_header)];
    taddr.sin_family = AF_INET;
    taddr.sin_port = remotePort;
    taddr.sin_addr.s_addr = remoteAddr;
#ifdef STRUCT_SOCKADDR_HAS_SA_LEN
    taddr.sin_len = sizeof(struct sockaddr_in);
#endif
    while (1) {
	memset(&theader, 0, sizeof(theader));
	theader.epoch = htonl(999);
	theader.cid = 0;
	theader.callNumber = htonl(counter);
	theader.seq = 0;
	theader.serial = 0;
	theader.type = type;
	theader.flags = RX_CLIENT_INITIATED | RX_LAST_PACKET;
	theader.serviceId = 0;

	memcpy(tbuffer, &theader, sizeof(theader));
	memcpy(tp, inputData, inputLength);
	code =
	    sendto(socket, tbuffer, inputLength + sizeof(struct rx_header), 0,
		   (struct sockaddr *)&taddr, sizeof(struct sockaddr_in));

	/* see if there's a packet available */
	gettimeofday(&tv_wake,0);
	tv_wake.tv_sec += waitTime;
	for (;;) {
	    FD_ZERO(&imask);
	    FD_SET(socket, &imask);
	    tv_delta.tv_sec = tv_wake.tv_sec;
	    tv_delta.tv_usec = tv_wake.tv_usec;
	    gettimeofday(&tv_now, 0);

	    if (tv_delta.tv_usec < tv_now.tv_usec) {
		/* borrow */
		tv_delta.tv_usec += 1000000;
		tv_delta.tv_sec--;
	    }
	    tv_delta.tv_usec -= tv_now.tv_usec;

	    if (tv_delta.tv_sec < tv_now.tv_sec) {
		/* time expired */
		break;
	    }
	    tv_delta.tv_sec -= tv_now.tv_sec;

#ifdef AFS_NT40_ENV
	    code = select(0, &imask, 0, 0, &tv_delta);
#else /* AFS_NT40_ENV */
	    code = select(socket + 1, &imask, 0, 0, &tv_delta);
#endif /* AFS_NT40_ENV */
	    if (code == 1 && FD_ISSET(socket, &imask)) {
		/* now receive a packet */
		faddrLen = sizeof(struct sockaddr_in);
		code =
		    recvfrom(socket, tbuffer, sizeof(tbuffer), 0,
			     (struct sockaddr *)&faddr, &faddrLen);

		if (code > 0) {
		    memcpy(&theader, tbuffer, sizeof(struct rx_header));
		    if (counter == ntohl(theader.callNumber))
			goto success;
		    continue;
		}
	    }
	    break;
	}

	/* see if we've timed out */
	if (!--waitCount) {
            return -1;
	}
	waitTime <<= 1;
    }

 success:
    code -= sizeof(struct rx_header);
    if (code > outputLength)
	code = outputLength;
    memcpy(outputData, tp, code);
    return code;
}
#endif /* RXDEBUG */

afs_int32
rx_GetServerDebug(osi_socket socket, afs_uint32 remoteAddr,
		  afs_uint16 remotePort, struct rx_debugStats * stat,
		  afs_uint32 * supportedValues)
{
#if defined(RXDEBUG) || defined(MAKEDEBUGCALL)
    afs_int32 rc = 0;
    struct rx_debugIn in;

    *supportedValues = 0;
    in.type = htonl(RX_DEBUGI_GETSTATS);
    in.index = 0;

    rc = MakeDebugCall(socket, remoteAddr, remotePort, RX_PACKET_TYPE_DEBUG,
		       &in, sizeof(in), stat, sizeof(*stat));

    /*
     * If the call was successful, fixup the version and indicate
     * what contents of the stat structure are valid.
     * Also do net to host conversion of fields here.
     */

    if (rc >= 0) {
	if (stat->version >= RX_DEBUGI_VERSION_W_SECSTATS) {
	    *supportedValues |= RX_SERVER_DEBUG_SEC_STATS;
	}
	if (stat->version >= RX_DEBUGI_VERSION_W_GETALLCONN) {
	    *supportedValues |= RX_SERVER_DEBUG_ALL_CONN;
	}
	if (stat->version >= RX_DEBUGI_VERSION_W_RXSTATS) {
	    *supportedValues |= RX_SERVER_DEBUG_RX_STATS;
	}
	if (stat->version >= RX_DEBUGI_VERSION_W_WAITERS) {
	    *supportedValues |= RX_SERVER_DEBUG_WAITER_CNT;
	}
	if (stat->version >= RX_DEBUGI_VERSION_W_IDLETHREADS) {
	    *supportedValues |= RX_SERVER_DEBUG_IDLE_THREADS;
	}
	if (stat->version >= RX_DEBUGI_VERSION_W_NEWPACKETTYPES) {
	    *supportedValues |= RX_SERVER_DEBUG_NEW_PACKETS;
	}
	if (stat->version >= RX_DEBUGI_VERSION_W_GETPEER) {
	    *supportedValues |= RX_SERVER_DEBUG_ALL_PEER;
	}
	if (stat->version >= RX_DEBUGI_VERSION_W_WAITED) {
	    *supportedValues |= RX_SERVER_DEBUG_WAITED_CNT;
	}
	if (stat->version >= RX_DEBUGI_VERSION_W_PACKETS) {
	    *supportedValues |= RX_SERVER_DEBUG_PACKETS_CNT;
	}
	stat->nFreePackets = ntohl(stat->nFreePackets);
	stat->packetReclaims = ntohl(stat->packetReclaims);
	stat->callsExecuted = ntohl(stat->callsExecuted);
	stat->nWaiting = ntohl(stat->nWaiting);
	stat->idleThreads = ntohl(stat->idleThreads);
        stat->nWaited = ntohl(stat->nWaited);
        stat->nPackets = ntohl(stat->nPackets);
    }
#else
    afs_int32 rc = -1;
#endif
    return rc;
}

afs_int32
rx_GetServerStats(osi_socket socket, afs_uint32 remoteAddr,
		  afs_uint16 remotePort, struct rx_statistics * stat,
		  afs_uint32 * supportedValues)
{
#if defined(RXDEBUG) || defined(MAKEDEBUGCALL)
    afs_int32 rc = 0;
    struct rx_debugIn in;
    afs_int32 *lp = (afs_int32 *) stat;
    int i;

    /*
     * supportedValues is currently unused, but added to allow future
     * versioning of this function.
     */

    *supportedValues = 0;
    in.type = htonl(RX_DEBUGI_RXSTATS);
    in.index = 0;
    memset(stat, 0, sizeof(*stat));

    rc = MakeDebugCall(socket, remoteAddr, remotePort, RX_PACKET_TYPE_DEBUG,
		       &in, sizeof(in), stat, sizeof(*stat));

    if (rc >= 0) {

	/*
	 * Do net to host conversion here
	 */

	for (i = 0; i < sizeof(*stat) / sizeof(afs_int32); i++, lp++) {
	    *lp = ntohl(*lp);
	}
    }
#else
    afs_int32 rc = -1;
#endif
    return rc;
}

afs_int32
rx_GetServerVersion(osi_socket socket, afs_uint32 remoteAddr,
		    afs_uint16 remotePort, size_t version_length,
		    char *version)
{
#if defined(RXDEBUG) || defined(MAKEDEBUGCALL)
    char a[1] = { 0 };
    return MakeDebugCall(socket, remoteAddr, remotePort,
			 RX_PACKET_TYPE_VERSION, a, 1, version,
			 version_length);
#else
    return -1;
#endif
}

afs_int32
rx_GetServerConnections(osi_socket socket, afs_uint32 remoteAddr,
			afs_uint16 remotePort, afs_int32 * nextConnection,
			int allConnections, afs_uint32 debugSupportedValues,
			struct rx_debugConn * conn,
			afs_uint32 * supportedValues)
{
#if defined(RXDEBUG) || defined(MAKEDEBUGCALL)
    afs_int32 rc = 0;
    struct rx_debugIn in;
    int i;

    /*
     * supportedValues is currently unused, but added to allow future
     * versioning of this function.
     */

    *supportedValues = 0;
    if (allConnections) {
	in.type = htonl(RX_DEBUGI_GETALLCONN);
    } else {
	in.type = htonl(RX_DEBUGI_GETCONN);
    }
    in.index = htonl(*nextConnection);
    memset(conn, 0, sizeof(*conn));

    rc = MakeDebugCall(socket, remoteAddr, remotePort, RX_PACKET_TYPE_DEBUG,
		       &in, sizeof(in), conn, sizeof(*conn));

    if (rc >= 0) {
	*nextConnection += 1;

	/*
	 * Convert old connection format to new structure.
	 */

	if (debugSupportedValues & RX_SERVER_DEBUG_OLD_CONN) {
	    struct rx_debugConn_vL *vL = (struct rx_debugConn_vL *)conn;
#define MOVEvL(a) (conn->a = vL->a)

	    /* any old or unrecognized version... */
	    for (i = 0; i < RX_MAXCALLS; i++) {
		MOVEvL(callState[i]);
		MOVEvL(callMode[i]);
		MOVEvL(callFlags[i]);
		MOVEvL(callOther[i]);
	    }
	    if (debugSupportedValues & RX_SERVER_DEBUG_SEC_STATS) {
		MOVEvL(secStats.type);
		MOVEvL(secStats.level);
		MOVEvL(secStats.flags);
		MOVEvL(secStats.expires);
		MOVEvL(secStats.packetsReceived);
		MOVEvL(secStats.packetsSent);
		MOVEvL(secStats.bytesReceived);
		MOVEvL(secStats.bytesSent);
	    }
	}

	/*
	 * Do net to host conversion here
	 * NOTE:
	 *    I don't convert host or port since we are most likely
	 *    going to want these in NBO.
	 */
	conn->cid = ntohl(conn->cid);
	conn->serial = ntohl(conn->serial);
	for (i = 0; i < RX_MAXCALLS; i++) {
	    conn->callNumber[i] = ntohl(conn->callNumber[i]);
	}
	conn->error = ntohl(conn->error);
	conn->secStats.flags = ntohl(conn->secStats.flags);
	conn->secStats.expires = ntohl(conn->secStats.expires);
	conn->secStats.packetsReceived =
	    ntohl(conn->secStats.packetsReceived);
	conn->secStats.packetsSent = ntohl(conn->secStats.packetsSent);
	conn->secStats.bytesReceived = ntohl(conn->secStats.bytesReceived);
	conn->secStats.bytesSent = ntohl(conn->secStats.bytesSent);
	conn->epoch = ntohl(conn->epoch);
	conn->natMTU = ntohl(conn->natMTU);
    }
#else
    afs_int32 rc = -1;
#endif
    return rc;
}

afs_int32
rx_GetServerPeers(osi_socket socket, afs_uint32 remoteAddr,
		  afs_uint16 remotePort, afs_int32 * nextPeer,
		  afs_uint32 debugSupportedValues, struct rx_debugPeer * peer,
		  afs_uint32 * supportedValues)
{
#if defined(RXDEBUG) || defined(MAKEDEBUGCALL)
    afs_int32 rc = 0;
    struct rx_debugIn in;

    /*
     * supportedValues is currently unused, but added to allow future
     * versioning of this function.
     */

    *supportedValues = 0;
    in.type = htonl(RX_DEBUGI_GETPEER);
    in.index = htonl(*nextPeer);
    memset(peer, 0, sizeof(*peer));

    rc = MakeDebugCall(socket, remoteAddr, remotePort, RX_PACKET_TYPE_DEBUG,
		       &in, sizeof(in), peer, sizeof(*peer));

    if (rc >= 0) {
	*nextPeer += 1;

	/*
	 * Do net to host conversion here
	 * NOTE:
	 *    I don't convert host or port since we are most likely
	 *    going to want these in NBO.
	 */
	peer->ifMTU = ntohs(peer->ifMTU);
	peer->idleWhen = ntohl(peer->idleWhen);
	peer->refCount = ntohs(peer->refCount);
	peer->burstWait.sec = ntohl(peer->burstWait.sec);
	peer->burstWait.usec = ntohl(peer->burstWait.usec);
	peer->rtt = ntohl(peer->rtt);
	peer->rtt_dev = ntohl(peer->rtt_dev);
	peer->timeout.sec = ntohl(peer->timeout.sec);
	peer->timeout.usec = ntohl(peer->timeout.usec);
	peer->nSent = ntohl(peer->nSent);
	peer->reSends = ntohl(peer->reSends);
	peer->inPacketSkew = ntohl(peer->inPacketSkew);
	peer->outPacketSkew = ntohl(peer->outPacketSkew);
	peer->rateFlag = ntohl(peer->rateFlag);
	peer->natMTU = ntohs(peer->natMTU);
	peer->maxMTU = ntohs(peer->maxMTU);
	peer->maxDgramPackets = ntohs(peer->maxDgramPackets);
	peer->ifDgramPackets = ntohs(peer->ifDgramPackets);
	peer->MTU = ntohs(peer->MTU);
	peer->cwind = ntohs(peer->cwind);
	peer->nDgramPackets = ntohs(peer->nDgramPackets);
	peer->congestSeq = ntohs(peer->congestSeq);
	peer->bytesSent.high = ntohl(peer->bytesSent.high);
	peer->bytesSent.low = ntohl(peer->bytesSent.low);
	peer->bytesReceived.high = ntohl(peer->bytesReceived.high);
	peer->bytesReceived.low = ntohl(peer->bytesReceived.low);
    }
#else
    afs_int32 rc = -1;
#endif
    return rc;
}

afs_int32
rx_GetLocalPeers(afs_uint32 peerHost, afs_uint16 peerPort,
		struct rx_debugPeer * peerStats)
{
	struct rx_peer *tp;
	afs_int32 error = 1; /* default to "did not succeed" */
	afs_uint32 hashValue = PEER_HASH(peerHost, peerPort);

	MUTEX_ENTER(&rx_peerHashTable_lock);
	for(tp = rx_peerHashTable[hashValue];
	      tp != NULL; tp = tp->next) {
		if (tp->host == peerHost)
			break;
	}

	if (tp) {
                tp->refCount++;
                MUTEX_EXIT(&rx_peerHashTable_lock);

		error = 0;

                MUTEX_ENTER(&tp->peer_lock);
		peerStats->host = tp->host;
		peerStats->port = tp->port;
		peerStats->ifMTU = tp->ifMTU;
		peerStats->idleWhen = tp->idleWhen;
		peerStats->refCount = tp->refCount;
		peerStats->burstSize = tp->burstSize;
		peerStats->burst = tp->burst;
		peerStats->burstWait.sec = tp->burstWait.sec;
		peerStats->burstWait.usec = tp->burstWait.usec;
		peerStats->rtt = tp->rtt;
		peerStats->rtt_dev = tp->rtt_dev;
		peerStats->timeout.sec = tp->timeout.sec;
		peerStats->timeout.usec = tp->timeout.usec;
		peerStats->nSent = tp->nSent;
		peerStats->reSends = tp->reSends;
		peerStats->inPacketSkew = tp->inPacketSkew;
		peerStats->outPacketSkew = tp->outPacketSkew;
		peerStats->rateFlag = tp->rateFlag;
		peerStats->natMTU = tp->natMTU;
		peerStats->maxMTU = tp->maxMTU;
		peerStats->maxDgramPackets = tp->maxDgramPackets;
		peerStats->ifDgramPackets = tp->ifDgramPackets;
		peerStats->MTU = tp->MTU;
		peerStats->cwind = tp->cwind;
		peerStats->nDgramPackets = tp->nDgramPackets;
		peerStats->congestSeq = tp->congestSeq;
		peerStats->bytesSent.high = tp->bytesSent.high;
		peerStats->bytesSent.low = tp->bytesSent.low;
		peerStats->bytesReceived.high = tp->bytesReceived.high;
		peerStats->bytesReceived.low = tp->bytesReceived.low;
                MUTEX_EXIT(&tp->peer_lock);

                MUTEX_ENTER(&rx_peerHashTable_lock);
                tp->refCount--;
	}
	MUTEX_EXIT(&rx_peerHashTable_lock);

	return error;
}

void
shutdown_rx(void)
{
    struct rx_serverQueueEntry *np;
    int i, j;
#ifndef KERNEL
    struct rx_call *call;
    struct rx_serverQueueEntry *sq;
#endif /* KERNEL */

    LOCK_RX_INIT;
    if (rxinit_status == 1) {
	UNLOCK_RX_INIT;
	return;			/* Already shutdown. */
    }
#ifndef KERNEL
    rx_port = 0;
#ifndef AFS_PTHREAD_ENV
    FD_ZERO(&rx_selectMask);
#endif /* AFS_PTHREAD_ENV */
    rxi_dataQuota = RX_MAX_QUOTA;
#ifndef AFS_PTHREAD_ENV
    rxi_StopListener();
#endif /* AFS_PTHREAD_ENV */
    shutdown_rxevent();
    rx_SetEpoch(0);
#ifndef AFS_PTHREAD_ENV
#ifndef AFS_USE_GETTIMEOFDAY
    clock_UnInit();
#endif /* AFS_USE_GETTIMEOFDAY */
#endif /* AFS_PTHREAD_ENV */

    while (!queue_IsEmpty(&rx_freeCallQueue)) {
	call = queue_First(&rx_freeCallQueue, rx_call);
	queue_Remove(call);
	rxi_Free(call, sizeof(struct rx_call));
    }

    while (!queue_IsEmpty(&rx_idleServerQueue)) {
	sq = queue_First(&rx_idleServerQueue, rx_serverQueueEntry);
	queue_Remove(sq);
    }
#endif /* KERNEL */

    {
	struct rx_peer **peer_ptr, **peer_end;
	for (peer_ptr = &rx_peerHashTable[0], peer_end =
	     &rx_peerHashTable[rx_hashTableSize]; peer_ptr < peer_end;
	     peer_ptr++) {
	    struct rx_peer *peer, *next;

            MUTEX_ENTER(&rx_peerHashTable_lock);
            for (peer = *peer_ptr; peer; peer = next) {
		rx_interface_stat_p rpc_stat, nrpc_stat;
		size_t space;

                MUTEX_ENTER(&rx_rpc_stats);
                MUTEX_ENTER(&peer->peer_lock);
		for (queue_Scan
		     (&peer->rpcStats, rpc_stat, nrpc_stat,
		      rx_interface_stat)) {
		    unsigned int num_funcs;
		    if (!rpc_stat)
			break;
		    queue_Remove(&rpc_stat->queue_header);
		    queue_Remove(&rpc_stat->all_peers);
		    num_funcs = rpc_stat->stats[0].func_total;
		    space =
			sizeof(rx_interface_stat_t) +
			rpc_stat->stats[0].func_total *
			sizeof(rx_function_entry_v1_t);

		    rxi_Free(rpc_stat, space);

                    /* rx_rpc_stats must be held */
		    rxi_rpc_peer_stat_cnt -= num_funcs;
		}
                MUTEX_EXIT(&peer->peer_lock);
                MUTEX_EXIT(&rx_rpc_stats);

		next = peer->next;
		rxi_FreePeer(peer);
                if (rx_stats_active)
                    rx_atomic_dec(&rx_stats.nPeerStructs);
	    }
            MUTEX_EXIT(&rx_peerHashTable_lock);
	}
    }
    for (i = 0; i < RX_MAX_SERVICES; i++) {
	if (rx_services[i])
	    rxi_Free(rx_services[i], sizeof(*rx_services[i]));
    }
    for (i = 0; i < rx_hashTableSize; i++) {
	struct rx_connection *tc, *ntc;
	MUTEX_ENTER(&rx_connHashTable_lock);
	for (tc = rx_connHashTable[i]; tc; tc = ntc) {
	    ntc = tc->next;
	    for (j = 0; j < RX_MAXCALLS; j++) {
		if (tc->call[j]) {
		    rxi_Free(tc->call[j], sizeof(*tc->call[j]));
		}
	    }
	    rxi_Free(tc, sizeof(*tc));
	}
	MUTEX_EXIT(&rx_connHashTable_lock);
    }

    MUTEX_ENTER(&freeSQEList_lock);

    while ((np = rx_FreeSQEList)) {
	rx_FreeSQEList = *(struct rx_serverQueueEntry **)np;
	MUTEX_DESTROY(&np->lock);
	rxi_Free(np, sizeof(*np));
    }

    MUTEX_EXIT(&freeSQEList_lock);
    MUTEX_DESTROY(&freeSQEList_lock);
    MUTEX_DESTROY(&rx_freeCallQueue_lock);
    MUTEX_DESTROY(&rx_connHashTable_lock);
    MUTEX_DESTROY(&rx_peerHashTable_lock);
    MUTEX_DESTROY(&rx_serverPool_lock);

    osi_Free(rx_connHashTable,
	     rx_hashTableSize * sizeof(struct rx_connection *));
    osi_Free(rx_peerHashTable, rx_hashTableSize * sizeof(struct rx_peer *));

    UNPIN(rx_connHashTable,
	  rx_hashTableSize * sizeof(struct rx_connection *));
    UNPIN(rx_peerHashTable, rx_hashTableSize * sizeof(struct rx_peer *));

    rxi_FreeAllPackets();

    MUTEX_ENTER(&rx_quota_mutex);
    rxi_dataQuota = RX_MAX_QUOTA;
    rxi_availProcs = rxi_totalMin = rxi_minDeficit = 0;
    MUTEX_EXIT(&rx_quota_mutex);
    rxinit_status = 1;
    UNLOCK_RX_INIT;
}

#ifdef RX_ENABLE_LOCKS
void
osirx_AssertMine(afs_kmutex_t * lockaddr, char *msg)
{
    if (!MUTEX_ISMINE(lockaddr))
	osi_Panic("Lock not held: %s", msg);
}
#endif /* RX_ENABLE_LOCKS */

#ifndef KERNEL

/*
 * Routines to implement connection specific data.
 */

int
rx_KeyCreate(rx_destructor_t rtn)
{
    int key;
    MUTEX_ENTER(&rxi_keyCreate_lock);
    key = rxi_keyCreate_counter++;
    rxi_keyCreate_destructor = (rx_destructor_t *)
	realloc((void *)rxi_keyCreate_destructor,
		(key + 1) * sizeof(rx_destructor_t));
    rxi_keyCreate_destructor[key] = rtn;
    MUTEX_EXIT(&rxi_keyCreate_lock);
    return key;
}

void
rx_SetSpecific(struct rx_connection *conn, int key, void *ptr)
{
    int i;
    MUTEX_ENTER(&conn->conn_data_lock);
    if (!conn->specific) {
	conn->specific = (void **)malloc((key + 1) * sizeof(void *));
	for (i = 0; i < key; i++)
	    conn->specific[i] = NULL;
	conn->nSpecific = key + 1;
	conn->specific[key] = ptr;
    } else if (key >= conn->nSpecific) {
	conn->specific = (void **)
	    realloc(conn->specific, (key + 1) * sizeof(void *));
	for (i = conn->nSpecific; i < key; i++)
	    conn->specific[i] = NULL;
	conn->nSpecific = key + 1;
	conn->specific[key] = ptr;
    } else {
	if (conn->specific[key] && rxi_keyCreate_destructor[key])
	    (*rxi_keyCreate_destructor[key]) (conn->specific[key]);
	conn->specific[key] = ptr;
    }
    MUTEX_EXIT(&conn->conn_data_lock);
}

void
rx_SetServiceSpecific(struct rx_service *svc, int key, void *ptr)
{
    int i;
    MUTEX_ENTER(&svc->svc_data_lock);
    if (!svc->specific) {
	svc->specific = (void **)malloc((key + 1) * sizeof(void *));
	for (i = 0; i < key; i++)
	    svc->specific[i] = NULL;
	svc->nSpecific = key + 1;
	svc->specific[key] = ptr;
    } else if (key >= svc->nSpecific) {
	svc->specific = (void **)
	    realloc(svc->specific, (key + 1) * sizeof(void *));
	for (i = svc->nSpecific; i < key; i++)
	    svc->specific[i] = NULL;
	svc->nSpecific = key + 1;
	svc->specific[key] = ptr;
    } else {
	if (svc->specific[key] && rxi_keyCreate_destructor[key])
	    (*rxi_keyCreate_destructor[key]) (svc->specific[key]);
	svc->specific[key] = ptr;
    }
    MUTEX_EXIT(&svc->svc_data_lock);
}

void *
rx_GetSpecific(struct rx_connection *conn, int key)
{
    void *ptr;
    MUTEX_ENTER(&conn->conn_data_lock);
    if (key >= conn->nSpecific)
	ptr = NULL;
    else
	ptr = conn->specific[key];
    MUTEX_EXIT(&conn->conn_data_lock);
    return ptr;
}

void *
rx_GetServiceSpecific(struct rx_service *svc, int key)
{
    void *ptr;
    MUTEX_ENTER(&svc->svc_data_lock);
    if (key >= svc->nSpecific)
	ptr = NULL;
    else
	ptr = svc->specific[key];
    MUTEX_EXIT(&svc->svc_data_lock);
    return ptr;
}


#endif /* !KERNEL */

/*
 * processStats is a queue used to store the statistics for the local
 * process.  Its contents are similar to the contents of the rpcStats
 * queue on a rx_peer structure, but the actual data stored within
 * this queue contains totals across the lifetime of the process (assuming
 * the stats have not been reset) - unlike the per peer structures
 * which can come and go based upon the peer lifetime.
 */

static struct rx_queue processStats = { &processStats, &processStats };

/*
 * peerStats is a queue used to store the statistics for all peer structs.
 * Its contents are the union of all the peer rpcStats queues.
 */

static struct rx_queue peerStats = { &peerStats, &peerStats };

/*
 * rxi_monitor_processStats is used to turn process wide stat collection
 * on and off
 */

static int rxi_monitor_processStats = 0;

/*
 * rxi_monitor_peerStats is used to turn per peer stat collection on and off
 */

static int rxi_monitor_peerStats = 0;

/*
 * rxi_AddRpcStat - given all of the information for a particular rpc
 * call, create (if needed) and update the stat totals for the rpc.
 *
 * PARAMETERS
 *
 * IN stats - the queue of stats that will be updated with the new value
 *
 * IN rxInterface - a unique number that identifies the rpc interface
 *
 * IN currentFunc - the index of the function being invoked
 *
 * IN totalFunc - the total number of functions in this interface
 *
 * IN queueTime - the amount of time this function waited for a thread
 *
 * IN execTime - the amount of time this function invocation took to execute
 *
 * IN bytesSent - the number bytes sent by this invocation
 *
 * IN bytesRcvd - the number bytes received by this invocation
 *
 * IN isServer - if true, this invocation was made to a server
 *
 * IN remoteHost - the ip address of the remote host
 *
 * IN remotePort - the port of the remote host
 *
 * IN addToPeerList - if != 0, add newly created stat to the global peer list
 *
 * INOUT counter - if a new stats structure is allocated, the counter will
 * be updated with the new number of allocated stat structures
 *
 * RETURN CODES
 *
 * Returns void.
 */

static int
rxi_AddRpcStat(struct rx_queue *stats, afs_uint32 rxInterface,
	       afs_uint32 currentFunc, afs_uint32 totalFunc,
	       struct clock *queueTime, struct clock *execTime,
	       afs_hyper_t * bytesSent, afs_hyper_t * bytesRcvd, int isServer,
	       afs_uint32 remoteHost, afs_uint32 remotePort,
	       int addToPeerList, unsigned int *counter)
{
    int rc = 0;
    rx_interface_stat_p rpc_stat, nrpc_stat;

    /*
     * See if there's already a structure for this interface
     */

    for (queue_Scan(stats, rpc_stat, nrpc_stat, rx_interface_stat)) {
	if ((rpc_stat->stats[0].interfaceId == rxInterface)
	    && (rpc_stat->stats[0].remote_is_server == isServer))
	    break;
    }

    /*
     * Didn't find a match so allocate a new structure and add it to the
     * queue.
     */

    if (queue_IsEnd(stats, rpc_stat) || (rpc_stat == NULL)
	|| (rpc_stat->stats[0].interfaceId != rxInterface)
	|| (rpc_stat->stats[0].remote_is_server != isServer)) {
	int i;
	size_t space;

	space =
	    sizeof(rx_interface_stat_t) +
	    totalFunc * sizeof(rx_function_entry_v1_t);

	rpc_stat = rxi_Alloc(space);
	if (rpc_stat == NULL) {
	    rc = 1;
	    goto fail;
	}
	*counter += totalFunc;
	for (i = 0; i < totalFunc; i++) {
	    rpc_stat->stats[i].remote_peer = remoteHost;
	    rpc_stat->stats[i].remote_port = remotePort;
	    rpc_stat->stats[i].remote_is_server = isServer;
	    rpc_stat->stats[i].interfaceId = rxInterface;
	    rpc_stat->stats[i].func_total = totalFunc;
	    rpc_stat->stats[i].func_index = i;
	    hzero(rpc_stat->stats[i].invocations);
	    hzero(rpc_stat->stats[i].bytes_sent);
	    hzero(rpc_stat->stats[i].bytes_rcvd);
	    rpc_stat->stats[i].queue_time_sum.sec = 0;
	    rpc_stat->stats[i].queue_time_sum.usec = 0;
	    rpc_stat->stats[i].queue_time_sum_sqr.sec = 0;
	    rpc_stat->stats[i].queue_time_sum_sqr.usec = 0;
	    rpc_stat->stats[i].queue_time_min.sec = 9999999;
	    rpc_stat->stats[i].queue_time_min.usec = 9999999;
	    rpc_stat->stats[i].queue_time_max.sec = 0;
	    rpc_stat->stats[i].queue_time_max.usec = 0;
	    rpc_stat->stats[i].execution_time_sum.sec = 0;
	    rpc_stat->stats[i].execution_time_sum.usec = 0;
	    rpc_stat->stats[i].execution_time_sum_sqr.sec = 0;
	    rpc_stat->stats[i].execution_time_sum_sqr.usec = 0;
	    rpc_stat->stats[i].execution_time_min.sec = 9999999;
	    rpc_stat->stats[i].execution_time_min.usec = 9999999;
	    rpc_stat->stats[i].execution_time_max.sec = 0;
	    rpc_stat->stats[i].execution_time_max.usec = 0;
	}
	queue_Prepend(stats, rpc_stat);
	if (addToPeerList) {
	    queue_Prepend(&peerStats, &rpc_stat->all_peers);
	}
    }

    /*
     * Increment the stats for this function
     */

    hadd32(rpc_stat->stats[currentFunc].invocations, 1);
    hadd(rpc_stat->stats[currentFunc].bytes_sent, *bytesSent);
    hadd(rpc_stat->stats[currentFunc].bytes_rcvd, *bytesRcvd);
    clock_Add(&rpc_stat->stats[currentFunc].queue_time_sum, queueTime);
    clock_AddSq(&rpc_stat->stats[currentFunc].queue_time_sum_sqr, queueTime);
    if (clock_Lt(queueTime, &rpc_stat->stats[currentFunc].queue_time_min)) {
	rpc_stat->stats[currentFunc].queue_time_min = *queueTime;
    }
    if (clock_Gt(queueTime, &rpc_stat->stats[currentFunc].queue_time_max)) {
	rpc_stat->stats[currentFunc].queue_time_max = *queueTime;
    }
    clock_Add(&rpc_stat->stats[currentFunc].execution_time_sum, execTime);
    clock_AddSq(&rpc_stat->stats[currentFunc].execution_time_sum_sqr,
		execTime);
    if (clock_Lt(execTime, &rpc_stat->stats[currentFunc].execution_time_min)) {
	rpc_stat->stats[currentFunc].execution_time_min = *execTime;
    }
    if (clock_Gt(execTime, &rpc_stat->stats[currentFunc].execution_time_max)) {
	rpc_stat->stats[currentFunc].execution_time_max = *execTime;
    }

  fail:
    return rc;
}

/*
 * rx_IncrementTimeAndCount - increment the times and count for a particular
 * rpc function.
 *
 * PARAMETERS
 *
 * IN peer - the peer who invoked the rpc
 *
 * IN rxInterface - a unique number that identifies the rpc interface
 *
 * IN currentFunc - the index of the function being invoked
 *
 * IN totalFunc - the total number of functions in this interface
 *
 * IN queueTime - the amount of time this function waited for a thread
 *
 * IN execTime - the amount of time this function invocation took to execute
 *
 * IN bytesSent - the number bytes sent by this invocation
 *
 * IN bytesRcvd - the number bytes received by this invocation
 *
 * IN isServer - if true, this invocation was made to a server
 *
 * RETURN CODES
 *
 * Returns void.
 */

void
rx_IncrementTimeAndCount(struct rx_peer *peer, afs_uint32 rxInterface,
			 afs_uint32 currentFunc, afs_uint32 totalFunc,
			 struct clock *queueTime, struct clock *execTime,
			 afs_hyper_t * bytesSent, afs_hyper_t * bytesRcvd,
			 int isServer)
{

    if (!(rxi_monitor_peerStats || rxi_monitor_processStats))
        return;

    MUTEX_ENTER(&rx_rpc_stats);

    if (rxi_monitor_peerStats) {
        MUTEX_ENTER(&peer->peer_lock);
	rxi_AddRpcStat(&peer->rpcStats, rxInterface, currentFunc, totalFunc,
		       queueTime, execTime, bytesSent, bytesRcvd, isServer,
		       peer->host, peer->port, 1, &rxi_rpc_peer_stat_cnt);
        MUTEX_EXIT(&peer->peer_lock);
    }

    if (rxi_monitor_processStats) {
	rxi_AddRpcStat(&processStats, rxInterface, currentFunc, totalFunc,
		       queueTime, execTime, bytesSent, bytesRcvd, isServer,
		       0xffffffff, 0xffffffff, 0, &rxi_rpc_process_stat_cnt);
    }

    MUTEX_EXIT(&rx_rpc_stats);

}

/*
 * rx_MarshallProcessRPCStats - marshall an array of rpc statistics
 *
 * PARAMETERS
 *
 * IN callerVersion - the rpc stat version of the caller.
 *
 * IN count - the number of entries to marshall.
 *
 * IN stats - pointer to stats to be marshalled.
 *
 * OUT ptr - Where to store the marshalled data.
 *
 * RETURN CODES
 *
 * Returns void.
 */
void
rx_MarshallProcessRPCStats(afs_uint32 callerVersion, int count,
			   rx_function_entry_v1_t * stats, afs_uint32 ** ptrP)
{
    int i;
    afs_uint32 *ptr;

    /*
     * We only support the first version
     */
    for (ptr = *ptrP, i = 0; i < count; i++, stats++) {
	*(ptr++) = stats->remote_peer;
	*(ptr++) = stats->remote_port;
	*(ptr++) = stats->remote_is_server;
	*(ptr++) = stats->interfaceId;
	*(ptr++) = stats->func_total;
	*(ptr++) = stats->func_index;
	*(ptr++) = hgethi(stats->invocations);
	*(ptr++) = hgetlo(stats->invocations);
	*(ptr++) = hgethi(stats->bytes_sent);
	*(ptr++) = hgetlo(stats->bytes_sent);
	*(ptr++) = hgethi(stats->bytes_rcvd);
	*(ptr++) = hgetlo(stats->bytes_rcvd);
	*(ptr++) = stats->queue_time_sum.sec;
	*(ptr++) = stats->queue_time_sum.usec;
	*(ptr++) = stats->queue_time_sum_sqr.sec;
	*(ptr++) = stats->queue_time_sum_sqr.usec;
	*(ptr++) = stats->queue_time_min.sec;
	*(ptr++) = stats->queue_time_min.usec;
	*(ptr++) = stats->queue_time_max.sec;
	*(ptr++) = stats->queue_time_max.usec;
	*(ptr++) = stats->execution_time_sum.sec;
	*(ptr++) = stats->execution_time_sum.usec;
	*(ptr++) = stats->execution_time_sum_sqr.sec;
	*(ptr++) = stats->execution_time_sum_sqr.usec;
	*(ptr++) = stats->execution_time_min.sec;
	*(ptr++) = stats->execution_time_min.usec;
	*(ptr++) = stats->execution_time_max.sec;
	*(ptr++) = stats->execution_time_max.usec;
    }
    *ptrP = ptr;
}

/*
 * rx_RetrieveProcessRPCStats - retrieve all of the rpc statistics for
 * this process
 *
 * PARAMETERS
 *
 * IN callerVersion - the rpc stat version of the caller
 *
 * OUT myVersion - the rpc stat version of this function
 *
 * OUT clock_sec - local time seconds
 *
 * OUT clock_usec - local time microseconds
 *
 * OUT allocSize - the number of bytes allocated to contain stats
 *
 * OUT statCount - the number stats retrieved from this process.
 *
 * OUT stats - the actual stats retrieved from this process.
 *
 * RETURN CODES
 *
 * Returns void.  If successful, stats will != NULL.
 */

int
rx_RetrieveProcessRPCStats(afs_uint32 callerVersion, afs_uint32 * myVersion,
			   afs_uint32 * clock_sec, afs_uint32 * clock_usec,
			   size_t * allocSize, afs_uint32 * statCount,
			   afs_uint32 ** stats)
{
    size_t space = 0;
    afs_uint32 *ptr;
    struct clock now;
    int rc = 0;

    *stats = 0;
    *allocSize = 0;
    *statCount = 0;
    *myVersion = RX_STATS_RETRIEVAL_VERSION;

    /*
     * Check to see if stats are enabled
     */

    MUTEX_ENTER(&rx_rpc_stats);
    if (!rxi_monitor_processStats) {
	MUTEX_EXIT(&rx_rpc_stats);
	return rc;
    }

    clock_GetTime(&now);
    *clock_sec = now.sec;
    *clock_usec = now.usec;

    /*
     * Allocate the space based upon the caller version
     *
     * If the client is at an older version than we are,
     * we return the statistic data in the older data format, but
     * we still return our version number so the client knows we
     * are maintaining more data than it can retrieve.
     */

    if (callerVersion >= RX_STATS_RETRIEVAL_FIRST_EDITION) {
	space = rxi_rpc_process_stat_cnt * sizeof(rx_function_entry_v1_t);
	*statCount = rxi_rpc_process_stat_cnt;
    } else {
	/*
	 * This can't happen yet, but in the future version changes
	 * can be handled by adding additional code here
	 */
    }

    if (space > (size_t) 0) {
	*allocSize = space;
	ptr = *stats = rxi_Alloc(space);

	if (ptr != NULL) {
	    rx_interface_stat_p rpc_stat, nrpc_stat;


	    for (queue_Scan
		 (&processStats, rpc_stat, nrpc_stat, rx_interface_stat)) {
		/*
		 * Copy the data based upon the caller version
		 */
		rx_MarshallProcessRPCStats(callerVersion,
					   rpc_stat->stats[0].func_total,
					   rpc_stat->stats, &ptr);
	    }
	} else {
	    rc = ENOMEM;
	}
    }
    MUTEX_EXIT(&rx_rpc_stats);
    return rc;
}

/*
 * rx_RetrievePeerRPCStats - retrieve all of the rpc statistics for the peers
 *
 * PARAMETERS
 *
 * IN callerVersion - the rpc stat version of the caller
 *
 * OUT myVersion - the rpc stat version of this function
 *
 * OUT clock_sec - local time seconds
 *
 * OUT clock_usec - local time microseconds
 *
 * OUT allocSize - the number of bytes allocated to contain stats
 *
 * OUT statCount - the number of stats retrieved from the individual
 * peer structures.
 *
 * OUT stats - the actual stats retrieved from the individual peer structures.
 *
 * RETURN CODES
 *
 * Returns void.  If successful, stats will != NULL.
 */

int
rx_RetrievePeerRPCStats(afs_uint32 callerVersion, afs_uint32 * myVersion,
			afs_uint32 * clock_sec, afs_uint32 * clock_usec,
			size_t * allocSize, afs_uint32 * statCount,
			afs_uint32 ** stats)
{
    size_t space = 0;
    afs_uint32 *ptr;
    struct clock now;
    int rc = 0;

    *stats = 0;
    *statCount = 0;
    *allocSize = 0;
    *myVersion = RX_STATS_RETRIEVAL_VERSION;

    /*
     * Check to see if stats are enabled
     */

    MUTEX_ENTER(&rx_rpc_stats);
    if (!rxi_monitor_peerStats) {
	MUTEX_EXIT(&rx_rpc_stats);
	return rc;
    }

    clock_GetTime(&now);
    *clock_sec = now.sec;
    *clock_usec = now.usec;

    /*
     * Allocate the space based upon the caller version
     *
     * If the client is at an older version than we are,
     * we return the statistic data in the older data format, but
     * we still return our version number so the client knows we
     * are maintaining more data than it can retrieve.
     */

    if (callerVersion >= RX_STATS_RETRIEVAL_FIRST_EDITION) {
	space = rxi_rpc_peer_stat_cnt * sizeof(rx_function_entry_v1_t);
	*statCount = rxi_rpc_peer_stat_cnt;
    } else {
	/*
	 * This can't happen yet, but in the future version changes
	 * can be handled by adding additional code here
	 */
    }

    if (space > (size_t) 0) {
	*allocSize = space;
	ptr = *stats = rxi_Alloc(space);

	if (ptr != NULL) {
	    rx_interface_stat_p rpc_stat, nrpc_stat;
	    char *fix_offset;

	    for (queue_Scan
		 (&peerStats, rpc_stat, nrpc_stat, rx_interface_stat)) {
		/*
		 * We have to fix the offset of rpc_stat since we are
		 * keeping this structure on two rx_queues.  The rx_queue
		 * package assumes that the rx_queue member is the first
		 * member of the structure.  That is, rx_queue assumes that
		 * any one item is only on one queue at a time.  We are
		 * breaking that assumption and so we have to do a little
		 * math to fix our pointers.
		 */

		fix_offset = (char *)rpc_stat;
		fix_offset -= offsetof(rx_interface_stat_t, all_peers);
		rpc_stat = (rx_interface_stat_p) fix_offset;

		/*
		 * Copy the data based upon the caller version
		 */
		rx_MarshallProcessRPCStats(callerVersion,
					   rpc_stat->stats[0].func_total,
					   rpc_stat->stats, &ptr);
	    }
	} else {
	    rc = ENOMEM;
	}
    }
    MUTEX_EXIT(&rx_rpc_stats);
    return rc;
}

/*
 * rx_FreeRPCStats - free memory allocated by
 *                   rx_RetrieveProcessRPCStats and rx_RetrievePeerRPCStats
 *
 * PARAMETERS
 *
 * IN stats - stats previously returned by rx_RetrieveProcessRPCStats or
 * rx_RetrievePeerRPCStats
 *
 * IN allocSize - the number of bytes in stats.
 *
 * RETURN CODES
 *
 * Returns void.
 */

void
rx_FreeRPCStats(afs_uint32 * stats, size_t allocSize)
{
    rxi_Free(stats, allocSize);
}

/*
 * rx_queryProcessRPCStats - see if process rpc stat collection is
 * currently enabled.
 *
 * PARAMETERS
 *
 * RETURN CODES
 *
 * Returns 0 if stats are not enabled != 0 otherwise
 */

int
rx_queryProcessRPCStats(void)
{
    int rc;
    MUTEX_ENTER(&rx_rpc_stats);
    rc = rxi_monitor_processStats;
    MUTEX_EXIT(&rx_rpc_stats);
    return rc;
}

/*
 * rx_queryPeerRPCStats - see if peer stat collection is currently enabled.
 *
 * PARAMETERS
 *
 * RETURN CODES
 *
 * Returns 0 if stats are not enabled != 0 otherwise
 */

int
rx_queryPeerRPCStats(void)
{
    int rc;
    MUTEX_ENTER(&rx_rpc_stats);
    rc = rxi_monitor_peerStats;
    MUTEX_EXIT(&rx_rpc_stats);
    return rc;
}

/*
 * rx_enableProcessRPCStats - begin rpc stat collection for entire process
 *
 * PARAMETERS
 *
 * RETURN CODES
 *
 * Returns void.
 */

void
rx_enableProcessRPCStats(void)
{
    MUTEX_ENTER(&rx_rpc_stats);
    rx_enable_stats = 1;
    rxi_monitor_processStats = 1;
    MUTEX_EXIT(&rx_rpc_stats);
}

/*
 * rx_enablePeerRPCStats - begin rpc stat collection per peer structure
 *
 * PARAMETERS
 *
 * RETURN CODES
 *
 * Returns void.
 */

void
rx_enablePeerRPCStats(void)
{
    MUTEX_ENTER(&rx_rpc_stats);
    rx_enable_stats = 1;
    rxi_monitor_peerStats = 1;
    MUTEX_EXIT(&rx_rpc_stats);
}

/*
 * rx_disableProcessRPCStats - stop rpc stat collection for entire process
 *
 * PARAMETERS
 *
 * RETURN CODES
 *
 * Returns void.
 */

void
rx_disableProcessRPCStats(void)
{
    rx_interface_stat_p rpc_stat, nrpc_stat;
    size_t space;

    MUTEX_ENTER(&rx_rpc_stats);

    /*
     * Turn off process statistics and if peer stats is also off, turn
     * off everything
     */

    rxi_monitor_processStats = 0;
    if (rxi_monitor_peerStats == 0) {
	rx_enable_stats = 0;
    }

    for (queue_Scan(&processStats, rpc_stat, nrpc_stat, rx_interface_stat)) {
	unsigned int num_funcs = 0;
	if (!rpc_stat)
	    break;
	queue_Remove(rpc_stat);
	num_funcs = rpc_stat->stats[0].func_total;
	space =
	    sizeof(rx_interface_stat_t) +
	    rpc_stat->stats[0].func_total * sizeof(rx_function_entry_v1_t);

	rxi_Free(rpc_stat, space);
	rxi_rpc_process_stat_cnt -= num_funcs;
    }
    MUTEX_EXIT(&rx_rpc_stats);
}

/*
 * rx_disablePeerRPCStats - stop rpc stat collection for peers
 *
 * PARAMETERS
 *
 * RETURN CODES
 *
 * Returns void.
 */

void
rx_disablePeerRPCStats(void)
{
    struct rx_peer **peer_ptr, **peer_end;
    int code;

    /*
     * Turn off peer statistics and if process stats is also off, turn
     * off everything
     */

    rxi_monitor_peerStats = 0;
    if (rxi_monitor_processStats == 0) {
	rx_enable_stats = 0;
    }

    for (peer_ptr = &rx_peerHashTable[0], peer_end =
	 &rx_peerHashTable[rx_hashTableSize]; peer_ptr < peer_end;
	 peer_ptr++) {
	struct rx_peer *peer, *next, *prev;

        MUTEX_ENTER(&rx_peerHashTable_lock);
        MUTEX_ENTER(&rx_rpc_stats);
        for (prev = peer = *peer_ptr; peer; peer = next) {
	    next = peer->next;
	    code = MUTEX_TRYENTER(&peer->peer_lock);
	    if (code) {
		rx_interface_stat_p rpc_stat, nrpc_stat;
		size_t space;

		if (prev == *peer_ptr) {
		    *peer_ptr = next;
		    prev = next;
		} else
		    prev->next = next;

                if (next)
                    next->refCount++;
                if (prev)
                    prev->refCount++;
                peer->refCount++;
                MUTEX_EXIT(&rx_peerHashTable_lock);

                for (queue_Scan
		     (&peer->rpcStats, rpc_stat, nrpc_stat,
		      rx_interface_stat)) {
		    unsigned int num_funcs = 0;
		    if (!rpc_stat)
			break;
		    queue_Remove(&rpc_stat->queue_header);
		    queue_Remove(&rpc_stat->all_peers);
		    num_funcs = rpc_stat->stats[0].func_total;
		    space =
			sizeof(rx_interface_stat_t) +
			rpc_stat->stats[0].func_total *
			sizeof(rx_function_entry_v1_t);

		    rxi_Free(rpc_stat, space);
		    rxi_rpc_peer_stat_cnt -= num_funcs;
		}
		MUTEX_EXIT(&peer->peer_lock);

                MUTEX_ENTER(&rx_peerHashTable_lock);
                if (next)
                    next->refCount--;
                if (prev)
                    prev->refCount--;
                peer->refCount--;
	    } else {
		prev = peer;
	    }
	}
        MUTEX_EXIT(&rx_rpc_stats);
        MUTEX_EXIT(&rx_peerHashTable_lock);
    }
}

/*
 * rx_clearProcessRPCStats - clear the contents of the rpc stats according
 * to clearFlag
 *
 * PARAMETERS
 *
 * IN clearFlag - flag indicating which stats to clear
 *
 * RETURN CODES
 *
 * Returns void.
 */

void
rx_clearProcessRPCStats(afs_uint32 clearFlag)
{
    rx_interface_stat_p rpc_stat, nrpc_stat;

    MUTEX_ENTER(&rx_rpc_stats);

    for (queue_Scan(&processStats, rpc_stat, nrpc_stat, rx_interface_stat)) {
	unsigned int num_funcs = 0, i;
	num_funcs = rpc_stat->stats[0].func_total;
	for (i = 0; i < num_funcs; i++) {
	    if (clearFlag & AFS_RX_STATS_CLEAR_INVOCATIONS) {
		hzero(rpc_stat->stats[i].invocations);
	    }
	    if (clearFlag & AFS_RX_STATS_CLEAR_BYTES_SENT) {
		hzero(rpc_stat->stats[i].bytes_sent);
	    }
	    if (clearFlag & AFS_RX_STATS_CLEAR_BYTES_RCVD) {
		hzero(rpc_stat->stats[i].bytes_rcvd);
	    }
	    if (clearFlag & AFS_RX_STATS_CLEAR_QUEUE_TIME_SUM) {
		rpc_stat->stats[i].queue_time_sum.sec = 0;
		rpc_stat->stats[i].queue_time_sum.usec = 0;
	    }
	    if (clearFlag & AFS_RX_STATS_CLEAR_QUEUE_TIME_SQUARE) {
		rpc_stat->stats[i].queue_time_sum_sqr.sec = 0;
		rpc_stat->stats[i].queue_time_sum_sqr.usec = 0;
	    }
	    if (clearFlag & AFS_RX_STATS_CLEAR_QUEUE_TIME_MIN) {
		rpc_stat->stats[i].queue_time_min.sec = 9999999;
		rpc_stat->stats[i].queue_time_min.usec = 9999999;
	    }
	    if (clearFlag & AFS_RX_STATS_CLEAR_QUEUE_TIME_MAX) {
		rpc_stat->stats[i].queue_time_max.sec = 0;
		rpc_stat->stats[i].queue_time_max.usec = 0;
	    }
	    if (clearFlag & AFS_RX_STATS_CLEAR_EXEC_TIME_SUM) {
		rpc_stat->stats[i].execution_time_sum.sec = 0;
		rpc_stat->stats[i].execution_time_sum.usec = 0;
	    }
	    if (clearFlag & AFS_RX_STATS_CLEAR_EXEC_TIME_SQUARE) {
		rpc_stat->stats[i].execution_time_sum_sqr.sec = 0;
		rpc_stat->stats[i].execution_time_sum_sqr.usec = 0;
	    }
	    if (clearFlag & AFS_RX_STATS_CLEAR_EXEC_TIME_MIN) {
		rpc_stat->stats[i].execution_time_min.sec = 9999999;
		rpc_stat->stats[i].execution_time_min.usec = 9999999;
	    }
	    if (clearFlag & AFS_RX_STATS_CLEAR_EXEC_TIME_MAX) {
		rpc_stat->stats[i].execution_time_max.sec = 0;
		rpc_stat->stats[i].execution_time_max.usec = 0;
	    }
	}
    }

    MUTEX_EXIT(&rx_rpc_stats);
}

/*
 * rx_clearPeerRPCStats - clear the contents of the rpc stats according
 * to clearFlag
 *
 * PARAMETERS
 *
 * IN clearFlag - flag indicating which stats to clear
 *
 * RETURN CODES
 *
 * Returns void.
 */

void
rx_clearPeerRPCStats(afs_uint32 clearFlag)
{
    rx_interface_stat_p rpc_stat, nrpc_stat;

    MUTEX_ENTER(&rx_rpc_stats);

    for (queue_Scan(&peerStats, rpc_stat, nrpc_stat, rx_interface_stat)) {
	unsigned int num_funcs = 0, i;
	char *fix_offset;
	/*
	 * We have to fix the offset of rpc_stat since we are
	 * keeping this structure on two rx_queues.  The rx_queue
	 * package assumes that the rx_queue member is the first
	 * member of the structure.  That is, rx_queue assumes that
	 * any one item is only on one queue at a time.  We are
	 * breaking that assumption and so we have to do a little
	 * math to fix our pointers.
	 */

	fix_offset = (char *)rpc_stat;
	fix_offset -= offsetof(rx_interface_stat_t, all_peers);
	rpc_stat = (rx_interface_stat_p) fix_offset;

	num_funcs = rpc_stat->stats[0].func_total;
	for (i = 0; i < num_funcs; i++) {
	    if (clearFlag & AFS_RX_STATS_CLEAR_INVOCATIONS) {
		hzero(rpc_stat->stats[i].invocations);
	    }
	    if (clearFlag & AFS_RX_STATS_CLEAR_BYTES_SENT) {
		hzero(rpc_stat->stats[i].bytes_sent);
	    }
	    if (clearFlag & AFS_RX_STATS_CLEAR_BYTES_RCVD) {
		hzero(rpc_stat->stats[i].bytes_rcvd);
	    }
	    if (clearFlag & AFS_RX_STATS_CLEAR_QUEUE_TIME_SUM) {
		rpc_stat->stats[i].queue_time_sum.sec = 0;
		rpc_stat->stats[i].queue_time_sum.usec = 0;
	    }
	    if (clearFlag & AFS_RX_STATS_CLEAR_QUEUE_TIME_SQUARE) {
		rpc_stat->stats[i].queue_time_sum_sqr.sec = 0;
		rpc_stat->stats[i].queue_time_sum_sqr.usec = 0;
	    }
	    if (clearFlag & AFS_RX_STATS_CLEAR_QUEUE_TIME_MIN) {
		rpc_stat->stats[i].queue_time_min.sec = 9999999;
		rpc_stat->stats[i].queue_time_min.usec = 9999999;
	    }
	    if (clearFlag & AFS_RX_STATS_CLEAR_QUEUE_TIME_MAX) {
		rpc_stat->stats[i].queue_time_max.sec = 0;
		rpc_stat->stats[i].queue_time_max.usec = 0;
	    }
	    if (clearFlag & AFS_RX_STATS_CLEAR_EXEC_TIME_SUM) {
		rpc_stat->stats[i].execution_time_sum.sec = 0;
		rpc_stat->stats[i].execution_time_sum.usec = 0;
	    }
	    if (clearFlag & AFS_RX_STATS_CLEAR_EXEC_TIME_SQUARE) {
		rpc_stat->stats[i].execution_time_sum_sqr.sec = 0;
		rpc_stat->stats[i].execution_time_sum_sqr.usec = 0;
	    }
	    if (clearFlag & AFS_RX_STATS_CLEAR_EXEC_TIME_MIN) {
		rpc_stat->stats[i].execution_time_min.sec = 9999999;
		rpc_stat->stats[i].execution_time_min.usec = 9999999;
	    }
	    if (clearFlag & AFS_RX_STATS_CLEAR_EXEC_TIME_MAX) {
		rpc_stat->stats[i].execution_time_max.sec = 0;
		rpc_stat->stats[i].execution_time_max.usec = 0;
	    }
	}
    }

    MUTEX_EXIT(&rx_rpc_stats);
}

/*
 * rxi_rxstat_userok points to a routine that returns 1 if the caller
 * is authorized to enable/disable/clear RX statistics.
 */
static int (*rxi_rxstat_userok) (struct rx_call * call) = NULL;

void
rx_SetRxStatUserOk(int (*proc) (struct rx_call * call))
{
    rxi_rxstat_userok = proc;
}

int
rx_RxStatUserOk(struct rx_call *call)
{
    if (!rxi_rxstat_userok)
	return 0;
    return rxi_rxstat_userok(call);
}

#ifdef AFS_NT40_ENV
/*
 * DllMain() -- Entry-point function called by the DllMainCRTStartup()
 *     function in the MSVC runtime DLL (msvcrt.dll).
 *
 *     Note: the system serializes calls to this function.
 */
BOOL WINAPI
DllMain(HINSTANCE dllInstHandle,	/* instance handle for this DLL module */
	DWORD reason,			/* reason function is being called */
	LPVOID reserved)		/* reserved for future use */
{
    switch (reason) {
    case DLL_PROCESS_ATTACH:
	/* library is being attached to a process */
	INIT_PTHREAD_LOCKS;
	return TRUE;

    case DLL_PROCESS_DETACH:
    	return TRUE;

    default:
	return FALSE;
    }
}
#endif /* AFS_NT40_ENV */

#ifndef KERNEL
int rx_DumpCalls(FILE *outputFile, char *cookie)
{
#ifdef RXDEBUG_PACKET
#ifdef KDUMP_RX_LOCK
    struct rx_call_rx_lock *c;
#else
    struct rx_call *c;
#endif
#ifdef AFS_NT40_ENV
    int zilch;
    char output[2048];
#define RXDPRINTF sprintf
#define RXDPRINTOUT output
#else
#define RXDPRINTF fprintf
#define RXDPRINTOUT outputFile
#endif

    RXDPRINTF(RXDPRINTOUT, "%s - Start dumping all Rx Calls - count=%u\r\n", cookie, rx_stats.nCallStructs);
#ifdef AFS_NT40_ENV
    WriteFile(outputFile, output, (DWORD)strlen(output), &zilch, NULL);
#endif

    for (c = rx_allCallsp; c; c = c->allNextp) {
        u_short rqc, tqc, iovqc;
        struct rx_packet *p, *np;

        MUTEX_ENTER(&c->lock);
        queue_Count(&c->rq, p, np, rx_packet, rqc);
        queue_Count(&c->tq, p, np, rx_packet, tqc);
        queue_Count(&c->iovq, p, np, rx_packet, iovqc);

	RXDPRINTF(RXDPRINTOUT, "%s - call=0x%p, id=%u, state=%u, mode=%u, conn=%p, epoch=%u, cid=%u, callNum=%u, connFlags=0x%x, flags=0x%x, "
                "rqc=%u,%u, tqc=%u,%u, iovqc=%u,%u, "
                "lstatus=%u, rstatus=%u, error=%d, timeout=%u, "
                "resendEvent=%d, timeoutEvt=%d, keepAliveEvt=%d, delayedAckEvt=%d, delayedAbortEvt=%d, abortCode=%d, abortCount=%d, "
                "lastSendTime=%u, lastRecvTime=%u, lastSendData=%u"
#ifdef RX_ENABLE_LOCKS
                ", refCount=%u"
#endif
#ifdef RX_REFCOUNT_CHECK
                ", refCountBegin=%u, refCountResend=%u, refCountDelay=%u, "
                "refCountAlive=%u, refCountPacket=%u, refCountSend=%u, refCountAckAll=%u, refCountAbort=%u"
#endif
                "\r\n",
                cookie, c, c->call_id, (afs_uint32)c->state, (afs_uint32)c->mode, c->conn, c->conn?c->conn->epoch:0, c->conn?c->conn->cid:0,
                c->callNumber?*c->callNumber:0, c->conn?c->conn->flags:0, c->flags,
                (afs_uint32)c->rqc, (afs_uint32)rqc, (afs_uint32)c->tqc, (afs_uint32)tqc, (afs_uint32)c->iovqc, (afs_uint32)iovqc,
                (afs_uint32)c->localStatus, (afs_uint32)c->remoteStatus, c->error, c->timeout,
                c->resendEvent?1:0, c->timeoutEvent?1:0, c->keepAliveEvent?1:0, c->delayedAckEvent?1:0, c->delayedAbortEvent?1:0,
                c->abortCode, c->abortCount, c->lastSendTime, c->lastReceiveTime, c->lastSendData
#ifdef RX_ENABLE_LOCKS
                , (afs_uint32)c->refCount
#endif
#ifdef RX_REFCOUNT_CHECK
                , c->refCDebug[0],c->refCDebug[1],c->refCDebug[2],c->refCDebug[3],c->refCDebug[4],c->refCDebug[5],c->refCDebug[6],c->refCDebug[7]
#endif
                );
        MUTEX_EXIT(&c->lock);

#ifdef AFS_NT40_ENV
        WriteFile(outputFile, output, (DWORD)strlen(output), &zilch, NULL);
#endif
    }
    RXDPRINTF(RXDPRINTOUT, "%s - End dumping all Rx Calls\r\n", cookie);
#ifdef AFS_NT40_ENV
    WriteFile(outputFile, output, (DWORD)strlen(output), &zilch, NULL);
#endif
#endif /* RXDEBUG_PACKET */
    return 0;
}
#endif
