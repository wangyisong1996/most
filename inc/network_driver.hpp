#ifndef DUCK_NETWORK_DRIVER_H
#define DUCK_NETWORK_DRIVER_H

#include <stdint.h>

namespace NetworkDriver {
	extern uint8_t ip[4];
	extern uint8_t mac[6];
	extern uint8_t gateway[4];
	extern uint8_t prefix_len;
	extern uint8_t server_ip[4];
	extern bool do_not_send_answer;
	
	void init();
	
	// returns: # of bytes sent
	extern int (*send)(const void *buf, int len);
	
	// returns: # of bytes received
	extern int (*receive)(void *buf);
	
	// returns: zero
	extern int (*flush)();
}

#endif
