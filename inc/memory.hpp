#ifndef DUCK_MEMORY_H
#define DUCK_MEMORY_H

namespace Memory {
	const uint32_t PAGE_SIZE = 4096;  // 4KiB
	const uint32_t HUGE_PAGE_SIZE = 2097152;  // 2MiB
	const uint32_t P3_PAGE_SIZE = 1 << 30;  // 1GiB
	
	void init();
	
	void register_available_huge_page(void *addr);
	
	template <class T>
	static inline T remap(const T &addr) {
		return (T) ((uint64_t) addr - (1024ull << 30));  // remap to -1024 GiB
	}
	
	// Lower bound of userspace memory
	uint64_t get_kernel_break();
	
	// Upper bound of userspace memory
	uint64_t get_vaddr_break();
	
	void set_page_flags_user_writable(uint64_t start, uint64_t end);
	void set_page_flags_user_executable(uint64_t start, uint64_t end);
	void set_page_flags_kernel(uint64_t start, uint64_t end);
	
	void clear_access_and_dirty_flags(uint64_t start, uint64_t end);
	uint64_t count_dirty_pages(uint64_t start, uint64_t end);
}

#endif
