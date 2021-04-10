#include "packet-format.h"
#include <limits.h>
#include <netdb.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>


void update_window(int *window, int window_size, uint32_t sel_ack) {
  for (int i=1; i<window_size; i++) {
    if((sel_ack & 1) == 1) {
      window[i] = 2;
    }
    sel_ack = sel_ack >> 1;
  }
}

int read_chunk(char *data, size_t data_len, FILE *file, int seq_num) {
  fseek(file, seq_num*CHUNK_SIZE, SEEK_SET);
  return fread(data, 1, data_len, file);
}

//shifts the window to the first position that lacks and ack
void shif_window(int *window, int window_size) {
  for (int i = 0; i<window_size-1; i++) {
    window[i] = window[i+1];
  }
  window[window_size-1] = 0;
}

int main(int argc, char *argv[]) {

  if (argc != 5){
    perror("[ERROR]: Invalid number of arguments\n");
    exit(EXIT_FAILURE);
  }

  char *file_name = argv[1];
  char *host = argv[2];
  int port = atoi(argv[3]);
  int window_size = atoi(argv[4]);

  if(strlen(argv[3])!= 4 || !port){
    perror("[ERROR]: Expecting a four digit number for port parameter\n");
    exit(EXIT_FAILURE);
  }

  if (window_size < 1 || window_size > MAX_WINDOW_SIZE){
    printf("[ERROR]: Expecting a window size between 1-%d\n", MAX_WINDOW_SIZE );
    exit(EXIT_FAILURE);
  }

  int window_base = 0;
  int end_of_file = 0;
  int timeout = 0;
  int* window = malloc(sizeof(int)*window_size);
  for (int i=0; i<window_size; i++) {
    window[i] = 0;
  }


  FILE *file = fopen(file_name, "r");
  if (!file) {
    perror("fopen");
    exit(EXIT_FAILURE);
  }

  // Prepare server host address.
  struct hostent *he;
  if (!(he = gethostbyname(host))) {
    perror("gethostbyname");
    exit(EXIT_FAILURE);
  }

  struct sockaddr_in srv_addr = {
      .sin_family = AF_INET,
      .sin_port = htons(port),
      .sin_addr = *((struct in_addr *)he->h_addr),
  };

  int sockfd = socket(AF_INET, SOCK_DGRAM, 0);
  if (sockfd == -1) {
    perror("socket");
    exit(EXIT_FAILURE);
  }

  struct timeval tv;
  tv.tv_sec = 1;
  tv.tv_usec = 0;
  setsockopt(sockfd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

  data_pkt_t data_pkt;
  size_t data_len;
  fseek(file, 0L, SEEK_END);
  int chunks_read = ftell(file) / CHUNK_SIZE;
  rewind(file);

  do { // Generate segments from file, until the the end of the file.
    // Prepare data segment.
    int chunks_sent = 0;
    for (int i=0; i<window_size; i++) {
      //ignores succesfully sent chunks or already sent chunks (before timeout occurs)
      if (window[i] == 1 || window[i] == 2) continue;
      if (window_base + i > chunks_read) break;

      // Load data from file.
      data_len = read_chunk(data_pkt.data, sizeof(data_pkt.data), file, i + window_base);
      data_pkt.seq_num = htonl(i + window_base);


      // Send segment.
      ssize_t sent_len =
          sendto(sockfd, &data_pkt, offsetof(data_pkt_t, data) + data_len, 0,
                 (struct sockaddr *)&srv_addr, sizeof(srv_addr));
      printf("[SENDER]Sending segment %d, size %ld.\n", ntohl(data_pkt.seq_num),
             offsetof(data_pkt_t, data) + data_len);
      if (sent_len != offsetof(data_pkt_t, data) + data_len) {
        fprintf(stderr, "Truncated packet.\n");
        exit(EXIT_FAILURE);
      }
      chunks_sent++;

      //marks chunk as sent in window
      window[i] = 1;
      //detects last chunk
      if (data_len < 1000) break;
    }



    ack_pkt_t ack_pkt;
    ssize_t rcvd_len =
        recvfrom(sockfd, &ack_pkt, sizeof(ack_pkt_t), 0,
                 (struct sockaddr *)&srv_addr, &(socklen_t){sizeof(srv_addr)});
    if (rcvd_len == -1) {

      if (++timeout == MAX_RETRIES) {
        printf("Consecutive Timeouts.\n");
        exit(EXIT_FAILURE);
      }
      //on a timeout, marks not acknolewdged chunks as not sent to retransmit
      for( int i = 0 ; i < window_size; i++ ){
        if (window[i] == 1)
            window[i] = 0;
      }

    } else {
      timeout = 0;
      for (int i = window_base; i < ntohl(ack_pkt.seq_num);i++){
        //marks chunk as succesfully transmitted
        window[i - window_base] = 2;
      }

      while (window[0] == 2) {
        shif_window(window, window_size);
        window_base++;
      }
      update_window(window, window_size, ntohl(ack_pkt.selective_acks));
    }
    printf("[SENDER]Received ack %d, size %ld.\n", ntohl(ack_pkt.seq_num), rcvd_len);

    if (feof(file)) {
      end_of_file = 1;
    }
  } while (end_of_file == 0 || window_base <= chunks_read);

  // Clean up and exit.
  close(sockfd);
  fclose(file);
  free(window);

  return 0;
}
