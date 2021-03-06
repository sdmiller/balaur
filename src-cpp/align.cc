#include <float.h>

#if(SEQAN_LIB)
#include <seqan/seeds.h>
#include <seqan/graph_algorithms.h>
#include <seqan/basic.h>
#include <seqan/sequence.h>
#include <seqan/stream.h>
#endif

#include <openssl/sha.h>
#include <emmintrin.h>
#include <smmintrin.h>
#include <iomanip>
#include <fstream>
#include <stdlib.h>
#include <unordered_set>
#include <stdio.h>
#include <iostream>
#include <string.h>
#include <assert.h>
#include <stdint.h>
#include <math.h>
#include <time.h>
#include <algorithm>
#include <utility>
#include <limits.h>
#include <queue>
#include <unordered_map>
#include <bitset>
#include <omp.h>
#include "index.h"
#include "io.h"
#include "hash.h"
#include "sam.h"
#include "lsh.h"

#if(CLASP_LIB)
extern "C" {
#include "clasp_v1_1/libs/sltypes.h"
#include "clasp_v1_1/libs/slchain.h"
#include "clasp_v1_1/libs/container.h"
#include "clasp_v1_1/libs/debug.h"
#include "clasp_v1_1/libs/mathematics.h"
#include "clasp_v1_1/libs/sort.h"
#include "clasp_v1_1/libs/vqueue.h"
#include "clasp_v1_1/libs/vebtree.h"
#include "clasp_v1_1/libs/bintree.h"
#include "clasp_v1_1/libs/rangetree.h"
}
#endif

inline bool pos_in_range_sig(int pos1, int pos2, uint32 delta) {
        return abs(pos1 - pos2) <= delta;
}

inline bool pos_in_range(uint32 pos1, uint32 pos2, uint32 delta) {
	return abs(pos1 - pos2) <= delta;
}

inline bool pos_in_range_asym(uint32 pos1, uint32 pos2, uint32 delta1, uint32 delta2) {
	uint32 delta11 = pos2 > delta1 ? delta1 : pos2;
	return pos1 > pos2 - delta11 && pos1 < pos2 + delta2;
}

struct heap_entry_t {
	seq_t pos;
	uint32_t len;
	uint16_t tid;
	uint16_t next_idx;
};

void heap_sort(heap_entry_t* heap, int n) {
	heap_entry_t tmp;
	int i, j;
	for(j = 1; j < n; j++) {
		tmp = heap[j];
		for(i = j - 1; (i >= 0) && (heap[i].pos > tmp.pos); i--) {
			heap[i+1] = heap[i];
		}
		heap[i+1] = tmp;
	}
}

inline void heap_set_min(heap_entry_t* heap, int n) {
	int min_idx = 0;
	for(int i = 1; i < n; i++) {
		if(heap[i].pos < heap[min_idx].pos) {
			min_idx = i;
		}
	}
	heap_entry_t tmp = heap[0];
	heap[0] = heap[min_idx];
	heap[min_idx] = tmp;
}

inline void heap_update(heap_entry_t* heap, uint32 n) {
	uint32 i = 0;
	uint32 k = i;
	heap_entry_t tmp = heap[i];
	while((k = (k << 1) + 1) < n) {
		if(k != (n - 1) && (heap[k].pos >= heap[k+1].pos)) ++k;
		if(heap[k].pos >= tmp.pos) break;
		heap[i] = heap[k];
		i = k;
	}
	heap[i] = tmp;
}

// min-heap
void swap(heap_entry_t*x, heap_entry_t*y) {
	heap_entry_t temp = *x;  *x = *y;  *y = temp;
}

void sift_down(heap_entry_t* heap, int n, int i) {
	int l = 2*i + 1;
	int r = 2*i + 2;
	int min = i;
	if (l < n && heap[l].pos < heap[i].pos) {
		min = l;
	}
	if (r < n && heap[r].pos < heap[min].pos) {
		min = r;
	}
	if (min != i) {
		swap(&heap[i], &heap[min]);
	    sift_down(heap, n, min);
	}
}

void heap_create(heap_entry_t* heap, int n) {
	int i = (n-1)/2;
	while(i >= 0) {
		sift_down(heap, n, i);
		i--;
	}
}

inline void heap_update_memmove(heap_entry_t* heap, uint32 n) {
	if(n <= 1) return;
	if(heap[1].pos >= heap[0].pos) return; // nothing to shift

	heap_entry_t tmp = heap[0];
	if(heap[n-1].pos <= heap[0].pos) { // shift all
		memmove(heap, heap+1, (n-1)*sizeof(heap_entry_t));
		heap[n-1] = tmp;
	} else { // binary search: find the first element > tmp
		int i, j, k;
		i = 1;
		j = n;
		while(i != j) {
			k = (i + j)/2;
			if (heap[k].pos < tmp.pos) {
				i = k + 1;
			} else {
				j = k;
			}
		}
		memmove(heap, heap+1, (i)*sizeof(heap_entry_t));
		heap[i-1] = tmp;
	}
}

int get_next_contig(const ref_t& ref, const std::vector<std::pair<uint64, minhash_t> >& ref_bucket_matches_by_table, uint32 t, heap_entry_t* entry) {
	const minhash_t read_proj_hash = ref_bucket_matches_by_table[t].second;
	const uint64 bid = ref_bucket_matches_by_table[t].first;
	if(bid == ref.index.bucket_offsets.size()) { // table ignored
		return 0;
	}
	const uint64 bucket_data_offset = ref.index.bucket_offsets[bid];
	const uint64 bucket_data_size = ref.index.bucket_offsets[bid+1] - bucket_data_offset;

	//const uint64 bucket_data_offset = r->bucket_offsets[t];
	//const uint64 bucket_data_size = r->bucket_offsets[t+1] - bucket_data_offset;

	// get the next entry in the bucket that matches the read projection hash value
	bool first = entry->next_idx == 0;
	if(first) {
		loc_t l;
		l.hash = read_proj_hash;
		l.pos = 0;
		l.len = 0;
		std::vector<loc_t>::const_iterator range_start = std::lower_bound(ref.index.buckets_data.begin() + bucket_data_offset, ref.index.buckets_data.begin() + bucket_data_offset + bucket_data_size, l, comp_loc()); 	
		entry->next_idx = std::distance(ref.index.buckets_data.begin() + bucket_data_offset, range_start);
	}
	//while(entry->next_idx < bucket_data_size) {
		if(ref.index.buckets_data[bucket_data_offset + entry->next_idx].hash == read_proj_hash) {
			entry->pos = ref.index.buckets_data[bucket_data_offset + entry->next_idx].pos;
			entry->len = ref.index.buckets_data[bucket_data_offset + entry->next_idx].len;
			entry->tid = t;
			entry->next_idx++;
			return 1;
		}
		//if(!first) {
		//	return 0;
		//}
		//entry->next_idx++;
	//}
	return 0;
}

#define CONTIG_PADDING 50
void process_merged_contig(seq_t contig_pos, uint32 contig_len, int n_diff_table_hits, const ref_t& ref, read_t* r, const bool rc, const index_params_t* params) {
#if(SIM_EVAL)
	 if(pos_in_range_asym(r->ref_pos_r, contig_pos, contig_len + params->ref_window_size, params->ref_window_size) ||
		pos_in_range_asym(r->ref_pos_l, contig_pos, contig_len + params->ref_window_size, params->ref_window_size))  {
		r->collected_true_hit = true;
		r->processed_true_hit = true;
		r->true_n_bucket_hits = n_diff_table_hits;
	}
#endif

	// filters
	if(contig_len > params->max_matched_contig_len) return;
	if(n_diff_table_hits < (int) params->min_n_hits) return;
	if(n_diff_table_hits < (int) (r->best_n_bucket_hits - params->dist_best_hit)) return;

	// passed filters
	if(n_diff_table_hits > r->best_n_bucket_hits) { // if more hits than best so far
		r->best_n_bucket_hits = n_diff_table_hits;
	}
	const seq_t hit_offset = contig_pos - contig_len + 1;
	const seq_t padded_hit_offset = (hit_offset >= CONTIG_PADDING) ? hit_offset - CONTIG_PADDING : 0;
	const uint32 search_len = contig_len + 2*CONTIG_PADDING + r->len;
	ref_match_t rm(padded_hit_offset, search_len, rc, n_diff_table_hits);
	r->ref_matches.push_back(rm);
	r->n_proc_contigs++;

	/* ---- split the ref contig
	int contig_chunk_size = (r->len + 1000);
	int n_contigs = ceil(((double)contig_len/contig_chunk_size));
	for(int x = 0; x < n_contigs; x++) {
		seq_t p = contig_pos - (n_contigs-1-x)*contig_chunk_size;
		int l = contig_chunk_size;
		if(x == 0) {
			l = contig_len - (n_contigs-1)*contig_chunk_size;
		}
		ref_match_t rm(p, l, rc);
		compute_ref_contig_votes(rm, ref, r, params);
	}*/

#if(SIM_EVAL)
	 if(pos_in_range_asym(r->ref_pos_r, contig_pos, contig_len + params->ref_window_size, params->ref_window_size) ||
		pos_in_range_asym(r->ref_pos_l, contig_pos, contig_len + params->ref_window_size, params->ref_window_size))  {
		r->bucketed_true_hit = n_diff_table_hits;
	}
#endif
}

