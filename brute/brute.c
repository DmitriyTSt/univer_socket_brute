#define _GNU_SOURCE
#include <sys/socket.h>
#include <sys/types.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <stdio.h>
#include <stdbool.h>
#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <semaphore.h>
#include <pthread.h>
#include <ucontext.h>
#include <crypt.h>

#define PASSWORD_LENGTH (9)
#define QUEUE_SIZE (4)
#define PREFIX_SIZE (3)

typedef enum {
  BM_ITER,
  BM_REC,
  BM_REC_ITER,
} brute_mode_t;

typedef enum {
  RM_SINGLE,
  RM_MULTI,
  RM_ITERATOR,
  RM_CLIENT,
  RM_SERVER,
} run_mode_t;

typedef struct config_t {
  char * tohash;
  char * hash;
  char * alph;
  char * addr;
  int port;
  int length;
  brute_mode_t brute_mode;
  run_mode_t run_mode;
} config_t;

typedef char password_t[PASSWORD_LENGTH + 1];

typedef struct result_t {
  password_t password;
} result_t;

typedef struct task_t {
  password_t password;
  int from, to;
} task_t;

typedef struct queue_t {
  task_t queue[QUEUE_SIZE];
  int head, tail;
  sem_t full, empty;
  pthread_mutex_t head_mutex, tail_mutex;
  bool cancel;
} queue_t;

typedef bool (*password_handler_t) (task_t * task, void * arg);

typedef struct st_data_t {
  config_t * config;
  result_t * result;
  struct crypt_data cd;
} st_data_t;

typedef struct mt_data_t {
  st_data_t st_data;
  queue_t queue;
  volatile int tip;
  pthread_mutex_t tip_mutex;
  pthread_cond_t tip_cond;
} mt_data_t;

typedef struct list_t {
  int nsd;
  struct list_t * prev;
  struct list_t * next;
} list_t;

typedef struct cs_data_t {
  mt_data_t mt_data;
  list_t * client;
  pthread_mutex_t list_mutex;
  pthread_mutex_t nsd_mutex;
} cs_data_t;

void queue_cancel (queue_t * queue)
{
  queue->cancel = true;
  sem_post (&queue->full);
  sem_post (&queue->empty);
}

void queue_init (queue_t * queue)
{
  queue->head = 0;
  queue->tail = 0;
  sem_init (&queue->full, 0, 0);
  sem_init (&queue->empty, 0, QUEUE_SIZE);
  pthread_mutex_init (&queue->head_mutex, NULL);
  pthread_mutex_init (&queue->tail_mutex, NULL);
  queue->cancel = false;
}

bool queue_push (queue_t * queue, task_t * task)
{
  sem_wait (&queue->empty);
  if (queue->cancel)
    {
      sem_post (&queue->empty);
      return (false);
    }
  pthread_mutex_lock (&queue->tail_mutex);

  queue->queue[queue->tail] = *task;
  queue->tail = (queue->tail + 1) % QUEUE_SIZE;

  pthread_mutex_unlock (&queue->tail_mutex);
  sem_post (&queue->full);
  return (true);
}

bool queue_pop (queue_t * queue, task_t * task)
{
  sem_wait (&queue->full);
  if (queue->cancel)
    {
      sem_post (&queue->full);
      return (false);
    }
  pthread_mutex_lock (&queue->head_mutex);

  *task = queue->queue[queue->head];
  queue->head = (queue->head + 1) % QUEUE_SIZE;

  pthread_mutex_unlock (&queue->head_mutex);
  sem_post (&queue->empty);
  return (true);
}

bool check_password (task_t * task, void * arg)
{
  //return false;
  st_data_t * st_data = arg;
  char * hash = crypt_r (task->password, st_data->config->hash, &st_data->cd);
  bool matched = (strcmp (hash, st_data->config->hash) == 0);
  if (matched) {
    strcpy (st_data->result->password, task->password);
    return true;
  }
  return false;
}

