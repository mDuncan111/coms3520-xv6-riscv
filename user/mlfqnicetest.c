#include "kernel/types.h"
#include "user/user.h"

static void
burn_ticks(int count)
{
  int start = uptime();
  volatile int work = 0;

  while (uptime() - start < count)
    work++;
}

static void
boundary_test(void)
{
  printf("mlfqnicetest: boundary case starts in Q1\n");
  burn_ticks(3);
  printf("mlfqnicetest: change nice 0->11 (Q1->Q0)\n");
  if (nice(getpid(), 11) != 11) {
    printf("mlfqnicetest: nice 0->11 failed\n");
    exit(1);
  }

  burn_ticks(5);
  printf("mlfqnicetest: change nice 11->-10 (Q0->Q2); check reset\n");
  if (nice(getpid(), -21) != -10) {
    printf("mlfqnicetest: nice 11->-10 failed\n");
    exit(1);
  }
  burn_ticks(2);
  printf("mlfqnicetest: boundary case complete\n");
}

static void
same_band_test(void)
{
  printf("mlfqnicetest: same-band case starts in Q1\n");
  if (nice(getpid(), -10) != -10) {
    printf("mlfqnicetest: nice 0->-10 failed\n");
    exit(1);
  }
  burn_ticks(2);
  printf("mlfqnicetest: now demoted from Q2 to Q1\n");

  burn_ticks(3);
  printf("mlfqnicetest: change nice -10->-15 within Q2 band\n");
  if (nice(getpid(), -5) != -15) {
    printf("mlfqnicetest: nice -10->-15 failed\n");
    exit(1);
  }

  burn_ticks(7);
  printf("mlfqnicetest: same-band follow-up complete\n");
}

static int
run_child(int same_band)
{
  int pid = fork();
  int status;

  if (pid < 0)
    return -1;
  if (pid == 0) {
    if (same_band)
      same_band_test();
    else
      boundary_test();
    exit(0);
  }
  if (wait(&status) != pid || status != 0)
    return -1;
  return 0;
}

int
main(void)
{
  if (set_scheduler(3) < 0) {
    printf("mlfqnicetest: set_scheduler(3) failed\n");
    exit(1);
  }
  startLogging();

  if (run_child(0) < 0 || run_child(1) < 0) {
    stopLogging();
    printf("mlfqnicetest: child test failed\n");
    exit(1);
  }

  stopLogging();
  printf("mlfqnicetest: done\n");
  exit(0);
}