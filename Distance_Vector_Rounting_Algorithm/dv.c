#include <assert.h>
#include <stdio.h>
#include <stdlib.h> 
#include <stdint.h>
#include <sys/time.h>
#include <sys/select.h>
#include <unistd.h>
#include <string.h>
#include <arpa/inet.h>

#include "dv.h"
#include "es.h"
#include "ls.h"
#include "rt.h"
#include "n2h.h"

#define INF_COST 150 

struct dest_pairs {
    uint64_t node;
    uint64_t cost;
} __attribute__((packed));

struct dv_update {
    uint32_t type;
    uint32_t version;
    uint64_t num_updates;
    struct dest_pairs updates[];
} __attribute__((packed));

// Function prototypes
void receive_updates(fd_set *readfds);
int process_received_update(char *buffer, ssize_t length, node sender);
cost get_cost_to_neighbor(node neighbor);
void send_periodic_updates();
struct dv_update *create_update_from_table();
void send_updates_to_neighbors(struct dv_update *message, size_t message_size);

// Global variables
extern struct el *g_lst;  // List of event sets from the parsed event config file
extern struct link *g_ls; // Link set (links of the current node)
extern struct rte *g_rt;  // Routing table of the current node

// Entry point for processing the list of event sets
void walk_event_set_list(int pupdate_interval, int evset_interval, int verbose){
    (void)verbose; // Suppress unused parameter warning

    struct el *es; // Event set

    print_el();

    // For each event set in the global list
    for (es = g_lst->next; es != g_lst; es = es->next){
        process_event_set(es);

        // Process updates
        dv_process_updates(pupdate_interval, evset_interval);

        printf("[es] >>>>>>> Start dumping data structures <<<<<<<<<<<\n");
        print_rt();
    }

    // Uncomment this line to continue running after all event sets are processed
    dv_process_updates(pupdate_interval, 0);
}

// Process individual events in a single event set
void process_event_set(struct el *es){
    struct es *ev_set; // Event set (list of events)
    struct es *ev;     // Single event

    assert(es);

    // Get the head of the event set
    ev_set = es->es_head;

    printf("[es] >>>>>>>>>> Dispatch next event set <<<<<<<<<<<<<\n");

    // For each event in the event set
    for (ev = ev_set->next; ev != ev_set; ev = ev->next){
        dispatch_single_event(ev);
    }
}

// Dispatch a single event, update data structures, and send updates to neighbors if needed
void dispatch_single_event(struct es *ev){
    assert(ev);
    print_event(ev);

    int updated = 0; 

    switch (ev->ev_ty){
    case _es_link:
        updated = add_link_if_local(ev->peer0, ev->port0, ev->peer1, ev->port1,
                                    ev->cost, ev->name);
        if (updated){
            // After adding link, add route to neighbor in routing table if it is shorter path
            struct link *lnk = find_link(ev->name);
            if (lnk)
            {
                struct rte *entry = find_rte(lnk->peer);
                if (lnk->c < entry->c){
                    update_rte(lnk->peer, lnk->c, lnk->peer);
                    print_rte(find_rte(lnk->peer));
                }
            }
        }
        break;
    case _ud_link:
        struct link *changed_link = find_link(ev->name);
        int difference = ev->cost-changed_link->c;
        updated = ud_link(ev->name, ev->cost);
        if (updated){
            struct rte *entry;
            for (entry = g_rt->next; entry != g_rt; entry=entry->next){
                
                //  Update outes that use the link 
                if (entry->nh == changed_link->peer){
                    update_rte(entry->d, entry->c+difference, entry->nh);
                    print_rte(find_rte(entry->d));
                }
                
                // Update the direct cost to node if cheaper to go directly 
                if (entry->d == changed_link->peer && ev->cost < (int)entry->c){
                    update_rte(entry->d, ev->cost, changed_link->peer);
                    print_rte(find_rte(entry->d));
                }
            }
        }
        break;
    case _td_link:
        struct link *removed_link = find_link(ev->name); 
        updated = del_link(ev->name);
        if (updated)
        {
            del_rte(removed_link->peer);
        }
        break;
    default:
        break;
    }

    if (updated){
        send_periodic_updates();
    }
}

