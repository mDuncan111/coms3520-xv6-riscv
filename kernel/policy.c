// Project 1C: scheduling policies.
//
// This is the file you modify. The scheduler framework in proc.c
// calls the functions below; it already handles locking and the
// context switch, so a policy only decides which process runs next
// and when the running process should be preempted.
//
// Each policy has an ID. Select one at run time with the
// set_scheduler(id) system call (for example: wl 2 1). To try a
// different setting, such as another quantum, give it its own ID.
// ID 3 is a nice-initialized multi-level feedback queue scheduler.
//
// Rules for policy code (Section 2.2 of the project document):
//  1. The system has one CPU. These functions run with interrupts off
//     and must not sleep or acquire p->lock (the framework holds the
//     locks it needs).
//  2. policy_pick_next_proc() must only return a RUNNABLE process, or
//     0 if there is none.
//  3. A scheduler meant to be fair must not starve a job: every ready
//     job eventually runs. (Strict priority, ID 2, is the one
//     deliberate exception.)
//  4. A scheduler must not recognize a workload: do not read
//     p->wl_slot, workload IDs, process names, or pids to decide how
//     to treat a process.
//  5. policy_init(id) may be called more than once for the same ID,
//     including at the start of every workload run. Each call must
//     reset all of the scheduler's global state.
//  6. Do not modify the protected files (kernel/workload.c,
//     kernel/workload.h, user/wl.c).

#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "spinlock.h"
#include "proc.h"
#include "defs.h"

#define SCHED_RR     0 // round robin, 10-tick (100 ms) quantum: stock xv6
#define SCHED_RR1    1 // round robin, 1-tick (10 ms) quantum
#define SCHED_PRIO   2 // strict priority by nice (Project 1B), 10-tick quantum
#define SCHED_MLFQ  3 // multi-level feedback queue
#define SCHED_STRIDE 4 // proportional share by nice weight
#define SCHED_MLFQ_WEIGHTED 5 // MLFQ with nice-weighted selection within each level
#define SCHED_RR5    6 // round robin, 5-tick quantum
#define SCHED_MLFQ_Q1_5 10 // MLFQ with a 5-tick Q1 quantum
#define SCHED_MLFQ_Q1_ALLOT5 11 // MLFQ with a 5-tick Q1 allotment
#define SCHED_MLFQ_BOOST30 12 // MLFQ with a 30-tick boost interval

#define STRIDE_BASE (1ULL << 24)
#define STRIDE_REBASE_AT (1ULL << 60)

// Linux CFS weights for nice -20 .. 19, indexed by nice + 20
// (nice 0 has weight 1024). This is the same table user/wl.c uses
// for the fairness metric. For example, a fair-share scheduler can
// advance a process's vrt by (1 << 24) / nice_weight[p->nice + 20]
// for each tick it runs.
const int nice_weight[40] = {
  88761, 71755, 56483, 46273, 36291, 29154, 23254, 18705, 14949, 11916,
  9548,  7620,  6100,  4904,  3906,  3121,  2501,  1991,  1586,  1277,
  1024,  820,   655,   526,   423,   335,   272,   215,   172,   137,
  110,   87,    70,    56,    45,    36,    29,    23,    18,    15,
};

static int quantum = 10; // ticks a process may run before preemption
static int last = -1;    // index in proc[] of the last process chosen
static int stride_last = -1;
static int mlfq_last[3] = { -1, -1, -1 };
static uint mlfq_last_boost_tick;
static uint64 mlfq_weighted_floor[3];

static uint64
stride_min_pass(struct proc *exclude)
{
  uint64 min_pass = ~(uint64)0;

  for (struct proc *p = proc; p < &proc[NPROC]; p++) {
    if (p == exclude || (p->state != RUNNABLE && p->state != RUNNING))
      continue;
    if (p->vrt < min_pass)
      min_pass = p->vrt;
  }
  return min_pass;
}

static uint64
stride_for_nice(int nice)
{
  uint64 weight = (uint64)nice_weight[nice + 20];
  return (STRIDE_BASE + weight - 1) / weight;
}

static void
stride_rebase_if_needed(void)
{
  uint64 min_pass = stride_min_pass(0);

  if (min_pass == ~(uint64)0 || min_pass < STRIDE_REBASE_AT)
    return;

  for (struct proc *p = proc; p < &proc[NPROC]; p++) {
    if (p->state == UNUSED)
      continue;
    p->vrt = p->vrt >= min_pass ? p->vrt - min_pass : 0;
  }
}

