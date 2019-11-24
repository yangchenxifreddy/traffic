#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <inttypes.h>
#include <sys/types.h>
#include <sys/queue.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <setjmp.h>
#include <stdarg.h>
#include <ctype.h>
#include <errno.h>
#include <getopt.h>
#include <signal.h>
#include <stdbool.h>
#include <unistd.h>

#include <rte_common.h>
#include <rte_log.h>
#include <rte_memory.h>
#include <rte_memcpy.h>
#include <rte_memzone.h>
#include <rte_eal.h>
#include <rte_per_lcore.h>
#include <rte_launch.h>
#include <rte_atomic.h>
#include <rte_cycles.h>
#include <rte_prefetch.h>
#include <rte_lcore.h>
#include <rte_per_lcore.h>
#include <rte_branch_prediction.h>
#include <rte_interrupts.h>
#include <rte_pci.h>
#include <rte_random.h>
#include <rte_debug.h>
#include <rte_ether.h>
#include <rte_arp.h>
#include <rte_ip.h>
#include <rte_tcp.h>
#include <rte_udp.h>
#include <rte_icmp.h>
#include <rte_ethdev.h>
#include <rte_ring.h>
#include <rte_mempool.h>
#include <rte_mbuf.h>

#include "pcapng.h"

static volatile bool force_quit;

#define RTE_LOGTYPE_L2FWD RTE_LOGTYPE_USER1

#define MAX_PKT_BURST 256
#define NB_MBUF   8192 - 1
#define CAPTURE_RX_QUEUE_PER_LCORE 1
#define CAPTURE_TX_QUEUE_PER_LCORE 1
#define CAPTURE_RING_SIZE 2048 * 4
#define MEMPOOL_CACHE_SIZE 1024

struct capture_port_conf {
	uint8_t dst_port;
	struct rte_ether_addr eth_addr;
	struct rte_ring *worker_ring;
	struct rte_ring *tx_ring;
} __rte_cache_aligned;
struct capture_port_conf capture_port_conf[RTE_MAX_ETHPORTS];
/* mask of enabled ports */
static uint32_t capture_enabled_port_mask = 0;

static int save_pcapng_enabled = 0;

static uint64_t pcapng_not_saved_count = 0;
static uint64_t pcapng_saved_count = 0;

/*
 * Configurable number of RX/TX ring descriptors
 */
#define RTE_TEST_RX_DESC_DEFAULT 128
#define RTE_TEST_TX_DESC_DEFAULT 512
static uint16_t nb_rxd = RTE_TEST_RX_DESC_DEFAULT;
static uint16_t nb_txd = RTE_TEST_TX_DESC_DEFAULT;

struct mbuf_table {
	unsigned len;
	struct rte_mbuf *m_table[MAX_PKT_BURST];
};

#define MAX_RX_QUEUE_PER_LCORE 16
#define MAX_TX_QUEUE_PER_PORT 16
struct lcore_queue_conf {
	uint8_t n_port;
	uint8_t port_list[MAX_RX_QUEUE_PER_LCORE];
} __rte_cache_aligned;
struct lcore_queue_conf lcore_queue_conf[RTE_MAX_LCORE];

static struct rte_eth_conf port_conf;

static enum lcore_type_conf
{
	unused = 0,
	rx,
	tx,
	worker,
	master
}lcore_type_conf[RTE_MAX_LCORE]; //0 for unused, 1 for RX, 2 for TX, 3 for worker

struct rte_mempool *capture_pktmbuf_pool = NULL;

