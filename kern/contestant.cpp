#include <string.h>
#include <assert.h>

#include <inc/contestant.hpp>
#include <inc/solver.hpp>
#include <inc/logger.hpp>
#include <inc/network_driver.hpp>
#include <inc/timer.hpp>
#include <inc/utils.hpp>

#include <lwip/init.h>
#include <lwip/netif.h>
#include <lwip/err.h>
#include <lwip/etharp.h>
#include <lwip/dhcp.h>
#include <lwip/timeouts.h>
#include <lwip/tcp.h>

using NetworkDriver::mac;
using NetworkDriver::ip;
using NetworkDriver::gateway;
using NetworkDriver::prefix_len;

#define MTU 1500

extern "C"
u32_t sys_now() {
	return Timer::ns_since_epoch() / 1000000ull;
}

static const char *invalid_buffer =
	"9999999999999999999999999999999999999999999999999999999999999999"
	"9999999999999999999999999999999999999999999999999999999999999999"
	"9999999999999999999999999999999999999999999999999999999999999999"
	"9999999999999999999999999999999999999999999999999999999999999999"
;
static const int invalid_buffer_len = 256;

// #define INIT_SERVER_IP IP4_ADDR(&server_ip, 47, 95, 111, 217)
#define INIT_SERVER_IP IP4_ADDR(&server_ip, 172, 1, 1, 119)

namespace Contestant {
	static struct netif netif;
	static ip4_addr server_ip;
	
	static enum {
		S_NOT_CONNECTED,
		S_CONNECTING_10002,
		S_CONNECTING_10001,
		S_CONNECTED,
		S_ABORT_NEXT_TICK
	} state;
	
	static struct tcp_pcb *conn_10001, *conn_10002;
	
	static void tcp_err_fn_10001(void *, err_t) {
		conn_10001 = NULL;
		state = S_ABORT_NEXT_TICK;
	}
	
	static void tcp_err_fn_10002(void *, err_t) {
		conn_10002 = NULL;
		state = S_ABORT_NEXT_TICK;
	}
	
	static void start_connecting_10001();
	
	static err_t tcp_connected_fn(void *, struct tcp_pcb *, err_t) {
		if (state == S_CONNECTING_10002) {
			start_connecting_10001();
			return ERR_OK;
		}
		
		assert(state == S_CONNECTING_10001);
		state = S_CONNECTED;
		
		Solver::recv_input(invalid_buffer, invalid_buffer_len);
		Solver::print_stat(0);
		
		return ERR_OK;
	}
	
	static int last_recv_len = 0;
	
	static err_t tcp_recv_fn_10001(void *, struct tcp_pcb *pcb, struct pbuf *p, err_t) {
		if (p == NULL) {
			state = S_ABORT_NEXT_TICK;
			return ERR_OK;
		}
		
		tcp_recved(pcb, p->tot_len);
		
		static char buf[MTU];
		int len = p->tot_len;
		pbuf_copy_partial(p, buf, len, 0);
		
		// printf("data:\n");
		// fwrite(buf, 1, len, stdout);
		// putchar('\n');
		
		bool has_non_digits = false;
		for (int i = 0; i < len; i++) {
			if (!(buf[i] >= '0' && buf[i] <= '9')) {
				has_non_digits = true;
				break;
			}
		}
		
		if (!has_non_digits) {
			Solver::recv_input(buf, len);
			last_recv_len = len;
			Solver::print_stat(last_recv_len);
		}
		
		pbuf_free(p);
		return ERR_OK;
	}
	
	static err_t tcp_recv_fn_10002(void *, struct tcp_pcb *pcb, struct pbuf *p, err_t) {
		if (p == NULL) {
			state = S_ABORT_NEXT_TICK;
			return ERR_OK;
		}
		
		tcp_recved(pcb, p->tot_len);
		pbuf_free(p);
		return ERR_OK;
	}
	
	static void start_connecting_10001() {
		state = S_CONNECTING_10001;
		conn_10001 = tcp_new();
		assert(conn_10001 != NULL);
		
		tcp_nagle_disable(conn_10001);
		tcp_err(conn_10001, tcp_err_fn_10001);
		tcp_recv(conn_10001, tcp_recv_fn_10001);
		tcp_connect(conn_10001, &server_ip, 10001, tcp_connected_fn);
	}
	
