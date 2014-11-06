#include <ccan/asort/asort.h>
#include <ccan/endian/endian.h>
#include <ccan/err/err.h>
#include <ccan/hash/hash.h>
#include <ccan/invbloom/invbloom.h>
#include <ccan/opt/opt.h>
#include <ccan/short_types/short_types.h>
#include <ccan/structeq/structeq.h>
#include <openssl/sha.h>
#include <stdio.h>
#include <assert.h>

#define SLICE_BYTES 8

struct slice {
	u8 id[6];
	u16 index;
	u8 part[SLICE_BYTES];
};

struct double_sha {
	u8 sha[SHA256_DIGEST_LENGTH /* 32 */ ];
};

struct tx {
	struct double_sha txid;
	u8 *tx;
};

static void SHA256_Double_Final(SHA256_CTX *ctx, struct double_sha *sha)
{
	SHA256_Final(sha->sha, ctx);
	SHA256_Init(ctx);
	SHA256_Update(ctx, sha, sizeof(*sha));
	SHA256_Final(sha->sha, ctx);
}

static bool char_to_hex(u8 *val, char c)
{
	if (c >= '0' && c <= '9') {
		*val = c - '0';
		return true;
	}
 	if (c >= 'a' && c <= 'f') {
		*val = c - 'a' + 10;
		return true;
	}
 	if (c >= 'A' && c <= 'F') {
		*val = c - 'A' + 10;
		return true;
	}
	return false;
}

static bool from_hex(const char *str, size_t slen, void *buf, size_t bufsize)
{
	u8 v1, v2;
	u8 *p = buf;

	while (slen > 1) {
		if (!char_to_hex(&v1, str[0]) || !char_to_hex(&v2, str[1]))
			return false;
		if (!bufsize)
			return false;
		*(p++) = (v1 << 4) | v2;
		str += 2;
		slen -= 2;
		bufsize--;
	}
	return slen == 0 && bufsize == 0;
}

static void read_tx(FILE *txs, struct tx *tx)
{
	char line[200000], *colon;
	SHA256_CTX sha256;
	struct double_sha sha;
	size_t i;

	if (fgets(line, sizeof(line), txs) == NULL)
		errx(1, "Reached EOF reading transactions");

	colon = strchr(line, ':');
	if (!colon)
		errx(1, "Missing colon in line '%s'", line);

	if (!from_hex(line, colon - line, &tx->txid, sizeof(tx->txid)))
		errx(1, "Bad txid line '%s'", line);

	/* For some reason, bitcoind spits them out backwards */
	for (i = 0; i < sizeof(tx->txid) / 2; i++) {
		u8 tmp = tx->txid.sha[sizeof(tx->txid) - 1 - i];
		tx->txid.sha[sizeof(tx->txid) - 1 - i] = tx->txid.sha[i];
		tx->txid.sha[i] = tmp;
	}

	/* last char is \n */
	tx->tx = tal_arr(NULL, u8, (strlen(colon+1) - 1) / 2);
	if (!from_hex(colon+1, strlen(colon+1) - 1, tx->tx, tal_count(tx->tx)))
		errx(1, "Bad tx in line '%s'", line);

	/* Check txid */
	SHA256_Init(&sha256);
	SHA256_Update(&sha256, tx->tx, tal_count(tx->tx));
	SHA256_Double_Final(&sha256, &sha);
	if (!structeq(&tx->txid, &sha))
		errx(1, "Txid wrong for line '%s'", line);
}

static void add_tx(struct invbloom *ib, const struct tx *tx)
{
	struct slice s;
	size_t off, end = tal_count(tx->tx);

	/* For simplicity, we just use txid for id. */
	memcpy(s.id, &tx->txid, sizeof(s.id));
	s.index = 0;
	for (off = 0, s.index = 0;
	     off < end;
	     off += sizeof(s.part), s.index++) {
		size_t len = sizeof(s.part);
		if (off + len > end)
			len = end - off;
		memset(s.part, 0, sizeof(s.part));
		memcpy(s.part, tx->tx + off, len);

		invbloom_insert(ib, &s);
	}
}

