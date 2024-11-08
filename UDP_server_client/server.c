#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <argp.h>
#include <unistd.h>
#include <time.h>
#include <sys/socket.h>
#include <netdb.h>
#include <inttypes.h>
#include <sys/select.h>
#include <arpa/inet.h>

#pragma pack(push, 1)
struct server_arguments {
    char ip_address[16];
    int port;
    int droprate;
    int condensed; 
};

typedef struct {
    uint32_t sequence_number;
    uint32_t version;
    uint64_t client_seconds;
    uint64_t client_nanoseconds;
} TimeRequest;

typedef struct {
    uint32_t sequence_number;
    uint32_t version;
    uint64_t client_seconds;
    uint64_t client_nanoseconds;
    uint64_t server_seconds;
    uint64_t server_nanoseconds;
} TimeResponse;

typedef struct {
    uint32_t sequence_number;
    uint16_t version;  
    uint64_t client_seconds;
    uint64_t client_nanoseconds;
} CondensedTimeRequest;

typedef struct {
    uint32_t sequence_number;
    uint16_t version;
    uint64_t client_seconds;
    uint64_t client_nanoseconds;
    uint64_t server_seconds;
    uint64_t server_nanoseconds;
} CondensedTimeResponse;
#pragma pack(pop)

// Define a unique key for the condensed option
#define OPT_CONDENSED 1000

error_t server_parser(int key, char *arg, struct argp_state *state) {
    struct server_arguments *args = state->input;
    error_t ret = 0;
    switch(key) {
    case 'p':
        args->port = atoi(arg);
        if (args->port <= 1024 || args->port > 65535) {
            argp_error(state, "Invalid port number, must be between 1025 and 65535");
        }
        break;
    case 'd':
        args->droprate = atoi(arg);
        if (args->droprate < 0 || args->droprate > 100) {
            argp_error(state, "Invalid drop rate, must be between 0 and 100");
        }
        break;
    case OPT_CONDENSED:  
        args->condensed = 1;  
        break;
    default:
        ret = ARGP_ERR_UNKNOWN;
        break;
    }
    return ret;
}

void server_parseopt(int argc, char *argv[], struct server_arguments *args) {
    struct argp_option options[] = {
        { "port", 'p', "port", 0, "The port that is being used at the server", 0},
        { "drop", 'd', "drop", 0, "The droprate (0-100 percent)", 0},
        { "condensed", OPT_CONDENSED, 0, 0, "Use condensed mode (2-byte version field)", 0},
        {0}
    };

    struct argp argp_settings = { options, server_parser, 0, 0, 0, 0, 0 };

    bzero(args, sizeof(*args));

    if (argp_parse(&argp_settings, argc, argv, 0, NULL, args) != 0) {
        printf("Got error in parse\n");
    }
}

typedef struct ClientInfo {
    struct sockaddr_in client_addr; // IPv4 address
    uint32_t highest_sequence_number;
    time_t last_update_time;
    struct ClientInfo *next;
} ClientInfo;

ClientInfo *find_client(ClientInfo *list, struct sockaddr_in *client_addr) {
    ClientInfo *current = list;
    while (current != NULL) {
        if (current->client_addr.sin_addr.s_addr == client_addr->sin_addr.s_addr &&
            current->client_addr.sin_port == client_addr->sin_port) {
            return current;
        }
        current = current->next;
    }
    return NULL;
}

