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

struct client_arguments {
	char ip_address[16]; /* You can store this as a string, but I probably wouldn't */
	in_port_t port; /* is there already a structure you can store the address
	           * and port in instead of like this? */
	int hashnum;
	int smin;
	int smax;
	char *filename; /* you can store this as a string, but I probably wouldn't */
};

error_t client_parser(int key, char *arg, struct argp_state *state) {
	struct client_arguments *args = state->input;
	error_t ret = 0;
	int len;
	switch(key) {
	case 'a':
		/* validate that address parameter makes sense */
		strncpy(args->ip_address, arg, 16);
		if (0 /* ip address is goofytown */) {
			argp_error(state, "Invalid address");
		}
		break;
	case 'p':
		/* Validate that port is correct and a number, etc!! */
		args->port = atoi(arg);
		if (0 /* port is invalid */) {
			argp_error(state, "Invalid option for a port, must be a number");
		}
		break;
	case 'n':
		/* validate argument makes sense */
		args->hashnum = atoi(arg);
		break;
	case 300:
		/* validate arg */
		args->smin = atoi(arg);
		break;
	case 301:
		/* validate arg */
		args->smax = atoi(arg);
		break;
	case 'f':
		/* validate file */
		len = strlen(arg);
		args->filename = malloc(len + 1);
		strcpy(args->filename, arg);
		break;
	default:
		ret = ARGP_ERR_UNKNOWN;
		break;
	}
	return ret;
}


void client_parseopt(int argc, char *argv[], struct client_arguments *args) {
	struct argp_option options[] = {
		{ "addr", 'a', "addr", 0, "The IP address the server is listening at", 0},
		{ "port", 'p', "port", 0, "The port that is being used at the server", 0},
		{ "hashreq", 'n', "hashreq", 0, "The number of hash requests to send to the server", 0},
		{ "smin", 300, "minsize", 0, "The minimum size for the data payload in each hash request", 0},
		{ "smax", 301, "maxsize", 0, "The maximum size for the data payload in each hash request", 0},
		{ "file", 'f', "file", 0, "The file that the client reads data from for all hash requests", 0},
		{0}
	};

	struct argp argp_settings = { options, client_parser, 0, 0, 0, 0, 0 };
	bzero(args, sizeof(*args));

	if (argp_parse(&argp_settings, argc, argv, 0, NULL, args) != 0) {
		printf("Got error in parse\n");
	}
}

int main(int argc, char *argv[]) {
    
    // struct holding arg data 
    struct client_arguments args;
    client_parseopt(argc, argv, &args); // parse and set arg data 

    // Create TCP socket
    int sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (sock < 0){
      printf("socket() failed");
    }

    // Construct server adress 
    struct sockaddr_in server_address;
    memset(&server_address, 0, sizeof(server_address));
    server_address.sin_family = AF_INET;
    inet_pton(AF_INET, args.ip_address, &server_address.sin_addr.s_addr);
    server_address.sin_port = htons(args.port);

    // Connect to server 
    if (connect(sock, (struct sockaddr *) &server_address, sizeof(server_address)) < 0){
      printf("connect() failed\n");
    }

    // Send acknowledgement 
    uint32_t type = htonl(1);
    uint32_t n = htonl(args.hashnum);
    uint32_t ret1, ret2;
    send(sock, &type, 4, 0);
    send(sock, &n, 4, 0);
    
    // Read acknoledgement - don't need to save information
    recv(sock, &ret1, 4, 0);
    recv(sock, &ret2, 4, 0);
    
    // Main loop for sending and receiving data 
    FILE *file;
    file = fopen(args.filename, "rb");
    if (file == NULL){
      printf("Error opening file\n");    
    }

    for (int i=0;i<args.hashnum;i++){
      
      // Read in bytes from file 
      int file_buffer_size = args.smin + rand() % (args.smax - args.smin + 1);
      char *file_buffer = (char *)malloc(file_buffer_size);
      fread(file_buffer, 1, file_buffer_size, file);

      // Send HashRequests
      type = htonl(3);
      uint32_t length = htonl((uint32_t)file_buffer_size);
      send(sock, &type, 4, 0);
      send(sock, &length, 4, 0);
      send(sock, file_buffer, file_buffer_size, 0);

      // Read and print out HashResponses
      uint32_t curr_buf;
      recv(sock, &curr_buf, 4, 0);
      recv(sock, &curr_buf, 4, 0);

      unsigned char hash_response[32];
      recv(sock, hash_response, 32, 0);
      
      printf("%d: 0x", i+1);

      for (size_t i=0;i<32;i++){
        printf("%02x", hash_response[i]);
      }
      printf("\n");
      free(file_buffer);
    }

    fclose(file);
    close(sock);
    free(args.filename);
    return 0;
}
