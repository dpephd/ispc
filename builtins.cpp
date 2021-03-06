/*
  Copyright (c) 2010-2011, Intel Corporation
  All rights reserved.

  Redistribution and use in source and binary forms, with or without
  modification, are permitted provided that the following conditions are
  met:

    * Redistributions of source code must retain the above copyright
      notice, this list of conditions and the following disclaimer.

    * Redistributions in binary form must reproduce the above copyright
      notice, this list of conditions and the following disclaimer in the
      documentation and/or other materials provided with the distribution.

    * Neither the name of Intel Corporation nor the names of its
      contributors may be used to endorse or promote products derived from
      this software without specific prior written permission.


   THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS
   IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED
   TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A
   PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER
   OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
   EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
   PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
   PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
   LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
   NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
   SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.  
*/

/** @file builtins.cpp
    @brief Definitions of functions related to setting up the standard library 
           and other builtins.
*/

#include "builtins.h"
#include "type.h"
#include "util.h"
#include "sym.h"
#include "expr.h"
#include "llvmutil.h"
#include "module.h"
#include "ctx.h"

#include <math.h>
#include <stdlib.h>
#include <llvm/LLVMContext.h>
#include <llvm/Module.h>
#include <llvm/Type.h>
#include <llvm/DerivedTypes.h>
#include <llvm/Instructions.h>
#include <llvm/Linker.h>
#include <llvm/Support/MemoryBuffer.h>
#include <llvm/Bitcode/ReaderWriter.h>

extern int yyparse();
struct yy_buffer_state;
extern yy_buffer_state *yy_scan_string(const char *);


/** Given an LLVM type, try to find the equivalent ispc type.  Note that
    this is an under-constrained problem due to LLVM's type representations
    carrying less information than ispc's.  (For example, LLVM doesn't
    distinguish between signed and unsigned integers in its types.)  

    However, because this function is only used for generating ispc
    declarations of functions defined in LLVM bitcode in the stdlib-*.ll
    files, in practice we can get enough of what we need for the relevant
    cases to make things work.
 */
static const Type *
lLLVMTypeToISPCType(const llvm::Type *t) {
    if (t == LLVMTypes::VoidType)
        return AtomicType::Void;
    else if (t == LLVMTypes::BoolType)
        return AtomicType::UniformBool;
    else if (t == LLVMTypes::Int32Type)
        return AtomicType::UniformInt32;
    else if (t == LLVMTypes::FloatType)
        return AtomicType::UniformFloat;
    else if (t == LLVMTypes::DoubleType)
        return AtomicType::UniformDouble;
    else if (t == LLVMTypes::Int64Type)
        return AtomicType::UniformInt64;
    else if (t == LLVMTypes::Int32VectorType)
        return AtomicType::VaryingInt32;
    else if (t == LLVMTypes::FloatVectorType)
        return AtomicType::VaryingFloat;
    else if (t == LLVMTypes::DoubleVectorType)
        return AtomicType::VaryingDouble;
    else if (t == LLVMTypes::Int64VectorType)
        return AtomicType::VaryingInt64;
    else if (t == LLVMTypes::Int32PointerType)
        return new ReferenceType(AtomicType::UniformInt32, false);
    else if (t == LLVMTypes::FloatPointerType)
        return new ReferenceType(AtomicType::UniformFloat, false);
    else if (t == LLVMTypes::Int32VectorPointerType)
        return new ReferenceType(AtomicType::VaryingInt32, false);
    else if (t == LLVMTypes::FloatVectorPointerType)
        return new ReferenceType(AtomicType::VaryingFloat, false);
    else if (llvm::isa<const llvm::PointerType>(t)) {
        const llvm::PointerType *pt = llvm::dyn_cast<const llvm::PointerType>(t);

        // Is it a pointer to an unsized array of objects?  If so, then
        // create the equivalent ispc type.  Note that it has to be a
        // reference to an array, since ispc passes arrays to functions by
        // reference.
        //
        // FIXME: generalize this to do more than uniform int32s (that's
        // all that's necessary for the stdlib currently.)
        const llvm::ArrayType *at = 
            llvm::dyn_cast<const llvm::ArrayType>(pt->getElementType());
        if (at && at->getNumElements() == 0 &&
            at->getElementType() == LLVMTypes::Int32Type)
            return new ReferenceType(new ArrayType(AtomicType::UniformInt32, 0),
                                     false);
    }

    return NULL;
}


