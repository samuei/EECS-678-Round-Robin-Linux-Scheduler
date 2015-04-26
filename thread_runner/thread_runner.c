#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>
#include <errno.h>
#include <sched.h>
#include <getopt.h>
#include <ctype.h>
#include <limits.h>

#include <sys/time.h>
#include <sys/resource.h>
#include <sys/syscall.h>

#define SCHED_OTHER_RR 6
#define SCHED_NORMAL 0

#define SYS_other_rr_getquantum 337
#define SYS_other_rr_setquantum 338

#define MIN_THREADS  1
#define MAX_THREADS  20
#define MAX_ARG_SIZE 80
#define START_CHAR   97
#define AGG_DEFAULT  10000

#define MIN_PRIO -19
#define MAX_PRIO 20

struct thread_args {
  int tid;
  int prio;
  int nchars;
  char mychar;
};

int num_threads, buffer_size, quantum, old_quantum, sched_policy, ppvals,
    prio_set, quantum_set, *prio_array, *rec;
char *val_buf;
int total_num_chars;
int pos = 0, aggregate = AGG_DEFAULT;
pthread_t *threads;

const char short_usage [] =
"Usage: thread_runner [OPTIONS] num_threads buffer_size\n";

const char long_usage [] =
"Run several threads under different scheduling policies. Each thread prints a\n"
"unique character to a common global buffer. This buffer is dumped at the end of\n"
"the run to show execution intervals for each thread.\n"
"\n"
"Arguments:\n"
"  num_threads  is the number of threads to run (between 1 and 20)\n"
"  buffer_size  is the size of the buffer (in bytes) to use -- can use k (KB),\n"
"               m (MB), or g (GB)\n"
"\n"
"Options:\n"
"  -s, --scheduler  is one of either `normal\' or `other_rr\' (default is normal)\n"
"  -p, --priority   is a comma separated list of integer priorities (for the\n"
"                   normal scheduling policy)\n"
"  -q, --quantum    is the default timeslice for processes with the other_rr\n"
"                   scheduling policy (0 is FCFS)\n"
"  -a, --aggregate  is an integer specifying the number of characters in the\n"
"                   global buffer that correspond to one character of printed\n"
"                   output at the end of the run. Default aggregate is 10000,\n"
"                   --aggregate=1 prints the entire buffer\n"
"  -v, --ppvals     print the length of each execution interval during\n"
"                   postprocessing\n"
"  -h, --help       print this message\n\n";

const char try_help [] =
"Try `thread_runner --help\' for more information.\n";

/*
 * store a string version of the scheduling policy in buf
 */
void get_policy_str(char *buf, int policy)
{
  switch(policy) {
    case SCHED_NORMAL:
      strcpy(buf, "normal");
      break;
    case SCHED_OTHER_RR:
      strcpy(buf, "other_rr");
      break;
    default:
      sprintf(buf, "%d (unknown)", policy);
      break;
  }
}

int is_valid_char(char c)
{
  return ( (((int)c) >= (START_CHAR)) &&
           (((int)c) <= (START_CHAR+num_threads)) );
}

/*
 * print out command line arguments
 */
void print_arguments()
{
  int i;

  printf("num_threads:  %d\n", num_threads);
  printf("buffer_size:  %d\n", buffer_size);
  printf("sched_policy: %d\n", sched_policy);
  printf("quantum:      %d\n", quantum);
  printf("aggregate:    %d\n", aggregate);
  printf("ppvals:       %d\n", ppvals);

  printf("prio_array:   { ");
  if (prio_array) {
    for (i = 0; i < num_threads-1; i++) {
      printf("%d, ", prio_array[i]);
    }
    printf("%d", prio_array[i]);
  } else {
    printf("(null)");
  }
  printf(" }\n");
}

/*
 * parse buffer size argument
 */
int parse_buf_size(char *bsize)
{
  int nbytes;
  char *endptr, c;

  errno  = 0;
  nbytes = strtol(bsize, &endptr, 10);
  if (errno) {
    perror("parsing buffer size");
    exit(1);
  }

  if (*endptr != '\0') {
    c = endptr[0];
    if (c == 'k' || c == 'K') {
      nbytes *= pow(2,10);
    }
    else if (c == 'm' || c == 'M') {
      nbytes *= pow(2,20);
    }
    else if (c == 'g' || c == 'G') {
      nbytes *= pow(2,30);
    } else {
      printf("Error: invalid buffer size: %s\n", bsize);
      printf("%s",try_help);
      exit(1);
    }
  }

  if (nbytes < 0) {
      printf("Error: buffer size too large (must be < %d)\n", INT_MAX);
      exit(1);
  }

  return nbytes;
}

/*
 * parse the priorities array
 */