// rec _wrap
// 2 contexs
// makecontext for rec
// swap context to rec
// from rec swap context to rec_next_state with password in task

bool rec (config_t * config, task_t * task, int pos, password_handler_t password_handler, void * arg) {
  if (pos == task->to) {
    return (password_handler (task, arg));
  }
  int i;
  for (i = 0; config->alph[i]; i++) {
    task->password[pos] = config->alph[i];
    if (rec (config, task, pos + 1, password_handler, arg))
      return (true);
  }
  return (false);
}

typedef struct iter_state_t {
  int idx[PASSWORD_LENGTH];
  config_t * config;
  task_t * task;
  int alph_len;
  bool finished;
} iter_state_t;

void iter_state_init (iter_state_t * iter_state, task_t * task, config_t * config)
{
  int i;
  iter_state->config = config;
  iter_state->task = task;
  iter_state->alph_len = strlen (config->alph) - 1;
  iter_state->finished = false;

  for (i = task->from; i < task->to; i++) {
    iter_state->idx[i] = 0;
    task->password[i] = config->alph[0];
  }
}

bool iter_state_next (iter_state_t * iter_state)
{
  task_t * task = iter_state->task;
  config_t * config = iter_state->config;
  int i;

  if (iter_state->finished)
    return (true);

  for (i = task->to - 1; i >= 0 && iter_state->idx[i] == iter_state->alph_len; i--) {
    iter_state->idx[i] = 0;
    task->password[i] = config->alph[0];
  }

  if (i < task->from)
    {
      iter_state->finished = true;
      return (true);
    }
  task->password[i] = config->alph[++iter_state->idx[i]];
  return (false);
}

void brute_iter (task_t * task, config_t * config, password_handler_t password_handler, void * arg)
{
  iter_state_t iter_state;

  iter_state_init (&iter_state, task, config);

  for (;;) {
    if (password_handler (task, arg))
      break;
    if (iter_state_next (&iter_state))
      break;
  }
}

typedef struct rec_state_t {
  ucontext_t main_con, rec_con;
  config_t * config;
  task_t * task;
  bool finished;
  char stack[1 << 12];
} rec_state_t;

bool check_password_rec (task_t * task, void * arg)
{
  rec_state_t * rec_state = arg;
  swapcontext (&rec_state->rec_con, &rec_state->main_con);
  return false;
}

void brute_rec (task_t * task, config_t * config, password_handler_t password_handler, void * arg)
{
  rec (config, task , task->from, password_handler, arg);
}

void brute_rec_context (rec_state_t * rec_state)
{
  brute_rec (rec_state->task, rec_state->config, check_password_rec, rec_state);
  rec_state->finished = true;
}

void rec_state_init (rec_state_t * rec_state, task_t * task, config_t * config)
{
  rec_state->task = task;
  rec_state->config = config;
  getcontext (&rec_state->main_con);
  getcontext (&rec_state->rec_con);
  rec_state->rec_con.uc_link = &rec_state->main_con;
  rec_state->rec_con.uc_stack.ss_sp = &rec_state->stack;
  rec_state->rec_con.uc_stack.ss_size = sizeof (rec_state->stack);
  makecontext (&rec_state->rec_con, (void (*) (void))brute_rec_context, 1, rec_state);
  swapcontext (&rec_state->main_con, &rec_state->rec_con);
}

bool rec_state_next(rec_state_t * rec_state)
{
    if (rec_state->finished) {
        return true;
    }
    swapcontext (&rec_state->main_con, &rec_state->rec_con);

    return (rec_state->finished);
}

void brute_iter_rec (task_t * task, config_t * config, password_handler_t password_handler, void * arg)
{
  rec_state_t rec_state;

  rec_state_init (&rec_state, task, config);

  for (;;) {
    //printf("password '%s'\n", task->password);
    if (password_handler (task, arg))
      break;
    if (rec_state_next (&rec_state))
      break;
  }
}

