#include <iostream>
#include <stdlib.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <assert.h>
#include <stdint.h>
#include <math.h>
#include <time.h>
#include "limits.h"
#include "lsh.h"
#include "hash.h"


void sha1_hash(const uint8_t *message, uint32_t len, uint32_t hash[5]) {
        hash[0] = UINT32_C(0x67452301);
        hash[1] = UINT32_C(0xEFCDAB89);
        hash[2] = UINT32_C(0x98BADCFE);
        hash[3] = UINT32_C(0x10325476);
        hash[4] = UINT32_C(0xC3D2E1F0);

        uint32_t i;
        for (i = 0; len - i >= 64; i += 64)
                sha1_compress(hash, message + i);

        uint8_t block[64];
        uint32_t rem = len - i;
        memcpy(block, message + i, rem);

        block[rem] = 0x80;
        rem++;
        if (64 - rem >= 8)
                memset(block + rem, 0, 56 - rem);
        else {
                memset(block + rem, 0, 64 - rem);
                sha1_compress(hash, block);
                memset(block, 0, 56);
        }

        uint64_t longLen = ((uint64_t)len) << 3;
        for (i = 0; i < 8; i++)
                block[64 - 1 - i] = (uint8_t)(longLen >> (i * 8));
        sha1_compress(hash, block);
}

// returns the hamming distance between two 64-bit fingerprints
int hamming_dist(hash_t h1, hash_t h2) {
	return __builtin_popcountll(h1 ^ h2);
}

// returns the weight of the given kmer
// 0 if the kmer should be ignored
uint32_t get_kmer_weight(const char* kmer_seq, uint32 kmer_len,
		const MarisaTrie& ref_high_freq_hist,
		const MarisaTrie& reads_low_freq_hist,
		const index_params_t* params) {

	for (uint32 k = 0; k < kmer_len; k++) {
		if(kmer_seq[k] == BASE_IGNORE) {
			return 0; // contains ambiguous bases
		}
	}

#if(USE_MARISA)
	// lookup high frequency kmer trie
	marisa::Agent agent;
	agent.set_query(kmer_seq, kmer_len);
	if(ref_high_freq_hist.lookup(agent)) {
		return 0;
	}
#endif
	return 1;
}

/////////////////////////
// --- LSH: minhash ---

bool minhash(const char* seq, const seq_t seq_len,
			const VectorBool& ref_freq_kmer_bitmap,
			const MarisaTrie& ref_freq_kmer_trie,
			const MarisaTrie& reads_hist,
			const index_params_t* params,
			VectorMinHash& min_hashes) {

	bool any_valid_kmers = false;
	uint32 n_valid_kmers = 0;

	for(uint32 i = 0; i <= (seq_len - params->k); i++) {
		// check if this kmer should be discarded
#if USE_MARISA
		if(!get_kmer_weight(&seq[i], params->k, ref_freq_kmer_trie, reads_hist, params)) continue;
#else
		uint32_t packed_kmer;
		if(pack_32(&seq[i], params->k, &packed_kmer) < 0) {
			continue; // has ambiguous bases
		}
		if(ref_freq_kmer_bitmap[packed_kmer]) {
			continue; // this is a high-freq kmer
		}
#endif
		n_valid_kmers++;
		minhash_t kmer_hash = params->kmer_hasher->encrypt_base_seq(&seq[i], params->k);
		for(uint32_t h = 0; h < params->h; h++) { // update the min values
			const rand_hash_function_t* f = &params->minhash_functions[h];
			minhash_t min = f->apply(kmer_hash);
			if(min < min_hashes[h] || !any_valid_kmers) {
				min_hashes[h] = min;
			}
		}
		any_valid_kmers = true;
	}

	return n_valid_kmers > 2*params->k;
}

