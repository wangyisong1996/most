#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <assert.h>
#include <x86intrin.h>
#include <algorithm>

#include <inc/solver.hpp>

#include "client_secret.h"

#define SERVER_ADDR "172.1.1.119"

#define TARGET_AVX2 __attribute__((optimize("Ofast"), target("avx2")))
#define TARGET_AVX512 __attribute__((optimize("Ofast"), target("avx512f")))

static bool no_report = false;
static bool no_report_and_slient = false;

static uint64_t last_recv_tsc, last_send_tsc;
const uint64_t tsc_freq_us = 2500;

static void (*send_fn)(const char *, int) = NULL;

namespace Reporter {
    const int MAX_N_STATS = 10;
    static struct Stat {
        double local_latency;
        int n_digits;
        int m_number;
    } stats[MAX_N_STATS];
    static int n_stats = 0;
    
    static void print_stat(int bytes_processed) {
        for (int i = 0; i < n_stats; i++) {
            printf(" ===== sent %d digits (M%d), local_lat %.0lf us\n",
                stats[i].n_digits, stats[i].m_number,
                stats[i].local_latency);
        }
		(void)(bytes_processed);
        // double dt = (last_process_done_tsc - last_recv_tsc) / (double) tsc_freq_us;
        // printf("processed %d bytes in %.0lf us (%.1lf ns/byte)\n",
        //     bytes_processed, dt, dt * 1000 / bytes_processed);
        n_stats = 0;
    }
    
    static void add_stat(double local_latency, int n_digits, int m_number) {
        if (n_stats >= MAX_N_STATS) return;
        stats[n_stats++] = (Stat) {
            local_latency, n_digits, m_number
        };
    }
    
    const char *header_format =
    "POST /submit?" CLIENT_SECRET " HTTP/1.1\r\n"
    "Host: " SERVER_ADDR ":10002\r\n"
    "User-Agent: curl/7.68.0\r\n"
    "Accept: */*\r\n"
    "Content-Length: %d\r\n"
    "Content-Type: application/x-www-form-urlencoded\r\n"
    "\r\n";
    
    
    static void init() {
		// nothing
    }
    
    static bool send_request(const char *buf, int len, int n_digits, int m_number) {
        last_send_tsc = __rdtsc();
        double local_latency = (last_send_tsc - last_recv_tsc) / (double) tsc_freq_us;
        // if (local_latency > 5000) {
        //     // DO NOT send !!!
        //     printf("DO_NOT_SEND: (local latency %.0lf us, n_digits %d, msg_len %d)\n", local_latency, n_digits, len);
        //     return false;
        // }
        
        add_stat(local_latency, n_digits, m_number);
		send_fn(buf, len);
		
		return true;
    }
    
    static void report_string(const char *str, int len, int m_number) {
        static char buf[1024];
        int header_len = sprintf(buf, header_format, len);
        
        memcpy(buf + header_len, str, len);
        send_request(buf, header_len + len, len, m_number);
    }
    
    static void report_digits_reversed(const int *digits, int len, int m_number) {
        if (no_report) return;
        
        static char buf[1024];
        for (int i = 0; i < len; i++) {
            buf[len - 1 - i] = digits[i] + '0';
        }
        
		// printf("send: ");
		// fwrite(buf, 1, len, stdout);
		// printf("\n");
        report_string(buf, len, m_number);
    }
}


// N  = 256
// M1 = 20220217214410
// M2 = 104648257118348370704723119
// M3 = 125000000000000140750000000000052207500000000006359661
// M4 = a hidden but fixed integer, whose prime factors include and only include 3, 7 and 11
// M4 = 3^50 * 7^30 * 11^20

namespace Solver {
    #define u16 uint16_t
    #define u32 uint32_t
    #define u64 uint64_t
    #define u128 __uint128_t
    
    const int N = 256;
    const int MAX_BATCH_SIZE = 400;
    
    static int digits[N];  // N-1 ... 0
    static int new_digits[MAX_BATCH_SIZE];  // len-1 ... 0
    static int digits_extended[N + MAX_BATCH_SIZE];  // concat(digits[N-1..0], new_digits[len-1..0])
    
