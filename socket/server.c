#include <sys/socket.h>
#include <sys/types.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>

#define MAX_THREAD_SIZE (10)

typedef struct msg_data_t {
  int content;
  pthread_mutex_t mutex;
  int * nsd_buf;
  pthread_mutex_t nsd_mutex;
} msg_data_t;

void * client_worker(void * arg) {
  // buf now as "len(4byte)message(int in string with size = len)"
  msg_data_t * msg_data = arg;
  char message[8];
  int bytes_read;
  int nsd = *(msg_data->nsd_buf);
  pthread_mutex_unlock(&msg_data->nsd_mutex);
  sprintf(message, "%d", msg_data->content);
  unsigned int len = strlen(message) + 1;
  send(nsd, &len, sizeof(len), 0);
  send(nsd, &message, len, 0);
  for (;;) {
    bytes_read = recv(nsd, &len, sizeof(len), 0);
    if (bytes_read <= 0) {
      if (bytes_read < 0) {
	printf("Recv error\n");
      }
      break;
    }
    bytes_read = recv(nsd, &message, len, 0);
    if (bytes_read <= 0) {
      if (bytes_read < 0) {
	printf("Recv error\n");
      }
      break;
    }
    pthread_mutex_lock(&msg_data->mutex);
    //sscanf(message, "%d", &msg_data->content);
    msg_data->content++;
    printf("now: %d\n", msg_data->content);
    sprintf(message, "%d", msg_data->content);
    pthread_mutex_unlock(&msg_data->mutex);
    len = strlen(message) + 1;
    send(nsd, &len, sizeof(len), 0);
    send(nsd, &message, len, 0);
  }

  close(nsd);
  return NULL;
}

int main(int argc, char * argv[]) {
  int sd, nsd;
  struct sockaddr_in srv_addr, cl_addr;
  socklen_t cl_addr_size;
  
  sd = socket(AF_INET, SOCK_STREAM, 0);
  if (sd == -1) {
    fprintf(stderr, "Socket error\n");
    return EXIT_FAILURE;
  }

  srv_addr.sin_family = AF_INET;
  srv_addr.sin_port = 1004;
  srv_addr.sin_addr.s_addr = INADDR_ANY;

  if (bind(sd, (struct sockaddr *) &srv_addr, sizeof(srv_addr)) == -1) {
    fprintf(stderr, "Bind error\n");
    return EXIT_FAILURE;
  }

  if (listen(sd, 1) == -1) {
    fprintf(stderr, "Listen error\n");
    return EXIT_FAILURE;
  }

  cl_addr_size = sizeof(struct sockaddr_in);
  msg_data_t msg_data;
  msg_data.content = 0;
  pthread_mutex_init(&msg_data.mutex, NULL);
  msg_data.nsd_buf = &nsd;
  pthread_mutex_init(&msg_data.nsd_mutex, NULL);
  
  for(;;) {
    pthread_mutex_lock (&msg_data.nsd_mutex);
    nsd = accept(sd, (struct sockaddr *) &cl_addr, &cl_addr_size);
    if (nsd == -1) {
      fprintf(stderr, "Accept error\n");
      return EXIT_FAILURE;
    }
    printf("connected %s\n", inet_ntoa(cl_addr.sin_addr));
    pthread_t tid;
    pthread_create(&tid, NULL, client_worker, &msg_data);
    pthread_mutex_lock (&msg_data.nsd_mutex);    
    pthread_mutex_unlock (&msg_data.nsd_mutex);    
  }

  close(sd);
}