#define N_TABLES_MAX 1024
// output matches (ordered by the number of projections matched)
void collect_read_hits(const ref_t& ref, read_t* r, const bool rc, const index_params_t* params) {
	r->ref_matches.reserve(10);

	// priority heap of matched positions
	heap_entry_t heap[params->n_tables];
	int heap_size = 0;
	// push the first entries in each sorted bucket onto the heap
	for(uint32 t = 0; t < params->n_tables; t++) { // for each table
		heap[heap_size].next_idx = 0;
		if(get_next_contig(ref, (rc ? r->ref_bucket_matches_by_table_rc : r->ref_bucket_matches_by_table_f), t, &heap[heap_size]) > 0) {
			heap_size++;
		}
	}
	if(heap_size == 0) return; // all the matched buckets are empty
	heap_sort(heap, heap_size); // build heap

	int n_diff_table_hits = 0;
	int len = 0;
	seq_t last_pos = -1;
	std::bitset<N_TABLES_MAX> occ;
	while(heap_size > 0) {
		heap_entry_t e = heap[0]; // get min
		seq_t e_last_pos = e.pos + e.len - 1;
		if(last_pos == (seq_t) -1 || (e.pos <= last_pos)) {
			if(last_pos == (seq_t) -1) { // first contig
				len = e.len - 1;
				last_pos = e_last_pos;
			} else if(last_pos < e_last_pos) { // extending contig
				len += e_last_pos - last_pos;
				last_pos = e_last_pos;
			}
			if(!occ.test(e.tid)) {
				n_diff_table_hits++;
			}
			occ.set(e.tid);
		} else {
			// found a boundary, store/handle last contig
			process_merged_contig(last_pos, len, n_diff_table_hits, ref, r, rc, params);

			// start a new contig
			n_diff_table_hits = 1;
			len = e.len - 1;
			last_pos = e_last_pos;
			occ.reset();
			occ.set(e.tid);
		}
		// push the next match from this bucket
		if(get_next_contig(ref, (rc ? r->ref_bucket_matches_by_table_rc : r->ref_bucket_matches_by_table_f), e.tid, &heap[0]) > 0) {
			heap_update(heap, heap_size);
		} else { // no more entries in this bucket
			heap[0] = heap[heap_size - 1];
			heap_size--;
			heap_update(heap, heap_size);
		}
	}

	// add the last position
	if(last_pos != (seq_t) -1) {
		process_merged_contig(last_pos, len, n_diff_table_hits, ref, r, rc, params);
	}
}

inline bool is_unique_kmer(const std::vector<std::pair<kmer_cipher_t, pos_cipher_t>>& sorted_kmers, const uint32 idx) {
	bool unique = true;
	kmer_cipher_t hash = sorted_kmers[idx].first;
	if(idx > 0 && sorted_kmers[idx-1].first == hash) {
		unique = false;
	} 
	if(idx < sorted_kmers.size()-1 && sorted_kmers[idx+1].first == hash) {
		unique = false;
	}
	return unique;
}

static const int N_INIT_ANCHORS_MAX = (getenv("N_INIT_ANCHORS_MAX") ? atoi(getenv("N_INIT_ANCHORS_MAX")) : 20);
#define UNIQUE1 0
#define UNIQUE2 1
void ransac(const ref_match_t ref_contig, const seq_t rlen,
			std::vector<std::pair<kmer_cipher_t, pos_cipher_t>>& read_ciphers,
			std::vector<std::pair<kmer_cipher_t, pos_cipher_t>>& contig_ciphers,
			const index_params_t* params, int* n_votes, int* pos) {

	int anchors[2][N_INIT_ANCHORS_MAX];
	uint32 n_anchors[2] = { 0 };
	int anchor_median_pos[2] = { 0 };

	for(int p = 0; p < 3; p++) {
		// start from the beginning in each pass
		uint32 idx_q = 0;
		uint32 idx_r = 0;
		while(idx_q < read_ciphers.size() && idx_r < contig_ciphers.size()) {
			if(contig_ciphers[idx_r].first == read_ciphers[idx_q].first) { // MATCH
				int match_aln_pos = contig_ciphers[idx_r].second - read_ciphers[idx_q].second;
				if(p == 0) { // -------- FIRST PASS: collect unique matches ---------
					if(is_unique_kmer(read_ciphers, idx_q)) {
						if(n_anchors[UNIQUE1] < params->n_init_anchors) {
							anchors[UNIQUE1][n_anchors[UNIQUE1]] = match_aln_pos;
							n_anchors[UNIQUE1]++;
						} else { // found all the anchors
							break;
						}
					}
					idx_q++;
					idx_r++;
			} else if(p == 1) { // --------- SECOND PASS: collect DIFF unique matches -------
					if(n_anchors[UNIQUE1] == 0) break; // no unique matches exist
					// compute the median first pass position
					if(anchor_median_pos[UNIQUE1] == 0) {
						std::sort(&anchors[UNIQUE1][0], &anchors[UNIQUE1][0] + n_anchors[UNIQUE1]);
						anchor_median_pos[UNIQUE1] = anchors[UNIQUE1][(n_anchors[UNIQUE1]-1)/2];
					}
					if(is_unique_kmer(read_ciphers, idx_q)) {
						// if this position is not close to the first pick
						if(!pos_in_range_sig(match_aln_pos, anchor_median_pos[UNIQUE1], params->delta_x*params->delta_inlier)) {
							if(n_anchors[UNIQUE2] < params->n_init_anchors) {
								anchors[UNIQUE2][n_anchors[UNIQUE2]] = match_aln_pos;
								n_anchors[UNIQUE2]++;
							} else { // found all the anchors
								break;
							}
						}
					}
					idx_q++;
					idx_r++;
				} else { // --------- THIRD PASS: count inliers to each candidate position -------
					if(n_anchors[UNIQUE1] == 0) break;
					// find the median alignment positions
					if(n_anchors[UNIQUE2] > 0 && anchor_median_pos[UNIQUE2] == 0) {
						std::sort(&anchors[UNIQUE2][0], &anchors[UNIQUE2][0] + n_anchors[UNIQUE2]);
						anchor_median_pos[UNIQUE2] = anchors[UNIQUE2][(n_anchors[UNIQUE2]-1)/2];
					}
					// count inliers
					for(int i = 0; i < 2; i++) {
						if(n_anchors[i] == 0) continue;
						if(pos_in_range_sig(anchor_median_pos[i], match_aln_pos, params->delta_inlier)) {
							n_votes[i]++;
							pos[i] += match_aln_pos;
						}
					}
					if(idx_r < (contig_ciphers.size()-1) && contig_ciphers[idx_r + 1].first == contig_ciphers[idx_r].first) {
						// if the next kmer is still a match in the reference, check it in next iteration as an inlier
						idx_r++;
					} else {
						idx_q++;
						idx_r++;
					}
				}
			} else if(read_ciphers[idx_q].first < contig_ciphers[idx_r].first) {
				idx_q++;
			} else {
				idx_r++;
			}
		}
	}
	for(int i = 0; i < 2; i++) {
		if(n_votes[i] > 0) {
			pos[i] = pos[i]/(int)n_votes[i];
			//std::cout << " final pos " << pos[i] << " n_inliners " << n_inliers[i] << "\n";
		}
	}

	if(n_anchors[UNIQUE1] < 5) {
		n_votes[0] = 0;
		n_votes[1] = 0;
		//votes_by_pos(ref_kmers, read_kmers, params, n_inliers, pos, total_n_matches);
	}
}


#define MATCH 1
#define ERROR 2
#define UNKNOWN 0
void propagate_matches(read_t* r, const int contig_pos,
		const std::vector<std::pair<minhash_t, uint16_t>>& contig_kmers,
		const std::vector<std::pair<minhash_t, uint16_t>>& read_kmers,
		const uint8* kmer_mask, const uint16_t kmer_mask_len,
		uint16_t* min_n_matches, uint16_t* min_n_errors,
		const index_params_t* params) {

	// find all the kmers in the read that have consistent matches
	std::vector<uint8> kmer_matches(read_kmers.size());
	//uint16_t last_match_pos = contig_pos > (int) params->delta_inlier ? contig_pos - params->delta_inlier : 0;
	int found = 0;
	for(uint16_t i = 0; i < read_kmers.size(); i++) {
		minhash_t kmer = read_kmers[i].first;
		// find the match in the contig if exists
		int start_pos = contig_pos + i > (int) params->delta_inlier ? contig_pos + i - params->delta_inlier : 0;
		int end_pos = contig_pos + i + params->delta_inlier;
		if(end_pos < 0) end_pos = 0; 
		for(int j = start_pos; j < end_pos; j++) {
		//for(uint16_t j = 0; j < contig_kmers.size(); j ++) {
			if(j > (int) contig_kmers.size() - 1) break;
			if(contig_kmers[j].first == kmer) {
				kmer_matches[i] = 1;
				//last_match_pos = j; // the next match must occur at a later position
				found++;
				break;
			}
		}
	}

	// mark positions in the read that correspond to kmer matches
	std::vector<uint8> base_matches(r->len);
	for(uint16_t i = 0; i < kmer_matches.size(); i++) {
		if(kmer_matches[i] == 0) continue;
		for(uint16_t j = 0; j < kmer_mask_len; j++) {
			if(kmer_mask[j] == 1) {
				base_matches[i+j] = MATCH;
			}
		}
	}
	
	for(uint16_t i = 0; i < base_matches.size(); i++) {
		if(base_matches[i] == MATCH) {
			 (*min_n_matches) += 1;
		}
	}

	//std::cout << base_matches.size() << " matches " <<  *min_n_matches << "\n";
	// propagate known positions to determine errors and known positions
	for(uint16_t i = 0; i < kmer_matches.size(); i++) {
		if(kmer_matches[i]) continue;
		int n_kmer_bases = 0;
		int n_matched_kmer_bases = 0;
		uint16_t mismatch_pos = 0;
		for(uint16_t j = 0; j < kmer_mask_len; j++) {
			if(kmer_mask[j]) {
				n_kmer_bases++;
				if(base_matches[i+j] == MATCH) {
					n_matched_kmer_bases++;
				} else {
					mismatch_pos = i + j;
				}
			}
		}
		// must be an error if only one unknown
		if(n_matched_kmer_bases == n_kmer_bases - 1) {
			base_matches[mismatch_pos] = ERROR;
			(*min_n_errors)++;
		}
	}
	//std::cout << " inliers[0] " << kmer_inliers[0] << " inliers[1] " << kmer_inliers[1] << " min_match[0] " << min_match[0] << " min_match[1] " << min_match[1] << "\n";
}

static int max_repeats = 0;
static int max_repeats_contig = 0;
static std::vector<int> n_repeats_v;
static std::vector<int> n_repeats_v_contig;

