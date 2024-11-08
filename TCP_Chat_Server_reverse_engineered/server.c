#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <getopt.h>
#include <errno.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <signal.h>
#include <sys/select.h>
#include <time.h>

#define MAX_CLIENTS 256
#define MAX_ROOMS 100
#define MAX_MESSAGE_SIZE 65801 // 7 + 1 + 256 + 2 + 65535

#define MESSAGE_HEADER_SIZE 7

// Define message types - incoming from client
#define MSG_TYPE_INIT_CONN    0x9b
#define MSG_TYPE_KEEP_ALIVE   0x13
#define MSG_TYPE_JOIN_ROOM    0x03
#define MSG_TYPE_LEAVE        0x06
#define MSG_TYPE_LIST_USERS   0x0c
#define MSG_TYPE_LIST_ROOMS   0x09
#define MSG_TYPE_SEND_USER    0x12
#define MSG_TYPE_SEND_ROOM    0x15
#define MSG_TYPE_CHANGE_NICK  0x0f
#define MSG_TYPE_SERVER_RESP  0x9a

// Status codes
#define STATUS_SUCCESS        0x00
#define STATUS_ERROR          0x01

typedef struct {
    int socket;
    char username[256];
    char room[256];
    time_t last_activity;          // For tracking inactivity
    time_t message_timestamps[5];  // For rate limiting
} Client;

typedef struct {
    char name[256];
    char password[256];
} Room;

typedef struct {
    uint32_t length;       // Payload length excluding header
    uint16_t version;      // Version or magic bytes (e.g., 0x0417)
    uint8_t type;          // Message type
    uint8_t payload[];     // Payload data
} __attribute__((packed)) Message;

/* Function Prototypes */
void send_message(int socket, uint8_t message_type, uint8_t status_code, const uint8_t *payload, uint32_t payload_length);
void send_user_message(int socket, const char *sender_username, const char *message);
void send_room_message(int socket, const char *room_name, const char *sender_username, const char *message);
void broadcast_room_message(const char *room, const char *sender_username, const char *message, int exclude_socket);
Client *get_client_by_username(const char *username);
Client *get_client_by_socket(int socket);
Room *get_room_by_name(const char *room_name);
Room *create_room(const char *room_name, const char *password);
void remove_client(int socket);
void remove_room(const char *room_name);
void handle_client_message(Client *client, Message *msg);
void handle_init_conn(Client *client, Message *msg);
void handle_join_room(Client *client, Message *msg);
void handle_leave(Client *client);
void handle_list_users(Client *client);
void handle_list_rooms(Client *client);
void handle_send_msg(Client *client, Message *msg);
void handle_change_nick(Client *client, Message *msg);
void handle_room_message(Client *client, Message *msg);
void print_usage(const char *prog_name);
int is_valid_name(const char *name);

/* Global Variables */
Client *clients[MAX_CLIENTS];
Room rooms[MAX_ROOMS];
int room_count = 0;
fd_set master_set;  // Global master file descriptor set