// Send updates to all direct neighbors
void send_updates_to_neighbors(struct dv_update *message, size_t message_size){
    struct link *current_link;
    for (current_link = g_ls->next; current_link != g_ls; current_link = current_link->next){
        ssize_t sent = sendto(current_link->sockfd, message, message_size, 0,
                              (struct sockaddr *)&current_link->peer_addr, sizeof(current_link->peer_addr));
        if (sent == -1){
            perror("sendto");
        }
    }
}

void dv_process_updates(int pupdate_interval, int evset_interval){

    struct timeval current_time, next_update_time, end_time, block_time;
    gettimeofday(&current_time, NULL);
    int has_end_time = 0;

    if (evset_interval > 0){
        has_end_time = 1;
        end_time = current_time;
        end_time.tv_sec += evset_interval;
    }

    next_update_time = current_time;
    next_update_time.tv_sec += pupdate_interval;

    while (1){

        gettimeofday(&current_time, NULL);

        // Calculate block time
        timersub(&next_update_time, &current_time, &block_time);

        if (has_end_time)
        {
            struct timeval evset_left;
            timersub(&end_time, &current_time, &evset_left);

            if ((evset_left.tv_sec < 0) || (evset_left.tv_sec == 0 && evset_left.tv_usec <= 0)){
                break;
            }

            if ((block_time.tv_sec > evset_left.tv_sec) ||
                (block_time.tv_sec == evset_left.tv_sec && block_time.tv_usec > evset_left.tv_usec)){
                block_time = evset_left;
            }
        }

        // Ensure block_time is not negative
        if (block_time.tv_sec < 0 || (block_time.tv_sec == 0 && block_time.tv_usec < 0)){
            block_time.tv_sec = 0;
            block_time.tv_usec = 0;
        }

        fd_set readfds;
        FD_ZERO(&readfds);
        int maxfd = -1;
        struct link *lnk;

        for (lnk = g_ls->next; lnk != g_ls; lnk = lnk->next){
            if (lnk->sockfd >= 0) {  // Ensure socket is valid
                FD_SET(lnk->sockfd, &readfds);
                if (lnk->sockfd > maxfd){
                    maxfd = lnk->sockfd;
                }
            }
        }

        int sel_return = select(maxfd + 1, &readfds, NULL, NULL, &block_time);

        if (sel_return == -1){
            perror("select()");
            // Consider adding a short sleep here to avoid tight loop in case of persistent error
            // sleep(1);
        }
        else if (sel_return == 0){
            send_periodic_updates();
            gettimeofday(&next_update_time, NULL);
            next_update_time.tv_sec += pupdate_interval;
        }
        else{
            receive_updates(&readfds);
        }
    }
}

// Receive updates from neighbors
void receive_updates(fd_set *readfds){
    struct link *lnk;
    int routing_table_updated = 0;

    for (lnk = g_ls->next; lnk != g_ls; lnk = lnk->next){
        if (FD_ISSET(lnk->sockfd, readfds)){
            char buffer[4096];
            ssize_t recv_len = recvfrom(lnk->sockfd, buffer, sizeof(buffer), 0, NULL, NULL);
            if (recv_len > 0){
                int updated = process_received_update(buffer, recv_len, lnk->peer);
                if (updated){
                    routing_table_updated = 1;
                }
            }
        }
    }

    if (routing_table_updated){
        send_periodic_updates();
    }
}

