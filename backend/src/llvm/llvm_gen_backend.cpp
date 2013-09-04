/*
 * Copyright © 2012 Intel Corporation
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library. If not, see <http://www.gnu.org/licenses/>.
 *
 * Author: Benjamin Segovia <benjamin.segovia@intel.com>
 */

/**
 * \file llvm_gen_backend.cpp
 * \author Benjamin Segovia <benjamin.segovia@intel.com>
 */

/* Transform the LLVM IR code into Gen IR code i.e. our temporary representation
 * for programs running on Gen.
 *
 * Overview
 * ========
 *
 * This code is mostly inspired by the (now defunct and replaced by CppBackend)
 * CBackend. Basically, there are two ways to transform LLVM code into machine
 * code (or anything else)
 * - You write a complete LLVM backend by the book. LLVM proposes a lot of
 *   useful tools to do so. This is obviously the path chosen by all CPU guys
 *   but also by AMD and nVidia which both use the backend infrastructure to
 *   output their own intermediate language. The good point is that you can
 *   reuse a lot of tools (like proper PHI elimination with phi congruence and
 *   global copy propagation a la Chaitin). Bad points are:
 *     1/ It is a *long* journey to generate anything.
 *     2/ More importantly, the code is hugely biased towards CPUs. Typically,
 *        the way registers are defined do not fit well Gen register file (which
 *        is really more like a regular piece of memory). Same issue apply for
 *        predicated instructions with mask which is a bit boring to use with
 *        SSA. Indeed, since DAGSelection still manipulates SSA values, anything
 *        predicated requires to insert extra sources
 * - You write function passes to do the translation yourself. Obviously, you
 *   reinvent the wheel. However, it is easy to do and easier to maintain
 *   (somehow)
 *
 * So, the code here just traverses LLVM asm and generates our own ISA. The
 * generated code is OK even if a global copy propagation pass is still overdue.
 * Right now, it is pretty straighforward and simplistic in that regard
 *
 * About Clang and the ABI / target
 * ================================
 *
 * A major question is: how did we actually generate this LLVM code from OpenCL?
 * Well, thing is that there is no generic target in LLVM since there are many
 * dependencies on endianness or ABIs. Fortunately, the ptx (and nvptx for LLVM
 * 3.2) profile is pretty well adapted to our needs since NV and Gen GPU are
 * kind of similar, or at least they are similar enough to share the same front
 * end.
 *
 * Problems
 * ========
 *
 * - Several things regarding constants like ConstantExpr are not properly handled.
 * - ptx front end generates function calls. Since we do not support them yet,
 *   the user needs to force the inlining of all functions. If a function call
 *   is intercepted, we just abort
 */

#include "llvm/Config/config.h"
#if LLVM_VERSION_MINOR <= 2
#include "llvm/CallingConv.h"
#include "llvm/Constants.h"
#include "llvm/DerivedTypes.h"
#include "llvm/Module.h"
#include "llvm/Instructions.h"
#else
#include "llvm/IR/CallingConv.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Instructions.h"
#endif  /* LLVM_VERSION_MINOR <= 2 */
#include "llvm/Pass.h"
#include "llvm/PassManager.h"
#if LLVM_VERSION_MINOR <= 2
#include "llvm/Intrinsics.h"
#include "llvm/IntrinsicInst.h"
#include "llvm/InlineAsm.h"
#else
#include "llvm/IR/Intrinsics.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/InlineAsm.h"
#endif  /* LLVM_VERSION_MINOR <= 2 */
#include "llvm/ADT/StringExtras.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/Analysis/ConstantsScanner.h"
#include "llvm/Analysis/FindUsedTypes.h"
#include "llvm/Analysis/LoopInfo.h"
#include "llvm/Analysis/ValueTracking.h"
#include "llvm/CodeGen/Passes.h"
#include "llvm/CodeGen/IntrinsicLowering.h"
#include "llvm/Target/Mangler.h"
#include "llvm/Transforms/Scalar.h"
#include "llvm/MC/MCAsmInfo.h"
#include "llvm/MC/MCContext.h"
#include "llvm/MC/MCInstrInfo.h"
#include "llvm/MC/MCObjectFileInfo.h"
#include "llvm/MC/MCRegisterInfo.h"
#include "llvm/MC/MCSubtargetInfo.h"
#include "llvm/MC/MCSymbol.h"
#if !defined(LLVM_VERSION_MAJOR) || (LLVM_VERSION_MINOR == 1)
#include "llvm/Target/TargetData.h"
#elif LLVM_VERSION_MINOR == 2
#include "llvm/DataLayout.h"
#else
#include "llvm/IR/DataLayout.h"
#endif
#include "llvm/Support/CallSite.h"
#include "llvm/Support/CFG.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/FormattedStream.h"
#include "llvm/Support/GetElementPtrTypeIterator.h"
#if (LLVM_VERSION_MAJOR == 3) && (LLVM_VERSION_MINOR <= 2)
#include "llvm/Support/InstVisitor.h"
#else
#include "llvm/InstVisitor.h"
#endif
#include "llvm/Support/MathExtras.h"
#include "llvm/Support/TargetRegistry.h"
#include "llvm/Support/Host.h"
#include "llvm/Support/ToolOutputFile.h"
#include "llvm/Support/SourceMgr.h"
#include "llvm/Config/config.h"

#include "llvm/llvm_gen_backend.hpp"
#include "ir/context.hpp"
#include "ir/unit.hpp"
#include "ir/liveness.hpp"
#include "sys/set.hpp"
#include "sys/cvar.hpp"

/* Not defined for LLVM 3.0 */
#if !defined(LLVM_VERSION_MAJOR)
#define LLVM_VERSION_MAJOR 3
#endif /* !defined(LLVM_VERSION_MAJOR) */

#if !defined(LLVM_VERSION_MINOR)
#define LLVM_VERSION_MINOR 0
#endif /* !defined(LLVM_VERSION_MINOR) */

#if (LLVM_VERSION_MAJOR != 3) || (LLVM_VERSION_MINOR > 3)
#error "Only LLVM 3.0 - 3.3 is supported"
#endif /* (LLVM_VERSION_MAJOR != 3) || (LLVM_VERSION_MINOR > 3) */

using namespace llvm;

namespace gbe
{
  /*! Gen IR manipulates only scalar types */
  static bool isScalarType(const Type *type)
  {
    return type->isFloatTy()   ||
           type->isIntegerTy() ||
           type->isDoubleTy()  ||
           type->isPointerTy();
  }

  /*! LLVM IR Type to Gen IR type translation */
  static ir::Type getType(const ir::Context &ctx, const Type *type)
  {
    GBE_ASSERT(isScalarType(type));
    if (type->isFloatTy() == true)
      return ir::TYPE_FLOAT;
    if (type->isDoubleTy() == true)
      return ir::TYPE_DOUBLE;
    if (type->isPointerTy() == true) {
      if (ctx.getPointerSize() == ir::POINTER_32_BITS)
        return ir::TYPE_U32;
      else
        return ir::TYPE_U64;
    }
    GBE_ASSERT(type->isIntegerTy() == true);
    if (type == Type::getInt1Ty(type->getContext()))
      return ir::TYPE_BOOL;
    if (type == Type::getInt8Ty(type->getContext()))
      return ir::TYPE_S8;
    if (type == Type::getInt16Ty(type->getContext()))
      return ir::TYPE_S16;
    if (type == Type::getInt32Ty(type->getContext()))
      return ir::TYPE_S32;
    if (type == Type::getInt64Ty(type->getContext()))
      return ir::TYPE_S64;
    GBE_ASSERT(0);
    return ir::TYPE_S64;
  }

  /*! LLVM IR Type to Gen IR unsigned type translation */
  static ir::Type getUnsignedType(const ir::Context &ctx, const Type *type)
  {
    GBE_ASSERT(type->isIntegerTy() == true);
    if (type == Type::getInt1Ty(type->getContext()))
      return ir::TYPE_BOOL;
    if (type == Type::getInt8Ty(type->getContext()))
      return ir::TYPE_U8;
    if (type == Type::getInt16Ty(type->getContext()))
      return ir::TYPE_U16;
    if (type == Type::getInt32Ty(type->getContext()))
      return ir::TYPE_U32;
    if (type == Type::getInt64Ty(type->getContext()))
      return ir::TYPE_U64;
    GBE_ASSERT(0);
    return ir::TYPE_U64;
  }

  /*! Type to register family translation */
  static ir::RegisterFamily getFamily(const ir::Context &ctx, const Type *type)
  {
    GBE_ASSERT(isScalarType(type) == true);
    if (type == Type::getInt1Ty(type->getContext()))
      return ir::FAMILY_BOOL;
    if (type == Type::getInt8Ty(type->getContext()))
      return ir::FAMILY_BYTE;
    if (type == Type::getInt16Ty(type->getContext()))
      return ir::FAMILY_WORD;
    if (type == Type::getInt32Ty(type->getContext()) || type->isFloatTy())
      return ir::FAMILY_DWORD;
    if (type == Type::getInt64Ty(type->getContext()) || type->isDoubleTy())
      return ir::FAMILY_QWORD;
    if (type->isPointerTy())
      return ctx.getPointerFamily();
    GBE_ASSERT(0);
    return ir::FAMILY_BOOL;
  }

  /*! Get number of element to process dealing either with a vector or a scalar
   *  value
   */
  static ir::Type getVectorInfo(const ir::Context &ctx, Type *llvmType, Value *value, uint32_t &elemNum, bool useUnsigned = false)
  {
    ir::Type type;
    if (llvmType->isVectorTy() == true) {
      VectorType *vectorType = cast<VectorType>(llvmType);
      Type *elementType = vectorType->getElementType();
      elemNum = vectorType->getNumElements();
      if (useUnsigned)
        type = getUnsignedType(ctx, elementType);
      else
        type = getType(ctx, elementType);
    } else {
      elemNum = 1;
      if (useUnsigned)
        type = getUnsignedType(ctx, llvmType);
      else
        type = getType(ctx, llvmType);
    }
    return type;
  }

  /*! OCL to Gen-IR address type */
  static INLINE ir::AddressSpace addressSpaceLLVMToGen(unsigned llvmMemSpace) {
    switch (llvmMemSpace) {
      case 0: return ir::MEM_PRIVATE;
      case 1: return ir::MEM_GLOBAL;
      case 2: return ir::MEM_CONSTANT;
      case 3: return ir::MEM_LOCAL;
      case 4: return ir::IMAGE;
    }
    GBE_ASSERT(false);
    return ir::MEM_GLOBAL;
  }

  /*! Handle the LLVM IR Value to Gen IR register translation. This has 2 roles:
   *  - Split the LLVM vector into several scalar values
   *  - Handle the transparent copies (bitcast or use of intrincics functions
   *    like get_local_id / get_global_id
   */
  class RegisterTranslator
  {
  public:
    /*! Indices will be zero for scalar values */
    typedef std::pair<Value*, uint32_t> ValueIndex;
    RegisterTranslator(ir::Context &ctx) : ctx(ctx) {}

    /*! Empty the maps */
    void clear(void) {
      valueMap.clear();
      scalarMap.clear();
    }
    /*! Some values will not be allocated. For example, a bit-cast destination
     *  like: %fake = bitcast %real or a vector insertion since we do not have
     *  vectors in Gen-IR
     */
    void newValueProxy(Value *real,
                       Value *fake,
                       uint32_t realIndex = 0u,
                       uint32_t fakeIndex = 0u) {
      const ValueIndex key(fake, fakeIndex);
      const ValueIndex value(real, realIndex);
      GBE_ASSERT(valueMap.find(key) == valueMap.end()); // Do not insert twice
      valueMap[key] = value;
    }
    /*! After scalarize pass, there are some valueMap in unit,
     *  use this function to copy from unit valueMap */
    void initValueMap(const map<ir::Unit::ValueIndex, ir::Unit::ValueIndex> &vMap) {
      for(auto &it : vMap)
        newValueProxy((Value*)it.second.first, (Value*)it.first.first,
                      it.second.second, it.first.second);
    }
    /*! Mostly used for the preallocated registers (lids, gids) */
    void newScalarProxy(ir::Register reg, Value *value, uint32_t index = 0u) {
      const ValueIndex key(value, index);
      GBE_ASSERT(scalarMap.find(key) == scalarMap.end());
      scalarMap[key] = reg;
    }
    /*! Allocate a new scalar register */
    ir::Register newScalar(Value *value, Value *key = NULL, uint32_t index = 0u)
    {
      GBE_ASSERT(dyn_cast<Constant>(value) == NULL);
      Type *type = value->getType();
      auto typeID = type->getTypeID();
      switch (typeID) {
        case Type::IntegerTyID:
        case Type::FloatTyID:
        case Type::DoubleTyID:
        case Type::PointerTyID:
          GBE_ASSERT(index == 0);
          return this->newScalar(value, key, type, index);
          break;
        case Type::VectorTyID:
        {
          auto vectorType = cast<VectorType>(type);
          auto elementType = vectorType->getElementType();
          auto elementTypeID = elementType->getTypeID();
          if (elementTypeID != Type::IntegerTyID &&
              elementTypeID != Type::FloatTyID &&
              elementTypeID != Type::DoubleTyID)
            GBE_ASSERTM(false, "Vectors of elements are not supported");
            return this->newScalar(value, key, elementType, index);
          break;
        }
        default: NOT_SUPPORTED;
      };
      return ir::Register();
    }

    /*! iterating in the value map to get the final real register */
    void getRealValue(Value* &value, uint32_t& index) {
      auto end = valueMap.end();
      for (;;) {
        auto it = valueMap.find(std::make_pair(value, index));
        if (it == end)
          break;
        else {
          value = it->second.first;
          index = it->second.second;
        }
      }
    }