int main(int argc, char *argv[]) {
    int opt;
    int port = 0;

    // Ignore SIGPIPE to handle client disconnections
    signal(SIGPIPE, SIG_IGN);

    // Parse command-line options
    while ((opt = getopt(argc, argv, "p:")) != -1) {
        switch (opt) {
            case 'p':
                port = atoi(optarg);
                break;
            default:
                print_usage(argv[0]);
                exit(EXIT_FAILURE);
        }
    }

    if (port == 0) {
        fprintf(stderr, "Port number is required.\n");
        print_usage(argv[0]);
        exit(EXIT_FAILURE);
    }

    // Create socket
    int server_socket;
    if ((server_socket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP)) < 0) {
        perror("socket() failed");
        exit(EXIT_FAILURE);
    }

    // Set socket options to reuse address
    int optval = 1;
    setsockopt(server_socket, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval));

    // Local address
    struct sockaddr_in server_address;
    memset(&server_address, 0, sizeof(server_address));
    server_address.sin_family = AF_INET;
    server_address.sin_addr.s_addr = htonl(INADDR_ANY);
    server_address.sin_port = htons(port);

    // Bind to local address
    if (bind(server_socket, (struct sockaddr *)&server_address, sizeof(server_address)) < 0) {
        perror("bind() failed");
        exit(EXIT_FAILURE);
    }

    // Mark socket to listen for incoming connections
    if (listen(server_socket, MAX_CLIENTS) < 0) {
        perror("listen() failed");
        exit(EXIT_FAILURE);
    }

    printf("Server listening on port %d\n", port);

    // Initialize client list
    for (int i = 0; i < MAX_CLIENTS; ++i) {
        clients[i] = NULL;
    }

    int fdmax = server_socket;

    FD_ZERO(&master_set);
    FD_SET(server_socket, &master_set);

    while (1) {
        fd_set read_fds = master_set;

        // No timeout, block indefinitely
        if (select(fdmax + 1, &read_fds, NULL, NULL, NULL) == -1) {
            perror("select() failed");
            exit(EXIT_FAILURE);
        }

        for (int i = 0; i <= fdmax; ++i) {
            if (FD_ISSET(i, &read_fds)) {
                if (i == server_socket) {
                    // New client connection
                    struct sockaddr_in client_address;
                    socklen_t client_address_length = sizeof(client_address);

                    int client_socket = accept(server_socket, (struct sockaddr *)&client_address, &client_address_length);
                    if (client_socket < 0) {
                        perror("accept() failed");
                        continue;
                    }

                    // Add new client to the master set
                    FD_SET(client_socket, &master_set);
                    if (client_socket > fdmax) {
                        fdmax = client_socket;
                    }

                    // Initialize client
                    Client *client = (Client *)malloc(sizeof(Client));
                    if (client == NULL) {
                        perror("malloc failed");
                        close(client_socket);
                        continue;
                    }
                    client->socket = client_socket;
                    client->username[0] = '\0';
                    client->room[0] = '\0';
                    client->last_activity = time(NULL);
                    memset(client->message_timestamps, 0, sizeof(client->message_timestamps));

                    // Add client to clients array
                    int added = 0;
                    for (int j = 0; j < MAX_CLIENTS; ++j) {
                        if (clients[j] == NULL) {
                            clients[j] = client;
                            added = 1;
                            break;
                        }
                    }
                    if (!added) {
                        fprintf(stderr, "Max clients reached, rejecting client on socket %d\n", client_socket);
                        free(client);
                        close(client_socket);
                        continue;
                    }

                    printf("New connection on socket %d\n", client_socket);
                } else {
                    // Handle data from client
                    Client *client = get_client_by_socket(i);
                    if (client == NULL) {
                        fprintf(stderr, "Client on socket %d not found\n", i);
                        close(i);
                        FD_CLR(i, &master_set);
                        continue;
                    }

                    // Read the 4-byte length field
                    uint32_t length_net;
                    ssize_t nbytes = recv(i, &length_net, sizeof(length_net), MSG_WAITALL);
                    if (nbytes <= 0) {
                        // Client disconnected or error
                        if (nbytes == 0) {
                            printf("Socket %d disconnected\n", i);
                        } else {
                            perror("recv() failed");
                        }
                        close(i);
                        FD_CLR(i, &master_set);
                        remove_client(i);
                        continue;
                    }

                    if (nbytes < sizeof(length_net)) {
                        fprintf(stderr, "Incomplete length field received from socket %d\n", i);
                        close(i);
                        FD_CLR(i, &master_set);
                        remove_client(i);
                        continue;
                    }

                    uint32_t length = ntohl(length_net);

                    // Validate length
                    if (length > MAX_MESSAGE_SIZE) {
                        fprintf(stderr, "Received message with invalid length %u from socket %d\n", length, i);
                        close(i);
                        FD_CLR(i, &master_set);
                        remove_client(i);
                        continue;
                    }

                    // Read the rest of the header (2 bytes version, 1 byte message_type)
                    uint8_t header_buffer[3];
                    nbytes = recv(i, header_buffer, sizeof(header_buffer), MSG_WAITALL);
                    if (nbytes <= 0) {
                        // Client disconnected or error
                        if (nbytes == 0) {
                            printf("Socket %d disconnected\n", i);
                        } else {
                            perror("recv() failed");
                        }
                        close(i);
                        FD_CLR(i, &master_set);
                        remove_client(i);
                        continue;
                    }

                    if (nbytes < sizeof(header_buffer)) {
                        fprintf(stderr, "Incomplete header received from socket %d\n", i);
                        close(i);
                        FD_CLR(i, &master_set);
                        remove_client(i);
                        continue;
                    }

                    uint16_t version_net;
                    memcpy(&version_net, header_buffer, sizeof(version_net));
                    uint16_t version = ntohs(version_net);

                    uint8_t message_type = header_buffer[2];

                    // Read the payload if length > 0
                    uint8_t *payload = NULL;
                    if (length > 0) {
                        payload = malloc(length);
                        if (payload == NULL) {
                            perror("malloc failed");
                            close(i);
                            FD_CLR(i, &master_set);
                            remove_client(i);
                            continue;
                        }

                        nbytes = recv(i, payload, length, MSG_WAITALL);
                        if (nbytes <= 0) {
                            // Client disconnected or error
                            if (nbytes == 0) {
                                printf("Socket %d disconnected\n", i);
                            } else {
                                perror("recv() failed");
                            }
                            free(payload);
                            close(i);
                            FD_CLR(i, &master_set);
                            remove_client(i);
                            continue;
                        }

                        if ((uint32_t)nbytes < length) {
                            fprintf(stderr, "Incomplete payload received from socket %d\n", i);
                            free(payload);
                            close(i);
                            FD_CLR(i, &master_set);
                            remove_client(i);
                            continue;
                        }
                    }

                    // Now we have the full message
                    Message *msg = (Message *)malloc(sizeof(Message) + length);
                    if (msg == NULL) {
                        perror("malloc failed");
                        if (payload) free(payload);
                        close(i);
                        FD_CLR(i, &master_set);
                        remove_client(i);
                        continue;
                    }

                    msg->length = length;
                    msg->version = version;
                    msg->type = message_type;
                    if (length > 0) {
                        memcpy(msg->payload, payload, length);
                    }

                    if (payload) free(payload);

                    // Update last_activity timestamp
                    client->last_activity = time(NULL);

                    // Process the message
                    handle_client_message(client, msg);

                    free(msg);
                }
            }
        }

        // After handling all sockets, check for inactive clients
        time_t current_time = time(NULL);
        for (int j = 0; j < MAX_CLIENTS; ++j) {
            if (clients[j]) {
                if (difftime(current_time, clients[j]->last_activity) >= 30) {
                    printf("Client on socket %d inactive for 30 seconds. Disconnecting.\n", clients[j]->socket);
                    close(clients[j]->socket);
                    FD_CLR(clients[j]->socket, &master_set);
                    remove_client(clients[j]->socket);
                }
            }
        }
    }

    close(server_socket);
    return 0;
}

