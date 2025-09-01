#pragma once

/**
 * @brief Convert a relative pointer to a virtual address
 *
 * A relative pointer P points to the virtual address (&P + P).
 * If P is NULL, it points to NULL as well.
 *
 * @param OFFSET The relative pointer to convert
 * @return Virtual address where OFFSET points to
 */
#define ADDR_OF(OFFSET)                                                        \
	({                                                                     \
		typeof(*(OFFSET)) _offset_val = *(OFFSET);                     \
		(typeof(*OFFSET))((uintptr_t)_offset_val +                     \
				  (uintptr_t)((_offset_val) ? (OFFSET) : NULL) \
		);                                                             \
	})

/**
 * @brief Set a relative pointer to point to a virtual address
 *
 * This macro sets a relative pointer to point to a specified virtual address.
 *
 * @param PTR Pointer to the relative pointer to be set
 * @param ADDR Virtual address that the relative pointer should point to
 *
 * @note After this macro is called, it is guaranteed that ADDR_OF(PTR) == ADDR.
 */
#define SET_OFFSET_OF(PTR, ADDR)                                               \
	do {                                                                   \
		typeof(ADDR) _addr_val = (ADDR);                               \
		typeof(PTR) _ptr_val = (PTR);                                  \
		*_ptr_val = ((typeof(ADDR))((uintptr_t)_addr_val -             \
					    (uintptr_t)((_addr_val) ? _ptr_val \
								    : NULL))); \
	} while (0)


/**
 * @brief Assign one relative pointer to another
 *
 * This macro makes an assignment PTR1 = PTR2 in the sense of relative pointers.
 *
 * @param PTR1 Pointer to the destination relative pointer
 * @param PTR2 Pointer to the source relative pointer
 *
 * @note After this macro is called, it is guaranteed that ADDR_OF(PTR1) ==
 * ADDR_OF(PTR2).
 */
#define EQUATE_OFFSET(PTR1, PTR2)                                              \
	do {                                                                   \
		SET_OFFSET_OF(PTR1, ADDR_OF(PTR2));                            \
	} while (0)