static void
pcapng_header_write(FILE *fp)
{
	struct block_header bh;
	struct section_header_block shb;
	struct block_trailer bt;
	struct interface_description_block idb;
	struct option_header oh;
	uint32_t msb = 6;

	bh.block_type = BT_SHB;
	bh.total_length = 28;
	fwrite(&bh, 1, sizeof(struct block_header), fp);
	shb.byte_order_magic = BYTE_ORDER_MAGIC;
	shb.major_version = PCAP_NG_VERSION_MAJOR;
	shb.minor_version = PCAP_NG_VERSION_MINOR;
	shb.section_length = 0xFFFFFFFFFFFFFFFF;
	fwrite(&shb, 1, sizeof(struct section_header_block), fp);
	bt.total_length = 28;
	fwrite(&bt, 1, sizeof(struct block_trailer), fp);

	bh.block_type = BT_IDB;
	bh.total_length = 32;
	fwrite(&bh, 1, sizeof(struct block_header), fp);
	idb.linktype = 1;
	idb.reserved = 0;
	idb.snaplen = 0xFFFF;
	fwrite(&idb, 1, sizeof(struct interface_description_block), fp);
	oh.option_code = 9;
	oh.option_length = 1;
	fwrite(&oh, 1, sizeof(struct option_header), fp);
	fwrite(&msb, 1, sizeof(uint32_t), fp);
	oh.option_code = OPT_ENDOFOPT;
	oh.option_length = 0;
	fwrite(&oh, 1, sizeof(struct option_header), fp);
	bt.total_length = 32;
	fwrite(&bt, 1, sizeof(struct block_trailer), fp);
}

static void
pcapng_epb_write(FILE *fp, uint8_t *packet, uint64_t pkt_len, uint64_t arrival_time)
{
	struct block_header bh;
	struct enhanced_packet_block epb;
	struct block_trailer bt;
	int ret = 0;
	uint32_t offset = 0;
	uint32_t padding = 0;
	uint32_t fakepadding = 0;

	offset = pkt_len;
	padding = offset % 4 ? (4 - offset % 4) : 0;
	bh.block_type = BT_EPB;
	bh.total_length = 32 + offset + padding;
	bt.total_length = 32 + offset + padding;
	epb.interface_id = 0;
	epb.timestamp_high = (uint32_t)(arrival_time >> 32);
	epb.timestamp_low = (uint32_t)arrival_time;
	epb.caplen = offset;
	epb.len = offset;
	fwrite(&bh, 1, sizeof(struct block_header), fp);
	fwrite(&epb, 1, sizeof(struct enhanced_packet_block), fp);
	ret = fwrite(packet, 1, pkt_len, fp);
	ret += fwrite(&fakepadding, 1, padding, fp);
	fwrite(&bt, 1, sizeof(struct block_trailer), fp);
}

static int
print_stats(void)
{
	struct rte_eth_stats eth_stats;
	int ret;
	unsigned portid;
	uint8_t nb_ports = 0;
	const char clr[] = { 27, '[', '2', 'J', '\0' };
	const char topLeft[] = { 27, '[', '1', ';', '1', 'H','\0' };

	nb_ports = rte_eth_dev_count();

	while(!force_quit)
	{
		/* Clear screen and move to top left */
		printf("%s%s", clr, topLeft);

		for (portid = 0; portid < nb_ports; portid++) {
			if ((capture_enabled_port_mask & (1 << portid)) == 0)
				continue;
			ret = rte_eth_stats_get(portid, &eth_stats);
			if (ret == 0)
			{
				printf("\nPort %u stats:\n", portid);
		        printf(" - Pkts in:   %"PRIu64"\n", eth_stats.ipackets);
		        printf(" - Pkts out:  %"PRIu64"\n", eth_stats.opackets);
		        printf(" - In Errs:   %"PRIu64"\n", eth_stats.ierrors);
		        printf(" - Out Errs:  %"PRIu64"\n", eth_stats.oerrors);
		        printf(" - Mbuf Errs: %"PRIu64"\n", eth_stats.rx_nombuf);
			}

		}
		printf(" - Pcapng saved count: %"PRIu64"\n", pcapng_saved_count);
		printf(" - Pcapng not saved count: %"PRIu64"\n", pcapng_not_saved_count);
		sleep(3);
	}
	return 0;
}

