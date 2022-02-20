#include <stdint.h>
#include <string.h>
#include <assert.h>
#include <algorithm>

#include <inc/virtio_net.hpp>
#include <inc/logger.hpp>
#include <inc/utils.hpp>
#include <inc/x86_64.hpp>
#include <inc/memory.hpp>
#include <inc/pci.hpp>
#include <inc/timer.hpp>

// kernel pages are 4k-sized
using Memory::PAGE_SIZE;

// using x86_64::memory_barrier;

static inline void memory_barrier() {
	asm volatile("mfence" : : : "memory");
}

namespace virtio_net {
	// (legacy interface)
	static uint32_t regio_base;
	static uint64_t mmio_base;
	static bool is_mmio;
	
	template <typename T>
	static T read_mem(uint64_t addr) {
		T ret;
		memcpy(&ret, (const void *) addr, sizeof(T));
		return ret;
	}
	
	template <typename T>
	static void write_mem(uint64_t addr, uint32_t value) {
		memcpy((void *) addr, &value, sizeof(T));
	}
	
	struct io_reg {
		int offset;
		int size;
		
		void init(int offset, int size) {
			this->offset = offset;
			this->size = size;
		}
		
		uint32_t read() {
			if (is_mmio) {
				memory_barrier();
				switch (size) {
					case 4: return read_mem<uint32_t>(mmio_base + offset);
					case 2: return read_mem<uint16_t>(mmio_base + offset);
					default: return read_mem<uint8_t>(mmio_base + offset);
				}
			} else {
				switch (size) {
					case 4: return x86_64::inl(regio_base + offset);
					case 2: return x86_64::inw(regio_base + offset);
					default: return x86_64::inb(regio_base + offset);
				}
			}
		}
		
		void write(uint32_t value) {
			if (is_mmio) {
				memory_barrier();
				switch (size) {
					case 4: return write_mem<uint32_t>(mmio_base + offset, value);
					case 2: return write_mem<uint16_t>(mmio_base + offset, value);
					default: return write_mem<uint8_t>(mmio_base + offset, value);
				}
				memory_barrier();
			} else {
				switch (size) {
					case 4: return x86_64::outl(regio_base + offset, value), void();
					case 2: return x86_64::outw(regio_base + offset, value), void();
					default: return x86_64::outb(regio_base + offset, value), void();
				}
			}
		}
		
		void write_or(uint32_t value) {
			write(read() | value);
		}
	};
	
	struct VirtIOCommonRegs {
		io_reg device_features;
		io_reg driver_features;
		io_reg queue_address;
		io_reg queue_size;
		io_reg queue_select;
		io_reg queue_notify;
		io_reg device_status;
		io_reg ISR_status;
		io_reg mac[6];
		
		void init() {
			device_features.init(0, 4);
			driver_features.init(4, 4);
			queue_address.init(8, 4);
			queue_size.init(12, 2);
			queue_select.init(14, 2);
			queue_notify.init(16, 2);
			device_status.init(18, 1);
			ISR_status.init(19, 1);
			
			for (int i = 0; i < 6; i++) {
				mac[i].init(20 + i, 1);
			}
		}
	};
	
	static VirtIOCommonRegs common_regs;
	
	#define VIRTQ_DESC_F_NEXT 1
	#define VIRTQ_DESC_F_WRITE 2
	#define VIRTQ_DESC_F_INDIRECT 4
	
	struct VirtQueueDesc {
		uint64_t addr;  // GPA
		uint32_t len;
		uint16_t flags;
		uint16_t next;
	} __attribute__((packed));
	
	#define VIRTQ_AVAIL_F_NO_INTERRUPT 1
	
	struct VirtQueueAvail {
		uint16_t flags;
		uint16_t idx;
		uint16_t ring[];
	} __attribute__((packed));
	
	#define VIRTQ_USED_F_NO_NOTIFY 1
	
	struct VirtQueueUsedElement {
		uint32_t id;
		uint32_t len;
	} __attribute__((packed));
	
	struct VirtQueueUsed {
		uint16_t flags;
		uint16_t idx;
		VirtQueueUsedElement ring[];
	} __attribute__((packed));
	
	const int MAX_SUPPORTED_QUEUE_SIZE = 4096;
	const int QUEUE_MEMORY_SIZE = MAX_SUPPORTED_QUEUE_SIZE * (18 + 8) + 2 * PAGE_SIZE;
	static char queue_memory_pool[QUEUE_MEMORY_SIZE * 2] __attribute__((aligned(PAGE_SIZE)));
	static uint32_t queue_memory_pool_allocated = 0;
	
	static void * alloc_queue_memory(uint32_t size) {
		assert(size + queue_memory_pool_allocated <= sizeof(queue_memory_pool));
		void *ret = queue_memory_pool + queue_memory_pool_allocated;
		queue_memory_pool_allocated += size;
		return ret;
	}
	
