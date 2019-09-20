/*
 * Copyright (C) 2014 Paul Cercueil <paul@crapouillou.net>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 */

#ifndef DEBUG_H
#define DEBUG_H

#include <stdio.h>
#include <unistd.h>

#define NOLOG_L 0
#define ERROR_L 1
#define WARNING_L 2
#define INFO_L 3
#define DEBUG_L 4

#ifndef LOG_LEVEL
#define LOG_LEVEL INFO_L
#endif

// -------------

#ifndef COLOR_DEBUG
#define COLOR_DEBUG   "\e[0;32m"
#endif
#ifndef COLOR_WARNING
#define COLOR_WARNING "\e[01;35m"
#endif
#ifndef COLOR_ERROR
#define COLOR_ERROR   "\e[01;31m"
#endif

#define COLOR_END "\e[0m"

#if (LOG_LEVEL >= DEBUG_L)
# ifdef COLOR_DEBUG
#  define DEBUG(str, ...) do {						\
	if (isatty(STDOUT_FILENO))					\
		fprintf(stdout, COLOR_DEBUG "DEBUG: " str COLOR_END,	\
			##__VA_ARGS__);					\
	else								\
		fprintf(stdout, "DEBUG: " str, ##__VA_ARGS__);		\
	} while (0)
# else
#  define DEBUG(...) \
    fprintf(stdout, "DEBUG: " __VA_ARGS__)
# endif
#else
#define DEBUG(...)
#endif

#if (LOG_LEVEL >= INFO_L)
# ifdef COLOR_INFO
#  define INFO(str, ...) \
    fprintf(stdout, COLOR_INFO str COLOR_END, ##__VA_ARGS__)
# else
#  define INFO(...) \
    fprintf(stdout, __VA_ARGS__)
# endif
#else
#define INFO(...)
#endif

#if (LOG_LEVEL >= WARNING_L)
# ifdef COLOR_WARNING
#  define WARNING(str, ...) do {					\
	if (isatty(STDERR_FILENO))					\
		fprintf(stderr, COLOR_WARNING "WARNING: " str COLOR_END,\
			##__VA_ARGS__);					\
	else								\
		fprintf(stderr, "WARNING: " str, ##__VA_ARGS__);	\
	} while (0)
# else
#  define WARNING(...) \
    fprintf(stderr, "WARNING: " __VA_ARGS__)
# endif
#else
#define WARNING(...)
#endif

#if (LOG_LEVEL >= ERROR_L)
# ifdef COLOR_ERROR
#  define ERROR(str, ...) do {						\
	if (isatty(STDERR_FILENO))					\
		fprintf(stderr, COLOR_ERROR "ERROR: " str COLOR_END,	\
			##__VA_ARGS__);					\
	else								\
		fprintf(stderr, "ERROR: " str, ##__VA_ARGS__);		\
	} while (0)
# else
#  define ERROR(...) \
    fprintf(stderr, "ERROR: " __VA_ARGS__)
# endif
#else
#define ERROR(...)
#endif

#endif