static uint64
mlfq_weighted_min_pass(int level, struct proc *exclude)
{
  uint64 min_pass = ~(uint64)0;

  for (struct proc *p = proc; p < &proc[NPROC]; p++) {
    if (p == exclude || p->level != level ||
        (p->state != RUNNABLE && p->state != RUNNING))
      continue;
    if (p->mlfq_pass[level] < min_pass)
      min_pass = p->mlfq_pass[level];
  }
  return min_pass;
}

static void
mlfq_weighted_update_floor(int level, struct proc *exclude)
{
  uint64 min_pass = mlfq_weighted_min_pass(level, exclude);
  if (min_pass != ~(uint64)0 && min_pass > mlfq_weighted_floor[level])
    mlfq_weighted_floor[level] = min_pass;
}

static void
mlfq_weighted_update_floors(void)
{
  for (int level = 0; level < 3; level++)
    mlfq_weighted_update_floor(level, 0);
}

static void
mlfq_weighted_clamp_to_floor(struct proc *p, int level)
{
  if (p->mlfq_pass[level] < mlfq_weighted_floor[level])
    p->mlfq_pass[level] = mlfq_weighted_floor[level];
}

static void
mlfq_weighted_clamp(struct proc *p, int level)
{
  mlfq_weighted_update_floor(level, p);
  mlfq_weighted_clamp_to_floor(p, level);
}

static void
mlfq_weighted_rebase_if_needed(void)
{
  mlfq_weighted_update_floors();

  for (int level = 0; level < 3; level++) {
    uint64 floor = mlfq_weighted_floor[level];
    if (floor < STRIDE_REBASE_AT)
      continue;

    for (struct proc *p = proc; p < &proc[NPROC]; p++) {
      if (p->state == UNUSED)
        continue;
      p->mlfq_pass[level] = p->mlfq_pass[level] >= floor ?
                            p->mlfq_pass[level] - floor : 0;
    }
    mlfq_weighted_floor[level] = 0;
  }
}

static int
mlfq_start_level(int nice)
{
  if (nice <= -10)
    return 2;
  if (nice <= 10)
    return 1;
  return 0;
}

static int
mlfq_policy(int id)
{
  return id == SCHED_MLFQ || id == SCHED_MLFQ_WEIGHTED ||
         id == SCHED_MLFQ_Q1_5 ||
         id == SCHED_MLFQ_Q1_ALLOT5 || id == SCHED_MLFQ_BOOST30;
}

static int
mlfq_quantum(int level)
{
  if (level == 1 && (sched_current == SCHED_MLFQ_Q1_5 ||
                     sched_current == SCHED_MLFQ_WEIGHTED))
    return 5;
  if (level == 2)
    return 2;
  if (level == 1)
    return 10;
  return 15;
}

static int
mlfq_allotment(int level)
{
  if (level == 1 && (sched_current == SCHED_MLFQ_Q1_ALLOT5 ||
                     sched_current == SCHED_MLFQ_WEIGHTED))
    return 5;
  if (level == 2)
    return 2;
  if (level == 1)
    return 10;
  return 0;
}

static void
mlfq_log_level(struct proc *p, int old_level, int new_level, uint tick)
{
  if (logging)
    printk("level %d %d->%d at %d\n", p->pid, old_level, new_level, tick);
}

static void
mlfq_boost_if_due(void)
{
  uint interval = sched_current == SCHED_MLFQ_BOOST30 ||
                  sched_current == SCHED_MLFQ_WEIGHTED ? 30 : 60;

  while ((uint)(ticks - mlfq_last_boost_tick) >= interval) {
    mlfq_last_boost_tick += interval;
    uint boost_tick = mlfq_last_boost_tick;
    if (sched_current == SCHED_MLFQ_WEIGHTED)
      mlfq_weighted_update_floors();

    for (struct proc *p = proc; p < &proc[NPROC]; p++) {
      if (p->state == UNUSED)
        continue;
      int old_level = p->level;
      int new_level = p->start_level > 1 ? p->start_level : 1;
      p->level = new_level;
      if (sched_current == SCHED_MLFQ_WEIGHTED)
        mlfq_weighted_clamp_to_floor(p, new_level);
      p->used = 0;
      mlfq_log_level(p, old_level, new_level, boost_tick);
    }
    if (sched_current == SCHED_MLFQ_WEIGHTED)
      mlfq_weighted_update_floors();
  }
}

