#include "kernel/types.h"
#include "user/user.h"

#define STRETCHES 5
#define STRETCH_TICKS 2
#define SLEEP_TICKS 2

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
  int pid, status;

  if (set_scheduler(3) < 0) {
    printf("mlfqtest: set_scheduler(3) failed\n");
    exit(1);
  }
  startLogging();

  pid = fork();
  if (pid < 0) {
    stopLogging();
    printf("mlfqtest: fork failed\n");
    exit(1);
  }
  if (pid == 0) {
    for (int burst = 1; burst <= STRETCHES; burst++) {
      burn_ticks(STRETCH_TICKS);
      if (burst < STRETCHES) {
        printf("mlfqtest: burst %d before sleep\n", burst);
        if (pause(SLEEP_TICKS) < 0) {
          printf("mlfqtest: pause failed\n");
          exit(1);
        }
        printf("mlfqtest: burst %d after sleep\n", burst);
      }
    }
    exit(0);
  }

  if (wait(&status) != pid) {
    stopLogging();
    printf("mlfqtest: wait failed\n");
    exit(1);
  }
  stopLogging();
  if (status != 0) {
    printf("mlfqtest: child failed\n");
    exit(1);
  }
  printf("mlfqtest: done\n");
  exit(0);
}