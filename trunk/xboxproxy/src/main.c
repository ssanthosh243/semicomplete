/* 
 *
 * $Id$
 */

#include <sys/types.h>
#include <sys/time.h>

#include <stdlib.h>
#include <unistd.h>

#include <errno.h>
#include <string.h>

#ifdef __FreeBSD__
#include <netinet/in_systm.h>
#endif

#include <netdb.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/ip.h>
#include <net/ethernet.h>

#include <pthread.h>
#include <pcap.h>
#include <libnet.h>


#ifndef ETHER_ADDR_LEN
#define ETHER_ADDR_LEN 6
#endif

#include "log.h"
#include "hash.h"

#define SERVER_PORT 3434

/* The xbox network stuff only uses an ip of 0.0.0.1 */
static char filter_app[] = "host 0.0.0.1";

/* xbox broadcasts to FF:FF:FF:FF:FF:FF */
static char broadcastmac[ETHER_ADDR_LEN] = { 
	0xFF, 0xFF, 0xFF, 
	0xFF, 0xFF, 0xFF
};

struct proxy;

typedef struct xbox {
	u_char macaddr[ETHER_ADDR_LEN];
	struct proxy *proxy; /* The IP address of the proxy this xbox is located at */
	time_t lastseen; /* Last time a packet was seen from this xbox */
} xbox_t;

static hash_t *xboxen = NULL;

/* Container for remote proxies */
typedef struct proxy {
	int ip; /* The ip address of this proxy */
	struct in_addr addr;
	int fd; /* File descriptor for the connection to this proxy */
	hash_t *xboxen; /* Table containing known xboxes on this proxy */
} proxy_t;

static hash_t *proxies = NULL;
static fd_set proxysocks;

static char *pcapdev = NULL;
static char *proxyserver = NULL;
static int use_udp = 0;
static int serverport = SERVER_PORT;

#ifdef LIBNET_VERSION_1_0
struct libnet_link_int *libnet;
#else
#	ifdef LIBNET_VERSION_1_1
libnet_t *libnet;
#	endif
#endif

void addxbox(u_char *macaddr, proxy_t *ppt);
proxy_t *addproxy(struct sockaddr_in *addr, int sock);
void packet_handler(u_char *args, const struct pcap_pkthdr *head,
						  const u_char *packet);
void remove_proxy(proxy_t *ppt);
void connect_to_proxy();
int recv_from_proxy(proxy_t *ppt);
void distribute_packet(proxy_t *ppt, char *packet, int pktlen);

int comparemac(const void *k1, const void *k2) {
	return memcmp(k1, k2, ETHER_ADDR_LEN);
}

int compareip(const void *k1, const void *k2) {
	int a, b;
	a = *(unsigned int *)k1;
	b = *(unsigned int *)k2;

	debuglog(20, "compareip: %08x vs %08x = %d", a, b, (a < b ? -1 : (a > b ? 1 : 0)) );

	return (a < b ? -1 : (a > b ? 1 : 0));
}

hash_val_t haship(const void *key) {
	static unsigned long randbox[] = {
		0x49848f1bU, 0xe6255dbaU, 0x36da5bdcU, 0x47bf94e9U,
		0x8cbcce22U, 0x559fc06aU, 0xd268f536U, 0xe10af79aU,
		0xc1af4d69U, 0x1d2917b5U, 0xec4c304dU, 0x9ee5016cU,
		0x69232f74U, 0xfead7bb3U, 0xe9089ab6U, 0xf012f6aeU,
	};

	const unsigned char *str = key;
	int c = 0;
	hash_val_t acc = 0;

	while (c < sizeof(int)) {
		acc ^= randbox[(*str + acc) & 0xf];
		acc = (acc << 1) | (acc >> 31);
		acc &= 0xffffffffU;
		acc ^= randbox[((*str++ >> 4) + acc) & 0xf];
		acc = (acc << 2) | (acc >> 30);
		acc &= 0xffffffffU;
		c++;
	}
	return acc;
}