int *parse_prio_array(char *parg)
{
  char *x;
  int *ret, i;
  
  ret = malloc(num_threads * sizeof(int));
  if (ret == NULL) {
    printf("Error: parse_prio_arg out of memory\n");
    exit(1);
  }

  x = strtok(parg, ",");
  for (i=0; i<num_threads; i++) {
    if (x == NULL)
      break;
    ret[i] = atoi(x);
    if (ret[i] < MIN_PRIO || ret[i] > MAX_PRIO) {
      printf("Error: invalid thread priority: %d\n", ret[i]);
      printf("Thread priority min: %d, max: %d\n", MIN_PRIO, MAX_PRIO);
      exit(1);
    }
    x = strtok(NULL, ",");
  }

  if (i < num_threads) {
    printf("warning: not enough thread priorities specified.\n"
           "using priority=0 for the remaining threads ...\n");
    for (;i<num_threads;i++) {
      ret[i] = 0;
    }
  }

  if (x != NULL) {
    printf("Error: more thread priorities than threads\n");
    printf("%s",try_help);
    exit(1);
  }

  return ret;
}

/*
 * parse the quantum value from the command line
 */
int parse_quantum(char *qarg)
{
  int ret;
  ret = atoi(qarg);
  if (ret < 0) {
    printf("Error: invalid quantum value: %s\n", qarg);
    printf("%s",try_help);
  }
  return ret;
}

/*
 * parse command line arguments
 */
void parse_arguments(int argc, char *argv[])
{
  int c, i;
  char quantum_arg[MAX_ARG_SIZE], prio_arg[MAX_ARG_SIZE],
       policy_buf[MAX_ARG_SIZE], *endptr;

  prio_set = quantum_set = 0;

  opterr = 0;
  while (1) {
    int option_index = 0;

    /* define the allowable options */
    static struct option long_options[] = 
    {
      {"help",no_argument, 0,'h'},
      {"dump",no_argument, 0,'d'},
      {"ppvals",no_argument, 0,'v'},
      {"aggregate", required_argument,0,'a'},
      {"scheduler", required_argument,0,'s'},
      {"priority", required_argument,0,'p'},
      {"quantum", required_argument, 0, 'q'},
      {0,0,0,0}
    };

    c = getopt_long(argc, argv, "hds:p:q:", long_options,&option_index);
    if (c == -1)
      break;

    switch (c) {
      case 'h':
        printf("%s", short_usage);
        printf("%s", long_usage);
        exit(0);
      case 'v':
        ppvals = 1;
        break;
      case 'a':
        errno  = 0;
        aggregate = strtol(optarg, &endptr, 10);
        if (errno) {
          perror("parsing aggregate");
          exit(1);
        }
        break;
      case 's':
        if (strcasecmp(optarg,"normal") == 0) {
          sched_policy = SCHED_NORMAL;
        } else if (strcasecmp(optarg,"other_rr") == 0) {
          sched_policy = SCHED_OTHER_RR;
        } else {
          printf("Error: invalid scheduling policy: %s\n", optarg);
          printf("%s", try_help);
          exit(1);
        }
        get_policy_str(policy_buf, sched_policy);
        break;
      case 'p':
        strcpy(prio_arg, optarg);
        prio_set = 1;
        break;
      case 'q':
        strcpy(quantum_arg, optarg);
        quantum_set = 1;
        break;
      case '?':
        /* getopt_long already printed an error message */
        break;
    }
  }

  if ( (argc - optind) != 2 ) {
    printf("%s", short_usage);
    printf("%s", try_help);
    exit(1);
  }

  num_threads = atoi(argv[optind]);
  if (MIN_THREADS > num_threads || num_threads > MAX_THREADS) {
    printf("Error: invalid number of threads: %d\n", num_threads);
    printf("Number of threads must be between %d and %d\n",
           MIN_THREADS, MAX_THREADS);
    exit(1);
  }

  buffer_size = parse_buf_size(argv[optind+1]);

  if (prio_set) {
    if (sched_policy != SCHED_NORMAL) {
      printf("Error: thread priorities specified with %s scheduling policy\n"
             "Thread priorities are only valid with the normal scheduling policy\n",
             policy_buf);
      exit(1);
    }
    prio_array = parse_prio_array(prio_arg);
  } else {
    prio_array = malloc(num_threads * sizeof(int));
    if (prio_array == NULL) {
      printf("Error: out of memory creating prio_array\n");
      exit(1);
    }
    for (i=0; i<num_threads; i++) {
      prio_array[i] = 0;
    }
  }

  if (quantum_set) {
    if (sched_policy != SCHED_OTHER_RR) {
      printf("Error: quantum specified with %s scheduling policy\n"
             "A quantum value is only valid with the other_rr scheduling policy\n",
             policy_buf);
      exit(1);
    }
    quantum = parse_quantum(quantum_arg);
  }
}

/*
 * postprocess the val_buf
 */
