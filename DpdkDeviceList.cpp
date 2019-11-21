#ifdef USE_DPDK

#define LOG_MODULE PcapLogModuleDpdkDevice

#define __STDC_LIMIT_MACROS
#define __STDC_FORMAT_MACROS

#include "DpdkDeviceList.h"
#include "Logger.h"

#include <rte_config.h>
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
#include <rte_ethdev.h>
#include <rte_ring.h>
#include <rte_mempool.h>
#include <rte_mbuf.h>
#include <rte_version.h>

#include <rte_common.h>
#include <rte_log.h>
#include <rte_malloc.h>
#include <rte_memory.h>
#include <rte_memcpy.h>
#include <rte_eal.h>
#include <rte_launch.h>
#include <rte_atomic.h>
#include <rte_cycles.h>
#include <rte_prefetch.h>
#include <rte_lcore.h>
#include <rte_per_lcore.h>
#include <rte_branch_prediction.h>
#include <rte_interrupts.h>
#include <rte_random.h>
#include <rte_debug.h>
#include <rte_ether.h>
#include <rte_ethdev.h>
#include <rte_mempool.h>
#include <rte_mbuf.h>


#include <sstream>
#include <iomanip>
#include <string>
#include <algorithm>
#include <unistd.h>

namespace pcpp
{
	
uint8_t SNDpdkDeviceList::m_CoreMask= 1;
uint8_t SNDpdkDeviceList::m_N= 4;
uint8_t SNDpdkDeviceList::m_PortMask= 1;
uint8_t SNDpdkDeviceList::m_RxQueue= 4;


	static volatile bool force_quit;

	/* MAC updating enabled by default */
	static int mac_updating = 1;


	#define RTE_LOGTYPE_L2FWD RTE_LOGTYPE_USER1

	#define MAX_PKT_BURST 32
	#define BURST_TX_DRAIN_US 100 /* TX drain every ~100us */
	#define MEMPOOL_CACHE_SIZE 256


	/* ethernet addresses of ports */
	static struct ether_addr l2fwd_ports_eth_addr[RTE_MAX_ETHPORTS];

	/* mask of enabled ports */
	static uint32_t l2fwd_enabled_port_mask = 0;

	/* list of enabled ports */
	static uint32_t l2fwd_dst_ports[RTE_MAX_ETHPORTS];

	static unsigned int l2fwd_rx_queue_per_lcore = 1;

	#define MAX_RX_QUEUE_PER_LCORE 16
	#define MAX_TX_QUEUE_PER_PORT 16
	struct lcore_queue_conf {
		unsigned n_rx_port;
		unsigned rx_port_list[MAX_RX_QUEUE_PER_LCORE];
	} __rte_cache_aligned;
	struct lcore_queue_conf lcore_queue_conf[RTE_MAX_LCORE];

	static struct rte_eth_dev_tx_buffer *tx_buffer[RTE_MAX_ETHPORTS];

	static struct rte_eth_conf port_conf;


	/* Per-port statistics struct */
	struct l2fwd_port_statistics {
		uint64_t tx;
		uint64_t rx;
		uint64_t dropped;
	} __rte_cache_aligned;
	struct l2fwd_port_statistics port_statistics[RTE_MAX_ETHPORTS];
	
	#define MAX_TIMER_PERIOD 86400 /* 1 day max */
	/* A tsc-based timer responsible for triggering statistics printout */
	static uint64_t timer_period = 10; /* default period is 10 seconds */



	struct rte_mempool * l2fwd_pktmbuf_pool = NULL;
	
	struct lcore_queue_conf *qconf;
	uint16_t nb_ports;
	uint16_t nb_ports_available = 0;
	uint16_t portid, last_port;
	unsigned lcore_id, rx_lcore_id;
	unsigned nb_ports_in_mask = 0;
	unsigned int nb_lcores = 0;
	unsigned int nb_mbufs;