	const int BUFFER_LEN = 1600;
	const int MAX_ACTUAL_QUEUE_SIZE = 128;
	const int QUEUE_BUFFER_POOL_SIZE = MAX_ACTUAL_QUEUE_SIZE * BUFFER_LEN;
	static char queue_buffer_pool[QUEUE_BUFFER_POOL_SIZE * 2] __attribute__((aligned(PAGE_SIZE)));
	static uint32_t queue_buffer_pool_allocated = 0;
	
	static void * alloc_queue_buffer(uint32_t size) {
		assert(size + queue_buffer_pool_allocated <= sizeof(queue_buffer_pool));
		void *ret = queue_buffer_pool + queue_buffer_pool_allocated;
		queue_buffer_pool_allocated += size;
		return ret;
	}
	
	struct VirtQueue {
		int queue_id;
		uint32_t queue_size;
		uint32_t actual_queue_size;
		VirtQueueDesc *desc;
		VirtQueueAvail *avail;
		VirtQueueUsed *used;
		uint16_t cur_used_idx;
		
		void init(int queue_id, bool is_receive) {
			common_regs.queue_select.write(queue_id);
			uint32_t queue_size = common_regs.queue_size.read();
			uint32_t actual_queue_size = std::min((uint32_t) MAX_ACTUAL_QUEUE_SIZE, queue_size);
			LDEBUG("init_queue %d, size %u (%u actual)", queue_id, queue_size, actual_queue_size);
			
			uint32_t desc_size = 16 * queue_size;
			uint32_t avail_size = 6 + 2 * queue_size;
			uint32_t used_size = 6 + 8 * queue_size;
			
			uint32_t part1_size = Utils::round_up(desc_size + avail_size, PAGE_SIZE);
			uint32_t total_size = Utils::round_up(part1_size + used_size, PAGE_SIZE);
			
			uint64_t queue_base = (uint64_t) alloc_queue_memory(total_size);
			this->desc = (VirtQueueDesc *) queue_base;
			this->avail = (VirtQueueAvail *) (queue_base + desc_size);
			this->used = (VirtQueueUsed *) (queue_base + part1_size);
			
			this->avail->flags = VIRTQ_AVAIL_F_NO_INTERRUPT;
			this->avail->idx = 0;
			
			this->used->flags = VIRTQ_USED_F_NO_NOTIFY;
			this->used->idx = 0;
			this->cur_used_idx = 0;
			
			uint16_t desc_flags = is_receive ? VIRTQ_DESC_F_WRITE : 0;
			for (uint32_t id = 0; id < queue_size; id++) {
				if (id < actual_queue_size) {
					this->desc[id] = (VirtQueueDesc) {
						(uint64_t) alloc_queue_buffer(BUFFER_LEN),
						BUFFER_LEN,
						desc_flags,
						0
					};
				} else {
					this->desc[id] = (VirtQueueDesc) {
						0, 0, 0, 0
					};
				}
			}
			
			if (is_receive) {
				for (uint16_t id = 0; id < actual_queue_size; id++) {
					this->add_avail(id);
				}
			} else {
				// un-pop used?
				for (uint16_t id = 0; id < actual_queue_size; id++) {
					this->used->ring[-id & (queue_size - 1)].id = id;
				}
				this->cur_used_idx = -actual_queue_size;
			}
			
			memory_barrier();
			
			common_regs.queue_address.write(queue_base / PAGE_SIZE);
			common_regs.queue_notify.write(queue_id);
			
			this->queue_id = queue_id;
			this->queue_size = queue_size;
		}
		
		void add_avail(uint16_t desc_id) {
			uint16_t idx = avail->idx & (queue_size - 1);
			avail->ring[idx] = desc_id;
			memory_barrier();
			avail->idx++;
			memory_barrier();
		}
		
		uint16_t pop_used(uint32_t &used_len) {
			memory_barrier();
			uint16_t new_idx = used->idx;
			if (new_idx == cur_used_idx) {
				return 0xffff;
			}
			
			const VirtQueueUsedElement &e = used->ring[cur_used_idx++ & (queue_size - 1)];
			used_len = e.len;
			return e.id;
		}
		
		bool send(const void *buf, int len) {
			uint32_t _;
			uint16_t desc_id = pop_used(_);
			if (desc_id == 0xffff) {
				return false;
			}
			
			memcpy((void *) desc[desc_id].addr, buf, len);
			desc[desc_id].len = len;
			add_avail(desc_id);
			// TODO flush
			
			common_regs.queue_notify.write(queue_id);
			
			return true;
		}
		
		// buf has enough length
		int recv(void *buf, uint32_t offset = 0) {
			common_regs.queue_notify.write(queue_id);
			
			uint32_t len;
			uint16_t desc_id = pop_used(len);
			if (desc_id == 0xffff) {
				return -1;
			}
			
			if (len < offset) {
				return -1;
			}
			
			memcpy(buf, (const void *) (desc[desc_id].addr + offset), len - offset);
			add_avail(desc_id);
			
			return len;
		} 
	};
	
