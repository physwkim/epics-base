/*************************************************************************\
* Copyright (c) 2008 UChicago Argonne LLC, as Operator of Argonne
*     National Laboratory.
* Copyright (c) 2002 The Regents of the University of California, as
*     Operator of Los Alamos National Laboratory.
* Copyright (c) 2013 ITER Organization.
* SPDX-License-Identifier: EPICS
* EPICS BASE is distributed subject to a Software License Agreement found
* in file LICENSE that is included with this distribution.
\*************************************************************************/
/* callback.c */

/* general purpose callback tasks               */
/*
 *      Original Author:        Marty Kraimer
 *      Date:                   07-18-91
*/

#include <stddef.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include "cantProceed.h"
#include "dbDefs.h"
#include <stdint.h>

#include "epicsAtomic.h"
#include "epicsEvent.h"
#include "epicsInterrupt.h"
#include "epicsString.h"
#include "epicsThread.h"
#include "epicsTimer.h"
#include "errlog.h"
#include "errMdef.h"
#include "taskwd.h"

#include "callback.h"
#include "dbAccessDefs.h"
#include "dbAddr.h"
#include "dbBase.h"
#include "dbCommon.h"
#include "dbFldTypes.h"
#include "dbLock.h"
#include "dbStaticLib.h"
#include "epicsExport.h"
#include "link.h"
#include "recSup.h"
#include "dbUnitTest.h" /* for testSyncCallback() */


static int callbackQueueSize = 2000;

/* Each request takes a node from a lock-free pool of callbackQueueSize
 * nodes (so the same epicsCallback may be queued several times, as with
 * the ring buffer, and the pool running out is the "buffer full" error)
 * and pushes it onto an intrusive LIFO with one CAS. Neither has an
 * owner, so a preempted requester can never block a worker. A worker
 * takes the whole LIFO with one CAS and reverses it, restoring FIFO,
 * runs it in batches of CB_SHARE_EVERY and publishes the rest in its
 * pending slot. Before each callback a worker that still holds pending
 * work hands half of it to a sleeping worker, so no request waits in a
 * worker's hand while another worker sleeps. No lock anywhere on the
 * request path, no shared wake-up event, and no change to epicsCallback.
 *
 * Each worker has its own event and a state word: AWAKE, SLEEPING or
 * CLAIMED, tagged with the sleep epoch; only the worker moves out of
 * CLAIMED. A wake triggers the worker's event first and claims the
 * worker second, so a thread stopped between the two steps leaves
 * nothing that others wait for; the claim is a CAS on the state word,
 * so it can never land on a later sleep than the one its trigger woke.
 * A handed-over list is stored in the worker's handoff slot before the
 * trigger and is taken by the worker at the top of its loop and before
 * it sleeps, so it is delivered whether or not the claim succeeds; the
 * slot is a stack, so a second list handed over before the first was
 * taken joins it.
 *
 * A worker sets its bit in sleepers before it sleeps. The bit is taken
 * by CAS by the first thread that wants that sleeper, before anything
 * else it does, so other threads see at once that the sleeper is spoken
 * for and keep their work; the worker clears the bit itself if nobody
 * took it. The bit is a hint, never the truth: a request that finds no
 * bit while nAwake says a worker sleeps scans the state words, so a
 * thread stopped between taking a bit and triggering hides that worker
 * from sharers only, not from the rescue.
 *
 * A requester wakes a worker only when none is awake, plus one rescue
 * for a worker stopped where the kernel cannot migrate it: when exactly
 * one worker is counted awake and no batch was started for CB_STALE_US,
 * the requester wakes another. Time, not a request count, since the
 * requesters may be slow while the backlog they see is stuck. Once two
 * are counted the second either runs or was woken by the first, which
 * recruits by handing over work. An idle worker also steals from
 * another worker's handoff or pending slot before sleeping, looking at
 * most once per CB_STEAL_AGE_NS and taking only a list it already saw
 * there at its previous look while the owner made no progress since: a
 * running worker empties its slots within microseconds, a stopped one
 * does not, so a stopped worker holds at most one batch and a running
 * one is never robbed.
 */
#ifndef CB_SHARE_EVERY
#define CB_SHARE_EVERY 16
#endif
#ifndef CB_STALE_US
#define CB_STALE_US 20
#endif
#ifndef CB_STEAL_AGE_NS
#define CB_STEAL_AGE_NS 200000
#endif

/* worker state word: sleep epoch << 2 | state */
#define CB_AWAKE    0
#define CB_SLEEPING 1
#define CB_CLAIMED  2
#define CB_STATE(e, st) (((size_t)(e) << 2) | (st))
#define CB_ST(s)        ((s) & 3)
#define CB_EPOCH(s)     ((s) >> 2)
#define CB_MAX_WORKERS (8 * sizeof(size_t))

/* index of the lowest set bit of a non-zero mask */
#if defined(__GNUC__)
#  define CB_LOWBIT(m) ((unsigned)__builtin_ctzll(m))
#else
static unsigned cbLowBit(size_t m)
{
    unsigned i = 0;
    while (!(m & 1)) { m >>= 1; i++; }
    return i;
}
#  define CB_LOWBIT(m) cbLowBit(m)
#endif