Client *get_client_by_socket(int socket) {
    for (int i = 0; i < MAX_CLIENTS; ++i) {
        if (clients[i] != NULL && clients[i]->socket == socket) {
            return clients[i];
        }
    }
    return NULL;
}

void handle_client_message(Client *client, Message *msg) {

    // Handle message based on type
    switch (msg->type) {
        case MSG_TYPE_INIT_CONN:
            handle_init_conn(client, msg);
            break;
        case MSG_TYPE_KEEP_ALIVE:
            // No action needed for keep-alive
            break;
        case MSG_TYPE_JOIN_ROOM:
            handle_join_room(client, msg);
            break;
        case MSG_TYPE_LEAVE:
            handle_leave(client);
            break;
        case MSG_TYPE_LIST_USERS:
            handle_list_users(client);
            break;
        case MSG_TYPE_LIST_ROOMS:
            handle_list_rooms(client);
            break;
        case MSG_TYPE_SEND_USER:
            handle_send_msg(client, msg);
            break;
        case MSG_TYPE_CHANGE_NICK:
            handle_change_nick(client, msg);
            break;
        case MSG_TYPE_SEND_ROOM:
            handle_room_message(client, msg);
            break;
        default:
            fprintf(stderr, "Unknown message type: %02x\n", msg->type);
            break;
    }
}

void handle_init_conn(Client *client, Message *msg) {
    // Assign a unique username (e.g., "rand0")
    int user_count = 0;
    char username[256];

    while (1) {
        snprintf(username, sizeof(username), "rand%d", user_count++);
        // Check if username is taken
        int taken = 0;
        for (int i = 0; i < MAX_CLIENTS; ++i) {
            if (clients[i] && strcmp(clients[i]->username, username) == 0) {
                taken = 1;
                break;
            }
        }
        if (!taken) {
            break;
        }
    }

    strcpy(client->username, username);

    // Send server response with assigned username
    send_message(client->socket, MSG_TYPE_SERVER_RESP, STATUS_SUCCESS, (uint8_t *)username, strlen(username));

    printf("Client on socket %d assigned username: %s\n", client->socket, username);
}

