/* This helper tries to generate a block: stdin can add (verified)
 * transactions.
 *
 * Lyrics by The Who.
 */
#include "block.h"
#include "difficulty.h"
#include "features.h"
#include "generate.h"
#include "hex.h"
#include "marshal.h"
#include "merkle_hashes.h"
#include "protocol.h"
#include "shadouble.h"
#include "tal_packet.h"
#include "timestamp.h"
#include "tx_cmp.h"
#include "version.h"
#include "version.h"
#include <assert.h>
#include <ccan/array_size/array_size.h>
#include <ccan/asort/asort.h>
#include <ccan/cast/cast.h>
#include <ccan/err/err.h>
#include <ccan/read_write_all/read_write_all.h>
#include <ccan/str/str.h>
#include <ccan/tal/tal.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <sys/select.h>
#include <time.h>
#include <unistd.h>

static volatile bool input = true;

static void input_ready(int signum)
{
	input = true;
}

struct working_block {
	u32 feature_counts[8];
	u32 num_shards;
	u32 num_trans;
	struct block_info bi;
	struct protocol_txrefhash **trans_hashes;
	struct protocol_double_sha hash_of_merkles;
	struct protocol_double_sha hash_of_prev_txhashes;

	/* Non-const pointers for bi parts we update. */
	struct protocol_block_header hdr;
	struct protocol_double_sha *merkles_;
	u8 *num_txs_;
	struct protocol_block_tailer tailer;

	/* Unfinished hash without tailer. */
	SHA256_CTX partial;

	/* Finished result. */
	struct protocol_double_sha sha;
};

/* Update w->merkles[shard], and w->hash_of_merkles */
static void merkle_hash_shard(struct working_block *w, u32 shard)
{
	merkle_hashes(w->trans_hashes[shard],
		      0, w->bi.num_txs[shard],
		      &w->merkles_[shard]);
}

static void merkle_hash_changed(struct working_block *w)
{
	u32 i;
	SHA256_CTX ctx;

	/* Recalc hash of all merkles. */
	SHA256_Init(&ctx);
	for (i = 0; i < w->num_shards; i++)
		SHA256_Update(&ctx, block_merkle(&w->bi, i),
			      sizeof(struct protocol_double_sha));
	SHA256_Double_Final(&ctx, &w->hash_of_merkles);
}

static void update_partial_hash(struct working_block *w)
{
	SHA256_Init(&w->partial);
	SHA256_Update(&w->partial, &w->hash_of_prev_txhashes,
		      sizeof(w->hash_of_prev_txhashes));
	SHA256_Update(&w->partial, &w->hash_of_merkles,
		      sizeof(w->hash_of_merkles));
	SHA256_Update(&w->partial, w->bi.hdr, sizeof(*w->bi.hdr));
	SHA256_Update(&w->partial, w->bi.num_txs,
		      sizeof(*w->bi.num_txs)*w->num_shards);
}

/* Create a new block. */
static struct working_block *
new_working_block(const tal_t *ctx,
		  u32 difficulty,
		  u8 *prev_txhashes,
		  unsigned long num_prev_txhashes,
		  u32 height,
		  u8 shard_order,
		  const struct protocol_block_id *prevs,
		  const struct protocol_address *fees_to)
{
	struct working_block *w;
	SHA256_CTX shactx;
	u32 i;

	w = tal(ctx, struct working_block);
	if (!w)
		return NULL;

	memset(w->feature_counts, 0, sizeof(w->feature_counts));

	w->num_shards = 1 << shard_order;
	w->num_trans = 0;
	w->trans_hashes = tal_arr(w, struct protocol_txrefhash *,
				  w->num_shards);
	w->bi.hdr = &w->hdr;
	w->bi.num_txs = w->num_txs_ = tal_arrz(w, u8, w->num_shards);
	w->bi.merkles = w->merkles_
		= tal_arrz(w, struct protocol_double_sha, w->num_shards);
	w->bi.tailer = &w->tailer;
	for (i = 0; i < w->num_shards; i++) {
		w->trans_hashes[i] = tal_arr(w->trans_hashes,
					     struct protocol_txrefhash,
					     0);
		if (!w->trans_hashes[i])
			return tal_free(w);
	}

	w->hdr.version = current_version();
	w->hdr.features_vote = 0;
	memset(w->hdr.nonce2, 0, sizeof(w->hdr.nonce2));
	memcpy(w->hdr.prevs, prevs, sizeof(w->hdr.prevs));
	w->hdr.shard_order = shard_order;
	w->hdr.num_prev_txhashes = cpu_to_le32(num_prev_txhashes);
	w->hdr.height = cpu_to_le32(height);
	w->hdr.fees_to = *fees_to;

	w->tailer.timestamp = cpu_to_le32(current_time());
	w->tailer.nonce1 = cpu_to_le32(0);
	w->tailer.difficulty = cpu_to_le32(difficulty);

	/* Hash prev_txhashes: it doesn't change */
	w->bi.prev_txhashes = prev_txhashes;
	SHA256_Init(&shactx);
	SHA256_Update(&shactx, w->bi.prev_txhashes, num_prev_txhashes);
	SHA256_Double_Final(&shactx, &w->hash_of_prev_txhashes);

	for (i = 0; i < w->num_shards; i++)
		merkle_hash_shard(w, i);
	merkle_hash_changed(w);

	update_partial_hash(w);
	return w;
}

