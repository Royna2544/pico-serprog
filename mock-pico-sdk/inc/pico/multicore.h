
#include <pthread.h>
void multicore_launch_core1(void (*entry)()) {
  pthread_t thread;
  pthread_create(&thread, NULL, (void *(*)(void *))entry, NULL);
}
