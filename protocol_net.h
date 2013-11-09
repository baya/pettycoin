#ifndef PETTYCOIN_PROTOCOL_NET_H
#define PETTYCOIN_PROTOCOL_NET_H
#include "protocol.h"

#define PROTOCOL_MAX_PACKET_LEN (4 * 1024 * 1024)

/* Request-response protocol for pettycoin. */
enum protocol_req_type {
	/* Hi, my version is, and my hobbies are... */
	PROTOCOL_REQ_WELCOME,
	/* Your last response didn't make sense. */
	PROTOCOL_REQ_ERR,
	/* Do you have any blocks past this? */
	PROTOCOL_REQ_GETBLOCKS,
	/* Tell me about this block. */
	PROTOCOL_REQ_TRANSACTION_NUMS,
	/* Tell me about this batch in a block. */
	PROTOCOL_REQ_BATCH,
	/* Tell me about this transaction in a block. */
	PROTOCOL_REQ_TRANSACTION,
	/* I have a new block! */
	PROTOCOL_REQ_NEW_BLOCK,
	/* I have a new transaction */
	PROTOCOL_REQ_NEW_TRANSACTION,

	/* >= this is invalid. */
	PROTOCOL_REQ_MAX
};

/* High bit indicates a response packet. */
enum protocol_response_type {
	PROTOCOL_RESPONSE_WELCOME = 0x80000000,
	PROTOCOL_RESPONSE_ERR, /* Unused. */
	PROTOCOL_RESPONSE_BLOCKSTART,
	PROTOCOL_RESPONSE_TRANSACTION_NUMS,
	PROTOCOL_RESPONSE_BATCH,
	PROTOCOL_RESPONSE_TRANNSACTION,
	PROTOCOL_RESPONSE_NEW_BLOCK,
	PROTOCOL_RESPONSE_NEW_TRANSACTION,

	/* >= this is invalid. */
	PROTOCOL_RESPONSE_MAX
};

enum protocol_error {
	PROTOCOL_ERROR_NONE, /* happy camper. */
	/* General errors: */
	PROTOCOL_UNKNOWN_COMMAND,
	PROTOCOL_INVALID_LEN,
	/* protocol_req_welcome/protocol_response_welcome: */

	PROTOCOL_ERROR_HIGH_VERSION, /* version is unknown. */
	PROTOCOL_ERROR_LOW_VERSION, /* version is old. */
	PROTOCOL_ERROR_NO_INTEREST, /* not enough interest bits. */
	/* protocol_req_blockstart/protocol_req_batchnums/protocol_req_batch: */
	PROTOCOL_ERROR_UNKNOWN_BLOCK, /* I don't know that block? */
	/* protocol_req_batchnums/protocol_req_batch: */
	PROTOCOL_ERROR_BAD_BATCHNUM, /* Exceeds transaction count. */
	/* protocol_response_batch: */
	PROTOCOL_ERROR_INVALID_MERKLE,

	/* >= this is invalid. */
	PROTOCOL_ERROR_MAX
};

struct protocol_net_address {
	u8 addr[16];
	be16 port;
}  __attribute__((aligned(2)));

struct protocol_req_welcome {
	le32 len; /* sizeof(struct protocol_req_welcome) */
	le32 type; /* PROTOCOL_REQ_WELCOME */
	le32 version; /* Protocol version, currently 1. */
	/* Freeform software version. */
	char moniker[32];
	/* Self-detection */
	le64 random;
	/* Address we see you at. */
	struct protocol_net_address you;
	/* Port you can connect to us at (if != 0) */
	be16 listen_port;
	/* We are interested in certain addresses, based on their
	 * lower bits.  We must be interested in more than 1. */
	u8 interests[(1 << PROTOCOL_SHARD_BITS) / 8];
};

struct protocol_response_welcome {
	le32 len; /* sizeof(struct protocol_response_welcome) */
	le32 type; /* PROTOCOL_RESPONSE_WELCOME */
	le32 error; /* Expect PROTOCOL_ERROR_NONE. */
	/* Freeform software version. */
	char moniker[32];
	/* Address we see you at. */
	struct protocol_net_address you;
	/* We are interested in certain addresses, based on their
	 * lower bits bits.  We must be interested in more than 1. */
	le32 interests[(1 << PROTOCOL_SHARD_BITS) / 32];
};

/* Usually followed by a hangup, since communication has failed. */
struct protocol_req_err {
	le32 len; /* sizeof(struct protocol_req_welcome) */
	le32 type; /* PROTOCOL_REQ_ERR */
	le32 error;
};

