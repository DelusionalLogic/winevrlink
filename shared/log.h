#pragma once

#ifndef LOG_DEBUG
#define LOG(msg_fn, str, ...)
#define TRACE(str, ...)
#else
#define LOG(msg_fn, str, ...) \
	msg_fn(str __VA_OPT__(,) __VA_ARGS__)
#endif