static void del_tx(struct invbloom *ib, const struct tx *tx)
{
	struct slice s;
	size_t off, end = tal_count(tx->tx);

	/* For simplicity, we just use txid for id. */
	memcpy(s.id, &tx->txid, sizeof(s.id));
	s.index = 0;
	for (off = 0, s.index = 0;
	     off < end;
	     off += sizeof(s.part), s.index++) {
		size_t len = sizeof(s.part);
		if (off + len > end)
			len = end - off;
		memset(s.part, 0, sizeof(s.part));
		memcpy(s.part, tx->tx + off, len);

		invbloom_delete(ib, &s);
	}
}

static struct slice *slice_in_ib(struct invbloom *ib, size_t bucket)
{
	return (struct slice *)(ib->idsum + bucket * sizeof(struct slice));
}

static bool id_match(const struct slice *s, const u8 *id)
{
	return memcmp(s->id, id, sizeof(s->id)) == 0;
}

static const struct tx *find_by_slice(const struct tx *txs, struct slice *s)
{
	size_t i, num = tal_count(txs);

	for (i = 0; i < num; i++) {
		if (id_match(s, txs[i].txid.sha))
			return txs + i;
	}
	return NULL;
}

static bool remove_our_txs(struct invbloom *ib, const struct tx *ourtxs)
{
	size_t i;
	bool progress = false;

	for (i = 0; i < ib->n_elems; i++) {
		/* If we find one we had and they didn't, we can double check
		 * it's actually one our ours, and delete all the parts. */
		if (ib->count[i] == -1) {
			struct slice *s = slice_in_ib(ib, i);
			const struct tx *tx = find_by_slice(ourtxs, s);

			if (tx) {
				/* This cancels out the tx. */
				add_tx(ib, tx);
				progress = true;
			}
		}
	}

	return progress;
}

static bool use_bytes(const u8 **buf, size_t *max_len, size_t n)
{
	if (*max_len < n) {
		*max_len = 0;
		*buf = NULL;
		return false;
	}
	*max_len -= n;
	*buf += n;
	return true;
}

static u64 pull_varint(const u8 **buf, size_t *max_len)
{
	u64 ret;
	const u8 *p = *buf;

	if (*max_len == 0)
		return 0;

	if (*p < 0xfd) {
		ret = p[0];
		use_bytes(buf, max_len, 1);
	} else if (*p == 0xfd) {
		if (!use_bytes(buf, max_len, 3))
			return 0;

		ret = ((u64)p[2] << 8) + p[1];
	} else if (*p == 0xfe) {
		if (!use_bytes(buf, max_len, 5))
			return 0;

		ret = ((u64)p[4] << 24) + ((u64)p[3] << 16)
			+ ((u64)p[2] << 8) + p[1];
	} else {
		if (!use_bytes(buf, max_len, 9))
			return 0;

		ret = ((u64)p[8] << 56) + ((u64)p[7] << 48)
			+ ((u64)p[6] << 40) + ((u64)p[5] << 32)
			+ ((u64)p[4] << 24) + ((u64)p[3] << 16)
			+ ((u64)p[2] << 8) + p[1];
	}
	return ret;
}

static void pull_bytes(const u8 **buf, size_t *max_len, void *dst, size_t num)
{
	const u8 *src = *buf;

	if (use_bytes(buf, max_len, num)) {
		if (dst)
			memcpy(dst, src, num);
	} else {
		if (dst)
			memset(dst, 0, num);
	}
}

static u32 pull_u32(const u8 **buf, size_t *max_len)
{
	le32 ret;

	pull_bytes(buf, max_len, &ret, sizeof(ret));
	return le32_to_cpu(ret);
}

static u64 pull_u64(const u8 **buf, size_t *max_len)
{
	le64 ret;

	pull_bytes(buf, max_len, &ret, sizeof(ret));
	return le64_to_cpu(ret);
}

static void pull_hash(const u8 **buf, size_t *max_len, struct double_sha *sha)
{
	pull_bytes(buf, max_len, sha, sizeof(*sha));
}

