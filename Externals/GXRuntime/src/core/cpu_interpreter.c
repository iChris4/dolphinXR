#include "cpu_interpreter_private.h"

void ppc_memory_fence(void) {
#if defined(_MSC_VER)
    _ReadWriteBarrier();
#if defined(_M_IX86) || defined(_M_X64)
    _mm_mfence();
#endif
    _ReadWriteBarrier();
#else
    atomic_thread_fence(memory_order_seq_cst);
#endif
}