/* each worker on its own cache lines: the slots are written per batch */
#define CB_WORKER_ALIGN 128
#if defined(_MSC_VER)
#  define CB_ALIGN_PRE  __declspec(align(CB_WORKER_ALIGN))
#  define CB_ALIGN_POST
#elif defined(__GNUC__)
#  define CB_ALIGN_PRE
#  define CB_ALIGN_POST __attribute__((aligned(CB_WORKER_ALIGN)))
#else
#  define CB_ALIGN_PRE
#  define CB_ALIGN_POST
#endif

typedef struct cbNode {
    epicsCallback *cb;
    struct cbNode *next;
} cbNode;

/* free list head: node index + ABA tag packed in one size_t */
#if SIZE_MAX > 0xFFFFFFFFu
#  define CB_IDX_BITS 32
#else
#  define CB_IDX_BITS 16
#endif
#define CB_IDX_MASK   (((size_t)1 << CB_IDX_BITS) - 1)
#define CB_IDX_NONE   CB_IDX_MASK
#define CB_PACK(i, t) ((size_t)(i) | ((size_t)(t) << CB_IDX_BITS))
#define CB_IDX(h)     ((h) & CB_IDX_MASK)
#define CB_TAG(h)     ((h) >> CB_IDX_BITS)

typedef CB_ALIGN_PRE struct cbWorker {
    epicsEventId wake;
    size_t state;               /* atomic CB_STATE; AWAKE -> SLEEPING and
                                 * * -> AWAKE by the worker, SLEEPING ->
                                 * CLAIMED by a waker */
    EpicsAtomicPtrT handoff;    /* cbNode list handed over by a waker, or
                                 * NULL; stealable */
    EpicsAtomicPtrT pending;    /* our pending list beyond the batch being
                                 * run; stealable */
    int progress;               /* atomic: batches started */
    epicsThreadId tid;
    unsigned idx;
    cbNode **seenSlot;          /* what this worker saw in others' slots at
                                 * its last look */
    int *seenProgress;
    epicsUInt64 lastLook;       /* when it last looked */
} CB_ALIGN_POST cbWorker;

typedef struct cbQueueSet {
    EpicsAtomicPtrT inbox;  /* cbNode*, newest first */
    char pad0[64 - sizeof(EpicsAtomicPtrT)];
    size_t freeHead;        /* atomic, CB_PACK(index, tag) */
    char pad1[64 - sizeof(size_t)];
    int nQueued;            /* atomic: nodes taken but not yet run */
    int maxQueued;          /* atomic, racy high-water mark */
    int queueOverflows;
    int batches;            /* atomic: batches started by any worker */
    int lastBatches;        /* atomic: batches as last seen by a requester
                             * behind a backlog */
    size_t staleSince;      /* atomic: when (us) a requester first saw that
                             * value behind a backlog, 0 if none */
    char pad2[64 - 5 * sizeof(int) - sizeof(size_t)];
    size_t sleepers;        /* atomic bitmask hint: SLEEPING workers nobody
                             * has taken yet; set by the worker, cleared by
                             * its taker or the worker */
    int nAwake;             /* atomic: workers not SLEEPING (incl. CLAIMED);
                             * lags the claim CAS, so it can read below 0 */
    char pad3[64 - sizeof(size_t) - sizeof(int)];
    cbNode *pool;
    int shutdown; // use atomic
    int threadsConfigured;
    int threadsRunning;
    cbWorker *workers;
    void *workersRaw;       /* allocation behind the aligned workers array */
} cbQueueSet;

static cbQueueSet callbackQueue[NUM_CALLBACK_PRIORITIES];

int callbackThreadsDefault = 1;
/* Don't know what a reasonable default is (yet).
 * For the time being: parallel means 2 if not explicitly specified */
int callbackParallelThreadsDefault = 2;
epicsExportAddress(int,callbackParallelThreadsDefault);

/* Timer for Delayed Requests */
static epicsTimerQueueId timerQueue;

enum cbState_t {
    cbInit,  /* before callbackInit() and after callbackCleanup() */
    cbRun,   /* after callbackInit() and before callbackStop() */
    cbStop,  /* after callbackStop() and before callbackCleanup() */
};

static int cbState; // holdscbState_t, use atomic ops

static epicsEventId startStopEvent;

/* Static data */
static const char *threadNamePrefix[NUM_CALLBACK_PRIORITIES] = {
    "cbLow", "cbMedium", "cbHigh"
};
#define FULL_MSG(name) "callbackRequest: " ERL_ERROR " " name " ring buffer full\n"
static const char *fullMessage[NUM_CALLBACK_PRIORITIES] = {
    FULL_MSG("cbLow"), FULL_MSG("cbMedium"), FULL_MSG("cbHigh")
};
static const unsigned int threadPriority[NUM_CALLBACK_PRIORITIES] = {
    epicsThreadPriorityScanLow - 1,
    epicsThreadPriorityScanLow + 4,
    epicsThreadPriorityScanHigh + 1
};


int callbackSetQueueSize(int size)
{
    if (size<=0) {
        fprintf(stderr, "Queue size must be positive\n");
        return -1;
    }
    if (epicsAtomicGetIntT(&cbState)!=cbInit) {
        fprintf(stderr, "Callback system already initialized\n");
        return -1;
    }
    callbackQueueSize = size;
    return 0;
}

