#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <arpa/inet.h>
#include <unistd.h>

#include "chord_arg_parser.h"
#include "chord.h"
#include "hash.h"
#include "chord.pb-c.h" 

typedef struct LinkedChordNode {
    uint64_t key;
    struct sockaddr_in addr;
} LinkedChordNode;

typedef struct ChordNode {
    uint64_t key;
    struct sockaddr_in addr;
    LinkedChordNode *predecessor;
    LinkedChordNode *finger_table[64];
    LinkedChordNode *successor_list[32];
    int list_size;
} ChordNode;

void printKey(uint64_t key) {
    printf("%" PRIu64, key);
}

void chord_create(ChordNode *node);
void chord_join(ChordNode *node, struct sockaddr_in dest_addr);
int send_message(struct ChordMessage *chord_message, struct sockaddr_in dest_addr);
void print_state(ChordNode *node);
void chord_lookup(ChordNode *node, const char *key_str);
LinkedChordNode* find_successor(ChordNode *node, uint64_t key, int qtype);
void check_predecessor(ChordNode *node);
void notify(ChordNode *node, LinkedChordNode *notifyNode);
void stabilize(ChordNode *node);
void fix_fingers(ChordNode *node);
void handle_incoming_data(int sock, ChordNode *curr_node);

int main(int argc, char *argv[]) {

    // Parse args
    struct chord_arguments args = chord_parseopt(argc, argv);


    // Initialize ChordNode attributes
    ChordNode curr_node;

    curr_node.list_size = args.num_successors;
    curr_node.addr = args.my_address;

    if (args.id != 0){
        curr_node.key = args.id;
    } else {
        // set key to hash of ip addr + port num
        struct sha1sum_ctx *sha_sum = sha1sum_create(NULL, 0);
        uint8_t untruncated_hash[20]; 
        sha1sum_finish(sha_sum, (const uint8_t *)&curr_node.addr.sin_port, sizeof(curr_node.addr.sin_port) + sizeof(curr_node.addr.sin_addr), untruncated_hash);
        curr_node.key = sha1sum_truncated_head(untruncated_hash);
        sha1sum_destroy(sha_sum);
    }

    // Create tcp socket and bind and listen for other node requests 
    int tcp_sock = socket(AF_INET, SOCK_STREAM, 0);
    if (tcp_sock < 0) {
        perror("Failed to create TCP socket");
        return -1;
    }

    if (bind(tcp_sock, (struct sockaddr *)&curr_node.addr, sizeof(curr_node.addr)) < 0) {
        perror("Failed to bind TCP socket");
        close(tcp_sock);
        return -1;
    }

    listen(tcp_sock, 5);

    // Create or join Chord ring
    if (args.join_address.sin_port) {
        chord_join(&curr_node, args.join_address);
    } else {
        chord_create(&curr_node);
    }

    // Update counters 
    uint64_t stabilize_elapsed = 0;
    uint64_t fix_fingers_elapsed = 0;
    uint64_t check_predecessor_elapsed = 0;

    // Main loop 
    while (1) {

        // 1 decisecond timeout 
        struct timeval timeout;
        timeout.tv_sec = 0;
        timeout.tv_usec = 100000;

        // set up file descriptors and select 
        fd_set read_fds;
        FD_ZERO(&read_fds);
        FD_SET(STDIN_FILENO, &read_fds);
        FD_SET(tcp_sock, &read_fds);

        int maxfd = tcp_sock;
        int activity = select(maxfd + 1, &read_fds, NULL, NULL, &timeout);

        // select fail 
        if (activity < 0){
            perror("select failed");
            break;
        }

        // 1 decisecond has passed 
        if (activity == 0){
            stabilize_elapsed += 1;
            fix_fingers_elapsed += 1;
            check_predecessor_elapsed += 1;
            
            if (stabilize_elapsed >= args.stablize_period) {
                stabilize(&curr_node);
                stabilize_elapsed = 0;  
            }

            if (fix_fingers_elapsed >= args.fix_fingers_period) {
                fix_fingers(&curr_node);
                fix_fingers_elapsed = 0; 
            }

            if (check_predecessor_elapsed >= args.check_predecessor_period) {
                check_predecessor(&curr_node);
                check_predecessor_elapsed = 0;  
            }
        } 
        
        // Handle user input from stdin
        if (FD_ISSET(STDIN_FILENO, &read_fds)) {
            char input[256];
            printf("> ");
            fflush(stdout); 
            if (fgets(input, sizeof(input), stdin) != NULL) {

                // Lookup <key> command 
                if (strncmp(input, "Lookup", 6) == 0) {

                    // Get and format key 
                    char *key_str = input + 7; 
                    key_str[strcspn(key_str, "\n")] = 0; 

                    chord_lookup(&curr_node, key_str);                
                } 

                // PrintState command  
                else if (strncmp(input, "PrintState", 10) == 0) {
                    print_state(&curr_node); 
                } 

                // Undefined command
                else {
                    printf("Unknown command. Available commands: Lookup <key>, PrintState\n");
                }
            }
        }

        // Handle incoming connections from other nodes on TCP socket
        if (FD_ISSET(tcp_sock, &read_fds)) {
            struct sockaddr_in peer_addr;
            socklen_t peer_addr_len = sizeof(peer_addr);
            int new_socket = accept(tcp_sock, (struct sockaddr *)&peer_addr, &peer_addr_len);
            if (new_socket < 0) {
                perror("Failed to accept connection");
                continue;
            }

            handle_incoming_data(new_socket, &curr_node); 

            close(new_socket);  
        }    
    }

    close(tcp_sock);
    return 0;
}

