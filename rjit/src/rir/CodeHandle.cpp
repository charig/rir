#include "BC.h"

#include "Pool.h"
#include <iostream>
#include <iomanip>

#include "../RList.h"
#include "CodeStream.h"
#include "RIntlns.h"
#include "CodeHandle.h"
#include "FunctionHandle.h"
#include "CodeEditor.h"

namespace rjit {
namespace rir {

void CodeHandle::print() {
    BC_t* pc = (BC_t*)bc();

    unsigned * s = src(code);
    while ((uintptr_t)pc < (uintptr_t)endBc()) {
        if (*s != 0) {
            Rprintf("          # (idx %u) : ", *s);
            Rf_PrintValue(src_pool_at(globalContext(), *s));
        }
        Rprintf(" %5x ", ((uintptr_t)pc - (uintptr_t)bc()));
        BC bc = BC::advance(&pc);
        bc.print();
        ++s;
    }
}

FunctionHandle CodeHandle::function() {
    return (SEXP)((uintptr_t)::function(code) - FUNCTION_OFFSET);
}

fun_idx_t CodeHandle::idx() {
        fun_idx_t i = 0;
        for (auto c : function()) {
            if (c == code)
                return i;
            ++i;
        }
        assert(false);
        return -1;
}

}
}