int callbackQueueStatus(const int reset, callbackQueueStats *result)
{
    int ret;
    if (epicsAtomicGetIntT(&cbState)==cbInit) return -1;
    if (result) {
        int prio;
        result->size = callbackQueueSize;
        for(prio = 0; prio < NUM_CALLBACK_PRIORITIES; prio++) {
            cbQueueSet *mySet = &callbackQueue[prio];
            result->numUsed[prio] = epicsAtomicGetIntT(&mySet->nQueued);
            result->maxUsed[prio] = epicsAtomicGetIntT(&mySet->maxQueued);
            result->numOverflow[prio] = epicsAtomicGetIntT(&mySet->queueOverflows);
        }
        ret = 0;
    } else {
        ret = -2;
    }
    if (reset) {
        int prio;
        for(prio = 0; prio < NUM_CALLBACK_PRIORITIES; prio++) {
            epicsAtomicSetIntT(&callbackQueue[prio].maxQueued, 0);
        }
    }
    return ret;
}

void callbackQueueShow(const int reset)
{
    callbackQueueStats stats;
    if (callbackQueueStatus(reset, &stats) == -1) {
        fprintf(stderr, "Callback system not initialized, yet. Please run "
            "iocInit before using this command.\n");
    } else {
        int prio;
        printf("PRIORITY  HIGH-WATER MARK  ITEMS IN Q  Q SIZE  %% USED  Q OVERFLOWS\n");
        for (prio = 0; prio < NUM_CALLBACK_PRIORITIES; prio++) {
            double qusage = 100.0 * stats.numUsed[prio] / stats.size;
            printf("%8s  %15d  %10d  %6d  %6.1f  %11d\n",
                   threadNamePrefix[prio], stats.maxUsed[prio],
                   stats.numUsed[prio], stats.size, qusage,
                   stats.numOverflow[prio]);
        }
    }
}

int callbackParallelThreads(int count, const char *prio)
{
    if (epicsAtomicGetIntT(&cbState)!=cbInit) {
        fprintf(stderr, "Callback system already initialized\n");
        return -1;
    }

    if (count < 0)
        count = epicsThreadGetCPUs() + count;
    else if (count == 0)
        count = callbackParallelThreadsDefault;
    if (count < 1) count = 1;

    if (!prio || *prio == 0 || strcmp(prio, "*") == 0) {
        int i;

        for (i = 0; i < NUM_CALLBACK_PRIORITIES; i++) {
            callbackQueue[i].threadsConfigured = count;
        }
    }
    else {
        dbMenu *pdbMenu;
        int i;

        if (!pdbbase) {
            fprintf(stderr, "callbackParallelThreads: pdbbase not set\n");
            return -1;
        }

        /* Find prio in menuPriority */
        pdbMenu = dbFindMenu(pdbbase, "menuPriority");
        if (!pdbMenu) {
            fprintf(stderr, "callbackParallelThreads: No Priority menu\n");
            return -1;
        }

        for (i = 0; i < pdbMenu->nChoice; i++) {
            if (epicsStrCaseCmp(prio, pdbMenu->papChoiceValue[i]) == 0)
                goto found;
        }
        fprintf(stderr, "callbackParallelThreads: "
            "Unknown priority \"%s\"\n", prio);
        return -1;

found:
        callbackQueue[i].threadsConfigured = count;
    }
    return 0;
}

/* node pool: tagged lock-free LIFO; pop is per node, push is a chain */
static cbNode *nodeAlloc(cbQueueSet *mySet)
{
    for (;;) {
        size_t h = epicsAtomicGetSizeT(&mySet->freeHead);
        size_t i = CB_IDX(h);
        cbNode *n;
        size_t nx;
        if (i == CB_IDX_NONE) return NULL;
        n = &mySet->pool[i];
        nx = n->next ? (size_t)(n->next - mySet->pool) : CB_IDX_NONE;
        if (epicsAtomicCmpAndSwapSizeT(&mySet->freeHead, h, CB_PACK(nx, CB_TAG(h) + 1)) == h)
            return n;
    }
}

static void nodeFreeChain(cbQueueSet *mySet, cbNode *first, cbNode *last)
{
    for (;;) {
        size_t h = epicsAtomicGetSizeT(&mySet->freeHead);
        size_t i = CB_IDX(h);
        last->next = (i == CB_IDX_NONE) ? NULL : &mySet->pool[i];
        if (epicsAtomicCmpAndSwapSizeT(&mySet->freeHead, h,
                CB_PACK(first - mySet->pool, CB_TAG(h) + 1)) == h)
            return;
    }
}

/* take everything queued so far, oldest first; *pn = how many */
static cbNode *grabInbox(cbQueueSet *mySet, int *pn)
{
    cbNode *head, *rev = NULL;
    int n = 0;

    do {
        head = epicsAtomicGetPtrT(&mySet->inbox);
        if (!head) { *pn = 0; return NULL; }
    } while (epicsAtomicCmpAndSwapPtrT(&mySet->inbox, head, NULL) != head);
    while (head) {
        cbNode *nx = head->next;
        head->next = rev;
        rev = head;
        head = nx;
        n++;
    }
    *pn = n;
    return rev;
}