/* rx processing loop */
static void
capture_rx_loop(void)
{
	struct rte_mbuf *pkts_burst[MAX_PKT_BURST];
	unsigned lcore_id;
	unsigned i, j, portid, nb_rx;
	struct lcore_queue_conf *qconf;
	uint32_t eqnum;
	struct timeval tv;

	lcore_id = rte_lcore_id();
	qconf = &lcore_queue_conf[lcore_id];

	RTE_LOG(INFO, L2FWD, "entering rx loop on lcore %u\n", lcore_id);

	while (!force_quit) {
		/*
		 * Read packet from RX queues
		 */
		for (i = 0; i < qconf->n_port; i++) {

			portid = qconf->port_list[i];
			nb_rx = rte_eth_rx_burst((uint8_t) portid, 0,
						 pkts_burst, MAX_PKT_BURST);

			for (j = 0; j < nb_rx; j++) {
				struct rte_mbuf *m = pkts_burst[j];
				rte_prefetch0(rte_pktmbuf_mtod(m, void *));
				gettimeofday(&tv, NULL);
				m->udata64 = tv.tv_sec * 1000000 + tv.tv_usec;
			}

			eqnum = rte_ring_enqueue_burst(capture_port_conf[portid].worker_ring, (void **)pkts_burst, nb_rx,NULL);

			if (unlikely(eqnum < nb_rx))
			{
				unsigned k;
				for (k = eqnum; k < nb_rx; k++)
				{
					struct rte_mbuf *m = pkts_burst[k];
					rte_pktmbuf_free(m);
				}
			}
		}
	}
}

/* tx processing loop */
static void
capture_tx_loop(void)
{
	struct rte_mbuf *mbufs[MAX_PKT_BURST];
	unsigned lcore_id;
	unsigned i, portid, nb_tx;
	struct lcore_queue_conf *qconf;
	uint32_t dqnum;

	lcore_id = rte_lcore_id();
	qconf = &lcore_queue_conf[lcore_id];

	RTE_LOG(INFO, L2FWD, "entering tx loop on lcore %u\n", lcore_id);

	while (!force_quit) {
		/*
		 * Send packet to TX queues
		 */
		for (i = 0; i < qconf->n_port; i++) {
			portid = qconf->port_list[i];
			dqnum = rte_ring_dequeue_burst(capture_port_conf[portid].tx_ring, (void *)mbufs, MAX_PKT_BURST,NULL);
			if (unlikely(dqnum == 0))
                continue;

			nb_tx = rte_eth_tx_burst((uint8_t) portid, 0, mbufs, dqnum);

			if (unlikely(nb_tx < dqnum))
			{
				unsigned k;
				for (k = nb_tx; k < dqnum; k++)
				{
					struct rte_mbuf *m = mbufs[k];
					rte_pktmbuf_free(m);
				}
			}
		}
	}
}

/* worker processing loop */
static void
capture_worker_loop(void)
{
	struct rte_mbuf *mbufs[MAX_PKT_BURST];
	struct rte_mbuf *m;
	unsigned lcore_id;
	unsigned i, j, portid;
	struct lcore_queue_conf *qconf;
	uint32_t dqnum, eqnum;
	uint8_t dst_port;
	uint8_t *packet;

	FILE *fp = NULL;

	lcore_id = rte_lcore_id();
	qconf = &lcore_queue_conf[lcore_id];

	RTE_LOG(INFO, L2FWD, "entering worker loop on lcore %u\n", lcore_id);

	if(save_pcapng_enabled)
	{
		fp = fopen("port.pcapng", "wb");
		if (fp == NULL)
		{
			printf("Cannot open pcapng file.\n");
			save_pcapng_enabled = 0;
		}
		else
			pcapng_header_write(fp);
	}
	else
	{
		printf("Pcapng save not enabled.\n");
	}

	while (!force_quit) {
		/*
		 * Read packet from worker ring
		 */
		for (i = 0; i < qconf->n_port; i++) {

			portid = qconf->port_list[i];
			dqnum = rte_ring_dequeue_burst(capture_port_conf[portid].worker_ring, (void *)mbufs, MAX_PKT_BURST,NULL);
			dst_port = capture_port_conf[portid].dst_port;

			if (unlikely(dqnum == 0))
                continue;

            for (j = 0; j < dqnum; j++)
            {
            	m = mbufs[j];

            	if ((fp != NULL && rte_ring_count(capture_port_conf[portid].worker_ring) < MAX_PKT_BURST) && save_pcapng_enabled)
            	{
            		packet = rte_pktmbuf_mtod(m, uint8_t *);
            		pcapng_epb_write(fp, packet, rte_pktmbuf_data_len(m), m->udata64);
            		pcapng_saved_count++;
            		printf("pcapng_saved_count=%d \n",pcapng_saved_count);
            	}
            	else
            	{
            		pcapng_not_saved_count++;
            	}

            	if(unlikely(dst_port > RTE_MAX_ETHPORTS))
            	{
            		rte_pktmbuf_free(m);
            		continue;
            	}
            }

            if(unlikely(dst_port > RTE_MAX_ETHPORTS))
            	continue;

            
            eqnum = rte_ring_enqueue_burst(capture_port_conf[dst_port].tx_ring, (void **)mbufs, dqnum,NULL);


			if (unlikely(eqnum < dqnum))
			{
				unsigned k;
				for (k = eqnum; k < dqnum; k++)
				{
					struct rte_mbuf *m_temp = mbufs[k];
					rte_pktmbuf_free(m_temp);
				}
			}
		}
	}

	if (fp != NULL)
		fclose(fp);
}