void parse_params (config_t * config, int argc, char * argv[])
{
  int c;

  opterr = 0;
  while ((c = getopt (argc, argv, "e:a:l:h:p:d:irsmtcxz")) != -1)
    switch (c)
      {
      case 'e':
	config->tohash = optarg;
	break;
      case 'h':
        config->hash = optarg;
        break;
      case 'a':
        config->alph = optarg;
        break;
      case 'l':
	config->length = atoi (optarg);
	break;
      case 'i':
	config->brute_mode = BM_ITER;
	break;
      case 'r':
	config->brute_mode = BM_REC;
	break;
      case 'c':
	config->brute_mode = BM_REC_ITER;
	break;
      case 's':
	config->run_mode = RM_SINGLE;
	break;
      case 'm':
	config->run_mode = RM_MULTI;
	break;
      case 't':
	config->run_mode = RM_ITERATOR;
	break;
      case 'x':
	config->run_mode = RM_SERVER;
	break;
      case 'z':
	config->run_mode = RM_CLIENT;
	break;
      case 'p':
	config->port = atoi (optarg);
	break;
      case 'd':
	config->addr = optarg;
	break;
      }
}

void run_single (config_t * config, result_t * result)
{
  st_data_t st_data;
  task_t task;

  st_data.config = config;
  st_data.result = result;
  st_data.cd.initialized = 0;

  task.from = 0;
  task.to = config->length;
  task.password[config->length] = 0;

  switch (config->brute_mode) {
  case BM_ITER :
    brute_iter (&task, config, check_password, &st_data);
    break;
  case BM_REC :
    brute_rec (&task, config, check_password, &st_data);
    break;
  case BM_REC_ITER :
    brute_iter_rec (&task, config, check_password, &st_data);
    break;
  }
}

void * worker (void * arg)
{
  mt_data_t * mt_data = arg;
  st_data_t st_data;
  config_t * config = mt_data->st_data.config;
  task_t task;

  st_data.config = mt_data->st_data.config;
  st_data.result = mt_data->st_data.result;
  st_data.cd.initialized = 0;

  for (;;)
    {
      if (!queue_pop (&mt_data->queue, &task))
	break;

      task.to = task.from;
      task.from = 0;

      switch (config->brute_mode) {
      case BM_ITER :
	brute_iter (&task, config, check_password, &st_data);
	break;
      case BM_REC_ITER :
      case BM_REC :
	brute_rec (&task, config, check_password, &st_data);
	break;
      }

      pthread_mutex_lock (&mt_data->tip_mutex);
      if (0 == --mt_data->tip)
	pthread_cond_signal (&mt_data->tip_cond);
      pthread_mutex_unlock (&mt_data->tip_mutex);
    }
  return (NULL);
}

bool push_to_queue (task_t * task, void * arg)
{
  mt_data_t * mt_data = arg;

  pthread_mutex_lock (&mt_data->tip_mutex);
  ++mt_data->tip;
  pthread_mutex_unlock (&mt_data->tip_mutex);

  queue_push (&mt_data->queue, task);
  return (mt_data->st_data.result->password[0]);
}