void minhash_set(std::vector<minhash_t> encrypted_kmers, const index_params_t* params, VectorMinHash& min_hashes) {
	for(uint32 i = 0; i < encrypted_kmers.size(); i++) {
		minhash_t kmer_hash = encrypted_kmers[i];
		for(uint32_t h = 0; h < params->h; h++) { // update the min values
			const rand_hash_function_t* f = &params->minhash_functions[h];
			minhash_t min = f->apply(kmer_hash);
			if(min < min_hashes[h] || (i==0)) {
				min_hashes[h] = min;
			}
		}
	}
}

// avoid redundant computations
// reference-only
bool minhash_rolling_init(const char* seq, const seq_t ref_offset, const seq_t seq_len,
					minhash_matrix_t& rolling_minhash_matrix,
					const VectorBool& ref_freq_kmer_bitmask,
					const index_params_t* params,
					VectorMinHash& min_hashes) {

	// initialize the rolling matrix
	rolling_minhash_matrix.h_minhash_cols.resize(seq_len - params->k + 1);
	for(uint32 pos = 0; pos < seq_len - params->k + 1; pos++) {
		rolling_minhash_matrix.h_minhash_cols[pos].resize(params->h);
	}
	rolling_minhash_matrix.oldest_col_index = 0;

	bool any_valid_kmers = false;
	for(uint32 i = 0; i < seq_len - params->k + 1; i++) {
		if(!ref_freq_kmer_bitmask[ref_offset + i]) { // check if the kmer should be discarded
			minhash_t kmer_hash = CityHash32(&seq[ref_offset + i], params->k);
			for(uint32_t h = 0; h < params->h; h++) { // update the min values
				const rand_hash_function_t* f = &params->minhash_functions[h];
				minhash_t min = f->apply(kmer_hash);
				rolling_minhash_matrix.h_minhash_cols[i][h] = min;
				if(min < min_hashes[h] || !any_valid_kmers) {
					min_hashes[h] = min;
				}
			}
			any_valid_kmers = true;
		} else {
			for(uint32_t h = 0; h < params->h; h++) {
				rolling_minhash_matrix.h_minhash_cols[i][h] = UINT_MAX;
			}
		}
	}

	if(!any_valid_kmers) {
		std::fill(min_hashes.begin(), min_hashes.end(), UINT_MAX);
	}

	return any_valid_kmers;
}

bool minhash_rolling(const char* seq, const seq_t ref_offset, const seq_t seq_len,
					minhash_matrix_t& rolling_minhash_matrix,
					const VectorBool& ref_freq_kmer_bitmask,
					const index_params_t* params,
					VectorMinHash& min_hashes) {

	minhash_t new_kmer_hash = 0;
	bool new_kmer_hash_valid = false;
	seq_t last_kmer_pos = ref_offset + seq_len - params->k;
	if(!ref_freq_kmer_bitmask[last_kmer_pos]) { // check if the kmer should be discarded
		new_kmer_hash_valid = true;
		new_kmer_hash = CityHash32(&seq[last_kmer_pos], params->k);
	}
	bool any_valid_kmers = false;
	for(uint32 h = 0; h < params->h; h++) {
		minhash_t min_h = UINT_MAX;
		if(new_kmer_hash_valid) {
			const rand_hash_function_t* f = &params->minhash_functions[h];
			min_h = f->apply(new_kmer_hash);
		}
		// if less than current min, update min, populate oldest column
		if(min_h < min_hashes[h]) {
			min_hashes[h] = min_h;
			rolling_minhash_matrix.h_minhash_cols[rolling_minhash_matrix.oldest_col_index][h] = min_h;
		} else if(rolling_minhash_matrix.h_minhash_cols[rolling_minhash_matrix.oldest_col_index][h] != min_hashes[h]) {
			// if the minimum doesn't come from the old column, no need to recompute
			rolling_minhash_matrix.h_minhash_cols[rolling_minhash_matrix.oldest_col_index][h] = min_h;
		} else {
			// need to recompute the minimum
			min_hashes[h] = UINT_MAX;
			rolling_minhash_matrix.h_minhash_cols[rolling_minhash_matrix.oldest_col_index][h] = min_h;
			for(uint32 i = 0; i < seq_len - params->k + 1; i++) {
				if(rolling_minhash_matrix.h_minhash_cols[i][h] < min_hashes[h]) {
					min_hashes[h] = rolling_minhash_matrix.h_minhash_cols[i][h];
				}
			}
		}
		if(min_hashes[h] != UINT_MAX) {
			any_valid_kmers = true;
		}
	}
	rolling_minhash_matrix.oldest_col_index = (rolling_minhash_matrix.oldest_col_index + 1) % (seq_len - params->k + 1);
	return any_valid_kmers;
}