hash_val_t hashmac(const void *key) {
	static unsigned long randbox[] = {
		0x49848f1bU, 0xe6255dbaU, 0x36da5bdcU, 0x47bf94e9U,
		0x8cbcce22U, 0x559fc06aU, 0xd268f536U, 0xe10af79aU,
		0xc1af4d69U, 0x1d2917b5U, 0xec4c304dU, 0x9ee5016cU,
		0x69232f74U, 0xfead7bb3U, 0xe9089ab6U, 0xf012f6aeU,
	};

	const unsigned char *str = key;
	int c = 0;
	hash_val_t acc = 0;

	while (c < ETHER_ADDR_LEN) {
		acc ^= randbox[(*str + acc) & 0xf];
		acc = (acc << 1) | (acc >> 31);
		acc &= 0xffffffffU;
		acc ^= randbox[((*str++ >> 4) + acc) & 0xf];
		acc = (acc << 2) | (acc >> 30);
		acc &= 0xffffffffU;
		c++;
	}
	return acc;
}

/* 
 * Packet handling routine.
 */
void packet_handler(u_char *args, const struct pcap_pkthdr *head,
						  const u_char *packet) {
	struct ether_header *eptr;

	eptr = (struct ether_header *) packet;

	/* Try adding this xbox to the list */
	//debuglog(1, "Adding xbox in packet_handler");
	addxbox(eptr->ether_shost, 0);

	/* If this packet is sent to our local net, ignore it... */
	if (hash_lookup(xboxen, eptr->ether_shost) != NULL) {
		 debuglog(15, "Packet!");
		 debuglog(3, "From: %s", ether_ntoa((struct ether_addr *)eptr->ether_shost));
		 debuglog(3, "To: %s", ether_ntoa((struct ether_addr *)eptr->ether_dhost));
	 }

	/*
	 * If this is broadcast, send it to all known proxies
	 */
	if (memcmp(eptr->ether_dhost, broadcastmac, ETHER_ADDR_LEN) == 0) {
		hscan_t hs;
		hnode_t *node;

		debuglog(11, "BROADCAST");
		hash_scan_begin(&hs, proxies);
		while ((node = hash_scan_next(&hs))) {
			proxy_t *p = (proxy_t *)(node->hash_data);

			/* Make sure the source addr of this packet is not getting
			 * sent this packet. Do not send! */
			if ((hash_lookup(p->xboxen, eptr->ether_shost) != NULL))
				continue;

			debuglog(3, "Sending ethernet packet to %s", inet_ntoa(p->addr));

			if (use_udp) {
				struct sockaddr_in to;
				to.sin_addr = p->addr;
				to.sin_port = htons(serverport);
				to.sin_family = PF_INET;

				//sendto(p->fd, &(head->caplen), 4, 0, (struct sockaddr *)&to, sizeof(struct sockaddr));
				sendto(p->fd, packet, head->caplen, 0, (struct sockaddr *)&to, sizeof(struct sockaddr));
			} else {
				write(p->fd, &(head->caplen), 4); /* First 4 bytes is the size */
				write(p->fd, packet, head->caplen);
			}
		}
	} else {
		/* This packet is for a specific mac */
		proxy_t *p;
		hnode_t *box;

		box = hash_lookup(xboxen, eptr->ether_dhost);
		if (box == NULL) {
			debuglog(3, "Unknown destination %s, hasn't been seen yet.", 
						ether_ntoa((struct ether_addr *)(eptr->ether_dhost)));
			return;
		}

		debuglog(3, "Found packet destined for %s! (len: %d)", 
						ether_ntoa((struct ether_addr *)(eptr->ether_dhost)),
						head->caplen);

		p = ((xbox_t *)(box->hash_data))->proxy;

		if (p == NULL) {
			debuglog(3, "Packet is destined for this net, ignore it");
			return;
		}

		debuglog(3, "Proxy is: %s", (p == NULL ? "Local" : inet_ntoa(p->addr)));

		if (use_udp) {
			struct sockaddr_in to;
			to.sin_addr = p->addr;
			to.sin_port = htons(serverport);
			to.sin_family = PF_INET;

			//sendto(p->fd, &(head->caplen), 4, 0, (struct sockaddr *)&to, sizeof(struct sockaddr));
			sendto(p->fd, packet, head->caplen, 0, (struct sockaddr *)&to, sizeof(struct sockaddr));
		} else {
			write(p->fd, &(head->caplen), 4); /* First 4 bytes is the size */
			write(p->fd, packet, head->caplen);
		}

	}

}