void postprocess()
{
  int i, cnt, start;
  char cur;

  rec = malloc(num_threads * sizeof(int));
  if (rec == NULL) {
    printf("Error creating rec: out of memory\n");
    exit(1);
  }

  for ( i = 0 ; i < num_threads; i++ ) {
    rec[i] = 0;
  }

  /* postprocess the val_buf */
  cnt = 0;
  for (i = 0, cur = val_buf[0]; i < total_num_chars;) {
    if (cur == val_buf[i]) {
      if ( is_valid_char(val_buf[i]) ) {
        rec[(((int)val_buf[i]) - START_CHAR)]++;
      }
      cnt++; i++;
    } else {
      if ( is_valid_char(cur) ) {
        if (ppvals)
          printf("%c: %d\n", cur, cnt);
      }
      cur = val_buf[i];
      cnt = 0;
    }
  }

  if ( (((int)cur) >= (START_CHAR))
    && (((int)cur) <= (START_CHAR+num_threads)) ) {
    if (ppvals)
      printf("%c: %d\n", cur, cnt);
  }
}

void dump_val_buf()
{
  int i, cnt, lcnt;
  char cur;

  printf("\ndumping the val_buf (aggregate=%d):\n\n", aggregate);

  cnt = 0, lcnt = 0;
  for (i = 0, cur = val_buf[0]; i < total_num_chars;) {
    if (cur == val_buf[i]) {
      if ((cnt % aggregate) == 0) {
        if (is_valid_char(cur)) {
          printf("%c", cur);
          lcnt++;
          if (lcnt == 80) {
            printf("\n");
            lcnt = 0;
          }
        }
      }
      cnt++; i++;
    } else {
      cur = val_buf[i];
      cnt = 0;
    }
  }
  printf("\n");
}

void *run(void *arg) 
{
  int i;
  struct thread_args *my_args = (struct thread_args*) arg;
  pid_t tid;

  printf("thread: %-3d", my_args->tid);
  if (prio_set) {
    printf("(priority=%-2d) ", my_args->prio);
  }
  printf("writing %d %c's\n", my_args->nchars, my_args->mychar);

  /* set priority */
  tid = syscall(SYS_gettid);
  if (prio_set) {
    setpriority(PRIO_PROCESS, tid, my_args->prio);
  }

  /* write characters to the val_buf */
  for(i = 0; i<my_args->nchars; i++) {
    if (pos > total_num_chars)
      break;

    *(val_buf + pos) = my_args->mychar;
    __sync_fetch_and_add(&pos,1);
  }

  free(my_args);
  pthread_exit(NULL);
}

int main(int argc, char *argv[])
{
  int i, my_quantum;
  struct thread_args *targs;
  pthread_attr_t attr;
  struct sched_param param;
  struct timespec ts;

  sched_policy = SCHED_NORMAL;
  num_threads = quantum = ppvals = 0;

  parse_arguments(argc, argv);
  //print_arguments();


  /* set scheduling policy */
  if (sched_policy == SCHED_OTHER_RR) {

    /* priority has no effect -- just use 0 */
    param.sched_priority = 0;
    if ( sched_setscheduler(getpid(), sched_policy, &param) == -1) {
      perror("sched_setscheduler");
      exit(1);
    };

    if (quantum_set) {
      old_quantum = syscall (SYS_other_rr_getquantum);
      syscall (SYS_other_rr_setquantum, quantum);
    }

    my_quantum = syscall (SYS_other_rr_getquantum);
    printf("other_rr scheduler selected, quantum=%d", my_quantum);
    if (my_quantum == 0)
      printf(" (FCFS policy)");
    printf("\n");
  } else {
    printf("normal (CFS) scheduler selected\n");
  }

  /* create the buffer */
  if ( (val_buf = (char *) malloc(buffer_size)) == NULL ) {
    printf("error: could not allocate val_buf\n");
    exit(1);
  }
  total_num_chars  = (buffer_size / sizeof(char));


  /* create and start each thread */
  if ( (threads = malloc(num_threads*sizeof(pthread_t))) == NULL ) {
    printf("error: could not allocate threads\n");
    exit(1);
  };

  pthread_attr_init(&attr);
  pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_JOINABLE);

  for (i = 0; i < num_threads; i++) {
    targs = malloc(sizeof(*targs));
    targs->tid    = i;
    targs->prio   = prio_array[i];
    targs->mychar = (char) (i+START_CHAR);
    targs->nchars = (total_num_chars / num_threads);
    pthread_create(&threads[i], &attr, run, (void *)targs);
  }


  /* Wait for all threads to complete */ 
  for (i = 0; i < num_threads; i++) {
    pthread_join(threads[i], NULL);
  }

  printf ("\ncompleted %d threads -- processing shared memory segment\n",
          num_threads);

  postprocess();

  printf("\n");
  for ( i = 0 ; i < num_threads; i++ ) {
    printf("Thread: %d wrote %d %c's\n", i, rec[i], ((char)(i+START_CHAR)));
  }

  dump_val_buf();

  if (quantum_set)
    syscall (SYS_other_rr_setquantum, old_quantum);

  /* Clean up and exit */
  pthread_attr_destroy(&attr);
  pthread_exit (NULL);
}