void generate_voting_kmer_ciphers_read(kmer_cipher_t* ciphers, const char* seq, const seq_t seq_len, const uint64 key1, const uint64 key2, const ref_t& ref, const index_params_t* params) {

	const int n_kmers = seq_len - params->k2 + 1;
    	uint32_t hash[5];
	for(int i = 0; i < n_kmers; i++) {
#if(VANILLA)
		ciphers[i] = CityHash64(&seq[i], params->k2);
#else
		sha1_hash(reinterpret_cast<const uint8_t*>(&seq[i]), params->k2, hash);
		ciphers[i] = ((uint64) hash[0] << 32 | hash[1]);
#endif
	}

#if(!VANILLA)
	__m128i* c = (__m128i*)ciphers;
        __m128i xor_pad = _mm_set1_epi64((__m64)key1);
        for(int i = 0; i < n_kmers/2; i++) {
                c[i] = _mm_xor_si128(c[i], xor_pad);
                ciphers[2*i] *= key2;
                ciphers[2*i+1] *= key2;
        }
        for(int i = 2*(n_kmers/2); i < n_kmers; i++) {
                ciphers[i] ^= key1;
                ciphers[i] *= key2;
        }
	
	std::unordered_map<kmer_cipher_t, int> s;
	std::pair<std::unordered_map<kmer_cipher_t, int>::iterator, bool> r;
	for(int i = 0; i < n_kmers; i++) {
		r = s.insert(std::make_pair(ciphers[i], i));
		if(!r.second) {
			ciphers[(r.first)->second] = genrand64_int64();
			ciphers[i] = genrand64_int64();
		}
	}
#endif
}

void generate_voting_kmer_ciphers_ref(kmer_cipher_t* ciphers, const char* seq, const seq_t seq_offset, const seq_t seq_len,
		const uint64 key1, const uint64 key2, const ref_t& ref, const index_params_t* params) {

	const int n_kmers = seq_len - params->k2 + 1;
	memcpy(&ciphers[0], &ref.precomputed_kmer2_hashes[seq_offset], n_kmers*sizeof(kmer_cipher_t));

#if(!VANILLA)
	for(int i = 0; i < n_kmers; i+= params->sampling_intv) {
		uint16_t r = ref.precomputed_neighbor_repeats[seq_offset + i];
		if(ciphers[i] != 0 && (r == 0 || r >= (n_kmers-i))) {
			ciphers[i/params->sampling_intv] = (ciphers[i] ^ key1)*key2;
		} else {
			ciphers[i/params->sampling_intv] = genrand64_int64();
                        ciphers[i+r] = 0;
		}
	}
#endif
}

#define CHAIN_VOTES 1

#include "lemon/list_graph.h"
#include <lemon/connectivity.h>
#include <lemon/concepts/graph.h>
#include <lemon/concepts/graph_components.h>

using namespace lemon;


struct node_comp
{
	ListDigraph::NodeMap<int>* order;
	
	node_comp(ListDigraph::NodeMap<int>* _order) {
		 order = _order;
	}

    	bool operator()( const ListDigraph::Node& x, const ListDigraph::Node& y ) const {
    		return (*order)[x] < (*order)[y];
    	}
};

void chain_fragments_dp(std::vector<std::pair<pos_cipher_t, pos_cipher_t>>& fragments, std::vector<int>& fragment_weights, std::vector<int>& best_chain) {
	int n_frags = fragments.size();	
	
	// initialize DAG
	ListDigraph g;
	ListDigraph::NodeMap<int> frag_weights(g, 0); 
	ListDigraph::ArcMap<double> gap_weights(g, 0);
	ListDigraph::NodeMap<int> frag_ids(g);

	std::map<int, ListDigraph::Node> fragId_to_node;

	// add source node
	ListDigraph::Node source = g.addNode();
	ListDigraph::Node sink = g.addNode();
	frag_weights.set(source, 0);
	frag_weights.set(sink, 0);

	// each fragment is a node
	for(int i = 0; i < n_frags; i++) {
		// create new node with weight
		ListDigraph::Node u = g.addNode();
		frag_weights.set(u, 5*fragment_weights[i]);
		// link to source and sink with gap penalty 0
		ListDigraph::Arc a = g.addArc(source, u);
		ListDigraph::Arc b = g.addArc(u, sink);
		gap_weights.set(a, 0);
		gap_weights.set(b, 0);
		frag_ids.set(u, i);
		fragId_to_node.insert(std::make_pair(i, u));
	}

	//std::cout << "created graph init \n";
	int n_edges = 0;

	int mm_score = 0;
	int gap_score = 10;
	int fixed_cost = 2;
	for(int i = 0; i < n_frags; i++) {
		ListDigraph::Node u = fragId_to_node.find(i)->second;
		for(int j = i+1; j < n_frags; j++) {
			int delta_x = fragments[j].first - (fragments[i].first + fragment_weights[i]);
			int delta_y = fragments[j].second - (fragments[i].second + fragment_weights[i]);
			if(delta_x >= 0 && delta_y >= 0) {
				// add edge
				ListDigraph::Node v = fragId_to_node.find(j)->second;
				ListDigraph::Arc  a = g.addArc(u, v);
				
				int gap = delta_x > delta_y ? delta_x - delta_y : delta_y - delta_x;
				int mm = delta_x > delta_y ? delta_y : delta_x;
				gap_weights.set(a, gap*gap_score + mm_score*mm +fixed_cost); // custom gap scoring function
				n_edges++;
			}
		}
	}
	//std::cout << "created graph full n_edges=" << n_edges << "\n";
	

	// topological sort
	ListDigraph::NodeMap<int> order(g);
	topologicalSort(g, order);

	//std::cout << "topsort \n";

	std::vector<ListDigraph::Node> nodes;
	for(ListDigraph::NodeIt u(g); u != INVALID; ++u) {
		nodes.push_back(u);
		//std::cout << order[u] << "\n";
	}
	//std::cout << " n_nodes " << nodes.size() << "\n";
	std::sort(nodes.begin(), nodes.end(), node_comp(&order));
	
	//std::cout << " sorted! \n";
	
	ListDigraph::NodeMap<int> best_score(g, 0);
	best_score.set(source, 0);
	ListDigraph::NodeMap<ListDigraph::Node> pred(g);
	for(int i = 1; i < n_frags + 2; i++) {
		ListDigraph::Node u = nodes[i]; // ordered	
		
		int max_score = 0;
		ListDigraph::Node pred_n;
		// get incoming edges
		for(ListDigraph::InArcIt e(g,u); e != INVALID; ++e ) {
			ListDigraph::Node v = g.source(e);
			// get the best score to this node
			int score_v = best_score[v];
			int score = score_v - gap_weights[e] + frag_weights[u];
			if(score >= max_score) {
				max_score = score;
				pred_n = v;
			}
		}
		best_score.set(u, max_score);
		pred.set(u, pred_n);
	}

	// start at sink
	ListDigraph::Node n = sink;
	int best_chain_score = best_score[sink];
	while(n != source) {
		n = pred[n];
		if(n != source) best_chain.push_back(frag_ids[n]);
		//std::cout << "frag " << frag_ids[n] << " " << best_score[n] << "\n";
	}
	//std::cout << "best chain score " << best_chain_score << "\n";
	
}

void chain_fragments_local(std::vector<std::pair<pos_cipher_t, pos_cipher_t>>& fragments, std::vector<int>& fragment_weights) {

#if(CLASP_LIB)
        int n_frags = fragments.size();
        slmatch_t* frags =  (slmatch_t *) malloc(n_frags*sizeof(slmatch_t));
        for (int i = 0; i < n_frags; i++) {
                slmatch_t* frag = &frags[i];
                bl_slmatchInit(frag, 0);
                frag->p = fragments[i].first;
                frag->q = fragment_weights[i];
                frag->i = fragments[i].second;
                frag->j = fragment_weights[i];
                frag->scr = fragment_weights[i]*10;
                frag->idx = i;
                frag->subject = 0;
        }
        int eps = 0;
        int lambda = 1;
        int maxgap = -1;
        bl_slChainSop(frags, n_frags, eps, lambda, maxgap);

        for(int i = 0; i < n_frags; i++){
                slmatch_t *match = &frags[i];
                std::cout << "FRAG " << match->i << " " <<  match->i + match->j - 1 << " " << match->p << " " <<  match->p + match->q - 1 << " " <<  match->scr << " \n";
                if (match->chain) {
                        slchain_t *chain = (slchain_t *) match->chain;
                        if (chain->scr >= 0 && bl_containerSize(chain->matches) >= 1) {
                                fprintf(stderr, "CHAIN %d\t%d\t%d\t%d\t%.3f\n", chain->i, chain->i + chain->j - 1, chain->p, chain->p + chain->q - 1, chain->scr);
                                for (int k = 0; k < bl_containerSize(chain->matches); k++){
                                        slmatch_t *frag = *(slmatch_t **) bl_containerGet(chain->matches, k);
                                        fprintf(stderr, "%d\t%d\t%d\t%d\t%.3f\n", frag->i, frag->i + frag->j - 1, frag->p, frag->p + frag->q - 1, frag->scr);
                                }
                        }
                        //bl_slchainDestruct(chain);
                        //free(chain);
                        //match->chain = NULL;
                }
        }
#endif

}

void chain_fragments_global(std::vector<std::pair<pos_cipher_t, pos_cipher_t>>& fragments, std::vector<int>& fragment_weights) {

#if(SEQAN_LIB)
	seqan::String<seqan::Seed<int, seqan::MultiSeed>> s_fragments;
        for (int i = 0; i < fragments.size(); i++) {
                seqan::Seed<int, seqan::MultiSeed> seed(2);
                seqan::setLeftPosition(seed, 0, fragments[i].first);
                seqan::setRightPosition(seed, 0, fragments[i].first + fragment_weights[i]);
                seqan::setLeftPosition(seed, 1, fragments[i].second);
                seqan::setRightPosition(seed, 1, fragments[i].second + fragment_weights[i]);
                seqan::setWeight(seed, fragment_weights[i]*10);
                seqan::appendValue(s_fragments, seed);
        }

        int gap_diff_score = 3;
        int gap_m_score = 1;
        seqan::String<seqan::Seed<int, seqan::MultiSeed> > global_chain;
        seqan::Score<int, seqan::ChainSoP> gap_scoring(0, gap_m_score, gap_diff_score);
        int chain_score = seqan::globalChaining(s_fragments, global_chain, gap_scoring);

        int p1_x = seqan::leftPosition(global_chain[1], 0);
        int p1_y = seqan::leftPosition(global_chain[1], 1);
        int min_p_start = (p1_x < p1_y) ? p1_x : p1_y;
        int diff_p_start = (p1_x < p1_y) ? p1_y - p1_x : p1_x - p1_y;
        chain_score += min_p_start*gap_m_score;
        chain_score += diff_p_start*gap_diff_score;

        int p2_x = seqan::leftPosition(global_chain[seqan::length(global_chain) - 1], 0) - seqan::leftPosition(global_chain[seqan::length(global_chain) - 2], 0);
        int p2_y = seqan::leftPosition(global_chain[seqan::length(global_chain) - 1], 1) - seqan::leftPosition(global_chain[seqan::length(global_chain) - 2], 1);
        int min_p_end = (p2_x < p2_y) ? p2_x : p2_y;
        int diff_p_end = (p2_x < p2_y) ? p2_y - p2_x : p2_x - p2_y;
        chain_score += min_p_end*gap_m_score;
        chain_score += diff_p_end*gap_diff_score;


        //for (int i = 0; i < seqan::length(global_chain); i++) {
        //      std::cout <<  seqan::leftPosition(global_chain[i], 0) << " " <<  seqan::leftPosition(global_chain[i], 1) << ";";
        //}
        //std::cout << "(CHAIN SCORE =" << chain_score << ")\n";
        //std::cout << chain_score << "\n";

        // HIS over the fragments
        //seqan::String<unsigned int> weights;
        //seqan::resize(weights, fragment_weights.size(), 0);
        //for(int i = 0; i < fragment_weights.size(); i++) {
        //      seqan::assignProperty(weights, i, fragment_weights[i]);
        //}
        //typedef seqan::Position<seqan::String<unsigned int> >::Type TPosition;
        //seqan::String<TPosition> spos;
        //w = heaviestIncreasingSubsequence(fragments, weights, spos);

        //for (int i = 0; i < fragments.size(); i++) {
        //      std::cout << fragments[i] << "(Weight=" << seqan::getProperty(weights, i) << "),";
        //}
        //std::cout << "\n" << "His: \n";
        //for (int i = length(spos) - 1; i >= 0; i--) {
        //      std::cout << fragments[spos[i]] <<  ',';
        //}
        //std::cout << "(HIS Weight=" << w << ")\n";
#endif
}