	static void start_connecting() {
		LINFO("start_connecting!");
		
		state = S_CONNECTING_10002;
		conn_10002 = tcp_new();
		assert(conn_10002 != NULL);
		
		tcp_nagle_disable(conn_10002);
		tcp_err(conn_10002, tcp_err_fn_10002);
		tcp_recv(conn_10002, tcp_recv_fn_10002);
		tcp_connect(conn_10002, &server_ip, 10002, tcp_connected_fn);
	}
	
	static void contestant_tick() {
		if (state == S_NOT_CONNECTED) {
			if (netif.ip_addr.addr != IPADDR_ANY) {
				// has IP
				start_connecting();
			}
		} else if (state == S_ABORT_NEXT_TICK) {
			if (conn_10001) {
				tcp_abort(conn_10001);
				conn_10001 = NULL;
			}
			if (conn_10002) {
				tcp_abort(conn_10002);
				conn_10002 = NULL;
			}
			
			state = S_NOT_CONNECTED;
		}
	}
	
	static void contestant_send(const char *buf, int len) {
		if (state != S_CONNECTED) return;
		
		(void)(buf), (void)(len);
		tcp_write(conn_10002, buf, len, TCP_WRITE_FLAG_COPY);
		tcp_output(conn_10002);
	}
	
	static void contestant_init() {
		Solver::init(contestant_send);
		Solver::print_stat(0);
		
		INIT_SERVER_IP;
	}
	
	void run() {
		LDEBUG_ENTER_RET();
		
		contestant_init();
		
		while (1) {
			static char pkt[MTU + 64];
			int len = NetworkDriver::receive(pkt);
			
			// check "udp and dport REDACTED"
			if (len >= 14 + 20 + 8
				&& * (uint16_t *) (pkt + 14 + 20 + 2) == htons(REDACTED)) {
				Utils::GG_reboot();
			}
			
			if (len > 0) {
				struct pbuf* p = pbuf_alloc(PBUF_RAW, len, PBUF_POOL);
				
				if (p != NULL) {
					pbuf_take(p, pkt, len);
					
					if (netif.input(p, &netif) != ERR_OK) {
						pbuf_free(p);
					}
				}
			}
			
			// lwip timers check
			sys_check_timeouts();
			
			// your application here
			contestant_tick();
		}
	}
	
	static err_t netif_output(struct netif *, struct pbuf *p) {
		// no link stats
		
		static char buf[MTU];
		pbuf_copy_partial(p, buf, p->tot_len, 0);
		
		// TODO: return value
		NetworkDriver::send(buf, p->tot_len);
		
		return ERR_OK;
	}
	
	static err_t netif_init(struct netif *netif) {
		netif->linkoutput = netif_output;
		netif->output = etharp_output;
		// no ip6
		netif->mtu = MTU;
		netif->flags = NETIF_FLAG_ETHARP | NETIF_FLAG_ETHERNET;
		netif->hwaddr_len = ETH_HWADDR_LEN;
		SMEMCPY(netif->hwaddr, mac, ETH_HWADDR_LEN);
		
		return ERR_OK;
	}
	
	void init() {
		LDEBUG_ENTER_RET();
		
		lwip_init();
		
		netif_add(&netif, IP4_ADDR_ANY, IP4_ADDR_ANY, IP4_ADDR_ANY, NULL, netif_init, netif_input);
		
		netif.name[0] = 'e';
		netif.name[1] = '0';
		
		// no status callback
		
		netif_set_default(&netif);
		netif_set_up(&netif);
		
		netif_set_link_up(&netif);
		
		dhcp_start(&netif);
		
		// ip4_addr ip;
		
		// IP4_ADDR(&ip, 10, 0, 2, 111);
		// netif_set_ipaddr(&netif, &ip);
		
		// IP4_ADDR(&ip, 255, 255, 255, 0);
		// netif_set_netmask(&netif, &ip);
	}
}