	static VirtQueue receive_queue, transmit_queue;
	
	static void init_queue() {
		Memory::map_region_cache_disabled(
			(uint64_t) queue_memory_pool,
			(uint64_t) queue_memory_pool + sizeof(queue_memory_pool),
			(uint64_t) queue_memory_pool);
		
		Memory::map_region_cache_disabled(
			(uint64_t) queue_buffer_pool,
			(uint64_t) queue_buffer_pool + sizeof(queue_buffer_pool),
			(uint64_t) queue_buffer_pool);
		
		receive_queue.init(0, true);
		transmit_queue.init(1, false);
	}
	
	static bool init_virtio_net_regs(uint32_t vendor_id, uint32_t device_id) {
		// Try regio
		uint64_t r;
		r = PCI::get_device_reg_base(vendor_id, device_id);
		
		if (r != -1ull) {
			regio_base = r;
			is_mmio = false;
			common_regs.init();
			return true;
		}
		
		// Try mmio
		static char mmio[PAGE_SIZE] __attribute__((aligned(PAGE_SIZE)));
		r = PCI::map_device(vendor_id, device_id, (uint64_t) mmio, sizeof(mmio), 0);
		
		if (r != -1ull) {
			is_mmio = true;
			mmio_base = (uint64_t) mmio;
			common_regs.init();
			return true;
		} else {
			return false;
		}
	}
	
	bool init(uint8_t mac[6]) {
		LDEBUG_ENTER_RET();
		
		if (!init_virtio_net_regs(0x1af4, 0x1000)) {
			return false;
		}
		
		LDEBUG("status = 0x%x, resetting ...", common_regs.device_status.read());
		common_regs.device_status.write(0);  // RESET
		while (common_regs.device_status.read() != 0);
		LDEBUG("status = 0x%x", common_regs.device_status.read());
		
		common_regs.device_status.write_or(1);  // ACKNOWLEDGE
		common_regs.device_status.write_or(2);  // DRIVER
		LDEBUG("status = 0x%x", common_regs.device_status.read());
		
		uint32_t device_features = common_regs.device_features.read();
		LDEBUG("device_features = 0x%x", device_features);
		
		uint32_t supported_features = 1 << 5 | 1 << 16;  // MAC, STATUS
		common_regs.driver_features.write(supported_features & device_features);
		common_regs.device_status.write_or(8);  // FEATURES_OK
		
		LDEBUG("status = 0x%x, features_ok = %s",
			common_regs.device_status.read(),
			(common_regs.device_status.read() & 8) != 0 ? "true" : "false");
		
		for (int i = 0; i < 6; i++) {
			mac[i] = common_regs.mac[i].read();
		}
		
		LDEBUG("MAC (from device): %02x:%02x:%02x:%02x:%02x:%02x",
			mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
		
		init_queue();
		
		common_regs.device_status.write_or(4);  // DRIVER_OK
		
		LDEBUG("DRIVER_OK set, status = 0x%x", common_regs.device_status.read());
		
		return true;
	}
	
	#define VIRTIO_NET_HDR_F_NEEDS_CSUM 1
	#define VIRTIO_NET_HDR_F_DATA_VALID 2
	#define VIRTIO_NET_HDR_F_RSC_INFO 4
	
	#define VIRTIO_NET_HDR_GSO_NONE 0
	#define VIRTIO_NET_HDR_GSO_TCPV4 1
	#define VIRTIO_NET_HDR_GSO_UDP 3
	#define VIRTIO_NET_HDR_GSO_TCPV6 4
	#define VIRTIO_NET_HDR_GSO_ECN 0x80
	
	struct VirtIONetHeader {
		uint8_t flags;
		uint8_t gso_type;
		uint16_t hdr_len;
		uint16_t gso_size;
		uint16_t csum_start;
		uint16_t csum_offset;
	} __attribute__((packed));
	
	// returns: # of bytes sent
	int send(const void *buf, int len) {
		if (len < 0 || len > 1500) {
			return -1;
		}
		
		// LINFO("send len %d", len);
		
		static char buffer[sizeof(VirtIONetHeader) + 1500] __attribute__((aligned(16)));
		
		VirtIONetHeader *net_header = (VirtIONetHeader *) buffer;
		net_header->flags = 0;
		net_header->gso_type = VIRTIO_NET_HDR_GSO_NONE;
		// net_header->csum_start = 0;
		// net_header->csum_offset = len;
		memcpy(buffer + sizeof(VirtIONetHeader), buf, len);
		
		bool r = transmit_queue.send(buffer, sizeof(VirtIONetHeader) + len);
		// printf("r = %d\n", r);
		return r ? len : -1;
	}
	
	// returns: # of bytes received
	int receive(void *buf) {
		int len = receive_queue.recv(buf, sizeof(VirtIONetHeader));
		// if (len != -1) LINFO("recv len %d", len);
		return len;
	}
	
	// returns: zero
	int flush() {
		// unimplemented();
		// TODO
		return 0;
	}
}
