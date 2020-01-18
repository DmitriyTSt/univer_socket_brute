#include <sys/socket.h>
#include <sys/types.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

int main(int argc, char * argv[]) {
  int sd;
  struct sockaddr_in srv_addr;
  int buf;
  int bytes_read;

  sd = socket(AF_INET, SOCK_STREAM, 0);
  if (sd == -1) {
    fprintf(stderr, "Socket error\n");
    return EXIT_FAILURE;
  }

  srv_addr.sin_family = AF_INET;
  srv_addr.sin_port = 1004;
  srv_addr.sin_addr.s_addr = inet_addr("127.0.0.1");

  if (connect(sd, (struct sockaddr *) &srv_addr, sizeof(srv_addr)) == -1) {
    fprintf(stderr, "Bind error\n");
    return EXIT_FAILURE;
  }

  int content = 0;
  char message[8];
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
    sscanf(message, "%d", &content);
    content++;
    printf("now: %d\n", content);
    sprintf(message, "%d", content);
    len = strlen(message) + 1;
    send(sd, &len, sizeof(len), 0);
    send(sd, &message, len, 0);
  }

  close(sd);
}
