#include "packet-format.h"
#include <arpa/inet.h>
#include <limits.h>
#include <netinet/in.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>
#include <string.h>


int write_chunk(char *data, size_t data_len, FILE *file, int seq_num) {
  fseek(file, seq_num * CHUNK_SIZE, SEEK_SET);
  return fwrite(data, 1, data_len, file);
}

void shif_window(int *window, int window_size) {
  for (int i = 0; i<window_size-1; i++) {
    window[i] = window[i+1];
  }
  window[window_size-1] = 0;
}

int empty_window(int * window, int window_size) {
  for (int i=0; i<window_size; i++)
    if (window[i] == 1)
      return 0;
  return 1;
}

uint32_t acks(int *window, int window_size) {
  uint32_t sel_ack = 0;
  for (int i=1; i<window_size; i++)
    if (window[i] == 1)
      sel_ack = sel_ack | 1<<(i-1);
  return sel_ack;
}

int main(int argc, char *argv[]) {

  if (argc != 4){
    perror("[ERROR]: Invalid number of arguments\n");
    exit(EXIT_FAILURE);
  }
  char *file_name = argv[1];
  int port = atoi(argv[2]);
  int window_size = atoi(argv[3]);

  if(strlen(argv[2])!= 4 || !port){
    perror("[ERROR]: Expecting a four digit number for port parameter\n");
    exit(EXIT_FAILURE);
  }

  if (window_size < 1 || window_size > MAX_WINDOW_SIZE){
    printf("[ERROR]: Expecting a window size between 1-%d\n", MAX_WINDOW_SIZE );
    exit(EXIT_FAILURE);
  }

  int window_base = 0;
  int last_pkt_rcvd = 0;
  int* window = malloc(sizeof(int)*window_size);
  for (int i=0; i<window_size; i++) {
    window[i] = 0;
  }

  FILE *file = fopen(file_name, "w");
  if (!file) {
    perror("fopen");
    exit(EXIT_FAILURE);
  }

  // Prepare server socket.
  int sockfd = socket(AF_INET, SOCK_DGRAM, 0);
  if (sockfd == -1) {
    perror("socket");
    exit(EXIT_FAILURE);
  }

  // Allow address reuse so we can rebind to the same port,
  // after restarting the server.
  if (setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &(int){1}, sizeof(int)) <
      0) {
    perror("setsockopt");
    exit(EXIT_FAILURE);
  }

  struct sockaddr_in srv_addr = {
      .sin_family = AF_INET,
      .sin_addr.s_addr = htonl(INADDR_ANY),
      .sin_port = htons(port),
  };
  if (bind(sockfd, (struct sockaddr *)&srv_addr, sizeof(srv_addr))) {
    perror("bind");
    exit(EXIT_FAILURE);
  }

  fprintf(stderr, "Receiving on port: %d\n", port);

  ssize_t len;
  do { // Iterate over segments, until last the segment is detected.
    // Receive segment.
    struct sockaddr_in src_addr;
    data_pkt_t data_pkt;
    len =
        recvfrom(sockfd, &data_pkt, sizeof(data_pkt), 0,
                 (struct sockaddr *)&src_addr, &(socklen_t){sizeof(src_addr)});
    printf("[RECEIVER]Received segment %d, size %ld.\n", ntohl(data_pkt.seq_num), len);

    // Write data to file.
    write_chunk(data_pkt.data, len - offsetof(data_pkt_t, data), file, ntohl(data_pkt.seq_num));


    if (! ((ntohl(data_pkt.seq_num) - window_base) >= window_size)) {
      if (len != sizeof(data_pkt_t)) {
        last_pkt_rcvd = 1;
      }

      if (window[ntohl(data_pkt.seq_num) - window_base] == 1)
        continue;
      else
        window[ntohl(data_pkt.seq_num) - window_base] = 1;

      while (window[0] == 1) {
        shif_window(window, window_size);
        window_base++;
      }
    }

    ack_pkt_t ack_pkt;
    ack_pkt.seq_num = htonl(window_base);
    ack_pkt.selective_acks = htonl(acks(window, window_size));

    // Send segment.
    ssize_t sent_len =
        sendto(sockfd, &ack_pkt, sizeof(ack_pkt_t), 0,
               (struct sockaddr *)&src_addr, sizeof(src_addr));
    printf("[RECEIVER]Sending ack %d, size %ld.\n", ntohl(ack_pkt.seq_num),
           sizeof(ack_pkt_t));
    if (sent_len != sizeof(ack_pkt_t)) {
      fprintf(stderr, "Truncated packet.\n");
      exit(EXIT_FAILURE);
    }

  } while (last_pkt_rcvd != 1 || empty_window(window, window_size) != 1);
  printf("[RECEIVER]Im down!!\n");

  // Clean up and exit.
  close(sockfd);
  fclose(file);
  free(window);

  return 0;
}