proxy_t *addproxy(struct sockaddr_in *addr, int sock) {
	proxy_t *newproxy = NULL;
	hnode_t *proxy = NULL;

	debuglog(5, "Data from proxy: %s", inet_ntoa(addr->sin_addr));

	proxy = hash_lookup(proxies, &(addr->sin_addr.s_addr));
	if (proxy == NULL) {
		hscan_t hs;
		hnode_t *node;

		newproxy = malloc(sizeof(proxy_t));
		newproxy->ip = (int) addr->sin_addr.s_addr;
		newproxy->addr = addr->sin_addr;
		newproxy->fd = sock;
		newproxy->xboxen = hash_create(64, comparemac, hashmac);

		debuglog(1, "NEW PROXY FOUND: %s", inet_ntoa(addr->sin_addr));
		hash_alloc_insert(proxies, &(newproxy->ip), newproxy);

		/* Check known proxy connections for data */
		hash_scan_begin(&hs, proxies);
		fprintf(stderr, "KNOWN PROXIES:\n");
		while ((node = hash_scan_next(&hs))) {
			proxy_t *p = (proxy_t *)(node->hash_data);
			struct sockaddr_in i;
			i.sin_addr.s_addr = *(int *)(node->hash_key);
			fprintf(stderr, "PROXY: %s\n", inet_ntoa(p->addr));
		}
		fprintf(stderr, "END\n");


		return newproxy;
	} else {
		debuglog(15, "Packet from known proxy...");
		return (proxy->hash_data);
	}
}


void addxbox(u_char *macaddr, proxy_t *ppt) {
	xbox_t *newbox = NULL;
	hnode_t *box = NULL;

	box = hash_lookup(xboxen, macaddr);
	if (box == NULL) {
		hscan_t hs;
		hnode_t *node;

		//fprintf(stderr, "NEW XBOX FOUND: %s / LOCATION: %s\n", 
				  //ether_ntoa((struct ether_addr *)macaddr),
				  //(ppt == NULL ? "Local" : inet_ntoa(ppt->addr)));


		debuglog(1, "NEW XBOX FOUND: %s", ether_ntoa((struct ether_addr *)macaddr));
		debuglog(1, "\tLocation: %s", (ppt == NULL ? "Local" : inet_ntoa(ppt->addr)));
		newbox = malloc(sizeof(xbox_t));
		memcpy(newbox->macaddr,macaddr,ETHER_ADDR_LEN);
		//fprintf(stderr, "Stored macaddr: %s\n\n", ether_ntoa((struct ether_addr *)(newbox->macaddr)));
		newbox->lastseen = time(NULL);
		newbox->proxy = ppt;
		//hash_alloc_insert(xboxen, macaddr, newbox);
		hash_alloc_insert(xboxen, newbox->macaddr, newbox);
		if (ppt != NULL) 
			hash_alloc_insert(ppt->xboxen, newbox->macaddr, newbox);

		/* Check known proxy connections for data */
		hash_scan_begin(&hs, xboxen);
		fprintf(stderr, "KNOWN XBOXES:\n");
		while ((node = hash_scan_next(&hs))) {
			xbox_t *x = (xbox_t *)(node->hash_data);
			fprintf(stderr, "XBOX: %s\n", ether_ntoa((struct ether_addr *)x->macaddr));
		}
		fprintf(stderr, "END\n");
	}
}

void pcap(void *args) {
	pcap_t *handle;
	struct bpf_program filter;
	char errbuf[PCAP_ERRBUF_SIZE];
	bpf_u_int32	mask, net;

	pcap_lookupnet(pcapdev, &net, &mask, errbuf);

	/* A generous cap length */
	handle = pcap_open_live(pcapdev, 2048, 1, 10, errbuf);

	if (handle == NULL) {
		debuglog(0, "Error trying to open %s: %s", pcapdev, errbuf);
		//return 1;
		pthread_exit(NULL);
	}

#ifdef LIBNET_VERSION_1_0
	libnet = libnet_open_link_interface(pcapdev, errbuf);
	if (libnet == NULL) {
		debuglog(0, "libnet_open_link_interface() failed: %s", errbuf);
		pthread_exit(NULL);
	}
#else
#	ifdef LIBNET_VERSION_1_1
	//libnet = libnet_init(LIBNET_LINK, pcapdev, errbuf);
	libnet = libnet_init(LIBNET_LINK_ADV, pcapdev, errbuf);
	if (libnet == NULL) {
		debuglog(0, "libnet_init() failed: %s", errbuf);
		pthread_exit(NULL);
	}
#	endif
#endif

	debuglog(2, "Opened %s, listening for xbox traffic", pcapdev);

	pcap_compile(handle, &filter, filter_app, 1, net);
	pcap_setfilter(handle, &filter);
	pcap_loop(handle, -1, packet_handler, NULL);

	pcap_close(handle);

	pthread_exit(NULL);
}

