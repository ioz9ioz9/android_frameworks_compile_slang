/*
 * Copyright 2010, The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "slang_rs_backend.h"

#include <string>
#include <vector>

#include "llvm/ADT/Twine.h"
#include "llvm/ADT/StringExtras.h"

#include "llvm/Constant.h"
#include "llvm/Constants.h"
#include "llvm/DerivedTypes.h"
#include "llvm/Function.h"
#include "llvm/Metadata.h"
#include "llvm/Module.h"

#include "llvm/Support/IRBuilder.h"

#include "slang_assert.h"
#include "slang_rs.h"
#include "slang_rs_context.h"
#include "slang_rs_export_foreach.h"
#include "slang_rs_export_func.h"
#include "slang_rs_export_type.h"
#include "slang_rs_export_var.h"
#include "slang_rs_metadata.h"

namespace slang {

RSBackend::RSBackend(RSContext *Context,
                     clang::DiagnosticsEngine *DiagEngine,
                     const clang::CodeGenOptions &CodeGenOpts,
                     const clang::TargetOptions &TargetOpts,
                     PragmaList *Pragmas,
                     llvm::raw_ostream *OS,
                     Slang::OutputType OT,
                     clang::SourceManager &SourceMgr,
                     bool AllowRSPrefix)
  : Backend(DiagEngine, CodeGenOpts, TargetOpts, Pragmas, OS, OT),
    mContext(Context),
    mSourceMgr(SourceMgr),
    mAllowRSPrefix(AllowRSPrefix),
    mExportVarMetadata(NULL),
    mExportFuncMetadata(NULL),
    mExportForEachMetadata(NULL),
    mExportTypeMetadata(NULL),
    mRSObjectSlotsMetadata(NULL),
    mRefCount(mContext->getASTContext()) {
}

// 1) Add zero initialization of local RS object types
void RSBackend::AnnotateFunction(clang::FunctionDecl *FD) {
  if (FD &&
      FD->hasBody() &&
      !SlangRS::IsFunctionInRSHeaderFile(FD, mSourceMgr)) {
    mRefCount.Init();
    mRefCount.Visit(FD->getBody());
  }
  return;
}

void RSBackend::HandleTopLevelDecl(clang::DeclGroupRef D) {
  // Disallow user-defined functions with prefix "rs"
  if (!mAllowRSPrefix) {
    // Iterate all function declarations in the program.
    for (clang::DeclGroupRef::iterator I = D.begin(), E = D.end();
         I != E; I++) {
      clang::FunctionDecl *FD = llvm::dyn_cast<clang::FunctionDecl>(*I);
      if (FD == NULL)
        continue;
      if (!FD->getName().startswith("rs"))  // Check prefix
        continue;
      if (!SlangRS::IsFunctionInRSHeaderFile(FD, mSourceMgr))
        mDiagEngine.Report(
          clang::FullSourceLoc(FD->getLocation(), mSourceMgr),
          mDiagEngine.getCustomDiagID(clang::DiagnosticsEngine::Error,
                                      "invalid function name prefix, "
                                      "\"rs\" is reserved: '%0'"))
          << FD->getName();
    }
  }

  // Process any non-static function declarations
  for (clang::DeclGroupRef::iterator I = D.begin(), E = D.end(); I != E; I++) {
    clang::FunctionDecl *FD = llvm::dyn_cast<clang::FunctionDecl>(*I);
    if (FD && FD->isGlobal()) {
      AnnotateFunction(FD);
    }
  }

  Backend::HandleTopLevelDecl(D);
  return;
}

namespace {

static bool ValidateVarDecl(clang::VarDecl *VD) {
  if (!VD) {
    return true;
  }

  clang::ASTContext &C = VD->getASTContext();
  const clang::Type *T = VD->getType().getTypePtr();
  bool valid = true;

  if (VD->getLinkage() == clang::ExternalLinkage) {
    llvm::StringRef TypeName;
    if (!RSExportType::NormalizeType(T, TypeName, &C.getDiagnostics(), VD)) {
      valid = false;
    }
  }
  valid &= RSExportType::ValidateVarDecl(VD);

  return valid;
}

static bool ValidateASTContext(clang::ASTContext &C) {
  bool valid = true;
  clang::TranslationUnitDecl *TUDecl = C.getTranslationUnitDecl();
  for (clang::DeclContext::decl_iterator DI = TUDecl->decls_begin(),
          DE = TUDecl->decls_end();
       DI != DE;
       DI++) {
    clang::VarDecl *VD = llvm::dyn_cast<clang::VarDecl>(*DI);
    if (VD && !ValidateVarDecl(VD)) {
      valid = false;
    }
  }

  return valid;
}

}  // namespace

void RSBackend::HandleTranslationUnitPre(clang::ASTContext &C) {
  clang::TranslationUnitDecl *TUDecl = C.getTranslationUnitDecl();

  if (!ValidateASTContext(C)) {
    return;
  }

  int version = mContext->getVersion();
  if (version == 0) {
    // Not setting a version is an error
    mDiagEngine.Report(mDiagEngine.getCustomDiagID(
      clang::DiagnosticsEngine::Error,
      "Missing pragma for version in source file"));
  } else if (version > 1) {
    mDiagEngine.Report(mDiagEngine.getCustomDiagID(
      clang::DiagnosticsEngine::Error,
      "Pragma for version in source file must be set to 1"));
  }

  // Create a static global destructor if necessary (to handle RS object
  // runtime cleanup).
  clang::FunctionDecl *FD = mRefCount.CreateStaticGlobalDtor();
  if (FD) {
    HandleTopLevelDecl(clang::DeclGroupRef(FD));
  }

  // Process any static function declarations
  for (clang::DeclContext::decl_iterator I = TUDecl->decls_begin(),
          E = TUDecl->decls_end(); I != E; I++) {
    if ((I->getKind() >= clang::Decl::firstFunction) &&
        (I->getKind() <= clang::Decl::lastFunction)) {
      clang::FunctionDecl *FD = llvm::dyn_cast<clang::FunctionDecl>(*I);
      if (FD && !FD->isGlobal()) {
        AnnotateFunction(FD);
      }
    }
  }

  return;
}

///////////////////////////////////////////////////////////////////////////////
void RSBackend::HandleTranslationUnitPost(llvm::Module *M) {
  if (!mContext->processExport()) {
    return;
  }

  // Dump export variable info
  if (mContext->hasExportVar()) {
    int slotCount = 0;
    if (mExportVarMetadata == NULL)
      mExportVarMetadata = M->getOrInsertNamedMetadata(RS_EXPORT_VAR_MN);

    llvm::SmallVector<llvm::Value*, 2> ExportVarInfo;

    // We emit slot information (#rs_object_slots) for any reference counted
    // RS type or pointer (which can also be bound).

    for (RSContext::const_export_var_iterator I = mContext->export_vars_begin(),
            E = mContext->export_vars_end();
         I != E;
         I++) {
      const RSExportVar *EV = *I;
      const RSExportType *ET = EV->getType();
      bool countsAsRSObject = false;

      // Variable name
      ExportVarInfo.push_back(
          llvm::MDString::get(mLLVMContext, EV->getName().c_str()));

      // Type name
      switch (ET->getClass()) {
        case RSExportType::ExportClassPrimitive: {
          const RSExportPrimitiveType *PT =
              static_cast<const RSExportPrimitiveType*>(ET);
          ExportVarInfo.push_back(
              llvm::MDString::get(
                mLLVMContext, llvm::utostr_32(PT->getType())));
          if (PT->isRSObjectType()) {
            countsAsRSObject = true;
          }
          break;
        }
        case RSExportType::ExportClassPointer: {
          ExportVarInfo.push_back(
              llvm::MDString::get(
                mLLVMContext, ("*" + static_cast<const RSExportPointerType*>(ET)
                  ->getPointeeType()->getName()).c_str()));
          break;
        }
        case RSExportType::ExportClassMatrix: {
          ExportVarInfo.push_back(
              llvm::MDString::get(
                mLLVMContext, llvm::utostr_32(
                  RSExportPrimitiveType::DataTypeRSMatrix2x2 +
                  static_cast<const RSExportMatrixType*>(ET)->getDim() - 2)));
          break;
        }
        case RSExportType::ExportClassVector:
        case RSExportType::ExportClassConstantArray:
        case RSExportType::ExportClassRecord: {
          ExportVarInfo.push_back(
              llvm::MDString::get(mLLVMContext,
                EV->getType()->getName().c_str()));
          break;
        }
      }

      mExportVarMetadata->addOperand(
          llvm::MDNode::get(mLLVMContext, ExportVarInfo));
      ExportVarInfo.clear();

      if (mRSObjectSlotsMetadata == NULL) {
        mRSObjectSlotsMetadata =
            M->getOrInsertNamedMetadata(RS_OBJECT_SLOTS_MN);
      }

      if (countsAsRSObject) {
        mRSObjectSlotsMetadata->addOperand(llvm::MDNode::get(mLLVMContext,
            llvm::MDString::get(mLLVMContext, llvm::utostr_32(slotCount))));
      }

      slotCount++;
    }
  }

  // Dump export function info
  if (mContext->hasExportFunc()) {
    if (mExportFuncMetadata == NULL)
      mExportFuncMetadata =
          M->getOrInsertNamedMetadata(RS_EXPORT_FUNC_MN);

    llvm::SmallVector<llvm::Value*, 1> ExportFuncInfo;

    for (RSContext::const_export_func_iterator
            I = mContext->export_funcs_begin(),
            E = mContext->export_funcs_end();
         I != E;
         I++) {
      const RSExportFunc *EF = *I;

      // Function name
      if (!EF->hasParam()) {
        ExportFuncInfo.push_back(llvm::MDString::get(mLLVMContext,
                                                     EF->getName().c_str()));
      } else {
        llvm::Function *F = M->getFunction(EF->getName());
        llvm::Function *HelperFunction;
        const std::string HelperFunctionName(".helper_" + EF->getName());

        slangAssert(F && "Function marked as exported disappeared in Bitcode");

        // Create helper function
        {
          llvm::StructType *HelperFunctionParameterTy = NULL;

          if (!F->getArgumentList().empty()) {
            std::vector<llvm::Type*> HelperFunctionParameterTys;
            for (llvm::Function::arg_iterator AI = F->arg_begin(),
                 AE = F->arg_end(); AI != AE; AI++)
              HelperFunctionParameterTys.push_back(AI->getType());

            HelperFunctionParameterTy =
                llvm::StructType::get(mLLVMContext, HelperFunctionParameterTys);
          }

          if (!EF->checkParameterPacketType(HelperFunctionParameterTy)) {
            fprintf(stderr, "Failed to export function %s: parameter type "
                            "mismatch during creation of helper function.\n",
                    EF->getName().c_str());

            const RSExportRecordType *Expected = EF->getParamPacketType();
            if (Expected) {
              fprintf(stderr, "Expected:\n");
              Expected->getLLVMType()->dump();
            }
            if (HelperFunctionParameterTy) {
              fprintf(stderr, "Got:\n");
              HelperFunctionParameterTy->dump();
            }
          }

          std::vector<llvm::Type*> Params;
          if (HelperFunctionParameterTy) {
            llvm::PointerType *HelperFunctionParameterTyP =
                llvm::PointerType::getUnqual(HelperFunctionParameterTy);
            Params.push_back(HelperFunctionParameterTyP);
          }

          llvm::FunctionType * HelperFunctionType =
              llvm::FunctionType::get(F->getReturnType(),
                                      Params,
                                      /* IsVarArgs = */false);

          HelperFunction =
              llvm::Function::Create(HelperFunctionType,
                                     llvm::GlobalValue::ExternalLinkage,
                                     HelperFunctionName,
                                     M);

          HelperFunction->addFnAttr(llvm::Attribute::NoInline);
          HelperFunction->setCallingConv(F->getCallingConv());

          // Create helper function body
          {
            llvm::Argument *HelperFunctionParameter =
                &(*HelperFunction->arg_begin());
            llvm::BasicBlock *BB =
                llvm::BasicBlock::Create(mLLVMContext, "entry", HelperFunction);
            llvm::IRBuilder<> *IB = new llvm::IRBuilder<>(BB);
            llvm::SmallVector<llvm::Value*, 6> Params;
            llvm::Value *Idx[2];

            Idx[0] =
                llvm::ConstantInt::get(llvm::Type::getInt32Ty(mLLVMContext), 0);

            // getelementptr and load instruction for all elements in
            // parameter .p
            for (size_t i = 0; i < EF->getNumParameters(); i++) {
              // getelementptr
              Idx[1] = llvm::ConstantInt::get(
                llvm::Type::getInt32Ty(mLLVMContext), i);

              llvm::Value *Ptr =
                IB->CreateInBoundsGEP(HelperFunctionParameter, Idx);

              // load
              llvm::Value *V = IB->CreateLoad(Ptr);
              Params.push_back(V);
            }

            // Call and pass the all elements as parameter to F
            llvm::CallInst *CI = IB->CreateCall(F, Params);

            CI->setCallingConv(F->getCallingConv());

            if (F->getReturnType() == llvm::Type::getVoidTy(mLLVMContext))
              IB->CreateRetVoid();
            else
              IB->CreateRet(CI);

            delete IB;
          }
        }

        ExportFuncInfo.push_back(
            llvm::MDString::get(mLLVMContext, HelperFunctionName.c_str()));
      }

      mExportFuncMetadata->addOperand(
          llvm::MDNode::get(mLLVMContext, ExportFuncInfo));
      ExportFuncInfo.clear();
    }
  }

  // Dump export function info
  if (mContext->hasExportForEach()) {
    if (mExportForEachMetadata == NULL)
      mExportForEachMetadata =
          M->getOrInsertNamedMetadata(RS_EXPORT_FOREACH_MN);

    llvm::SmallVector<llvm::Value*, 1> ExportForEachInfo;

    for (RSContext::const_export_foreach_iterator
            I = mContext->export_foreach_begin(),
            E = mContext->export_foreach_end();
         I != E;
         I++) {
      const RSExportForEach *EFE = *I;

      ExportForEachInfo.push_back(
          llvm::MDString::get(mLLVMContext,
                              llvm::utostr_32(EFE->getMetadataEncoding())));

      mExportForEachMetadata->addOperand(
          llvm::MDNode::get(mLLVMContext, ExportForEachInfo));
      ExportForEachInfo.clear();
    }
  }

  // Dump export type info
  if (mContext->hasExportType()) {
    llvm::SmallVector<llvm::Value*, 1> ExportTypeInfo;

    for (RSContext::const_export_type_iterator
            I = mContext->export_types_begin(),
            E = mContext->export_types_end();
         I != E;
         I++) {
      // First, dump type name list to export
      const RSExportType *ET = I->getValue();

      ExportTypeInfo.clear();
      // Type name
      ExportTypeInfo.push_back(
          llvm::MDString::get(mLLVMContext, ET->getName().c_str()));

      if (ET->getClass() == RSExportType::ExportClassRecord) {
        const RSExportRecordType *ERT =
            static_cast<const RSExportRecordType*>(ET);

        if (mExportTypeMetadata == NULL)
          mExportTypeMetadata =
              M->getOrInsertNamedMetadata(RS_EXPORT_TYPE_MN);

        mExportTypeMetadata->addOperand(
            llvm::MDNode::get(mLLVMContext, ExportTypeInfo));

        // Now, export struct field information to %[struct name]
        std::string StructInfoMetadataName("%");
        StructInfoMetadataName.append(ET->getName());
        llvm::NamedMDNode *StructInfoMetadata =
            M->getOrInsertNamedMetadata(StructInfoMetadataName);
        llvm::SmallVector<llvm::Value*, 3> FieldInfo;

        slangAssert(StructInfoMetadata->getNumOperands() == 0 &&
                    "Metadata with same name was created before");
        for (RSExportRecordType::const_field_iterator FI = ERT->fields_begin(),
                FE = ERT->fields_end();
             FI != FE;
             FI++) {
          const RSExportRecordType::Field *F = *FI;

          // 1. field name
          FieldInfo.push_back(llvm::MDString::get(mLLVMContext,
                                                  F->getName().c_str()));

          // 2. field type name
          FieldInfo.push_back(
              llvm::MDString::get(mLLVMContext,
                                  F->getType()->getName().c_str()));

          // 3. field kind
          switch (F->getType()->getClass()) {
            case RSExportType::ExportClassPrimitive:
            case RSExportType::ExportClassVector: {
              const RSExportPrimitiveType *EPT =
                  static_cast<const RSExportPrimitiveType*>(F->getType());
              FieldInfo.push_back(
                  llvm::MDString::get(mLLVMContext,
                                      llvm::itostr(EPT->getKind())));
              break;
            }

            default: {
              FieldInfo.push_back(
                  llvm::MDString::get(mLLVMContext,
                                      llvm::itostr(
                                        RSExportPrimitiveType::DataKindUser)));
              break;
            }
          }

          StructInfoMetadata->addOperand(
              llvm::MDNode::get(mLLVMContext, FieldInfo));
          FieldInfo.clear();
        }
      }   // ET->getClass() == RSExportType::ExportClassRecord
    }
  }

  return;
}

RSBackend::~RSBackend() {
  return;
}

}  // namespace slang