    static bool need_check_M1 = false;
    static int current_m_number = 0;
    
    // M1: u64 (use M1/10 which is coprime with 10)
    // M2: u128
    // M3: [u64; 3]
    // M4: [u128; 3]
    // M4(wrong): [u64; 1] (use 3^14 * 7^7 * 11^5 (~6.3e17))
    
    // 0.1 = M2/10+1 = 10464825711834837070472312 mod M2
    // 0.1 = 350000000000000103 mod M3[0]  /* wolfram: 10^(-1) modulo XXX */
    // 0.1 = 350000000000000145 mod M3[1]
    // 0.1 = 50000000000000021 mod M3[2]
    // 0.1 = 71789798769185258877025 mod 3^50
    // 0.1 = 2253934029069225808786325 mod 7^30
    // 0.1 = 605474995439304008281 mod 11^20
    
    const u64 M1 = 2022021721441ull;  // M1/10
    const u128 M2 = (u128) 1046482571183 * (u128) 100000000000000 + 48370704723119ull;
    const u64 M3_base = 500000000000000000ull;
    const u64 M3[3] = { M3_base + 147, M3_base + 207, M3_base + 209 };
    const u128 M4[3] = {
        (u128) 847288609443ull * (u128) 847288609443ull,
        (u128) 4747561509943ull * (u128) 4747561509943ull,
        (u128) 25937424601ull * (u128) 25937424601ull,
    };
    // const u64 M4[1] = { 634376770918484517ull };
    
    const u64 inv_10_M1 = 1819819549297ull;
    const u128 inv_10_M2 = (u128) 104648257118 * (u128) 100000000000000 + 34837070472312ull;
    const u64 inv_10_M3[3] = {
        350000000000000103ull,
        350000000000000145ull,
        50000000000000021ull
    };
    const u128 inv_10_M4[3] = {
        (u128) 717897987ull * (u128) 100000000000000 + 69185258877025ull,
        (u128) 22539340290ull * (u128) 100000000000000 + 69225808786325ull,
        (u128) 6054749ull * (u128) 100000000000000 + 95439304008281ull
    };
    // const u64 inv_10_M4[1] = { 444063739642939162ull };
    
    // pm[i]: [last (N-1) ... last i] || [0] * i
    // pm.length: N
    
    // Decimal point at head
    static u64 prefix_mod_M1[N], prepared_values_M1[MAX_BATCH_SIZE * 10];
    static u128 prefix_mod_M2[N], prepared_values_M2[MAX_BATCH_SIZE * 10];
    static u64 prefix_mod_M3[3][N], prepared_values_M3[3][MAX_BATCH_SIZE * 10];
    static u128 prefix_mod_M4[3][N], prepared_values_M4[3][MAX_BATCH_SIZE * 10];
    // static u64 prefix_mod_M4[1][N], prepared_values_M4[1][MAX_BATCH_SIZE * 10];
    
    // 10 ^ (- digits_read)
    static u64 cur_exp_M1;
    static u128 cur_exp_M2;
    static u64 cur_exp_M3[3];
    static u128 cur_exp_M4[3];
    // static u64 cur_exp_M4[1];
    
	static int rand_seed = 23331234;
	
	static inline int rnd() {
		return rand_seed = rand_seed * 16807LL % 217483647;
	}
	
    template <typename T>
    void rand_fill(T *a) {
        int size = sizeof(T);
        assert(size % 16 == 0);
        for (int i = 0; i * 4 < size; i += 4) {
            ((int *) a)[i] = rnd();
        }
    }
    
    static void init_prefix_mod() {
        rand_fill(&prefix_mod_M1);
        rand_fill(&prefix_mod_M2);
        rand_fill(&prefix_mod_M3);
        rand_fill(&prefix_mod_M4);
        
        cur_exp_M1 = 1;
        cur_exp_M2 = 1;
        cur_exp_M3[0] = cur_exp_M3[1] = cur_exp_M3[2] = 1;
        cur_exp_M4[0] = cur_exp_M4[1] = cur_exp_M4[2] = 1;
        // cur_exp_M4[0] = 1;
    }
    