void proxy(void *args) {
	int server;
	struct sockaddr_in serveraddr;

	FD_ZERO(&proxysocks);

	if (use_udp) {
		server = socket(PF_INET, SOCK_DGRAM, 0);
	} else {
		server = socket(PF_INET, SOCK_STREAM, 0);
	}
	serveraddr.sin_family = PF_INET;
	serveraddr.sin_addr.s_addr = INADDR_ANY;
	serveraddr.sin_port = htons(serverport);
	
	debuglog(5, "Binding to any on port %d", serverport);
	if (bind(server, (struct sockaddr *)&serveraddr, sizeof(struct sockaddr)) == -1) {
		debuglog(0, "bind() failed: %s", strerror(errno));
		pthread_exit(NULL);
	}

	if (!use_udp) {
		if (listen(server, SOMAXCONN) < 0) {
			debuglog(0, "listen() failed: %s", strerror(errno));
			debuglog(0, "UDP: %d", use_udp);
			pthread_exit(NULL);
		}
	}

	debuglog(1, "Listening on port %d", serverport);

	FD_SET(server, &proxysocks);

	/* If server is non-null, connect to that server and listen for data on it */
	if (proxyserver != NULL) {
		connect_to_proxy();
	}

	for (;;) {
		int fd;
		struct sockaddr_in srcaddr;
		int size = sizeof(struct sockaddr_in); /* sizeof(srcaddr) */
		int sel;
		struct timeval timeout;
		timeout.tv_sec = 0;
		timeout.tv_usec = 0;
		fd_set proxycopy;

		/* Some sort of select() thing here */

		proxycopy = proxysocks;

		FD_SET(server, &proxysocks);
		debuglog(150, "select: %08x", proxysocks);
		sel = select(FD_SETSIZE, &proxysocks, NULL, NULL, &timeout);
		if (sel < 0) {
			debuglog(0, "select() failed: %s", strerror(errno));
			pthread_exit(NULL);
		} else if (sel == 0) {
			debuglog(150, "select() timed out waiting for data... trying again");
			proxysocks = proxycopy;
		} else {
			hscan_t hs;
			hnode_t *node;

			debuglog(15, "Data ready from %d descriptors", sel);

			if (use_udp) {
				if (FD_ISSET(server, &proxysocks)) {
					proxy_t p;
					p.fd = server;
					debuglog(13, "Data received on udp");
					if (recv_from_proxy(&p) > 0 )
						FD_SET(server, &proxysocks);
					else
						FD_CLR(server, &proxysocks);
				} else {
					FD_SET(server, &proxysocks);
				}

			} else { /* if using tcp... */
				/* Check known proxy connections for data */
				hash_scan_begin(&hs, proxies);
				while ((node = hash_scan_next(&hs))) {
					proxy_t *p = (proxy_t *)(node->hash_data);
					if (FD_ISSET(p->fd, &proxysocks)) {
						debuglog(13, "Data received from %s", inet_ntoa(p->addr));
						if (recv_from_proxy(p) > 0 )
							FD_SET(p->fd, &proxysocks);
						else
							FD_CLR(p->fd, &proxysocks);
					} else {
						FD_SET(p->fd, &proxysocks);
					}
				}

				debuglog(14,"Data from a connected proxy client!");
				if (FD_ISSET(server, &proxysocks)) {
					//proxy_t *newproxy;

					if ((fd = accept(server, (struct sockaddr *)&srcaddr, &size)) < 0) {
						debuglog(0, "accept() failed: %s", strerror(errno));
						pthread_exit(NULL);
					}

					/* Check bufsize... */
					do {
						int bufsiz;
						int sizeofthing = 4;
						getsockopt(fd, SOL_SOCKET, SO_RCVBUF, &bufsiz, &sizeofthing);
						debuglog(0, "Socket rcvbuf: %d", bufsiz);
						getsockopt(fd, SOL_SOCKET, SO_SNDBUF, &bufsiz, &sizeofthing);
						debuglog(0, "Socket sndbuf: %d", bufsiz);
					} while (0);

				
					debuglog(1, "New proxy connection from %s:%d",
								inet_ntoa(srcaddr.sin_addr), htons(srcaddr.sin_port));

					/*
					newproxy = malloc(sizeof(proxy_t));
					newproxy->ip = (int) srcaddr.sin_addr.s_addr;
					newproxy->addr = srcaddr.sin_addr;
					newproxy->fd = fd;
					newproxy->xboxen = hash_create(64, comparemac, hashmac);

					hash_alloc_insert(proxies, &(newproxy->ip), newproxy);
					*/
					addproxy(&srcaddr, fd);
					FD_SET(fd, &proxysocks);
				} 
			} /* else if using udp */
		} /* else, selected data is available */
	} /* infinite loop */
}