void run_multi (config_t * config, result_t * result)
{
  mt_data_t mt_data;
  int i, ncpu = (int) sysconf (_SC_NPROCESSORS_ONLN);
  pthread_t tid[ncpu];
  task_t task;

  mt_data.st_data.config = config;
  mt_data.st_data.result = result;
  mt_data.tip = 0;
  pthread_mutex_init (&mt_data.tip_mutex, NULL);
  pthread_cond_init (&mt_data.tip_cond, NULL);

  queue_init (&mt_data.queue);

  task.from = PREFIX_SIZE;
  task.to = config->length;
  task.password[config->length] = 0;

  for (i = 0; i < ncpu; ++i) {
    pthread_create (&tid[i], NULL, worker, &mt_data);
  }

  switch (config->brute_mode) {
  case BM_ITER :
    brute_iter (&task, config, push_to_queue, &mt_data);
    break;
  case BM_REC :
    brute_rec (&task, config, push_to_queue, &mt_data);
    break;
  case BM_REC_ITER :
    brute_rec (&task, config, push_to_queue, &mt_data);
    break;
  }

  pthread_mutex_lock (&mt_data.tip_mutex);
  while (mt_data.tip != 0)
    pthread_cond_wait (&mt_data.tip_cond, &mt_data.tip_mutex);
  pthread_mutex_unlock (&mt_data.tip_mutex);

  queue_cancel (&mt_data.queue);
  for (i = 0; i < ncpu; ++i) {
    pthread_join (tid[i], NULL);
  }
}

typedef struct it_data_t {
  st_data_t st_data;
  iter_state_t iter_state;
  pthread_mutex_t mutex;
  task_t task;
} it_data_t;

void * it_worker (void * arg)
{
  it_data_t * it_data = arg;
  task_t task;
  st_data_t st_data;
  config_t * config = it_data->st_data.config;

  st_data.config = it_data->st_data.config;
  st_data.result = it_data->st_data.result;
  st_data.cd.initialized = 0;
  pthread_mutex_init(&it_data->mutex, NULL);

  for (;;)
    {
      if (it_data->st_data.result->password[0])
	break;

      pthread_mutex_lock (&it_data->mutex);
      task = it_data->task;
      bool finished = iter_state_next (&it_data->iter_state);
      pthread_mutex_unlock (&it_data->mutex);

      task.to = task.from;
      task.from = 0;

      switch (config->brute_mode) {
      case BM_ITER :
	brute_iter (&task, config, check_password, &st_data);
	break;
      case BM_REC_ITER :
      case BM_REC :
	brute_rec (&task, config, check_password, &st_data);
	break;
      }

      if (finished)
	break;
    }
  return (NULL);
}

void run_iterator (config_t * config, result_t * result)
{
  it_data_t it_data;
  int i, ncpu = (int) sysconf (_SC_NPROCESSORS_ONLN);
  pthread_t tid[ncpu];

  it_data.st_data.config = config;
  it_data.st_data.result = result;

  it_data.task.from = PREFIX_SIZE;
  it_data.task.to = config->length;
  it_data.task.password[config->length] = 0;

  iter_state_init (&it_data.iter_state, &it_data.task, config);

  for (i = 0; i < ncpu; ++i) {
    pthread_create (&tid[i], NULL, it_worker, &it_data);
  }

  it_worker (&it_data);

  for (i = 0; i < ncpu; ++i) {
    pthread_join (tid[i], NULL);
  }
}

void * server_worker(void * arg) {
  cs_data_t * cs_data = arg;
  int nsd = cs_data->client->nsd;
  pthread_mutex_unlock(&cs_data->nsd_mutex);
  return (NULL);
}