    TARGET_AVX2
    static inline u64 mul_mod(const u64 &a, const u64 &b, const u64 &M) {
        // return (u128) a * b % M;
        u64 ret;
        __asm__ ("mulq %2; divq %3" : "=d"(ret) : "a"(a), "D"(b), "c"(M));
        return ret;
    }
    
    // 0 <= a, b < M < 1.2e27 < (1 << 90)
    TARGET_AVX2
    static inline u128 mul_mod(const u128 &a, const u128 &b, const u128 &M, const u128 *e67890) {
        // u32 b0 = b, b1 = b >> 32, b2 = b >> 64;
        // u64 e = 1ull << 32;
        // return (a * b0 % M + a * b1 % M * e % M + a * b2 % M * e % M * e % M) % M;
        // return (a * b0 + a * b1 % M * e + a * b2 % M * e % M * e) % M;
        // return (a * b0 + (a * b1 + a * b2 % M * e) % M * e) % M;
        
        #define AA(i) (u16) (a >> (i * 16))
        #define BB(i) (u16) (b >> (i * 16))
        u16 A[6] = { AA(0), AA(1), AA(2), AA(3), AA(4), AA(5) };
        u16 B[6] = { BB(0), BB(1), BB(2), BB(3), BB(4), BB(5) };
        #undef AA
        #undef BB
        
        #define M(i,j) ((u64) A[i] * B[j])
        u64 p[11] = {
            M(0,0),
            M(0,1) + M(1,0),
            M(0,2) + M(1,1) + M(2,0),
            M(0,3) + M(1,2) + M(2,1) + M(3,0),
            M(0,4) + M(1,3) + M(2,2) + M(3,1) + M(4,0),
            M(0,5) + M(1,4) + M(2,3) + M(3,2) + M(4,1) + M(5,0),
            M(1,5) + M(2,4) + M(3,3) + M(4,2) + M(5,1),
            M(2,5) + M(3,4) + M(4,3) + M(5,2),
            M(3,5) + M(4,4) + M(5,3),
            M(4,5) + M(5,4),
            M(5,5)
        };
        #undef M
        
        #define P(i) ((u128) p[i] << (i * 16))
        #define Q(i) (p[i] * e67890[i - 6])
        u128 sum = P(0) + P(1) + P(2) + P(3) + P(4) + P(5)
            + Q(6) + Q(7) + Q(8) + Q(9) + Q(10);
        #undef P
        #undef Q
        
        return sum % M;
    }
    
    #define T u64
    TARGET_AVX2
    static void do_prepare(T *values, T &cur_exp, const T &inv_10, const T &M) {
        for (int i = 0; i < MAX_BATCH_SIZE; i++) {
            values[0] = 0;
            values[1] = cur_exp;
            for (int j = 2; j < 10; j++) {
                values[j] = values[j - 1] + cur_exp;
                values[j] = values[j] >= M ? values[j] - M : values[j];
            }
            
            cur_exp = mul_mod(cur_exp, inv_10, M);
            values += 10;
        }
    }
    #undef T
    
    #define T u128
    TARGET_AVX2
    static void do_prepare(T *values, T &cur_exp, const T &inv_10, const T &M) {
        T e67890[11];
        e67890[6] = ((u128) 1ull << 96) % M;
        e67890[7] = ((u128) 1ull << 112) % M;
        e67890[8] = (e67890[7] << 16) % M;
        e67890[9] = (e67890[8] << 16) % M;
        e67890[10] = (e67890[9] << 16) % M;
        
        for (int i = 0; i < MAX_BATCH_SIZE; i++) {
            values[0] = 0;
            values[1] = cur_exp;
            for (int j = 2; j < 10; j++) {
                values[j] = values[j - 1] + cur_exp;
                values[j] = values[j] >= M ? values[j] - M : values[j];
            }
            
            cur_exp = mul_mod(cur_exp, inv_10, M, e67890 + 6);
            values += 10;
        }
    }
    #undef T
    
    static bool prepared = false;
    