// Called by set_scheduler(id), and again when a workload starts.
// Set up global state for policy id.
// Return 0 if id is a known policy, -1 otherwise.
int
policy_init(int id)
{
  last = -1;
  stride_last = -1;
  for (int level = 0; level < 3; level++)
    mlfq_last[level] = -1;
  mlfq_last_boost_tick = ticks;
  for (int level = 0; level < 3; level++)
    mlfq_weighted_floor[level] = 0;

  switch (id) {
  case SCHED_RR:
  case SCHED_PRIO:
  case SCHED_MLFQ:
  case SCHED_MLFQ_WEIGHTED:
  case SCHED_MLFQ_Q1_5:
  case SCHED_MLFQ_Q1_ALLOT5:
  case SCHED_MLFQ_BOOST30:
    quantum = 10;
    break;
  case SCHED_STRIDE:
  case SCHED_RR1:
    quantum = 1;
    break;
  case SCHED_RR5:
    quantum = 5;
    break;
  default:
    return -1;
  }
  return 0;
}

// Called when a process is created, and for every process when the
// policy changes or a workload starts. Initialize any per-process
// policy fields.
void
policy_proc_init(struct proc *p)
{
  p->qticks = 0;
  // Level 0 is the lowest queue (Q0); Q2 is the highest. An MLFQ must
  // set the starting level from p->nice here: workload_start() calls
  // this for every process after the jobs' nice values are set.
  p->start_level = mlfq_start_level(p->nice);
  p->level = mlfq_policy(sched_current) ? p->start_level : 0;
  p->used = 0;
  for (int level = 0; level < 3; level++)
    p->mlfq_pass[level] = sched_current == SCHED_MLFQ_WEIGHTED ?
                          mlfq_weighted_floor[level] : 0;
  if (sched_current == SCHED_STRIDE && p->state == USED) {
    uint64 min_pass = stride_min_pass(p);
    p->vrt = min_pass == ~(uint64)0 ? 0 : min_pass;
  } else {
    p->vrt = 0;
  }
}

// Round robin: the next RUNNABLE process after the last one chosen,
// scanning proc[] in a circle so no process is starved.
static struct proc *
pick_rr(void)
{
  for (int i = 1; i <= NPROC; i++) {
    struct proc *p = &proc[(last + i) % NPROC];
    if (p->state == RUNNABLE) {
      last = p - proc;
      return p;
    }
  }
  return 0;
}

// Strict priority: a RUNNABLE process with the lowest nice value;
// processes with equal nice take turns in round-robin order.
static struct proc *
pick_prio(void)
{
  int best = 100;

  for (struct proc *p = proc; p < &proc[NPROC]; p++)
    if (p->state == RUNNABLE && p->nice < best)
      best = p->nice;
  for (int i = 1; i <= NPROC; i++) {
    struct proc *p = &proc[(last + i) % NPROC];
    if (p->state == RUNNABLE && p->nice == best) {
      last = p - proc;
      return p;
    }
  }
  return 0;
}

static struct proc *
pick_mlfq(void)
{
  for (int level = 2; level >= 0; level--) {
    for (int i = 1; i <= NPROC; i++) {
      struct proc *p = &proc[(mlfq_last[level] + i) % NPROC];
      if (p->state == RUNNABLE && p->level == level) {
        mlfq_last[level] = p - proc;
        return p;
      }
    }
  }
  return 0;
}

static struct proc *
pick_mlfq_weighted(void)
{
  for (int level = 2; level >= 0; level--) {
    mlfq_weighted_update_floor(level, 0);
    uint64 min_pass = ~(uint64)0;

    for (struct proc *p = proc; p < &proc[NPROC]; p++)
      if (p->state == RUNNABLE && p->level == level &&
          p->mlfq_pass[level] < min_pass)
        min_pass = p->mlfq_pass[level];

    if (min_pass == ~(uint64)0)
      continue;

    for (int i = 1; i <= NPROC; i++) {
      struct proc *p = &proc[(mlfq_last[level] + i) % NPROC];
        if (p->state == RUNNABLE && p->level == level &&
          p->mlfq_pass[level] == min_pass) {
        mlfq_last[level] = p - proc;
        return p;
      }
    }
  }
  return 0;
}

static struct proc *
pick_stride(void)
{
  uint64 min_pass = stride_min_pass(0);

  if (min_pass == ~(uint64)0)
    return 0;

  for (int i = 1; i <= NPROC; i++) {
    struct proc *p = &proc[(stride_last + i) % NPROC];
    if (p->state == RUNNABLE && p->vrt == min_pass) {
      stride_last = p - proc;
      return p;
    }
  }
  return 0;
}