int is_valid_name(const char *name) {
    for (int i = 0; name[i] != '\0'; ++i) {
        if ((unsigned char)name[i] < 32 || (unsigned char)name[i] > 126) {
            return 0; // Invalid character found
        }
    }
    return 1; // All characters are valid
}

void handle_join_room(Client *client, Message *msg) {
    uint8_t *payload = msg->payload;

    uint8_t room_name_len = payload[0];

    char room_name[256];
    memcpy(room_name, &payload[1], room_name_len);
    room_name[room_name_len] = '\0';

    // Check if room name contains only valid ASCII characters
    if (!is_valid_name(room_name)) {
        const char *error_msg = "Protocol only allows pedestrians. Get in the bike lane or something.";
        send_message(client->socket, MSG_TYPE_SERVER_RESP, STATUS_ERROR, (uint8_t *)error_msg, strlen(error_msg));
        close(client->socket);
        FD_CLR(client->socket, &master_set);
        remove_client(client->socket);
        return;
    }

    uint8_t password_len = payload[1 + room_name_len];

    char password[256];
    memcpy(password, &payload[1 + room_name_len + 1], password_len);
    password[password_len] = '\0';

    // Check if room exists
    Room *room = get_room_by_name(room_name);
    if (room == NULL) {
        // Room doesn't exist, create it
        room = create_room(room_name, password);
        strcpy(client->room, room_name);
        send_message(client->socket, MSG_TYPE_SERVER_RESP, STATUS_SUCCESS, NULL, 0);
        printf("%s created and joined room: %s\n", client->username, client->room);
    } else {
        // Room exists, check password
        if (strcmp(room->password, password) == 0) {
            // Correct password
            strcpy(client->room, room_name);
            send_message(client->socket, MSG_TYPE_SERVER_RESP, STATUS_SUCCESS, NULL, 0);
            printf("%s joined room: %s\n", client->username, client->room);
        } else {
            // Incorrect password
            const char *error_msg = "Invalid Password. Get off that damn scoobert and touch some grass. Then ask the room manager for the password.";
            send_message(client->socket, MSG_TYPE_SERVER_RESP, STATUS_ERROR, (uint8_t *)error_msg, strlen(error_msg));
        }
    }
}

void handle_leave(Client *client) {
    if (client->room[0] != '\0') {
        // Client is in a room, leave the room
        printf("%s left room: %s\n", client->username, client->room);

        // Check if any other clients are in the room
        int room_empty = 1;
        for (int j = 0; j < MAX_CLIENTS; ++j) {
            if (clients[j] && strcmp(clients[j]->room, client->room) == 0 && clients[j]->socket != client->socket) {
                room_empty = 0;
                break;
            }
        }

        if (room_empty) {
            remove_room(client->room);
        }

        client->room[0] = '\0';  // Clear room
        send_message(client->socket, MSG_TYPE_SERVER_RESP, STATUS_SUCCESS, NULL, 0);
    } else {
        // Client is not in a room, disconnect them
        printf("%s is not in a room, disconnecting client.\n", client->username);
        send_message(client->socket, MSG_TYPE_SERVER_RESP, STATUS_SUCCESS, NULL, 0);
        close(client->socket);
        FD_CLR(client->socket, &master_set);
        remove_client(client->socket);
    }
}

void remove_room(const char *room_name) {
    for (int i = 0; i < room_count; ++i) {
        if (strcmp(rooms[i].name, room_name) == 0) {
            // Shift the remaining rooms
            for (int j = i; j < room_count - 1; ++j) {
                rooms[j] = rooms[j + 1];
            }
            room_count--;
            printf("Room '%s' deleted because it is empty.\n", room_name);
            break;
        }
    }
}

void handle_list_users(Client *client) {
    // Compile a list of usernames
    uint8_t user_list[65792]; // max clients * (1 + max client name len)
    size_t offset = 0;

    for (int j = 0; j < MAX_CLIENTS; j++) {
        if (clients[j] != NULL) {
            if (client->room[0] == '\0' || strcmp(clients[j]->room, client->room) == 0) {
                uint8_t uname_len = strlen(clients[j]->username);
                user_list[offset++] = uname_len;
                memcpy(&user_list[offset], clients[j]->username, uname_len);
                offset += uname_len;
            }
        }
    }

    send_message(client->socket, MSG_TYPE_SERVER_RESP, STATUS_SUCCESS, user_list, offset);
}

