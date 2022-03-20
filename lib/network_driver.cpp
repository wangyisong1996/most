#include <string.h>

#include <inc/network_driver.hpp>
#include <inc/pci.hpp>
#include <inc/logger.hpp>
#include <inc/e1000.hpp>
#include <inc/virtio_net.hpp>
#include <inc/multiboot2_loader.hpp>
#include <inc/utils.hpp>

namespace NetworkDriver {
	uint8_t ip[4], mac[6];
	uint8_t gateway[4];
	uint8_t prefix_len;
	uint8_t server_ip[4];
	bool do_not_send_answer;
	
	static bool read_ip(const char *ip_str, uint8_t ip[4]) {
		int r = sscanf(ip_str, "%hhu.%hhu.%hhu.%hhu", &ip[0], &ip[1], &ip[2], &ip[3]);
		return r == 4;
	}
	
	static bool read_mac(const char *mac_str, uint8_t mac[6]) {
		int r = sscanf(mac_str, "%hhx:%hhx:%hhx:%hhx:%hhx:%hhx",
			&mac[0], &mac[1], &mac[2], &mac[3], &mac[4], &mac[5]);
		return r == 6;
	}
	
	static bool read_network_config(const char *cmdline) {
		const int MAX_LEN = 256;
		bool has_valid_ip = false;
		bool has_valid_mac = false;
		bool has_valid_gateway = false;
		bool has_valid_prefix_len = false;
		bool has_valid_server_ip = false;
		
		const char *ch = cmdline;
		for (; *ch; ch++) {
			if (*ch == ' ') continue;
			
			char buf[MAX_LEN], content[MAX_LEN];
			int buf_len = 0;
			while (*ch && *ch != ' ' && *ch != '\t' && buf_len + 1 < MAX_LEN) {
				buf[buf_len++] = *ch;
				ch++;
			}
			buf[buf_len] = 0;
			
			if (1 == sscanf(buf, " ip = %s ", content)) {
				has_valid_ip = read_ip(content, ip);
			} else if (1 == sscanf(buf, " mac = %s ", content)) {
				has_valid_mac = read_mac(content, mac);
			} else if (1 == sscanf(buf, " gateway = %s ", content)) {
				has_valid_gateway = read_ip(content, gateway);
			} else if (1 == sscanf(buf, " prefix_len = %hhu ", &prefix_len)) {
				has_valid_prefix_len = prefix_len <= 32;
			} else if (1 == sscanf(buf, " server_ip = %s", content)) {
				has_valid_server_ip = read_ip(content, server_ip);
			} else if (1 == sscanf(buf, " do_not_send_answer = %s", content)) {
				int val;
				if (1 == sscanf(content, "%d", &val)) {
					do_not_send_answer = val != 0;
				}
			}
		}
		
		if (!has_valid_ip || !has_valid_gateway || !has_valid_prefix_len) {
			return false;
		}
		
		if (!has_valid_mac) {		
			LWARN("Invalid or unspecified mac, using defaults...");
			uint8_t _mac[6] = { 0x52, 0x54, 0x00, 0x12, 0x34, 0x56 };
			memcpy(mac, _mac, 6 * sizeof(uint8_t));
		}
		
		if (!has_valid_server_ip) {
			LERROR("No valid server ip!");
			Utils::GG_reboot();
		}
		
		return true;
	}
	
	// returns: # of bytes sent
	int (*send)(const void *buf, int len);
	
	// returns: # of bytes received
	int (*receive)(void *buf);
	
	// returns: zero
	int (*flush)();
	
	void init() {
		LDEBUG_ENTER_RET();
		
		if (!read_network_config(Multiboot2_Loader::command_line)) {
			LWARN("Invalid network configuration, using defaults...");
			uint8_t _ip[4] = { 10, 0, 2, 123 };
			uint8_t _mac[6] = { 0x52, 0x54, 0x00, 0x12, 0x34, 0x56 };
			uint8_t _gateway[4] = { 0, 0, 0, 0 };
			uint8_t _prefix_len = 0;
			
			memcpy(ip, _ip, sizeof(ip));
			memcpy(mac, _mac, sizeof(mac));
			memcpy(gateway, _gateway, sizeof(gateway));
			prefix_len = _prefix_len;
		}
		
		if (e1000::init(mac)) {
			send = e1000::send;
			receive = e1000::receive;
			flush = e1000::flush;
			LINFO("e1000 driver initialized");
		} else if (virtio_net::init(mac)) {
			send = virtio_net::send;
			receive = virtio_net::receive;
			flush = virtio_net::flush;
			LINFO("virtio-net driver initialized");
		} else {
			LWARN("No network driver found. Running in standalone mode...");
		}
		
		LINFO("IP = %u.%u.%u.%u/%u  MAC = %02x:%02x:%02x:%02x:%02x:%02x  gateway = %u.%u.%u.%u",
			ip[0], ip[1], ip[2], ip[3], prefix_len,
			mac[0], mac[1], mac[2], mac[3], mac[4], mac[5],
			gateway[0], gateway[1], gateway[2], gateway[3]);
	}
}
