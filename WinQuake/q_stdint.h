/*
 * q_stdint.h — fixed-width types for LP64-clean Quake
 *
 * Prefer these over bare `long` (8 bytes on Linux LP64, 4 on Win32 LLP64).
 * QuakeC / progs.dat values stay 32-bit; host pointers use intptr_t.
 */
#ifndef Q_STDINT_H
#define Q_STDINT_H

#include <stdint.h>
#include <stddef.h>

/* Progs / network / file format sizes (always 32-bit on disk and in VM). */
typedef int32_t		qint32_t;
typedef uint32_t	quint32_t;
typedef int16_t		qint16_t;
typedef uint16_t	quint16_t;
typedef int8_t		qint8_t;
typedef uint8_t		quint8_t;

/* Host pointer-sized integers for address math (never store in progs fields). */
typedef intptr_t	qintptr_t;
typedef uintptr_t	quintptr_t;
typedef ptrdiff_t	qptrdiff_t;
typedef size_t		qsize_t;

/* Explicit cast helpers for pointer ↔ 32-bit progs offsets. */
#define Q_PTR_TO_INT32(p)	((int32_t)(intptr_t)(p))
#define Q_INT32_TO_PTR(t, i)	((t)(intptr_t)(int32_t)(i))

#endif /* Q_STDINT_H */