    static void prepare_next_batch() {
        if (prepared) return;
        prepared = true;
        
        do_prepare(prepared_values_M1, cur_exp_M1, inv_10_M1, M1);
        
        do_prepare(prepared_values_M2, cur_exp_M2, inv_10_M2, M2);
        
        for (int i = 0; i < 3; i++) {
            do_prepare(prepared_values_M3[i], cur_exp_M3[i], inv_10_M3[i], M3[i]);
        }
        // do_prepare(prepared_values_M3[0], cur_exp_M3[0], inv_10_M3[0], M3[0]);
        
        for (int i = 0; i < 3; i++) {
            do_prepare(prepared_values_M4[i], cur_exp_M4[i], inv_10_M4[i], M4[i]);
        }
        // do_prepare(prepared_values_M4[0], cur_exp_M4[0], inv_10_M4[0], M4[0]);
    }
    
    void init(void (*send)(const char *, int)) {
        init_prefix_mod();
        prepare_next_batch();
		
		Reporter::init();
		
		send_fn = send;
    }
	
	void print_stat(int bytes_processed) {
		Reporter::print_stat(bytes_processed);
		prepare_next_batch();
	}
    
    template <typename T>
    // pv[i * 10 + j]: j * (10^(cur-i) mod M)  [will not overflow]
    static void do_extend(T *pm, const T &M, const T *pv, int *new_digits, int len, T *dst) {
        memcpy(dst + len, pm, N * sizeof(T));
        
        T sum = dst[len];
        for (int i = 0; i < len; i++) {
            sum = sum + pv[i * 10 + new_digits[len - 1 - i]];
            sum = sum >= M ? sum - M : sum;
            dst[len - 1 - i] = sum;
        }
    }
    
    // id1 < id2 && id2 - id1 <= N && a[id2 - 1] != 0
    // number(reversed): [id1, id2)
    static void add_to_report(int id1, int id2) {
        if (need_check_M1) {
            if (digits_extended[id1] != 0) {
                return;
            }
        }
        
        if (!no_report) {
            Reporter::report_digits_reversed(digits_extended + id1, id2 - id1, current_m_number);
        } else if (!no_report_and_slient) {
            printf("not reporting %d %d (len = %d)\n", id1, id2, id2 - id1);
        }
    }
    
    #define SORT_MASK 0x7ff
    #define SORT_COUNTER_TYPE int
    #define SORT_CNT 1
    #define SORTED_MASK 0x7ff
    
    template <typename T>
    TARGET_AVX2
    static void sort(T *a, int *ids, int *_ids, int n) {
        SORT_COUNTER_TYPE cnt[SORT_MASK + 1];
        
        for (int off = 0; off < 8 * SORT_CNT; off += 8) {
            memset(cnt, 0, sizeof(cnt));
            for (int i = 0; i < n; i++) cnt[(a[ids[i]] >> off) & SORT_MASK]++;
            for (int i = 1; i < SORT_MASK + 1; i++) cnt[i] += cnt[i - 1];
            for (int i = n - 1; i >= 0; i--) _ids[--cnt[(a[ids[i]] >> off) & SORT_MASK]] = ids[i];
            std::swap(ids, _ids);
        }
    }
    
