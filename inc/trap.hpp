#ifndef DUCK_TRAP_H
#define DUCK_TRAP_H

#include <inc/x86_64.hpp>

namespace Trap {
	void init();
	
	static inline void enable() {
		x86_64::sti();
	}
	
	static inline void disable() {
		x86_64::cli();
	}
}

#endif