/** Given an LLVM function declaration, synthesize the equivalent ispc
    symbol for the function (if possible).  Returns true on success, false
    on failure.
 */
static bool
lCreateISPCSymbol(llvm::Function *func, SymbolTable *symbolTable) {
    SourcePos noPos;
    noPos.name = "__stdlib";

    const llvm::FunctionType *ftype = func->getFunctionType();
    std::string name = func->getName();

    const Type *returnType = lLLVMTypeToISPCType(ftype->getReturnType());
    if (!returnType)
        // return type not representable in ispc -> not callable from ispc
        return false;

    // Iterate over the arguments and try to find their equivalent ispc
    // types.
    std::vector<const Type *> argTypes;
    for (unsigned int i = 0; i < ftype->getNumParams(); ++i) {
        const llvm::Type *llvmArgType = ftype->getParamType(i);
        const Type *type = lLLVMTypeToISPCType(llvmArgType);
        if (type == NULL)
            return false;
        argTypes.push_back(type);
    }

    FunctionType *funcType = new FunctionType(returnType, argTypes, noPos);
    Symbol *sym = new Symbol(name, noPos, funcType);
    sym->function = func;
    symbolTable->AddFunction(sym);
    return true;
}


/** Given an LLVM module, create ispc symbols for the functions in the
    module.
 */
static void
lAddModuleSymbols(llvm::Module *module, SymbolTable *symbolTable) {
#if 0
    // FIXME: handle globals?
    assert(module->global_empty());
#endif

    llvm::Module::iterator iter;
    for (iter = module->begin(); iter != module->end(); ++iter) {
        llvm::Function *func = iter;
        lCreateISPCSymbol(func, symbolTable);
    }
}

/** Declare the function symbol 'bool __is_compile_time_constant_mask(mask type)'.  
    This function will never be defined; it's just a placeholder
    that will be handled during the optimization process.  See the
    discussion of the implementation of CompileTimeConstantResolvePass for
    more details.
 */
static void
lDeclareCompileTimeConstant(llvm::Module *module) {
    SourcePos noPos;
    noPos.name = "__stdlib";

    std::vector<const llvm::Type *> argTypes;
    argTypes.push_back(LLVMTypes::MaskType);

    llvm::FunctionType *fType = 
        llvm::FunctionType::get(LLVMTypes::BoolType, argTypes, false);
    llvm::Function *func =
        llvm::Function::Create(fType, llvm::GlobalValue::ExternalLinkage,
                               "__is_compile_time_constant_mask", module);
    func->setOnlyReadsMemory(true);
    func->setDoesNotThrow(true);
}


/** Declare the 'pseudo-gather' functions.  When the ispc front-end needs
    to perform a gather, it generates a call to one of these functions,
    which have signatures:
    
    varying int32 __pseudo_gather(varying int32 *, mask)
    varying int64 __pseudo_gather(varying int64 *, mask)

    These functions are never actually implemented; the
    GatherScatterFlattenOpt optimization pass finds them and then converts
    them to make calls to the following functions, which represent gathers
    from a common base pointer with offsets.  This approach allows the
    front-end to be relatively simple in how it emits address calculation
    for gathers.

    varying int32 __pseudo_gather_base_offsets_32(uniform int32 *base, 
                                                  int32 offsets, mask)
    varying int64 __pseudo_gather_base_offsets_64(uniform int64 *base, 
                                                  int64 offsets, mask)

    Then, the GSImprovementsPass optimizations finds these and either
    converts them to native gather functions or converts them to vector
    loads, if equivalent.
 */