	#define RTE_TEST_RX_DESC_DEFAULT 1024
	#define RTE_TEST_TX_DESC_DEFAULT 1024
	static uint16_t nb_rxd = RTE_TEST_RX_DESC_DEFAULT;
	static uint16_t nb_txd = RTE_TEST_TX_DESC_DEFAULT;





/* Print out statistics on packets dropped */
static void
print_stats(void)
{
	uint64_t total_packets_dropped, total_packets_tx, total_packets_rx;
	unsigned portid;

	total_packets_dropped = 0;
	total_packets_tx = 0;
	total_packets_rx = 0;

	const char clr[] = { 27, '[', '2', 'J', '\0' };
	const char topLeft[] = { 27, '[', '1', ';', '1', 'H','\0' };

		/* Clear screen and move to top left */
	printf("%s%s", clr, topLeft);

	printf("\nPort statistics ====================================");

	for (portid = 0; portid < RTE_MAX_ETHPORTS; portid++) {
		/* skip disabled ports */
		if ((l2fwd_enabled_port_mask & (1 << portid)) == 0)
			continue;
		printf("\nStatistics for port %u ------------------------------"
			   "\nPackets sent: %24"PRIu64
			   "\nPackets received: %20"PRIu64
			   "\nPackets dropped: %21"PRIu64,
			   portid,
			   port_statistics[portid].tx,
			   port_statistics[portid].rx,
			   port_statistics[portid].dropped);

		total_packets_dropped += port_statistics[portid].dropped;
		total_packets_tx += port_statistics[portid].tx;
		total_packets_rx += port_statistics[portid].rx;
	}
	printf("\nAggregate statistics ==============================="
		   "\nTotal packets sent: %18"PRIu64
		   "\nTotal packets received: %14"PRIu64
		   "\nTotal packets dropped: %15"PRIu64,
		   total_packets_tx,
		   total_packets_rx,
		   total_packets_dropped);
	printf("\n====================================================\n");
}

static void
l2fwd_mac_updating(struct rte_mbuf *m, unsigned dest_portid)
{
	struct ether_hdr *eth;
	void *tmp;

	eth = rte_pktmbuf_mtod(m, struct ether_hdr *);

	/* 02:00:00:00:00:xx */
	tmp = &eth->d_addr.addr_bytes[0];
	*((uint64_t *)tmp) = 0x000000000002 + ((uint64_t)dest_portid << 40);

	/* src addr */
	ether_addr_copy(&l2fwd_ports_eth_addr[dest_portid], &eth->s_addr);
}

static void
l2fwd_simple_forward(struct rte_mbuf *m, unsigned portid)
{
	unsigned dst_port;
	int sent;
	struct rte_eth_dev_tx_buffer *buffer;

	dst_port = l2fwd_dst_ports[portid];

	if (mac_updating)
		l2fwd_mac_updating(m, dst_port);

	buffer = tx_buffer[dst_port];
	sent = rte_eth_tx_buffer(dst_port, 0, buffer, m);
	if (sent)
		port_statistics[dst_port].tx += sent;
}

/* main processing loop */
static void
l2fwd_main_loop(void)
{
	struct rte_mbuf *pkts_burst[MAX_PKT_BURST];
	struct rte_mbuf *m;
	int sent;
	unsigned lcore_id;
	uint64_t prev_tsc, diff_tsc, cur_tsc, timer_tsc;
	unsigned i, j, portid, nb_rx;
	struct lcore_queue_conf *qconf;
	const uint64_t drain_tsc = (rte_get_tsc_hz() + US_PER_S - 1) / US_PER_S *
			BURST_TX_DRAIN_US;
	struct rte_eth_dev_tx_buffer *buffer;

	prev_tsc = 0;
	timer_tsc = 0;

	lcore_id = rte_lcore_id();
	qconf = &lcore_queue_conf[lcore_id];

	if (qconf->n_rx_port == 0) {
		RTE_LOG(INFO, L2FWD, "lcore %u has nothing to do\n", lcore_id);
		return;
	}

	RTE_LOG(INFO, L2FWD, "entering main loop on lcore %u\n", lcore_id);

	for (i = 0; i < qconf->n_rx_port; i++) {

		portid = qconf->rx_port_list[i];
		RTE_LOG(INFO, L2FWD, " -- lcoreid=%u portid=%u\n", lcore_id,
			portid);

	}

	while (!force_quit) {

		cur_tsc = rte_rdtsc();

		/*
		 * TX burst queue drain
		 */
		diff_tsc = cur_tsc - prev_tsc;
		if (unlikely(diff_tsc > drain_tsc)) {

			for (i = 0; i < qconf->n_rx_port; i++) {

				portid = l2fwd_dst_ports[qconf->rx_port_list[i]];
				buffer = tx_buffer[portid];

				sent = rte_eth_tx_buffer_flush(portid, 0, buffer);
				if (sent)
					port_statistics[portid].tx += sent;

			}

			/* if timer is enabled */
			if (timer_period > 0) {

				/* advance the timer */
				timer_tsc += diff_tsc;

				/* if timer has reached its timeout */
				if (unlikely(timer_tsc >= timer_period)) {

					/* do this only on master core */
					if (lcore_id == rte_get_master_lcore()) {
						print_stats();
						/* reset the timer */
						timer_tsc = 0;
					}
				}
			}

			prev_tsc = cur_tsc;
		}

		/*
		 * Read packet from RX queues
		 */
		for (i = 0; i < qconf->n_rx_port; i++) {

			portid = qconf->rx_port_list[i];
			nb_rx = rte_eth_rx_burst(portid, 0,
						 pkts_burst, MAX_PKT_BURST);

			port_statistics[portid].rx += nb_rx;

			for (j = 0; j < nb_rx; j++) {
				m = pkts_burst[j];
				rte_prefetch0(rte_pktmbuf_mtod(m, void *));
				l2fwd_simple_forward(m, portid);
			}
		}
	}
}

static int
l2fwd_launch_one_lcore(__attribute__((unused)) void *dummy)
{
	l2fwd_main_loop();
	return 0;
}

/* display usage */
static void
l2fwd_usage(const char *prgname)
{
	printf("%s [EAL options] -- -p PORTMASK [-q NQ]\n"
	       "  -p PORTMASK: hexadecimal bitmask of ports to configure\n"
	       "  -q NQ: number of queue (=ports) per lcore (default is 1)\n"
		   "  -T PERIOD: statistics will be refreshed each PERIOD seconds (0 to disable, 10 default, 86400 maximum)\n"
		   "  --[no-]mac-updating: Enable or disable MAC addresses updating (enabled by default)\n"
		   "      When enabled:\n"
		   "       - The source MAC address is replaced by the TX port MAC address\n"
		   "       - The destination MAC address is replaced by 02:00:00:00:00:TX_PORT_ID\n",
	       prgname);
}

static int
l2fwd_parse_portmask(const char *portmask)
{
	char *end = NULL;
	unsigned long pm;

	/* parse hexadecimal string */
	pm = strtoul(portmask, &end, 16);
	if ((portmask[0] == '\0') || (end == NULL) || (*end != '\0'))
		return -1;

	if (pm == 0)
		return -1;

	return pm;
}

static unsigned int
l2fwd_parse_nqueue(const char *q_arg)
{
	char *end = NULL;
	unsigned long n;

	/* parse hexadecimal string */
	n = strtoul(q_arg, &end, 10);
	if ((q_arg[0] == '\0') || (end == NULL) || (*end != '\0'))
		return 0;
	if (n == 0)
		return 0;
	if (n >= MAX_RX_QUEUE_PER_LCORE)
		return 0;

	return n;
}

static int
l2fwd_parse_timer_period(const char *q_arg)
{
	char *end = NULL;
	int n;

	/* parse number string */
	n = strtol(q_arg, &end, 10);
	if ((q_arg[0] == '\0') || (end == NULL) || (*end != '\0'))
		return -1;
	if (n >= MAX_TIMER_PERIOD)
		return -1;

	return n;
}


/* Check the link status of all ports in up to 9s, and print them finally */
static void
check_all_ports_link_status(uint32_t port_mask)
{
#define CHECK_INTERVAL 100 /* 100ms */
#define MAX_CHECK_TIME 90 /* 9s (90 * 100ms) in total */
	uint16_t portid;
	uint8_t count, all_ports_up, print_flag = 0;
	struct rte_eth_link link;

	printf("\nChecking link status");
	fflush(stdout);
	for (count = 0; count <= MAX_CHECK_TIME; count++) {
		if (force_quit)
			return;
		all_ports_up = 1;
		RTE_ETH_FOREACH_DEV(portid) {
			if (force_quit)
				return;
			if ((port_mask & (1 << portid)) == 0)
				continue;
			memset(&link, 0, sizeof(link));
			rte_eth_link_get_nowait(portid, &link);
			/* print link status if flag set */
			if (print_flag == 1) {
				if (link.link_status)
					printf(
					"Port%d Link Up. Speed %u Mbps - %s\n",
						portid, link.link_speed,
				(link.link_duplex == ETH_LINK_FULL_DUPLEX) ?
					("full-duplex") : ("half-duplex\n"));
				else
					printf("Port %d Link Down\n", portid);
				continue;
			}
			/* clear all_ports_up flag if any link down */
			if (link.link_status == ETH_LINK_DOWN) {
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
			printf("done\n");
		}
	}
}

int dpdkmain()
{
	
struct rte_mempool * l2fwd_pktmbuf_pool = NULL;
	
	
	struct lcore_queue_conf *qconf;
	int ret;
	uint16_t nb_ports;
	uint16_t nb_ports_available = 0;
	uint16_t portid, last_port;
	unsigned lcore_id, rx_lcore_id;
	unsigned nb_ports_in_mask = 0;
	unsigned int nb_lcores = 0;
	unsigned int nb_mbufs;

	#define RTE_TEST_RX_DESC_DEFAULT 1024
	#define RTE_TEST_TX_DESC_DEFAULT 1024
	static uint16_t nb_rxd = RTE_TEST_RX_DESC_DEFAULT;
	static uint16_t nb_txd = RTE_TEST_TX_DESC_DEFAULT;



	char **argvlist;
	
	unsigned int paramid= 0,argcsize= 10,coremask= 0x07;
	l2fwd_enabled_port_mask= 0x01;	
	l2fwd_rx_queue_per_lcore= 0x04;

	argvlist= (char**)malloc(argcsize* sizeof(char*));
	argvlist[paramid]= (char*)malloc(16*sizeof(char));
	strcpy(argvlist[paramid++],"TrafficMonitor");

	argvlist[paramid]= (char*)malloc(16*sizeof(char));
	strcpy(argvlist[paramid++],"-c");

	argvlist[paramid]= (char*)malloc(16*sizeof(char));
	sprintf(argvlist[paramid++],"%d",coremask);


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




	/* init EAL */
	ret = rte_eal_init(argcsize, argvlist);
	if (ret < 0)
		rte_exit(EXIT_FAILURE, "Invalid EAL arguments\n");
	argcsize -= ret;
	argvlist += ret;

	force_quit = false;


	printf("MAC updating %s\n", mac_updating ? "enabled" : "disabled");

	/* convert to number of cycles */
	timer_period *= rte_get_timer_hz();

	nb_ports = rte_eth_dev_count_avail();
	if (nb_ports == 0)
		rte_exit(EXIT_FAILURE, "No Ethernet ports - bye\n");

	/* check port mask to possible port mask */
	if (l2fwd_enabled_port_mask & ~((1 << nb_ports) - 1))
		rte_exit(EXIT_FAILURE, "Invalid portmask; possible (0x%x)\n",
			(1 << nb_ports) - 1);

	/* reset l2fwd_dst_ports */
	for (portid = 0; portid < RTE_MAX_ETHPORTS; portid++)
		l2fwd_dst_ports[portid] = 0;
	last_port = 0;

	/*
	 * Each logical core is assigned a dedicated TX queue on each port.
	 */
	RTE_ETH_FOREACH_DEV(portid) {
		/* skip ports that are not enabled */
		if ((l2fwd_enabled_port_mask & (1 << portid)) == 0)
			continue;

		if (nb_ports_in_mask % 2) {
			l2fwd_dst_ports[portid] = last_port;
			l2fwd_dst_ports[last_port] = portid;
		}
		else
			last_port = portid;

		nb_ports_in_mask++;
	}
	if (nb_ports_in_mask % 2) {
		printf("Notice: odd number of ports in portmask.\n");
		l2fwd_dst_ports[last_port] = last_port;
	}

	rx_lcore_id = 0;
	qconf = NULL;

	/* Initialize the port/queue configuration of each logical core */
	RTE_ETH_FOREACH_DEV(portid) {
		/* skip ports that are not enabled */
		if ((l2fwd_enabled_port_mask & (1 << portid)) == 0)
			continue;

		/* get the lcore_id for this port */
		while (rte_lcore_is_enabled(rx_lcore_id) == 0 ||
		       lcore_queue_conf[rx_lcore_id].n_rx_port ==
		       l2fwd_rx_queue_per_lcore) {
			rx_lcore_id++;
			if (rx_lcore_id >= RTE_MAX_LCORE)
				rte_exit(EXIT_FAILURE, "Not enough cores\n");
		}

		if (qconf != &lcore_queue_conf[rx_lcore_id]) {
			/* Assigned a new logical core in the loop above. */
			qconf = &lcore_queue_conf[rx_lcore_id];
			nb_lcores++;
		}

		qconf->rx_port_list[qconf->n_rx_port] = portid;
		qconf->n_rx_port++;
		printf("Lcore %u: RX port %u\n", rx_lcore_id, portid);
	}

	nb_mbufs = RTE_MAX(nb_ports * (nb_rxd + nb_txd + MAX_PKT_BURST +
		nb_lcores * MEMPOOL_CACHE_SIZE), 8192U);

	/* create the mbuf pool */
	l2fwd_pktmbuf_pool = rte_pktmbuf_pool_create("mbuf_pool", nb_mbufs,
		MEMPOOL_CACHE_SIZE, 0, RTE_MBUF_DEFAULT_BUF_SIZE,
		rte_socket_id());
	if (l2fwd_pktmbuf_pool == NULL)
		rte_exit(EXIT_FAILURE, "Cannot init mbuf pool\n");

	memset(&port_conf,0,sizeof(port_conf));

	port_conf.rxmode.split_hdr_size = 0;
	port_conf.rxmode.ignore_offload_bitfield = 1;
	port_conf.rxmode.offloads = DEV_RX_OFFLOAD_CRC_STRIP;

		port_conf.txmode.mq_mode = ETH_MQ_TX_NONE;

	/* Initialise each port */
	RTE_ETH_FOREACH_DEV(portid) {
		struct rte_eth_rxconf rxq_conf;
		struct rte_eth_txconf txq_conf;
		struct rte_eth_conf local_port_conf = port_conf;
		struct rte_eth_dev_info dev_info;

		/* skip ports that are not enabled */
		if ((l2fwd_enabled_port_mask & (1 << portid)) == 0) {
			printf("Skipping disabled port %u\n", portid);
			continue;
		}
		nb_ports_available++;

		/* init port */
		printf("Initializing port %u... ", portid);
		fflush(stdout);
		rte_eth_dev_info_get(portid, &dev_info);
		if (dev_info.tx_offload_capa & DEV_TX_OFFLOAD_MBUF_FAST_FREE)
			local_port_conf.txmode.offloads |=
				DEV_TX_OFFLOAD_MBUF_FAST_FREE;
		ret = rte_eth_dev_configure(portid, 1, 1, &local_port_conf);
		if (ret < 0)
			rte_exit(EXIT_FAILURE, "Cannot configure device: err=%d, port=%u\n",
				  ret, portid);

		ret = rte_eth_dev_adjust_nb_rx_tx_desc(portid, &nb_rxd,
						       &nb_txd);
		if (ret < 0)
			rte_exit(EXIT_FAILURE,
				 "Cannot adjust number of descriptors: err=%d, port=%u\n",
				 ret, portid);

		rte_eth_macaddr_get(portid,&l2fwd_ports_eth_addr[portid]);

		/* init one RX queue */
		fflush(stdout);
		rxq_conf = dev_info.default_rxconf;
		rxq_conf.offloads = local_port_conf.rxmode.offloads;
		ret = rte_eth_rx_queue_setup(portid, 0, nb_rxd,
					     rte_eth_dev_socket_id(portid),
					     &rxq_conf,
					     l2fwd_pktmbuf_pool);
		if (ret < 0)
			rte_exit(EXIT_FAILURE, "rte_eth_rx_queue_setup:err=%d, port=%u\n",
				  ret, portid);

		/* init one TX queue on each port */
		fflush(stdout);
		txq_conf = dev_info.default_txconf;
		txq_conf.txq_flags = ETH_TXQ_FLAGS_IGNORE;
		txq_conf.offloads = local_port_conf.txmode.offloads;
		ret = rte_eth_tx_queue_setup(portid, 0, nb_txd,
				rte_eth_dev_socket_id(portid),
				&txq_conf);
		if (ret < 0)
			rte_exit(EXIT_FAILURE, "rte_eth_tx_queue_setup:err=%d, port=%u\n",
				ret, portid);

		/* Initialize TX buffers */
		tx_buffer[portid] = (rte_eth_dev_tx_buffer*)rte_zmalloc_socket("tx_buffer",
				RTE_ETH_TX_BUFFER_SIZE(MAX_PKT_BURST), 0,
				rte_eth_dev_socket_id(portid));
		if (tx_buffer[portid] == NULL)
			rte_exit(EXIT_FAILURE, "Cannot allocate buffer for tx on port %u\n",
					portid);

		rte_eth_tx_buffer_init(tx_buffer[portid], MAX_PKT_BURST);

		ret = rte_eth_tx_buffer_set_err_callback(tx_buffer[portid],
				rte_eth_tx_buffer_count_callback,
				&port_statistics[portid].dropped);
		if (ret < 0)
			rte_exit(EXIT_FAILURE,
			"Cannot set error callback for tx buffer on port %u\n",
				 portid);

		/* Start device */
		ret = rte_eth_dev_start(portid);
		if (ret < 0)
			rte_exit(EXIT_FAILURE, "rte_eth_dev_start:err=%d, port=%u\n",
				  ret, portid);

		printf("done: \n");

		rte_eth_promiscuous_enable(portid);

		printf("Port %u, MAC address: %02X:%02X:%02X:%02X:%02X:%02X\n\n",
				portid,
				l2fwd_ports_eth_addr[portid].addr_bytes[0],
				l2fwd_ports_eth_addr[portid].addr_bytes[1],
				l2fwd_ports_eth_addr[portid].addr_bytes[2],
				l2fwd_ports_eth_addr[portid].addr_bytes[3],
				l2fwd_ports_eth_addr[portid].addr_bytes[4],
				l2fwd_ports_eth_addr[portid].addr_bytes[5]);

		/* initialize port stats */
		memset(&port_statistics, 0, sizeof(port_statistics));
	}

	if (!nb_ports_available) {
		rte_exit(EXIT_FAILURE,
			"All available ports are disabled. Please set portmask.\n");
	}

	check_all_ports_link_status(l2fwd_enabled_port_mask);

	ret = 0;
	/* launch per-lcore init on every lcore */
	rte_eal_mp_remote_launch(l2fwd_launch_one_lcore, NULL, CALL_MASTER);
	RTE_LCORE_FOREACH_SLAVE(lcore_id) {
		if (rte_eal_wait_lcore(lcore_id) < 0) {
			ret = -1;
			break;
		}
	}

	RTE_ETH_FOREACH_DEV(portid) {
		if ((l2fwd_enabled_port_mask & (1 << portid)) == 0)
			continue;
		printf("Closing port %d...", portid);
		rte_eth_dev_stop(portid);
		rte_eth_dev_close(portid);
		printf(" Done\n");
	}
	printf("Bye...\n");

	return ret;
}
int SNDpdkDeviceList::dpdkWorkerThreadStart(void *ptr)
{
	DpdkWorkerThread* workerThread = (DpdkWorkerThread*)ptr;
	workerThread->run(rte_lcore_id());
	return 0;
}	


SNDpdkDeviceList::~SNDpdkDeviceList()
{

		std::vector<DpdkWorkerThread*>::iterator threaditer;
	
		for(threaditer= m_WorkerThreads.begin();threaditer!= m_WorkerThreads.end();++threaditer)
			delete (*threaditer);
		
		m_WorkerThreads.clear();

}

/* Print out statistics on packets dropped */
void SNDpdkDeviceList::print_stats(void)
{
	uint64_t total_packets_dropped, total_packets_tx, total_packets_rx;
	unsigned portid;

	total_packets_dropped = 0;
	total_packets_tx = 0;
	total_packets_rx = 0;

	const char clr[] = { 27, '[', '2', 'J', '\0' };
	const char topLeft[] = { 27, '[', '1', ';', '1', 'H','\0' };

		/* Clear screen and move to top left */
	printf("%s%s", clr, topLeft);

	printf("\nPort statistics ====================================");

	for (portid = 0; portid < RTE_MAX_ETHPORTS; portid++) {
		/* skip disabled ports */
		if ((l2fwd_enabled_port_mask & (1 << portid)) == 0)
			continue;
		printf("\nStatistics for port %u ------------------------------"
			   "\nPackets sent: %24"PRIu64
			   "\nPackets received: %20"PRIu64
			   "\nPackets dropped: %21"PRIu64,
			   portid,
			   port_statistics[portid].tx,
			   port_statistics[portid].rx,
			   port_statistics[portid].dropped);

		total_packets_dropped += port_statistics[portid].dropped;
		total_packets_tx += port_statistics[portid].tx;
		total_packets_rx += port_statistics[portid].rx;
	}
	printf("\nAggregate statistics ==============================="
		   "\nTotal packets sent: %18"PRIu64
		   "\nTotal packets received: %14"PRIu64
		   "\nTotal packets dropped: %15"PRIu64,
		   total_packets_tx,
		   total_packets_rx,
		   total_packets_dropped);
	printf("\n====================================================\n");
}

/* main processing loop */
void SNDpdkDeviceList::l2fwd_main_loop(void)
{
	struct rte_mbuf *pkts_burst[MAX_PKT_BURST];
	struct rte_mbuf *m;
	int sent;
	unsigned lcore_id;
	uint64_t prev_tsc, diff_tsc, cur_tsc, timer_tsc;
	unsigned i, j, portid, nb_rx;
	struct lcore_queue_conf *qconf;
	const uint64_t drain_tsc = (rte_get_tsc_hz() + US_PER_S - 1) / US_PER_S *
			BURST_TX_DRAIN_US;
	struct rte_eth_dev_tx_buffer *buffer;

	prev_tsc = 0;
	timer_tsc = 0;

	lcore_id = rte_lcore_id();
	qconf = &lcore_queue_conf[lcore_id];

	if (qconf->n_rx_port == 0) {
		RTE_LOG(INFO, L2FWD, "lcore %u has nothing to do\n", lcore_id);
		return;
	}

	RTE_LOG(INFO, L2FWD, "entering main loop on lcore %u\n", lcore_id);

	for (i = 0; i < qconf->n_rx_port; i++) {

		portid = qconf->rx_port_list[i];
		RTE_LOG(INFO, L2FWD, " -- lcoreid=%u portid=%u\n", lcore_id,
			portid);

	}

	while (!force_quit) {

		cur_tsc = rte_rdtsc();

		/*
		 * TX burst queue drain
		 */
		diff_tsc = cur_tsc - prev_tsc;
		if (unlikely(diff_tsc > drain_tsc)) {

			for (i = 0; i < qconf->n_rx_port; i++) {

				portid = l2fwd_dst_ports[qconf->rx_port_list[i]];
				buffer = tx_buffer[portid];

				sent = rte_eth_tx_buffer_flush(portid, 0, buffer);
				if (sent)
					port_statistics[portid].tx += sent;

			}

			/* if timer is enabled */
			if (timer_period > 0) {

				/* advance the timer */
				timer_tsc += diff_tsc;

				/* if timer has reached its timeout */
				if (unlikely(timer_tsc >= timer_period)) {

					/* do this only on master core */
					if (lcore_id == rte_get_master_lcore()) {
						print_stats();
						/* reset the timer */
						timer_tsc = 0;
					}
				}
			}

			prev_tsc = cur_tsc;
		}

		/*
		 * Read packet from RX queues
		 */
		for (i = 0; i < qconf->n_rx_port; i++) {

			portid = qconf->rx_port_list[i];
			nb_rx = rte_eth_rx_burst(portid, 0,
						 pkts_burst, MAX_PKT_BURST);

			port_statistics[portid].rx += nb_rx;

			for (j = 0; j < nb_rx; j++) {
				m = pkts_burst[j];
				rte_prefetch0(rte_pktmbuf_mtod(m, void *));
//				l2fwd_simple_forward(m, portid);
if(1)
{
	
	unsigned dst_port;
	int sent;
	struct rte_eth_dev_tx_buffer *buffer;

	dst_port = l2fwd_dst_ports[portid];

	buffer = tx_buffer[dst_port];
	sent = rte_eth_tx_buffer(dst_port, 0, buffer, m);
	if (sent)
		port_statistics[dst_port].tx += sent;

	
	}
			}
		}
	}
}


int SNDpdkDeviceList::l2fwd_launch_one_lcore(__attribute__((unused)) void *dummy)
{
	l2fwd_main_loop();
	return 0;
}


int SNDpdkDeviceList::initDpdkStartThreads(uint8_t coremask, uint8_t paramn, uint8_t paramq, uint8_t paramp,std::vector<DpdkWorkerThread*>& workerThreadsVec)
{
	int ret= 0;

	m_CoreMask= 7;

	char **argvlist;
	unsigned int paramid= 0,argcsize= 10;
	l2fwd_enabled_port_mask= 0x01;	
	l2fwd_rx_queue_per_lcore= 0x04;

	argvlist= (char**)malloc(argcsize* sizeof(char*));
	argvlist[paramid]= (char*)malloc(16*sizeof(char));
	strcpy(argvlist[paramid++],"TrafficMonitor");

	argvlist[paramid]= (char*)malloc(16*sizeof(char));
	strcpy(argvlist[paramid++],"-c");

	argvlist[paramid]= (char*)malloc(16*sizeof(char));
	sprintf(argvlist[paramid++],"%d",m_CoreMask);


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




	/* init EAL */
	ret = rte_eal_init(argcsize, argvlist);
	if (ret < 0)
		rte_exit(EXIT_FAILURE, "Invalid EAL arguments\n");
	argcsize -= ret;
	argvlist += ret;

	/* convert to number of cycles */
	timer_period *= rte_get_timer_hz();

	nb_ports = rte_eth_dev_count_avail();
	if (nb_ports == 0)
		rte_exit(EXIT_FAILURE, "No Ethernet ports - bye\n");

	/* check port mask to possible port mask */
	if (l2fwd_enabled_port_mask & ~((1 << nb_ports) - 1))
		rte_exit(EXIT_FAILURE, "Invalid portmask; possible (0x%x)\n",
			(1 << nb_ports) - 1);

	/* reset l2fwd_dst_ports */
	for (portid = 0; portid < RTE_MAX_ETHPORTS; portid++)
		l2fwd_dst_ports[portid] = 0;
	last_port = 0;

	/*
	 * Each logical core is assigned a dedicated TX queue on each port.
	 */
	RTE_ETH_FOREACH_DEV(portid) {
		/* skip ports that are not enabled */
		if ((l2fwd_enabled_port_mask & (1 << portid)) == 0)
			continue;

		if (nb_ports_in_mask % 2) {
			l2fwd_dst_ports[portid] = last_port;
			l2fwd_dst_ports[last_port] = portid;
		}
		else
			last_port = portid;

		nb_ports_in_mask++;
	}
	if (nb_ports_in_mask % 2) {
		printf("Notice: odd number of ports in portmask.\n");
		l2fwd_dst_ports[last_port] = last_port;
	}

	rx_lcore_id = 0;
	qconf = NULL;

	/* Initialize the port/queue configuration of each logical core */
	RTE_ETH_FOREACH_DEV(portid) {
		/* skip ports that are not enabled */
		if ((l2fwd_enabled_port_mask & (1 << portid)) == 0)
			continue;

		/* get the lcore_id for this port */
		while (rte_lcore_is_enabled(rx_lcore_id) == 0 ||
		       lcore_queue_conf[rx_lcore_id].n_rx_port ==
		       l2fwd_rx_queue_per_lcore) {
			rx_lcore_id++;
			if (rx_lcore_id >= RTE_MAX_LCORE)
				rte_exit(EXIT_FAILURE, "Not enough cores\n");
		}

		if (qconf != &lcore_queue_conf[rx_lcore_id]) {
			/* Assigned a new logical core in the loop above. */
			qconf = &lcore_queue_conf[rx_lcore_id];
			nb_lcores++;
		}

		qconf->rx_port_list[qconf->n_rx_port] = portid;
		qconf->n_rx_port++;
		printf("Lcore %u: RX port %u\n", rx_lcore_id, portid);
	}

	nb_mbufs = RTE_MAX(nb_ports * (nb_rxd + nb_txd + MAX_PKT_BURST +
		nb_lcores * MEMPOOL_CACHE_SIZE), 8192U);

	/* create the mbuf pool */
	l2fwd_pktmbuf_pool = rte_pktmbuf_pool_create("mbuf_pool", nb_mbufs,
		MEMPOOL_CACHE_SIZE, 0, RTE_MBUF_DEFAULT_BUF_SIZE,
		rte_socket_id());
	if (l2fwd_pktmbuf_pool == NULL)
		rte_exit(EXIT_FAILURE, "Cannot init mbuf pool\n");

	memset(&port_conf,0,sizeof(port_conf));

	port_conf.rxmode.split_hdr_size = 0;
	port_conf.rxmode.ignore_offload_bitfield = 1;
	port_conf.rxmode.offloads = DEV_RX_OFFLOAD_CRC_STRIP;

		port_conf.txmode.mq_mode = ETH_MQ_TX_NONE;

	/* Initialise each port */
	RTE_ETH_FOREACH_DEV(portid) {
		struct rte_eth_rxconf rxq_conf;
		struct rte_eth_txconf txq_conf;
		struct rte_eth_conf local_port_conf = port_conf;
		struct rte_eth_dev_info dev_info;

		/* skip ports that are not enabled */
		if ((l2fwd_enabled_port_mask & (1 << portid)) == 0) {
			printf("Skipping disabled port %u\n", portid);
			continue;
		}
		nb_ports_available++;

		/* init port */
		printf("Initializing port %u... ", portid);
		fflush(stdout);
		rte_eth_dev_info_get(portid, &dev_info);
		if (dev_info.tx_offload_capa & DEV_TX_OFFLOAD_MBUF_FAST_FREE)
			local_port_conf.txmode.offloads |=
				DEV_TX_OFFLOAD_MBUF_FAST_FREE;
		ret = rte_eth_dev_configure(portid, 1, 1, &local_port_conf);
		if (ret < 0)
			rte_exit(EXIT_FAILURE, "Cannot configure device: err=%d, port=%u\n",
				  ret, portid);

		ret = rte_eth_dev_adjust_nb_rx_tx_desc(portid, &nb_rxd,
						       &nb_txd);
		if (ret < 0)
			rte_exit(EXIT_FAILURE,
				 "Cannot adjust number of descriptors: err=%d, port=%u\n",
				 ret, portid);

		rte_eth_macaddr_get(portid,&l2fwd_ports_eth_addr[portid]);

		/* init one RX queue */
		fflush(stdout);
		rxq_conf = dev_info.default_rxconf;
		rxq_conf.offloads = local_port_conf.rxmode.offloads;
		ret = rte_eth_rx_queue_setup(portid, 0, nb_rxd,
					     rte_eth_dev_socket_id(portid),
					     &rxq_conf,
					     l2fwd_pktmbuf_pool);
		if (ret < 0)
			rte_exit(EXIT_FAILURE, "rte_eth_rx_queue_setup:err=%d, port=%u\n",
				  ret, portid);

		/* init one TX queue on each port */
		fflush(stdout);
		txq_conf = dev_info.default_txconf;
		txq_conf.txq_flags = ETH_TXQ_FLAGS_IGNORE;
		txq_conf.offloads = local_port_conf.txmode.offloads;
		ret = rte_eth_tx_queue_setup(portid, 0, nb_txd,
				rte_eth_dev_socket_id(portid),
				&txq_conf);
		if (ret < 0)
			rte_exit(EXIT_FAILURE, "rte_eth_tx_queue_setup:err=%d, port=%u\n",
				ret, portid);

		/* Initialize TX buffers */
		tx_buffer[portid] = (rte_eth_dev_tx_buffer*)rte_zmalloc_socket("tx_buffer",
				RTE_ETH_TX_BUFFER_SIZE(MAX_PKT_BURST), 0,
				rte_eth_dev_socket_id(portid));
		if (tx_buffer[portid] == NULL)
			rte_exit(EXIT_FAILURE, "Cannot allocate buffer for tx on port %u\n",
					portid);

		rte_eth_tx_buffer_init(tx_buffer[portid], MAX_PKT_BURST);

		ret = rte_eth_tx_buffer_set_err_callback(tx_buffer[portid],
				rte_eth_tx_buffer_count_callback,
				&port_statistics[portid].dropped);
		if (ret < 0)
			rte_exit(EXIT_FAILURE,
			"Cannot set error callback for tx buffer on port %u\n",
				 portid);

		/* Start device */
		ret = rte_eth_dev_start(portid);
		if (ret < 0)
			rte_exit(EXIT_FAILURE, "rte_eth_dev_start:err=%d, port=%u\n",
				  ret, portid);

		printf("done: \n");

		rte_eth_promiscuous_enable(portid);

		printf("Port %u, MAC address: %02X:%02X:%02X:%02X:%02X:%02X\n\n",
				portid,
				l2fwd_ports_eth_addr[portid].addr_bytes[0],
				l2fwd_ports_eth_addr[portid].addr_bytes[1],
				l2fwd_ports_eth_addr[portid].addr_bytes[2],
				l2fwd_ports_eth_addr[portid].addr_bytes[3],
				l2fwd_ports_eth_addr[portid].addr_bytes[4],
				l2fwd_ports_eth_addr[portid].addr_bytes[5]);

		/* initialize port stats */
		memset(&port_statistics, 0, sizeof(port_statistics));
	}

	if (!nb_ports_available) {
		rte_exit(EXIT_FAILURE,
			"All available ports are disabled. Please set portmask.\n");
	}


	ret = 0;
	/* launch per-lcore init on every lcore */
	rte_eal_mp_remote_launch(l2fwd_launch_one_lcore, NULL, CALL_MASTER);
	RTE_LCORE_FOREACH_SLAVE(lcore_id) {
		if (rte_eal_wait_lcore(lcore_id) < 0) {
			ret = -1;
			break;
		}
	}

	RTE_ETH_FOREACH_DEV(portid) {
		if ((l2fwd_enabled_port_mask & (1 << portid)) == 0)
			continue;
		printf("Closing port %d...", portid);
		rte_eth_dev_stop(portid);
		rte_eth_dev_close(portid);
		printf(" Done\n");
	}
	printf("Bye...\n");

	
	return ret;
	
}

void SNDpdkDeviceList::stopDpdkWorkerThreads()
{


}

	


	
	
	
	//***************************************************************
	
	
	
	
	
	
	
	
	
	
	

bool DpdkDeviceList::m_IsDpdkInitialized = false;
CoreMask DpdkDeviceList::m_CoreMask = 0;
uint32_t DpdkDeviceList::m_MBufPoolSizePerDevice = 0;

DpdkDeviceList::DpdkDeviceList()
{
	m_IsInitialized = false;
}

DpdkDeviceList::~DpdkDeviceList()
{
	for (std::vector<DpdkDevice*>::iterator iter = m_DpdkDeviceList.begin(); iter != m_DpdkDeviceList.end(); iter++)
	{
		delete (*iter);
	}

	m_DpdkDeviceList.clear();
}

const uint32_t initDpdkArgc = 7;
const uint32_t maxArgLen = 20;
char** initDpdkArgv;

bool DpdkDeviceList::initDpdk(CoreMask coreMask, uint32_t mBufPoolSizePerDevice, uint8_t masterCore)
{
	if (m_IsDpdkInitialized)
	{
		if (coreMask == m_CoreMask)
			return true;
		else
		{
			LOG_ERROR("Trying to re-initialize DPDK with a different core mask");
			return false;
		}
	}

	if (!verifyHugePagesAndDpdkDriver())
	{
		return false;
	}

	// verify mBufPoolSizePerDevice is power of 2 minus 1
	bool isPoolSizePowerOfTwoMinusOne = !(mBufPoolSizePerDevice == 0) && !((mBufPoolSizePerDevice+1) & (mBufPoolSizePerDevice));
	if (!isPoolSizePowerOfTwoMinusOne)
	{
		LOG_ERROR("mBuf pool size must be a power of two minus one: n = (2^q - 1). It's currently: %d", mBufPoolSizePerDevice);
		return false;
	}


	std::stringstream dpdkParamsStream;
	dpdkParamsStream << "pcapplusplusapp ";
	dpdkParamsStream << "-n ";
	dpdkParamsStream << "2 ";
	dpdkParamsStream << "-c ";
	dpdkParamsStream << "0x" << std::hex << std::setw(2) << std::setfill('0') << coreMask << " ";
	dpdkParamsStream << "--master-lcore ";
	dpdkParamsStream << (int)masterCore;

	std::string dpdkParamsArray[initDpdkArgc];
	initDpdkArgv = new char*[initDpdkArgc];
	uint32_t i = 0;
    while (dpdkParamsStream.good() && i < initDpdkArgc){
    	dpdkParamsStream >> dpdkParamsArray[i];
    	initDpdkArgv[i] = new char[maxArgLen];
    	strcpy(initDpdkArgv[i], dpdkParamsArray[i].c_str());
        i++;
    }

    char* lastParam = initDpdkArgv[i-1];

	for (i = 0; i < initDpdkArgc; i++)
	{
		LOG_DEBUG("DPDK initialization params: %s", initDpdkArgv[i]);
	}

	optind = 1;
	// init the EAL
	int ret = rte_eal_init(initDpdkArgc, (char**)initDpdkArgv);
	if (ret < 0) {
		LOG_ERROR("failed to init the DPDK EAL");
		return false;
	}

	for (i = 0; i < initDpdkArgc-1; i++)
	{
		delete [] initDpdkArgv[i];
	}
	delete [] lastParam;

	delete [] initDpdkArgv;

	m_CoreMask = coreMask;
	m_IsDpdkInitialized = true;

	m_MBufPoolSizePerDevice = mBufPoolSizePerDevice;
	DpdkDeviceList::getInstance().setDpdkLogLevel(LoggerPP::Normal);
	return DpdkDeviceList::getInstance().initDpdkDevices(m_MBufPoolSizePerDevice);
}

bool DpdkDeviceList::initDpdkDevices(uint32_t mBufPoolSizePerDevice)
{
	if (!m_IsDpdkInitialized)
	{
		LOG_ERROR("DPDK is not initialized!! Please call DpdkDeviceList::initDpdk(coreMask, mBufPoolSizePerDevice) before start using DPDK devices");
		return false;
	}

	if (m_IsInitialized)
		return true;

#if (RTE_VER_YEAR < 18) || (RTE_VER_YEAR == 18 && RTE_VER_MONTH < 5)
	int numOfPorts = (int)rte_eth_dev_count();
#else
	int numOfPorts = (int)rte_eth_dev_count_avail();
#endif

	if (numOfPorts <= 0)
	{
		LOG_ERROR("Zero DPDK ports are initialized. Something went wrong while initializing DPDK");
		return false;
	}

	LOG_DEBUG("Found %d DPDK ports. Constructing DpdkDevice for each one", numOfPorts);

	// Initialize a DpdkDevice per port
	for (int i = 0; i < numOfPorts; i++)
	{
		DpdkDevice* newDevice = new DpdkDevice(i, mBufPoolSizePerDevice);
		LOG_DEBUG("DpdkDevice #%d: Name='%s', PCI-slot='%s', PMD='%s', MAC Addr='%s'",
				i,
				newDevice->getDeviceName().c_str(),
				newDevice->getPciAddress().c_str(),
				newDevice->getPMDName().c_str(),
				newDevice->getMacAddress().toString().c_str());
		m_DpdkDeviceList.push_back(newDevice);
	}

	m_IsInitialized = true;
	return true;
}

DpdkDevice* DpdkDeviceList::getDeviceByPort(int portId)
{
	if (!isInitialized())
	{
		LOG_ERROR("DpdkDeviceList not initialized");
		return NULL;
	}

	if ((uint32_t)portId >= m_DpdkDeviceList.size())
	{
		return NULL;
	}

	return m_DpdkDeviceList.at(portId);
}

DpdkDevice* DpdkDeviceList::getDeviceByPciAddress(const std::string& pciAddr)
{
	if (!isInitialized())
	{
		LOG_ERROR("DpdkDeviceList not initialized");
		return NULL;
	}

	for (std::vector<DpdkDevice*>::iterator iter = m_DpdkDeviceList.begin(); iter != m_DpdkDeviceList.end(); iter++)
	{
		if ((*iter)->getPciAddress() == pciAddr)
			return (*iter);
	}

	return NULL;
}

bool DpdkDeviceList::verifyHugePagesAndDpdkDriver()
{
	std::string execResult = executeShellCommand("cat /proc/meminfo | grep -s HugePages_Total | awk '{print $2}'");
	// trim '\n' at the end
	execResult.erase(std::remove(execResult.begin(), execResult.end(), '\n'), execResult.end());

	// convert the result to long
	char* endPtr;
	long totalHugePages = strtol(execResult.c_str(), &endPtr, 10);

	LOG_DEBUG("Total number of huge-pages is %lu", totalHugePages);

	if (totalHugePages <= 0)
	{
		LOG_ERROR("Huge pages aren't set, DPDK cannot be initialized. Please run <PcapPlusPlus_Root>/setup_dpdk.sh");
		return false;
	}

	execResult = executeShellCommand("lsmod | grep -s igb_uio");
	if (execResult == "")
	{
		LOG_ERROR("igb_uio driver isn't loaded, DPDK cannot be initialized. Please run <PcapPlusPlus_Root>/setup_dpdk.sh");
		return false;

	}
	else
		LOG_DEBUG("igb_uio driver is loaded");

	return true;
}

SystemCore DpdkDeviceList::getDpdkMasterCore()
{
	return SystemCores::IdToSystemCore[rte_get_master_lcore()];
}

void DpdkDeviceList::setDpdkLogLevel(LoggerPP::LogLevel logLevel)
{
#if (RTE_VER_YEAR > 17) || (RTE_VER_YEAR == 17 && RTE_VER_MONTH >= 11)
	if (logLevel == LoggerPP::Normal)
		rte_log_set_global_level(RTE_LOG_NOTICE);
	else // logLevel == LoggerPP::Debug
		rte_log_set_global_level(RTE_LOG_DEBUG);
#else
	if (logLevel == LoggerPP::Normal)
		rte_set_log_level(RTE_LOG_NOTICE);
	else // logLevel == LoggerPP::Debug
		rte_set_log_level(RTE_LOG_DEBUG);
#endif
}

LoggerPP::LogLevel DpdkDeviceList::getDpdkLogLevel()
{
#if (RTE_VER_YEAR > 17) || (RTE_VER_YEAR == 17 && RTE_VER_MONTH >= 11)
	if (rte_log_get_global_level() <= RTE_LOG_NOTICE)
#else
	if (rte_get_log_level() <= RTE_LOG_NOTICE)
#endif
		return LoggerPP::Normal;
	else
		return LoggerPP::Debug;
}

bool DpdkDeviceList::writeDpdkLogToFile(FILE* logFile)
{
	return (rte_openlog_stream(logFile) == 0);
}

int DpdkDeviceList::dpdkWorkerThreadStart(void *ptr)
{
	DpdkWorkerThread* workerThread = (DpdkWorkerThread*)ptr;
	workerThread->run(rte_lcore_id());
	return 0;
}

bool DpdkDeviceList::startDpdkWorkerThreads(CoreMask coreMask, std::vector<DpdkWorkerThread*>& workerThreadsVec)
{
	if (!isInitialized())
	{
		LOG_ERROR("DpdkDeviceList not initialized");
		return false;
	}

	CoreMask tempCoreMask = coreMask;
	size_t numOfCoresInMask = 0;
	int coreNum = 0;
	while (tempCoreMask > 0)
	{
		if (tempCoreMask & 1)
		{
			if (!rte_lcore_is_enabled(coreNum))
			{
				LOG_ERROR("Trying to use core #%d which isn't initialized by DPDK", coreNum);
				return false;
			}

			numOfCoresInMask++;
		}
		tempCoreMask = tempCoreMask >> 1;
		coreNum++;
	}

	if (numOfCoresInMask == 0)
	{
		LOG_ERROR("Number of cores in mask is 0");
		return false;
	}

	if (numOfCoresInMask != workerThreadsVec.size())
	{
		LOG_ERROR("Number of cores in core mask different from workerThreadsVec size");
		return false;
	}

	if (coreMask & getDpdkMasterCore().Mask)
	{
		LOG_ERROR("Cannot run worker thread on DPDK master core");
		return false;
	}

	m_WorkerThreads.clear();
	uint32_t index = 0;
	std::vector<DpdkWorkerThread*>::iterator iter = workerThreadsVec.begin();
	while (iter != workerThreadsVec.end())
	{
		SystemCore core = SystemCores::IdToSystemCore[index];
		if (!(coreMask & core.Mask))
		{
			index++;
			continue;
		}

		int err = rte_eal_remote_launch(dpdkWorkerThreadStart, *iter, core.Id);
		if (err != 0)
		{
			for (std::vector<DpdkWorkerThread*>::iterator iter2 = workerThreadsVec.begin(); iter2 != iter; iter2++)
			{
				(*iter)->stop();
				rte_eal_wait_lcore((*iter)->getCoreId());
				LOG_DEBUG("Thread on core [%d] stopped", (*iter)->getCoreId());
			}
			LOG_ERROR("Cannot create worker thread #%d. Error was: [%s]", core.Id, strerror(err));
			return false;
		}
		m_WorkerThreads.push_back(*iter);

		index++;
		iter++;
	}

	return true;
}

void DpdkDeviceList::stopDpdkWorkerThreads()
{
	if (m_WorkerThreads.empty())
	{
		LOG_ERROR("No worker threads were set");
		return;
	}

	for (std::vector<DpdkWorkerThread*>::iterator iter = m_WorkerThreads.begin(); iter != m_WorkerThreads.end(); iter++)
	{
		(*iter)->stop();
		rte_eal_wait_lcore((*iter)->getCoreId());
		LOG_DEBUG("Thread on core [%d] stopped", (*iter)->getCoreId());
	}

	LOG_DEBUG("All worker threads stopped");
}

} // namespace pcpp

#endif /* USE_DPDK */