void handle_list_rooms(Client *client) {
    // Compile a list of rooms
    uint8_t room_list[4096];
    size_t offset = 0;

    for (int j = 0; j < room_count; ++j) {
        uint8_t room_len = strlen(rooms[j].name);
        room_list[offset++] = room_len;
        memcpy(&room_list[offset], rooms[j].name, room_len);
        offset += room_len;
    }

    send_message(client->socket, MSG_TYPE_SERVER_RESP, STATUS_SUCCESS, room_list, offset);
}

void handle_send_msg(Client *client, Message *msg) {
    uint8_t *payload = msg->payload;
    uint32_t payload_length = msg->length;

    if (payload_length < 1) {
        // Not enough data
        return;
    }

    uint8_t recipient_len = payload[0];
    if (recipient_len > 255 || recipient_len == 0 || recipient_len + 1 > payload_length) {
        // Invalid recipient length
        return;
    }

    char recipient[256];
    memcpy(recipient, &payload[1], recipient_len);
    recipient[recipient_len] = '\0';

    if (payload_length < 1 + recipient_len + 2) {
        // Not enough data for message length
        return;
    }

    uint16_t message_len;
    memcpy(&message_len, &payload[1 + recipient_len], 2);
    message_len = ntohs(message_len);

    if (message_len > 65535 || 1 + recipient_len + 2 + message_len > payload_length) {
        // Invalid message length
        return;
    }

    if (message_len > 256) {
        const char *error_msg = "Length limit exceeded.";
        send_message(client->socket, MSG_TYPE_SERVER_RESP, STATUS_ERROR, (uint8_t *)error_msg, strlen(error_msg));
        close(client->socket);
        FD_CLR(client->socket, &master_set);
        remove_client(client->socket);
        return;
    }

    char message[257]; // Limit to 256 bytes
    memcpy(message, &payload[1 + recipient_len + 2], message_len);
    message[message_len] = '\0';

    // Find recipient and send the message
    Client *dest_client = get_client_by_username(recipient);
    if (dest_client) {
        // Send message to recipient
        send_user_message(dest_client->socket, client->username, message);
        // Also send back a confirmation to the sender
        send_message(client->socket, MSG_TYPE_SERVER_RESP, STATUS_SUCCESS, NULL, 0);
    } else {
        // Recipient not found
        const char *error_msg = "We couldn't find any scooters registered under that ID.";
        send_message(client->socket, MSG_TYPE_SERVER_RESP, STATUS_ERROR, (uint8_t *)error_msg, strlen(error_msg));
    }
}

void handle_change_nick(Client *client, Message *msg) {
    uint8_t *payload = msg->payload;
    uint32_t payload_length = msg->length;

    if (payload_length < 1) {
        // Not enough data
        return;
    }

    uint8_t nick_len = payload[0];
    if (nick_len > 255 || nick_len == 0 || nick_len + 1 > payload_length) {
        // Invalid nickname length
        return;
    }

    char new_nick[256];
    memcpy(new_nick, &payload[1], nick_len);
    new_nick[nick_len] = '\0';

    // Check if nickname contains only valid ASCII characters
    if (!is_valid_name(new_nick)) {
        const char *error_msg = "We tried to nick it but it caught fire during the heist.";
        send_message(client->socket, MSG_TYPE_SERVER_RESP, STATUS_ERROR, (uint8_t *)error_msg, strlen(error_msg));
        close(client->socket);
        FD_CLR(client->socket, &master_set);
        remove_client(client->socket);
        return;
    }

    // Check if nickname is already taken
    int nickname_taken = 0;
    for (int j = 0; j < MAX_CLIENTS; ++j) {
        if (clients[j] && strcmp(clients[j]->username, new_nick) == 0) {
            nickname_taken = 1;
            break;
        }
    }

    if (nickname_taken) {
        const char *error_msg = "Stop trying to nick it! Carl already paid good money for that scooter.";
        send_message(client->socket, MSG_TYPE_SERVER_RESP, STATUS_ERROR, (uint8_t *)error_msg, strlen(error_msg));
    } else {
        strcpy(client->username, new_nick);
        send_message(client->socket, MSG_TYPE_SERVER_RESP, STATUS_SUCCESS, NULL, 0);
        printf("Client on socket %d changed nickname to: %s\n", client->socket, client->username);
    }
}

