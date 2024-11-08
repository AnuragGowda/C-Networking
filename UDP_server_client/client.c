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

#pragma pack(push, 1)
struct client_arguments {
    char ip_address[16];
    int port;
    int request_count;
    int time;
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

error_t client_parser(int key, char *arg, struct argp_state *state) {
    struct client_arguments *args = state->input;
    error_t ret = 0;
    switch(key) {
    case 'a':
        strncpy(args->ip_address, arg, sizeof(args->ip_address) - 1);
        args->ip_address[sizeof(args->ip_address) - 1] = '\0'; 
        break;
    case 'p':
        args->port = atoi(arg);
        if (args->port <= 0 || args->port > 65535) {
            argp_error(state, "Invalid port number, must be between 1 and 65535");
        }
        break;
    case 'n':
        args->request_count = atoi(arg);
        if (args->request_count <= 0) {
            argp_error(state, "Request count must be positive");
        }
        break;
    case 't':
        args->time = atoi(arg);
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

void client_parseopt(int argc, char *argv[], struct client_arguments *args) {
    struct argp_option options[] = {
        { "addr", 'a', "addr", 0, "The IP address the server is listening at", 0},
        { "port", 'p', "port", 0, "The port that is being used at the server", 0},
        { "req_count", 'n', "req_count", 0, "The number of requests to send to the server", 0},
        { "time", 't', "time", 0, "The timeout", 0},
        { "condensed", OPT_CONDENSED, 0, 0, "Use condensed mode (2-byte version field)", 0},
        {0}
    };

    struct argp argp_settings = { options, client_parser, 0, 0, 0, 0, 0 };

    bzero(args, sizeof(*args));

    if (argp_parse(&argp_settings, argc, argv, 0, NULL, args) != 0) {
        printf("Got error in parse\n");
    }
}

int main(int argc, char *argv[]) {
    struct client_arguments args;
    client_parseopt(argc, argv, &args);

    struct addrinfo address_criteria;
    memset(&address_criteria, 0, sizeof(address_criteria));
    address_criteria.ai_family = AF_UNSPEC;
    address_criteria.ai_socktype = SOCK_DGRAM;
    address_criteria.ai_protocol = IPPROTO_UDP;

    char port_str[6];
    snprintf(port_str, sizeof(port_str), "%d", args.port);

    struct addrinfo *server_address;
    int rtnVal = getaddrinfo(args.ip_address, port_str, &address_criteria, &server_address);
    if (rtnVal != 0) {
        fprintf(stderr, "getaddrinfo() failed: %s\n", gai_strerror(rtnVal));
        return 1;
    }

    int sock = socket(server_address->ai_family, server_address->ai_socktype, server_address->ai_protocol);
    if (sock < 0) {
        perror("socket() failed");
        freeaddrinfo(server_address);
        return 1;
    }

    for (int i = 0; i < args.request_count; i++) {
        if (args.condensed) {
            // Condensed mode with 2-byte version field
            CondensedTimeRequest request;
            struct timespec current_time;
            clock_gettime(CLOCK_REALTIME, &current_time);

            request.sequence_number = htonl(i + 1);
            request.version = htons(7);  
            request.client_seconds = htobe64(current_time.tv_sec);
            request.client_nanoseconds = htobe64(current_time.tv_nsec);

            sendto(sock, &request, sizeof(request), 0, server_address->ai_addr, server_address->ai_addrlen);
        } else {
            // Normal mode with 4-byte version field
            TimeRequest request;
            struct timespec current_time;
            clock_gettime(CLOCK_REALTIME, &current_time);

            request.sequence_number = htonl(i + 1);
            request.version = htonl(7);  // Example version value
            request.client_seconds = htobe64(current_time.tv_sec);
            request.client_nanoseconds = htobe64(current_time.tv_nsec);

            sendto(sock, &request, sizeof(request), 0, server_address->ai_addr, server_address->ai_addrlen);
        }
    }

    double *time_offsets = malloc(args.request_count * sizeof(double));
    double *round_trip_delays = malloc(args.request_count * sizeof(double));
    int *received = calloc(args.request_count, sizeof(int));

    if (!time_offsets || !round_trip_delays || !received) {
        fprintf(stderr, "Memory allocation failed\n");
        free(time_offsets);
        free(round_trip_delays);
        free(received);
        freeaddrinfo(server_address);
        close(sock);
        return 1;
    }

    for (int i = 0; i < args.request_count; i++) {
        fd_set read_fds;
        FD_ZERO(&read_fds);
        FD_SET(sock, &read_fds);

        struct timeval timeout;
        timeout.tv_sec = args.time;
        timeout.tv_usec = 0;

        int ready = select(sock + 1, &read_fds, NULL, NULL, &timeout);
        if (ready == 0) {
            break;
        }

        if (args.condensed) {
            CondensedTimeResponse response;
            struct sockaddr_storage from_address;
            socklen_t from_address_length = sizeof(from_address);
            recvfrom(sock, &response, sizeof(response), 0, (struct sockaddr *)&from_address, &from_address_length);
            
            response.sequence_number = ntohl(response.sequence_number);
            response.version = ntohs(response.version);
            response.client_seconds = be64toh(response.client_seconds);
            response.client_nanoseconds = be64toh(response.client_nanoseconds);
            response.server_seconds = be64toh(response.server_seconds);
            response.server_nanoseconds = be64toh(response.server_nanoseconds);

            struct timespec current_time;
            clock_gettime(CLOCK_REALTIME, &current_time);

            uint64_t current_time_ns = current_time.tv_sec * 1e9 + current_time.tv_nsec;
            uint64_t server_time_ns = response.server_seconds * 1e9 + response.server_nanoseconds;
            uint64_t client_request_time_ns = response.client_seconds * 1e9 + response.client_nanoseconds;
            uint64_t time_offset_ns = ((server_time_ns - client_request_time_ns) + (server_time_ns - current_time_ns)) / 2;
            uint64_t round_trip_delay_ns = current_time_ns - client_request_time_ns;

            int seq_index = response.sequence_number - 1;
            if (seq_index >= 0 && seq_index < args.request_count) {
                time_offsets[seq_index] = time_offset_ns / 1e9;
                round_trip_delays[seq_index] = round_trip_delay_ns / 1e9;
                received[seq_index] = 1;
            }
        } else {
            TimeResponse response;
            struct sockaddr_storage from_address;
            socklen_t from_address_length = sizeof(from_address);
            recvfrom(sock, &response, sizeof(response), 0, (struct sockaddr *)&from_address, &from_address_length);

            response.sequence_number = ntohl(response.sequence_number);
            response.version = ntohl(response.version);
            response.client_seconds = be64toh(response.client_seconds);
            response.client_nanoseconds = be64toh(response.client_nanoseconds);
            response.server_seconds = be64toh(response.server_seconds);
            response.server_nanoseconds = be64toh(response.server_nanoseconds);

            struct timespec current_time;
            clock_gettime(CLOCK_REALTIME, &current_time);

            uint64_t current_time_ns = current_time.tv_sec * 1e9 + current_time.tv_nsec;
            uint64_t server_time_ns = response.server_seconds * 1e9 + response.server_nanoseconds;
            uint64_t client_request_time_ns = response.client_seconds * 1e9 + response.client_nanoseconds;
            uint64_t time_offset_ns = ((server_time_ns - client_request_time_ns) + (server_time_ns - current_time_ns)) / 2;
            uint64_t round_trip_delay_ns = current_time_ns - client_request_time_ns;

            int seq_index = response.sequence_number - 1;
            if (seq_index >= 0 && seq_index < args.request_count) {
                time_offsets[seq_index] = time_offset_ns / 1e9;
                round_trip_delays[seq_index] = round_trip_delay_ns / 1e9;
                received[seq_index] = 1;
            }
        }
    }

    for (int i = 0; i < args.request_count; i++) {
        printf("%d: ", i + 1);
        if (received[i]) {
            printf("%.4f %.4f\n", time_offsets[i], round_trip_delays[i]);
        } else {
            printf("Dropped\n");
        }
    }

    free(time_offsets);
    free(round_trip_delays);
    free(received);
    freeaddrinfo(server_address);
    close(sock);
    return 0;
}