void connect_to_proxy() {
	int sock;
	struct sockaddr_in destaddr;
	struct hostent *hostdata;
	struct in_addr in;

	//proxy_t *newproxy;

	if (use_udp) {
		if ((sock = socket(PF_INET, SOCK_DGRAM, 0)) < 0) {
			debuglog(0, "socket() failed: %s", strerror(errno));
			pthread_exit(NULL);
		}
	} else {
		if ((sock = socket(PF_INET, SOCK_STREAM, 0)) < 0) {
			debuglog(0, "socket() failed: %s", strerror(errno));
			pthread_exit(NULL);
		}
	}

	if ((hostdata = gethostbyname(proxyserver))== NULL) {
		debuglog(0, "gethostbyname() failed: %s", strerror(errno));
		pthread_exit(NULL);
	}

	/* Grab the IP this hostname is, this is an ugly oneliner */
	memcpy(&(in.s_addr),hostdata->h_addr_list[0],hostdata->h_length);

	debuglog(1, "%s is %s (%s)", proxyserver, hostdata->h_name, inet_ntoa(in));
				//inet_ntoa((struct in_addr *)(hostdata->h_addr_list[0])));

	destaddr.sin_family = PF_INET;
	destaddr.sin_addr = in;
	destaddr.sin_port = htons(serverport);

	if (use_udp) {
		if (sendto(sock, NULL, 0, 0, (struct sockaddr *)&destaddr, sizeof(struct sockaddr)) < 0) {
			debuglog(0, "sendto() failed [while saying hi]: %s", strerror(errno));
			pthread_exit(NULL);
		}
	} else {
		if (connect(sock, (struct sockaddr *)&destaddr, sizeof(struct sockaddr))) {
			debuglog(0, "connect() failed: %s", strerror(errno));
			pthread_exit(NULL);
		}
	} 


	/* After we establish a connection with the proxy server host,
	 * we should completely forget about this relationship 
	 * (us being the client). Now, add it to the list of
	 * known proxies and move along.
	 */
	/*
	newproxy = malloc(sizeof(proxy_t));
	newproxy->ip = (int) destaddr.sin_addr.s_addr;
	newproxy->addr = destaddr.sin_addr;
	newproxy->fd = sock;
	newproxy->xboxen = hash_create(64, comparemac, hashmac);

	hash_alloc_insert(proxies, &(newproxy->ip), newproxy);
	*/
	addproxy(&destaddr, sock);
	FD_SET(sock, &proxysocks);
}

int recv_from_proxy(proxy_t *ppt) {
	int bytes = 0;
	int pktlen = 0;
	int total;

	char *packet;

	if (use_udp) {
		bytes = 4;
		pktlen = 8192;
	} else {
		bytes = recv(ppt->fd, &pktlen, 4, 0);
		debuglog(100, "Bytes read: [%d] %d", bytes, pktlen);

		/* if bytes read is 0, then the connection was closed. */
		if (bytes < 0) {
			debuglog(0, "recv_from_proxy - recv() (1) failed: %s", strerror(errno));
			return bytes;
		} else if (bytes == 0) {
			remove_proxy(ppt);
			return 0;
		}
	}

	packet = malloc(pktlen);

	if (use_udp) {
		struct sockaddr_in from;
		int len = sizeof(struct sockaddr);
		bytes = recvfrom(ppt->fd, packet, pktlen, 0, (struct sockaddr *)&from, &len);
		pktlen = bytes; /* XXX: Is this a good idea? */

		ppt = addproxy(&from, ppt->fd);
	} else {
		bytes = recv(ppt->fd, packet, pktlen, 0);
	}

	if (bytes < 0) {
		if (use_udp)
			debuglog(0, "recv_from_proxy - recvfrom() (2) failed: %s", strerror(errno));
		else
			debuglog(0, "recv_from_proxy - recv() (2) failed: %s", strerror(errno));
		return bytes;
	} if (bytes == 0) {
		if (!use_udp) 
			remove_proxy(ppt);
		return 0;
	}

	//debuglog(1, "Adding xbox in  recv_from_proxy");
	addxbox(((struct ether_header *)packet)->ether_shost, ppt);

	debuglog(1, "Packet received from %s. Length: %d vs %d (%s)", 
				inet_ntoa(ppt->addr), bytes, pktlen,
				(bytes == pktlen ? "OK" : "FRAGMENTED"));
	if (bytes < pktlen) {
		total = bytes;
		while (total < pktlen) {
			bytes = recv(ppt->fd, packet + total, pktlen - total, 0);
			debuglog(1, "REASSEMBLY PROGRESS - %s. Length: %d vs %d", inet_ntoa(ppt->addr), total, pktlen);
			total += bytes;
		}
	}

	distribute_packet(ppt, packet, bytes);

	return bytes;
}