void handle_room_message(Client *client, Message *msg) {
    if (client->room[0] == '\0') {
        // Not in a room
        const char *error_msg = "You gotta use the app in order to ride.";
        send_message(client->socket, MSG_TYPE_SERVER_RESP, STATUS_ERROR, (uint8_t *)error_msg, strlen(error_msg));
        return;
    }
    uint8_t *payload = msg->payload;
    uint32_t payload_length = msg->length;

    if (payload_length < 1) {
        // Not enough data for room name length
        return;
    }

    uint8_t room_name_len = payload[0];
    if (room_name_len == 0 || room_name_len > 255) {
        // Invalid room name length
        return;
    }

    if (payload_length < 1 + room_name_len + 2) {
        // Not enough data for room name and message length
        return;
    }

    char room_name[256];
    memcpy(room_name, &payload[1], room_name_len);
    room_name[room_name_len] = '\0';

    uint16_t message_len;
    memcpy(&message_len, &payload[1 + room_name_len], 2);
    message_len = ntohs(message_len);

    if (message_len > 65535 || payload_length < 1 + room_name_len + 2 + message_len) {
        // Invalid message length or not enough data
        return;
    }

    if (message_len > 256) {
        const char *error_msg = "Length limit exceeded.";
        send_message(client->socket, MSG_TYPE_SERVER_RESP, STATUS_ERROR, (uint8_t *)error_msg, strlen(error_msg));
        close(client->socket);
        FD_CLR(client->socket, &master_set);
        remove_client(client->socket);
        return;
    }

    char message[257]; // Limit to 256 bytes
    memcpy(message, &payload[1 + room_name_len + 2], message_len);
    message[message_len] = '\0';

    // Check if client is in the specified room
    if (strcmp(client->room, room_name) != 0) {
        // Client is not in the room
        const char *error_msg = "You are not in this room.";
        send_message(client->socket, MSG_TYPE_SERVER_RESP, STATUS_ERROR, (uint8_t *)error_msg, strlen(error_msg));
        return;
    }

    // Rate limiting logic
    time_t current_time = time(NULL);
    int messages_in_window = 0;
    
    // Count messages within the window
    for (int i = 0; i < 5; ++i) {
        if (current_time - client->message_timestamps[i] <= 3) {
            messages_in_window++;
        }
    }

    if (messages_in_window >= 5) {
        const char *error_msg = "Campus Drive speed limit is 10 MPH. Slow Down!";
        send_message(client->socket, MSG_TYPE_SERVER_RESP, STATUS_ERROR, (uint8_t *)error_msg, strlen(error_msg));
        return;
    } else {
        // Record the current message timestamp
        // Find the oldest timestamp to replace
        int oldest_index = 0;
        for (int i = 1; i < 5; ++i) {
            if (client->message_timestamps[i] < client->message_timestamps[oldest_index]) {
                oldest_index = i;
            }
        }
        client->message_timestamps[oldest_index] = current_time;
    }

    // Debug print
    printf("Received message from %s in room %s: %s\n", client->username, room_name, message);

    // Broadcast the message to all clients in the room
    broadcast_room_message(room_name, client->username, message, client->socket);

    send_message(client->socket, MSG_TYPE_SERVER_RESP, STATUS_SUCCESS, NULL, 0);
}

void send_message(int socket, uint8_t message_type, uint8_t status_code, const uint8_t *payload, uint32_t payload_length) {
    uint8_t buffer[MAX_MESSAGE_SIZE];
    size_t offset = 0;

    // Placeholder for length field
    offset += 4;

    // Version/magic bytes
    uint16_t version = htons(0x0417);
    memcpy(buffer + offset, &version, 2);
    offset += 2;

    // Message type
    buffer[offset++] = message_type;

    // Status code for server responses
    if (message_type == MSG_TYPE_SERVER_RESP) {
        buffer[offset++] = status_code;
    }

    // Payload
    if (payload_length > 0 && payload != NULL) {
        memcpy(buffer + offset, payload, payload_length);
        offset += payload_length;
    }

    // Now fill in the length field
    uint32_t total_length = offset - 7; // Excluding length field and header
    uint32_t net_total_length = htonl(total_length);
    memcpy(buffer, &net_total_length, 4);

    // Send the message
    if (send(socket, buffer, offset, 0) < 0) {
        perror("send() failed");
    }
}