static void
lDeclarePseudoGathers(llvm::Module *module) {
    SourcePos noPos;
    noPos.name = "__stdlib";

    {
        std::vector<const llvm::Type *> argTypes;
        argTypes.push_back(LLVMTypes::VoidPointerVectorType);
        argTypes.push_back(LLVMTypes::MaskType);

        llvm::FunctionType *fType = 
            llvm::FunctionType::get(LLVMTypes::Int32VectorType, argTypes, false);
        llvm::Function *func =
            llvm::Function::Create(fType, llvm::GlobalValue::ExternalLinkage,
                                   "__pseudo_gather_32", module);
        func->setOnlyReadsMemory(true);
        func->setDoesNotThrow(true);

        fType = llvm::FunctionType::get(LLVMTypes::Int64VectorType, argTypes, false);
        func = llvm::Function::Create(fType, llvm::GlobalValue::ExternalLinkage,
                                      "__pseudo_gather_64", module);
        func->setOnlyReadsMemory(true);
        func->setDoesNotThrow(true);
    }

    {
        std::vector<const llvm::Type *> argTypes;
        argTypes.push_back(LLVMTypes::VoidPointerType);
        argTypes.push_back(LLVMTypes::Int32VectorType);
        argTypes.push_back(LLVMTypes::MaskType);

        llvm::FunctionType *fType = 
            llvm::FunctionType::get(LLVMTypes::Int32VectorType, argTypes, false);
        llvm::Function *func =
            llvm::Function::Create(fType, llvm::GlobalValue::ExternalLinkage,
                                   "__pseudo_gather_base_offsets_32", module);
        func->setOnlyReadsMemory(true);
        func->setDoesNotThrow(true);

        fType = llvm::FunctionType::get(LLVMTypes::Int64VectorType, argTypes, false);
        func = llvm::Function::Create(fType, llvm::GlobalValue::ExternalLinkage,
                                      "__pseudo_gather_base_offsets_64", module);
        func->setOnlyReadsMemory(true);
        func->setDoesNotThrow(true);
    }
}


/** Similarly to the 'pseudo-gathers' defined by lDeclarePseudoGathers(),
    we also declare (but never define) pseudo-scatter instructions with
    signatures:

    void __pseudo_scatter_32(varying int32 *, varying int32 values, mask)
    void __pseudo_scatter_64(varying int64 *, varying int64 values, mask)

    The GatherScatterFlattenOpt optimization pass also finds these and
    transforms them to scatters like:

    void __pseudo_scatter_base_offsets_32(uniform int32 *base, 
                    varying int32 offsets, varying int32 values, mask)
    void __pseudo_scatter_base_offsets_64(uniform int64 *base, 
                    varying int62 offsets, varying int64 values, mask)

    And the GSImprovementsPass in turn converts these to actual native
    scatters or masked stores.  
*/
static void
lDeclarePseudoScatters(llvm::Module *module) {
    SourcePos noPos;
    noPos.name = "__stdlib";

    {
        std::vector<const llvm::Type *> argTypes;
        argTypes.push_back(LLVMTypes::VoidPointerVectorType);
        argTypes.push_back(LLVMTypes::Int32VectorType);
        argTypes.push_back(LLVMTypes::MaskType);

        llvm::FunctionType *fType = 
            llvm::FunctionType::get(LLVMTypes::VoidType, argTypes, false);
        llvm::Function *func =
            llvm::Function::Create(fType, llvm::GlobalValue::ExternalLinkage,
                                   "__pseudo_scatter_32", module);
        func->setDoesNotThrow(true);
    }
    {
        std::vector<const llvm::Type *> argTypes;
        argTypes.push_back(LLVMTypes::VoidPointerVectorType);
        argTypes.push_back(LLVMTypes::Int64VectorType);
        argTypes.push_back(LLVMTypes::MaskType);

        llvm::FunctionType *fType = 
            llvm::FunctionType::get(LLVMTypes::VoidType, argTypes, false);
        llvm::Function *func =
            llvm::Function::Create(fType, llvm::GlobalValue::ExternalLinkage,
                                   "__pseudo_scatter_64", module);
        func->setDoesNotThrow(true);
    }

    {
        std::vector<const llvm::Type *> argTypes;
        argTypes.push_back(LLVMTypes::VoidPointerType);
        argTypes.push_back(LLVMTypes::Int32VectorType);
        argTypes.push_back(LLVMTypes::Int32VectorType);
        argTypes.push_back(LLVMTypes::MaskType);

        llvm::FunctionType *fType = 
            llvm::FunctionType::get(LLVMTypes::VoidType, argTypes, false);
        llvm::Function *func =
            llvm::Function::Create(fType, llvm::GlobalValue::ExternalLinkage,
                                   "__pseudo_scatter_base_offsets_32", module);
        func->setDoesNotThrow(true);
    }
    {
        std::vector<const llvm::Type *> argTypes;
        argTypes.push_back(LLVMTypes::VoidPointerType);
        argTypes.push_back(LLVMTypes::Int32VectorType);
        argTypes.push_back(LLVMTypes::Int64VectorType);
        argTypes.push_back(LLVMTypes::MaskType);

        llvm::FunctionType *fType = 
            llvm::FunctionType::get(LLVMTypes::VoidType, argTypes, false);
        llvm::Function *func =
            llvm::Function::Create(fType, llvm::GlobalValue::ExternalLinkage,
                                   "__pseudo_scatter_base_offsets_64", module);
        func->setDoesNotThrow(true);
    }
}


