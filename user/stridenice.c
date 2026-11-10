#include "kernel/types.h"
#include "user/user.h"

#define PHASE_TICKS 200

static void
run_worker(int gate[2], int ready[2])
{
  char token;

  close(gate[1]);
  close(ready[0]);

  if (nice(getpid(), 0) != 0)
    exit(1);
  if (read(gate[0], &token, 1) != 1)
    exit(1);
  close(gate[0]);
  if (token != 'G')
    exit(0);

  volatile uint64 work = 0;
  work++;
  token = 'R';
  if (write(ready[1], &token, 1) != 1)
    exit(1);
  close(ready[1]);

  for (;;)
    work++;
}

static void
stop_workers(int worker_a, int worker_b)
{
  int status;

  if (worker_a > 0)
    kill(worker_a);
  if (worker_b > 0)
    kill(worker_b);
  if (worker_a > 0)
    wait(&status);
  if (worker_b > 0)
    wait(&status);
}

int
main(void)
{
  int gate[2], ready[2];
  int worker_a = -1, worker_b = -1;
  int failed = 0;
  char start[2] = { 'G', 'G' };
  char token;
  int changed;

  if (set_scheduler(4) < 0) {
    printf("stridenice: set_scheduler(4) failed\n");
    exit(1);
  }
  if (pipe(gate) < 0) {
    printf("stridenice: start pipe failed\n");
    exit(1);
  }
  if (pipe(ready) < 0) {
    close(gate[0]);
    close(gate[1]);
    printf("stridenice: ready pipe failed\n");
    exit(1);
  }

  worker_a = fork();
  if (worker_a == 0)
    run_worker(gate, ready);
  if (worker_a < 0) {
    close(gate[0]);
    close(gate[1]);
    close(ready[0]);
    close(ready[1]);
    printf("stridenice: worker A fork failed\n");
    exit(1);
  }

  worker_b = fork();
  if (worker_b == 0)
    run_worker(gate, ready);
  if (worker_b < 0) {
    close(gate[0]);
    write(gate[1], "X", 1);
    close(gate[1]);
    close(ready[0]);
    close(ready[1]);
    stop_workers(worker_a, -1);
    printf("stridenice: worker B fork failed\n");
    exit(1);
  }

  close(gate[0]);
  close(ready[1]);
  if (write(gate[1], start, sizeof(start)) != sizeof(start))
    failed = 1;
  close(gate[1]);

  for (int i = 0; i < 2 && !failed; i++) {
    if (read(ready[0], &token, 1) != 1 || token != 'R')
      failed = 1;
  }
  close(ready[0]);

  if (failed) {
    stop_workers(worker_a, worker_b);
    printf("stridenice: workers failed to start\n");
    exit(1);
  }

  printf("stridenice: count only running lines for worker PIDs %d and %d\n",
         worker_a, worker_b);
  printf("stridenice: with the one-tick quantum, each dispatch is about one charged CPU tick; phase boundaries and logging add noise\n");
    printf("stridenice: expect about 50:50 before and a trend toward 90:10 after; not guaranteed and not direct proof of pass preservation\n");

  startLogging();
  printf("stridenice: baseline start tick %d A=%d B=%d\n",
         uptime(), worker_a, worker_b);
  if (pause(PHASE_TICKS) < 0)
    failed = 1;
  printf("stridenice: baseline end tick %d\n", uptime());

  if (!failed) {
    changed = nice(worker_a, -10);
    printf("stridenice: nice change A pid %d by -10 returned %d at tick %d\n",
           worker_a, changed, uptime());
    if (changed != -10)
      failed = 1;
  }

  if (!failed) {
    printf("stridenice: weighted phase start tick %d A=%d nice=-10 B=%d nice=0\n",
           uptime(), worker_a, worker_b);
    if (pause(PHASE_TICKS) < 0)
      failed = 1;
    printf("stridenice: weighted phase end tick %d\n", uptime());
  }

  stopLogging();
  stop_workers(worker_a, worker_b);
  if (failed) {
    printf("stridenice: measurement failed\n");
    exit(1);
  }
  printf("stridenice: done\n");
  exit(0);
}