void server_sync (config_t * config, result_t * result) {
  int sd, nsd;
  struct sockaddr_in srv_addr, cl_addr;
  socklen_t cl_addr_size;
  
  sd = socket (AF_INET, SOCK_STREAM, 0);
  if (sd == -1) {
    fprintf (stderr, "Socket error\n");
    return ;
  }
  
  srv_addr.sin_family = AF_INET;
  srv_addr.sin_port = config->port;
  srv_addr.sin_addr.s_addr = INADDR_ANY;
  
  if (bind (sd, (struct sockaddr *) &srv_addr, sizeof(srv_addr)) == -1) {
    fprintf (stderr, "Bind error\n");
    return ;
  }
  
  if (listen (sd, 1) == -1) {
    fprintf (stderr, "Listen error\n");
    return ;
  }
  
  cl_addr_size = sizeof (struct sockaddr_in);
  cs_data_t cs_data;
  cs_data.mt_data.st_data.config = config;
  cs_data.mt_data.st_data.result = result;
  cs_data.mt_data.tip = 0;
  pthread_mutex_init (&cs_data.mt_data.tip_mutex, NULL);
  pthread_cond_init (&cs_data.mt_data.tip_cond, NULL);
  pthread_mutex_init(&cs_data.list_mutex, NULL);
  pthread_mutex_init(&cs_data.nsd_mutex, NULL);

  queue_init (&cs_data.mt_data.queue);
  list_t root_list;
  root_list.prev = &root_list;
  root_list.next = &root_list;
  for (;;) {
    nsd = accept (sd, (struct sockaddr *) &cl_addr, &cl_addr_size);
    if (nsd == -1) {
      fprintf (stderr, "Accept error\n");
      break;
    }
    
    list_t * client = malloc (sizeof (*client));
    if (NULL == client)
      {
	fprintf (stderr, "Error\n");
	break;
      }
    
    client->nsd = nsd;
    pthread_mutex_lock (&cs_data.list_mutex);
    client->next = root_list.next;
    root_list.next->prev = client;
    client->prev = &root_list;
    root_list.next = client;
    pthread_mutex_unlock (&cs_data.list_mutex);

    pthread_mutex_lock (&cs_data.nsd_mutex);
    cs_data.client = client;

    pthread_t tid;
    if (0 == pthread_create (&tid, NULL, server_worker, &cs_data))
      pthread_mutex_lock(&cs_data.nsd_mutex);
	
    pthread_mutex_unlock(&cs_data.nsd_mutex);
  }
  close (sd);
}

void client_sync(config_t * config, result_t * result) {
  int sd;
  struct sockaddr_in srv_addr;
  int bytes_read;
  sd = socket(AF_INET, SOCK_STREAM, 0);
  if (sd == -1) {
    fprintf(stderr, "Socket error\n");
    return ;
  }
  srv_addr.sin_family = AF_INET;
  srv_addr.sin_port = config->port;
  srv_addr.sin_addr.s_addr = inet_addr(config->addr);
  if (connect(sd, (struct sockaddr *) &srv_addr, sizeof(srv_addr)) == -1) {
    fprintf(stderr, "Bind error\n");
    return ;
  }
  char * message;
  unsigned int len = 0;
  for (;;) {
    bytes_read = recv(sd, &len, sizeof(len), 0);
    if (bytes_read <= 0) {
      if (bytes_read < 0) {
	      printf("Recv error\n");
      }
      break;
    }
    bytes_read = recv(sd, &message, len, 0);
    if (bytes_read <= 0) {
      if (bytes_read < 0) {
	      printf("Recv error\n");
      }
      break;
    }
    // now message is data
  }
  close(sd);
}

int main(int argc, char * argv[]) {
  config_t config = {
    .tohash = NULL,
    .hash = NULL,
    .alph = "abcd",
    .length = 4,
    .brute_mode = BM_ITER,
    .run_mode = RM_SINGLE,
    .addr = "127.0.0.1",
    .port = 1000,
  };
  result_t result = { "" };

  parse_params (&config, argc, argv);
  if (NULL != config.tohash) {
    printf("%s\n", crypt(config.tohash, "ab"));
    return 0;
  }
  if (NULL == config.hash)
    {
      fprintf (stderr, "Hash required\n");
      return (EXIT_FAILURE);
    }

  switch (config.run_mode)
    {
    case RM_SINGLE:
      run_single (&config, &result);
      break;
    case RM_MULTI:
      run_multi (&config, &result);
      break;
    case RM_ITERATOR:
      run_iterator (&config, &result);
      break;
    case RM_SERVER:
      server_sync (&config, &result);
      break;
    case RM_CLIENT:
      client_sync(&config, &result);
      break;
    }

  if (result.password[0])
    printf ("password '%s'\n", result.password);
  else
    printf ("password not found\n");

  return (EXIT_SUCCESS);
}