static int
capture_launch_one_lcore(__attribute__((unused)) void *dummy)
{
	uint8_t lcore_id = rte_lcore_id();

	switch(lcore_type_conf[lcore_id])
	{
		case rx:
			printf("Lcore: %u Type: RX\n", lcore_id);
			capture_rx_loop();
			break;
		case tx:
			printf("Lcore: %u Type: TX\n", lcore_id);
			capture_tx_loop();
			break;
		case worker:
			printf("Lcore: %u Type: WORKER\n", lcore_id);
			capture_worker_loop();
			break;
		case master:
			printf("Lcore: %u Type: MASTER\n", lcore_id);
			print_stats();
			break;
		default:
			printf("Lcore: %u Type: unused....exit\n", lcore_id);
			break;
	}
	return 0;
}

static void
capture_usage(const char *prgname)
{
	printf("%s [EAL options] -- -p PORTMASK -w\n"
	       "  -p PORTMASK: hexadecimal bitmask of ports to configure\n"
	       "  -w : enable pcapng file save\n",
	       prgname);
}

static int
capture_parse_portmask(const char *portmask)
{
	char *end = NULL;
	unsigned long pm;

	/* parse hexadecimal string */
	pm = strtoul(portmask, &end, 16);
	if ((portmask[0] == '\0') || (end == NULL) || (*end != '\0'))
		return 0;

	return pm;
}

/* Parse the argument given in the command line of the application */
static int
capture_parse_args(int argc, char **argv)
{
	int opt, ret;
	char **argvopt;
	int option_index;
	char *prgname = argv[0];
	static struct option lgopts[] = {
		{NULL}
	};

	argvopt = argv;

	while ((opt = getopt_long(argc, argvopt, "p:w",
				  lgopts, &option_index)) != EOF) {

		switch (opt) {
		/* portmask */
		case 'p':
			capture_enabled_port_mask = capture_parse_portmask(optarg);
			if (capture_enabled_port_mask == 0) {
				printf("invalid portmask\n");
				capture_usage(prgname);
				return -1;
			}
			break;

		case 'w':
			save_pcapng_enabled = 1;
			break;

		/* long options */
		case 0:
		default:
			capture_usage(prgname);
			return -1;
		}
	}

	if (optind >= 0)
		argv[optind-1] = prgname;

	ret = optind-1;
	optind = 0; /* reset getopt lib */
	return ret;
}

