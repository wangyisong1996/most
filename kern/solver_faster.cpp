#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <assert.h>
#include <x86intrin.h>

#include <inc/solver.hpp>
#include <inc/timer.hpp>

// #pragma GCC target("avx2,avx512f")
#pragma GCC optimize("Ofast")

#define SERVER_ADDR "59.110.124.141"

static void (*send_fn)(const char *, int) = NULL;

static uint64_t last_recv_tsc, last_send_tsc;
static uint64_t tsc_freq_us = 0;

namespace Reporter {
    const int MAX_N_STATS = 10;
    static struct Stat {
        double local_latency;
        int n_digits;
        int m_number;
    } stats[MAX_N_STATS];
    static int n_stats = 0;
    
    static double sum_latency = 0;
    static int n_total_stats = 0;
    
    static void print_stat(int bytes_processed) {
        for (int i = 0; i < n_stats; i++) {
            sum_latency += stats[i].local_latency;
            n_total_stats++;
            
            printf(" #%d: %d digits (M%d), local_lat %.0lf, avg %.2lf us\n",
                n_total_stats,
                stats[i].n_digits, stats[i].m_number,
                stats[i].local_latency,
                sum_latency / n_total_stats);
        }
        printf(".");fflush(stdout);
        (void)(bytes_processed);
        n_stats = 0;
    }
    
    static void add_stat(double local_latency, int n_digits, int m_number) {
        if (n_stats >= MAX_N_STATS) return;
        stats[n_stats++] = (Stat) {
            local_latency, n_digits, m_number
        };
    }
    
    const char *header_format =
    "POST /submit"  " HTTP/1.1\r\n"
    "Host: " SERVER_ADDR ":10002\r\n"
    "User-Agent: curl/7.68.0\r\n"
    "Accept: */*\r\n"
    "Content-Length: %d\r\n"
    "Content-Type: application/x-www-form-urlencoded\r\n"
    "\r\n";
    
    static bool send_request(const char *buf, int len, int n_digits, int m_number) {
        last_send_tsc = __rdtsc();
        double local_latency = (last_send_tsc - last_recv_tsc) / (double) tsc_freq_us;
        
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
    
    static void report_digits(const int *digits, int len, int m_number) {
        if (digits[0] == 0) return;
        if (m_number == 1 && len < 14) return;
        if (m_number == 2 && len < 27) return;
        if (m_number == 3 && len < 54) return;
        if (m_number == 4 && len < 68) return;
        
        static char buf[1024];
        for (int i = 0; i < len; i++) {
            buf[i] = digits[i] + '0';
        }
        
        report_string(buf, len, m_number);
    }
}

namespace Solver {
    #define u16 uint16_t
    #define u32 uint32_t
    #define u64 uint64_t
    #define u128 __uint128_t
    
    const int N = 256;
    const int MAX_BATCH_SIZE = 400;
    const int HASH_SIZE = 1024;
    
    // ===== M1, M2, M3, M4 =====
    
    // const u64 M1 = 20220311122858ull;
    const u32 M1 = 299236546;  // 2*7*887*24097
    
    // 104648257118348370704723401
    const u128 M2 = (u128) 1046482571183 * (u128) 100000000000000 + 48370704723401ull;
    
    const u64 M3 = 500000000000000000ull + 243ull;
    
    const u32 M4 = 387420489;  // 3^18
    
    // =====
    
    static int digits[N + MAX_BATCH_SIZE];  // N-1 ... 0
    
    struct Hash {
        u16 first[HASH_SIZE];
        u16 next[N + MAX_BATCH_SIZE + 5];
        u16 values[N + MAX_BATCH_SIZE + 5];
        u64 keys[N + MAX_BATCH_SIZE + 5];
        u32 cnt;
        
        void clear() {
            cnt = 0;
            memset(first, 0, sizeof(first));
        }
        
        void insert(u64 key, u32 value) {
            u32 idx = key & (HASH_SIZE - 1);
            
            cnt++;
            next[cnt] = first[idx];
            keys[cnt] = key;
            values[cnt] = value;
            first[idx] = cnt;
        }
        