void vote_cast_and_count_chaining(const ref_match_t ref_contig, const seq_t rlen,
		std::vector<std::pair<kmer_cipher_t, pos_cipher_t>>& read_ciphers,
		std::vector<std::pair<kmer_cipher_t, pos_cipher_t>>& contig_ciphers,
		const index_params_t* params, int* n_votes, int* pos) {

	std::vector<std::pair<pos_cipher_t, pos_cipher_t>> matched_kmers;
	int pos0 = rlen;
	uint32 skip = 0;
	for(uint32 i = 0; i < read_ciphers.size(); i++) {
		if(read_ciphers[i].first == 0) continue;
		if(!is_unique_kmer(read_ciphers, i)) continue;
		for(uint32 j = skip; j < contig_ciphers.size(); j++) {
			if(!is_unique_kmer(contig_ciphers, j)) continue;
			if(read_ciphers[i].first == contig_ciphers[j].first) {
				int match_aln_pos = contig_ciphers[j].second - read_ciphers[i].second;
				matched_kmers.push_back(std::make_pair(contig_ciphers[j].second, read_ciphers[i].second));
				//skip = j+1;
				//break;
			} else if(contig_ciphers[j].first > read_ciphers[i].first) {
				skip = j;
				break;
			}
		}
	}
	
	if(matched_kmers.size() == 0) return;
	std::sort(matched_kmers.begin(), matched_kmers.end());

	// find maximal overlapping fragments
	std::vector<int> fragment_weights;
	std::vector<std::pair<pos_cipher_t, pos_cipher_t>> fragments;
	int idx = 0;
	fragments.push_back(std::make_pair(matched_kmers[0].first, matched_kmers[0].second));
	fragment_weights.push_back(params->k2);
	//std::cout << "SORTED ORIG MATCH " << matched_kmers[0].first - matched_kmers[0].second << " " << matched_kmers[0].first << " " << matched_kmers[0].second << "\n";
	for(int i = 1; i < matched_kmers.size(); i++) {
		int match_aln_pos = matched_kmers[i].first - matched_kmers[i].second;
		//std::cout << "SORTED ORIG MATCH " << match_aln_pos << " " << matched_kmers[i].first << " " << matched_kmers[i].second << "\n";
		//if((matched_kmers[i].first == matched_kmers[i-1].first + params->sampling_intv) && 
	   	//(matched_kmers[i].second == matched_kmers[i-1].second + params->sampling_intv)) {
		int delta_contig = matched_kmers[i].first - matched_kmers[i-1].first;		
		int delta_read = matched_kmers[i].second - matched_kmers[i-1].second;
		int last_pos = fragments[idx].first + fragment_weights[idx];
		if(delta_contig == delta_read && matched_kmers[i].first < (last_pos + 1)) {
			fragment_weights[idx] += matched_kmers[i].first + params->k2 - last_pos;
		} else {
			int r = 0;
			if(matched_kmers[i].second > matched_kmers[i-1].second) { // indels
				int d1 = last_pos - matched_kmers[i].first;
				int d2 = fragments[idx].second + fragment_weights[idx] - matched_kmers[i].second;
				if(d1 > 0) r = d1;
				if(d2 > 0) r = d1 > d2 ? d1 : d2;	
			}
			fragments.push_back(std::make_pair(matched_kmers[i].first + r, matched_kmers[i].second + r));
			fragment_weights.push_back(params->k2 - r);
			idx++;
		}
	}

	//for (int i = 0; i < fragments.size(); i++) {
	//	std::cout << fragments[i].first << " " << fragments[i].second << " (Weight=" << fragment_weights[i] << "),";
        //}
	//std::cout << "\n";

	std::vector<int> best_chain;
	chain_fragments_dp(fragments, fragment_weights, best_chain);

	std::vector<int> votes(ref_contig.len + rlen);	
	for(int i = 0; i < best_chain.size(); i++) {      
        	int match_aln_pos = fragments[best_chain[i]].first - fragments[best_chain[i]].second;
        //for (int i = 0; i < fragments.size(); i++) {
	//	int match_aln_pos = fragments[i].first - fragments[i].second;
		votes[pos0 + match_aln_pos] += fragment_weights[i] - params->k2 +1;
        //      std::cout << "CHAIN MATCH " << match_aln_pos << " " << fragments[i].first << " " << fragments[i].second << "\n";

		votes[pos0 + match_aln_pos] += fragment_weights[best_chain[i]] - params->k2 +1;
       	//	std::cout << "CHAIN MATCH " << match_aln_pos << " " << fragments[best_chain[i]].first << " " << fragments[best_chain[i]].second << "\n";
	}


	//std::vector<int> votes(ref_contig.len + rlen);
	//for(int i = 1; i < seqan::length(global_chain)-1; i++) {	
	//	int match_aln_pos = seqan::leftPosition(global_chain[i], 0) - seqan::leftPosition(global_chain[i], 1);
	//	votes[pos0 + match_aln_pos] += 20*(seqan::rightPosition(global_chain[i], 0) - seqan::leftPosition(global_chain[i], 0));
	//}

//*#if(CHAIN_VOTES) 

	std::vector<int> votes_prefsum(ref_contig.len + rlen);
        votes_prefsum[0] = votes[0];
        for(int i = 1; i < votes.size(); i++) {
                votes_prefsum[i] = votes[i] + votes_prefsum[i-1];
		//std::cout << votes[i] << " ";
        }
	//std::cout << "\n";

        int max = 0;
        uint32 max_pos = 0;
        for(int i = 0; i < votes.size(); i++) {
                uint32 start = i > params->delta_inlier ? i - params->delta_inlier - 1 : 0;
                uint32 end = i + params->delta_inlier >=  votes.size() ? votes.size() - 1: i + params->delta_inlier;
                int votes_conv = votes_prefsum[end] - votes_prefsum[start];
                if(votes_conv > max) {
                        max = votes_conv;
                        max_pos = i;
                }
        }

	 // pick the middle position in the max range
        int i = max_pos;
        while(i<votes.size()) {
                uint32 start = i > params->delta_inlier ? i - params->delta_inlier - 1 : 0;
                uint32 end = i + params->delta_inlier >=  votes.size() ? votes.size() - 1: i + params->delta_inlier;
                int votes_conv = votes_prefsum[end] - votes_prefsum[start];
                if(votes_conv == max) {
                        i++;
                } else {
                        break;
                }
        }
        max_pos = (i + max_pos)/2;

        n_votes[0] = max;
        pos[0] = max_pos - pos0;

        /*int second_best = 0;
        for(int i = 0; i < votes.size(); i++) {
                if(i == max_pos) continue;
                uint32 start = i > params->delta_inlier ? i - params->delta_inlier - 1 : 0;
                uint32 end = i + params->delta_inlier >=  votes.size() ? votes.size() - 1: i + params->delta_inlier;
                int votes_conv = votes_prefsum[end] - votes_prefsum[start];
                if(votes_conv > second_best && !pos_in_range(max_pos, i, params->delta_x*params->delta_inlier)) {
                        second_best = votes_conv;
                        n_votes[1] = second_best;
                        pos[1] = i - pos0;
                }
        }*/

//#endif*/
}

