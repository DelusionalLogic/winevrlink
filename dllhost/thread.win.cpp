#include "thread.h"

#include <cstdint>
#include <windows.h>

static __attribute__((ms_abi)) uint32_t abi_adapter(void* userdata) {
	shim::ThreadData *thread = (shim::ThreadData*)userdata;
	thread->proc(thread->userdata);
	return 0;
}

namespace shim {
	ThreadData::ThreadData(ThreadProc proc, void* userdata) : proc(proc), userdata(userdata) { }

	thread::thread(ThreadProc proc, void* userdata) : data(new ThreadData(proc, userdata)) {
		CreateThread(
			nullptr,
			0x100000,
			abi_adapter,
			this->data,
			STACK_SIZE_PARAM_IS_A_RESERVATION,
			nullptr
		);
	}
}