/* Check the link status of all ports in up to 9s, and print them finally */
static void
check_all_ports_link_status(uint8_t port_num, uint32_t port_mask)
{
#define CHECK_INTERVAL 100 /* 100ms */
#define MAX_CHECK_TIME 90 /* 9s (90 * 100ms) in total */
	uint8_t portid, count, all_ports_up, print_flag = 0;
	struct rte_eth_link link;

	printf("\nChecking link status");
	fflush(stdout);
	for (count = 0; count <= MAX_CHECK_TIME; count++) {
		all_ports_up = 1;
		for (portid = 0; portid < port_num; portid++) {
			if ((port_mask & (1 << portid)) == 0)
				continue;
			memset(&link, 0, sizeof(link));
			rte_eth_link_get_nowait(portid, &link);
			/* print link status if flag set */
			if (print_flag == 1) {
				if (link.link_status)
					printf("Port %d Link Up - speed %u "
						"Mbps - %s\n", (unsigned) portid,
						(unsigned)link.link_speed,
				(link.link_duplex == ETH_LINK_FULL_DUPLEX) ?
					("full-duplex") : ("half-duplex\n"));
				else
					printf("Port %d Link Down\n",
						(unsigned) portid);
				continue;
			}
			/* clear all_ports_up flag if any link down */
			if (link.link_status == 0) {
				all_ports_up = 0;
				break;
			}
		}
		/* after finally printing all link status, get out */
		if (print_flag == 1)
			break;

		if (all_ports_up == 0) {
			printf(".");
			fflush(stdout);
			rte_delay_ms(CHECK_INTERVAL);
		}

		/* set the print_flag if all ports up or timeout */
		if (all_ports_up == 1 || count == (MAX_CHECK_TIME - 1)) {
			print_flag = 1;
			printf("... done\n");
		}
	}
}

static void
signal_handler(int signum)
{
	if (signum == SIGINT || signum == SIGTERM) {
		printf("\n\nSignal %d received, preparing to exit...\n",
				signum);
		force_quit = true;
	}
}