/* trigger a sleeping worker, then claim it (SLEEPING -> CLAIMED of the
 * same epoch) so it is counted awake. A failed claim means the worker
 * left that sleep by itself and counted itself; the trigger then at
 * worst wakes its next sleep once for nothing. */
/* clear worker i's hint bit; returns whether this call cleared it */
static int takeSleeperBit(cbQueueSet *mySet, unsigned i)
{
    size_t bit = (size_t)1 << i;
    for (;;) {
        size_t m = epicsAtomicGetSizeT(&mySet->sleepers);
        if (!(m & bit)) return 0;
        if (epicsAtomicCmpAndSwapSizeT(&mySet->sleepers, m, m & ~bit) == m) return 1;
    }
}

/* push list (n nodes, newest first) onto a worker's slot */
static void pushSlot(EpicsAtomicPtrT *slot, cbNode *list, int n)
{
    cbNode *tail = list, *old;
    while (--n > 0) tail = tail->next;
    do {
        old = epicsAtomicGetPtrT(slot);
        tail->next = old;
    } while (epicsAtomicCmpAndSwapPtrT(slot, old, list) != old);
}

/* returns whether the claim succeeded */
static int triggerAndClaim(cbQueueSet *mySet, cbWorker *w, size_t s)
{
    epicsEventMustTrigger(w->wake);
    if (epicsAtomicCmpAndSwapSizeT(&w->state, s, CB_STATE(CB_EPOCH(s), CB_CLAIMED)) != s)
        return 0;
    epicsAtomicIncrIntT(&mySet->nAwake);
    return 1;
}

/* hand list (n nodes) to a sleeping worker and wake it. The list is
 * stored in the worker's slot before the trigger: the worker takes the
 * slot at the top of its loop and before every sleep, so once stored
 * it is run whether the worker is still asleep, waking by itself, or
 * about to sleep again. Returns 0 if no worker sleeps with an empty
 * slot; the caller then keeps its list. */
static int wakeOne(cbQueueSet *mySet, cbNode *list, int n)
{
    size_t m = epicsAtomicGetSizeT(&mySet->sleepers);

    while (m) {
        unsigned i = CB_LOWBIT(m);
        cbWorker *w = &mySet->workers[i];
        size_t s = epicsAtomicGetSizeT(&w->state);
        m &= ~((size_t)1 << i);
        if (CB_ST(s) != CB_SLEEPING || !takeSleeperBit(mySet, i)) continue;
        pushSlot(&w->handoff, list, n);
        triggerAndClaim(mySet, w, s);
        return 1;
    }
    return 0;
}

/* wake one sleeping worker with nothing to hand over; retry when the
 * claim fails, since the request may then still be unseen. Tries the
 * hinted workers first, then every worker once if nAwake says one
 * sleeps: a sharer stopped after taking a bit leaves none. A worker
 * found in neither pass is leaving its sleep and reads the inbox. */
static void pokeSleeper(cbQueueSet *mySet)
{
    size_t m = epicsAtomicGetSizeT(&mySet->sleepers);
    int i;

    while (m) {
        i = CB_LOWBIT(m);
        m &= ~((size_t)1 << i);
        if (!takeSleeperBit(mySet, i)) continue;
        {
            cbWorker *w = &mySet->workers[i];
            size_t s = epicsAtomicGetSizeT(&w->state);
            if (CB_ST(s) == CB_SLEEPING && triggerAndClaim(mySet, w, s))
                return;
        }
        /* that worker moved on by itself; scan again */
        m = epicsAtomicGetSizeT(&mySet->sleepers);
    }
    if (epicsAtomicGetIntT(&mySet->nAwake) < mySet->threadsConfigured) {
        for (i = 0; i < mySet->threadsConfigured; i++) {
            cbWorker *w = &mySet->workers[i];
            size_t s = epicsAtomicGetSizeT(&w->state);
            if (CB_ST(s) == CB_SLEEPING && triggerAndClaim(mySet, w, s))
                return;
        }
    }
}

/* hand ceil(p/2) of the p pending callbacks to a sleeping worker, if
 * any. Pending work is the n nodes of *plist (newest last) plus the
 * batch entries batch[*pk - 1] down to batch[i] whose nodes were
 * already returned to the pool; those get fresh nodes. Returns the
 * number of nodes left in *plist. */
