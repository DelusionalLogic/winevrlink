#pragma once

#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <map>
#include <mutex>
#include <vector>
#include <thread>
#include "thread.h"


enum PipeMethod : uint8_t {
	METH_DRIVER_FACTORY,
	METH_DRIVER_INIT,
	METH_DRIVER_RUNFRAME,

	METH_GET_INTERFACE,

	METH_RES_LOAD,
	METH_RES_PATH,

	METH_SETS_GBOOL,
	METH_SETS_GINT,
	METH_SETS_GSTR,

	METH_PATH_READ,
	METH_PATH_WRITE,
	METH_PATH_S2H,

	METH_PROP_READ,
	METH_PROP_WRITE,
	METH_PROP_TRANS,

	METH_SERVER_DEVADD,
	METH_SERVER_POSE,
	METH_SERVER_POLL,

	METH_DEV_ACTIVATE,
	METH_DEV_COMPONENT,

	METH_COMP_DISTORTION,
	METH_COMP_TARGETSIZE,
	METH_COMP_PROJRAW,
	METH_COMP_WINSIZE,
	METH_COMP_EYEVIEWPORT,
	METH_COMP_ONDESKTOP,
	METH_COMP_REALDISPLAY,

	METH_DIRECT_CSWAP,
	METH_DIRECT_NEXT,
	METH_DIRECT_SUBMIT,
	METH_DIRECT_PRESENT,
	METH_DIRECT_POSTPRES,
	METH_DIRECT_FTIME,

	METH_INPUT_CBOOL,
	METH_INPUT_UBOOL,

	METH_MB_UNDOC1,
	METH_MB_UNDOC4,

	METH_PROTO_RET,
};

typedef void (*Handler)(enum PipeMethod, void* userdata);

struct WaitSlot {
	ssize_t next = -1;
	std::unique_ptr<std::mutex> lock = std::make_unique<std::mutex>();
	std::unique_ptr<std::condition_variable> cond = std::make_unique<std::condition_variable>();
	bool triggered = false;
};

struct Thread {
	shim::thread thread;

	enum PipeMethod method;

	Thread() {};
	Thread (const Thread&) = delete;
	Thread& operator= (const Thread&) = delete;

	std::unique_ptr<std::mutex> lock = std::make_unique<std::mutex>();
	std::unique_ptr<std::condition_variable> cond = std::make_unique<std::condition_variable>();
	bool active = false;
};

class Pipe {
	size_t allocate_task();

public:
	FILE *read;
	FILE *write;
	FILE *log = nullptr;

	std::mutex writeLock;
	std::mutex readLock;

	std::vector<struct WaitSlot> wait;
	ssize_t waitHead;

	Handler handler;
	std::map<std::thread::id, struct Thread> threads;

	std::vector<void *> objs;

	size_t currentTask;

	// Eventually
	// public:
	Pipe() {};
	Pipe(bool crossover, Handler);

	void _reinit(bool crossover, Handler);

	void msg(const char *format, ...);

	// Recv Thread

	// This also gives you the read channel, such that you can read the
	// arguments
	void dispatch_requests(void *userdata);

	// We are done reading
	size_t complete_reading_args();

	// We have processed the call and want to write some values. We still own the
	// read channel
	void return_from_call(size_t taskId);

	// Others

	// Begin a call to a remote method. Takes ownership over the write channel
	void begin_call(enum PipeMethod m);

	// Signals the end of the arguments. This returns with the return values ready
	// to be read on the read channel, which we borrow from the Recv Thread.
	void wait_for_return();

	// Returns the read channel to the Recv Thread
	void return_read_channel();

	// Generic send and recieve methods. Requires you to own the respective
	// channel
	void send(const void* buf, size_t len);
	void recv(void* buf, size_t max_len);

	void send_new_obj(void* obj);

	void send_fd(int fd);
	void recv_fd(int *fd);

  // New interface?
};