/* Append a new transaction hash to the block. */
static bool add_tx(struct working_block *w, struct gen_update *update)
{
	unsigned int i;
	u8 new_features = 0;
	size_t num;

	assert(update->shard < tal_count(w->bi.num_txs));
	assert(w->bi.num_txs[update->shard] < 255);
	assert(update->txoff <= w->bi.num_txs[update->shard]);

	num = w->bi.num_txs[update->shard];

	tal_resize(&w->trans_hashes[update->shard], num + 1);

	/* ''Just because we get around.'' */
	memmove(w->trans_hashes[update->shard] + update->txoff + 1,
		w->trans_hashes[update->shard] + update->txoff,
		(num - update->txoff)
		* sizeof(w->trans_hashes[update->shard][0]));
	w->trans_hashes[update->shard][update->txoff] = update->hashes;
	w->num_txs_[update->shard]++;
	w->num_trans++;

	merkle_hash_shard(w, update->shard);
	merkle_hash_changed(w);

	for (i = 0; i < ARRAY_SIZE(w->feature_counts); i++) {
		if (update->features & (1 << i))
			w->feature_counts[i]++;
		/* If less than half vote for it, clear feature. */
		if (w->feature_counts[i] < w->num_trans / 2)
			new_features &= ~(1 << i);
	}
	w->hdr.features_vote = new_features;
	update_partial_hash(w);
	return true;
}

static void increment_nonce2(struct protocol_block_header *hdr)
{
	unsigned int i;

	for (i = 0; i < sizeof(hdr->nonce2); i++) {
		hdr->nonce2[i]++;
		if (hdr->nonce2[i])
			break;
	}
}

/* Try to solve the block. */
static bool solve_block(struct working_block *w)
{
	SHA256_CTX ctx;
	uint32_t *nonce1;

	ctx = w->partial;
	SHA256_Update(&ctx, w->bi.tailer, sizeof(*w->bi.tailer));
	SHA256_Double_Final(&ctx, &w->sha);

	if (beats_target(&w->sha, block_difficulty(&w->bi)))
		return true;

	/* Keep sparse happy: we don't care about nonce endianness. */
	nonce1 = (ENDIAN_CAST uint32_t *)&w->tailer.nonce1;

	/* Increment nonce1. */
	(*nonce1)++;

	/* ''I hope I die before I get old'' */
	if ((*nonce1 & 0xFFFF) == 0) {
		w->tailer.timestamp = cpu_to_le32(current_time());

		/* If nonce1 completely wraps, time to update nonce2. */
		if (*nonce1 == 0) {
			increment_nonce2(&w->hdr);
			update_partial_hash(w);
		}
	}

	return false;
}

/* ''And don't try to d-dig what we all s-s-say'' */
static bool read_all_if_any(int fd, void *buf, size_t len)
{
	size_t off = 0;

	while (off < len) {
		int r = read(STDIN_FILENO, (char *)buf + off, len - off);
		if (r == 0) {
			/* Terminated cleanly? */
			if (off == 0)
				exit(0);
			errx(1, "''Things they do look awful c-c-cold''");
		}
		if (r == -1) {
			if (errno == EAGAIN || errno == EWOULDBLOCK) {
				/* Nothing there?  OK. */
				if (off == 0)
					return false;
				/* May spin, but shouldn't be long. */
				continue;
			}
			errx(1, "''Things they do look awful c-c-cold!''");
		}
		off += r;
	}
	return true;
}

