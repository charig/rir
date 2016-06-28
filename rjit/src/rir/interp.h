#ifndef RIR_INTERPRETER_C_H
#define RIR_INTERPRETER_C_H

#include <R.h>
#include <Rinternals.h>

#undef length

#include <stdint.h>

#include "interp_context.h"

// If 1, when a function that has not yet been compiled by rir is to be called in the interpreter, it will be compiled first.
// Set to 0 if rir should handle the execution to GNU-R.
#define COMPILE_ON_DEMAND 1


#ifdef __cplusplus
extern "C" {
#endif

/** How many bytes do we need to align on 4 byte boundary?
 */
INLINE unsigned pad4(unsigned sizeInBytes) {
    unsigned x = sizeInBytes % 4;
    return (x != 0) ? (sizeInBytes + 4 - x) : sizeInBytes;
}



// we cannot use specific sizes for enums in C
typedef uint8_t OpcodeT;

/** Any argument to BC must be the size of this type. */
// TODO add static asserts somewhere in C++
typedef uint32_t ArgT;

// type  for constant & ast pool indices
typedef uint32_t Immediate;

// type  signed immediate values (unboxed ints)
typedef uint32_t SignedImmediate;

// type of relative jump offset (all jumps are relative)
typedef int32_t JumpOffset;

typedef unsigned FunctionIndex;
typedef unsigned ArgumentsCount;

// enums in C are not namespaces so I am using OP_ to disambiguate
typedef enum {
#define DEF_INSTR(name, ...) name,
#include "insns.h"
    numInsns_
} Opcode;

/**
 * Aliases for readability.
 */
typedef SEXP FunctionSEXP;
typedef SEXP ClosureSEXP;
typedef SEXP PromiseSEXP;
typedef SEXP IntSEXP;

// type of relative jump offset (all jumps are relative)
typedef int32_t JumpOffset;

struct Function; // Forward declaration


// all sizes in bytes,
// length in element sizes

// Function magic constant is designed to help to distinguish between Function objects and normal INTSXPs. Normally this is not necessary, but a very creative user might try to assign arbitrary INTSXP to a closure which we would like to spot. Of course, such a creative user might actually put the magic in his vector too...
#define FUNCTION_MAGIC (unsigned)0xCAFEBABE

// Code magic constant is intended to trick the GC into believing that it is dealing with already marked SEXP.
//  It also makes the SEXP look like NILSXP (0x00) so that we can determine whether a standard promise execution, or rir promise should be executed.
#define CODE_MAGIC (unsigned)0x00ff

// Missing argument offset.
// The offset is 0 (this would be impossible).
// TODO This changes from old where it was some other number
#define MISSING_ARG_OFFSET  (unsigned)0


/**
 * Code holds a sequence of instructions; for each instruction
 * it records the index of the source AST. Code is part of a
 * Function.
 *
 * Code objects are allocated contiguously within the data
 * section of a Function. The Function header can be found,
 * at an offset from the start of each Code object
 *
 * Instructions are variable size; Code knows how many bytes
 * are required for instructions.
 *
 * The number of indices of source ASTs stored in Code equals
 * the number of instructions.
 *
 * Instructions and AST indices are allocated one after the
 * other in the Code's data section with padding to ensure
 * alignment of indices.
 */
#pragma pack(push)
#pragma pack(1)
typedef struct Code {
    unsigned magic; ///< Magic number that attempts to be PROMSXP already marked by the GC

    unsigned header; /// offset to Function object

    // TODO comment these
    unsigned src; /// AST of the function (or promise) represented by the code

    unsigned stackLength; /// Number of slots in stack required

    unsigned iStackLength; /// Number of slots in the integer stack required

    unsigned codeSize; /// bytes of code (not padded)

    unsigned srcLength; /// number of instructions

    uint8_t data[]; /// the instructions
} Code;
#pragma pack(pop)


/** Returns a pointer to the instructions in c.  */
INLINE OpcodeT* code(Code* c) {
    return (OpcodeT*)c->data;
}

/** Returns a pointer to the source AST indices in c.  */
INLINE unsigned* src(Code* c) {
    return (unsigned*)(c->data + pad4(c->codeSize));
}

/** Returns a pointer to the Function to which c belongs. */
INLINE struct Function* function(Code* c) {
    return (struct Function*)((uint8_t*)c - c->header);
}

/** Returns the next Code in the current function. */
INLINE Code* next(Code* c) {
    return (Code*)(c->data + pad4(c->codeSize) + c->srcLength * sizeof(unsigned));
}

// TODO removed src reference, now each code has its own

/** A Function holds the RIR code for some GNU R function.
 *  Each function start with a header and a sequence of
 *  Code objects for the body and all of the promises
 *  in the code.
 *
 *  The header start with a magic constant. This is a
 *  temporary hack so that it is possible to differentiate
 *  an R int vector from a Function. Eventually, we will
 *  add a new SEXP type for this purpose.
 *
 *  The size of the function, in bytes, includes the size
 *  of all of its Code objects and is padded to a word
 *  boundary.
 *
 *  A Function may be the result of optimizing another
 *  Function, in which case the origin field stores that
 *  Function as a SEXP pointer.
 *
 *  A Function has a source AST, stored in src.
 *
 *  A Function has a number of Code objects, codeLen, stored
 *  inline in data.
 */
#pragma pack(push)
#pragma pack(1)
struct Function {
    unsigned magic; /// used to detect Functions 0xCAFEBABE

    unsigned size; /// Size, in bytes, of the function and its data

    FunctionSEXP origin; /// Same Function with fewer optimizations, NULL if original

    unsigned codeLength; /// number of Code objects in the Function

    uint8_t data[]; // Code objects stored inline

};
#pragma pack(pop)

typedef struct Function Function;

INLINE bool isValidFunction(SEXP s) {
    if (TYPEOF(s) != INTSXP)
        return false;
    return (unsigned)INTEGER(s)[0] == FUNCTION_MAGIC;
}

/** Returns the first code object associated with the function.
 */
INLINE Code* begin(Function* f) {
    return (Code*)f->data;
}

/** Returns the end of the function as code object, for interation purposes.
 */
INLINE Code* end(Function* f) {
    return (Code*)((uint8_t*)f + f->size);
}

/** Returns the code object with given offset */
INLINE Code * codeAt(Function * f, unsigned offset) {
    return (Code*)((uint8_t*)f + offset);
}

/** C implementation of the Precious class to protect 
    the elements of the ast and constant pool from being
    gc'ed */
// void poolGcCallBack(void (*forward_node)(SEXP));
// void poolAdd(SEXP value);
// void poolRemove(SEXP value);


SEXP rirEval_c(Code* c, Context* ctx, SEXP env, unsigned numArgs);

#ifdef __cplusplus
}
#endif

#endif // RIR_INTERPRETER_C_H