/** This function declares placeholder masked store functions for the
    front-end to use.

    void __pseudo_masked_store_32(uniform int32 *ptr, varying int32 values, mask)
    void __pseudo_masked_store_64(uniform int64 *ptr, varying int64 values, mask)

    These in turn are converted to native masked stores or to regular
    stores (if the mask is all on) by the MaskedStoreOptPass optimization
    pass.
 */
static void
lDeclarePseudoMaskedStore(llvm::Module *module) {
    SourcePos noPos;
    noPos.name = "__stdlib";

    {
    std::vector<const llvm::Type *> argTypes;
    argTypes.push_back(LLVMTypes::Int32VectorPointerType);
    argTypes.push_back(LLVMTypes::Int32VectorType);
    argTypes.push_back(LLVMTypes::MaskType);

    llvm::FunctionType *fType = 
        llvm::FunctionType::get(LLVMTypes::VoidType, argTypes, false);
    llvm::Function *func = 
        llvm::Function::Create(fType, llvm::GlobalValue::ExternalLinkage,
                               "__pseudo_masked_store_32", module);
    func->setDoesNotThrow(true);
    func->addFnAttr(llvm::Attribute::AlwaysInline);
    func->setDoesNotCapture(1, true);
    }

    {
    std::vector<const llvm::Type *> argTypes;
    argTypes.push_back(LLVMTypes::Int64VectorPointerType);
    argTypes.push_back(LLVMTypes::Int64VectorType);
    argTypes.push_back(LLVMTypes::MaskType);

    llvm::FunctionType *fType = 
        llvm::FunctionType::get(LLVMTypes::VoidType, argTypes, false);
    llvm::Function *func = 
        llvm::Function::Create(fType, llvm::GlobalValue::ExternalLinkage,
                               "__pseudo_masked_store_64", module);
    func->setDoesNotThrow(true);
    func->addFnAttr(llvm::Attribute::AlwaysInline);
    func->setDoesNotCapture(1, true);
    }
}


/** This utility function takes serialized binary LLVM bitcode and adds its
    definitions to the given module.  Functions in the bitcode that can be
    mapped to ispc functions are also added to the symbol table.

    @param bitcode     Binary LLVM bitcode (e.g. the contents of a *.bc file)
    @param length      Length of the bitcode buffer
    @param module      Module to link the bitcode into
    @param symbolTable Symbol table to add definitions to
 */
static void
lAddBitcode(const unsigned char *bitcode, int length,
            llvm::Module *module, SymbolTable *symbolTable) {
    std::string bcErr;
    llvm::StringRef sb = llvm::StringRef((char *)bitcode, length);
    llvm::MemoryBuffer *bcBuf = llvm::MemoryBuffer::getMemBuffer(sb);
    llvm::Module *bcModule = llvm::ParseBitcodeFile(bcBuf, *g->ctx, &bcErr);
    if (!bcModule)
        Error(SourcePos(), "Error parsing stdlib bitcode: %s", bcErr.c_str());
    else {
        std::string(linkError);
        if (llvm::Linker::LinkModules(module, bcModule, &linkError))
            Error(SourcePos(), "Error linking stdlib bitcode: %s", linkError.c_str());
        lAddModuleSymbols(module, symbolTable);
    }
}