static void pull_input(const u8 **buf, size_t *max_len)
{
	struct double_sha txid;
	u64 script_len;

	pull_hash(buf, max_len, &txid);
	/* index */
	pull_u32(buf, max_len);
	script_len = pull_varint(buf, max_len);
	pull_bytes(buf, max_len, NULL, script_len);
	/* sequence number */
	pull_u32(buf, max_len);
}

static void pull_output(const u8 **buf, size_t *max_len)
{
	u64 script_len;

	/* amount */
	pull_u64(buf, max_len);
	script_len = pull_varint(buf, max_len);
	pull_bytes(buf, max_len, NULL, script_len);
}

static bool parse_tx(const u8 *tx, size_t max_len, size_t *used)
{
	u64 i, input_count, output_count;

	*used = max_len;

	/* version */
	pull_u32(&tx, &max_len);
	input_count = pull_varint(&tx, &max_len);
	for (i = 0; i < input_count; i++) {
		pull_input(&tx, &max_len);
		if (!tx)
			return false;
	}
	output_count = pull_varint(&tx, &max_len);
	for (i = 0; i < output_count; i++) {
		pull_output(&tx, &max_len);
		if (!tx)
			return false;
	}
	/* lock_time */
	pull_u32(&tx, &max_len);

	if (!tx)
		return false;
	*used -= max_len;
	return true;
}

static struct tx *construct_tx(const tal_t *ctx, struct slice **slices)
{
	struct tx *tx = tal(ctx, struct tx);
	size_t i, len;

	tx->tx = tal_arrz(tx, u8, tal_count(slices) * sizeof(slices[0]->part));

	for (i = 0; i < tal_count(slices); i++) {
		const struct slice *s = slices[i];
		if (!s)
			return tal_free(tx);
		memcpy(tx->tx + i * sizeof(s->part), s->part, sizeof(s->part));
	}

	if (!parse_tx(tx->tx, tal_count(tx->tx), &len))
		tx = tal_free(tx);
	else {
		SHA256_CTX sha256;

		SHA256_Init(&sha256);
		SHA256_Update(&sha256, tx->tx, len);
		SHA256_Double_Final(&sha256, &tx->txid);
	}
	return tx;
}

static struct tx *find_all_parts(struct invbloom *ib, const struct slice *one)
{
	struct slice **slices = tal_arrz(ib, struct slice *, one->index+1);
	size_t i;
	struct tx *tx;

	for (i = 0; i < ib->n_elems; i++) {
		struct slice *s = slice_in_ib(ib, i);

		if (ib->count[i] != 1)
			continue;

		if (!id_match(s, one->id))
			continue;

		if (s->index >= tal_count(slices))
			tal_resizez(&slices, s->index + 1);
		slices[s->index] = s;
	}

	tx = construct_tx(ib, slices);
	tal_free(slices);
	return tx;
}

static bool recover_their_txs(struct invbloom *ib, const struct tx *theirtxs,
			      struct double_sha **txids_recovered)
{
	size_t i;
	bool progress = false;

	for (i = 0; i < ib->n_elems; i++) {
		if (ib->count[i] == 1) {
			struct slice *s = slice_in_ib(ib, i);
			struct tx *tx = find_all_parts(ib, s);

			if (tx) {
				size_t num_recovered;
				del_tx(ib, tx);
				progress = true;

				num_recovered = tal_count(*txids_recovered);
				tal_resize(txids_recovered, num_recovered+1);
				(*txids_recovered)[num_recovered] = tx->txid;
			}
			tal_free(tx);
		}
	}

	return progress;
}

static int cmp_size_t(const size_t *a, const size_t *b, void *unused)
{
	if (*a < *b)
		return -1;
	else if (*a > *b)
		return 1;
	return 0;
}

static size_t find_tx(const struct tx *txs, size_t num,
		      const struct double_sha *txid)
{
	size_t i;

	for (i = 0; i < num; i++) {
		if (structeq(&txs[i].txid, txid))
			return i;
	}
	return (size_t)-1;
}