    template <typename T>
    TARGET_AVX2
    static void extend_and_check_multi(T **pm, const T *M, T **prepared_values, T *cur_exp, int cnt, int *new_digits, int len) {
        static T extended[N + MAX_BATCH_SIZE];
        do_extend(pm[0], M[0], prepared_values[0], new_digits, len, extended);
        memcpy(pm[0], extended, N * sizeof(T));
        
        static int ids[N + MAX_BATCH_SIZE], _ids[N + MAX_BATCH_SIZE];
        for (int i = 0; i < N + len; i++) _ids[i] = i;
        sort(extended, _ids, ids, N + len);
        
        const int MAX_N_ANS = 10;
        int ans_id1[MAX_N_ANS], ans_id2[MAX_N_ANS];
        bool ans_valid[MAX_N_ANS];
        int n_ans = 0;
        
        for (int i = 0; i < N + len; i++) {
            T value = extended[ids[i]];
            for (int j = i + 1; j < N + len && (value & SORTED_MASK) == (extended[ids[j]] & SORTED_MASK); j++) {
                if (value != extended[ids[j]]) continue;
                
                int id1 = ids[i], id2 = ids[j];
                if (id1 > id2) std::swap(id1, id2);
                if (digits_extended[id2 - 1] == 0 || id1 >= len || id2 - id1 > N) continue;
                
                if (n_ans < MAX_N_ANS) {
                    ans_id1[n_ans] = id1;
                    ans_id2[n_ans] = id2;
                    ans_valid[n_ans] = true;
                    ++n_ans;
                }
            }
        }
        
        for (int idx = 1; idx < cnt; idx++) {
            do_extend(pm[idx], M[idx], prepared_values[idx], new_digits, len, extended);
            memcpy(pm[idx], extended, N * sizeof(T));
            
            for (int i = 0; i < n_ans; i++) {
                ans_valid[i] &= extended[ans_id1[i]] == extended[ans_id2[i]];
            }
        }
        
        for (int i = 0; i < n_ans; i++) {
            if (!ans_valid[i]) continue;
            add_to_report(ans_id1[i], ans_id2[i]);
        }
        
        // Bugfix: need to correct cur_exp after processing
        for (int i = 0; i < cnt; i++) {
            cur_exp[i] = prepared_values[i][len * 10 + 1];
        }
    }
    
    static void do_recv_input(const char *buf, int len) {
        for (int i = 0; i < len; i++) {
            new_digits[len - 1 - i] = buf[i] - '0';
            digits_extended[len - 1 - i] = buf[i] - '0';
        }
        memcpy(digits_extended + len, digits, N * sizeof(int));
        
        current_m_number = 1;
        need_check_M1 = true;
        u64 *pm_M1[1] = { prefix_mod_M1 };
        u64 *pv_M1[1] = { prepared_values_M1 };
        extend_and_check_multi(pm_M1, &M1, pv_M1, &cur_exp_M1, 1, new_digits, len);
        need_check_M1 = false;
        
        current_m_number = 2;
        u128 *pm_M2[1] = { prefix_mod_M2 };
        u128 *pv_M2[1] = { prepared_values_M2 };
        extend_and_check_multi(pm_M2, &M2, pv_M2, &cur_exp_M2, 1, new_digits, len);
        
        current_m_number = 3;
        u64 *pm_M3[3] = { prefix_mod_M3[0], prefix_mod_M3[1], prefix_mod_M3[2] };
        u64 *pv_M3[3] = { prepared_values_M3[0], prepared_values_M3[1], prepared_values_M3[2] };
        extend_and_check_multi(pm_M3, M3, pv_M3, cur_exp_M3, 3, new_digits, len);
        // extend_and_check_multi(pm_M3, M3, pv_M3, cur_exp_M3, 1, new_digits, len);
        
        current_m_number = 4;
        u128 *pm_M4[3] = { prefix_mod_M4[0], prefix_mod_M4[1], prefix_mod_M4[2] };
        u128 *pv_M4[3] = { prepared_values_M4[0], prepared_values_M4[1], prepared_values_M4[2] };
        extend_and_check_multi(pm_M4, M4, pv_M4, cur_exp_M4, 3, new_digits, len);
        // u64 *pm_M4[1] = { prefix_mod_M4[0] };
        // u64 *pv_M4[1] = { prepared_values_M4[0] };
        // extend_and_check_multi(pm_M4, M4, pv_M4, cur_exp_M4, 1, new_digits, len);
        
        memcpy(digits, digits_extended, N * sizeof(int));
    }
    
    static void _recv_input(const char *buf, int len) {
        if (len > MAX_BATCH_SIZE) {
            _recv_input(buf, MAX_BATCH_SIZE);
            _recv_input(buf + MAX_BATCH_SIZE, len - MAX_BATCH_SIZE);
            return;
        }
        
        if (!prepared) {
            printf("warn not prepared\n");
            prepare_next_batch();
        }
        
        // TODO proper setup ?
        do_recv_input(buf, len);
        
        prepared = false;
    }
    
    void recv_input(const char *buf, int len) {
        last_recv_tsc = __rdtsc();
        
        _recv_input(buf, len);
    }
}

