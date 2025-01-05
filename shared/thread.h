#pragma once

typedef void (*ThreadProc)(void* userdata);

namespace shim {
	struct ThreadData {
		ThreadProc proc;
		void *userdata;

		ThreadData();
		ThreadData(ThreadProc, void*);
	};

	class thread {
		struct ThreadData *data;
		public:
			thread() {};
			explicit thread(ThreadProc, void*);
	};
}