static int shareWithSleeper(cbQueueSet *mySet, cbWorker *me, cbNode **plist,
                            int n, epicsCallback **batch, int *pk, int i)
{
    int p = n + (*pk - i), give, fromList, fromBatch, keep, j;
    cbNode *head = NULL, *tail = NULL, *cut = NULL;

    if (p < 1 || epicsAtomicGetSizeT(&mySet->sleepers) == 0)
        return n;
    /* reclaim the published list; NULL means an idle worker stole it */
    if (n && epicsAtomicCmpAndSwapPtrT(&me->pending, *plist, NULL) != *plist) {
        *plist = NULL;
        n = 0;
        p = *pk - i;
        if (p < 1) return 0;
    }
    give = (p + 1) / 2;
    fromList = give < n ? give : n;
    fromBatch = give - fromList;
    /* nodes for the batch part (older than the list part) */
    for (j = 0; j < fromBatch; j++) {
        cbNode *nd = nodeAlloc(mySet);
        if (!nd) break;
        nd->cb = batch[*pk - fromBatch + j];
        nd->next = NULL;
        if (tail) tail->next = nd; else head = nd;
        tail = nd;
    }
    /* pool exhausted: hand only what has nodes; the newest keep
     * entries stay in batch[], untouched until the handover succeeds */
    keep = fromBatch - j;
    fromBatch = j;
    if (fromList) {
        cut = *plist;
        for (j = 1; j < n - fromList; j++)
            cut = cut->next;
        if (fromList == n) {
            if (tail) tail->next = *plist; else head = *plist;
            *plist = NULL;
        }
        else {
            if (tail) tail->next = cut->next; else head = cut->next;
            cut->next = NULL;
        }
    }
    if (!head) {
        if (*plist) epicsAtomicSetPtrT(&me->pending, *plist);
        return n;
    }
    if (fromBatch)
        epicsAtomicAddIntT(&mySet->nQueued, fromBatch);
    if (!wakeOne(mySet, head, fromBatch + fromList)) {
        /* sleeper vanished: undo. Batch part goes back to the pool, its
         * entries are still in batch[]; list part is re-attached. */
        if (fromBatch) {
            cbNode *bl = head, *bt = head;
            for (j = 1; j < fromBatch; j++) bt = bt->next;
            if (fromList) {
                if (fromList == n) *plist = bt->next;
                else cut->next = bt->next;
            }
            bt->next = NULL;
            nodeFreeChain(mySet, bl, bt);
            epicsAtomicAddIntT(&mySet->nQueued, -fromBatch);
        } else {
            if (fromList == n) *plist = head; else cut->next = head;
        }
        if (*plist) epicsAtomicSetPtrT(&me->pending, *plist);
        return n;
    }
    /* drop the handed entries from batch[], sliding the kept ones down */
    for (j = 0; j < keep; j++)
        batch[*pk - fromBatch - keep + j] = batch[*pk - keep + j];
    *pk -= fromBatch;
    if (*plist) epicsAtomicSetPtrT(&me->pending, *plist);
    return n - fromList;
}

/* take the whole list in another worker's slot, counting it */
static cbNode *takeSlot(EpicsAtomicPtrT *slot, int *pn)
{
    cbNode *l, *q;
    int n = 0;

    do {
        l = epicsAtomicGetPtrT(slot);
        if (!l) { *pn = 0; return NULL; }
    } while (epicsAtomicCmpAndSwapPtrT(slot, l, NULL) != l);
    for (q = l; q; q = q->next) n++;
    *pn = n;
    return l;
}

/* an idle worker takes the pending or handed-over list of another
 * worker that has not progressed since the list was last seen there */
static cbNode *stealWork(cbQueueSet *mySet, cbWorker *me, int *pn)
{
    int j;

    epicsUInt64 now = epicsMonotonicGet();

    if (now - me->lastLook < CB_STEAL_AGE_NS) {
        *pn = 0;
        return NULL;
    }
    me->lastLook = now;
    for (j = 0; j < mySet->threadsConfigured; j++) {
        cbWorker *w = &mySet->workers[j];
        cbNode *pend, *hand, *l;
        int prog;
        if (w == me) continue;
        pend = epicsAtomicGetPtrT(&w->pending);
        hand = epicsAtomicGetPtrT(&w->handoff);
        prog = epicsAtomicGetIntT(&w->progress);
        if ((pend || hand) && prog == me->seenProgress[j] &&
            pend == me->seenSlot[2 * j] && hand == me->seenSlot[2 * j + 1]) {
            if (pend) {
                l = takeSlot(&w->pending, pn);
                if (l) return l;
            }
            if (hand) {
                l = takeSlot(&w->handoff, pn);
                if (l) return l;
            }
        }
        me->seenSlot[2 * j] = pend;
        me->seenSlot[2 * j + 1] = hand;
        me->seenProgress[j] = prog;
    }
    *pn = 0;
    return NULL;
}

/* run a list of n nodes, returning nodes to the pool a batch at a time
 * before the batch runs, so a callback may re-request itself without
 * needing its own node back; before each callback, pending work is
 * shared with a sleeping worker */
static void runList(cbQueueSet *mySet, cbWorker *me, cbNode *list, int n)
{
    epicsCallback *batch[CB_SHARE_EVERY];
    cbNode *pub = NULL;     /* what we have published in me->pending */
    int k = 0, i = 0;

    for (;;) {
        if (i == k) {
            cbNode *first, *last;
            /* reclaim the published list; NULL means an idle worker stole it */
            if (pub) {
                if (epicsAtomicCmpAndSwapPtrT(&me->pending, pub, NULL) != pub) {
                    list = NULL;
                    n = 0;
                }
                pub = NULL;
            }
            if (!list) return;
            first = list;
            k = 0;
            for (;;) {
                batch[k++] = list->cb;
                last = list;
                list = list->next;
                if (!list || k == CB_SHARE_EVERY)
                    break;
            }
            nodeFreeChain(mySet, first, last);
            n -= k;
            epicsAtomicAddIntT(&mySet->nQueued, -k);
            epicsAtomicIncrIntT(&me->progress);
            epicsAtomicIncrIntT(&mySet->batches);
            i = 0;
            /* the rest is stealable while this batch runs */
            if (list) {
                epicsAtomicSetPtrT(&me->pending, list);
                pub = list;
            }
        }
        /* keep halving the pending work into sleepers while any sleep */
        for (;;) {
            int before = n + k;
            n = shareWithSleeper(mySet, me, &list, n, batch, &k, i + 1);
            if (n + k == before || n + (k - i - 1) < 1)
                break;
        }
        pub = list;
        (*batch[i]->callback)(batch[i]);
        i++;
    }
}