void vote_cast_and_count(const ref_match_t ref_contig, const seq_t rlen,
                std::vector<std::pair<kmer_cipher_t, pos_cipher_t>>& read_ciphers,
                std::vector<std::pair<kmer_cipher_t, pos_cipher_t>>& contig_ciphers,
                const index_params_t* params, int* n_votes, int* pos) {

	int approx_pos_range = params->k2;
        std::vector<int> votes(ref_contig.len + rlen);
        int pos0 = rlen;
        uint32 skip = 0;
        bool any_matches = false;
	for(int i = 0; i < read_ciphers.size(); i++) {
                if(read_ciphers[i].first == 0) continue;
                if(!is_unique_kmer(read_ciphers, i)) continue;
                for(uint32 j = skip; j < contig_ciphers.size(); j++) {
                        if(!is_unique_kmer(contig_ciphers, j)) continue;
                        if(read_ciphers[i].first == contig_ciphers[j].first) {
                                any_matches = true;
				
				int pos_read = read_ciphers[i].second;
				int read_range_start = (pos_read/approx_pos_range)*approx_pos_range;
				int read_range_end = read_range_start + approx_pos_range;
				if(read_range_end >= params->sampling_intv*read_ciphers.size()) read_range_end = params->sampling_intv*read_ciphers.size();

				int pos_contig = contig_ciphers[j].second;				
				int contig_range_start = (pos_contig/approx_pos_range)*approx_pos_range;
				int contig_range_end = contig_range_start + approx_pos_range;
				if(contig_range_end >= params->sampling_intv*contig_ciphers.size()) contig_range_end = params->sampling_intv*contig_ciphers.size();				

	//			std::cout << " true pos " << contig_ciphers[j].second << " " << read_ciphers[i].second << " match pos = " << contig_ciphers[j].second - read_ciphers[i].second << " c_s/c_e/_s/r_e " << contig_range_start << "/" << contig_range_end << "/" << read_range_start << "/" << read_range_end << "\n"; 
				int s = contig_range_start - read_range_end;
				int t = contig_range_end - read_range_start;

				for(int k = s; k < t; k++) {
					votes[pos0 + k]++;
					//for(int m = contig_range_start; m < contig_range_end; m++) {
						//int match_aln_pos = m - k;
						//std::cout << match_aln_pos << "\n"; 
						//votes[pos0 + match_aln_pos]++;
					//}
				}
				

				//int match_aln_pos = contig_ciphers[j].second - read_ciphers[i].second;
				//votes[pos0 + match_aln_pos]++;
				//skip = j + 1; break;
                        } else if(contig_ciphers[j].first > read_ciphers[i].first) {
                                skip = j;
                                break;
                        }
                }

        }

	if(!any_matches) return;

	//std::cout << "VOTES: \n";
        std::vector<int> votes_prefsum(ref_contig.len + rlen);
        votes_prefsum[0] = votes[0];
	for(int i = 1; i < votes.size(); i++) {
		//std::cout << i << ":" << votes[i] << " ";
		votes_prefsum[i] = votes[i] + votes_prefsum[i-1];
	}
	//std::cout << "\n";

	int max = 0;
        uint32 max_pos = 0;
        for(int i = 0; i < votes.size(); i++) {
                uint32 start = i > params->delta_inlier ? i - params->delta_inlier - 1 : 0;
                uint32 end = i + params->delta_inlier >=  votes.size() ? votes.size() - 1: i + params->delta_inlier;
                int votes_conv = votes_prefsum[end] - votes_prefsum[start];
                if(votes_conv > max) {
			max = votes_conv;
                       	max_pos = i;
                }
        }

	// pick the middle position in the max range
	int i = max_pos; 
	while(i < votes.size()) {
		uint32 start = i > params->delta_inlier ? i - params->delta_inlier - 1 : 0;
                uint32 end = i + params->delta_inlier >=  votes.size() ? votes.size() - 1: i + params->delta_inlier;
		int votes_conv = votes_prefsum[end] - votes_prefsum[start];
		if(votes_conv == max) {
			i++;
		} else {
			break;
		}
	}
	max_pos = (i + max_pos)/2;

        n_votes[0] = max;
	pos[0] = max_pos - pos0;
	
	int second_best = 0;
	for(int i = 0; i < votes.size(); i++) {
                if(i == max_pos) continue;
		uint32 start = i > params->delta_inlier ? i - params->delta_inlier - 1 : 0;
                uint32 end = i + params->delta_inlier >=  votes.size() ? votes.size() - 1: i + params->delta_inlier;
                int votes_conv = votes_prefsum[end] - votes_prefsum[start];
                if(votes_conv > second_best && !pos_in_range(max_pos, i, params->delta_x*params->delta_inlier)) {
                        second_best = votes_conv;
                        n_votes[1] = second_best;
			pos[1] = i - pos0;
                }
        }
}


void store_precomp_contigs(const char* fileName, reads_t& reads) {
	std::string fname(fileName);
	std::ofstream file;
	file.open(fname.c_str(), std::ios::out | std::ios::binary);
	if (!file.is_open()) {
		printf("store_or_load_contigs: Cannot open file %s!\n", fname.c_str());
		exit(1);
	}
	for(uint32 i = 0; i < reads.reads.size(); i++) {
		read_t* r = &reads.reads[i];
		if(!r->valid_minhash_f && !r->valid_minhash_rc) continue;
		uint32 ref_size = r->ref_matches.size();
		file.write(reinterpret_cast<char*>(&ref_size), sizeof(r->ref_matches.size()));
		file.write(reinterpret_cast<char*>(&(r->ref_matches[0])), r->ref_matches.size()*sizeof(ref_match_t));
	}
}
void load_precomp_contigs(const char* fileName, reads_t& reads) {
	std::string fname(fileName);
	std::ifstream file;
	file.open(fname.c_str(), std::ios::in | std::ios::binary);
	if (!file.is_open()) {
		printf("store_or_load_contigs: Cannot open file %s!\n", fname.c_str());
		exit(1);
	}
	for(uint32 i = 0; i < reads.reads.size(); i++) {
		read_t* r = &reads.reads[i];
		if(!r->valid_minhash_f && !r->valid_minhash_rc) continue;
		uint32 ref_size;
		file.read(reinterpret_cast<char*>(&ref_size), sizeof(r->ref_matches.size()));
		r->ref_matches.resize(ref_size);
		file.read(reinterpret_cast<char*>(&(r->ref_matches[0])), r->ref_matches.size()*sizeof(ref_match_t));
		r->n_proc_contigs = ref_size;
		for(uint32 j = 0; j < r->ref_matches.size(); j++) {
			ref_match_t ref_contig = r->ref_matches[j];
			if(ref_contig.n_diff_bucket_hits > r->best_n_bucket_hits) {
				r->best_n_bucket_hits = ref_contig.n_diff_bucket_hits;
			}
		}
	}
}

#define MAX_BUCKET_SIZE 1000
//#define KMER_MASK_LEN 20
//static const uint8 kmer_mask[KMER_MASK_LEN] = {1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1};
static const int VERBOSE = (getenv("VERBOSE") ? atoi(getenv("VERBOSE")) : 0);

//////////// PRIVACY-PRESERVING READ ALIGNMENT ////////////

void phase1_minhash(const ref_t& ref, reads_t& reads, const index_params_t* params);
void phase1_merge(reads_t& reads, const ref_t& ref, const index_params_t* params);
void phase2_encryption(reads_t& reads, const ref_t& ref, const index_params_t* params);
void phase2_voting(reads_t& reads, const ref_t& ref, const index_params_t* params, int* avg_score);
void phase2_monolith(reads_t& reads, const ref_t& ref, const index_params_t* params, int* avg_score);
void finalize(reads_t& reads, const uint32 avg_score, const ref_t& ref, const index_params_t* params);
void eval(reads_t& reads, const ref_t& ref, const index_params_t* params);

void balaur_main(const char* fastaName, ref_t& ref, reads_t& reads, const index_params_t* params) {
	get_sim_read_info(ref, reads);

	// --- phase 1 ---
	double start_time = omp_get_wtime();
	phase1_minhash(ref, reads, params);
	if(params->load_mhi) {
		ref.index.release();
		if(params->precomp_contig_file_name.size() != 0) {
			store_precomp_contigs(params->precomp_contig_file_name.c_str(), reads);
		}
	} else {
		load_precomp_contigs(params->precomp_contig_file_name.c_str(), reads);
	}

	// --- phase 2 ---
	int avg_score;
	load_kmer2_hashes(fastaName, ref, params);
	load_repeat_info(fastaName, ref, params);
	//load_repeat_local(fastaName, ref, params);
	if(!params->monolith) {
		phase2_encryption(reads, ref, params);
		phase2_voting(reads, ref, params, &avg_score);
	} else {
		phase2_monolith(reads, ref, params, &avg_score);
	}
	finalize(reads, avg_score, ref, params);
	eval(reads, ref, params);
	printf("****TOTAL ALIGNMENT TIME****: %.2f sec\n", omp_get_wtime() - start_time);	
}


bool minhash_opt(const char* seq, const seq_t seq_len,
                        const VectorBool& ref_freq_kmer_bitmap,
                        const index_params_t* params,
                        VectorMinHash& min_hashes) {

        minhash_t v[seq_len - params->k + 1]  __attribute__((aligned(16)));;
        uint32 n_valid_kmers = 0;
        for(uint32 i = 0; i < seq_len - params->k + 1; i++) {
                uint32_t packed_kmer;
                if((pack_32(&seq[i], params->k, &packed_kmer) < 0) || ref_freq_kmer_bitmap[packed_kmer]) {
                        continue;
                }
                v[n_valid_kmers] = CityHash32(&seq[i], params->k); //packed_kmer;
                n_valid_kmers++;
        }
        if(n_valid_kmers <= 2*params->k) {
                return false;
        }

        __m128i* vs = (__m128i*)v;
        __m128i scalar, p1, curr_min_vec;
        for(uint32_t h = 0; h < params->h; h++) { // update the min values
                const minhash_t s = params->minhash_functions[h].a;
                scalar = _mm_set1_epi32(s);
                curr_min_vec = _mm_mullo_epi32(vs[0], scalar);
                for(uint32 i = 1; i < n_valid_kmers/4; i++) {
                        p1 = _mm_mullo_epi32(vs[i], scalar);
                        curr_min_vec = _mm_min_epu32(p1, curr_min_vec);
                }
                minhash_t result[4] __attribute__((aligned(16)));
                _mm_store_si128((__m128i*)result, curr_min_vec);
                min_hashes[h] = result[0];
                for(int i = 1; i < 4; i++) {
                        if(result[i] < min_hashes[h]) {
                                min_hashes[h] = result[i];
                        }
                }

                for(int i = 4*(n_valid_kmers/4); i < n_valid_kmers; i++) {
                        const minhash_t p = s*v[i];
                        if(p < min_hashes[h]) {
                                 min_hashes[h] = p;
                        }
                }
        }

	/*std::vector<minhash_t> min_hashes2(n_valid_kmers);
        for(uint32_t h = 0; h < params->h; h++) { // update the min values
                const minhash_t s = params->minhash_functions[h].a;
                min_hashes2[h] = s*v[0];
                for(uint32 i = 1; i < n_valid_kmers; i++) {
                        const minhash_t p = s*v[i];
                        if(p < min_hashes2[h]) {
                                min_hashes2[h] = p;
                        }
                }
        }*/

        return true;
}