// Process received update; returns 1 if routing table was updated
int process_received_update(char *buffer, ssize_t length, node sender){
    int updated = 0;

    if ((size_t)length < sizeof(struct dv_update)){
        // Message too short
        return updated;
    }

    // Parse the message
    struct dv_update *message = (struct dv_update *)buffer;

    uint64_t num_updates = be64toh(message->num_updates);

    // Calculate expected size
    size_t expected_size = sizeof(struct dv_update) + num_updates * sizeof(struct dest_pairs);

    if ((size_t)length < expected_size){
        // Message too short
        return updated;
    }

    // Get the cost to the sender (neighbor)
    cost cost_to_sender = get_cost_to_neighbor(sender);
    if (cost_to_sender == INF_COST){
        // Sender is not a direct neighbor; ignore the update
        return updated;
    }

    // Process each update
    struct dest_pairs *updates = message->updates;
    for (uint64_t i = 0; i < num_updates; i++){
        node dest = (node)be64toh(updates[i].node);
        cost cost_from_sender = (cost)be64toh(updates[i].cost);

        // Avoid loops: skip if the destination is ourselves
        if (dest == get_myid()){
            continue;
        }

        // Total cost is cost to sender plus cost from sender to destination
        cost total_cost = cost_to_sender + cost_from_sender;
        if (total_cost > INF_COST){
            total_cost = INF_COST;
        }

        // Check if we need to update the routing table
        struct rte *entry = find_rte(dest);
        if (!entry){
            add_rte(dest, total_cost, sender);
            if (total_cost != INF_COST){
                updated = 1;
            }
        }else if (total_cost < entry->c){

            update_rte(dest, total_cost, sender);
            print_rte(find_rte(dest));
            updated = 1;
        }else if (entry->nh == sender && total_cost != entry->c){
            // Update cost if the cost via the same next hop has changed
            update_rte(dest, total_cost, sender);
        }


        struct rte *post_entry = find_rte(dest);
        struct link *lnk;
        for (lnk = g_ls->next; lnk != g_ls; lnk = lnk->next){
            if (lnk->peer == dest && lnk->c < post_entry->c){
                update_rte(dest, lnk->c, dest);
                print_rte(find_rte(dest));
                updated = 0;
            }
        }
    }
    return updated;
}

// Create update message from routing table
struct dv_update *create_update_from_table()
{
    // Count the number of entries in the routing table
    uint64_t num_entries = 0;
    struct rte *entry;
    for (entry = g_rt->next; entry != g_rt; entry = entry->next){
        num_entries++;
    }

    // Calculate the size of the message
    size_t message_size = sizeof(struct dv_update) + num_entries * sizeof(struct dest_pairs);

    // Allocate memory for the message
    struct dv_update *message = (struct dv_update *)malloc(message_size);
    if (!message){
        perror("malloc");
        exit(1);
    }

    // Fill in the message
    message->type = htonl(0x7);      // Convert to network byte order
    message->version = htonl(0x1);   // Convert to network byte order
    message->num_updates = htobe64(num_entries);

    // Fill in the updates
    struct dest_pairs *updates = message->updates;
    int i = 0;
    for (entry = g_rt->next; entry != g_rt; entry = entry->next){
        updates[i].node = htobe64(entry->d);
        updates[i].cost = htobe64(entry->c);
        i++;
    }

    // Return the message
    return message;
}

// Send periodic updates to neighbors
void send_periodic_updates(){
    // Generate the update
    struct dv_update *message = create_update_from_table();

    // Calculate the message size
    uint64_t num_entries = be64toh(message->num_updates);
    size_t message_size = sizeof(struct dv_update) + num_entries * sizeof(struct dest_pairs);

    // Send to neighbors
    send_updates_to_neighbors(message, message_size);

    // Free after sending
    free(message);
}

// Get the cost to a neighbor
cost get_cost_to_neighbor(node neighbor){
    struct link *lnk;
    for (lnk = g_ls->next; lnk != g_ls; lnk = lnk->next){
        if (lnk->peer == neighbor){
            return lnk->c;
        }
    }
    // Return a large cost if neighbor not found
    return INF_COST;
}