static void callbackTask(void *arg)
{
    cbWorker *me = arg;
    cbQueueSet *mySet = &callbackQueue[me->idx >> 8];
    size_t mybit = (size_t)1 << (me->idx & 0xff);
    size_t epoch = 0;

    taskwdInsert(0, NULL, NULL);
    epicsEventSignal(startStopEvent);

    while(!epicsAtomicGetIntT(&mySet->shutdown)) {
        cbNode *list;
        int n;

        /* a list handed to us comes first: a poke may have woken us
         * before our claimer stored it, so it can arrive while we run */
        list = takeSlot(&me->handoff, &n);
        if (list) {
            runList(mySet, me, list, n);
            continue;
        }
        list = grabInbox(mySet, &n);
        if (list) {
            runList(mySet, me, list, n);
            continue;
        }
        list = stealWork(mySet, me, &n);
        if (list) {
            runList(mySet, me, list, n);
            continue;
        }

        /* announce sleep with a fresh epoch (state, hint bit, count),
         * then re-check the inbox and our slot: a request pushed or a
         * list stored before this point is seen here; one after it sees
         * nAwake==0 or our bit, and our SLEEPING state behind it.
         * Leaving the sleep by ourselves counts us awake; if a waker
         * claimed us first, it counted us and its trigger stays stored
         * in the event. */
        {
            size_t sleeping = CB_STATE(++epoch, CB_SLEEPING);
            epicsAtomicSetSizeT(&me->state, sleeping);
            for (;;) {
                size_t m = epicsAtomicGetSizeT(&mySet->sleepers);
                if (epicsAtomicCmpAndSwapSizeT(&mySet->sleepers, m, m | mybit) == m) break;
            }
            epicsAtomicDecrIntT(&mySet->nAwake);
            if (epicsAtomicGetPtrT(&mySet->inbox) == NULL &&
                epicsAtomicGetPtrT(&me->handoff) == NULL)
                epicsEventMustWait(me->wake);
            if (epicsAtomicCmpAndSwapSizeT(&me->state, sleeping,
                    CB_STATE(epoch, CB_AWAKE)) == sleeping)
                epicsAtomicIncrIntT(&mySet->nAwake);
            else
                epicsAtomicSetSizeT(&me->state, CB_STATE(epoch, CB_AWAKE));
            takeSleeperBit(mySet, me->idx & 0xff);
        }
    }

    if(!epicsAtomicDecrIntT(&mySet->threadsRunning))
        epicsEventSignal(startStopEvent);
    taskwdRemove(0);
}

void callbackStop(void)
{
    int i;

    if (epicsAtomicCmpAndSwapIntT(&cbState, cbRun, cbStop)!=cbRun) return;

    for (i = 0; i < NUM_CALLBACK_PRIORITIES; i++) {
        cbQueueSet *mySet = &callbackQueue[i];
        int j;

        epicsAtomicSetIntT(&mySet->shutdown, 1);
        while (epicsAtomicGetIntT(&mySet->threadsRunning)) {
            for(j=0; j<mySet->threadsConfigured; j++)
                epicsEventSignal(mySet->workers[j].wake);
            epicsEventWaitWithTimeout(startStopEvent, 0.1);
        }
        for(j=0; j<mySet->threadsConfigured; j++) {
            epicsThreadMustJoin(mySet->workers[j].tid);
        }
    }
}

void callbackCleanup(void)
{
    int i;

    if(epicsAtomicCmpAndSwapIntT(&cbState, cbStop, cbInit)!=cbStop) {
        fprintf(stderr, "callbackCleanup() but not stopped\n");
    }

    for (i = 0; i < NUM_CALLBACK_PRIORITIES; i++) {
        cbQueueSet *mySet = &callbackQueue[i];

        int j;

        assert(epicsAtomicGetIntT(&mySet->threadsRunning)==0);
        for(j=0; j<mySet->threadsConfigured; j++) {
            epicsEventDestroy(mySet->workers[j].wake);
            free(mySet->workers[j].seenSlot);
            free(mySet->workers[j].seenProgress);
        }
        free(mySet->workersRaw);
        mySet->workers = NULL;
        mySet->workersRaw = NULL;
        free(mySet->pool);
        mySet->pool = NULL;
    }

    epicsTimerQueueRelease(timerQueue);
    memset(callbackQueue, 0, sizeof(callbackQueue));
}