void phase1_minhash(const ref_t& ref, reads_t& reads, const index_params_t* params) {
	printf("////////////// Phase 1: MinHash //////////////\n");
	omp_set_num_threads(params->n_threads);
	double start_time = omp_get_wtime();
	///// ---- fingerprints ----
	//#pragma omp parallel for
	for(uint32 i = 0; i < reads.reads.size(); i++) {
		read_t* r = &reads.reads[i];
		r->minhashes_f.resize(params->h);
		r->minhashes_rc.resize(params->h);
		r->valid_minhash_f = minhash_opt(r->seq.c_str(), r->len, ref.high_freq_kmer_bitmap, params, r->minhashes_f);
		r->valid_minhash_rc = minhash_opt(r->rc.c_str(), r->len, ref.high_freq_kmer_bitmap, params, r->minhashes_rc);
	}
	printf("Runtime (fingerprints): %.2f sec\n", omp_get_wtime() - start_time);

	if(!params->load_mhi) return;

	///// ---- project and merge ----
	//#pragma omp parallel for
	for(uint32 i = 0; i < reads.reads.size(); i++) {
		read_t* r = &reads.reads[i];
		if(r->valid_minhash_f) {
			r->ref_bucket_matches_by_table_f.resize(params->n_tables);
			for(uint32 t = 0; t < params->n_tables; t++) {
				const minhash_t proj_hash = params->sketch_proj_hash_func.apply_vector(r->minhashes_f, params->sketch_proj_indices, t*params->sketch_proj_len);
				const uint64_t bid = t*params->n_buckets + params->sketch_proj_hash_func.bucket_hash(proj_hash);
				const uint32 bucket_size = ref.index.bucket_offsets[bid + 1] - ref.index.bucket_offsets[bid];
				if(bucket_size > MAX_BUCKET_SIZE) {
					r->ref_bucket_matches_by_table_f[t] = std::pair<uint64, minhash_t>(ref.index.bucket_offsets.size(), 0);
					continue;
				}
				r->ref_bucket_matches_by_table_f[t] = std::pair<uint64, minhash_t>(bid, proj_hash);
				//_mm_prefetch((const void *)&ref.index.buckets_data[ref.index.bucket_offsets[bid]],_MM_HINT_T0);
			}
			collect_read_hits(ref, r, false, params);
		}
		if(r->valid_minhash_rc) {
			r->ref_bucket_matches_by_table_rc.resize(params->n_tables);
			for(uint32 t = 0; t < params->n_tables; t++) {
				const minhash_t proj_hash = params->sketch_proj_hash_func.apply_vector(r->minhashes_rc, params->sketch_proj_indices, t*params->sketch_proj_len);
				const uint64_t bid = t*params->n_buckets + params->sketch_proj_hash_func.bucket_hash(proj_hash);
				uint32 bucket_size = ref.index.bucket_offsets[bid + 1] - ref.index.bucket_offsets[bid];
				if(bucket_size > MAX_BUCKET_SIZE) {
					r->ref_bucket_matches_by_table_rc[t] = std::pair<uint64, minhash_t>(ref.index.bucket_offsets.size(), 0);
					continue;
				}
				r->any_bucket_hits = true;
				r->ref_bucket_matches_by_table_rc[t] = std::pair<uint64, minhash_t>(bid, proj_hash);
				//_mm_prefetch((const char *)&ref.index.buckets_data[ref.index.bucket_offsets[bid]],_MM_HINT_T0);
			}
			collect_read_hits(ref, r, true, params);
		}
	}
	printf("Runtime time (total): %.2f sec\n", omp_get_wtime() - start_time);
}

void phase2_encryption(reads_t& reads, const ref_t& ref, const index_params_t* params) {
	printf("////////////// Phase 2: Contig Encryption //////////////\n");
	omp_set_num_threads(params->n_threads);

	int d_thr = 10000;
	//if(params->ref_window_size > 150) d_thr = 500;
	//if(params->ref_window_size > 1000) d_thr = 20;

	// allocate temp storage for the seeds, initiate keys
	#pragma omp parallel for
	for(uint32 i = 0; i < reads.reads.size(); i++) {
		read_t* r = &reads.reads[i];
		if(!r->valid_minhash_f && !r->valid_minhash_rc) continue;
		int n_proc_contigs = 0;
		r->contig_kmer_ciphers.resize(r->ref_matches.size());
		for(uint32 j = 0; j < r->ref_matches.size(); j++) {
			ref_match_t ref_contig = r->ref_matches[j];
			if(r->n_proc_contigs > d_thr && ref_contig.n_diff_bucket_hits < 2) continue;
			if(ref_contig.n_diff_bucket_hits < (int) (r->best_n_bucket_hits - params->dist_best_hit)) continue;
			//if(ref_contig.n_diff_bucket_hits < params->min_n_hits) continue;
			r->contig_kmer_ciphers[j] = new kmer_cipher_t[r->ref_matches[j].len - params->k2 + 1];//.resize(r->ref_matches[j].len - params->k2 + 1);
			n_proc_contigs++;
		}
		r->n_proc_contigs = n_proc_contigs;
	}

	// generate the read and contig kmer hashes
	double start_time = omp_get_wtime();
	#pragma omp parallel for
        for(uint32 i = 0; i < reads.reads.size(); i++) {
                read_t* r = &reads.reads[i];
                if(!r->valid_minhash_f && !r->valid_minhash_rc) continue;
                r->kmers_f = new kmer_cipher_t[r->len - params->k2 + 1];
                r->kmers_rc = new kmer_cipher_t[r->len - params->k2 + 1];
                for(uint32 j = 0; j < r->ref_matches.size(); j++) {
                        if(r->contig_kmer_ciphers[j] == NULL) continue;
			ref_match_t ref_contig = r->ref_matches[j];
                        if(r->ref_strand != 3) {
                                if(ref_contig.rc) r->ref_strand |= 2;
                                else r->ref_strand |= 1;
                        }
                }
                r->key1_xor_pad = genrand64_int64();
                r->key2_mult_pad = genrand64_int64();
        }

	double t2 = omp_get_wtime();
	#pragma omp parallel for
	for(uint32 i = 0; i < reads.reads.size(); i++) {
		read_t* r = &reads.reads[i];
		if(!r->valid_minhash_f && !r->valid_minhash_rc) continue;
		if(r->ref_strand & 1) generate_voting_kmer_ciphers_read(r->kmers_f, r->seq.c_str(), r->len, r->key1_xor_pad, r->key2_mult_pad, ref, params);
		if(r->ref_strand & 2) generate_voting_kmer_ciphers_read(r->kmers_rc, r->rc.c_str(), r->len, r->key1_xor_pad, r->key2_mult_pad, ref, params);

		for(uint32 j = 0; j < r->ref_matches.size(); j++) {
			if(r->contig_kmer_ciphers[j] == NULL) continue;			
			generate_voting_kmer_ciphers_ref(r->contig_kmer_ciphers[j], ref.seq.c_str(), r->ref_matches[j].pos, r->ref_matches[j].len, r->key1_xor_pad, r->key2_mult_pad, ref, params);
		}
	}
	printf("Encryption time: %.2f sec\n", omp_get_wtime() - t2);
	printf("Total client prep time: %.2f sec\n", omp_get_wtime() - start_time);
	
	// ---- determine the total communication size ----
    	uint64 total_size = 0;
	uint64 total_count = 0;
    	uint64 total_contigs = 0;
	for(uint32 i = 0; i < reads.reads.size(); i++) {
		read_t* r = &reads.reads[i];
		if(r->ref_strand & 1) total_size += (r->len - params->k2 + 1)*sizeof(kmer_cipher_t);
		if(r->ref_strand & 2) total_size += (r->len - params->k2 + 1)*sizeof(kmer_cipher_t);
		for(uint32 j = 0; j < r->ref_matches.size(); j++) {
			if(r->contig_kmer_ciphers[j] == NULL) continue;
			int n_kmers = r->ref_matches[j].len - params->k2 + 1;
			int n_sampled_kmers = (n_kmers-1)/params->sampling_intv + 1;
			total_size += n_sampled_kmers*sizeof(kmer_cipher_t);	
			total_count += n_sampled_kmers;
		}
		total_contigs += r->n_proc_contigs;
    	}
	printf("Total contigs: %llu \n", total_contigs);
	printf("Total count: %llu contig kmers \n", total_count);
	printf("Total size: %.2f MB\n", ((float) total_size)/1024/1024);
}