void send_user_message(int socket, const char *sender_username, const char *message) {
    uint8_t buffer[MAX_MESSAGE_SIZE];
    size_t offset = 0;

    // Placeholder for length field
    offset += 4;

    // Version/magic bytes
    uint16_t version = htons(0x0417);
    memcpy(buffer + offset, &version, 2);
    offset += 2;

    // Message type
    buffer[offset++] = MSG_TYPE_SEND_USER;

    // Sender username length and username
    uint8_t sender_len = strlen(sender_username);
    buffer[offset++] = sender_len;
    memcpy(buffer + offset, sender_username, sender_len);
    offset += sender_len;

    // Message length and message
    uint16_t message_len = strlen(message);
    uint16_t net_message_len = htons(message_len);
    memcpy(buffer + offset, &net_message_len, 2);
    offset += 2;
    memcpy(buffer + offset, message, message_len);
    offset += message_len;

    // Now fill in the length field
    uint32_t total_length = offset - 7; // Excluding length field and header
    uint32_t net_total_length = htonl(total_length);
    memcpy(buffer, &net_total_length, 4);

    // Send the message
    if (send(socket, buffer, offset, 0) < 0) {
        perror("send() failed");
    }
}

void send_room_message(int socket, const char *room_name, const char *sender_username, const char *message) {
    uint8_t buffer[MAX_MESSAGE_SIZE];
    size_t offset = 0;

    // Placeholder for length field
    offset += 4;

    // Version/magic bytes
    uint16_t version = htons(0x0417);
    memcpy(buffer + offset, &version, 2);
    offset += 2;

    // Message type
    buffer[offset++] = MSG_TYPE_SEND_ROOM;

    // Room name length and name
    uint8_t room_len = strlen(room_name);
    buffer[offset++] = room_len;
    memcpy(buffer + offset, room_name, room_len);
    offset += room_len;

    // Sender username length and username
    uint8_t sender_len = strlen(sender_username);
    buffer[offset++] = sender_len;
    memcpy(buffer + offset, sender_username, sender_len);
    offset += sender_len;

    // Message length and message
    uint16_t message_len = strlen(message);
    uint16_t net_message_len = htons(message_len);
    memcpy(buffer + offset, &net_message_len, 2);
    offset += 2;
    memcpy(buffer + offset, message, message_len);
    offset += message_len;

    // Now fill in the length field
    uint32_t total_length = offset - 7; // Excluding length field and header
    uint32_t net_total_length = htonl(total_length);
    memcpy(buffer, &net_total_length, 4);

    // Send the message
    if (send(socket, buffer, offset, 0) < 0) {
        perror("send() failed");
    }
}

void broadcast_room_message(const char *room, const char *sender_username, const char *message, int exclude_socket) {
    for (int i = 0; i < MAX_CLIENTS; ++i) {
        if (clients[i] && strcmp(clients[i]->room, room) == 0 && clients[i]->socket != exclude_socket) {
            send_room_message(clients[i]->socket, room, sender_username, message);
        }
    }
}

Client *get_client_by_username(const char *username) {
    for (int i = 0; i < MAX_CLIENTS; ++i) {
        if (clients[i] && strcmp(clients[i]->username, username) == 0) {
            return clients[i];
        }
    }
    return NULL;
}

Room *get_room_by_name(const char *room_name) {
    for (int i = 0; i < room_count; ++i) {
        if (strcmp(rooms[i].name, room_name) == 0) {
            return &rooms[i];
        }
    }
    return NULL;
}

Room *create_room(const char *room_name, const char *password) {
    if (room_count >= MAX_ROOMS) {
        fprintf(stderr, "Maximum number of rooms reached.\n");
        return NULL;
    }

    strcpy(rooms[room_count].name, room_name);
    strcpy(rooms[room_count].password, password);
    room_count++;
    return &rooms[room_count - 1];
}

void remove_client(int socket) {
    for (int i = 0; i < MAX_CLIENTS; ++i) {
        if (clients[i] && clients[i]->socket == socket) {
            free(clients[i]);
            clients[i] = NULL;
        }
    }
}

void print_usage(const char *prog_name) {
    fprintf(stderr, "Usage: %s -p <port>\n", prog_name);
}