void callbackInit(void)
{
    int i;
    int j;
    char threadName[32];

    if (epicsAtomicCmpAndSwapIntT(&cbState, cbInit, cbRun)!=cbInit) {
        fprintf(stderr, "Warning: callbackInit called again before callbackCleanup\n");
        return;
    }

    if(!startStopEvent)
        startStopEvent = epicsEventMustCreate(epicsEventEmpty);

    timerQueue = epicsTimerQueueAllocate(0, epicsThreadPriorityScanHigh);

    for (i = 0; i < NUM_CALLBACK_PRIORITIES; i++) {
        epicsThreadId tid;

        {
            cbQueueSet *q = &callbackQueue[i];
            int k;
            q->pool = callocMustSucceed(callbackQueueSize, sizeof(*q->pool), "callbackInit");
            for (k = 0; k < callbackQueueSize - 1; k++)
                q->pool[k].next = &q->pool[k + 1];
            q->pool[callbackQueueSize - 1].next = NULL;
            q->freeHead = CB_PACK(0, 0);
        }
        callbackQueue[i].inbox = NULL;
        callbackQueue[i].nQueued = 0;
        callbackQueue[i].maxQueued = 0;
        callbackQueue[i].sleepers = 0;

        if (callbackQueue[i].threadsConfigured == 0)
            callbackQueue[i].threadsConfigured = callbackThreadsDefault;
        if (callbackQueue[i].threadsConfigured > (int)CB_MAX_WORKERS)
            callbackQueue[i].threadsConfigured = CB_MAX_WORKERS;

        callbackQueue[i].workersRaw = callocMustSucceed(1,
            callbackQueue[i].threadsConfigured * sizeof(*callbackQueue[i].workers) + CB_WORKER_ALIGN,
            "callbackInit");
        callbackQueue[i].workers = (cbWorker *)(((uintptr_t)callbackQueue[i].workersRaw +
                                                 CB_WORKER_ALIGN - 1) & ~(uintptr_t)(CB_WORKER_ALIGN - 1));
        /* workers start awake; each goes to sleep once it finds the inbox empty */
        callbackQueue[i].nAwake = callbackQueue[i].threadsConfigured;

        for (j = 0; j < callbackQueue[i].threadsConfigured; j++) {
            cbWorker *w = &callbackQueue[i].workers[j];
            epicsThreadOpts opts = EPICS_THREAD_OPTS_INIT;
            opts.joinable = 1;
            opts.priority = threadPriority[i];
            opts.stackSize = epicsThreadStackBig;
            if (callbackQueue[i].threadsConfigured > 1 )
                sprintf(threadName, "%s-%d", threadNamePrefix[i], j);
            else
                strcpy(threadName, threadNamePrefix[i]);
            w->idx = (i << 8) | j;
            w->seenSlot = callocMustSucceed(2 * callbackQueue[i].threadsConfigured,
                                            sizeof(*w->seenSlot), "callbackInit");
            w->seenProgress = callocMustSucceed(callbackQueue[i].threadsConfigured,
                                                sizeof(*w->seenProgress), "callbackInit");
            w->wake = epicsEventMustCreate(epicsEventEmpty);
            w->tid = tid = epicsThreadCreateOpt(threadName,
                (EPICSTHREADFUNC)callbackTask, w, &opts);
            if (tid == 0) {
                cantProceed("Failed to spawn callback thread %s\n", threadName);
            } else {
                epicsEventWait(startStopEvent);
                epicsAtomicIncrIntT(&callbackQueue[i].threadsRunning);
            }
        }
    }
}

/* This routine can be called from interrupt context */
int callbackRequest(epicsCallback *pcallback)
{
    int priority;
    int n;
    cbNode *node;
    cbQueueSet *mySet;

    if (!pcallback) {
        epicsInterruptContextMessage("callbackRequest: " ERL_ERROR " pcallback was NULL\n");
        return S_db_notInit;
    }
    if (!pcallback->callback) {
        epicsInterruptContextMessage("callbackRequest: " ERL_ERROR " pcallback->callback was NULL\n");
        return S_db_notInit;
    }
    priority = pcallback->priority;
    if (priority < 0 || priority >= NUM_CALLBACK_PRIORITIES) {
        epicsInterruptContextMessage("callbackRequest: " ERL_ERROR " Bad priority\n");
        return S_db_badChoice;
    }
    mySet = &callbackQueue[priority];
    if (!mySet->workers) {
        epicsInterruptContextMessage("callbackRequest: " ERL_ERROR " Callbacks not initialized\n");
        return S_db_notInit;
    }

    node = nodeAlloc(mySet);
    if (!node) {
        epicsInterruptContextMessage(fullMessage[priority]);
        epicsAtomicIncrIntT(&mySet->queueOverflows);
        return S_db_bufFull;
    }
    node->cb = pcallback;
    n = epicsAtomicIncrIntT(&mySet->nQueued);
    if (n > epicsAtomicGetIntT(&mySet->maxQueued))
        epicsAtomicSetIntT(&mySet->maxQueued, n);

    {
        cbNode *old;
        int awake;
        do {
            old = epicsAtomicGetPtrT(&mySet->inbox);
            node->next = old;
        } while (epicsAtomicCmpAndSwapPtrT(&mySet->inbox, old, node) != old);

        /* a worker that is awake will take this; recruiting more is its
         * job. But if only one of several workers is counted awake and
         * for CB_STALE_US no batch has been started, that worker is
         * stopped, so wake a sleeper now. nAwake lags its claim CAS: a
         * claimer stopped between the claim and its increment lets the
         * claimed worker run and sleep first, so the count can read
         * below zero; anything non-positive means nobody is counted. */
        awake = epicsAtomicGetIntT(&mySet->nAwake);
        if (awake <= 0)
            pokeSleeper(mySet);
        else if (awake == 1 && mySet->threadsConfigured > 1) {
            int b = epicsAtomicGetIntT(&mySet->batches);
            if (b != epicsAtomicGetIntT(&mySet->lastBatches)) {
                epicsAtomicSetIntT(&mySet->lastBatches, b);
                epicsAtomicSetSizeT(&mySet->staleSince, 0);
            }
            else {
                size_t now = (size_t)(epicsMonotonicGet() >> 10) | 1;
                size_t since = epicsAtomicGetSizeT(&mySet->staleSince);
                if (since == 0)
                    epicsAtomicSetSizeT(&mySet->staleSince, now);
                else if (now - since >= CB_STALE_US) {
                    epicsAtomicSetSizeT(&mySet->staleSince, 0);
                    pokeSleeper(mySet);
                }
            }
        }
    }
    return 0;
}