void phase2_voting(reads_t& reads, const ref_t& ref, const index_params_t* params, int* avg_score) {
	printf("////////////// Phase 2: Voting //////////////\n");
	double start_time = omp_get_wtime();
	omp_set_num_threads(params->n_threads);
	int sum_score = 0;
	int n_nonzero_scores = 0;
	#pragma omp parallel for reduction(+:sum_score, n_nonzero_scores)
	for(uint32 i = 0; i < reads.reads.size(); i++) {
		read_t* r = &reads.reads[i];
		if(!r->valid_minhash_f && !r->valid_minhash_rc) continue;

		std::vector<std::pair<kmer_cipher_t, uint16_t>> read_kmers_f;
		std::vector<std::pair<kmer_cipher_t, uint16_t>> read_kmers_rc;
		if(r->ref_strand & 1) read_kmers_f.resize(r->len - params->k2 + 1);
		if(r->ref_strand & 2) read_kmers_rc.resize(r->len - params->k2 + 1);
		
		for(int c = 0; c < r->len - params->k2 + 1; c++) {
			 if(r->ref_strand & 1) read_kmers_f[c] = std::make_pair(r->kmers_f[c], c);
			 if(r->ref_strand & 2) read_kmers_rc[c] = std::make_pair(r->kmers_rc[c], c);
		}	
		if(r->ref_strand & 1) std::sort(read_kmers_f.begin(), read_kmers_f.end());
		if(r->ref_strand & 2) std::sort(read_kmers_rc.begin(), read_kmers_rc.end());

		for(uint32 j = 0; j < r->ref_matches.size(); j++) {
			ref_match_t ref_contig = r->ref_matches[j];
			if(r->contig_kmer_ciphers[j] == NULL) continue;
			std::vector<std::pair<kmer_cipher_t, uint16_t>>& read_kmer_ciphers = (ref_contig.rc) ? read_kmers_rc : read_kmers_f;
			int n_kmers = ref_contig.len - params->k2 + 1;
			int n_sampled_kmers = (n_kmers-1)/params->sampling_intv + 1;
			std::vector<std::pair<kmer_cipher_t, uint16_t>> contig_kmer_ciphers(n_sampled_kmers);
			for(int c = 0; c < n_sampled_kmers; c++) {
                        	contig_kmer_ciphers[c] = std::make_pair((r->contig_kmer_ciphers[j])[c], params->sampling_intv*c);
                	}
			std::sort(contig_kmer_ciphers.begin(), contig_kmer_ciphers.end());

			int pos_sig[2] = { 0 };
			seq_t pos[2] = { 0 };
			int n_votes[2] = { 0 };
			vote_cast_and_count(ref_contig, r->len, read_kmer_ciphers, contig_kmer_ciphers, params, n_votes, pos_sig);

			for(int i = 0; i < 2; i++) {
				if(n_votes[i] == 0) continue;
				pos[i] = pos_sig[i] + ref_contig.pos;
			
				/*if(pos_in_range_asym(r->ref_pos_r, pos[i], ref_contig.len + params->ref_window_size, params->ref_window_size) ||
                                        pos_in_range_asym(r->ref_pos_l, pos[i], ref_contig.len + params->ref_window_size, params->ref_window_size)) {
					
					std::cout << "chain " << n_votes[i] << " " << pos[i] << " sig " << pos_sig[i] << " contig " << ref_contig.pos <<"\n";
					print_read(r);

				}*/
			}

			/*n_sampled_kmers = (n_kmers-1)/3 + 1;
                        std::vector<std::pair<kmer_cipher_t, uint16_t>> contig_kmer_ciphers_tmp(n_sampled_kmers);
                        for(int c = 0; c < n_sampled_kmers; c++) {
                                contig_kmer_ciphers_tmp[c] = std::make_pair((r->contig_kmer_ciphers[j])[3*c], 3*c);
                        }
                        std::sort(contig_kmer_ciphers_tmp.begin(), contig_kmer_ciphers_tmp.end());
			
			int pos_sig_tmp[2] = { 0 };
                        seq_t pos_tmp[2] = { 0 };
                        int n_votes_tmp[2] = { 0 };
			std::cout << "SAMPLED \n";
			vote_cast_and_count(ref_contig, r->len, read_kmer_ciphers, contig_kmer_ciphers, params, n_votes_tmp, pos_sig_tmp);
		
			for(int i = 0; i < 2; i++) {
                                if(n_votes_tmp[i] == 0) continue;
                                pos_tmp[i] = pos_sig_tmp[i] + ref_contig.pos;

				if(pos_tmp[i] != pos[i] || n_votes_tmp[i] != n_votes[i]) {
					std::cout << "chain " << n_votes[i] << " " << pos[i] << " orig " << n_votes_tmp[i] << " " << pos_tmp[i] << "\n";
					//print_read(r);
					std::cout << "DIFF\n";
				 	if(pos_in_range_asym(r->ref_pos_r, pos[i], ref_contig.len + params->ref_window_size, params->ref_window_size) ||
                                        	pos_in_range_asym(r->ref_pos_l, pos[i], ref_contig.len + params->ref_window_size, params->ref_window_size)) {
                                		std::cout << "TRUE!!!!!!\n";
						
						if((pos_in_range(r->ref_pos_l, pos_tmp[i], 10) || pos_in_range(r->ref_pos_r, pos_tmp[i], 10)) &&
							(!pos_in_range(r->ref_pos_l, pos[i], 10) && !pos_in_range(r->ref_pos_r, pos[i], 10))) {
						//if(n_votes_tmp[i] > n_votes[i]) {
							std::cout << "TRUE!!!!!! BETTER XXXX\n";
							print_read(r);
							int z = r->ref_pos_l;
							while(true) {
								if(z > r->ref_pos_r) break;
								printf("%c", iupacChar[(int)ref.seq[z]]);
								z++;
							}
							printf("\n");
						}
					}
					//r->comp_votes_hit = n_votes[0] > n_votes[1] ? n_votes[0] : n_votes[1];
                        	}
                        }
			*/

			// update votes and its alignment position
			for(int i = 0; i < 2; i++) {
				if(n_votes[i] > r->top_aln.inlier_votes) {
					if(!pos_in_range(pos[i], r->top_aln.ref_start, 30)) {
						r->second_best_aln.inlier_votes = r->top_aln.inlier_votes;
						r->second_best_aln.total_votes = r->top_aln.total_votes;
						r->second_best_aln.ref_start = r->top_aln.ref_start;
					}
					// update best alignment
					r->top_aln.inlier_votes = n_votes[i];
					r->top_aln.ref_start = pos[i];
					r->top_aln.rc = ref_contig.rc;
				} else if(n_votes[i] > r->second_best_aln.inlier_votes) {
					if(!pos_in_range(pos[i], r->top_aln.ref_start, 30)) {
						r->second_best_aln.inlier_votes = n_votes[i];
						r->second_best_aln.ref_start = pos[i];
					}
				}
			}

#if(SIM_EVAL)
			if(pos_in_range_asym(r->ref_pos_r, ref_contig.pos, ref_contig.len + params->ref_window_size, params->ref_window_size) ||
					pos_in_range_asym(r->ref_pos_l, ref_contig.pos, ref_contig.len + params->ref_window_size, params->ref_window_size)) {
				r->comp_votes_hit = n_votes[0] > n_votes[1] ? n_votes[0] : n_votes[1];
			}
#endif

		}
		if(r->top_aln.inlier_votes > 0) {
			sum_score += r->top_aln.inlier_votes;
			n_nonzero_scores++;
		}
	}

	if(n_nonzero_scores > 0) {
		*avg_score = sum_score/n_nonzero_scores;
	}
	printf("Total time: %.2f sec\n", omp_get_wtime() - start_time);

	// ---- determine the total communication size ----
	uint64 total_size = 0;
	for(uint32 i = 0; i < reads.reads.size(); i++) {
        read_t* r = &reads.reads[i];
		if(!r->valid_minhash_f && !r->valid_minhash_rc) continue;
		total_size += 2*sizeof(r->top_aln.inlier_votes);
		total_size += sizeof(r->top_aln.rc);
		total_size += sizeof(r->top_aln.ref_start);
		total_size += sizeof(int);
	} 
	printf("Total size: %.2f MB\n", ((float) total_size)/1024/1024);
}

void phase2_monolith(reads_t& reads, const ref_t& ref, const index_params_t* params, int* avg_score) {
	printf("////////////// Phase 2: MONOLITH //////////////\n");
	int d_thr = 800;
        if(params->ref_window_size > 150) d_thr = 500;
        //if(params->ref_window_size > 1000) d_thr = 20;

        omp_set_num_threads(params->n_threads);
        int sum_score = 0;
        int n_nonzero_scores = 0;
        double start_time = omp_get_wtime();
        #pragma omp parallel for reduction(+:sum_score, n_nonzero_scores)
        for(uint32 i = 0; i < reads.reads.size(); i++) {
                read_t* r = &reads.reads[i];
                if(!r->valid_minhash_f && !r->valid_minhash_rc) continue;
                r->key1_xor_pad = genrand64_int64();
                r->key2_mult_pad = genrand64_int64();
                std::vector<std::pair<kmer_cipher_t, uint16_t>> read_kmers_f;
                std::vector<std::pair<kmer_cipher_t, uint16_t>> read_kmers_rc;

                for(uint32 j = 0; j < r->ref_matches.size(); j++) {
                        if(r->n_proc_contigs > d_thr && r->ref_matches[j].n_diff_bucket_hits < 2) continue;
                        if(r->ref_matches[j].n_diff_bucket_hits < (int) (r->best_n_bucket_hits - params->dist_best_hit)) continue;
                        if(r->ref_matches[j].n_diff_bucket_hits < params->min_n_hits) continue;

                        const int n_kmers = r->ref_matches[j].len - params->k2 + 1;
                        kmer_cipher_t* contig_ciphers = new kmer_cipher_t[n_kmers];
                        generate_voting_kmer_ciphers_ref(contig_ciphers, ref.seq.c_str(), r->ref_matches[j].pos, r->ref_matches[j].len, r->key1_xor_pad, r->key2_mult_pad, ref, params);
                        std::vector<std::pair<kmer_cipher_t, uint16_t>> contig_kmer_ciphers(n_kmers);
                        for(int c = 0; c < n_kmers; c++) {
                                contig_kmer_ciphers[c] = std::make_pair(contig_ciphers[c], c);
                        }
                        delete(contig_ciphers);
                        std::sort(contig_kmer_ciphers.begin(), contig_kmer_ciphers.end());

                        if((!r->ref_matches[j].rc) && (!(r->ref_strand & 1))) {
                                r->ref_strand |= 1;
                                r->kmers_f = new kmer_cipher_t[r->len - params->k2 + 1];
                                generate_voting_kmer_ciphers_read(r->kmers_f, r->seq.c_str(), r->len, r->key1_xor_pad, r->key2_mult_pad, ref, params);
                                read_kmers_f.resize(r->len - params->k2 + 1);
                                for(int c = 0; c < r->len - params->k2 + 1; c++) {
                                        read_kmers_f[c] = std::make_pair(r->kmers_f[c], c);
                                }
                                std::sort(read_kmers_f.begin(), read_kmers_f.end());

                        } else if((r->ref_matches[j].rc) && (!(r->ref_strand & 2))) {
                                r->ref_strand |= 2;
                                r->kmers_rc = new kmer_cipher_t[r->len - params->k2 + 1];
                                generate_voting_kmer_ciphers_read(r->kmers_rc, r->rc.c_str(), r->len, r->key1_xor_pad, r->key2_mult_pad, ref, params);
                                read_kmers_rc.resize(r->len - params->k2 + 1);
                                for(int c = 0; c < r->len - params->k2 + 1; c++) {
                                        read_kmers_rc[c] = std::make_pair(r->kmers_rc[c], c);
                                }
                                std::sort(read_kmers_rc.begin(), read_kmers_rc.end());
                        }

                        std::vector<std::pair<kmer_cipher_t, uint16_t>>& read_kmer_ciphers = (r->ref_matches[j].rc) ? read_kmers_rc : read_kmers_f;
                        int pos_sig[2] = { 0 };
                        seq_t pos[2] = { 0 };
                        int n_votes[2] = { 0 };
                        vote_cast_and_count(r->ref_matches[j], r->len, read_kmer_ciphers, contig_kmer_ciphers, params, n_votes, pos_sig);

                        for(int i = 0; i < 2; i++) {
                                if(n_votes[i] == 0) continue;
                                pos[i] = pos_sig[i] + r->ref_matches[j].pos;
                        }
                        // update votes and its alignment position
                        for(int i = 0; i < 2; i++) {
                                if(n_votes[i] > r->top_aln.inlier_votes) {
                                        if(!pos_in_range(pos[i], r->top_aln.ref_start, 30)) {
                                                r->second_best_aln.inlier_votes = r->top_aln.inlier_votes;
                                                r->second_best_aln.total_votes = r->top_aln.total_votes;
                                                r->second_best_aln.ref_start = r->top_aln.ref_start;
                                        }
                                        // update best alignment
					 r->top_aln.inlier_votes = n_votes[i];
                                        r->top_aln.ref_start = pos[i];
                                        r->top_aln.rc = r->ref_matches[j].rc;
                                } else if(n_votes[i] > r->second_best_aln.inlier_votes) {
                                        if(!pos_in_range(pos[i], r->top_aln.ref_start, 30)) {
                                                r->second_best_aln.inlier_votes = n_votes[i];
                                                r->second_best_aln.ref_start = pos[i];
                                        }
                                }
                        }

#if(SIM_EVAL)
                        if(pos_in_range_asym(r->ref_pos_r, r->ref_matches[j].pos, r->ref_matches[j].len + params->ref_window_size, params->ref_window_size) ||
                                        pos_in_range_asym(r->ref_pos_l, r->ref_matches[j].pos, r->ref_matches[j].len + params->ref_window_size, params->ref_window_size)) {
                                r->comp_votes_hit = n_votes[0] > n_votes[1] ? n_votes[0] : n_votes[1];
                        }
#endif
                }

                if(r->ref_strand & 1) {
                        delete(r->kmers_f);
                }
                if(r->ref_strand & 2) {
                        delete(r->kmers_rc);
                }

                if(r->top_aln.inlier_votes > 0) {
                        sum_score += r->top_aln.inlier_votes;
                        n_nonzero_scores++;
                }
        }
        if(n_nonzero_scores > 0) {
                *avg_score = sum_score/n_nonzero_scores;
        }
        printf("Total time: %.2f sec\n", omp_get_wtime() - start_time);
}