void chord_create(ChordNode *node) {
    
    node->predecessor = NULL;

    // Set all successors to point to the node itself
    for (int i=0; i<node->list_size; i++){
        LinkedChordNode *copy = malloc(sizeof(LinkedChordNode));
        if (!copy) {
            perror("Failed to allocate memory for successor list");
            exit(EXIT_FAILURE);
        }
        copy->key = node->key;
        copy->addr = node->addr;
        node->successor_list[i] = copy;
    }
}

void chord_join(ChordNode *node, struct sockaddr_in dest_addr) {

    node->predecessor = NULL;

    // Get successor 
    ChordMessage chord_message = CHORD_MESSAGE__INIT;
    chord_message.version = 417;
    chord_message.query_id = qtype; 

    // Create a Node protobuf message for the requester
    Node requester = NODE__INIT;
    requester.key = node->key;
    requester.address = ntohl(node->addr.sin_addr.s_addr); 
    requester.port = ntohs(node->addr.sin_port);

    // Create FindSuccessorRequest and populate
    FindSuccessorRequest find_succ_req = FIND_SUCCESSOR_REQUEST__INIT;
    find_succ_req.key = key;
    find_succ_req.requester = &requester;

    // Add FindSuccessorRequest to ChordMessage
    chord_message.find_successor_request = &find_succ_req;

    // If successfully sent, break loop 
    send_message(&chord_message, closest->addr) != -1){

    /* dont need?
    // Create and populate a ChordMessage, need to req successors
    ChordMessage chord_message = CHORD_MESSAGE__INIT;
    chord_message.version = 417;

    // Create FindSuccessorRequest and add to the message  
    GetSuccessorListRequest get_succ_list_req = GET_SUCCESSOR_LIST_REQUEST__INIT;
    chord_message.get_successor_list_request = &get_succ_list_req;

    // Send the message
    send_message(&chord_message, dest_addr);
    */
}