static int
mlfq_higher_runnable(int level)
{
  for (struct proc *p = proc; p < &proc[NPROC]; p++)
    if (p->state == RUNNABLE && p->level > level)
      return 1;
  return 0;
}

// Choose the next process to run, or return 0 if none is RUNNABLE.
struct proc *
policy_pick_next_proc(void)
{
  switch (sched_current) {
  case SCHED_PRIO:
    return pick_prio();
  case SCHED_STRIDE:
    return pick_stride();
  case SCHED_MLFQ_WEIGHTED:
    mlfq_boost_if_due();
    return pick_mlfq_weighted();
  case SCHED_MLFQ:
  case SCHED_MLFQ_Q1_5:
  case SCHED_MLFQ_Q1_ALLOT5:
  case SCHED_MLFQ_BOOST30:
    mlfq_boost_if_due();
    return pick_mlfq();
  default:
    return pick_rr();
  }
}

// Called just before the framework switches to p.
void
policy_dispatch(struct proc *p)
{
  p->qticks = 0; // a new quantum starts
}

// Called on every timer tick while p is running.
// Return 1 to preempt p, or 0 to let it keep running.
// A process is charged only for ticks that arrive while it is running.
// A loop of pause(1) wakes at a tick, runs briefly, and sleeps again
// before the next one, so it is never charged and cannot test a rule
// such as an allotment kept across sleeps. A test that is charged must
// keep the CPU across a tick, for example by spinning until uptime()
// changes before each pause(1); workload jobs are charged correctly.
int
policy_tick(struct proc *p)
{
  if (sched_current == SCHED_MLFQ_WEIGHTED) {
    p->used++;
    p->qticks++;
    mlfq_weighted_rebase_if_needed();
    p->mlfq_pass[p->level] += stride_for_nice(p->nice);
    mlfq_weighted_update_floor(p->level, 0);

    mlfq_boost_if_due();

    int allotment = mlfq_allotment(p->level);
    if (allotment > 0 && p->used >= allotment) {
      int old_level = p->level;
      p->level--;
      p->used = 0;
      mlfq_weighted_clamp(p, p->level);
      mlfq_log_level(p, old_level, p->level, ticks);
      return 1;
    }

    int preempt = p->qticks >= mlfq_quantum(p->level);
    if (mlfq_higher_runnable(p->level))
      preempt = 1;
    return preempt;
  }

  if (sched_current == SCHED_STRIDE) {
    p->qticks++;
    stride_rebase_if_needed();
    p->vrt += stride_for_nice(p->nice);
    return p->qticks >= quantum;
  }

  if (mlfq_policy(sched_current)) {
    p->used++;
    p->qticks++;

    mlfq_boost_if_due();

    int allotment = mlfq_allotment(p->level);
    if (allotment > 0 && p->used >= allotment) {
      int old_level = p->level;
      p->level--;
      p->used = 0;
      mlfq_log_level(p, old_level, p->level, ticks);
    }

    int preempt = p->qticks >= mlfq_quantum(p->level);
    if (mlfq_higher_runnable(p->level))
      preempt = 1;
    return preempt;
  }

  p->qticks++;
  return p->qticks >= quantum;
}

// Called when p wakes from sleep (for example, when its simulated I/O
// or a pause() ends) and becomes RUNNABLE again. Use it to decide how
// a waking process re-enters the policy; for example, a fair-share or
// stride policy may move a process that slept a long time up to the
// current minimum, so it cannot claim the CPU for the time it slept.
void
policy_wakeup(struct proc *p)
{
  if (sched_current == SCHED_MLFQ_WEIGHTED) {
    mlfq_weighted_clamp(p, p->level);
  } else if (sched_current == SCHED_STRIDE) {
    uint64 min_pass = stride_min_pass(p);
    if (min_pass != ~(uint64)0 && p->vrt < min_pass)
      p->vrt = min_pass;
  }
}

// Called whenever p's nice value changes.
void
policy_nice(struct proc *p)
{
  if (!mlfq_policy(sched_current))
    return;

  int new_start_level = mlfq_start_level(p->nice);
  if (new_start_level != p->start_level) {
    int old_level = p->level;
    p->start_level = new_start_level;
    p->level = new_start_level;
    p->used = 0;
    if (sched_current == SCHED_MLFQ_WEIGHTED && old_level != p->level)
      mlfq_weighted_clamp(p, p->level);
    mlfq_log_level(p, old_level, p->level, ticks);
  }
}
