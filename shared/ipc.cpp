#include "ipc.h"

#include <cassert>
#include <cerrno>
#include <cstdarg>
#include <cstdlib>
#include <iostream>
#include <mutex>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <stdint.h>
#include <ftw.h>
#include <unistd.h>

static int removeFiles(const char *pathname, const struct stat *sbuf, int type, struct FTW *ftwb) {
	return remove(pathname);
}

size_t Pipe::allocate_task() {
	if(waitHead == -1) {
		size_t id = wait.size();
		wait.emplace_back();

		return id;
	}

	size_t id = waitHead;
	waitHead = wait[waitHead].next;

	wait[waitHead].next = -1;

	return id;
}

Pipe::Pipe(bool crossover, Handler handler) : waitHead(-1), handler(handler) {
	if(crossover) {
		log = stderr;

		int sock;
		struct sockaddr_un addr = {0};
		sock = socket(AF_UNIX, SOCK_STREAM, 0);
		assert(sock != -1);
		addr.sun_family = AF_UNIX;
		strcpy(addr.sun_path, "/tmp/vrlink/sock");

		int rc = connect(sock, (struct sockaddr*)&addr, sizeof(addr));
		if(rc == -1) {
			perror("Connect failed");
		}
		assert(rc == 0);

		write = fdopen(sock, "w");
		read = fdopen(sock, "r");

		/* read = fopen("/tmp/vrlink/forward", "rb"); */
		setbuf(read, NULL);
		/* assert(!unlink("/tmp/vrlink/forward")); */
		/* write = fopen("/tmp/vrlink/backward", "wb"); */
		setbuf(write, NULL);
		/* assert(!unlink("/tmp/vrlink/backward")); */
	}
}

void Pipe::_reinit(bool crossover, Handler handler) {
	this->waitHead = -1;
	this->handler = handler;
	assert(!crossover);
	if(mkdir("/tmp/vrlink", 0777) != 0) {
		if(errno == EEXIST) {
			/* if(nftw("/tmp/vrlink", removeFiles, 10, FTW_DEPTH|FTW_MOUNT|FTW_PHYS) < 0) { */
			/* 	std::cerr << "Can't cleanup the old files\n"; */
			/* 	abort(); */
			/* } */
		} else {
			std::cerr << "Creating the directory just failed\n";
			std::flush(std::cerr);
		}
	}

	this->log = fopen("/tmp/vrlink/log", "a");
	if(this->log == nullptr) {
		std::cerr << "Ahh, we can't open the log!!!: " << errno << std::endl;
		std::flush(std::cerr);
		abort();
	}
	msg("Hello from driver %d\n", getpid());

	if(mkfifo("/tmp/vrlink/forward", 0777) != 0) {
		msg("Failed to create forward file");
		abort();
	}
	if(mkfifo("/tmp/vrlink/backward", 0777) != 0) {
		msg("Failed to create backward file");
		abort();
	}
	msg("Created fifos\n");

	int sock;
	struct sockaddr_un addr = {0};
	msg("Sock\n");
	msg("Creating socket?\n");
	sock = socket(AF_UNIX, SOCK_STREAM, 0);
	msg("Creating socket?\n");
	assert(sock != -1);
	addr.sun_family = AF_UNIX;
	strcpy(addr.sun_path, "/tmp/vrlink/sock");
	int rc;
	msg("Bind\n");
	rc = bind(sock, (struct sockaddr*)&addr, sizeof(addr));
	if(rc == -1) {
		msg("Failed to bind\n");
		perror("Bind failed");
	}
	assert(rc == 0);
	msg("Listen\n");
	rc = listen(sock, 1);
	if(rc == -1) {
		msg("Failed to listen\n");
		perror("Accept failed");
	}
	assert(rc == 0);
	msg("Accept\n");
	int sock_conn = accept(sock, NULL, NULL);
	if(sock_conn == -1) {
		msg("Failed to accept\n");
		perror("Accept failed");
	}
	assert(sock_conn > 0);
	msg("Connection!\n");

	write = fdopen(sock_conn, "w");
	read = fdopen(sock_conn, "r");

	/* this->write = fopen("/tmp/vrlink/forward", "wb"); */
	setbuf(write, NULL);
	/* this->read = fopen("/tmp/vrlink/backward", "rb"); */
	setbuf(read, NULL);

	msg("Connection established\n");
}

void Pipe::msg(const char *format, ...) {
	va_list argptr;
	va_start(argptr, format);
	vfprintf(log, format, argptr);
	fflush(log);
	va_end(argptr);
}

struct HandlerArgs {
	Pipe *pipe;
	struct Thread *thread;
	void *userdata;
};

static void InternalHandler(void *userdata) {
	struct HandlerArgs *args = (struct HandlerArgs*)userdata;
	while(true) {
		std::unique_lock taskLock(*args->thread->lock);
		args->pipe->msg("Blocking thread %d\n", std::this_thread::get_id());
		args->thread->cond->wait(taskLock, [&] { return args->thread->active; });
		args->pipe->handler(args->thread->method, args->userdata);
		fflush(args->pipe->write);
		args->pipe->msg("Done handling %d\n", args->thread->method);
		args->pipe->writeLock.unlock();
		args->thread->active = false;
	}
}