/////////////////////////
// --- LSH: simhash ---

// for each bit position i in the kmer hash
// if hash[i] is 1: increment v[i]; otherwise, decrement v[i]
void add_kmer_hash_bits(int* v, hash_t hash) {
	for(int b = 0; b < SIMHASH_BITLEN; b++) {
		if(((hash >> b) & 1) == 1) {
			v[b]++;
		} else {
			v[b]--;
		}
	}
}

// computes the simhash fingerprint
hash_t generate_simhash_fp(int* v) {
	hash_t simhash = 0;
	for (int b = 0; b < SIMHASH_BITLEN; b++) {
		if(v[b] >= 0) {
			simhash |= (1ULL << b);
		}
	}
	return simhash;
}

// computes the simhash fingerprint of the given sequence
// using the specified kmer generation scheme
//hash_t simhash(const char* seq, const seq_t seq_offset, const seq_t seq_len,
//		const MapKmerCounts& ref_hist, const MapKmerCounts& reads_hist,
//		const index_params_t* params, const uint8_t is_ref) {
//
//	int v[SIMHASH_BITLEN] = { 0 };
//
//	// generate the kmers, hash them, and add the hash to V
//	if(params->kmer_type == SPARSE) {
//		char* kmer = (char*) malloc(params->k*sizeof(char));
//		for(uint32_t i = 0; i < params->m; i++) {
//			const uint32_t* ids = &params->sparse_kmers[i*params->k];
//			for(uint32_t j = 0; j < params->k; j++) {
//				kmer[j] = seq[seq_offset + ids[j]];
//			}
//			if(get_kmer_weight(kmer, params->k, ref_hist, reads_hist, is_ref, params) == 0) {
//				continue;
//			}
//			hash_t kmer_hash = CityHash64(kmer, params->k);
//			add_kmer_hash_bits(v, kmer_hash);
//		}
//	} else {
//		for(uint32_t i = 0; i <= (seq_len - params->k); i += params->kmer_dist) {
//			if(get_kmer_weight(&seq[seq_offset + i], params->k, ref_hist, reads_hist, is_ref, params) == 0) {
//				continue;
//			}
//			hash_t kmer_hash = CityHash64(&seq[seq_offset + i], params->k);
//			add_kmer_hash_bits(v, kmer_hash);
//		}
//	}
//	return generate_simhash_fp(v);
//}

/////////////////////////
// --- LSH: sampling ---

//hash_t sampling(const char* seq, const seq_t seq_offset, const seq_t i, const index_params_t* params) {
//	hash_t fingerprint = 0;
//	const uint32_t* idxs = &params->sparse_kmers[i*params->k];
//	for(uint32_t j = 0; j < params->k; j++) {
//		const char c = seq[seq_offset + idxs[j]];
//		fingerprint |= (c & 1ULL) << j; // 1st ls bit
//	}
//	return fingerprint;
//}
//
//hash_t sampling_hash(const char* seq, const seq_t seq_offset, const seq_t i, const index_params_t* params) {
//	const uint32_t* idxs = &params->sparse_kmers[i*params->k];
//	char* kmer = (char*) malloc(params->k*sizeof(char));
//	for(uint32_t j = 0; j < params->k; j++) {
//		kmer[j] = seq[seq_offset + idxs[j]];
//	}
//	return CityHash64(kmer, params->k);
//}