    /*! Get the register from the given value at given index possibly iterating
     *  in the value map to get the final real register
     */
    ir::Register getScalar(Value *value, uint32_t index = 0u) {
      getRealValue(value, index);

      const auto key = std::make_pair(value, index);
      GBE_ASSERT(scalarMap.find(key) != scalarMap.end());
      return scalarMap[key];
    }
    /*! Insert a given register at given Value position */
    void insertRegister(const ir::Register &reg, Value *value, uint32_t index) {
      const auto key = std::make_pair(value, index);
      GBE_ASSERT(scalarMap.find(key) == scalarMap.end());
      scalarMap[key] = reg;
    }
    /*! Says if the value exists. Otherwise, it is undefined */
    bool valueExists(Value *value, uint32_t index) {
      getRealValue(value, index);

      const auto key = std::make_pair(value, index);
      return scalarMap.find(key) != scalarMap.end();
    }
    /*! if it's a undef const value, return true. Otherwise, return false. */
    bool isUndefConst(Value *value, uint32_t index) {
      getRealValue(value, index);

      Constant *CPV = dyn_cast<Constant>(value);
      return (CPV && (isa<UndefValue>(CPV)));
    }
  private:
    /*! This creates a scalar register for a Value (index is the vector index when
     *  the value is a vector of scalars)
     */
    ir::Register newScalar(Value *value, Value *key, Type *type, uint32_t index) {
      const ir::RegisterFamily family = getFamily(ctx, type);
      const ir::Register reg = ctx.reg(family);
      key = key == NULL ? value : key;
      this->insertRegister(reg, key, index);
      return reg;
    }
    /*! Map value to ir::Register */
    map<ValueIndex, ir::Register> scalarMap;
    /*! Map values to values when this is only a translation (eq bitcast) */
    map<ValueIndex, ValueIndex> valueMap;
    /*! Actually allocates the registers */
    ir::Context &ctx;
  };

  /*! Translate LLVM IR code to Gen IR code */
  class GenWriter : public FunctionPass, public InstVisitor<GenWriter>
  {
    /*! Unit to compute */
    ir::Unit &unit;
    /*! Helper structure to compute the unit */
    ir::Context ctx;
    /*! Make the LLVM-to-Gen translation */
    RegisterTranslator regTranslator;
    /*! Map target basic block to its ir::LabelIndex */
    map<const BasicBlock*, ir::LabelIndex> labelMap;
    /*! Condition inversion can simplify branch code. We store here all the
     *  compare instructions we need to invert to decrease branch complexity
     */
    set<const Value*> conditionSet;
    /*! We visit each function twice. Once to allocate the registers and once to
     *  emit the Gen IR instructions
     */
    enum Pass {
      PASS_EMIT_REGISTERS = 0,
      PASS_EMIT_INSTRUCTIONS = 1
    } pass;

    LoopInfo *LI;
    const Module *TheModule;

  public:
    static char ID;
    explicit GenWriter(ir::Unit &unit)
      : FunctionPass(ID),
        unit(unit),
        ctx(unit),
        regTranslator(ctx),
        LI(0),
        TheModule(0)
    {
      initializeLoopInfoPass(*PassRegistry::getPassRegistry());
      pass = PASS_EMIT_REGISTERS;
    }

    virtual const char *getPassName() const { return "Gen Back-End"; }

    void getAnalysisUsage(AnalysisUsage &AU) const {
      AU.addRequired<LoopInfo>();
      AU.setPreservesAll();
    }

    virtual bool doInitialization(Module &M);

    void collectGlobalConstant(void) const;

    bool runOnFunction(Function &F) {
     // Do not codegen any 'available_externally' functions at all, they have
     // definitions outside the translation unit.
     if (F.hasAvailableExternallyLinkage())
       return false;

      LI = &getAnalysis<LoopInfo>();

      emitFunction(F);
      return false;
    }

    virtual bool doFinalization(Module &M) { return false; }

    /*! Emit the complete function code and declaration */
    void emitFunction(Function &F);
    /*! Handle input and output function parameters */
    void emitFunctionPrototype(Function &F);
    /*! Emit the code for a basic block */
    void emitBasicBlock(BasicBlock *BB);
    /*! Each block end may require to emit MOVs for further PHIs */
    void emitMovForPHI(BasicBlock *curr, BasicBlock *succ);
    /*! Alocate one or several registers (if vector) for the value */
    INLINE void newRegister(Value *value, Value *key = NULL);
    /*! Return a valid register from an operand (can use LOADI to make one) */
    INLINE ir::Register getRegister(Value *value, uint32_t index = 0);
    /*! Create a new immediate from a constant */
    ir::ImmediateIndex newImmediate(Constant *CPV, uint32_t index = 0);
    /*! Insert a new label index when this is a scalar value */
    INLINE void newLabelIndex(const BasicBlock *bb);
    /*! Inspect the terminator instruction and try to see if we should invert
     *  the value to simplify the code
     */
    INLINE void simplifyTerminator(BasicBlock *bb);
    /*! Helper function to emit loads and stores */
    template <bool isLoad, typename T> void emitLoadOrStore(T &I);
    /*! Will try to remove MOVs due to PHI resolution */
    void removeMOVs(const ir::Liveness &liveness, ir::Function &fn);
    /*! Will try to remove redundants LOADI in basic blocks */
    void removeLOADIs(const ir::Liveness &liveness, ir::Function &fn);
    /*! To avoid lost copy, we need two values for PHI. This function create a
     * fake value for the copy (basically ptr+1)
     */
    INLINE Value *getPHICopy(Value *PHI);
    // Currently supported instructions
#define DECL_VISIT_FN(NAME, TYPE) \
    void regAllocate##NAME(TYPE &I); \
    void emit##NAME(TYPE &I); \
    void visit##NAME(TYPE &I) { \
      if (pass == PASS_EMIT_INSTRUCTIONS) \
        emit##NAME(I); \
      else \
        regAllocate##NAME(I); \
    }
    DECL_VISIT_FN(BinaryOperator, Instruction);
    DECL_VISIT_FN(CastInst, CastInst);
    DECL_VISIT_FN(ReturnInst, ReturnInst);
    DECL_VISIT_FN(LoadInst, LoadInst);
    DECL_VISIT_FN(StoreInst, StoreInst);
    DECL_VISIT_FN(CallInst, CallInst);
    DECL_VISIT_FN(ICmpInst, ICmpInst);
    DECL_VISIT_FN(FCmpInst, FCmpInst);
    DECL_VISIT_FN(InsertElement, InsertElementInst);
    DECL_VISIT_FN(ExtractElement, ExtractElementInst);
    DECL_VISIT_FN(ShuffleVectorInst, ShuffleVectorInst);
    DECL_VISIT_FN(SelectInst, SelectInst);
    DECL_VISIT_FN(BranchInst, BranchInst);
    DECL_VISIT_FN(PHINode, PHINode);
    DECL_VISIT_FN(AllocaInst, AllocaInst);
#undef DECL_VISIT_FN

    // Emit unary instructions from gen native function
    void emitUnaryCallInst(CallInst &I, CallSite &CS, ir::Opcode opcode);
    // Emit unary instructions from gen native function
    void emitAtomicInst(CallInst &I, CallSite &CS, ir::AtomicOps opcode);

    // These instructions are not supported at all
    void visitVAArgInst(VAArgInst &I) {NOT_SUPPORTED;}
    void visitSwitchInst(SwitchInst &I) {NOT_SUPPORTED;}
    void visitInvokeInst(InvokeInst &I) {NOT_SUPPORTED;}
#if LLVM_VERSION_MINOR == 0
    void visitUnwindInst(UnwindInst &I) {NOT_SUPPORTED;}
#endif /* __LLVM_30__ */
    void visitResumeInst(ResumeInst &I) {NOT_SUPPORTED;}
    void visitInlineAsm(CallInst &I) {NOT_SUPPORTED;}
    void visitIndirectBrInst(IndirectBrInst &I) {NOT_SUPPORTED;}
    void visitUnreachableInst(UnreachableInst &I) {NOT_SUPPORTED;}
    void visitGetElementPtrInst(GetElementPtrInst &I) {NOT_SUPPORTED;}
    void visitInsertValueInst(InsertValueInst &I) {NOT_SUPPORTED;}
    void visitExtractValueInst(ExtractValueInst &I) {NOT_SUPPORTED;}
    template <bool isLoad, typename T> void visitLoadOrStore(T &I);