/* Make sure they're all matched, only once. */
static bool check_recovered(const struct tx *theirtxs,
			    const struct double_sha *recovered)
{
	size_t i, num = tal_count(recovered), *matched;

	matched = tal_arr(NULL, size_t, num);

	if (tal_count(theirtxs) != num)
		goto fail;

	for (i = 0; i < num; i++) {
		size_t match = find_tx(theirtxs, tal_count(theirtxs),
				       recovered + i);

		if (match == (size_t)-1)
			goto fail;
		matched[i] = match;
	}

	asort(matched, num, cmp_size_t, NULL);
	for (i = 0; i < num; i++)
		if (matched[i] != i)
			goto fail;

	tal_free(matched);
	return true;

fail:
	tal_free(matched);
	return false;
}

static void read_txs(const char *filename,
		     unsigned int num_theirs, unsigned int num_ours,
		     const struct tx **theirs, const struct tx **ours)
{
	FILE *txs = fopen(filename, "r");
	struct tx *us, *them;
	size_t i;

	if (!txs)
		err(1, "opening %s", filename);

	them = tal_arr(NULL, struct tx, num_theirs);
	us = tal_arr(NULL, struct tx, num_ours);

	/* After subtraction, we'll have a table with all their txs added,
	 * and our txs (which they don't have) subtracted. */
	for (i = 0; i < tal_count(them); i++)
		read_tx(txs, &them[i]);

	for (i = 0; i < tal_count(us); i++)
		read_tx(txs, &us[i]);
	fclose(txs);

	*theirs = them;
	*ours = us;
}

int main(int argc, char *argv[])
{
	struct invbloom *ib;
	size_t i, elemsize, run, runs, num_success = 0;
	const struct tx *theirtxs, *ourtxs;
	struct double_sha *txids_recovered;
	unsigned int max_mem = 1048576;
	unsigned int hashsum_bytes = 0;
	bool verbose = false;

	err_set_progname(argv[0]);

	opt_register_noarg("--verbose|-v", opt_set_bool, &verbose,
			   "Print out progress");
	opt_register_arg("--mem", opt_set_uintval_bi, opt_show_uintval_bi,
			 &max_mem,
			 "Memory to use");
	opt_register_noarg("--help|-h", opt_usage_and_exit, "",
			 "This usage message");

	opt_parse(&argc, argv, opt_log_stderr_exit);

	if (argc != 5)
		errx(1, "Usage: test-txs <numtxsadded> <numtxsmissing> <txfile> <runs>");

	/* ibt needs a counter and (optionally) another hash sum entry */
	elemsize = sizeof(struct slice) + sizeof(u32) + hashsum_bytes;

	if (verbose)
		printf("Making ib table of %zu elements\n", max_mem / elemsize);

	read_txs(argv[3], atoi(argv[1]), atoi(argv[2]), &theirtxs, &ourtxs);
	if (verbose)
		printf("Read %zu transactions\n",
		       tal_count(theirtxs) + tal_count(ourtxs));

	runs = atoi(argv[4]);

	for (run = 0; run < runs; run++) {
		bool progress;

		ib = invbloom_new(NULL, struct slice, max_mem / elemsize,
				  hashsum_bytes, run);

		if (verbose) {
			printf(">"); fflush(stdout);
		}
		for (i = 0; i < tal_count(theirtxs); i++)
			add_tx(ib, &theirtxs[i]);

		for (i = 0; i < tal_count(ourtxs); i++)
			del_tx(ib, &ourtxs[i]);

		txids_recovered = tal_arr(NULL, struct double_sha, 0);
		do {
			progress = (remove_our_txs(ib, ourtxs)
				    || recover_their_txs(ib, theirtxs,
							 &txids_recovered));
			if (verbose) {
				printf("."); fflush(stdout);
			}
		} while (progress);

		if (check_recovered(theirtxs, txids_recovered)) {
			if (verbose)
				printf("OK\n");
			num_success++;
		} else {
			if (verbose)
				printf("FAIL\n");
		}

		tal_free(ib);
	}

	printf("%zu, %zu, %zu\n",
	       tal_count(theirtxs), tal_count(ourtxs), num_success*100 / runs);
	return 0;
}
