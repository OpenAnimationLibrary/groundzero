#pragma once

// The pinned expression emitter predates LLVM's explicitly typed builder
// interfaces. Keep this compatibility layer private to the qualified backend.
#include <llvm/IR/Constants.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Module.h>

class SeExprIRBuilder : public llvm::IRBuilder<> {
  using Base = llvm::IRBuilder<>;

  static llvm::Type *element(llvm::Type *type, llvm::Value *pointer) {
    return type ? type : pointer->getType()->getPointerElementType();
  }

public:
  using Base::Base;
  using Base::CreateCall;
  using Base::CreateLoad;

  llvm::LoadInst *CreateLoad(llvm::Value *pointer,
                             const llvm::Twine &name = "") {
    return Base::CreateLoad(element(nullptr, pointer), pointer, name);
  }

  llvm::CallInst *CreateCall(llvm::Value *callee,
                             llvm::ArrayRef<llvm::Value *> arguments,
                             const llvm::Twine &name = "") {
    auto *type = llvm::cast<llvm::FunctionType>(
        callee->getType()->getPointerElementType());
    return Base::CreateCall(type, callee, arguments, name);
  }

  llvm::Value *CreateGEP(llvm::Type *type, llvm::Value *pointer,
                         llvm::Value *index, const llvm::Twine &name = "") {
    return Base::CreateGEP(element(type, pointer), pointer, index, name);
  }

  llvm::Value *CreateInBoundsGEP(llvm::Type *type, llvm::Value *pointer,
                                 llvm::Value *index,
                                 const llvm::Twine &name = "") {
    return Base::CreateInBoundsGEP(element(type, pointer), pointer, index, name);
  }

  llvm::Value *CreateInBoundsGEP(llvm::Value *pointer, llvm::Value *index,
                                 const llvm::Twine &name = "") {
    return CreateInBoundsGEP(nullptr, pointer, index, name);
  }

  llvm::Value *CreateConstGEP1_32(llvm::Type *type, llvm::Value *pointer,
                                  unsigned index,
                                  const llvm::Twine &name = "") {
    return Base::CreateConstGEP1_32(element(type, pointer), pointer, index, name);
  }

  llvm::Value *CreateConstGEP1_32(llvm::Value *pointer, unsigned index,
                                  const llvm::Twine &name = "") {
    return CreateConstGEP1_32(nullptr, pointer, index, name);
  }

  llvm::Value *CreateConstInBoundsGEP1_32(llvm::Type *type,
                                          llvm::Value *pointer, unsigned index,
                                          const llvm::Twine &name = "") {
    return Base::CreateConstInBoundsGEP1_32(element(type, pointer), pointer,
                                           index, name);
  }

  llvm::Value *CreateConstGEP2_32(llvm::Type *type, llvm::Value *pointer,
                                  unsigned first, unsigned second,
                                  const llvm::Twine &name = "") {
    return Base::CreateConstGEP2_32(element(type, pointer), pointer, first,
                                    second, name);
  }
};

inline unsigned seexprLLVMVectorElements(llvm::Type *type) {
  return llvm::cast<llvm::FixedVectorType>(type)->getNumElements();
}

inline void seexprLLVMCheckBudget(SeExprIRBuilder &builder) {
  llvm::BasicBlock *current = builder.GetInsertBlock();
  llvm::Function *function = current->getParent();
  llvm::Module *module = function->getParent();
  auto *check = module->getFunction("GroundzeroSeExprBudgetStep");
  auto *proceed = llvm::BasicBlock::Create(builder.getContext(), "budget.ok", function);
  auto *abort = llvm::BasicBlock::Create(builder.getContext(), "budget.abort", function);
  builder.CreateCondBr(builder.CreateCall(check, {}), proceed, abort);
  builder.SetInsertPoint(abort);
  if (function->getReturnType()->isVoidTy())
    builder.CreateRetVoid();
  else
    builder.CreateRet(llvm::Constant::getNullValue(function->getReturnType()));
  builder.SetInsertPoint(proceed);
}