static void read_txs(struct working_block *w)
{
	struct gen_update *update = tal(w, struct gen_update);

	/* Gratuitous initial read handles race */
	while (read_all_if_any(STDIN_FILENO, update, sizeof(*update))) {
		if (!add_tx(w, update))
			err(1, "Adding transaction");
		update = tal(w, struct gen_update);
	}

	tal_free(update);
}

static struct protocol_pkt_shard *make_shard_pkt(const struct working_block *w,
						 u32 shard)
{
	struct protocol_pkt_shard *s;
	unsigned int i;

	s = tal_packet(w, struct protocol_pkt_shard, PROTOCOL_PKT_SHARD);
	s->block.sha = w->sha;
	s->shard = cpu_to_le16(shard);
	s->err = cpu_to_le16(PROTOCOL_ECODE_NONE);

	for (i = 0; i < w->bi.num_txs[shard]; i++)
		tal_packet_append_txrefhash(&s, &w->trans_hashes[shard][i]);

	return s;
}

/* ''Talkin' 'bout my generation...''  */
static void write_block(int fd, const struct working_block *w)
{
	struct protocol_pkt_block *b;
	struct protocol_pkt_shard *s;
	u32 shard;

	b = marshal_block(w, &w->bi);
	if (!write_all(fd, b, le32_to_cpu(b->len)))
		err(1, "''I'm not trying to cause a b-big s-s-sensation''");

	/* Write out the shard hashes. */
	for (shard = 0; shard < w->num_shards; shard++) {
		s = make_shard_pkt(w, shard);
		if (!write_all(fd, s, le32_to_cpu(s->len)))
			err(1, "''I'm just talkin' 'bout my g-g-generation''"
			    " %i", shard);
	}
}

int main(int argc, char *argv[])
{
	tal_t *ctx = tal(NULL, char);
	struct working_block *w;
	struct protocol_address reward_address;
	struct protocol_block_id prev_hashes[PROTOCOL_NUM_PREV_IDS];
	u8 *prev_txhashes;
	u32 difficulty, num_prev_txhashes, height, shard_order;

	err_set_progname(argv[0]);

	if (argc != 7 && argc != 8)
		errx(1, "Usage: %s <reward_addr> <difficulty> <prevhashes>"
		     " <num-prev-txhashes> <height> <shardorder> [<nonce>]",
			argv[0]);

	if (!from_hex(argv[1], strlen(argv[1]),
		      reward_address.addr, sizeof(reward_address)))
		errx(1, "Invalid reward address");

	difficulty = strtoul(argv[2], NULL, 0);
	if (!valid_difficulty(difficulty))
		errx(1, "Invalid difficulty");

	if (!from_hex(argv[3], strlen(argv[3]), &prev_hashes,
		      sizeof(prev_hashes)))
		errx(1, "Invalid previous hashes");

	height = strtoul(argv[5], NULL, 0);
	shard_order = strtoul(argv[6], NULL, 0);
	num_prev_txhashes = strtoul(argv[4], NULL, 0);
	prev_txhashes = tal_arr(ctx, u8, num_prev_txhashes + 1);

	/* Read in prev txhashes, plus "go" byte.  If we are to
	 * terminate immediately, this might be 0 bytes. */
	if (!read_all(STDIN_FILENO, prev_txhashes, num_prev_txhashes+1))
		exit(0);

	w = new_working_block(ctx, difficulty, prev_txhashes, num_prev_txhashes,
			      height, shard_order, prev_hashes, &reward_address);

	if (argv[7]) {
		strncpy((char *)w->hdr.nonce2, argv[7],
			sizeof(w->hdr.nonce2));
		update_partial_hash(w);
	}

	signal(SIGIO, input_ready);
	if (fcntl(STDIN_FILENO, F_SETOWN, getpid()) != 0)
		err(1, "Setting F_SETOWN on stdin");
	if (fcntl(STDIN_FILENO, F_SETFL,
		  fcntl(STDIN_FILENO, F_GETFL)|O_ASYNC|O_NONBLOCK) != 0)
		err(1, "Setting O_ASYNC and O_NONBLOCK on stdin");

	do {
		if (input) {
			input = false;
			read_txs(w);
		}
	} while (!solve_block(w));

	write_block(STDOUT_FILENO, w);

	/* ''Why don't you all f-fade away'' */
	tal_free(ctx);
	return 0;
}
