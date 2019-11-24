/*
 * Block types.
 */

/*
 * Common part at the beginning of all blocks.
 */
struct block_header {
	uint32_t	block_type;
	uint32_t	total_length;
};

/*
 * Common trailer at the end of all blocks.
 */
struct block_trailer {
	uint32_t	total_length;
};

/*
 * Common options.
 */
#define OPT_ENDOFOPT	0	/* end of options */
#define OPT_COMMENT	1	/* comment string */

/*
 * Option header.
 */
struct option_header {
	uint16_t		option_code;
	uint16_t		option_length;
};

/*
 * Section Header Block.
 */
#define BT_SHB			0x0A0D0D0A

 /*
 * Byte-order magic value.
 */
#define BYTE_ORDER_MAGIC	0x1A2B3C4D

/*
 * Current version number.  If major_version isn't PCAP_NG_VERSION_MAJOR,
 * that means that this code can't read the file.
 */
#define PCAP_NG_VERSION_MAJOR	1
#define PCAP_NG_VERSION_MINOR	0

struct section_header_block {
	uint32_t	byte_order_magic;
	uint16_t	major_version;
	uint16_t	minor_version;
	uint64_t	section_length;
	/* followed by options and trailer */
};


/*
 * Interface Description Block.
 */
#define BT_IDB			0x00000001

struct interface_description_block {
	uint16_t	linktype;
	uint16_t	reserved;
	uint32_t	snaplen;
	/* followed by options and trailer */
};

/*
 * Options in the IDB.
 */
#define IF_NAME		2	/* interface name string */
#define IF_DESCRIPTION	3	/* interface description string */
#define IF_IPV4ADDR	4	/* interface's IPv4 address and netmask */
#define IF_IPV6ADDR	5	/* interface's IPv6 address and prefix length */
#define IF_MACADDR	6	/* interface's MAC address */
#define IF_EUIADDR	7	/* interface's EUI address */
#define IF_SPEED	8	/* interface's speed, in bits/s */
#define IF_TSRESOL	9	/* interface's time stamp resolution */
#define IF_TZONE	10	/* interface's time zone */
#define IF_FILTER	11	/* filter used when capturing on interface */
#define IF_OS		12	/* string OS on which capture on this interface was done */
#define IF_FCSLEN	13	/* FCS length for this interface */
#define IF_TSOFFSET	14	/* time stamp offset for this interface */

 /*
 * Enhanced Packet Block.
 */
#define BT_EPB			0x00000006

struct enhanced_packet_block {
	uint32_t	interface_id;
	uint32_t	timestamp_high;
	uint32_t	timestamp_low;
	uint32_t	caplen;
	uint32_t	len;
	/* followed by packet data, options, and trailer */
};

/*
 * Simple Packet Block.
 */
#define BT_SPB			0x00000003

struct simple_packet_block {
	uint32_t	len;
	/* followed by packet data and trailer */
};