static void ProcessCallback(epicsCallback *pcallback)
{
    dbCommon *pRec;

    callbackGetUser(pRec, pcallback);
    if (!pRec) return;
    dbScanLock(pRec);
    (*pRec->rset->process)(pRec);
    dbScanUnlock(pRec);
}

void callbackSetProcess(epicsCallback *pcallback, int Priority, void *pRec)
{
    callbackSetCallback(ProcessCallback, pcallback);
    callbackSetPriority(Priority, pcallback);
    callbackSetUser(pRec, pcallback);
}

int  callbackRequestProcessCallback(epicsCallback *pcallback,
    int Priority, void *pRec)
{
    callbackSetProcess(pcallback, Priority, pRec);
    return callbackRequest(pcallback);
}

static void notify(void *pPrivate)
{
    epicsCallback *pcallback = (epicsCallback *)pPrivate;
    callbackRequest(pcallback);
}

void callbackRequestDelayed(epicsCallback *pcallback, double seconds)
{
    epicsTimerId timer = (epicsTimerId)pcallback->timer;

    if (timer == 0) {
        timer = epicsTimerQueueCreateTimer(timerQueue, notify, pcallback);
        pcallback->timer = timer;
    }
    epicsTimerStartDelay(timer, seconds);
}

void callbackCancelDelayed(epicsCallback *pcallback)
{
    epicsTimerId timer = (epicsTimerId)pcallback->timer;

    if (timer != 0) {
        epicsTimerCancel(timer);
    }
}

void callbackRequestProcessCallbackDelayed(epicsCallback *pcallback,
    int Priority, void *pRec, double seconds)
{
    callbackSetProcess(pcallback, Priority, pRec);
    callbackRequestDelayed(pcallback, seconds);
}

/* Sync. process of testSyncCallback()
 *
 * 1. For each priority, make a call to callbackRequest() for each worker.
 * 2. Wait until all callbacks are concurrently being executed
 * 3. Last worker to begin executing signals success and begins waking up other workers
 * 4. Last worker to wake signals testSyncCallback() to complete
 */
typedef struct {
    epicsEventId wait_phase2, wait_phase4;
    int nphase2, nphase3;
    epicsCallback cb;
} sync_helper;

static void sync_callback(epicsCallback *cb)
{
    sync_helper *helper;
    callbackGetUser(helper, cb);

    testGlobalLock();

    assert(helper->nphase2 > 0);
    if(--helper->nphase2!=0) {
        /* we are _not_ the last to start. */
        testGlobalUnlock();
        epicsEventMustWait(helper->wait_phase2);
        testGlobalLock();
    }

    /* we are either the last to start, or have been
     * woken by the same and must pass the wakeup along
     */
    epicsEventMustTrigger(helper->wait_phase2);

    assert(helper->nphase2 == 0);
    assert(helper->nphase3 > 0);

    if(--helper->nphase3==0) {
        /* we are the last to wake up.  wake up testSyncCallback() */
        epicsEventMustTrigger(helper->wait_phase4);
    }

    testGlobalUnlock();
}

void testSyncCallback(void)
{
    sync_helper helper[NUM_CALLBACK_PRIORITIES];
    unsigned i;

    testDiag("Begin testSyncCallback()");

    for(i=0; i<NUM_CALLBACK_PRIORITIES; i++) {
        helper[i].wait_phase2 = epicsEventMustCreate(epicsEventEmpty);
        helper[i].wait_phase4 = epicsEventMustCreate(epicsEventEmpty);

        /* no real need to lock here, but do so anyway so that valgrind can establish
         * the locking requirements for sync_helper.
         */
        testGlobalLock();
        helper[i].nphase2 = helper[i].nphase3 = callbackQueue[i].threadsRunning;
        testGlobalUnlock();

        callbackSetUser(&helper[i], &helper[i].cb);
        callbackSetPriority(i, &helper[i].cb);
        callbackSetCallback(sync_callback, &helper[i].cb);

        callbackRequest(&helper[i].cb);
    }

    for(i=0; i<NUM_CALLBACK_PRIORITIES; i++) {
        epicsEventMustWait(helper[i].wait_phase4);
    }

    for(i=0; i<NUM_CALLBACK_PRIORITIES; i++) {
        testGlobalLock();
        epicsEventDestroy(helper[i].wait_phase2);
        epicsEventDestroy(helper[i].wait_phase4);
        testGlobalUnlock();
    }

    testDiag("Complete testSyncCallback()");
}