/** Utility routine that defines a constant int32 with given value, adding
    the symbol to both the ispc symbol table and the given LLVM module.
 */
static void
lDefineConstantInt(const char *name, int val, llvm::Module *module,
                   SymbolTable *symbolTable) {
    Symbol *pw = new Symbol(name, SourcePos(), AtomicType::UniformConstInt32);
    pw->isStatic = true;
    pw->constValue = new ConstExpr(pw->type, val, SourcePos());
    const llvm::Type *ltype = LLVMTypes::Int32Type;
    llvm::Constant *linit = LLVMInt32(val);
    pw->storagePtr = new llvm::GlobalVariable(*module, ltype, true, 
                                              llvm::GlobalValue::InternalLinkage,
                                              linit, pw->name.c_str());
    symbolTable->AddVariable(pw);
}


static void
lDefineProgramIndex(llvm::Module *module, SymbolTable *symbolTable) {
    Symbol *pidx = new Symbol("programIndex", SourcePos(), 
                              AtomicType::VaryingConstInt32);
    pidx->isStatic = true;

    int pi[ISPC_MAX_NVEC];
    for (int i = 0; i < g->target.vectorWidth; ++i)
        pi[i] = i;
    pidx->constValue = new ConstExpr(pidx->type, pi, SourcePos());

    const llvm::Type *ltype = LLVMTypes::Int32VectorType;
    llvm::Constant *linit = LLVMInt32Vector(pi);
    pidx->storagePtr = new llvm::GlobalVariable(*module, ltype, true, 
                                                llvm::GlobalValue::InternalLinkage, linit, 
                                                pidx->name.c_str());
    symbolTable->AddVariable(pidx);
}