// Sends a serialized protobuf message over TCP
int send_message(ChordMessage *chord_message, struct sockaddr_in dest_addr) {

    size_t message_size = chord_message__get_packed_size(chord_message);
    void *buffer = malloc(message_size);
    if (!buffer) {
        perror("Failed to allocate memory for serialized message");
        return -1;
    }
    chord_message__pack(chord_message, buffer);

    uint64_t net_len = htobe64(message_size);

    // Initialize TCP connection
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        perror("Socket creation failed");
        free(buffer);
        return -1;
    }

    if (connect(sock, (struct sockaddr *)&dest_addr, sizeof(dest_addr)) < 0) {
        perror("Connection failed");
        close(sock);
        free(buffer);
        return -1;
    }

    // Send the length
    if (send(sock, &net_len, sizeof(net_len), 0) < 0) {
        perror("Failed to send message length");
        close(sock);
        free(buffer);
        return -1;
    }

    // Send data 
    if (send(sock, buffer, message_size, 0) < 0) {
        perror("Failed to send message data");
        close(sock);
        free(buffer);
        return -1;
    }

    // Clean up
    close(sock);
    free(buffer);
    return 0;
}

// Called for PrintState user command
void print_state(ChordNode *node){

    // First line, print key and addr info
    printf("< Self ");
    printKey(node->key);

    // Convert ip to readable format
    char ip_str[INET_ADDRSTRLEN]; 
    inet_ntop(AF_INET, &(node->addr.sin_addr), ip_str, INET_ADDRSTRLEN);
    printf(" %s:%d\n", ip_str, ntohs(node->addr.sin_port));


    // Print successors 
    for (int i=0; i<node->list_size; i++){
        
        LinkedChordNode *successor = node->successor_list[i]; 

        printf("< Successor [%d] ", i+1);
        
        printKey(successor->key);
        
        inet_ntop(AF_INET, &(successor->addr.sin_addr), ip_str, INET_ADDRSTRLEN);
        printf(" %s %d\n", ip_str, ntohs(successor->addr.sin_port));
    }

    // Print finger table 
    for (int i=0; i<64; i++){
        
        LinkedChordNode *finger = node->finger_table[i]; 

        printf("< Finger [%d] ", i+1);
        
        printKey(finger->key);
        
        inet_ntop(AF_INET, &(finger->addr.sin_addr), ip_str, INET_ADDRSTRLEN);
        printf(" %s %d\n", ip_str, ntohs(finger->addr.sin_port));
    }
}

// Given a key, lookup where the key is and print out node hash and addr
void chord_lookup(ChordNode *node, const char *key_str){

    struct sha1sum_ctx *sha_sum = sha1sum_create(NULL, 0);
    if (!sha_sum) {
        perror("Failed to create hash context");
        return;
    }

    uint8_t hash[20];
    sha1sum_finish(sha_sum, (const uint8_t *)key_str, strlen(key_str), hash);
    uint64_t final_hash = sha1sum_truncated_head(hash);
    sha1sum_destroy(sha_sum);
    
    printf("< %s ", key_str);
    printKey(final_hash);
    printf("\n");

    // Call get_successor to find node with the data 
    find_successor(node, final_hash, 1);
}

LinkedChordNode* find_successor(ChordNode *node, uint64_t key, int qtype){

    // First look at finger table to get closest node 
    LinkedChordNode* closest = NULL;
    
    for (int i=63; i>=0; i--){
        closest = node->finger_table[i];
        if (closest != NULL && key >= closest->key){

            // Create message 
            ChordMessage chord_message = CHORD_MESSAGE__INIT;
            chord_message.version = 417;
            chord_message.query_id = qtype; 

            // Create a Node protobuf message for the requester
            Node requester = NODE__INIT;
            requester.key = node->key;
            requester.address = ntohl(node->addr.sin_addr.s_addr); 
            requester.port = ntohs(node->addr.sin_port);

            // Create FindSuccessorRequest and populate
            FindSuccessorRequest find_succ_req = FIND_SUCCESSOR_REQUEST__INIT;
            find_succ_req.key = key;
            find_succ_req.requester = &requester;

            // Add FindSuccessorRequest to ChordMessage
            chord_message.find_successor_request = &find_succ_req;

            // If successfully sent, break loop 
            if (send_message(&chord_message, closest->addr) != -1){
                break;
            }
        }
    }

    if (closest != NULL){
        return closest;
    } else {

        // Curr node is the successor 
        LinkedChordNode *currNode = malloc(sizeof(LinkedChordNode));
        currNode->key = node->key;
        currNode->addr = node->addr;  

        // TODO: get_successor_reqponse here 

        return currNode;
    }
}

