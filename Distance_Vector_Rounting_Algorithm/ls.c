/* $Id: ls.h,v 1.1 2000/02/23 01:00:30 bobby Exp $
 * Link Set
 */

#ifndef _LS_C_
#define _LS_C_

#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <string.h>
#include <errno.h>
#include <netdb.h>
#include <unistd.h>
#include "common.h"
#include "ls.h"
#include "queue.h"
#include "n2h.h"
#include "rt.h"

struct link *g_ls;
static node g_host;

int create_ls()
{
	InitDQ(g_ls, struct link);
	assert(g_ls);

	g_ls->peer = g_ls->c = -1;
	g_ls->name = 0x0;

	g_host = get_myid();

	return (g_ls != 0x0);
}


// returns a socket descriptor that is bound on the port that current node will listen to
// return a negative value on error

int create_link_sock(int port)
{
    struct addrinfo address_criteria;
    memset(&address_criteria, 0, sizeof(address_criteria));
    address_criteria.ai_family = AF_INET;           
    address_criteria.ai_flags = AI_PASSIVE;         
    address_criteria.ai_socktype = SOCK_DGRAM;      
    address_criteria.ai_protocol = IPPROTO_UDP;     

    struct addrinfo *server_address;
    char port_str[6];
    snprintf(port_str, sizeof(port_str), "%d", port);
    if (getaddrinfo(NULL, port_str, &address_criteria, &server_address) != 0) {
        perror("getaddrinfo() failed");
        return -1;
    }

    // Create socket
    int sock = socket(server_address->ai_family, server_address->ai_socktype, server_address->ai_protocol);
    if (sock < 0) {
        perror("socket() failed");
        freeaddrinfo(server_address);
        return -1;
    }

    // Bind to local address
    if (bind(sock, server_address->ai_addr, server_address->ai_addrlen) < 0) {
        perror("bind() failed");
        freeaddrinfo(server_address);
        close(sock);
        return -1;
    }

    freeaddrinfo(server_address);  	

    return sock;
}

// add a link to the global link set
int add_link(int host_port,
			 node peer, int peer_port,
			 cost c, char *name)
{
	struct in_addr peer_addr;
	memset(&peer_addr, 0, sizeof(peer_addr));

	const char* peer_hostname = gethostbynode(peer);
	if(peer_hostname == NULL){
		return -1;
	}

	peer_addr = getaddrbyhost(peer_hostname);
	if(peer_addr.s_addr == 0){
		return -1;
	}

	struct link *nl = (struct link *)malloc(sizeof(struct link));
	if (!nl)
	{
		return ENOMEM;
	}
	memset(nl, 0, sizeof(*nl));

	nl->host_port = host_port;
	nl->peer = peer;

	nl->peer_addr.sin_family = AF_INET;
	nl->peer_addr.sin_port = htons(peer_port);
	nl->peer_addr.sin_addr = peer_addr;

	nl->peer_port = peer_port;
	nl->c = c;
	nl->name = (char *)malloc(strlen(name) + 1);
	if (!(nl->name))
	{
		free(nl);
		return ENOMEM;
	}
	strcpy(nl->name, name);

	int rv = create_link_sock(nl->host_port);
	if (rv < 0)
	{
		free(nl->name);
		free(nl);
		return rv;
	}
	nl->sockfd = rv;

	InsertDQ(g_ls, nl);
	return 1;
}

// check if current host is participating in this link
// if so, call `add_link()` to add it to the global link set
// returns 0 if irrelevant, 1 if link was added, <0 if error
int add_link_if_local(node peer0, int port0, node peer1, int port1, cost c, char *name)
{
	node host = get_myid();
	int host_port = -1;
	int peer_port = -1;
	int peer = -1;
	if (peer0 == host)
	{
		host_port = port0;
		peer_port = port1;
		peer = peer1;
	}
	else if (peer1 == host)
	{
		host_port = port1;
		peer_port = port0;
		peer = peer0;
	}
	else
	{
		return 0; // This link is not relevant
	}

	return add_link(host_port, peer, peer_port, c, name);
}

// update cost of a link
// returns 0 if link was not found in my set (i.e. updated link is irrelevant)
// returns 1 if link was found and updated
int ud_link(char *n, int cost)
{
	struct link *i = find_link(n);
	if (!i)
	{
		return 0;
	}

	i->c = cost;
	return 1;
}

struct link *find_link(char *n)
{
	struct link *i;
	for (i = g_ls->next; i != g_ls; i = i->next)
	{
		if (!(strcmp(i->name, n)))
		{
			break;
		}
	}
	if (!strcmp(i->name, n))
	{
		return i;
	}
	else
	{
		return 0x0;
	}
}

// delete a link from the global link set
// return 0 if link was not found (i.e. deleted link is irrelevant)
// return 1 if link was found and deleted
int del_link(char *name)
{
	struct link *i = find_link(name);
	if (!i)
	{
		return 0;
	}
	if (i->sockfd >= 0)
	{
		close(i->sockfd);
	}
	DelDQ(i);
	free(i->name);
	free(i);
	return 1;
}

void print_link(struct link *i)
{
	fprintf(stdout, "[ls]\t ----- link name(%s) ----- \n", i->name);
	fprintf(stdout, "[ls]\t node(%d)host(%s)port(%d) <--> node(%d)host(%s)port(%d)\n",
			g_host, gethostbynode(g_host), i->host_port,
			i->peer, gethostbynode(i->peer), i->peer_port);
	fprintf(stdout, "[ls]\t cost(%d), sock(%d)\n",
			i->c, i->sockfd);
}

void print_ls()
{
	struct link *i;

	fprintf(stdout, "\n[ls] ***** dumping link set *****\n");
	for (i = g_ls->next; i != g_ls; i = i->next)
	{
		assert(i);
		print_link(i);
	}
}

#endif