void
DefineStdlib(SymbolTable *symbolTable, llvm::LLVMContext *ctx, llvm::Module *module,
             bool includeStdlibISPC) {
    // Add the definitions from the compiled stdlib-c.c file
    extern unsigned char stdlib_bitcode_c[];
    extern int stdlib_bitcode_c_length;
    lAddBitcode(stdlib_bitcode_c, stdlib_bitcode_c_length, module, symbolTable);

    // Next, add the target's custom implementations of the various needed
    // builtin functions (e.g. __masked_store_32(), etc).
    switch (g->target.isa) {
    case Target::SSE2:
        extern unsigned char stdlib_bitcode_sse2[];
        extern int stdlib_bitcode_sse2_length;
        lAddBitcode(stdlib_bitcode_sse2, stdlib_bitcode_sse2_length, module,
                    symbolTable);
        break;
    case Target::SSE4:
        extern unsigned char stdlib_bitcode_sse4[];
        extern int stdlib_bitcode_sse4_length;
        extern unsigned char stdlib_bitcode_sse4x2[];
        extern int stdlib_bitcode_sse4x2_length;
        switch (g->target.vectorWidth) {
        case 4: 
            lAddBitcode(stdlib_bitcode_sse4, stdlib_bitcode_sse4_length, 
                        module, symbolTable);
            break;
        case 8:
            lAddBitcode(stdlib_bitcode_sse4x2, stdlib_bitcode_sse4x2_length, 
                        module, symbolTable);
            break;
        default:
            FATAL("logic error in DefineStdlib");
        }
        break;
    case Target::AVX:
        extern unsigned char stdlib_bitcode_avx[];
        extern int stdlib_bitcode_avx_length;
        lAddBitcode(stdlib_bitcode_avx, stdlib_bitcode_avx_length, module, 
                    symbolTable);
        break;
    default:
        FATAL("logic error");
    }

    // Add a declaration of void *ISPCMalloc(int64_t).  The user is
    // responsible for linking in a definition of this if it's needed by
    // the compiled program.
    { std::vector<const llvm::Type *> argTypes;
        argTypes.push_back(llvm::Type::getInt64Ty(*ctx));
        llvm::FunctionType *ftype = llvm::FunctionType::get(LLVMTypes::VoidPointerType, 
                                                            argTypes, false);
        llvm::Function *func = 
            llvm::Function::Create(ftype, llvm::GlobalValue::ExternalLinkage,
                                   "ISPCMalloc", module);
        func->setDoesNotThrow(true);
    }

    // Add a declaration of void ISPCFree(void *).  The user is
    // responsible for linking in a definition of this if it's needed by
    // the compiled program.
    { std::vector<const llvm::Type *> argTypes;
        argTypes.push_back(LLVMTypes::VoidPointerType);
        llvm::FunctionType *ftype = llvm::FunctionType::get(LLVMTypes::VoidPointerType, 
                                                            argTypes, false);
        llvm::Function *func = 
            llvm::Function::Create(ftype, llvm::GlobalValue::ExternalLinkage,
                                   "ISPCFree", module);
        func->setDoesNotThrow(true);
    }

    // Add a declaration of void ISPCLaunch(void *funcPtr, void *data).
    // The user is responsible for linking in a definition of this if it's
    // needed by the compiled program.
    { std::vector<const llvm::Type *> argTypes;
        argTypes.push_back(LLVMTypes::VoidPointerType);
        argTypes.push_back(LLVMTypes::VoidPointerType);
        llvm::FunctionType *ftype = llvm::FunctionType::get(LLVMTypes::VoidType, 
                                                            argTypes, false);
        llvm::Function *func = 
            llvm::Function::Create(ftype, llvm::GlobalValue::ExternalLinkage,
                                   "ISPCLaunch", module);
        func->setDoesNotThrow(true);
    }

    // Add a declaration of void ISPCSync().  The user is responsible for
    // linking in a definition of this if it's needed by the compiled
    // program.
    { 
        std::vector<const llvm::Type *> argTypes;
        llvm::FunctionType *ftype = llvm::FunctionType::get(LLVMTypes::VoidType, 
                                                            argTypes, false);
        llvm::Function *func = 
            llvm::Function::Create(ftype, llvm::GlobalValue::ExternalLinkage,
                                   "ISPCSync", module);
        func->setDoesNotThrow(true);
    }

    // Add a declaration of void ISPCInstrument(void *, void *, int, int).
    // The user is responsible for linking in a definition of this if it's
    // needed by the compiled program.
    { 
        std::vector<const llvm::Type *> argTypes;
        argTypes.push_back(llvm::PointerType::get(llvm::Type::getInt8Ty(*g->ctx), 0));
        argTypes.push_back(llvm::PointerType::get(llvm::Type::getInt8Ty(*g->ctx), 0));
        argTypes.push_back(LLVMTypes::Int32Type);
        argTypes.push_back(LLVMTypes::Int32Type);
        llvm::FunctionType *ftype = llvm::FunctionType::get(LLVMTypes::VoidType, 
                                                            argTypes, false);
        llvm::Function *func = 
            llvm::Function::Create(ftype, llvm::GlobalValue::ExternalLinkage,
                                   "ISPCInstrument", module);
        func->setDoesNotThrow(true);
    }

    // Declare various placeholder functions that the optimizer will later
    // find and replace with something more useful.
    lDeclareCompileTimeConstant(module);
    lDeclarePseudoGathers(module);
    lDeclarePseudoScatters(module);
    lDeclarePseudoMaskedStore(module);

    // define the 'programCount' builtin variable
    lDefineConstantInt("programCount", g->target.vectorWidth, module, symbolTable);

    // define the 'programIndex' builtin
    lDefineProgramIndex(module, symbolTable);

    // Define __math_lib stuff.  This is used by stdlib.ispc, for example, to
    // figure out which math routines to end up calling...
    lDefineConstantInt("__math_lib", (int)g->mathLib, module, symbolTable);
    lDefineConstantInt("__math_lib_ispc", (int)Globals::Math_ISPC, module,
                       symbolTable);
    lDefineConstantInt("__math_lib_ispc_fast", (int)Globals::Math_ISPCFast, 
                       module, symbolTable);
    lDefineConstantInt("__math_lib_svml", (int)Globals::Math_SVML, module,
                       symbolTable);
    lDefineConstantInt("__math_lib_system", (int)Globals::Math_System, module,
                       symbolTable);

    if (includeStdlibISPC) {
        // If the user wants the standard library to be included, parse the
        // serialized version of the stdlib.ispc file to get its definitions
        // added.
        extern const char *stdlib_code;
        yy_scan_string(stdlib_code);
        yyparse();
    }
}