void notify(ChordNode *node, LinkedChordNode *notifyNode){
    
    // Create notify protobuf and send 
    ChordMessage chord_message = CHORD_MESSAGE__INIT;
    chord_message.version = 417;

    // Create a Node protobuf message for the requester
    Node requester = NODE__INIT;
    requester.key = node->key;
    requester.address = ntohl(node->addr.sin_addr.s_addr); 
    requester.port = ntohs(node->addr.sin_port);
    
    // Create NotifyRequest and add to message
    NotifyRequest notify_req = NOTIFY_REQUEST__INIT;
    notify_req.node = &requester;
    chord_message.notify_request = &notify_req;

    // Send message
    send_message(&chord_message, notifyNode->addr);
}

void stabilize(ChordNode *node){
    
    // For successor in the table try to request predecessor 
    for (int i=0; i<node->list_size; i++){
        
        // Create chord message 
        ChordMessage chord_message = CHORD_MESSAGE__INIT;
        chord_message.version = 417;

        // Create request 
        GetPredecessorRequest get_pred_req = GET_PREDECESSOR_REQUEST__INIT;
        chord_message.get_predecessor_request = &get_pred_req;

        // Successor has died 
        if (send_message(&chord_message, node->successor_list[i]->addr) < 0){
            
            // All successors have died 
            if (i+1 == node->list_size){
                for (int j=0; i<node->list_size; j++){
                    LinkedChordNode *copy = malloc(sizeof(LinkedChordNode));
                    if (!copy) {
                        perror("Failed to allocate memory for successor list");
                        exit(EXIT_FAILURE);
                    }
                    copy->key = node->key;
                    copy->addr = node->addr;
                    node->successor_list[j] = copy;
                }
                return;
            }
            
            // Have other successors, so ask next to replace current immediate successor
            ChordMessage chord_message = CHORD_MESSAGE__INIT;
            chord_message.version = 417;

            // Create FindSuccessorRequest and add to the message  
            GetSuccessorListRequest get_succ_list_req = GET_SUCCESSOR_LIST_REQUEST__INIT;
            chord_message.get_successor_list_request = &get_succ_list_req;

            // Send the message
            send_message(&chord_message, node->successor_list[i+1]->addr);

        } else {
            return;
        }
    }
}

int next = 0;
void fix_fingers(ChordNode *node){

    // Set finger to NULL, need to recompute this finger, find lowest hash assoc with finger 
    node->finger_table[next] = NULL;

    // TODO: Check formula 
    uint64_t target_id = ((node->key+1ULL)<<next)%(1ULL<<63);

    // Find the successor of target_id
    find_successor(node, target_id, next << 1);

    // increment next finger to fix 
    next += 1;
    if (next > 63){
        next = 0;
    }
}

void check_predecessor(ChordNode *node){
    
    // Doesn't have a predecessor
    if (node->predecessor == NULL){
        return;
    }
    
    // Otherwise send network request to check if predecessor is still alive 
    ChordMessage chord_message = CHORD_MESSAGE__INIT;
    chord_message.version = 417;

    // Create request and add to the message  
    CheckPredecessorRequest check_pred_req = CHECK_PREDECESSOR_REQUEST__INIT;
    chord_message.check_predecessor_request = &check_pred_req;

    // Predecessor is not on network anymore 
    if (send_message(&chord_message, node->predecessor->addr) < 0){
        node->predecessor = NULL;
    }
}