int main(int argc, char *argv[]) {
    struct server_arguments args;
    server_parseopt(argc, argv, &args);

    struct addrinfo address_criteria;
    memset(&address_criteria, 0, sizeof(address_criteria));
    address_criteria.ai_family = AF_INET;           
    address_criteria.ai_flags = AI_PASSIVE;         
    address_criteria.ai_socktype = SOCK_DGRAM;      
    address_criteria.ai_protocol = IPPROTO_UDP;     

    struct addrinfo *server_address;
    char port_str[6];
    snprintf(port_str, sizeof(port_str), "%d", args.port);
    if (getaddrinfo(NULL, port_str, &address_criteria, &server_address) != 0) {
        perror("getaddrinfo() failed");
        return 1;
    }

    // Create socket
    int sock = socket(server_address->ai_family, server_address->ai_socktype, server_address->ai_protocol);
    if (sock < 0) {
        perror("socket() failed");
        freeaddrinfo(server_address);
        return 1;
    }

    // Bind to local address
    if (bind(sock, server_address->ai_addr, server_address->ai_addrlen) < 0) {
        perror("bind() failed");
        freeaddrinfo(server_address);
        close(sock);
        return 1;
    }

    freeaddrinfo(server_address);  

    // Seed the random number generator
    srand(time(NULL));

    ClientInfo *client_list = NULL;

    for (;;) {
        if (args.condensed) {
            // Condensed mode with 2-byte version field
            CondensedTimeRequest request;
            struct sockaddr_storage client_address;
            socklen_t client_address_len = sizeof(client_address);

            ssize_t num_bytes = recvfrom(sock, &request, sizeof(request), 0,
                                         (struct sockaddr *)&client_address, &client_address_len);
            if (num_bytes < 0) {
                perror("recvfrom() failed");
                continue;
            }

            request.sequence_number = ntohl(request.sequence_number);
            request.version = ntohs(request.version);
            request.client_seconds = be64toh(request.client_seconds);
            request.client_nanoseconds = be64toh(request.client_nanoseconds);

            // Randomly decide whether to drop the request based on droprate
            if (args.droprate > 0 && (rand() % 100) < args.droprate) {
                // Packet is dropped, do not process further
                continue;
            }

            // Extract client's IP and port
            struct sockaddr_in *client_addr_in = (struct sockaddr_in *)&client_address;
            char client_ip[INET_ADDRSTRLEN];
            inet_ntop(AF_INET, &client_addr_in->sin_addr, client_ip, INET_ADDRSTRLEN);
            uint16_t client_port = ntohs(client_addr_in->sin_port);

            // Handle client info
            ClientInfo *client = find_client(client_list, client_addr_in);
            time_t current_time_sec = time(NULL);

            if (client != NULL) {
                // Client found
                if (difftime(current_time_sec, client->last_update_time) >= 120) {
                    // Reset highest_sequence_number
                    client->highest_sequence_number = request.sequence_number;
                    client->last_update_time = current_time_sec;
                } else {
                    if (request.sequence_number < client->highest_sequence_number) {
                        // Print as specified
                        printf("%s:%u %u %u\n", client_ip, client_port,
                               request.sequence_number, client->highest_sequence_number);
                    }
                    if (request.sequence_number > client->highest_sequence_number) {
                        // Update highest_sequence_number
                        client->highest_sequence_number = request.sequence_number;
                        client->last_update_time = current_time_sec;
                    }
                }
            } else {
                // Client not found, add to client_list
                ClientInfo *new_client = malloc(sizeof(ClientInfo));
                if (new_client == NULL) {
                    perror("malloc failed");
                    continue;
                }
                new_client->client_addr = *client_addr_in;
                new_client->highest_sequence_number = request.sequence_number;
                new_client->last_update_time = current_time_sec;
                new_client->next = client_list;
                client_list = new_client;
            }

            // Get current time
            struct timespec current_time;
            clock_gettime(CLOCK_REALTIME, &current_time);

            // Prepare condensed response
            CondensedTimeResponse response;
            response.sequence_number = htonl(request.sequence_number);
            response.version = htons(request.version);
            response.client_seconds = htobe64(request.client_seconds);
            response.client_nanoseconds = htobe64(request.client_nanoseconds);
            response.server_seconds = htobe64(current_time.tv_sec);
            response.server_nanoseconds = htobe64(current_time.tv_nsec);

            // Send response back to the client
            ssize_t sent_bytes = sendto(sock, &response, sizeof(response), 0,
                                        (struct sockaddr *)&client_address, client_address_len);
            if (sent_bytes < 0) {
                perror("sendto() failed");
            }
        } else {
            // Normal mode with 4-byte version field
            TimeRequest request;
            struct sockaddr_storage client_address;
            socklen_t client_address_len = sizeof(client_address);

            ssize_t num_bytes = recvfrom(sock, &request, sizeof(request), 0,
                                         (struct sockaddr *)&client_address, &client_address_len);
            if (num_bytes < 0) {
                perror("recvfrom() failed");
                continue;
            }

            request.sequence_number = ntohl(request.sequence_number);
            request.version = ntohl(request.version);
            request.client_seconds = be64toh(request.client_seconds);
            request.client_nanoseconds = be64toh(request.client_nanoseconds);

            // Randomly decide whether to drop the request based on droprate
            if (args.droprate > 0 && (rand() % 100) < args.droprate) {
                // Packet is dropped, do not process further
                continue;
            }

            // Extract client's IP and port
            struct sockaddr_in *client_addr_in = (struct sockaddr_in *)&client_address;
            char client_ip[INET_ADDRSTRLEN];
            inet_ntop(AF_INET, &client_addr_in->sin_addr, client_ip, INET_ADDRSTRLEN);
            uint16_t client_port = ntohs(client_addr_in->sin_port);

            // Handle client info
            ClientInfo *client = find_client(client_list, client_addr_in);
            time_t current_time_sec = time(NULL);

            if (client != NULL) {
                // Client found
                if (difftime(current_time_sec, client->last_update_time) >= 120) {
                    // Reset highest_sequence_number
                    client->highest_sequence_number = request.sequence_number;
                    client->last_update_time = current_time_sec;
                } else {
                    if (request.sequence_number < client->highest_sequence_number) {
                        // Print as specified
                        printf("%s:%u %u %u\n", client_ip, client_port,
                               request.sequence_number, client->highest_sequence_number);
                    }
                    if (request.sequence_number > client->highest_sequence_number) {
                        // Update highest_sequence_number
                        client->highest_sequence_number = request.sequence_number;
                        client->last_update_time = current_time_sec;
                    }
                }
            } else {
                // Client not found, add to client_list
                ClientInfo *new_client = malloc(sizeof(ClientInfo));
                if (new_client == NULL) {
                    perror("malloc failed");
                    continue;
                }
                new_client->client_addr = *client_addr_in;
                new_client->highest_sequence_number = request.sequence_number;
                new_client->last_update_time = current_time_sec;
                new_client->next = client_list;
                client_list = new_client;
            }

            // Get current time
            struct timespec current_time;
            clock_gettime(CLOCK_REALTIME, &current_time);

            // Prepare response
            TimeResponse response;
            response.sequence_number = htonl(request.sequence_number);
            response.version = htonl(request.version);
            response.client_seconds = htobe64(request.client_seconds);
            response.client_nanoseconds = htobe64(request.client_nanoseconds);
            response.server_seconds = htobe64(current_time.tv_sec);
            response.server_nanoseconds = htobe64(current_time.tv_nsec);

            // Send response back to the client
            ssize_t sent_bytes = sendto(sock, &response, sizeof(response), 0,
                                        (struct sockaddr *)&client_address, client_address_len);
            if (sent_bytes < 0) {
                perror("sendto() failed");
            }
        }
    }

    close(sock);
    return 0;
}
