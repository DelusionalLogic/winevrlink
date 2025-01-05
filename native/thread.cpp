#include "thread.h"

#include <thread>

namespace shim {
	ThreadData::ThreadData(ThreadProc proc, void* userdata) { }
	ThreadData::ThreadData() { }

	thread::thread(ThreadProc proc, void* userdata) {
		std::thread(proc, userdata).detach();
	}
}