    void visitInstruction(Instruction &I) {NOT_SUPPORTED;}
  };

  char GenWriter::ID = 0;

  void GenWriter::collectGlobalConstant(void) const {
    const Module::GlobalListType &globalList = TheModule->getGlobalList();
    for(auto i = globalList.begin(); i != globalList.end(); i ++) {
      const GlobalVariable &v = *i;
      const char *name = v.getName().data();
      unsigned addrSpace = v.getType()->getAddressSpace();
      if(addrSpace == ir::AddressSpace::MEM_CONSTANT) {
        GBE_ASSERT(v.hasInitializer());
        const Constant *c = v.getInitializer();
        if (c->getType()->getTypeID() != Type::ArrayTyID) {
          void *mem = malloc(sizeof(double));
          int size = 0;
          switch(c->getType()->getTypeID()) {
            case Type::TypeID::IntegerTyID: {
              const ConstantInt *ci = dyn_cast<ConstantInt>(c);
              *(int *)mem = ci->isNegative() ? ci->getSExtValue() : ci->getZExtValue();
              size = sizeof(int);
              break;
            }
            case Type::TypeID::FloatTyID: {
              const ConstantFP *cf = dyn_cast<ConstantFP>(c);
              *(float *)mem = cf->getValueAPF().convertToFloat();
              size = sizeof(float);
              break;
            }
            case Type::TypeID::DoubleTyID: {
              const ConstantFP *cf = dyn_cast<ConstantFP>(c);
              *(double *)mem = cf->getValueAPF().convertToDouble();
              size = sizeof(double);
              break;
            }
            default:
              NOT_IMPLEMENTED;
          }
          unit.newConstant((char *)mem, name, size, size);
          free(mem);
          continue;
        }
        GBE_ASSERT(c->getType()->getTypeID() == Type::ArrayTyID);
        const ConstantDataArray *cda = dyn_cast<ConstantDataArray>(c);
        GBE_ASSERT(cda);
        unsigned len = cda->getNumElements();
        uint64_t elementSize = cda->getElementByteSize();
        Type::TypeID typeID = cda->getElementType()->getTypeID();
        if(typeID == Type::TypeID::IntegerTyID)
          elementSize = sizeof(unsigned);
        void *mem = malloc(elementSize * len);
        for(unsigned j = 0; j < len; j ++) {
          switch(typeID) {
            case Type::TypeID::FloatTyID:
             {
              float f = cda->getElementAsFloat(j);
              memcpy((float *)mem + j, &f, elementSize);
             }
              break;
            case Type::TypeID::DoubleTyID:
             {
              double d = cda->getElementAsDouble(j);
              memcpy((double *)mem + j, &d, elementSize);
             }
              break;
            case Type::TypeID::IntegerTyID:
             {
              unsigned u = (unsigned) cda->getElementAsInteger(j);
              memcpy((unsigned *)mem + j, &u, elementSize);
             }
              break;
            default:
              NOT_IMPLEMENTED;
          }
        }
        unit.newConstant((char *)mem, name, elementSize * len, sizeof(unsigned));
        free(mem);
      }
    }
  }

  bool GenWriter::doInitialization(Module &M) {
    FunctionPass::doInitialization(M);

    // Initialize
    TheModule = &M;
    collectGlobalConstant();
    return false;
  }

  static Constant *extractConstantElem(Constant *CPV, uint32_t index) {
    ConstantVector *CV = dyn_cast<ConstantVector>(CPV);
    GBE_ASSERT(CV != NULL);
#if GBE_DEBUG
    const uint32_t elemNum = CV->getNumOperands();
    GBE_ASSERTM(index < elemNum, "Out-of-bound constant vector access");
#endif /* GBE_DEBUG */
    CPV = cast<Constant>(CV->getOperand(index));
    return CPV;
  }

  template <typename U, typename T>
  static U processConstant(Constant *CPV, T doIt, uint32_t index = 0u)
  {
#if GBE_DEBUG
    GBE_ASSERTM(dyn_cast<ConstantExpr>(CPV) == NULL, "Unsupported constant expression");
    if (isa<UndefValue>(CPV) && CPV->getType()->isSingleValueType())
      GBE_ASSERTM(false, "Unsupported constant expression");
#endif /* GBE_DEBUG */

#if LLVM_VERSION_MINOR > 0
    ConstantDataSequential *seq = dyn_cast<ConstantDataSequential>(CPV);

    if (seq) {
      Type *Ty = seq->getElementType();
      if (Ty == Type::getInt1Ty(CPV->getContext())) {
        const uint64_t u64 = seq->getElementAsInteger(index);
        return doIt(bool(u64));
      } else if (Ty == Type::getInt8Ty(CPV->getContext())) {
        const uint64_t u64 = seq->getElementAsInteger(index);
        return doIt(uint8_t(u64));
      } else if (Ty == Type::getInt16Ty(CPV->getContext())) {
        const uint64_t u64 = seq->getElementAsInteger(index);
        return doIt(uint16_t(u64));
      } else if (Ty == Type::getInt32Ty(CPV->getContext())) {
        const uint64_t u64 = seq->getElementAsInteger(index);
        return doIt(uint32_t(u64));
      } else if (Ty == Type::getInt64Ty(CPV->getContext())) {
        const uint64_t u64 = seq->getElementAsInteger(index);
        return doIt(u64);
      } else if (Ty == Type::getFloatTy(CPV->getContext())) {
        const float f32 = seq->getElementAsFloat(index);
        return doIt(f32);
      } else if (Ty == Type::getDoubleTy(CPV->getContext())) {
        const float f64 = seq->getElementAsDouble(index);
        return doIt(f64);
      }
    } else
#endif /* LLVM_VERSION_MINOR > 0 */

    if (dyn_cast<ConstantAggregateZero>(CPV)) {
      return doIt(uint32_t(0)); // XXX Handle type
    } else {
      if (dyn_cast<ConstantVector>(CPV))
        CPV = extractConstantElem(CPV, index);
      GBE_ASSERTM(dyn_cast<ConstantExpr>(CPV) == NULL, "Unsupported constant expression");

      // Integers
      if (ConstantInt *CI = dyn_cast<ConstantInt>(CPV)) {
        Type* Ty = CI->getType();
        if (Ty == Type::getInt1Ty(CPV->getContext())) {
          const bool b = CI->getZExtValue();
          return doIt(b);
        } else if (Ty == Type::getInt8Ty(CPV->getContext())) {
          const uint8_t u8 = CI->getZExtValue();
          return doIt(u8);
        } else if (Ty == Type::getInt16Ty(CPV->getContext())) {
          const uint16_t u16 = CI->getZExtValue();
          return doIt(u16);
        } else if (Ty == Type::getInt32Ty(CPV->getContext())) {
          const uint32_t u32 = CI->getZExtValue();
          return doIt(u32);
        } else if (Ty == Type::getInt64Ty(CPV->getContext())) {
          const uint64_t u64 = CI->getZExtValue();
          return doIt(u64);
        } else {
          GBE_ASSERTM(false, "Unsupported integer size");
          return doIt(uint64_t(0));
        }
      }

      // NULL pointers
      if(isa<ConstantPointerNull>(CPV)) {
        return doIt(uint32_t(0));
      }

      // Floats and doubles
      const Type::TypeID typeID = CPV->getType()->getTypeID();
      switch (typeID) {
        case Type::FloatTyID:
        case Type::DoubleTyID:
        {
          ConstantFP *FPC = cast<ConstantFP>(CPV);
          GBE_ASSERT(isa<UndefValue>(CPV) == false);

          if (FPC->getType() == Type::getFloatTy(CPV->getContext())) {
            const float f32 = FPC->getValueAPF().convertToFloat();
            return doIt(f32);
          } else {
            const double f64 = FPC->getValueAPF().convertToDouble();
            return doIt(f64);
          }
        }
        break;
        default:
          GBE_ASSERTM(false, "Unsupported constant type");
          break;
      }
    }

    GBE_ASSERTM(false, "Unsupported constant type");
    return doIt(uint64_t(0));
  }

  /*! Pfff. I cannot use a lambda, since it is templated. Congratulation c++ */
  struct NewImmediateFunctor
  {
    NewImmediateFunctor(ir::Context &ctx) : ctx(ctx) {}
    template <typename T> ir::ImmediateIndex operator() (const T &t) {
      return ctx.newImmediate(t);
    }
    ir::Context &ctx;
  };

  ir::ImmediateIndex GenWriter::newImmediate(Constant *CPV, uint32_t index) {
    return processConstant<ir::ImmediateIndex>(CPV, NewImmediateFunctor(ctx), index);
  }

  void GenWriter::newRegister(Value *value, Value *key) {
    auto type = value->getType();
    auto typeID = type->getTypeID();
    switch (typeID) {
      case Type::IntegerTyID:
      case Type::FloatTyID:
      case Type::DoubleTyID:
      case Type::PointerTyID:
        regTranslator.newScalar(value, key);
        break;
      case Type::VectorTyID:
      {
        auto vectorType = cast<VectorType>(type);
        const uint32_t elemNum = vectorType->getNumElements();
        for (uint32_t elemID = 0; elemID < elemNum; ++elemID)
          regTranslator.newScalar(value, key, elemID);
        break;
      }
      default: NOT_SUPPORTED;
    };
  }

  ir::Register GenWriter::getRegister(Value *value, uint32_t elemID) {
    //the real value may be constant, so get real value before constant check
    regTranslator.getRealValue(value, elemID);

    if (dyn_cast<ConstantExpr>(value)) {
      ConstantExpr *ce = dyn_cast<ConstantExpr>(value);
      if(ce->isCast()) {
        GBE_ASSERT(ce->getOpcode() == Instruction::PtrToInt);
        const Value *pointer = ce->getOperand(0);
        GBE_ASSERT(pointer->hasName());
        auto name = pointer->getName().str();
        uint16_t reg = unit.getConstantSet().getConstant(name).getReg();
        return ir::Register(reg);
      }
    }
    Constant *CPV = dyn_cast<Constant>(value);
    if (CPV) {
      if (isa<GlobalValue>(CPV)) {
        auto name = CPV->getName().str();
        uint16_t reg = unit.getConstantSet().getConstant(name).getReg();
        return ir::Register(reg);
      }
      if (isa<ConstantExpr>(CPV)) {
        ConstantExpr *CE = dyn_cast<ConstantExpr>(CPV);
        GBE_ASSERT(CE->isGEPWithNoNotionalOverIndexing());
        auto pointer = CE->getOperand(0);
        auto offset1 = dyn_cast<ConstantInt>(CE->getOperand(1));
        GBE_ASSERT(offset1->getZExtValue() == 0);
        auto offset2 = dyn_cast<ConstantInt>(CE->getOperand(2));
        int type_size = pointer->getType()->getTypeID() == Type::TypeID::DoubleTyID ? sizeof(double) : sizeof(int);
        int type_offset = offset2->getSExtValue() * type_size;
        auto pointer_name = pointer->getName().str();
        ir::Register pointer_reg = ir::Register(unit.getConstantSet().getConstant(pointer_name).getReg());
        ir::Register offset_reg = ctx.reg(ir::RegisterFamily::FAMILY_DWORD);
        ctx.LOADI(ir::Type::TYPE_S32, offset_reg, ctx.newIntegerImmediate(type_offset, ir::Type::TYPE_S32));
        ir::Register reg = ctx.reg(ir::RegisterFamily::FAMILY_DWORD);
        ctx.ADD(ir::Type::TYPE_S32, reg, pointer_reg, offset_reg);
        return reg;
      }
      const ir::ImmediateIndex immIndex = this->newImmediate(CPV, elemID);
      const ir::Immediate imm = ctx.getImmediate(immIndex);
      const ir::Register reg = ctx.reg(getFamily(imm.type));
      ctx.LOADI(imm.type, reg, immIndex);
      return reg;
    }
    else
      return regTranslator.getScalar(value, elemID);
  }

  INLINE Value *GenWriter::getPHICopy(Value *PHI) {
    const uintptr_t ptr = (uintptr_t) PHI;
    return (Value*) (ptr+1);
  }

  void GenWriter::newLabelIndex(const BasicBlock *bb) {
    if (labelMap.find(bb) == labelMap.end()) {
      const ir::LabelIndex label = ctx.label();
      labelMap[bb] = label;
    }
  }

  void GenWriter::simplifyTerminator(BasicBlock *bb) {
    Value *value = --bb->end();
    BranchInst *I = NULL;
    if ((I = dyn_cast<BranchInst>(value)) != NULL) {
      if (I->isConditional() == false)
        return;
      // If the "taken" successor is the next block, we try to invert the
      // branch.
      BasicBlock *succ = I->getSuccessor(0);
      if (llvm::next(Function::iterator(bb)) != Function::iterator(succ))
        return;

      // More than one use is too complicated: we skip it
      Value *condition = I->getCondition();
      if (condition->hasOneUse() == false)
        return;

      // Right now, we only invert comparison instruction
      ICmpInst *CI = dyn_cast<ICmpInst>(condition);
      if (CI != NULL) {
        GBE_ASSERT(conditionSet.find(CI) == conditionSet.end());
        conditionSet.insert(CI);
        return;
      }
    }
  }

  void GenWriter::emitBasicBlock(BasicBlock *BB) {
    GBE_ASSERT(labelMap.find(BB) != labelMap.end());
    ctx.LABEL(labelMap[BB]);
    for (auto II = BB->begin(), E = BB->end(); II != E; ++II) visit(*II);
  }

  void GenWriter::emitMovForPHI(BasicBlock *curr, BasicBlock *succ) {
    for (BasicBlock::iterator I = succ->begin(); isa<PHINode>(I); ++I) {
      PHINode *PN = cast<PHINode>(I);
      Value *IV = PN->getIncomingValueForBlock(curr);
      if (!isa<UndefValue>(IV)) {
        Type *llvmType = PN->getType();
        GBE_ASSERTM(llvmType != Type::getInt1Ty(llvmType->getContext()),
          "TODO Boolean values cannot escape their definition basic block");
        const ir::Type type = getType(ctx, llvmType);

        // Emit the MOV required by the PHI function. We do it simple and do not
        // try to optimize them. A next data flow analysis pass on the Gen IR
        // will remove them
        Value *PHICopy = this->getPHICopy(PN);
        const ir::Register dst = this->getRegister(PHICopy);
        Constant *CP = dyn_cast<Constant>(IV);
        if (CP) {
          GBE_ASSERT(isa<GlobalValue>(CP) == false);
          ConstantVector *CPV = dyn_cast<ConstantVector>(CP);
          if (CPV && dyn_cast<ConstantVector>(CPV) &&
              isa<UndefValue>(extractConstantElem(CPV, 0)))
            continue;
          const ir::ImmediateIndex immIndex = this->newImmediate(CP);
          const ir::Immediate imm = ctx.getImmediate(immIndex);
          ctx.LOADI(imm.type, dst, immIndex);
        } else if (regTranslator.valueExists(IV,0) || dyn_cast<Constant>(IV)) {
          const ir::Register src = this->getRegister(IV);
          ctx.MOV(type, dst, src);
        }
      }
    }
  }

  void GenWriter::emitFunctionPrototype(Function &F)
  {
    GBE_ASSERTM(F.hasStructRetAttr() == false,
                "Returned value for kernel functions is forbidden");
    // Loop over the arguments and output registers for them
    if (!F.arg_empty()) {
      uint32_t argID = 0;
      Function::arg_iterator I = F.arg_begin(), E = F.arg_end();

      // Insert a new register for each function argument
#if LLVM_VERSION_MINOR <= 1
      const AttrListPtr &PAL = F.getAttributes();
#endif /* LLVM_VERSION_MINOR <= 1 */
      for (; I != E; ++I, ++argID) {
        const std::string &argName = I->getName().str();
        Type *type = I->getType();

        //add support for vector argument
        if(type->isVectorTy()) {
          VectorType *vectorType = cast<VectorType>(type);

          this->newRegister(I);
          ir::Register reg = getRegister(I, 0);

          Type *elemType = vectorType->getElementType();
          const uint32_t elemSize = getTypeByteSize(unit, elemType);
          const uint32_t elemNum = vectorType->getNumElements();
          //vector's elemType always scalar type
          ctx.input(argName, ir::FunctionArgument::VALUE, reg, elemNum*elemSize);

          ir::Function& fn = ctx.getFunction();
          for(uint32_t i=1; i < elemNum; i++) {
            ir::PushLocation argLocation(fn, argID, elemSize*i);
            reg = getRegister(I, i);
            ctx.appendPushedConstant(reg, argLocation);  //add to push map for reg alloc
          }
          continue;
        }

        GBE_ASSERTM(isScalarType(type) == true,
                    "vector type in the function argument is not supported yet");
        const ir::Register reg = regTranslator.newScalar(I);
        if (type->isPointerTy() == false)
          ctx.input(argName, ir::FunctionArgument::VALUE, reg, getTypeByteSize(unit, type));
        else {
          PointerType *pointerType = dyn_cast<PointerType>(type);
          // By value structure
#if LLVM_VERSION_MINOR <= 1
          if (PAL.paramHasAttr(argID+1, Attribute::ByVal)) {
#else
          if (I->hasByValAttr()) {
#endif /* LLVM_VERSION_MINOR <= 1 */
            Type *pointed = pointerType->getElementType();
            const size_t structSize = getTypeByteSize(unit, pointed);
            ctx.input(argName, ir::FunctionArgument::STRUCTURE, reg, structSize);
          }
          // Regular user provided pointer (global, local or constant)
          else {
            const uint32_t addr = pointerType->getAddressSpace();
            const ir::AddressSpace addrSpace = addressSpaceLLVMToGen(addr);
            const uint32_t ptrSize = getTypeByteSize(unit, type);
              switch (addrSpace) {
              case ir::MEM_GLOBAL:
                ctx.input(argName, ir::FunctionArgument::GLOBAL_POINTER, reg, ptrSize);
              break;
              case ir::MEM_LOCAL:
                ctx.input(argName, ir::FunctionArgument::LOCAL_POINTER, reg, ptrSize);
                ctx.getFunction().setUseSLM(true);
              break;
              case ir::MEM_CONSTANT:
                ctx.input(argName, ir::FunctionArgument::CONSTANT_POINTER, reg, ptrSize);
              break;
              case ir::IMAGE:
                ctx.input(argName, ir::FunctionArgument::IMAGE, reg, ptrSize);
                ctx.getFunction().getImageSet()->append(reg, &ctx);
              break;
              default: GBE_ASSERT(addrSpace != ir::MEM_PRIVATE);
            }
          }
        }
      }
    }

    // When returning a structure, first input register is the pointer to the
    // structure
#if GBE_DEBUG
    const Type *type = F.getReturnType();
    GBE_ASSERTM(type->isVoidTy() == true,
                "Returned value for kernel functions is forbidden");

    // Variable number of arguments is not supported
    FunctionType *FT = cast<FunctionType>(F.getFunctionType());
    GBE_ASSERT(FT->isVarArg() == false);
#endif /* GBE_DEBUG */
  }

  static inline bool isFPIntBitCast(const Instruction &I) {
    if (!isa<BitCastInst>(I))
      return false;
    Type *SrcTy = I.getOperand(0)->getType();
    Type *DstTy = I.getType();
    return (SrcTy->isFloatingPointTy() && DstTy->isIntegerTy()) ||
           (DstTy->isFloatingPointTy() && SrcTy->isIntegerTy());
  }

  /*! To track last read and write of the registers */
  struct RegInfoForMov {
    ir::Instruction *lastWriteInsn;
    ir::Instruction *lastReadInsn;
    uint32_t lastWrite;
    uint32_t lastRead;
  };

  /*! Replace register "from" by register "to" in the destination(s) */
  static void replaceDst(ir::Instruction *insn, ir::Register from, ir::Register to) {
    const uint32_t dstNum = insn->getDstNum();
    for (uint32_t dstID = 0; dstID < dstNum; ++dstID)
      if (insn->getDst(dstID) == from)
        insn->setDst(dstID, to);
  }

  /*! Replace register "from" by register "to" in the source(s) */
  static void replaceSrc(ir::Instruction *insn, ir::Register from, ir::Register to) {
    const uint32_t srcNum = insn->getSrcNum();
    for (uint32_t srcID = 0; srcID < srcNum; ++srcID)
      if (insn->getSrc(srcID) == from)
        insn->setSrc(srcID, to);
  }

  /*! lastUse maintains data about last uses (reads/writes) for each
   * ir::Register
   */
  static void buildRegInfo(ir::BasicBlock &bb, vector<RegInfoForMov> &lastUse)
  {
    // Clear the register usages
    for (auto &x : lastUse) {
      x.lastWrite = x.lastRead = 0;
      x.lastWriteInsn = x.lastReadInsn = NULL;
    }

    // Find use intervals for all registers (distinguish sources and
    // destinations)
    uint32_t insnID = 2;
    bb.foreach([&](ir::Instruction &insn) {
      const uint32_t dstNum = insn.getDstNum();
      const uint32_t srcNum = insn.getSrcNum();
      for (uint32_t srcID = 0; srcID < srcNum; ++srcID) {
        const ir::Register reg = insn.getSrc(srcID);
        lastUse[reg].lastRead = insnID;
        lastUse[reg].lastReadInsn = &insn;
      }
      for (uint32_t dstID = 0; dstID < dstNum; ++dstID) {
        const ir::Register reg = insn.getDst(dstID);
        lastUse[reg].lastWrite = insnID+1;
        lastUse[reg].lastWriteInsn = &insn;
      }
      insnID+=2;
    });
  }

  void GenWriter::removeMOVs(const ir::Liveness &liveness, ir::Function &fn)
  {
    // We store the last write and last read for each register
    const uint32_t regNum = fn.regNum();
    vector<RegInfoForMov> lastUse;
    lastUse.resize(regNum);

    // Remove the MOVs per block (local analysis only) Note that we do not try
    // to remove MOV for variables that outlives the block. So we use liveness
    // information to figure out which variable is alive
    fn.foreachBlock([&](ir::BasicBlock &bb)
    {
      // We need to know when each register will be read or written
      buildRegInfo(bb, lastUse);

      // Liveinfo helps us to know if the source outlives the block
      const ir::Liveness::BlockInfo &info = liveness.getBlockInfo(&bb);

      auto it = --bb.end();
      if (it->isMemberOf<ir::BranchInstruction>() == true) --it;
      for (auto it = --bb.end(); it != bb.end();) {
        ir::Instruction *insn = &*it; it--;
        const ir::Opcode op = insn->getOpcode();
        if (op == ir::OP_MOV) {
          const ir::Register dst = insn->getDst(0);
          const ir::Register src = insn->getSrc(0);
          // Outlives the block. We do not do anything
          if (info.inLiveOut(src))
            continue;
          const RegInfoForMov &dstInfo = lastUse[dst];
          const RegInfoForMov &srcInfo = lastUse[src];
          // The source is not computed in this block
          if (srcInfo.lastWrite == 0)
            continue;
          // dst is read after src is written. We cannot overwrite dst
          if (dstInfo.lastRead > srcInfo.lastWrite)
            continue;
          // We are good. We first patch the destination then all the sources
          replaceDst(srcInfo.lastWriteInsn, src, dst);
          // Then we patch all subsequent uses of the source
          ir::Instruction *next = static_cast<ir::Instruction*>(srcInfo.lastWriteInsn->next);
          while (next != insn) {
            replaceSrc(next, src, dst);
            next = static_cast<ir::Instruction*>(next->next);
          }
          insn->remove();
        } else if (op == ir::OP_LOADI)
          continue;
        else
          break;
      }
    });
  }

  void GenWriter::removeLOADIs(const ir::Liveness &liveness, ir::Function &fn)
  {
    // We store the last write and last read for each register
    const uint32_t regNum = fn.regNum();
    vector<RegInfoForMov> lastUse;
    lastUse.resize(regNum);

    // Traverse all blocks and remove redundant immediates. Do *not* remove
    // immediates that outlive the block
    fn.foreachBlock([&](ir::BasicBlock &bb)
    {
      // Each immediate that is already loaded in the block
      map<ir::Immediate, ir::Register> loadedImm;

      // Immediate to immediate translation
      map<ir::Register, ir::Register> immTranslate;

      // Liveinfo helps us to know if the loaded immediate outlives the block
      const ir::Liveness::BlockInfo &info = liveness.getBlockInfo(&bb);

      // We need to know when each register will be read or written
      buildRegInfo(bb, lastUse);

      // Top bottom traversal -> remove useless LOADIs
      uint32_t insnID = 2;
      bb.foreach([&](ir::Instruction &insn)
      {
        // We either try to remove the LOADI or we will try to use it as a
        // replacement for the next same LOADIs
        if (insn.isMemberOf<ir::LoadImmInstruction>()) {
          ir::LoadImmInstruction &loadImm = cast<ir::LoadImmInstruction>(insn);
          const ir::Immediate imm = loadImm.getImmediate();
          const ir::Register dst = loadImm.getDst(0);

          // Not here: cool, we put it in the map if the register is not
          // overwritten. If it is, we just ignore it for simplicity. Note that
          // it should not happen with the way we "unSSA" the code
          auto it = loadedImm.find(imm);
          auto end = loadedImm.end();
          if (it == end && lastUse[dst].lastWrite == insnID+1)
            loadedImm.insert(std::make_pair(imm, dst));
          // We already pushed the same immediate and we do not outlive the
          // block. We are good to replace this immediate by the previous one
          else if (it != end && info.inLiveOut(dst) == false) {
            immTranslate.insert(std::make_pair(dst, it->second));
            insn.remove();
          }
        }
        // Traverse all the destinations and sources and perform the
        // substitutions (if any)
        else {
          const uint32_t srcNum = insn.getSrcNum();
          const uint32_t dstNum = insn.getSrcNum();
          for (uint32_t srcID = 0; srcID < srcNum; ++srcID) {
            const ir::Register src = insn.getSrc(srcID);
            auto it = immTranslate.find(src);
            if (it != immTranslate.end())
              insn.setSrc(srcID, it->second);
          }
          for (uint32_t dstID = 0; dstID < dstNum; ++dstID) {
            const ir::Register dst = insn.getSrc(dstID);
            auto it = immTranslate.find(dst);
            if (it != immTranslate.end())
              insn.setDst(dstID, it->second);
          }
        }
        insnID += 2;
      });
    });
  }

  BVAR(OCL_OPTIMIZE_PHI_MOVES, true);
  BVAR(OCL_OPTIMIZE_LOADI, true);

  void GenWriter::emitFunction(Function &F)
  {
    switch (F.getCallingConv()) {
#if LLVM_VERSION_MINOR <= 2
      case CallingConv::PTX_Device: // we do not emit device function
        return;
      case CallingConv::PTX_Kernel:
#else
      case CallingConv::C:
#endif
        break;
      default: GBE_ASSERTM(false, "Unsupported calling convention");
    }

    ctx.startFunction(F.getName());
    this->regTranslator.clear();
    this->regTranslator.initValueMap(unit.getValueMap());
    this->labelMap.clear();
    this->emitFunctionPrototype(F);

    // Allocate a virtual register for each global constant array
    const Module::GlobalListType &globalList = TheModule->getGlobalList();
    size_t j = 0;
    for(auto i = globalList.begin(); i != globalList.end(); i ++) {
      const GlobalVariable &v = *i;
      unsigned addrSpace = v.getType()->getAddressSpace();
      if(addrSpace != ir::AddressSpace::MEM_CONSTANT)
        continue;
      GBE_ASSERT(v.hasInitializer());
      ir::Register reg = ctx.reg(ir::RegisterFamily::FAMILY_DWORD);
      ir::Constant &con = unit.getConstantSet().getConstant(j ++);
      con.setReg(reg.value());
      ctx.LOADI(ir::TYPE_S32, reg, ctx.newIntegerImmediate(con.getOffset(), ir::TYPE_S32));
    }

    // Visit all the instructions and emit the IR registers or the value to
    // value mapping when a new register is not needed
    pass = PASS_EMIT_REGISTERS;
    for (inst_iterator I = inst_begin(&F), E = inst_end(&F); I != E; ++I)
      visit(*I);

    // First create all the labels (one per block) ...
    for (Function::iterator BB = F.begin(), E = F.end(); BB != E; ++BB)
      this->newLabelIndex(BB);

    // Then, for all branch instructions that have conditions, see if we can
    // simplify the code by inverting condition code
    for (Function::iterator BB = F.begin(), E = F.end(); BB != E; ++BB)
      this->simplifyTerminator(BB);

    // ... then, emit the instructions for all basic blocks
    pass = PASS_EMIT_INSTRUCTIONS;
    for (Function::iterator BB = F.begin(), E = F.end(); BB != E; ++BB)
      emitBasicBlock(BB);
    ir::Function &fn = ctx.getFunction();
    ctx.endFunction();

    // Liveness can be shared when we optimized the immediates and the MOVs
    const ir::Liveness liveness(fn);

    if (OCL_OPTIMIZE_LOADI) this->removeLOADIs(liveness, fn);
    if (OCL_OPTIMIZE_PHI_MOVES) this->removeMOVs(liveness, fn);
  }

  void GenWriter::regAllocateReturnInst(ReturnInst &I) {}

  void GenWriter::emitReturnInst(ReturnInst &I) {
    const ir::Function &fn = ctx.getFunction();
    GBE_ASSERTM(fn.outputNum() <= 1, "no more than one value can be returned");
    if (fn.outputNum() == 1 && I.getNumOperands() > 0) {
      const ir::Register dst = fn.getOutput(0);
      const ir::Register src = this->getRegister(I.getOperand(0));
      const ir::RegisterFamily family = fn.getRegisterFamily(dst);
      ctx.MOV(ir::getType(family), dst, src);
    }
    ctx.RET();
  }

  void GenWriter::regAllocateBinaryOperator(Instruction &I) {
    this->newRegister(&I);
  }

  void GenWriter::emitBinaryOperator(Instruction &I) {
#if GBE_DEBUG
    GBE_ASSERT(I.getType()->isPointerTy() == false);
    // We accept logical operations on booleans
    switch (I.getOpcode()) {
      case Instruction::And:
      case Instruction::Or:
      case Instruction::Xor:
        break;
      default:
        GBE_ASSERT(I.getType() != Type::getInt1Ty(I.getContext()));
    }
#endif /* GBE_DEBUG */

    // Get the element type for a vector
    const ir::Type type = getType(ctx, I.getType());

    // Emit the instructions in a row
    const ir::Register dst = this->getRegister(&I);
    const ir::Register src0 = this->getRegister(I.getOperand(0));
    const ir::Register src1 = this->getRegister(I.getOperand(1));

    switch (I.getOpcode()) {
      case Instruction::Add:
      case Instruction::FAdd: ctx.ADD(type, dst, src0, src1); break;
      case Instruction::Sub:
      case Instruction::FSub: ctx.SUB(type, dst, src0, src1); break;
      case Instruction::Mul:
      case Instruction::FMul: ctx.MUL(type, dst, src0, src1); break;
      case Instruction::URem: ctx.REM(getUnsignedType(ctx, I.getType()), dst, src0, src1); break;
      case Instruction::SRem:
      case Instruction::FRem: ctx.REM(type, dst, src0, src1); break;
      case Instruction::UDiv: ctx.DIV(getUnsignedType(ctx, I.getType()), dst, src0, src1); break;
      case Instruction::SDiv:
      case Instruction::FDiv: ctx.DIV(type, dst, src0, src1); break;
      case Instruction::And:  ctx.AND(type, dst, src0, src1); break;
      case Instruction::Or:   ctx.OR(type, dst, src0, src1); break;
      case Instruction::Xor:  ctx.XOR(type, dst, src0, src1); break;
      case Instruction::Shl:  ctx.SHL(type, dst, src0, src1); break;
      case Instruction::LShr: ctx.SHR(getUnsignedType(ctx, I.getType()), dst, src0, src1); break;
      case Instruction::AShr: ctx.ASR(type, dst, src0, src1); break;
      default: NOT_SUPPORTED;
    }
  }

  void GenWriter::regAllocateICmpInst(ICmpInst &I) {
    this->newRegister(&I);
  }

  static ir::Type makeTypeSigned(const ir::Type &type) {
    if (type == ir::TYPE_U8) return ir::TYPE_S8;
    else if (type == ir::TYPE_U16) return ir::TYPE_S16;
    else if (type == ir::TYPE_U32) return ir::TYPE_S32;
    else if (type == ir::TYPE_U64) return ir::TYPE_S64;
    return type;
  }

  static ir::Type makeTypeUnsigned(const ir::Type &type) {
    if (type == ir::TYPE_S8) return ir::TYPE_U8;
    else if (type == ir::TYPE_S16) return ir::TYPE_U16;
    else if (type == ir::TYPE_S32) return ir::TYPE_U32;
    else if (type == ir::TYPE_S64) return ir::TYPE_U64;
    return type;
  }

  void GenWriter::emitICmpInst(ICmpInst &I) {
    GBE_ASSERT(I.getOperand(0)->getType() != Type::getInt1Ty(I.getContext()));

    // Get the element type and the number of elements
    Type *operandType = I.getOperand(0)->getType();
    const ir::Type type = getType(ctx, operandType);
    const ir::Type signedType = makeTypeSigned(type);
    const ir::Type unsignedType = makeTypeUnsigned(type);

    // Emit the instructions in a row
    const ir::Register dst = this->getRegister(&I);
    const ir::Register src0 = this->getRegister(I.getOperand(0));
    const ir::Register src1 = this->getRegister(I.getOperand(1));

    // We must invert the condition to simplify the branch code
    if (conditionSet.find(&I) != conditionSet.end()) {
      switch (I.getPredicate()) {
        case ICmpInst::ICMP_EQ:  ctx.NE(type, dst, src0, src1); break;
        case ICmpInst::ICMP_NE:  ctx.EQ(type, dst, src0, src1); break;
        case ICmpInst::ICMP_ULE: ctx.GT((unsignedType), dst, src0, src1); break;
        case ICmpInst::ICMP_SLE: ctx.GT(signedType, dst, src0, src1); break;
        case ICmpInst::ICMP_UGE: ctx.LT(unsignedType, dst, src0, src1); break;
        case ICmpInst::ICMP_SGE: ctx.LT(signedType, dst, src0, src1); break;
        case ICmpInst::ICMP_ULT: ctx.GE(unsignedType, dst, src0, src1); break;
        case ICmpInst::ICMP_SLT: ctx.GE(signedType, dst, src0, src1); break;
        case ICmpInst::ICMP_UGT: ctx.LE(unsignedType, dst, src0, src1); break;
        case ICmpInst::ICMP_SGT: ctx.LE(signedType, dst, src0, src1); break;
        default: NOT_SUPPORTED;
      }
    }
    // Nothing special to do
    else {
      switch (I.getPredicate()) {
        case ICmpInst::ICMP_EQ:  ctx.EQ(type, dst, src0, src1); break;
        case ICmpInst::ICMP_NE:  ctx.NE(type, dst, src0, src1); break;
        case ICmpInst::ICMP_ULE: ctx.LE((unsignedType), dst, src0, src1); break;
        case ICmpInst::ICMP_SLE: ctx.LE(signedType, dst, src0, src1); break;
        case ICmpInst::ICMP_UGE: ctx.GE(unsignedType, dst, src0, src1); break;
        case ICmpInst::ICMP_SGE: ctx.GE(signedType, dst, src0, src1); break;
        case ICmpInst::ICMP_ULT: ctx.LT(unsignedType, dst, src0, src1); break;
        case ICmpInst::ICMP_SLT: ctx.LT(signedType, dst, src0, src1); break;
        case ICmpInst::ICMP_UGT: ctx.GT(unsignedType, dst, src0, src1); break;
        case ICmpInst::ICMP_SGT: ctx.GT(signedType, dst, src0, src1); break;
        default: NOT_SUPPORTED;
      }
    }
  }

  void GenWriter::regAllocateFCmpInst(FCmpInst &I) {
    this->newRegister(&I);
  }

  void GenWriter::emitFCmpInst(FCmpInst &I) {

    // Get the element type and the number of elements
    Type *operandType = I.getOperand(0)->getType();
    const ir::Type type = getType(ctx, operandType);

    // Emit the instructions in a row
    const ir::Register dst = this->getRegister(&I);
    const ir::Register src0 = this->getRegister(I.getOperand(0));
    const ir::Register src1 = this->getRegister(I.getOperand(1));

    switch (I.getPredicate()) {
      case ICmpInst::FCMP_OEQ:
      case ICmpInst::FCMP_UEQ: ctx.EQ(type, dst, src0, src1); break;
      case ICmpInst::FCMP_ONE:
      case ICmpInst::FCMP_UNE: ctx.NE(type, dst, src0, src1); break;
      case ICmpInst::FCMP_OLE:
      case ICmpInst::FCMP_ULE: ctx.LE(type, dst, src0, src1); break;
      case ICmpInst::FCMP_OGE:
      case ICmpInst::FCMP_UGE: ctx.GE(type, dst, src0, src1); break;
      case ICmpInst::FCMP_OLT:
      case ICmpInst::FCMP_ULT: ctx.LT(type, dst, src0, src1); break;
      case ICmpInst::FCMP_OGT:
      case ICmpInst::FCMP_UGT: ctx.GT(type, dst, src0, src1); break;
      default: NOT_SUPPORTED;
    }
  }

  void GenWriter::regAllocateCastInst(CastInst &I) {
    Value *dstValue = &I;
    Value *srcValue = I.getOperand(0);
    const auto op = I.getOpcode();

    switch (op)
    {
      // When casting pointer to integers, be aware with integers
      case Instruction::PtrToInt:
      case Instruction::IntToPtr:
      {
        Constant *CPV = dyn_cast<Constant>(srcValue);
        if (CPV == NULL) {
#if GBE_DEBUG
          Type *dstType = dstValue->getType();
          Type *srcType = srcValue->getType();
          GBE_ASSERT(getTypeByteSize(unit, dstType) == getTypeByteSize(unit, srcType));
#endif /* GBE_DEBUG */
          regTranslator.newValueProxy(srcValue, dstValue);
        } else
          this->newRegister(dstValue);
      }
      break;
      // Bitcast just forward registers
      case Instruction::BitCast:
      {
        regTranslator.newValueProxy(srcValue, dstValue);
      }
      break;
      // Various conversion operations -> just allocate registers for them
      case Instruction::FPToUI:
      case Instruction::FPToSI:
      case Instruction::SIToFP:
      case Instruction::UIToFP:
      case Instruction::SExt:
      case Instruction::ZExt:
      case Instruction::FPExt:
      case Instruction::FPTrunc:
      case Instruction::Trunc:
        this->newRegister(&I);
      break;
      default: NOT_SUPPORTED;
    }
  }

  void GenWriter::emitCastInst(CastInst &I) {
    switch (I.getOpcode())
    {
      case Instruction::PtrToInt:
      case Instruction::IntToPtr:
      {
        Value *dstValue = &I;
        Value *srcValue = I.getOperand(0);
        Constant *CPV = dyn_cast<Constant>(srcValue);
        if (CPV != NULL) {
          const ir::ImmediateIndex index = ctx.newImmediate(CPV);
          const ir::Immediate imm = ctx.getImmediate(index);
          const ir::Register reg = this->getRegister(dstValue);
          ctx.LOADI(imm.type, reg, index);
        }
      }
      break;
      case Instruction::BitCast: break; // nothing to emit here
      case Instruction::FPToUI:
      case Instruction::FPToSI:
      case Instruction::SIToFP:
      case Instruction::UIToFP:
      case Instruction::SExt:
      case Instruction::ZExt:
      case Instruction::FPExt:
      case Instruction::FPTrunc:
      case Instruction::Trunc:
      {
        // Get the element type for a vector
        Type *llvmDstType = I.getType();
        Type *llvmSrcType = I.getOperand(0)->getType();
        const ir::Type dstType = getType(ctx, llvmDstType);
        ir::Type srcType;
        if (I.getOpcode() == Instruction::ZExt) {
          srcType = getUnsignedType(ctx, llvmSrcType);
        } else {
          srcType = getType(ctx, llvmSrcType);
        }

        // We use a select (0,1) not a convert when the destination is a boolean
        if (srcType == ir::TYPE_BOOL) {
          const ir::RegisterFamily family = getFamily(dstType);
          const ir::ImmediateIndex zero = ctx.newIntegerImmediate(0, dstType);
          ir::ImmediateIndex one;
          if (I.getOpcode() == Instruction::SExt
              && (dstType == ir::TYPE_S8 || dstType == ir::TYPE_S16 || dstType == ir::TYPE_S32))
            one = ctx.newIntegerImmediate(-1, dstType);
          else
            one = ctx.newIntegerImmediate(1, dstType);
          const ir::Register zeroReg = ctx.reg(family);
          const ir::Register oneReg = ctx.reg(family);
          ctx.LOADI(dstType, zeroReg, zero);
          ctx.LOADI(dstType, oneReg, one);
          const ir::Register dst = this->getRegister(&I);
          const ir::Register src = this->getRegister(I.getOperand(0));
          ctx.SEL(dstType, dst, src, oneReg, zeroReg);
        }
        // Use a convert for the other cases
        else {
          const ir::Register dst = this->getRegister(&I);
          const ir::Register src = this->getRegister(I.getOperand(0));
          ctx.CVT(dstType, srcType, dst, src);
        }
      }
      break;
      default: NOT_SUPPORTED;
    }
  }

  /*! Once again, it is a templated functor. No lambda */
  struct InsertExtractFunctor {
    InsertExtractFunctor(ir::Context &ctx) : ctx(ctx) {}
    template <typename T> ir::Immediate operator() (const T &t) {
      return ir::Immediate(t);
    }
    ir::Context &ctx;
  };

  /*! Because there are still fake insert/extract instruction for
   *  load/store, so keep empty function here */
  void GenWriter::regAllocateInsertElement(InsertElementInst &I) {}
  void GenWriter::emitInsertElement(InsertElementInst &I) {}

  void GenWriter::regAllocateExtractElement(ExtractElementInst &I) {}
  void GenWriter::emitExtractElement(ExtractElementInst &I) {}

  void GenWriter::regAllocateShuffleVectorInst(ShuffleVectorInst &I) {}
  void GenWriter::emitShuffleVectorInst(ShuffleVectorInst &I) {}

  void GenWriter::regAllocateSelectInst(SelectInst &I) {
    this->newRegister(&I);
  }

  void GenWriter::emitSelectInst(SelectInst &I) {
    // Get the element type for a vector
    const ir::Type type = getType(ctx, I.getType());

    // Emit the instructions in a row
    const ir::Register dst = this->getRegister(&I);
    const ir::Register cond = this->getRegister(I.getOperand(0));
    const ir::Register src0 = this->getRegister(I.getOperand(1));
    const ir::Register src1 = this->getRegister(I.getOperand(2));
    ctx.SEL(type, dst, cond, src0, src1);
  }

  void GenWriter::regAllocatePHINode(PHINode &I) {
    // Copy 1 for the PHI
    this->newRegister(&I);
    // Copy 2 to avoid lost copy issue
    Value *copy = this->getPHICopy(&I);
    this->newRegister(&I, copy);
  }

  void GenWriter::emitPHINode(PHINode &I) {
    Value *copy = this->getPHICopy(&I);
    const ir::Type type = getType(ctx, I.getType());

    const ir::Register dst = this->getRegister(&I);
    const ir::Register src = this->getRegister(copy);
    ctx.MOV(type, dst, src);
  }

  void GenWriter::regAllocateBranchInst(BranchInst &I) {}

  void GenWriter::emitBranchInst(BranchInst &I) {
    // Emit MOVs if required
    BasicBlock *bb = I.getParent();
    this->emitMovForPHI(bb, I.getSuccessor(0));
    if (I.isConditional())
      this->emitMovForPHI(bb, I.getSuccessor(1));

    // Inconditional branch. Just check that we jump to a block which is not our
    // successor
    if (I.isConditional() == false) {
      BasicBlock *target = I.getSuccessor(0);
      if (llvm::next(Function::iterator(bb)) != Function::iterator(target)) {
        GBE_ASSERT(labelMap.find(target) != labelMap.end());
        const ir::LabelIndex labelIndex = labelMap[target];
        ctx.BRA(labelIndex);
      }
    }
    // The LLVM branch has two targets
    else {
      BasicBlock *taken = NULL, *nonTaken = NULL;
      Value *condition = I.getCondition();

      // We may inverted the branch condition to simplify the branching code
      const bool inverted = conditionSet.find(condition) != conditionSet.end();
      taken = inverted ? I.getSuccessor(1) : I.getSuccessor(0);
      nonTaken = inverted ? I.getSuccessor(0) : I.getSuccessor(1);

      // Get both taken label and predicate register
      GBE_ASSERT(labelMap.find(taken) != labelMap.end());
      const ir::LabelIndex index = labelMap[taken];
      const ir::Register reg = this->getRegister(condition);
      ctx.BRA(index, reg);

      // If non-taken target is the next block, there is nothing to do
      BasicBlock *bb = I.getParent();
      if (llvm::next(Function::iterator(bb)) == Function::iterator(nonTaken))
        return;

      // This is slightly more complicated here. We need to issue one more
      // branch for the non-taken condition.
      GBE_ASSERT(labelMap.find(nonTaken) != labelMap.end());
      const ir::LabelIndex untakenIndex = ctx.label();
      ctx.LABEL(untakenIndex);
      ctx.BRA(labelMap[nonTaken]);
    }
  }

  void GenWriter::regAllocateCallInst(CallInst &I) {
    Value *dst = &I;
    Value *Callee = I.getCalledValue();
    GBE_ASSERT(ctx.getFunction().getProfile() == ir::PROFILE_OCL);
    GBE_ASSERT(isa<InlineAsm>(I.getCalledValue()) == false);
    GBE_ASSERT(I.hasStructRetAttr() == false);

    // We only support a small number of intrinsics right now
    if (Function *F = I.getCalledFunction()) {
      const Intrinsic::ID intrinsicID = (Intrinsic::ID) F->getIntrinsicID();
      if (intrinsicID != 0) {
        switch (F->getIntrinsicID()) {
          case Intrinsic::stacksave:
            this->newRegister(&I);
          break;
          case Intrinsic::stackrestore:
          break;
#if LLVM_VERSION_MINOR >= 2
          case Intrinsic::lifetime_start:
          case Intrinsic::lifetime_end:
          break;
          case Intrinsic::fmuladd:
            this->newRegister(&I);
          break;
#endif /* LLVM_VERSION_MINOR >= 2 */
          default:
          GBE_ASSERTM(false, "Unsupported intrinsics");
        }
        return;
      }
    }

    // Get the name of the called function and handle it
    const std::string fnName = Callee->getName();
    auto it = instrinsicMap.map.find(fnName);
    GBE_ASSERT(it != instrinsicMap.map.end());
    switch (it->second) {
      case GEN_OCL_GET_GROUP_ID0:
        regTranslator.newScalarProxy(ir::ocl::groupid0, dst); break;
      case GEN_OCL_GET_GROUP_ID1:
        regTranslator.newScalarProxy(ir::ocl::groupid1, dst); break;
      case GEN_OCL_GET_GROUP_ID2:
        regTranslator.newScalarProxy(ir::ocl::groupid2, dst); break;
      case GEN_OCL_GET_LOCAL_ID0:
        regTranslator.newScalarProxy(ir::ocl::lid0, dst); break;
      case GEN_OCL_GET_LOCAL_ID1:
        regTranslator.newScalarProxy(ir::ocl::lid1, dst); break;
      case GEN_OCL_GET_LOCAL_ID2:
        regTranslator.newScalarProxy(ir::ocl::lid2, dst); break;
      case GEN_OCL_GET_NUM_GROUPS0:
        regTranslator.newScalarProxy(ir::ocl::numgroup0, dst); break;
      case GEN_OCL_GET_NUM_GROUPS1:
        regTranslator.newScalarProxy(ir::ocl::numgroup1, dst); break;
      case GEN_OCL_GET_NUM_GROUPS2:
        regTranslator.newScalarProxy(ir::ocl::numgroup2, dst); break;
      case GEN_OCL_GET_LOCAL_SIZE0:
        regTranslator.newScalarProxy(ir::ocl::lsize0, dst); break;
      case GEN_OCL_GET_LOCAL_SIZE1:
        regTranslator.newScalarProxy(ir::ocl::lsize1, dst); break;
      case GEN_OCL_GET_LOCAL_SIZE2:
        regTranslator.newScalarProxy(ir::ocl::lsize2, dst); break;
      case GEN_OCL_GET_GLOBAL_SIZE0:
        regTranslator.newScalarProxy(ir::ocl::gsize0, dst); break;
      case GEN_OCL_GET_GLOBAL_SIZE1:
        regTranslator.newScalarProxy(ir::ocl::gsize1, dst); break;
      case GEN_OCL_GET_GLOBAL_SIZE2:
        regTranslator.newScalarProxy(ir::ocl::gsize2, dst); break;
      case GEN_OCL_GET_GLOBAL_OFFSET0:
        regTranslator.newScalarProxy(ir::ocl::goffset0, dst); break;
      case GEN_OCL_GET_GLOBAL_OFFSET1:
        regTranslator.newScalarProxy(ir::ocl::goffset1, dst); break;
      case GEN_OCL_GET_GLOBAL_OFFSET2:
        regTranslator.newScalarProxy(ir::ocl::goffset2, dst); break;
      case GEN_OCL_GET_WORK_DIM:
        regTranslator.newScalarProxy(ir::ocl::workdim, dst); break;
      case GEN_OCL_FBH:
      case GEN_OCL_FBL:
      case GEN_OCL_COS:
      case GEN_OCL_SIN:
      case GEN_OCL_SQR:
      case GEN_OCL_RSQ:
      case GEN_OCL_LOG:
      case GEN_OCL_POW:
      case GEN_OCL_RCP:
      case GEN_OCL_ABS:
      case GEN_OCL_FABS:
      case GEN_OCL_RNDZ:
      case GEN_OCL_RNDE:
      case GEN_OCL_RNDU:
      case GEN_OCL_RNDD:
      case GEN_OCL_GET_IMAGE_WIDTH:
      case GEN_OCL_GET_IMAGE_HEIGHT:
      case GEN_OCL_GET_IMAGE_CHANNEL_DATA_TYPE:
      case GEN_OCL_GET_IMAGE_CHANNEL_ORDER:
      case GEN_OCL_GET_IMAGE_DEPTH:
      case GEN_OCL_ATOMIC_ADD0:
      case GEN_OCL_ATOMIC_ADD1:
      case GEN_OCL_ATOMIC_SUB0:
      case GEN_OCL_ATOMIC_SUB1:
      case GEN_OCL_ATOMIC_AND0:
      case GEN_OCL_ATOMIC_AND1:
      case GEN_OCL_ATOMIC_OR0:
      case GEN_OCL_ATOMIC_OR1:
      case GEN_OCL_ATOMIC_XOR0:
      case GEN_OCL_ATOMIC_XOR1:
      case GEN_OCL_ATOMIC_XCHG0:
      case GEN_OCL_ATOMIC_XCHG1:
      case GEN_OCL_ATOMIC_UMAX0:
      case GEN_OCL_ATOMIC_UMAX1:
      case GEN_OCL_ATOMIC_UMIN0:
      case GEN_OCL_ATOMIC_UMIN1:
      case GEN_OCL_ATOMIC_IMAX0:
      case GEN_OCL_ATOMIC_IMAX1:
      case GEN_OCL_ATOMIC_IMIN0:
      case GEN_OCL_ATOMIC_IMIN1:
      case GEN_OCL_ATOMIC_INC0:
      case GEN_OCL_ATOMIC_INC1:
      case GEN_OCL_ATOMIC_DEC0:
      case GEN_OCL_ATOMIC_DEC1:
      case GEN_OCL_ATOMIC_CMPXCHG0:
      case GEN_OCL_ATOMIC_CMPXCHG1:
        // No structure can be returned
        this->newRegister(&I);
        break;
      case GEN_OCL_FORCE_SIMD8:
      case GEN_OCL_FORCE_SIMD16:
      case GEN_OCL_LBARRIER:
      case GEN_OCL_GBARRIER:
      case GEN_OCL_LGBARRIER:
        ctx.getFunction().setUseSLM(true);
        break;
      case GEN_OCL_WRITE_IMAGE0:
      case GEN_OCL_WRITE_IMAGE1:
      case GEN_OCL_WRITE_IMAGE2:
      case GEN_OCL_WRITE_IMAGE3:
      case GEN_OCL_WRITE_IMAGE4:
      case GEN_OCL_WRITE_IMAGE5:
      case GEN_OCL_WRITE_IMAGE10:
      case GEN_OCL_WRITE_IMAGE11:
      case GEN_OCL_WRITE_IMAGE12:
      case GEN_OCL_WRITE_IMAGE13:
      case GEN_OCL_WRITE_IMAGE14:
      case GEN_OCL_WRITE_IMAGE15:
        break;
      case GEN_OCL_READ_IMAGE0:
      case GEN_OCL_READ_IMAGE1:
      case GEN_OCL_READ_IMAGE2:
      case GEN_OCL_READ_IMAGE3:
      case GEN_OCL_READ_IMAGE4:
      case GEN_OCL_READ_IMAGE5:
      case GEN_OCL_READ_IMAGE10:
      case GEN_OCL_READ_IMAGE11:
      case GEN_OCL_READ_IMAGE12:
      case GEN_OCL_READ_IMAGE13:
      case GEN_OCL_READ_IMAGE14:
      case GEN_OCL_READ_IMAGE15:
      {
        // dst is a 4 elements vector. We allocate all 4 registers here.
        uint32_t elemNum;
        (void)getVectorInfo(ctx, I.getType(), &I, elemNum);
        GBE_ASSERT(elemNum == 4);
        this->newRegister(&I);
        break;
      }
      case GEN_OCL_MUL_HI_INT:
      case GEN_OCL_MUL_HI_UINT:
      case GEN_OCL_UPSAMPLE_SHORT:
      case GEN_OCL_UPSAMPLE_INT:
      case GEN_OCL_UPSAMPLE_LONG:
      case GEN_OCL_SADD_SAT_CHAR:
      case GEN_OCL_SADD_SAT_SHORT:
      case GEN_OCL_SADD_SAT_INT:
      case GEN_OCL_SADD_SAT_LONG:
      case GEN_OCL_UADD_SAT_CHAR:
      case GEN_OCL_UADD_SAT_SHORT:
      case GEN_OCL_UADD_SAT_INT:
      case GEN_OCL_UADD_SAT_LONG:
      case GEN_OCL_SSUB_SAT_CHAR:
      case GEN_OCL_SSUB_SAT_SHORT:
      case GEN_OCL_SSUB_SAT_INT:
      case GEN_OCL_SSUB_SAT_LONG:
      case GEN_OCL_USUB_SAT_CHAR:
      case GEN_OCL_USUB_SAT_SHORT:
      case GEN_OCL_USUB_SAT_INT:
      case GEN_OCL_USUB_SAT_LONG:
      case GEN_OCL_HADD:
      case GEN_OCL_RHADD:
        this->newRegister(&I);
        break;
      default:
        GBE_ASSERTM(false, "Function call are not supported yet");
    };
  }

  struct U64CPVExtractFunctor {
    U64CPVExtractFunctor(ir::Context &ctx) : ctx(ctx) {}
    template <typename T> INLINE uint64_t operator() (const T &t) {
      return uint64_t(t);
    }
    ir::Context &ctx;
  };

  void GenWriter::emitUnaryCallInst(CallInst &I, CallSite &CS, ir::Opcode opcode) {
    CallSite::arg_iterator AI = CS.arg_begin();
#if GBE_DEBUG
    CallSite::arg_iterator AE = CS.arg_end();
#endif /* GBE_DEBUG */
    GBE_ASSERT(AI != AE);
    const ir::Register src = this->getRegister(*AI);
    const ir::Register dst = this->getRegister(&I);
    ctx.ALU1(opcode, ir::TYPE_FLOAT, dst, src);
  }

  void GenWriter::emitAtomicInst(CallInst &I, CallSite &CS, ir::AtomicOps opcode) {
    CallSite::arg_iterator AI = CS.arg_begin();
    CallSite::arg_iterator AE = CS.arg_end();
    GBE_ASSERT(AI != AE);

    unsigned int llvmSpace = (*AI)->getType()->getPointerAddressSpace();
    const ir::AddressSpace addrSpace = addressSpaceLLVMToGen(llvmSpace);
    const ir::Register dst = this->getRegister(&I);

    vector<ir::Register> src;
    uint32_t srcNum = 0;
    while(AI != AE) {
      src.push_back(this->getRegister(*(AI++)));
      srcNum++;
    }
    const ir::Tuple srcTuple = ctx.arrayTuple(&src[0], srcNum);
    ctx.ATOMIC(opcode, dst, addrSpace, srcTuple);
  }

  void GenWriter::emitCallInst(CallInst &I) {
    if (Function *F = I.getCalledFunction()) {
      if (F->getIntrinsicID() != 0) {
        const ir::Function &fn = ctx.getFunction();
        switch (F->getIntrinsicID()) {
          case Intrinsic::stacksave:
          {
            const ir::Register dst = this->getRegister(&I);
            const ir::Register src = ir::ocl::stackptr;
            const ir::RegisterFamily family = fn.getRegisterFamily(dst);
            ctx.MOV(ir::getType(family), dst, src);
          }
          break;
          case Intrinsic::stackrestore:
          {
            const ir::Register dst = ir::ocl::stackptr;
            const ir::Register src = this->getRegister(I.getOperand(0));
            const ir::RegisterFamily family = fn.getRegisterFamily(dst);
            ctx.MOV(ir::getType(family), dst, src);
          }
          break;
#if LLVM_VERSION_MINOR >= 2
          case Intrinsic::fmuladd:
          {
            const ir::Register tmp  = ctx.reg(ir::FAMILY_DWORD);
            const ir::Register dst  = this->getRegister(&I);
            const ir::Register src0 = this->getRegister(I.getOperand(0));
            const ir::Register src1 = this->getRegister(I.getOperand(1));
            const ir::Register src2 = this->getRegister(I.getOperand(2));
            ctx.MUL(ir::TYPE_FLOAT, tmp, src0, src1);
            ctx.ADD(ir::TYPE_FLOAT, dst, tmp, src2);
            break;
          }
          break;
          case Intrinsic::lifetime_start:
          case Intrinsic::lifetime_end:
          break;
#endif /* LLVM_VERSION_MINOR >= 2 */
          default: NOT_IMPLEMENTED;
        }
      } else {
        // Get the name of the called function and handle it
        Value *Callee = I.getCalledValue();
        const std::string fnName = Callee->getName();
        auto it = instrinsicMap.map.find(fnName);
        GBE_ASSERT(it != instrinsicMap.map.end());

        // Get the function arguments
        CallSite CS(&I);
        CallSite::arg_iterator AI = CS.arg_begin();
#if GBE_DEBUG
        CallSite::arg_iterator AE = CS.arg_end();
#endif /* GBE_DEBUG */

        switch (it->second) {
          case GEN_OCL_POW:
          {
            const ir::Register src0 = this->getRegister(*AI); ++AI;
            const ir::Register src1 = this->getRegister(*AI);
            const ir::Register dst = this->getRegister(&I);
            ctx.POW(ir::TYPE_FLOAT, dst, src0, src1);
            break;
          }
          case GEN_OCL_FBH: this->emitUnaryCallInst(I,CS,ir::OP_FBH); break;
          case GEN_OCL_FBL: this->emitUnaryCallInst(I,CS,ir::OP_FBL); break;
          case GEN_OCL_ABS:
          {
            const ir::Register src = this->getRegister(*AI);
            const ir::Register dst = this->getRegister(&I);
            ctx.ALU1(ir::OP_ABS, ir::TYPE_S32, dst, src);
            break;
          }
          case GEN_OCL_COS: this->emitUnaryCallInst(I,CS,ir::OP_COS); break;
          case GEN_OCL_SIN: this->emitUnaryCallInst(I,CS,ir::OP_SIN); break;
          case GEN_OCL_LOG: this->emitUnaryCallInst(I,CS,ir::OP_LOG); break;
          case GEN_OCL_SQR: this->emitUnaryCallInst(I,CS,ir::OP_SQR); break;
          case GEN_OCL_RSQ: this->emitUnaryCallInst(I,CS,ir::OP_RSQ); break;
          case GEN_OCL_RCP: this->emitUnaryCallInst(I,CS,ir::OP_RCP); break;
          case GEN_OCL_FABS: this->emitUnaryCallInst(I,CS,ir::OP_ABS); break;
          case GEN_OCL_RNDZ: this->emitUnaryCallInst(I,CS,ir::OP_RNDZ); break;
          case GEN_OCL_RNDE: this->emitUnaryCallInst(I,CS,ir::OP_RNDE); break;
          case GEN_OCL_RNDU: this->emitUnaryCallInst(I,CS,ir::OP_RNDU); break;
          case GEN_OCL_RNDD: this->emitUnaryCallInst(I,CS,ir::OP_RNDD); break;
          case GEN_OCL_FORCE_SIMD8: ctx.setSimdWidth(8); break;
          case GEN_OCL_FORCE_SIMD16: ctx.setSimdWidth(16); break;
          case GEN_OCL_LBARRIER: ctx.SYNC(ir::syncLocalBarrier); break;
          case GEN_OCL_GBARRIER: ctx.SYNC(ir::syncGlobalBarrier); break;
          case GEN_OCL_LGBARRIER: ctx.SYNC(ir::syncLocalBarrier | ir::syncGlobalBarrier); break;
          case GEN_OCL_ATOMIC_ADD0:
          case GEN_OCL_ATOMIC_ADD1: this->emitAtomicInst(I,CS,ir::ATOMIC_OP_ADD); break;
          case GEN_OCL_ATOMIC_SUB0:
          case GEN_OCL_ATOMIC_SUB1: this->emitAtomicInst(I,CS,ir::ATOMIC_OP_SUB); break;
          case GEN_OCL_ATOMIC_AND0:
          case GEN_OCL_ATOMIC_AND1: this->emitAtomicInst(I,CS,ir::ATOMIC_OP_AND); break;
          case GEN_OCL_ATOMIC_OR0:
          case GEN_OCL_ATOMIC_OR1: this->emitAtomicInst(I,CS,ir::ATOMIC_OP_OR); break;
          case GEN_OCL_ATOMIC_XOR0:
          case GEN_OCL_ATOMIC_XOR1: this->emitAtomicInst(I,CS,ir::ATOMIC_OP_XOR); break;
          case GEN_OCL_ATOMIC_XCHG0:
          case GEN_OCL_ATOMIC_XCHG1: this->emitAtomicInst(I,CS,ir::ATOMIC_OP_XCHG); break;
          case GEN_OCL_ATOMIC_INC0:
          case GEN_OCL_ATOMIC_INC1: this->emitAtomicInst(I,CS,ir::ATOMIC_OP_INC); break;
          case GEN_OCL_ATOMIC_DEC0:
          case GEN_OCL_ATOMIC_DEC1: this->emitAtomicInst(I,CS,ir::ATOMIC_OP_DEC); break;
          case GEN_OCL_ATOMIC_UMIN0:
          case GEN_OCL_ATOMIC_UMIN1: this->emitAtomicInst(I,CS,ir::ATOMIC_OP_UMIN); break;
          case GEN_OCL_ATOMIC_UMAX0:
          case GEN_OCL_ATOMIC_UMAX1: this->emitAtomicInst(I,CS,ir::ATOMIC_OP_UMAX); break;
          case GEN_OCL_ATOMIC_IMIN0:
          case GEN_OCL_ATOMIC_IMIN1: this->emitAtomicInst(I,CS,ir::ATOMIC_OP_IMIN); break;
          case GEN_OCL_ATOMIC_IMAX0:
          case GEN_OCL_ATOMIC_IMAX1: this->emitAtomicInst(I,CS,ir::ATOMIC_OP_IMAX); break;
          case GEN_OCL_ATOMIC_CMPXCHG0:
          case GEN_OCL_ATOMIC_CMPXCHG1: this->emitAtomicInst(I,CS,ir::ATOMIC_OP_CMPXCHG); break;
          case GEN_OCL_GET_IMAGE_WIDTH:
          case GEN_OCL_GET_IMAGE_HEIGHT:
          case GEN_OCL_GET_IMAGE_DEPTH:
          case GEN_OCL_GET_IMAGE_CHANNEL_DATA_TYPE:
          case GEN_OCL_GET_IMAGE_CHANNEL_ORDER:
          {
            GBE_ASSERT(AI != AE); const ir::Register surface_id = this->getRegister(*AI); ++AI;
            uint32_t elemNum;
            (void)getVectorInfo(ctx, I.getType(), &I, elemNum);
            vector<ir::Register> dstTupleData;
            ir::Register lastReg;
            for (uint32_t elemID = 0; elemID < elemNum; ++elemID) {
              const ir::Register reg = this->getRegister(&I, elemID);
              dstTupleData.push_back(reg);
              lastReg = reg;
            }
            // A walk around for the gen IR limitation.
            for (uint32_t elemID = elemNum; elemID < 4; ++ elemID) {
              dstTupleData.push_back(lastReg);
            }
            const ir::Tuple dstTuple = ctx.arrayTuple(&dstTupleData[0], 4);
            int infoType = it->second - GEN_OCL_GET_IMAGE_WIDTH;

            ctx.GET_IMAGE_INFO(infoType, dstTuple, surface_id);
            break;
          }
          case GEN_OCL_READ_IMAGE0:
          case GEN_OCL_READ_IMAGE1:
          case GEN_OCL_READ_IMAGE2:
          case GEN_OCL_READ_IMAGE3:
          case GEN_OCL_READ_IMAGE4:
          case GEN_OCL_READ_IMAGE5:
          case GEN_OCL_READ_IMAGE10:
          case GEN_OCL_READ_IMAGE11:
          case GEN_OCL_READ_IMAGE12:
          case GEN_OCL_READ_IMAGE13:
          case GEN_OCL_READ_IMAGE14:
          case GEN_OCL_READ_IMAGE15:
          {
            GBE_ASSERT(AI != AE); const ir::Register surface_id = this->getRegister(*AI); ++AI;
            GBE_ASSERT(AI != AE);
            Constant *CPV = dyn_cast<Constant>(*AI);
            ir::Register sampler;
            if (CPV != NULL)
            {
              // This is not a kernel argument sampler, we need to append it to sampler set,
              // and allocate a sampler slot for it.
              auto x = processConstant<ir::Immediate>(CPV, InsertExtractFunctor(ctx));
              GBE_ASSERTM(x.type == ir::TYPE_U32 || x.type == ir::TYPE_S32, "Invalid sampler type");
              sampler = ctx.getFunction().getSamplerSet()->append(x.data.u32, &ctx);
            } else {
              sampler = this->getRegister(*AI);
              ctx.getFunction().getSamplerSet()->append(sampler, &ctx);
            }
            ++AI;

            GBE_ASSERT(AI != AE); const ir::Register ucoord = this->getRegister(*AI); ++AI;
            GBE_ASSERT(AI != AE); const ir::Register vcoord = this->getRegister(*AI); ++AI;
            ir::Register wcoord;
            if (it->second == GEN_OCL_READ_IMAGE10 ||
                it->second == GEN_OCL_READ_IMAGE11 ||
                it->second == GEN_OCL_READ_IMAGE12 ||
                it->second == GEN_OCL_READ_IMAGE13 ||
                it->second == GEN_OCL_READ_IMAGE14) {
              GBE_ASSERT(AI != AE); wcoord = this->getRegister(*AI); ++AI;
            } else
              wcoord = ir::Register(0);

            vector<ir::Register> dstTupleData, srcTupleData;
            const uint32_t elemNum = 4;
            for (uint32_t elemID = 0; elemID < elemNum; ++elemID) {
              const ir::Register reg = this->getRegister(&I, elemID);
              dstTupleData.push_back(reg);
            }
            srcTupleData.push_back(surface_id);
            srcTupleData.push_back(sampler);
            srcTupleData.push_back(ucoord);
            srcTupleData.push_back(vcoord);
            srcTupleData.push_back(wcoord);
            const ir::Tuple dstTuple = ctx.arrayTuple(&dstTupleData[0], elemNum);
            const ir::Tuple srcTuple = ctx.arrayTuple(&srcTupleData[0], 5);

            ir::Type srcType = ir::TYPE_S32, dstType = ir::TYPE_U32;

            switch(it->second) {
              case GEN_OCL_READ_IMAGE0:
              case GEN_OCL_READ_IMAGE2:
              case GEN_OCL_READ_IMAGE10:
              case GEN_OCL_READ_IMAGE12:
                dstType = ir::TYPE_U32;
                srcType = ir::TYPE_S32;
                break;
              case GEN_OCL_READ_IMAGE1:
              case GEN_OCL_READ_IMAGE3:
              case GEN_OCL_READ_IMAGE11:
              case GEN_OCL_READ_IMAGE13:
                dstType = ir::TYPE_U32;
                srcType = ir::TYPE_FLOAT;
                break;
              case GEN_OCL_READ_IMAGE4:
              case GEN_OCL_READ_IMAGE14:
                dstType = ir::TYPE_FLOAT;
                srcType = ir::TYPE_S32;
                break;
              case GEN_OCL_READ_IMAGE5:
              case GEN_OCL_READ_IMAGE15:
                srcType = dstType = ir::TYPE_FLOAT;
                break;
              default:
                GBE_ASSERT(0); // never been here.
            }

            ctx.SAMPLE(dstTuple, srcTuple, dstType, srcType);
            break;
          }
          case GEN_OCL_WRITE_IMAGE0:
          case GEN_OCL_WRITE_IMAGE1:
          case GEN_OCL_WRITE_IMAGE2:
          case GEN_OCL_WRITE_IMAGE3:
          case GEN_OCL_WRITE_IMAGE4:
          case GEN_OCL_WRITE_IMAGE5:
          case GEN_OCL_WRITE_IMAGE10:
          case GEN_OCL_WRITE_IMAGE11:
          case GEN_OCL_WRITE_IMAGE12:
          case GEN_OCL_WRITE_IMAGE13:
          case GEN_OCL_WRITE_IMAGE14:
          case GEN_OCL_WRITE_IMAGE15:
          {
            GBE_ASSERT(AI != AE); const ir::Register surface_id = this->getRegister(*AI); ++AI;
            GBE_ASSERT(AI != AE); const ir::Register ucoord = this->getRegister(*AI); ++AI;
            GBE_ASSERT(AI != AE); const ir::Register vcoord = this->getRegister(*AI); ++AI;
            ir::Register wcoord;
            if(it->second == GEN_OCL_WRITE_IMAGE10 ||
               it->second == GEN_OCL_WRITE_IMAGE11 ||
               it->second == GEN_OCL_WRITE_IMAGE12 ||
               it->second == GEN_OCL_WRITE_IMAGE13 ||
               it->second == GEN_OCL_WRITE_IMAGE14) {
              GBE_ASSERT(AI != AE); wcoord = this->getRegister(*AI); ++AI;
            } else
              wcoord = ir::Register(0);
            GBE_ASSERT(AI != AE);
            vector<ir::Register> srcTupleData;

            srcTupleData.push_back(surface_id);
            srcTupleData.push_back(ucoord);
            srcTupleData.push_back(vcoord);
            srcTupleData.push_back(wcoord);

            const uint32_t elemNum = 4;
            for (uint32_t elemID = 0; elemID < elemNum; ++elemID) {
              const ir::Register reg = this->getRegister(*AI, elemID);
              srcTupleData.push_back(reg);
            }
            const ir::Tuple srcTuple = ctx.arrayTuple(&srcTupleData[0], 8);

            ir::Type srcType = ir::TYPE_U32, coordType = ir::TYPE_U32;

            switch(it->second) {
              case GEN_OCL_WRITE_IMAGE0:
              case GEN_OCL_WRITE_IMAGE2:
              case GEN_OCL_WRITE_IMAGE10:
              case GEN_OCL_WRITE_IMAGE12:
                srcType = coordType = ir::TYPE_U32;
                break;
              case GEN_OCL_WRITE_IMAGE1:
              case GEN_OCL_WRITE_IMAGE3:
              case GEN_OCL_WRITE_IMAGE11:
              case GEN_OCL_WRITE_IMAGE13:
                coordType = ir::TYPE_FLOAT;
                srcType = ir::TYPE_U32;
                break;
              case GEN_OCL_WRITE_IMAGE4:
              case GEN_OCL_WRITE_IMAGE14:
                srcType = ir::TYPE_FLOAT;
                coordType = ir::TYPE_U32;
                break;
              case GEN_OCL_WRITE_IMAGE5:
              case GEN_OCL_WRITE_IMAGE15:
                srcType = coordType = ir::TYPE_FLOAT;
                break;
              default:
                GBE_ASSERT(0); // never been here.
            }

            ctx.TYPED_WRITE(srcTuple, srcType, coordType);
            break;
          }
          case GEN_OCL_MUL_HI_INT:
          {
            GBE_ASSERT(AI != AE); const ir::Register src0 = this->getRegister(*AI); ++AI;
            GBE_ASSERT(AI != AE); const ir::Register src1 = this->getRegister(*AI); ++AI;
            const ir::Register dst = this->getRegister(&I);
            ctx.MUL_HI(getType(ctx, I.getType()), dst, src0, src1);
            break;
          }
          case GEN_OCL_MUL_HI_UINT:
          {
            GBE_ASSERT(AI != AE); const ir::Register src0 = this->getRegister(*AI); ++AI;
            GBE_ASSERT(AI != AE); const ir::Register src1 = this->getRegister(*AI); ++AI;
            const ir::Register dst = this->getRegister(&I);
            ctx.MUL_HI(getUnsignedType(ctx, I.getType()), dst, src0, src1);
            break;
          }
          case GEN_OCL_UPSAMPLE_SHORT:
          {
            GBE_ASSERT(AI != AE); const ir::Register src0 = this->getRegister(*AI); ++AI;
            GBE_ASSERT(AI != AE); const ir::Register src1 = this->getRegister(*AI); ++AI;
            const ir::Register dst = this->getRegister(&I);
            ctx.UPSAMPLE_SHORT(getType(ctx, I.getType()), dst, src0, src1);
            break;
          }
          case GEN_OCL_UPSAMPLE_INT:
          {
            GBE_ASSERT(AI != AE); const ir::Register src0 = this->getRegister(*AI); ++AI;
            GBE_ASSERT(AI != AE); const ir::Register src1 = this->getRegister(*AI); ++AI;
            const ir::Register dst = this->getRegister(&I);
            ctx.UPSAMPLE_INT(getType(ctx, I.getType()), dst, src0, src1);
            break;
          }
          case GEN_OCL_UPSAMPLE_LONG:
          {
            GBE_ASSERT(AI != AE); const ir::Register src0 = this->getRegister(*AI); ++AI;
            GBE_ASSERT(AI != AE); const ir::Register src1 = this->getRegister(*AI); ++AI;
            const ir::Register dst = this->getRegister(&I);
            ctx.UPSAMPLE_LONG(getType(ctx, I.getType()), dst, src0, src1);
            break;
          }
          case GEN_OCL_SADD_SAT_CHAR:
          case GEN_OCL_SADD_SAT_SHORT:
          case GEN_OCL_SADD_SAT_INT:
          case GEN_OCL_SADD_SAT_LONG:
          {
            GBE_ASSERT(AI != AE); const ir::Register src0 = this->getRegister(*AI); ++AI;
            GBE_ASSERT(AI != AE); const ir::Register src1 = this->getRegister(*AI); ++AI;
            const ir::Register dst = this->getRegister(&I);
            ctx.ADDSAT(getType(ctx, I.getType()), dst, src0, src1);
            break;
          }
          case GEN_OCL_UADD_SAT_CHAR:
          case GEN_OCL_UADD_SAT_SHORT:
          case GEN_OCL_UADD_SAT_INT:
          case GEN_OCL_UADD_SAT_LONG:
          {
            GBE_ASSERT(AI != AE); const ir::Register src0 = this->getRegister(*AI); ++AI;
            GBE_ASSERT(AI != AE); const ir::Register src1 = this->getRegister(*AI); ++AI;
            const ir::Register dst = this->getRegister(&I);
            ctx.ADDSAT(getUnsignedType(ctx, I.getType()), dst, src0, src1);
            break;
          }
          case GEN_OCL_SSUB_SAT_CHAR:
          case GEN_OCL_SSUB_SAT_SHORT:
          case GEN_OCL_SSUB_SAT_INT:
          case GEN_OCL_SSUB_SAT_LONG:
          {
            GBE_ASSERT(AI != AE); const ir::Register src0 = this->getRegister(*AI); ++AI;
            GBE_ASSERT(AI != AE); const ir::Register src1 = this->getRegister(*AI); ++AI;
            const ir::Register dst = this->getRegister(&I);
            ctx.SUBSAT(getType(ctx, I.getType()), dst, src0, src1);
            break;
          }
          case GEN_OCL_USUB_SAT_CHAR:
          case GEN_OCL_USUB_SAT_SHORT:
          case GEN_OCL_USUB_SAT_INT:
          case GEN_OCL_USUB_SAT_LONG:
          {
            GBE_ASSERT(AI != AE); const ir::Register src0 = this->getRegister(*AI); ++AI;
            GBE_ASSERT(AI != AE); const ir::Register src1 = this->getRegister(*AI); ++AI;
            const ir::Register dst = this->getRegister(&I);
            ctx.SUBSAT(getUnsignedType(ctx, I.getType()), dst, src0, src1);
            break;
          }
          case GEN_OCL_HADD: {
            GBE_ASSERT(AI != AE); const ir::Register src0 = this->getRegister(*AI); ++AI;
            GBE_ASSERT(AI != AE); const ir::Register src1 = this->getRegister(*AI); ++AI;
            const ir::Register dst = this->getRegister(&I);
            ctx.HADD(getUnsignedType(ctx, I.getType()), dst, src0, src1);
            break;
          }
          case GEN_OCL_RHADD: {
            GBE_ASSERT(AI != AE); const ir::Register src0 = this->getRegister(*AI); ++AI;
            GBE_ASSERT(AI != AE); const ir::Register src1 = this->getRegister(*AI); ++AI;
            const ir::Register dst = this->getRegister(&I);
            ctx.RHADD(getUnsignedType(ctx, I.getType()), dst, src0, src1);
            break;
          }
          default: break;
        }
      }
    }
  }

  void GenWriter::regAllocateAllocaInst(AllocaInst &I) {
    this->newRegister(&I);
  }
  void GenWriter::emitAllocaInst(AllocaInst &I) {
    Value *src = I.getOperand(0);
    Type *elemType = I.getType()->getElementType();
    ir::ImmediateIndex immIndex;
    bool needMultiply = true;

    // Be aware, we manipulate pointers
    if (ctx.getPointerSize() == ir::POINTER_32_BITS)
      immIndex = ctx.newImmediate(uint32_t(getTypeByteSize(unit, elemType)));
    else
      immIndex = ctx.newImmediate(uint64_t(getTypeByteSize(unit, elemType)));

    // OK, we try to see if we know compile time the size we need to allocate
    if (I.isArrayAllocation() == false) // one element allocated only
      needMultiply = false;
    else {
      Constant *CPV = dyn_cast<Constant>(src);
      if (CPV) {
        const uint64_t elemNum = processConstant<uint64_t>(CPV, U64CPVExtractFunctor(ctx));
        ir::Immediate imm = ctx.getImmediate(immIndex);
        imm.data.u64 = ALIGN(imm.data.u64 * elemNum, 4);
        ctx.setImmediate(immIndex, imm);
        needMultiply = false;
      } else {
        // Brutal but cheap way to get arrays aligned on 4 bytes: we just align
        // the element on 4 bytes!
        ir::Immediate imm = ctx.getImmediate(immIndex);
        imm.data.u64 = ALIGN(imm.data.u64, 4);
        ctx.setImmediate(immIndex, imm);
      }
    }

    // Now emit the stream of instructions to get the allocated pointer
    const ir::RegisterFamily pointerFamily = ctx.getPointerFamily();
    const ir::Register dst = this->getRegister(&I);
    const ir::Register stack = ir::ocl::stackptr;
    const ir::Register reg = ctx.reg(pointerFamily);
    const ir::Immediate imm = ctx.getImmediate(immIndex);

    // Set the destination register properly
    ctx.MOV(imm.type, dst, stack);

    // Easy case, we just increment the stack pointer
    if (needMultiply == false) {
      ctx.LOADI(imm.type, reg, immIndex);
      ctx.ADD(imm.type, stack, stack, reg);
    }
    // Harder case (variable length array) that requires a multiply
    else {
      ctx.LOADI(imm.type, reg, immIndex);
      ctx.MUL(imm.type, reg, this->getRegister(src), reg);
      ctx.ADD(imm.type, stack, stack, reg);
    }
  }

  static INLINE Value *getLoadOrStoreValue(LoadInst &I) {
    return &I;
  }
  static INLINE Value *getLoadOrStoreValue(StoreInst &I) {
    return I.getValueOperand();
  }
  void GenWriter::regAllocateLoadInst(LoadInst &I) {
    this->newRegister(&I);
  }
  void GenWriter::regAllocateStoreInst(StoreInst &I) {}

  extern int OCL_SIMD_WIDTH;
  template <bool isLoad, typename T>
  INLINE void GenWriter::emitLoadOrStore(T &I)
  {
    unsigned int llvmSpace = I.getPointerAddressSpace();
    Value *llvmPtr = I.getPointerOperand();
    Value *llvmValues = getLoadOrStoreValue(I);
    Type *llvmType = llvmValues->getType();
    const bool dwAligned = (I.getAlignment() % 4) == 0;
    const ir::AddressSpace addrSpace = addressSpaceLLVMToGen(llvmSpace);
    const ir::Register ptr = this->getRegister(llvmPtr);

    // Scalar is easy. We neednot build register tuples
    if (isScalarType(llvmType) == true) {
      const ir::Type type = getType(ctx, llvmType);
      const ir::Register values = this->getRegister(llvmValues);
      if (isLoad)
        ctx.LOAD(type, ptr, addrSpace, dwAligned, values);
      else
        ctx.STORE(type, ptr, addrSpace, dwAligned, values);
    }
    // A vector type requires to build a tuple
    else {
      VectorType *vectorType = cast<VectorType>(llvmType);
      Type *elemType = vectorType->getElementType();

      // We follow OCL spec and support 2,3,4,8,16 elements only
      uint32_t elemNum = vectorType->getNumElements();
      GBE_ASSERTM(elemNum == 2 || elemNum == 3 || elemNum == 4 || elemNum == 8 || elemNum == 16,
                  "Only vectors of 2,3,4,8 or 16 elements are supported");
      // Per OPenCL 1.2 spec 6.1.5:
      //   For 3-component vector data types, the size of the data type is 4 * sizeof(component).
      // And the llvm does cast a type3 data to type4 for load/store instruction,
      // so a 4 elements vector may only have 3 valid elements. We need to fix it to correct element
      // count here.
      if (elemNum == 4 && regTranslator.isUndefConst(llvmValues, 3))
          elemNum = 3;
      // The code is going to be fairly different from types to types (based on
      // size of each vector element)
      const ir::Type type = getType(ctx, elemType);
      const ir::RegisterFamily pointerFamily = ctx.getPointerFamily();

      if ((type == ir::TYPE_FLOAT || type == ir::TYPE_U32 || type == ir::TYPE_S32) && addrSpace != ir::MEM_CONSTANT) {
        // One message is enough here. Nothing special to do
        if (elemNum <= 4) {
          // Build the tuple data in the vector
          vector<ir::Register> tupleData; // put registers here
          for (uint32_t elemID = 0; elemID < elemNum; ++elemID) {
            const ir::Register reg = this->getRegister(llvmValues, elemID);
            tupleData.push_back(reg);
          }
          const ir::Tuple tuple = ctx.arrayTuple(&tupleData[0], elemNum);

          // Emit the instruction
          if (isLoad)
            ctx.LOAD(type, tuple, ptr, addrSpace, elemNum, dwAligned);
          else
            ctx.STORE(type, tuple, ptr, addrSpace, elemNum, dwAligned);
        }
        // Not supported by the hardware. So, we split the message and we use
        // strided loads and stores
        else {
          // We simply use several uint4 loads
          const uint32_t msgNum = elemNum / 4;
          for (uint32_t msg = 0; msg < msgNum; ++msg) {
            // Build the tuple data in the vector
            vector<ir::Register> tupleData; // put registers here
            for (uint32_t elemID = 0; elemID < 4; ++elemID) {
              const ir::Register reg = this->getRegister(llvmValues, 4*msg+elemID);
              tupleData.push_back(reg);
            }
            const ir::Tuple tuple = ctx.arrayTuple(&tupleData[0], 4);

            // We may need to update to offset the pointer
            ir::Register addr;
            if (msg == 0)
              addr = ptr;
            else {
              const ir::Register offset = ctx.reg(pointerFamily);
              ir::ImmediateIndex immIndex;
              ir::Type immType;
              if (pointerFamily == ir::FAMILY_DWORD) {
                immIndex = ctx.newImmediate(int32_t(msg*sizeof(uint32_t[4])));
                immType = ir::TYPE_S32;
              } else {
                immIndex = ctx.newImmediate(int64_t(msg*sizeof(uint64_t[4])));
                immType = ir::TYPE_S64;
              }

              addr = ctx.reg(pointerFamily);
              ctx.LOADI(immType, offset, immIndex);
              ctx.ADD(immType, addr, ptr, offset);
            }

            // Emit the instruction
            if (isLoad)
              ctx.LOAD(type, tuple, addr, addrSpace, 4, true);
            else
              ctx.STORE(type, tuple, addr, addrSpace, 4, true);
          }
        }
      } else {
        for (uint32_t elemID = 0; elemID < elemNum; elemID++) {
          const ir::Register reg = this->getRegister(llvmValues, elemID);
          ir::Register addr;
          if (elemID == 0)
            addr = ptr;
          else {
              const ir::Register offset = ctx.reg(pointerFamily);
              ir::ImmediateIndex immIndex;
              int elemSize = getTypeByteSize(unit, elemType);
              immIndex = ctx.newImmediate(int32_t(elemID * elemSize));
              addr = ctx.reg(pointerFamily);
              ctx.LOADI(ir::TYPE_S32, offset, immIndex);
              ctx.ADD(ir::TYPE_S32, addr, ptr, offset);
          }
          if (isLoad)
           ctx.LOAD(type, addr, addrSpace, dwAligned, reg);
          else
           ctx.STORE(type, addr, addrSpace, dwAligned, reg);
        }
      }
    }
  }

  void GenWriter::emitLoadInst(LoadInst &I) {
    this->emitLoadOrStore<true>(I);
  }

  void GenWriter::emitStoreInst(StoreInst &I) {
    this->emitLoadOrStore<false>(I);
  }

  llvm::FunctionPass *createGenPass(ir::Unit &unit) {
    return new GenWriter(unit);
  }
} /* namespace gbe */

