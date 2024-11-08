#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <argp.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include "hash.h"

static const int MAX_PENDING = 5;

struct server_arguments {
	int port;
	char *salt;
	size_t salt_len;
};

error_t server_parser(int key, char *arg, struct argp_state *state) {
	struct server_arguments *args = state->input;
	error_t ret = 0;
	switch(key) {
	case 'p':
		/* Validate that port is correct and a number, etc!! */
		args->port = atoi(arg);
		if (0 /* port is invalid */) {
			argp_error(state, "Invalid option for a port, must be a number");
		}
		break;
	case 's':
		args->salt_len = strlen(arg);
		args->salt = malloc(args->salt_len+1);
		strcpy(args->salt, arg);
		break;
	default:
		ret = ARGP_ERR_UNKNOWN;
		break;
	}
	return ret;
}

void server_parseopt(int argc, char *argv[], struct server_arguments *args) {

	struct argp_option options[] = {
		{ "port", 'p', "port", 0, "The port to be used for the server" ,0},
		{ "salt", 's', "salt", 0, "The salt to be used for the server. Zero by default", 0},
		{0}
	};

	struct argp argp_settings = { options, server_parser, 0, 0, 0, 0, 0 };
	bzero(args, sizeof(*args));
	
    if (argp_parse(&argp_settings, argc, argv, 0, NULL, args) != 0) {
		printf("Got an error condition when parsing\n");
	}

}

void HandleTCPClient(int client_socket, char *salt, size_t salt_len){
  uint32_t num_hash_req;
  recv(client_socket, &num_hash_req, 4, 0);
  recv(client_socket, &num_hash_req, 4, 0);

  uint32_t ack_flag = htonl(2);
  uint32_t length = htonl(ntohl(num_hash_req) * 40);
  send(client_socket, &ack_flag, 4, 0);
  send(client_socket, &length, 4, 0);

  uint32_t trash;
  struct checksum_ctx *checksum_ptr = checksum_create((uint8_t *)salt, salt_len);
  
  for (uint32_t i=0;i<ntohl(num_hash_req);i++){
    // Get data from client 
    recv(client_socket, &trash, 4, 0);
    recv(client_socket, &length, 4, 0);
    char buffer[4096];
    char hash[32];
    ssize_t num_bytes = recv(client_socket, buffer, 4096, 0);
    while (num_bytes > 0){
      if (num_bytes < 4096){
        checksum_finish(checksum_ptr, (uint8_t *)buffer, num_bytes, (uint8_t *)hash);
        break;
      }
      checksum_update(checksum_ptr, (uint8_t *)buffer);
      num_bytes = recv(client_socket, buffer, 4096, 0);
    }    

    // Send hash information
    uint32_t type = htonl(4);
    uint32_t idx = htonl(i);
    send(client_socket, &type, 4, 0);
    send(client_socket, &idx, 4, 0);
    send(client_socket, hash, 32, 0);
    checksum_reset(checksum_ptr);
  }
  checksum_destroy(checksum_ptr);
}

int main(int argc, char *argv[]) {

    struct server_arguments args;
    server_parseopt(argc, argv, &args);

    // Create socket 
    int server_socket;
    if ((server_socket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP)) < 0){
      printf("socket() failed\n");
    }

    // Local Adress
    struct sockaddr_in server_address;
    memset(&server_address, 0, sizeof(server_address));
    server_address.sin_family = AF_INET;
    server_address.sin_addr.s_addr = htonl(INADDR_ANY);
    server_address.sin_port = htons(args.port);

    // Bind to local address 
    if (bind(server_socket, (struct sockaddr*) &server_address, sizeof(server_address)) < 0){
      printf("bind() failed\n");
    }

    // Mark socket to listen for incoming connections 
    if (listen(server_socket, MAX_PENDING) < 0){
      printf("listen() failed\n");
    }

    for (;;){
      struct sockaddr_in client_address;
      socklen_t client_address_length = sizeof(client_address);

      // Client connects 
      int client_socket = accept(server_socket, (struct sockaddr *) &client_address, &client_address_length);
      if (client_socket < 0){
        printf("accept() failed\n");
      }

      char client_name[INET_ADDRSTRLEN];
      if (inet_ntop(AF_INET, &client_address.sin_addr.s_addr, client_name, sizeof(client_name)) != NULL){
        printf("Handling client %s/%d\n", client_name, ntohs(client_address.sin_port));
        HandleTCPClient(client_socket, args.salt, args.salt_len);
      }
    }


    free(args.salt);
	return 0;
}