        void query_all(u64 key, u32 q_idx, int m_number) {
            u32 idx = key & (HASH_SIZE - 1);
            
            for (u32 i = first[idx]; i; i = next[i]) {
                if (keys[i] != key) continue;
                
                u32 value = values[i];
                if (q_idx - value > N) continue;
                
                // value is digit idx
                Reporter::report_digits(digits + value + 1, q_idx - value, m_number);
            }
        }
    };
    
    static Hash h1, h2, h3, h4;
    
    template <typename T>
    static inline T add_mod(T a, T b, T M) {
        T sum = a + b;
        return sum >= M ? sum - M : sum;
    }
    
    static u32 e1[10 * (N + MAX_BATCH_SIZE)];
    static u128 e2[10 * (N + MAX_BATCH_SIZE)];
    static u64 e3[10 * (N + MAX_BATCH_SIZE)];
    static u32 e4[10 * (N + MAX_BATCH_SIZE)];
    
    static u32 sum1;
    static u128 sum2;
    static u64 sum3;
    static u32 sum4;
    
    template <typename T>
    static void do_add_digits(int len, T sum, const T *e, Hash &h, T M, int m_number) {
        for (int i = 0; i < len; i++) {
            int d = digits[N + i];
            sum = add_mod(sum, e[(N + i) * 10 + d], M);
            
            h.query_all((u64) sum, N + i, m_number);
            h.insert((u64) sum, N + i);
        }
    }
    
    static void add_digits(int *new_digits, int len) {
        memcpy(digits + N, new_digits, len * sizeof(int));
        
        do_add_digits(len, sum1, e1, h1, M1, 1);
        do_add_digits(len, sum4, e4, h4, M4, 4);
        do_add_digits(len, sum3, e3, h3, M3, 3);
        do_add_digits(len, sum2, e2, h2, M2, 2);
        
        memmove(digits, digits + len, N * sizeof(digits[0]));
    }
    
    template <typename T>
    static void init_e(T *e, T M) {
        T cur = 1;
        
        for (int i = N + MAX_BATCH_SIZE - 1; i >= 0; i--) {
            T tmp = 0;
            
            for (int j = 0; j <= 9; j++) {
                e[i * 10 + j] = tmp;
                tmp = add_mod(tmp, cur, M);
            }
            
            cur = tmp;
        }
    }
    
    template <typename T>
    static void do_prepare_digits(T &sum, const T *e, Hash &h, T M) {
        sum = 233333333;
        h.clear();
        
        for (int i = 0; i < N; i++) {
            int d = digits[i];
            sum = add_mod(sum, e[i * 10 + d], M);
            
            h.insert((u64) sum, i);
            sum = sum;
        }
    }
    
    void prepare() {
        do_prepare_digits(sum1, e1, h1, M1);
        do_prepare_digits(sum2, e2, h2, M2);
        do_prepare_digits(sum3, e3, h3, M3);
        do_prepare_digits(sum4, e4, h4, M4);
    }
    
    void init(void (*send)(const char *, int)) {
        tsc_freq_us = Timer::tsc_freq / 1000000;
        printf("tsc_freq = %lu\n", tsc_freq_us);
        
        send_fn = send;
        assert((N & -N) == N);
        
        for (int i = 0; i < N; i++) {
            digits[i] = 9;
        }
        
        init_e(e1, M1);
        init_e(e2, M2);
        init_e(e3, M3);
        init_e(e4, M4);
        
        prepare();
    }
    
    void recv_input(const char *buf, int len) {
        assert(1 <= len && len <= MAX_BATCH_SIZE);
        last_recv_tsc = __rdtsc();
        
        static int new_digits[MAX_BATCH_SIZE];
        
        for (int i = 0; i < len; i++) {
            new_digits[i] = buf[i] - '0';
        }
        
        add_digits(new_digits, len);
    }
    
    void print_stat(int bytes_processed) {
        Reporter::print_stat(bytes_processed);
        
        prepare();
    }
}
