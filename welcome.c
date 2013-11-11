#include "welcome.h"
#include "state.h"
#include "version.h"
#include "block.h"

#define TALV_LABEL(type, lastelem)		\
	stringify(type) " " stringify(lastelem)
#define talv(ctx, type, lastelem)					\
	((type *)tal_alloc_((ctx), offsetof(type, lastelem),		\
			    false, TALV_LABEL(type, lastelem)))

static size_t welcome_iter(const struct state *state,
			   struct protocol_double_sha *block_arr)
{
	struct block *b, *last;
	unsigned int n, step;

	last = b = list_tail(&state->blocks, struct block, list);

	if (block_arr)
		block_arr[0] = b->sha;

	for (n = 1; b; n++) {
		unsigned int i;

		if (n < 10)
			step = 1;
		else
			step *= 2;

		for (i = 0; i < step; i++) {
			b = list_prev(&state->blocks, b, list);
			if (!b)
				goto out;
		}

		if (block_arr)
			block_arr[n] = b->sha;

		last = b;
	}

out:
	/* Always include the genesis block. */
	b = list_top(&state->blocks, struct block, list);
	if (last != b) {
		if (block_arr)
			block_arr[n] = b->sha;
		n++;
	}

	return n;
}

static size_t num_welcome_blocks(const struct state *state)
{
	return welcome_iter(state, NULL);
}

static void welcome_blocks(const struct state *state,
			   struct protocol_double_sha *block)
{
	welcome_iter(state, block);
}

struct protocol_req_welcome *make_welcome(const tal_t *ctx,
					  const struct state *state,
					  const struct protocol_net_address *a)
{
	struct protocol_req_welcome *w;
	size_t num_blocks = num_welcome_blocks(state);

	w = talv(ctx, struct protocol_req_welcome, block[num_blocks]);
	w->len = cpu_to_le32(sizeof(*w) + sizeof(w->block[0]) * num_blocks
			     - sizeof(w->len));
	w->type = cpu_to_le32(PROTOCOL_REQ_WELCOME);
	w->num_blocks = num_blocks;
	w->version = cpu_to_le32(current_version());
	memcpy(w->moniker, "Can't believe it's not bitcoin!", 32);
	w->random = state->random_welcome;
	w->you = *a;
	w->listen_port = state->listen_port;
	memset(w->interests, 0xFF, sizeof(w->interests));
	welcome_blocks(state, w->block);

	return w;
}

static size_t popcount(const u8 *bits, size_t num)
{
	size_t n = 0, i;

	for (i = 0; i < num * CHAR_BIT; i++)
		if (bits[i/CHAR_BIT] & (1 << (i % CHAR_BIT)))
			n++;
	return n;
}

enum protocol_error check_welcome(const struct state *state,
				  const struct protocol_req_welcome *w)
{
	size_t len = le32_to_cpu(w->len);
	struct block *genesis = list_top(&state->blocks, struct block, list);

	if (len < sizeof(*w) - sizeof(w->len))
		return PROTOCOL_INVALID_LEN;
	if (w->type != cpu_to_le32(PROTOCOL_REQ_WELCOME))
		return PROTOCOL_UNKNOWN_COMMAND;
	if (w->version != cpu_to_le32(current_version()))
		return PROTOCOL_ERROR_HIGH_VERSION;
	if (popcount(w->interests, sizeof(w->interests)) < 2)
		return PROTOCOL_ERROR_NO_INTEREST;

	/* At least one block. */
	if (w->num_blocks < 1)
		return PROTOCOL_INVALID_LEN;
	len -= sizeof(*w) - sizeof(w->len);
	if (len != le32_to_cpu(w->num_blocks) * sizeof(w->block[0]))
		return PROTOCOL_INVALID_LEN;

	/* We must agree on genesis block. */
	if (memcmp(&w->block[0], &genesis->sha, sizeof(genesis->sha)) != 0)
		return PROTOCOL_ERROR_WRONG_GENESIS;

	return PROTOCOL_ERROR_NONE;
}