void distribute_packet(proxy_t *ppt, char *packet, int pktlen) {
	struct ether_header *eptr;
	u_short ether_type;
	int bytes = 0;

	eptr = (struct ether_header *)packet;
	ether_type = ntohs(eptr->ether_type);

	debuglog(30, "----- REMOTE PACKET From: %s", ether_ntoa((struct ether_addr *)eptr->ether_shost));
	debuglog(30, "----- REMOTE PACKET To: %s", ether_ntoa((struct ether_addr *)eptr->ether_dhost));

#ifdef LIBNET_VERSION_1_0
	bytes = libnet_write_link_layer(libnet, pcapdev, packet, pktlen);
#else
#	ifdef LIBNET_VERSION_1_1
	bytes = libnet_adv_write_link(libnet, packet, pktlen);
#	endif
#endif

	if (bytes < 0) {
		debuglog(0, "FATAL ERROR WRITING RAW PACKET TO DEVICE");
	} else {
		debuglog(4, "Packet dumped on local net (%s), bytes vs length = %d vs %d", 
					pcapdev, bytes, pktlen);
	}

	if (get_log_level() > 10)
		hexdump(packet, pktlen);
}

void remove_proxy(proxy_t *ppt) {
	hscan_t hs;
	hnode_t *node;

	debuglog(1, "Removing proxy %s", inet_ntoa(ppt->addr));

	hash_scan_begin(&hs, proxies);
	while ((node = hash_scan_next(&hs))) {
		proxy_t *p = (proxy_t *)(node->hash_data);
		if (ppt->ip == p->ip) {
			hash_delete(proxies, node);
			break;
		}
	}

}

int main(int argc, char **argv) {
	char errbuf[PCAP_ERRBUF_SIZE];
	//int *pthread_return;
	char ch;

	/* Initialization and Defaults */
	xboxen = hash_create(64, comparemac, hashmac);
	proxies = hash_create(64, compareip, haship);
	set_log_level(0);

	pthread_t pcapthread, proxythread;

	/* Argument Processing */
	while ((ch = getopt(argc, argv, "us:i:d:h?p:")) != -1) {
		switch (ch) {
			case 'd':
				set_log_level(atoi(optarg));
				break;
			case 'u':
				debuglog(10, "-u flag, enabling udp");
				use_udp = 1;
				break;
			case 'i':
				pcapdev = malloc(strlen(optarg) + 1);
				strlcpy(pcapdev,optarg,strlen(optarg) + 1);
				debuglog(10, "-i flag, setting device to %s", pcapdev);
				break;
			case 's':
				proxyserver = malloc(strlen(optarg) + 1);
				strlcpy(proxyserver,optarg,strlen(optarg) + 1);
				debuglog(10, "-s flag, setting proxy server to %s", proxyserver);
				break;
			case 'p':
				serverport = atoi(optarg);
				break;
			case 'h':
			case '?':
			default:
				//usage();
				break;
		}
	}

	argc -= optind;
	argv += optind;

	if (pcapdev == NULL) 
		pcapdev = pcap_lookupdev(errbuf);

	if (pcapdev == NULL) {
		debuglog(0, "No device specified or unable to find a device to open.");
		debuglog(0, "Error message: %s", errbuf);
		return 1;
	}

	pthread_create(&pcapthread, NULL, (void*)pcap, NULL);
	pthread_create(&proxythread, NULL, (void*)proxy, NULL);

	pthread_join(pcapthread, NULL);
	debuglog(0, "pcap thread exited unexpectedly");
	exit(1);
	pthread_join(proxythread, NULL);

	return 0;
}