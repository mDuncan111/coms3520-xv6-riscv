#include "kernel/types.h"
#include "user/user.h"

#define Q1_SLEEP_TICKS 8
#define Q0_RUN_TICKS 24

static void
burn_ticks(int count)
{
  int start = uptime();
  volatile int work = 0;

  while (uptime() - start < count)
    work++;
}

int
main(void)
{
  int gate[2], ready[2];
  int q0pid, q1pid, children = 0, failed = 0, status;
  char token = 'x';

  if (set_scheduler(3) < 0) {
    printf("mlfqpreempttest: set_scheduler(3) failed\n");
    exit(1);
  }
  if (nice(getpid(), 11) != 11) {
    printf("mlfqpreempttest: could not place parent in Q0\n");
    exit(1);
  }
  startLogging();

  if (pipe(gate) < 0 || pipe(ready) < 0) {
    stopLogging();
    printf("mlfqpreempttest: pipe failed\n");
    exit(1);
  }

  q0pid = fork();
  if (q0pid < 0) {
    close(gate[0]);
    close(gate[1]);
    close(ready[0]);
    close(ready[1]);
    stopLogging();
    printf("mlfqpreempttest: Q0 fork failed\n");
    exit(1);
  }
  if (q0pid == 0) {
    close(gate[1]);
    close(ready[0]);
    close(ready[1]);
    if (nice(getpid(), 11) != 11) {
      printf("mlfqpreempttest: could not place child in Q0\n");
      exit(1);
    }
    if (read(gate[0], &token, 1) != 1) {
      printf("mlfqpreempttest: Q0 gate read failed\n");
      exit(1);
    }
    close(gate[0]);
    printf("mlfqpreempttest: Q0 pid %d active start at %d\n",
           getpid(), uptime());
    burn_ticks(Q0_RUN_TICKS);
    printf("mlfqpreempttest: Q0 pid %d active end at %d\n",
           getpid(), uptime());
    exit(0);
  }
  children++;
  close(gate[0]);

  q1pid = fork();
  if (q1pid < 0) {
    failed = 1;
    close(ready[0]);
    close(ready[1]);
  } else if (q1pid == 0) {
    close(gate[0]);
    close(gate[1]);
    close(ready[0]);
    printf("mlfqpreempttest: Q1 pid %d sleeping at %d\n",
           getpid(), uptime());
    if (write(ready[1], &token, 1) != 1) {
      printf("mlfqpreempttest: Q1 ready signal failed\n");
      exit(1);
    }
    close(ready[1]);
    if (pause(Q1_SLEEP_TICKS) < 0) {
      printf("mlfqpreempttest: Q1 pause failed\n");
      exit(1);
    }
    printf("mlfqpreempttest: Q1 pid %d woke at %d\n",
           getpid(), uptime());
    exit(0);
  } else {
    children++;
    close(ready[1]);
    if (read(ready[0], &token, 1) != 1)
      failed = 1;
    close(ready[0]);
  }

  if (write(gate[1], &token, 1) != 1)
    failed = 1;
  close(gate[1]);

  while (children-- > 0) {
    if (wait(&status) < 0 || status != 0)
      failed = 1;
  }

  stopLogging();
  if (failed) {
    printf("mlfqpreempttest: test setup or child failed\n");
    exit(1);
  }
  printf("mlfqpreempttest: done\n");
  exit(0);
}