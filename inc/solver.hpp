#ifndef DUCK_SOLVER_H
#define DUCK_SOLVER_H

namespace Solver {
	void init(void (*send)(const char *, int));
	void recv_input(const char *buf, int len);
	void print_stat(int bytes_processed);
	void prepare();
}

#endif