void finalize(reads_t& reads, const uint32 avg_score, const ref_t& ref, const index_params_t* params) {
	printf("////////////// Finalize Mappings //////////////\n");
	double start_time = omp_get_wtime();
	omp_set_num_threads(params->n_threads);
	// ---- mapq score -----
	#pragma omp parallel for
	for(uint32 i = 0; i < reads.reads.size(); i++) {
		read_t* r = &reads.reads[i];
		r->top_aln.score = 0;
		// top > 0 and top != second best
		if(r->top_aln.inlier_votes > r->second_best_aln.inlier_votes) {
			// if sufficient votes were accumulated (lower thresholds for unique hit)
			if(r->top_aln.inlier_votes > params->votes_cutoff) {
				if(r->second_best_aln.inlier_votes < 0) r->second_best_aln.inlier_votes = 0;

				r->top_aln.score = params->mapq_scale_x*(r->top_aln.inlier_votes - r->second_best_aln.inlier_votes)/r->top_aln.inlier_votes;
				// scale by the distance from theoretical best possible votes
				if(avg_score > 0 && params->enable_scale) {
					r->top_aln.score *= (float) r->top_aln.inlier_votes/(float) avg_score;
				}
			}
			if(r->top_aln.rc) {
				r->top_aln.ref_start += r->len;
			}
		}
	}
	store_alns_sam(reads, ref, params);
	printf("Total post-processing time: %.2f sec\n", omp_get_wtime() - start_time);
}

void eval(reads_t& reads, const ref_t& ref, const index_params_t* params) {
	printf("////////////// Evaluation //////////////\n");
	// ---- debug -----
	for(uint32 i = 0; i < reads.reads.size(); i++) {
		read_t* r = &reads.reads[i];
		if (VERBOSE > 0 && r->top_aln.score >= 10 &&
				!pos_in_range(r->ref_pos_r, r->top_aln.ref_start, 30) &&
				!pos_in_range(r->ref_pos_l, r->top_aln.ref_start, 30)) {
			printf("WRONG: score %u max-votes: %u second-best-votes: %u true-contig-votes: %u true-bucket-hits: %u max-bucket-hits %u true-pos-l  %u true-pos-r: %u found-pos %u\n",
					r->top_aln.score,
					r->top_aln.inlier_votes, r->second_best_aln.inlier_votes, r->comp_votes_hit, r->true_n_bucket_hits, r->best_n_bucket_hits,
					r->ref_pos_l, r->ref_pos_r, r->top_aln.ref_start);
			//print_read(r);
		}
	}

#if(!SIM_EVAL)
	int q10a = 0;
	for(uint32 i = 0; i < reads.reads.size(); i++) {
		read_t* r = &reads.reads[i];
		if(!r->valid_minhash_f && !r->valid_minhash_rc) continue;
		if(r->top_aln.score >= 10) {
			q10a++;
		}
	}
	printf("Number of confidently mapped reads Q10 %u\n", q10a);
	return;
#endif

	int valid_hash = 0;
	int n_collected = 0;
	int n_proc_contigs = 0;
	int processed_true = 0;
	int bucketed_true = 0;
	int confident = 0;
	int acc = 0;
	int best_hits = 0;
	int true_pos_hits = 0;
	int score = 0;
	int max_votes_inl = 0;
	int max_votes_all = 0;
	int q10 = 0;
	int q30 = 0;
	int q30acc = 0;
	int q10acc = 0;
	int q30processed_true = 0;
	int q30bucketed_true = 0;
	int q10bucketed_true = 0;
	for(uint32 i = 0; i < reads.reads.size(); i++) {
		read_t* r = &reads.reads[i];
		if(!r->valid_minhash_f && !r->valid_minhash_rc) continue;
		valid_hash++;
		n_proc_contigs += r->n_proc_contigs;
		true_pos_hits += r->true_n_bucket_hits;
		if(r->collected_true_hit) {
			n_collected++;
		}
		if(r->processed_true_hit) {
			processed_true++;
		}
		if(r->bucketed_true_hit) {
			bucketed_true++;
		}
		if(r->top_aln.score < 5){
			continue;
		}
		confident++;
		if(!r->top_aln.rc) {
			if(pos_in_range(r->ref_pos_l, r->top_aln.ref_start, 20)) {
				r->dp_hit_acc = 1;
			}
		} else {
			if(pos_in_range(r->ref_pos_r, r->top_aln.ref_start, 20)) {
				r->dp_hit_acc = 1;
			}
		}
		acc += r->dp_hit_acc;
		best_hits += r->best_n_bucket_hits;
		score += r->top_aln.score;
		max_votes_inl += r->top_aln.inlier_votes;
		max_votes_all += r->top_aln.total_votes;

		if(r->top_aln.score >= 30) {
			q30++;
			if(r->dp_hit_acc) {
				q30acc++;
			}
			if(r->processed_true_hit) {
				q30processed_true++;
			}
			if(r->bucketed_true_hit) {
				q30bucketed_true++;
			}
		}
		if(r->top_aln.score >= 10) {
			q10++;
			if(r->dp_hit_acc) {
				q10acc++;
			}
			if(r->bucketed_true_hit) {
				q10bucketed_true++;
			}
		}
	}
	float avg_n_proc_contigs = (float) n_proc_contigs/valid_hash;
	float avg_true_pos_hits = (float) true_pos_hits/valid_hash;
	double stddev_true_pos_hits = 0;
	for(uint32 i = 0; i < reads.reads.size(); i++) {
		read_t* r = &reads.reads[i];
		if(!r->valid_minhash_f && !r->valid_minhash_rc) continue;
		stddev_true_pos_hits += pow((double)avg_true_pos_hits - r->true_n_bucket_hits, (double) 2);
	}
	stddev_true_pos_hits = sqrt((double)stddev_true_pos_hits/valid_hash);

	printf("Number of reads with valid F or RC hash %u \n", valid_hash);
	printf("Number of mapped reads COLLECTED true hit %u \n", n_collected);
	printf("Number of mapped reads PROC true hit %u \n", processed_true);
	printf("Number of mapped reads BUCK true hit %u \n", bucketed_true);
	printf("Number of confidently mapped reads > 0 %u / accurate %u (%f pct)\n", confident, acc, (float)acc/(float)confident);
	printf("Number of confidently mapped reads Q10 %u / accurate %u (%f pct)\n", q10, q10acc, 100 - 100 * (float)q10acc/(float)q10);
	printf("Number of confidently mapped reads Q30 %u / accurate %u (%f pct)\n", q30, q30acc, (float)q30acc/(float)q30);
	printf("Number of confidently mapped reads Q30 PROCESSED true %u \n", q30processed_true);
	printf("Number of confidently mapped reads Q30 BUCKET true %u \n", q30bucketed_true);
	printf("Number of confidently mapped reads Q10 BUCKET true %u \n", q10bucketed_true);
	printf("Avg number of max bucket hits per read %.8f \n", (float) best_hits/confident);
	printf("MEAN number of bucket hits with TRUE pos per read %.8f \n", (float) avg_true_pos_hits);
	printf("STDDEV number of bucket hits with TRUE pos per read %.8f \n", (float) stddev_true_pos_hits);
	printf("Avg score per read %.8f \n", (float) score/confident);
	printf("Avg max inlier votes per read %.8f \n", (float) max_votes_inl/confident);
	printf("Avg max all votes per read %.8f \n", (float) max_votes_all/confident);
	printf("AVG N_PROC_CONTIGS %.8f \n", avg_n_proc_contigs);

	/*std::string fname("process_contigs");
	fname += std::string("_h");
	fname += std::to_string(params->h);
	fname += std::string("_T");
	fname += std::to_string(params->n_tables);
	fname += std::string("_b");
	fname += std::to_string(params->sketch_proj_len);
	fname += std::string("_w");
	fname += std::to_string(params->ref_window_size);
	fname += std::string("_p");
	fname += std::to_string(params->n_buckets_pow2);
	fname += std::string("_k");
	fname += std::to_string(params->k);
	fname += std::string("_H");
	fname += std::to_string(params->max_count);
	std::ofstream stats_file;
	stats_file.open(fname.c_str(), std::ios::out | std::ios::app);
	if (!stats_file.is_open()) {
		printf("store_ref_idx: Cannot open the CONTIG STATS file %s!\n", fname.c_str());
		exit(1);
	}
	//stats_file << "length,count,m\n";
	for(uint32 i = 0; i < reads.reads.size(); i++) {
				read_t* r = &reads.reads[i];
				if(!r->valid_minhash && !r->valid_minhash_rc) continue;
		stats_file << r->len << "," << r->n_proc_contigs << "," << params->min_n_hits << "\n";
	}
		stats_file.close();
	*/
}