// Handles data send on TCP from another chord
void handle_incoming_data(int sock, ChordNode *curr_node) {

    uint64_t net_len;
    ssize_t bytes_read = recv(sock, &net_len, sizeof(net_len), MSG_WAITALL);
    if (bytes_read <= 0) {
        perror("Failed to read message length");
        return;
    }

    uint64_t message_size = be64toh(net_len);

    // Allocate buffer for the incoming message
    uint8_t *buffer = malloc(message_size);
    if (!buffer) {
        perror("Failed to allocate memory for incoming message");
        return;
    }

    // Read the message data
    bytes_read = recv(sock, buffer, message_size, MSG_WAITALL);
    if (bytes_read <= 0) {
        perror("Failed to read message data");
        free(buffer);
        return;
    }

    // Deserialize the message
    ChordMessage *chord_message = chord_message__unpack(NULL, message_size, buffer);
    if (!chord_message) {
        fprintf(stderr, "Failed to unpack ChordMessage\n");
        free(buffer);
        return;
    }

    // Handle the message based on its content
    if (chord_message->get_successor_list_request) {
        //TODO fix logic
        // Prepare the response message
        ChordMessage response = CHORD_MESSAGE__INIT;
        response.version = chord_message->version;

        // Create SuccessorListResponse
        GetSuccessorListResponse succ_list_resp = GET_SUCCESSOR_LIST_RESPONSE__INIT;

        // Populate the successor list
        Node **successor_nodes = malloc(curr_node->list_size * sizeof(Node *));
        for (int i = 0; i < curr_node->list_size; i++) {
            successor_nodes[i] = malloc(sizeof(Node));
            node__init(successor_nodes[i]);
            successor_nodes[i]->key = curr_node->successor_list[i]->key;
            successor_nodes[i]->address = ntohl(curr_node->successor_list[i]->addr.sin_addr.s_addr);
            successor_nodes[i]->port = ntohs(curr_node->successor_list[i]->addr.sin_port);
        }

        succ_list_resp.n_successors = curr_node->list_size;
        succ_list_resp.successors = successor_nodes;
        response.get_successor_list_response = &succ_list_resp;

        // TODO: Send the response
        send_message(&response, curr_node->addr);

        // Clean up
        for (int i = 0; i < curr_node->list_size; i++) {
            free(successor_nodes[i]);
        }
        free(successor_nodes);

    } else if (chord_message->get_predecessor_request) {

        // Prepare the response message
        ChordMessage response = CHORD_MESSAGE__INIT;
        response.version = chord_message->version;

        GetPredecessorResponse pred_response = GET_PREDECESSOR_RESPONSE__INIT;
        if (curr_node->predecessor) {
            // Fill in predecessor information
            Node pred_node = NODE__INIT;
            pred_node.key = curr_node->predecessor->key;
            pred_node.address = ntohl(curr_node->predecessor->addr.sin_addr.s_addr);
            pred_node.port = ntohs(curr_node->predecessor->addr.sin_port);
            pred_response.node = &pred_node;
        } else {
            // No predecessor
            pred_response.node = NULL;
        }
        response.get_predecessor_response = &pred_response;

        // TODO: Send the response
        send_message(&response, curr_node->addr);

    } else if (chord_message->notify_request) {
        //TODO fix logic
        // Extract the notifying node
        Node *notifier = chord_message->notify_request->node;

        if (notifier) {
            uint64_t notifier_key = notifier->key;
            struct sockaddr_in notifier_addr;
            notifier_addr.sin_family = AF_INET;
            notifier_addr.sin_addr.s_addr = htonl(notifier->address);
            notifier_addr.sin_port = htons(notifier->port);

            // Update predecessor if necessary
            if (curr_node->predecessor == NULL ||
                (notifier_key > curr_node->predecessor->key && notifier_key < curr_node->key)) {

                // Update predecessor
                if (curr_node->predecessor == NULL) {
                    curr_node->predecessor = malloc(sizeof(LinkedChordNode));
                }
                curr_node->predecessor->key = notifier_key;
                curr_node->predecessor->addr = notifier_addr;uccessor = n′.ﬁnd successor(
            }
        }

    } else if (chord_message->find_successor_request) {
        // TODOL FIX 

        // Extract the requested key
        uint64_t key = chord_message->find_successor_request->key;

        // Find the successor of the key
        find_successor(curr_node, key, chord_message->query_id);

    } else if (chord_message->check_predecessor_request) {
        
        // Don't need send anything since getting TCP conn verifies we are alive

    } else if (chord_message->get_successor_list_response) {

        // Update successor list based on the response
        GetSuccessorListResponse *resp = chord_message->get_successor_list_response;

        // Free the old successor list except first
        for (int i = 1; i < curr_node->list_size; i++) {
            free(curr_node->successor_list[i]);
        }

        // Allocate and update successor list except first 
        curr_node->list_size = resp->n_successors;
        for (int i = 0; i < curr_node->list_size-1; i++) {
            curr_node->successor_list[i+1] = malloc(sizeof(LinkedChordNode));
            curr_node->successor_list[i+1]->key = resp->successors[i]->key;

            curr_node->successor_list[i+1]->addr.sin_family = AF_INET;
            curr_node->successor_list[i+1]->addr.sin_addr.s_addr = htonl(resp->successors[i]->address);
            curr_node->successor_list[i+1]->addr.sin_port = htons(resp->successors[i]->port);
        }    

    } else if (chord_message->get_predecessor_response) {
        
        GetPredecessorResponse *resp = chord_message->get_predecessor_response;
        if (resp->node) {
            // Compare the pred key w/ succ key 
            uint64_t pred_key = resp->node->key;
            if (pred_key > curr_node->key && pred_key < curr_node->successor_list[0]->key) {

                // Update immediate successor
                LinkedChordNode *new_successor = malloc(sizeof(LinkedChordNode));
                new_successor->key = pred_key;

                new_successor->addr.sin_family = AF_INET;
                new_successor->addr.sin_addr.s_addr = htonl(resp->node->address);
                new_successor->addr.sin_port = htons(resp->node->port);

                // shifts the list_size-1 entries by 1
                memmove(curr_node->successor_list[0], curr_node->successor_list[1], (curr_node->list_size - 1) * sizeof(LinkedChordNode));
                // Replace the first entry in the successor list
                curr_node->successor_list[0] = new_successor;

                // Notify successor about current node, do this always or only when changes?
                notify(curr_node, curr_node->successor_list[0]);
            }
        }


    } else if (chord_message->find_successor_response) {
        
        FindSuccessorResponse *resp = chord_message->find_successor_response;

        // Depending on the query_id, update finger table or process lookup response
        int query_id = chord_message->query_id;
        if (query_id == 1) {

            // Lookup command response
            printf("< \n");
            printKey(resp->node->key);
            printf(" %s %d\n",
                   inet_ntoa(*(struct in_addr *)&(resp->node->address)),
                   resp->node->port);
        } else {
            // Update finger table
            int finger_index = query_id >> 1;
            if (finger_index >= 0 && finger_index < 64) {
                if (curr_node->finger_table[finger_index] == NULL) {
                    curr_node->finger_table[finger_index] = malloc(sizeof(LinkedChordNode));
                }
                curr_node->finger_table[finger_index]->key = resp->node->key;

                curr_node->finger_table[finger_index]->addr.sin_family = AF_INET;
                curr_node->finger_table[finger_index]->addr.sin_addr.s_addr = htonl(resp->node->address);
                curr_node->finger_table[finger_index]->addr.sin_port = htons(resp->node->port);
            }
        }    

    } else if (chord_message->check_predecessor_response) {

        // Don't need to do anything
        
    } else {
        fprintf(stderr, "Received unknown message type\n");
    }

    // Clean up
    chord_message__free_unpacked(chord_message, NULL);
    free(buffer);
}