void Pipe::dispatch_requests(void* userdata) {
	enum PipeMethod method;
	while(true) {
		std::unique_lock chanLock(readLock);
		msg("Waiting for next\n");
		recv(&method, sizeof(enum PipeMethod));
		msg("DBG METHOD %d\n", method);

		if(method == METH_PROTO_RET) {
			size_t taskId;
			recv(&taskId, sizeof(uint64_t));
			msg("Task %d return\n", taskId);

			wait[taskId].triggered = true;
			// Wake up the task to have it read whatever it wants
			wait[taskId].cond->notify_one();
			wait[taskId].cond->wait(chanLock, [&] { return !wait[taskId].triggered; });
			continue;
		}

		std::thread::id remoteId;
		recv(&remoteId, sizeof(std::thread::id));
		msg("Incoming call %d on %d\n", method, remoteId);

		auto [it, inserted] = threads.try_emplace(remoteId);
		struct Thread *thread;
		if(inserted) {
			msg("No existing thread for %d, creating one\n", remoteId);
			thread = &it->second;
			struct HandlerArgs *args = (struct HandlerArgs*)malloc(sizeof(struct HandlerArgs));
			args->pipe = this;
			args->thread = thread;
			args->userdata = userdata;
			thread->thread = shim::thread(&InternalHandler, args);
		} else {
			msg("Using existing thread\n");
			thread = &threads.at(remoteId);
		}

		std::unique_lock taskLock(*thread->lock);
		assert(thread->active == false);
		thread->method = method;
		thread->active = true;
		// Wake up the thread. The thread now owns the readLock.
		thread->cond->notify_all();
		chanLock.release();
	}
}

size_t Pipe::complete_reading_args() {
	msg("Done reading args\n");
	size_t taskId;
	recv(&taskId, sizeof(uint64_t));
	readLock.unlock();
	return taskId;
}

static const enum PipeMethod retValue = METH_PROTO_RET;
void Pipe::return_from_call(size_t taskId) {
	msg("Taking write lock to return %d\n", taskId);
	writeLock.lock();
	msg("Returning to %d\n", taskId);
	send(&retValue, sizeof(enum PipeMethod));
	send(&taskId, sizeof(uint64_t));
}

void Pipe::begin_call(enum PipeMethod method) {
	msg("Call remote %d\n", method);
	writeLock.lock();
	send(&method, sizeof(enum PipeMethod));

	std::thread::id thisId = std::this_thread::get_id();
	send(&thisId, sizeof(std::thread::id));
}

void Pipe::wait_for_return() {
	msg("Waiting for message return\n");
	size_t taskId = allocate_task();

	// Send the taskid to the remote end such that it knows who to return to
	send(&taskId, sizeof(uint64_t));

	fflush(write);
	// We no longer intend to write anything
	writeLock.unlock();

	msg("Wait for return of %d\n", taskId);
	// Wait for our call to return before reading any data
	std::unique_lock taskLock(readLock);
	wait[taskId].cond->wait(taskLock, [&] { return wait[taskId].triggered; });
	msg("Wakeup %d\n", taskId);
	wait[taskId].triggered = false;
	wait[taskId].cond->notify_one();
	currentTask = taskId;
	taskLock.release();
}

void Pipe::return_read_channel() {
	msg("Done reading return values\n");
	readLock.unlock();
}

void Pipe::send(const void *buf, size_t len) {
	if(fwrite(buf, 1, len, write) != len) {
		msg("Write denied\n");
		abort();
	}
}

void Pipe::recv(void *buf, size_t max_len) {
	if(fread(buf, 1, max_len, read) != max_len) {
		msg("Read denied\n");
		abort();
	}
}

void Pipe::send_new_obj(void *obj) {
	size_t handle = objs.size() + 1;
	objs.push_back(obj);
	send(&handle, sizeof(uint64_t));
}

void Pipe::send_fd(int fd) {
	char data = 'x';
    struct iovec iov = {
		.iov_base = &data,
		.iov_len = 1,
	};
	char control[CMSG_SPACE(sizeof (int))];
    struct msghdr msg = {
		.msg_name = NULL,
		.msg_namelen = 0,
		.msg_iov = &iov,
		.msg_iovlen = 1,

		.msg_control = control,
		.msg_controllen = sizeof(control),
	};


	struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msg);
	cmsg->cmsg_len = CMSG_LEN(sizeof (int));
	cmsg->cmsg_level = SOL_SOCKET;
	cmsg->cmsg_type = SCM_RIGHTS;

	*((int *) CMSG_DATA(cmsg)) = fd;

    size_t size = sendmsg(fileno(this->write), &msg, 0);
	assert(size == 1);
}

void Pipe::recv_fd(int *fd) {
	char data;
    struct iovec iov = {
		.iov_base = &data,
		.iov_len = 1,
	};
	char control[CMSG_SPACE(sizeof (int))];
    struct msghdr msg = {
		.msg_name = NULL,
		.msg_namelen = 0,
		.msg_iov = &iov,
		.msg_iovlen = 1,

		.msg_control = control,
		.msg_controllen = sizeof(control),
	};


    size_t size = recvmsg(fileno(this->read), &msg, 0);
	assert(size == 1);

	struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msg);
	assert(cmsg != nullptr);
	assert(cmsg->cmsg_len == CMSG_LEN(sizeof (int)));
	assert(cmsg->cmsg_level == SOL_SOCKET);
	assert(cmsg->cmsg_type == SCM_RIGHTS);

	*fd  = *((int *) CMSG_DATA(cmsg));
}