int main(int argc, char **argv)
{
	struct lcore_queue_conf *qconf;
	struct rte_eth_dev_info dev_info;
	uint8_t portid = 0, rx_lcore_id = 0, tx_lcore_id = 0, m_lcore_id, lcore_id, worker_lcore_id;
	uint8_t nb_ports = 0, last_port = 0, nb_ports_in_mask = 0;
	uint8_t nb_ports_available;
	int ret;
	char name[128];

		port_conf.rxmode.split_hdr_size = 0;
		//port_conf.rxmode.header_split   = 0;/**< Header Split disabled */
		//port_conf.rxmode.hw_ip_checksum = 0;/**< IP checksum offload disabled */
		//port_conf.rxmode.hw_vlan_filter = 0;/**< VLAN filtering disabled */
		//port_conf.rxmode.jumbo_frame    = 0;/**< Jumbo Frame Support disabled */
		//port_conf.rxmode.hw_strip_crc   = 0;/**< CRC stripped by hardware */
		port_conf.txmode .mq_mode = ETH_MQ_TX_NONE;

		port_conf.rxmode.split_hdr_size = 0l;
		//port_conf.rxmode.ignore_offload_bitfield = 1;
		//port_conf.rxmode.offloads = DEV_RX_OFFLOAD_CRC_STRIP;
		//port_conf.txmode.mq_mode = ETH_MQ_TX_NONE;

	char **argvlist;
	
	unsigned int paramid= 0,argcsize= 10;
	int l2fwd_enabled_port_mask= 0x01;	
	int l2fwd_rx_queue_per_lcore= 0x04;

	argvlist= (char**)malloc(argcsize* sizeof(char*));
	argvlist[paramid]= (char*)malloc(16*sizeof(char));
	strcpy(argvlist[paramid++],"TrafficMonitor");

	argvlist[paramid]= (char*)malloc(16*sizeof(char));
	strcpy(argvlist[paramid++],"-c");

	argvlist[paramid]= (char*)malloc(16*sizeof(char));
	sprintf(argvlist[paramid++],"%d",7);


	argvlist[paramid]= (char*)malloc(16*sizeof(char));
	strcpy(argvlist[paramid++],"-n");

	argvlist[paramid]= (char*)malloc(16*sizeof(char));
	sprintf(argvlist[paramid++],"%d",4);

// 
	argvlist[paramid]= (char*)malloc(16*sizeof(char));
	strcpy(argvlist[paramid++],"--");


	argvlist[paramid]= (char*)malloc(16*sizeof(char));
	strcpy(argvlist[paramid++],"-q");

	argvlist[paramid]= (char*)malloc(16*sizeof(char));
	sprintf(argvlist[paramid++],"%d",l2fwd_rx_queue_per_lcore);


	argvlist[paramid]= (char*)malloc(16*sizeof(char));
	strcpy(argvlist[paramid++],"-p");

	argvlist[paramid]= (char*)malloc(16*sizeof(char));
	sprintf(argvlist[paramid++],"%d",l2fwd_enabled_port_mask);
printf("  %\n\n ");
int i=0;
while(i<argcsize)
{
//for(int i=0;i<argcsize;++i)
printf("  %s  ",argvlist[i++]);
}
printf("  %\n\n ");

	/* init EAL */
	ret = rte_eal_init(argcsize, argvlist);
		rte_exit(EXIT_FAILURE, "Invalid EAL arguments\n");
	argcsize -= ret;
	argvlist += ret;

	force_quit = false;
	signal(SIGINT, signal_handler);
	signal(SIGTERM, signal_handler);

	/* parse application arguments (after the EAL ones) */
	ret = capture_parse_args(argcsize, argvlist);
	if (ret < 0)
		rte_exit(EXIT_FAILURE, "Invalid CAPTURE arguments\n");

	/* create the mbuf pool */
	capture_pktmbuf_pool = rte_pktmbuf_pool_create("mbuf_pool", NB_MBUF, MEMPOOL_CACHE_SIZE,
		0, RTE_MBUF_DEFAULT_BUF_SIZE, rte_socket_id());
	if (capture_pktmbuf_pool == NULL)
		rte_exit(EXIT_FAILURE, "Cannot init mbuf pool\n");

	nb_ports = rte_eth_dev_count();
	if (nb_ports == 0)
		rte_exit(EXIT_FAILURE, "No Ethernet ports - bye\n");

	/*
	 * Each logical core is assigned a dedicated TX queue on each port.
	 */
	for (portid = 0; portid < nb_ports; portid++)
	{
		/* skip ports that are not enabled */
		if ((capture_enabled_port_mask & (1 << portid)) == 0)
			continue;

		if (nb_ports_in_mask % 2)
		{
			capture_port_conf[portid].dst_port = last_port;
			capture_port_conf[last_port].dst_port = portid;
			snprintf(name, sizeof(name), "capture_port%u_tx_ring", portid);
			capture_port_conf[portid].tx_ring = rte_ring_create(name, CAPTURE_RING_SIZE, rte_socket_id(), RING_F_SP_ENQ | RING_F_SC_DEQ);
			snprintf(name, sizeof(name), "capture_port%u_worker_ring", portid);
			capture_port_conf[portid].worker_ring = rte_ring_create(name, CAPTURE_RING_SIZE, rte_socket_id(), RING_F_SP_ENQ | RING_F_SC_DEQ);
			snprintf(name, sizeof(name), "capture_port%u_tx_ring", last_port);
			capture_port_conf[last_port].tx_ring = rte_ring_create(name, CAPTURE_RING_SIZE, rte_socket_id(), RING_F_SP_ENQ | RING_F_SC_DEQ);
			snprintf(name, sizeof(name), "capture_port%u_worker_ring", last_port);
			capture_port_conf[last_port].worker_ring = rte_ring_create(name, CAPTURE_RING_SIZE, rte_socket_id(), RING_F_SP_ENQ | RING_F_SC_DEQ);

			if(capture_port_conf[portid].tx_ring == NULL)
				rte_exit(EXIT_FAILURE, "Cannot create tx ring for port%u\n", portid);
			else if(capture_port_conf[portid].worker_ring == NULL)
				rte_exit(EXIT_FAILURE, "Cannot create worker ring for port%u\n", portid);
			else if(capture_port_conf[last_port].tx_ring == NULL)
				rte_exit(EXIT_FAILURE, "Cannot create tx ring for port%u\n", last_port);
			else if(capture_port_conf[last_port].worker_ring == NULL)
				rte_exit(EXIT_FAILURE, "Cannot create worker ring for port%u\n", last_port);
		}
		else
			last_port = portid;

		nb_ports_in_mask++;

		rte_eth_dev_info_get(portid, &dev_info);
	}

	if (nb_ports_in_mask % 2) {
		printf("Notice: odd number of ports in portmask. Entering RX only mode.\n");
		capture_port_conf[last_port].dst_port = RTE_MAX_ETHPORTS + 1;

		snprintf(name, sizeof(name), "capture_port%u_tx_ring", last_port);
		capture_port_conf[last_port].tx_ring = rte_ring_create(name, CAPTURE_RING_SIZE, rte_socket_id(), RING_F_SP_ENQ | RING_F_SC_DEQ);
		snprintf(name, sizeof(name), "capture_port%u_worker_ring", last_port);
		capture_port_conf[last_port].worker_ring = rte_ring_create(name, CAPTURE_RING_SIZE, rte_socket_id(), RING_F_SP_ENQ | RING_F_SC_DEQ);

		if(capture_port_conf[last_port].tx_ring == NULL)
			rte_exit(EXIT_FAILURE, "Cannot create tx ring for port%u\n", last_port);
		else if(capture_port_conf[last_port].worker_ring == NULL)
			rte_exit(EXIT_FAILURE, "Cannot create worker ring for port%u\n", last_port);
	}

	if (nb_ports_in_mask > 2)
		rte_exit(EXIT_FAILURE, "Number of Enabled ethernet ports must be less than 2\n");

	m_lcore_id = rte_get_master_lcore();
	lcore_type_conf[m_lcore_id] = master;

	/* Initialize the port/queue configuration of each logical core */
	for (portid = 0; portid < nb_ports; portid++) {
		/* skip ports that are not enabled */
		if ((capture_enabled_port_mask & (1 << portid)) == 0)
			continue;

		/* get the lcore_id for this port */
		while ((rte_lcore_is_enabled(rx_lcore_id) == 0 ||
		       lcore_queue_conf[rx_lcore_id].n_port ==
		       CAPTURE_RX_QUEUE_PER_LCORE) || rx_lcore_id == m_lcore_id) {
			rx_lcore_id++;
			if (rx_lcore_id >= RTE_MAX_LCORE)
				rte_exit(EXIT_FAILURE, "Not enough cores\n");
		}

		lcore_type_conf[rx_lcore_id] = rx;

		if (qconf != &lcore_queue_conf[rx_lcore_id])
			/* Assigned a new logical core in the loop above. */
			qconf = &lcore_queue_conf[rx_lcore_id];

		qconf->port_list[qconf->n_port] = portid;
		qconf->n_port++;
		printf("Lcore %u: RX port %u\n", rx_lcore_id, (unsigned) portid);

		tx_lcore_id = ++rx_lcore_id;

		/* get the lcore_id for this port */
		while ((rte_lcore_is_enabled(tx_lcore_id) == 0 ||
		       lcore_queue_conf[tx_lcore_id].n_port ==
		       CAPTURE_TX_QUEUE_PER_LCORE) || tx_lcore_id == m_lcore_id) {
			tx_lcore_id++;
			if (tx_lcore_id >= RTE_MAX_LCORE)
				rte_exit(EXIT_FAILURE, "Not enough cores\n");
		}

		lcore_type_conf[tx_lcore_id] = tx;

		if (qconf != &lcore_queue_conf[tx_lcore_id])
			/* Assigned a new logical core in the loop above. */
			qconf = &lcore_queue_conf[tx_lcore_id];

		qconf->port_list[qconf->n_port] = portid;
		qconf->n_port++;
		printf("Lcore %u: TX port %u\n", tx_lcore_id, (unsigned) portid);

	}

	worker_lcore_id = ++tx_lcore_id;

	while(rte_lcore_is_enabled(worker_lcore_id) == 0 || worker_lcore_id == m_lcore_id)
	{
		worker_lcore_id++;
		if (worker_lcore_id >= RTE_MAX_LCORE)
				rte_exit(EXIT_FAILURE, "Not enough cores\n");
	}

	/* Initialize the port/queue configuration of each logical core */
	for (portid = 0; portid < nb_ports; portid++) {
		/* skip ports that are not enabled */
		if ((capture_enabled_port_mask & (1 << portid)) == 0)
			continue;

		lcore_type_conf[worker_lcore_id] = worker;

		if (qconf != &lcore_queue_conf[worker_lcore_id])
			/* Assigned a new logical core in the loop above. */
			qconf = &lcore_queue_conf[worker_lcore_id];

		qconf->port_list[qconf->n_port] = portid;
		qconf->n_port++;
		printf("Lcore %u: Worker port %u\n", m_lcore_id, (unsigned) portid);
	}

	nb_ports_available = nb_ports;

	/* Initialise each port */
	for (portid = 0; portid < nb_ports; portid++) {
		/* skip ports that are not enabled */
		if ((capture_enabled_port_mask & (1 << portid)) == 0) {
			printf("Skipping disabled port %u\n", (unsigned) portid);
			nb_ports_available--;
			continue;
		}
		/* init port */
		printf("Initializing port %u... ", (unsigned) portid);
		fflush(stdout);
		ret = rte_eth_dev_configure(portid, 1, 1, &port_conf);
		if (ret < 0)
			rte_exit(EXIT_FAILURE, "Cannot configure device: err=%d, port=%u\n",
				  ret, (unsigned) portid);

		rte_eth_macaddr_get(portid,&capture_port_conf[portid].eth_addr);

		/* init one RX queue */
		fflush(stdout);
		ret = rte_eth_rx_queue_setup(portid, 0, nb_rxd,
					     rte_eth_dev_socket_id(portid),
					     NULL,
					     capture_pktmbuf_pool);
		if (ret < 0)
			rte_exit(EXIT_FAILURE, "rte_eth_rx_queue_setup:err=%d, port=%u\n",
				  ret, (unsigned) portid);

		/* init one TX queue on each port */
		fflush(stdout);
		ret = rte_eth_tx_queue_setup(portid, 0, nb_txd,
				rte_eth_dev_socket_id(portid),
				NULL);
		if (ret < 0)
			rte_exit(EXIT_FAILURE, "rte_eth_tx_queue_setup:err=%d, port=%u\n",
				ret, (unsigned) portid);

		/* Start device */
		ret = rte_eth_dev_start(portid);
		if (ret < 0)
			rte_exit(EXIT_FAILURE, "rte_eth_dev_start:err=%d, port=%u\n",
				  ret, (unsigned) portid);

		printf("done: \n");

		rte_eth_promiscuous_enable(portid);

		printf("Port %u, MAC address: %02X:%02X:%02X:%02X:%02X:%02X\n\n",
				(unsigned) portid,
				capture_port_conf[portid].eth_addr.addr_bytes[0],
				capture_port_conf[portid].eth_addr.addr_bytes[1],
				capture_port_conf[portid].eth_addr.addr_bytes[2],
				capture_port_conf[portid].eth_addr.addr_bytes[3],
				capture_port_conf[portid].eth_addr.addr_bytes[4],
				capture_port_conf[portid].eth_addr.addr_bytes[5]);

	}

	if (!nb_ports_available) {
		rte_exit(EXIT_FAILURE,
			"All available ports are disabled. Please set portmask.\n");
	}

	check_all_ports_link_status(nb_ports, capture_enabled_port_mask);

	/* launch per-lcore init on every lcore */
	rte_eal_mp_remote_launch(capture_launch_one_lcore, NULL, CALL_MASTER);
	RTE_LCORE_FOREACH_SLAVE(lcore_id) {
		if (rte_eal_wait_lcore(lcore_id) < 0)
			break;
	}

	for (portid = 0; portid < nb_ports; portid++) {
		if ((capture_enabled_port_mask & (1 << portid)) == 0)
			continue;

		printf("Closing port %d...", portid);
		rte_eth_dev_stop(portid);
		rte_eth_dev_close(portid);
		printf(" Done\n");
	}

	return 0;
}