/* As per bitcoin: last 10 blocks, then power of 2 back. */
struct protocol_req_getblocks {
	le32 len; /* sizeof(struct protocol_req_blockstart) + num*sizeof(block)*/
	le32 type; /* PROTOCOL_REQ_GETBLOCKS */
	le32 num;
	struct protocol_double_sha block[ /* num */ ];
};

struct protocol_response_getblocks {
	le32 len; /* sizeof(struct protocol_response_welcome) */
	le32 type; /* PROTOCOL_RESPONSE_GETBLOCKS */
	le32 error; /* Expect PROTOCOL_ERROR_NONE. */
	/* Common block we share. */
	struct protocol_double_sha common;
	/* How many past that. */
	le32 extras; 
};

/* Which transactions are interesting to me? */
struct protocol_req_batch_nums {
	le32 len; /* sizeof(struct protocol_req_batchnums) */
	le32 type; /* PROTOCOL_REQ_TRANSACTION_NUMS */
	/* Which block do I want to know about. */
	struct protocol_double_sha block;
};

/* Here's some transaction nums for you. */
struct protocol_response_transaction_nums {
	le32 len; /* sizeof(struct protocol_response_welcome) */
	le32 type; /* PROTOCOL_RESPONSE_TRANSACTION_NUMS */
	le32 error; /* Expect PROTOCOL_ERROR_NONE. */
	le32 num_transactions; /* Number of individual transactions. */
	le32 idx[ /* num_transactions */ ];
};

/* Give me this batch. */
struct protocol_req_batch {
	le32 len; /* sizeof(struct protocol_req_batch) */
	le32 type; /* PROTOCOL_REQ_BATCH */
	/* Which block do I want to know about. */
	struct protocol_double_sha block;
	/* Which batch number. */
	le32 batchnum;
};

/* Here's a batch for you. */
struct protocol_response_batch {
	le32 len; /* sizeof(struct protocol_response_batch + ...) */
	le32 type; /* PROTOCOL_RESPONSE_WELCOME */
	le32 error; /* Expect PROTOCOL_ERROR_NONE. */
	le32 num; /* Number of transactions in batch. */

	/* Marshalled transaction. */
	union protocol_transaction trans;
	/* ... */
};

/* Give me this transaction. */
struct protocol_req_transaction {
	le32 len; /* sizeof(struct protocol_req_transaction) */
	le32 type; /* PROTOCOL_REQ_TRANSACTION */
	/* Which block do I want to know about. */
	struct protocol_double_sha block;
	/* Which transaction */
	le32 transnum;
};

/* Here's a transaction for you. */
struct protocol_response_transaction {
	le32 len; /* sizeof(struct protocol_response_batch + ...) */
	le32 type; /* PROTOCOL_RESPONSE_WELCOME */
	le32 error; /* Expect PROTOCOL_ERROR_NONE. */

	/* This is the tree of double shas which proves it. */
	struct protocol_double_sha proof[PETTYCOIN_BATCH_ORDER];

	/* Marshalled transaction. */
	union protocol_transaction trans;
	/* ... */
};

/* I have a new block for you! */
struct protocol_req_new_block {
	le32 len; /* sizeof(struct protocol_req_new_block + ...) */
	le32 type; /* PROTOCOL_REQ_NEW_BLOCK */

	/* Marshalled block. */
	struct protocol_block_header hdr;
	/* ... */
};

struct protocol_response_new_block {
	le32 len; /* sizeof(struct protocol_response_new_block) */
	le32 type; /* PROTOCOL_RESPONSE_NEW_BLOCK */
	le32 error; /* Expect PROTOCOL_ERROR_NONE. */
};

/* I have a new transaction for you! */
struct protocol_req_new_transaction {
	le32 len; /* ... */
	le32 type; /* PROTOCOL_REQ_NEW_TRANSACTION */

	/* Marshalled transaction. */
	union protocol_transaction trans;
	/* ... */
};

struct protocol_response_new_transaction {
	le32 len; /* sizeof(struct protocol_response_new_transaction) */
	le32 type; /* PROTOCOL_RESPONSE_NEW_TRANSACTION */
	le32 error; /* Expect PROTOCOL_ERROR_NONE. */
};

/* IPv4 addresses are represented as per rfc4291#section-2.5.5.2 */
struct protocol_net_addr {
	u8 addr[16];
};
#endif /* PETTYCOIN_PROTOCOL_NET_H */
