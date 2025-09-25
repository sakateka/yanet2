#pragma once

// Relative pointer P points to the virtual address (&P + P)
// If P is NULL, it points to NULL also.

// Here OFFSET is a relative pointer.
// This macros returns virtual address where OFFSET points to,
// which equals to (&OFFSET + OFFSET).
#define ADDR_OF(OFFSET)                                                        \
	((typeof(*OFFSET))((uintptr_t)*(OFFSET) +                              \
			   (uintptr_t)((*(OFFSET)) ? (OFFSET) : NULL)))

// Here PTR is a pointer on relative pointer (for some type T, typeof(PTR) == T
// **), and ADDR is virtual address. This macros makes relative pointer *PTR to
// point on virtual address ADDR. After this macros is called, it is guaranted
// ADDR_OF(PTR) == ADDR.
#define SET_OFFSET_OF(PTR, ADDR)                                               \
	do {                                                                   \
		*(PTR) = ((typeof(ADDR))((uintptr_t)(ADDR) -                   \
					 (uintptr_t)((ADDR) ? (PTR) : NULL))); \
	} while (0)

// Here PTR1 and PTR2 are relative pointers.
// This macros makes assignment PTR1 = PTR2 in sense of relative pointers.
// After this macros called, it is guarated ADDR_OR(PTR1) == ADDR_OF(PTR2).
#define EQUATE_OFFSET(PTR1, PTR2)                                              \
	do {                                                                   \
		typeof(*PTR2) addr = ADDR_OF(PTR2);                            \
		SET_OFFSET_OF(PTR1, addr);                                     \
	} while (0)

#define ATOMIC_ADDR_OF(OFFSET)                                                 \
	__extension__({                                                        \
		typeof(*OFFSET) _offset = atomic_load_explicit(                \
			(_Atomic(typeof(*OFFSET)) *)OFFSET,                    \
			memory_order_acquire                                   \
		);                                                             \
		(typeof(_offset))((uintptr_t)_offset +                         \
				  (uintptr_t)(_offset ? (OFFSET) : NULL));     \
	})

#define ATOMIC_SET_OFFSET_OF(PTR, ADDR)                                        \
	atomic_store_explicit(                                                 \
		(_Atomic(typeof(*PTR)) *)PTR,                                  \
		(typeof(ADDR))((uintptr_t)(ADDR) -                             \
			       (uintptr_t)((ADDR) ? (PTR) : NULL)),            \
		memory_order_release                                           \
	)
