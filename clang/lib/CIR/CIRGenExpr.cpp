//===--- CIRGenExpr.cpp - Emit LLVM Code from Expressions -----------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This contains code to emit Expr nodes as CIR code.
//
//===----------------------------------------------------------------------===//

#include "CIRGenCXXABI.h"
#include "CIRGenCall.h"
#include "CIRGenFunction.h"
#include "CIRGenModule.h"
#include "UnimplementedFeatureGuarding.h"

#include "clang/AST/GlobalDecl.h"

#include "mlir/Dialect/CIR/IR/CIRDialect.h"
#include "mlir/Dialect/StandardOps/IR/Ops.h"
#include "mlir/IR/Value.h"

using namespace cir;
using namespace clang;
using namespace mlir::cir;

static mlir::FuncOp buildFunctionDeclPointer(CIRGenModule &CGM, GlobalDecl GD) {
  const auto *FD = cast<FunctionDecl>(GD.getDecl());
  assert(!FD->hasAttr<WeakRefAttr>() && "NYI");

  auto V = CGM.GetAddrOfFunction(GD);
  assert(FD->hasPrototype() &&
         "Only prototyped functions are currently callable");

  return V;
}

static Address buildPreserveStructAccess(CIRGenFunction &CGF, LValue base,
                                         Address addr, const FieldDecl *field) {
  llvm_unreachable("NYI");
}

/// Get the address of a zero-sized field within a record. The resulting address
/// doesn't necessarily have the right type.
static Address buildAddrOfFieldStorage(CIRGenFunction &CGF, Address Base,
                                       const FieldDecl *field) {
  if (field->isZeroSize(CGF.getContext()))
    llvm_unreachable("NYI");

  auto loc = CGF.getLoc(field->getLocation());

  auto fieldType = CGF.convertType(field->getType());
  auto fieldPtr =
      mlir::cir::PointerType::get(CGF.getBuilder().getContext(), fieldType);
  auto sea = CGF.getBuilder().create<mlir::cir::StructElementAddr>(
      loc, fieldPtr, CGF.CXXThisValue->getOperand(0), field->getName());

  // TODO: We could get the alignment from the CIRGenRecordLayout, but given the
  // member name based lookup of the member here we probably shouldn't be. We'll
  // have to consider this later.
  auto addr = Address(sea->getResult(0), CharUnits::One());
  return addr;
}

LValue CIRGenFunction::buildLValueForField(LValue base,
                                           const FieldDecl *field) {
  LValueBaseInfo BaseInfo = base.getBaseInfo();

  if (field->isBitField()) {
    llvm_unreachable("NYI");
  }

  // Fields of may-alias structures are may-alais themselves.
  // FIXME: this hould get propagated down through anonymous structs and unions.
  QualType FieldType = field->getType();
  const RecordDecl *rec = field->getParent();
  AlignmentSource BaseAlignSource = BaseInfo.getAlignmentSource();
  LValueBaseInfo FieldBaseInfo(getFieldAlignmentSource(BaseAlignSource));
  if (UnimplementedFeature::tbaa() || rec->hasAttr<MayAliasAttr>() ||
      FieldType->isVectorType()) {
    // TODO(CIR): TBAAAccessInfo FieldTBAAInfo
    llvm_unreachable("NYI");
  } else if (rec->isUnion()) {
    llvm_unreachable("NYI");
  } else {
    // If no base type been assigned for the base access, then try to generate
    // one for this base lvalue.
    assert(!UnimplementedFeature::tbaa() && "NYI");
  }

  Address addr = base.getAddress();
  if (auto *ClassDef = dyn_cast<CXXRecordDecl>(rec)) {
    if (CGM.getCodeGenOpts().StrictVTablePointers &&
        ClassDef->isDynamicClass()) {
      llvm_unreachable("NYI");
    }
  }

  unsigned RecordCVR = base.getVRQualifiers();
  if (rec->isUnion()) {
    llvm_unreachable("NYI");
  } else {
    if (!IsInPreservedAIRegion &&
        (!getDebugInfo() || !rec->hasAttr<BPFPreserveAccessIndexAttr>()))
      addr = buildAddrOfFieldStorage(*this, addr, field);
    else
      // Remember the original struct field index
      addr = buildPreserveStructAccess(*this, base, addr, field);
  }

  // If this is a reference field, load the reference right now.
  if (FieldType->isReferenceType()) {
    llvm_unreachable("NYI");
  }

  // Make sure that the address is pointing to the right type. This is critical
  // for both unions and structs. A union needs a bitcast, a struct element will
  // need a bitcast if the CIR type laid out doesn't match the desired type.
  // TODO(CIR): CodeGen requires a bitcast here for unions or for structs where
  // the LLVM type doesn't match the desired type. No idea when the latter might
  // occur, though.

  if (field->hasAttr<AnnotateAttr>())
    llvm_unreachable("NYI");

  if (UnimplementedFeature::tbaa())
    // Next line should take a TBAA object
    llvm_unreachable("NYI");
  LValue LV = makeAddrLValue(addr, FieldType, FieldBaseInfo);
  LV.getQuals().addCVRQualifiers(RecordCVR);

  // __weak attribute on a field is ignored.
  if (LV.getQuals().getObjCGCAttr() == Qualifiers::Weak)
    llvm_unreachable("NYI");

  return LV;
}

LValue CIRGenFunction::buildLValueForFieldInitialization(
    LValue Base, const clang::FieldDecl *Field) {
  QualType FieldType = Field->getType();

  if (!FieldType->isReferenceType())
    return buildLValueForField(Base, Field);

  llvm_unreachable("NYI");
}

static CIRGenCallee buildDirectCallee(CIRGenModule &CGM, GlobalDecl GD) {
  const auto *FD = cast<FunctionDecl>(GD.getDecl());

  assert(!FD->getBuiltinID() && "Builtins NYI");

  auto CalleePtr = buildFunctionDeclPointer(CGM, GD);

  assert(!CGM.getLangOpts().CUDA && "NYI");

  return CIRGenCallee::forDirect(CalleePtr, GD);
}

// TODO: this can also be abstrated into common AST helpers
bool CIRGenFunction::hasBooleanRepresentation(QualType Ty) {

  if (Ty->isBooleanType())
    return true;

  if (const EnumType *ET = Ty->getAs<EnumType>())
    return ET->getDecl()->getIntegerType()->isBooleanType();

  if (const AtomicType *AT = Ty->getAs<AtomicType>())
    return hasBooleanRepresentation(AT->getValueType());

  return false;
}

CIRGenCallee CIRGenFunction::buildCallee(const clang::Expr *E) {
  E = E->IgnoreParens();

  if (const auto *ICE = dyn_cast<ImplicitCastExpr>(E)) {
    assert(ICE && "Only ICE supported so far!");
    assert(ICE->getCastKind() == CK_FunctionToPointerDecay &&
           "No other casts supported yet");

    return buildCallee(ICE->getSubExpr());
  } else if (const auto *DRE = dyn_cast<DeclRefExpr>(E)) {
    const auto *FD = dyn_cast<FunctionDecl>(DRE->getDecl());
    assert(FD &&
           "DeclRef referring to FunctionDecl only thing supported so far");
    return buildDirectCallee(CGM, FD);
  }

  assert(!dyn_cast<MemberExpr>(E) && "NYI");
  assert(!dyn_cast<SubstNonTypeTemplateParmExpr>(E) && "NYI");
  assert(!dyn_cast<CXXPseudoDestructorExpr>(E) && "NYI");

  assert(false && "Nothing else supported yet!");
}

mlir::Value CIRGenFunction::buildToMemory(mlir::Value Value, QualType Ty) {
  // Bool has a different representation in memory than in registers.
  return Value;
}

void CIRGenFunction::buildStoreOfScalar(mlir::Value value, LValue lvalue,
                                        const Decl *InitDecl) {
  // TODO: constant matrix type, volatile, non temporal, TBAA
  buildStoreOfScalar(value, lvalue.getAddress(), false, lvalue.getType(),
                     lvalue.getBaseInfo(), InitDecl, false);
}

void CIRGenFunction::buildStoreOfScalar(mlir::Value Value, Address Addr,
                                        bool Volatile, QualType Ty,
                                        LValueBaseInfo BaseInfo,
                                        const Decl *InitDecl,
                                        bool isNontemporal) {
  if (!CGM.getCodeGenOpts().PreserveVec3Type) {
    if (Ty->isVectorType()) {
      llvm_unreachable("NYI");
    }
  }

  Value = buildToMemory(Value, Ty);

  if (Ty->isAtomicType()) {
    llvm_unreachable("NYI");
  }

  // Update the alloca with more info on initialization.
  assert(Addr.getPointer() && "expected pointer to exist");
  auto SrcAlloca =
      dyn_cast_or_null<mlir::cir::AllocaOp>(Addr.getPointer().getDefiningOp());
  if (InitDecl && SrcAlloca) {
    InitStyle IS;
    const VarDecl *VD = dyn_cast_or_null<VarDecl>(InitDecl);
    assert(VD && "VarDecl expected");
    if (VD->hasInit()) {
      switch (VD->getInitStyle()) {
      case VarDecl::CInit:
        IS = InitStyle::cinit;
        break;
      case VarDecl::CallInit:
        IS = InitStyle::callinit;
        break;
      case VarDecl::ListInit:
        IS = InitStyle::listinit;
        break;
      }
      SrcAlloca.initAttr(InitStyleAttr::get(builder.getContext(), IS));
    }
  }

  assert(currSrcLoc && "must pass in source location");
  builder.create<mlir::cir::StoreOp>(*currSrcLoc, Value, Addr.getPointer());

  if (isNontemporal) {
    llvm_unreachable("NYI");
  }

  if (UnimplementedFeature::tbaa())
    llvm_unreachable("NYI");
}

void CIRGenFunction::buldStoreThroughLValue(RValue Src, LValue Dst,
                                            const Decl *InitDecl) {
  assert(Dst.isSimple() && "only implemented simple");
  // TODO: ObjC lifetime.
  assert(Src.isScalar() && "Can't emit an agg store with this method");
  buildStoreOfScalar(Src.getScalarVal(), Dst, InitDecl);
}

static LValue buildGlobalVarDeclLValue(CIRGenFunction &CGF, const Expr *E,
                                       const VarDecl *VD) {
  QualType T = E->getType();

  // If it's thread_local, emit a call to its wrapper function instead.
  if (VD->getTLSKind() == VarDecl::TLS_Dynamic &&
      CGF.CGM.getCXXABI().usesThreadWrapperFunction(VD))
    assert(0 && "not implemented");

  // Check if the variable is marked as declare target with link clause in
  // device codegen.
  if (CGF.getLangOpts().OpenMPIsDevice) {
    assert(0 && "not implemented");
  }

  auto V = CGF.CGM.getAddrOfGlobalVar(VD);
  auto RealVarTy = CGF.getTypes().convertTypeForMem(VD->getType());
  // TODO(cir): do we need this for CIR?
  // V = EmitBitCastOfLValueToProperType(CGF, V, RealVarTy);
  CharUnits Alignment = CGF.getContext().getDeclAlign(VD);
  Address Addr(V, RealVarTy, Alignment);
  // Emit reference to the private copy of the variable if it is an OpenMP
  // threadprivate variable.
  if (CGF.getLangOpts().OpenMP && !CGF.getLangOpts().OpenMPSimd &&
      VD->hasAttr<clang::OMPThreadPrivateDeclAttr>()) {
    assert(0 && "NYI");
  }
  LValue LV;
  if (VD->getType()->isReferenceType())
    assert(0 && "NYI");
  else
    LV = CGF.makeAddrLValue(Addr, T, AlignmentSource::Decl);
  assert(!UnimplementedFeature::setObjCGCLValueClass() && "NYI");
  return LV;
}

LValue CIRGenFunction::buildDeclRefLValue(const DeclRefExpr *E) {
  const NamedDecl *ND = E->getDecl();
  QualType T = E->getType();

  assert(E->isNonOdrUse() != NOUR_Unevaluated &&
         "should not emit an unevaluated operand");

  if (const auto *VD = dyn_cast<VarDecl>(ND)) {
    // Global Named registers access via intrinsics only
    assert(VD->getStorageClass() != SC_Register && "not implemented");
    assert(E->isNonOdrUse() != NOUR_Constant && "not implemented");
    assert(!E->refersToEnclosingVariableOrCapture() && "not implemented");
  }

  // FIXME(CIR): We should be able to assert this for FunctionDecls as well!
  // FIXME(CIR): We should be able to assert this for all DeclRefExprs, not just
  // those with a valid source location.
  assert((ND->isUsed(false) || !isa<VarDecl>(ND) || E->isNonOdrUse() ||
          !E->getLocation().isValid()) &&
         "Should not use decl without marking it used!");

  if (ND->hasAttr<WeakRefAttr>()) {
    llvm_unreachable("NYI");
  }

  if (const auto *VD = dyn_cast<VarDecl>(ND)) {
    // Check if this is a global variable
    if (VD->hasLinkage() || VD->isStaticDataMember())
      return buildGlobalVarDeclLValue(*this, E, VD);

    Address addr = Address::invalid();

    // The variable should generally be present in the local decl map.
    auto iter = LocalDeclMap.find(VD);
    if (iter != LocalDeclMap.end()) {
      addr = iter->second;
    }
    // Otherwise, it might be static local we haven't emitted yet for some
    // reason; most likely, because it's in an outer function.
    else if (VD->isStaticLocal()) {
      llvm_unreachable("NYI");
    } else {
      llvm_unreachable("DeclRefExpr for decl not entered in LocalDeclMap?");
    }

    // Check for OpenMP threadprivate variables.
    if (getLangOpts().OpenMP && !getLangOpts().OpenMPSimd &&
        VD->hasAttr<OMPThreadPrivateDeclAttr>()) {
      llvm_unreachable("NYI");
    }

    // Drill into block byref variables.
    bool isBlockByref = VD->isEscapingByref();
    if (isBlockByref) {
      llvm_unreachable("NYI");
    }

    // Drill into reference types.
    assert(!VD->getType()->isReferenceType() && "NYI");
    LValue LV = makeAddrLValue(addr, T, AlignmentSource::Decl);

    assert(symbolTable.count(VD) && "should be already mapped");

    bool isLocalStorage = VD->hasLocalStorage();

    bool NonGCable =
        isLocalStorage && !VD->getType()->isReferenceType() && !isBlockByref;

    if (NonGCable) {
      // TODO: nongcable
    }

    bool isImpreciseLifetime =
        (isLocalStorage && !VD->hasAttr<ObjCPreciseLifetimeAttr>());
    if (isImpreciseLifetime)
      ; // TODO: LV.setARCPreciseLifetime
    // TODO: setObjCGCLValueClass(getContext(), E, LV);

    mlir::Value V = symbolTable.lookup(VD);
    assert(V && "Name lookup must succeed");

    return LV;
  }

  llvm_unreachable("Unhandled DeclRefExpr?");
}

LValue CIRGenFunction::buildBinaryOperatorLValue(const BinaryOperator *E) {
  // Comma expressions just emit their LHS then their RHS as an l-value.
  if (E->getOpcode() == BO_Comma) {
    assert(0 && "not implemented");
  }

  if (E->getOpcode() == BO_PtrMemD || E->getOpcode() == BO_PtrMemI)
    assert(0 && "not implemented");

  assert(E->getOpcode() == BO_Assign && "unexpected binary l-value");

  // Note that in all of these cases, __block variables need the RHS
  // evaluated first just in case the variable gets moved by the RHS.

  switch (CIRGenFunction::getEvaluationKind(E->getType())) {
  case TEK_Scalar: {
    assert(E->getLHS()->getType().getObjCLifetime() ==
               clang::Qualifiers::ObjCLifetime::OCL_None &&
           "not implemented");

    RValue RV = buildAnyExpr(E->getRHS());
    LValue LV = buildLValue(E->getLHS());

    SourceLocRAIIObject Loc{*this, getLoc(E->getSourceRange())};
    buldStoreThroughLValue(RV, LV, nullptr /*InitDecl*/);
    assert(!getContext().getLangOpts().OpenMP &&
           "last priv cond not implemented");
    return LV;
  }

  case TEK_Complex:
    assert(0 && "not implemented");
  case TEK_Aggregate:
    assert(0 && "not implemented");
  }
  llvm_unreachable("bad evaluation kind");
}

/// Given an expression of pointer type, try to
/// derive a more accurate bound on the alignment of the pointer.
Address CIRGenFunction::buildPointerWithAlignment(const Expr *E,
                                                  LValueBaseInfo *BaseInfo) {
  // We allow this with ObjC object pointers because of fragile ABIs.
  assert(E->getType()->isPointerType() ||
         E->getType()->isObjCObjectPointerType());
  E = E->IgnoreParens();

  // Casts:
  if (const CastExpr *CE = dyn_cast<CastExpr>(E)) {
    if (const auto *ECE = dyn_cast<ExplicitCastExpr>(CE))
      assert(0 && "not implemented");

    switch (CE->getCastKind()) {
    default:
      assert(0 && "not implemented");
    // Nothing to do here...
    case CK_LValueToRValue:
      break;
    }
  }

  // Unary &.
  if (const UnaryOperator *UO = dyn_cast<UnaryOperator>(E)) {
    assert(0 && "not implemented");
    // if (UO->getOpcode() == UO_AddrOf) {
    //   LValue LV = buildLValue(UO->getSubExpr());
    //   if (BaseInfo)
    //     *BaseInfo = LV.getBaseInfo();
    //   // TODO: TBBA info
    //   return LV.getAddress();
    // }
  }

  // TODO: conditional operators, comma.
  // Otherwise, use the alignment of the type.
  CharUnits Align = CGM.getNaturalPointeeTypeAlignment(E->getType(), BaseInfo);
  return Address(buildScalarExpr(E), Align);
}

/// Perform the usual unary conversions on the specified
/// expression and compare the result against zero, returning an Int1Ty value.
mlir::Value CIRGenFunction::evaluateExprAsBool(const Expr *E) {
  // TODO: PGO
  if (const MemberPointerType *MPT = E->getType()->getAs<MemberPointerType>()) {
    assert(0 && "not implemented");
  }

  QualType BoolTy = getContext().BoolTy;
  SourceLocation Loc = E->getExprLoc();
  // TODO: CGFPOptionsRAII for FP stuff.
  assert(!E->getType()->isAnyComplexType() &&
         "complex to scalar not implemented");
  return buildScalarConversion(buildScalarExpr(E), E->getType(), BoolTy, Loc);
}

LValue CIRGenFunction::buildUnaryOpLValue(const UnaryOperator *E) {
  // __extension__ doesn't affect lvalue-ness.
  assert(E->getOpcode() != UO_Extension && "not implemented");

  switch (E->getOpcode()) {
  default:
    llvm_unreachable("Unknown unary operator lvalue!");
  case UO_Deref: {
    QualType T = E->getSubExpr()->getType()->getPointeeType();
    assert(!T.isNull() && "CodeGenFunction::EmitUnaryOpLValue: Illegal type");

    LValueBaseInfo BaseInfo;
    // TODO: add TBAAInfo
    Address Addr = buildPointerWithAlignment(E->getSubExpr(), &BaseInfo);

    // Tag 'load' with deref attribute.
    if (auto loadOp =
            dyn_cast<::mlir::cir::LoadOp>(Addr.getPointer().getDefiningOp())) {
      loadOp.isDerefAttr(mlir::UnitAttr::get(builder.getContext()));
    }

    LValue LV = LValue::makeAddr(Addr, T, BaseInfo);
    // TODO: set addr space
    // TODO: ObjC/GC/__weak write barrier stuff.
    return LV;
  }
  case UO_Real:
  case UO_Imag: {
    assert(0 && "not implemented");
  }
  case UO_PreInc:
  case UO_PreDec: {
    assert(0 && "not implemented");
  }
  }
}

/// Emit code to compute the specified expression which
/// can have any type.  The result is returned as an RValue struct.
RValue CIRGenFunction::buildAnyExpr(const Expr *E, AggValueSlot aggSlot,
                                    bool ignoreResult) {
  switch (CIRGenFunction::getEvaluationKind(E->getType())) {
  case TEK_Scalar:
    return RValue::get(buildScalarExpr(E));
  case TEK_Complex:
    assert(0 && "not implemented");
  case TEK_Aggregate:
    assert(0 && "not implemented");
  }
  llvm_unreachable("bad evaluation kind");
}

RValue CIRGenFunction::buildCallExpr(const clang::CallExpr *E,
                                     ReturnValueSlot ReturnValue) {
  assert(!E->getCallee()->getType()->isBlockPointerType() && "ObjC Blocks NYI");

  if (const auto *CE = dyn_cast<CXXMemberCallExpr>(E))
    return buildCXXMemberCallExpr(CE, ReturnValue);

  assert(!dyn_cast<CUDAKernelCallExpr>(E) && "CUDA NYI");
  assert(!dyn_cast<CXXOperatorCallExpr>(E) && "NYI");

  CIRGenCallee callee = buildCallee(E->getCallee());

  assert(!callee.isBuiltin() && "builtins NYI");
  assert(!callee.isPsuedoDestructor() && "NYI");

  return buildCall(E->getCallee()->getType(), callee, E, ReturnValue);
}

RValue CIRGenFunction::buildCall(clang::QualType CalleeType,
                                 const CIRGenCallee &OrigCallee,
                                 const clang::CallExpr *E,
                                 ReturnValueSlot ReturnValue,
                                 mlir::Value Chain) {
  // Get the actual function type. The callee type will always be a pointer to
  // function type or a block pointer type.
  assert(CalleeType->isFunctionPointerType() &&
         "Call must have function pointer type!");

  auto *TargetDecl = OrigCallee.getAbstractInfo().getCalleeDecl().getDecl();
  (void)TargetDecl;

  CalleeType = getContext().getCanonicalType(CalleeType);

  auto PointeeType = cast<clang::PointerType>(CalleeType)->getPointeeType();

  CIRGenCallee Callee = OrigCallee;

  if (getLangOpts().CPlusPlus)
    assert(!SanOpts.has(SanitizerKind::Function) && "Sanitizers NYI");

  const auto *FnType = cast<FunctionType>(PointeeType);

  assert(!SanOpts.has(SanitizerKind::CFIICall) && "Sanitizers NYI");

  CallArgList Args;

  assert(!Chain && "FIX THIS");

  // C++17 requires that we evaluate arguments to a call using assignment syntax
  // right-to-left, and that we evaluate arguments to certain other operators
  // left-to-right. Note that we allow this to override the order dictated by
  // the calling convention on the MS ABI, which means that parameter
  // destruction order is not necessarily reverse construction order.
  // FIXME: Revisit this based on C++ committee response to unimplementability.
  EvaluationOrder Order = EvaluationOrder::Default;
  assert(!dyn_cast<CXXOperatorCallExpr>(E) && "Operators NYI");

  buildCallArgs(Args, dyn_cast<FunctionProtoType>(FnType), E->arguments(),
                E->getDirectCallee(), /*ParamsToSkip*/ 0, Order);

  const CIRGenFunctionInfo &FnInfo = CGM.getTypes().arrangeFreeFunctionCall(
      Args, FnType, /*ChainCall=*/Chain.getAsOpaquePointer());

  // C99 6.5.2.2p6:
  //   If the expression that denotes the called function has a type that does
  //   not include a prototype, [the default argument promotions are performed].
  //   If the number of arguments does not equal the number of parameters, the
  //   behavior is undefined. If the function is defined with at type that
  //   includes a prototype, and either the prototype ends with an ellipsis (,
  //   ...) or the types of the arguments after promotion are not compatible
  //   with the types of the parameters, the behavior is undefined. If the
  //   function is defined with a type that does not include a prototype, and
  //   the types of the arguments after promotion are not compatible with those
  //   of the parameters after promotion, the behavior is undefined [except in
  //   some trivial cases].
  // That is, in the general case, we should assume that a call through an
  // unprototyped function type works like a *non-variadic* call. The way we
  // make this work is to cast to the exxact type fo the promoted arguments.
  //
  // Chain calls use the same code path to add the inviisble chain parameter to
  // the function type.
  assert(!isa<FunctionNoProtoType>(FnType) && "NYI");
  // if (isa<FunctionNoProtoType>(FnType) || Chain) {
  //   mlir::FunctionType CalleeTy = getTypes().GetFunctionType(FnInfo);
  // int AS = Callee.getFunctionPointer()->getType()->getPointerAddressSpace();
  // CalleeTy = CalleeTy->getPointerTo(AS);

  // llvm::Value *CalleePtr = Callee.getFunctionPointer();
  // CalleePtr = Builder.CreateBitCast(CalleePtr, CalleeTy, "callee.knr.cast");
  // Callee.setFunctionPointer(CalleePtr);
  // }

  assert(!CGM.getLangOpts().HIP && "HIP NYI");

  assert(!MustTailCall && "Must tail NYI");
  mlir::CallOp callOP = nullptr;
  RValue Call = buildCall(FnInfo, Callee, ReturnValue, Args, &callOP,
                          E == MustTailCall, E->getExprLoc());

  assert(!getDebugInfo() && "Debug Info NYI");

  return Call;
}

/// Emit code to compute the specified expression, ignoring the result.
void CIRGenFunction::buildIgnoredExpr(const Expr *E) {
  if (E->isPRValue())
    return (void)buildAnyExpr(E);

  // Just emit it as an l-value and drop the result.
  buildLValue(E);
}

static mlir::Value maybeBuildArrayDecay(mlir::OpBuilder &builder,
                                        mlir::Location loc,
                                        mlir::Value arrayPtr,
                                        mlir::Type eltTy) {
  auto arrayPtrTy = arrayPtr.getType().dyn_cast<::mlir::cir::PointerType>();
  assert(arrayPtrTy && "expected pointer type");
  auto arrayTy = arrayPtrTy.getPointee().dyn_cast<::mlir::cir::ArrayType>();

  if (arrayTy) {
    mlir::cir::PointerType flatPtrTy =
        mlir::cir::PointerType::get(builder.getContext(), arrayTy.getEltType());
    return builder.create<mlir::cir::CastOp>(
        loc, flatPtrTy, mlir::cir::CastKind::array_to_ptrdecay, arrayPtr);
  }

  assert(arrayPtrTy.getPointee() == eltTy &&
         "flat pointee type must match original array element type");
  return arrayPtr;
}

Address CIRGenFunction::buildArrayToPointerDecay(const Expr *E,
                                                 LValueBaseInfo *BaseInfo) {
  assert(E->getType()->isArrayType() &&
         "Array to pointer decay must have array source type!");

  // Expressions of array type can't be bitfields or vector elements.
  LValue LV = buildLValue(E);
  Address Addr = LV.getAddress();

  // If the array type was an incomplete type, we need to make sure
  // the decay ends up being the right type.
  auto lvalueAddrTy =
      Addr.getPointer().getType().dyn_cast<mlir::cir::PointerType>();
  assert(lvalueAddrTy && "expected pointer");

  auto pointeeTy = lvalueAddrTy.getPointee().dyn_cast<mlir::cir::ArrayType>();
  assert(pointeeTy && "expected array");

  mlir::Type arrayTy = convertType(E->getType());
  assert(arrayTy.isa<mlir::cir::ArrayType>() && "expected array");
  assert(pointeeTy == arrayTy);

  // TODO(cir): in LLVM codegen VLA pointers are always decayed, so we don't
  // need to do anything here. Revisit this for VAT when its supported in CIR.
  assert(!E->getType()->isVariableArrayType() && "what now?");

  // The result of this decay conversion points to an array element within the
  // base lvalue. However, since TBAA currently does not support representing
  // accesses to elements of member arrays, we conservatively represent accesses
  // to the pointee object as if it had no any base lvalue specified.
  // TODO: Support TBAA for member arrays.
  QualType EltType = E->getType()->castAsArrayTypeUnsafe()->getElementType();
  if (BaseInfo)
    *BaseInfo = LV.getBaseInfo();
  assert(!UnimplementedFeature::tbaa() && "NYI");

  mlir::Value ptr = maybeBuildArrayDecay(
      CGM.getBuilder(), CGM.getLoc(E->getSourceRange()), Addr.getPointer(),
      getTypes().convertTypeForMem(EltType));
  return Address(ptr, Addr.getAlignment());
}

/// If the specified expr is a simple decay from an array to pointer,
/// return the array subexpression.
/// FIXME: this could be abstracted into a commeon AST helper.
static const Expr *isSimpleArrayDecayOperand(const Expr *E) {
  // If this isn't just an array->pointer decay, bail out.
  const auto *CE = dyn_cast<CastExpr>(E);
  if (!CE || CE->getCastKind() != CK_ArrayToPointerDecay)
    return nullptr;

  // If this is a decay from variable width array, bail out.
  const Expr *SubExpr = CE->getSubExpr();
  if (SubExpr->getType()->isVariableArrayType())
    return nullptr;

  return SubExpr;
}

/// Given an array base, check whether its member access belongs to a record
/// with preserve_access_index attribute or not.
/// TODO(cir): don't need to be specific to LLVM's codegen, refactor into common
/// AST helpers.
static bool isPreserveAIArrayBase(CIRGenFunction &CGF, const Expr *ArrayBase) {
  if (!ArrayBase || !CGF.getDebugInfo())
    return false;

  // Only support base as either a MemberExpr or DeclRefExpr.
  // DeclRefExpr to cover cases like:
  //    struct s { int a; int b[10]; };
  //    struct s *p;
  //    p[1].a
  // p[1] will generate a DeclRefExpr and p[1].a is a MemberExpr.
  // p->b[5] is a MemberExpr example.
  const Expr *E = ArrayBase->IgnoreImpCasts();
  if (const auto *ME = dyn_cast<MemberExpr>(E))
    return ME->getMemberDecl()->hasAttr<BPFPreserveAccessIndexAttr>();

  if (const auto *DRE = dyn_cast<DeclRefExpr>(E)) {
    const auto *VarDef = dyn_cast<VarDecl>(DRE->getDecl());
    if (!VarDef)
      return false;

    const auto *PtrT = VarDef->getType()->getAs<clang::PointerType>();
    if (!PtrT)
      return false;

    const auto *PointeeT =
        PtrT->getPointeeType()->getUnqualifiedDesugaredType();
    if (const auto *RecT = dyn_cast<RecordType>(PointeeT))
      return RecT->getDecl()->hasAttr<BPFPreserveAccessIndexAttr>();
    return false;
  }

  return false;
}

static mlir::IntegerAttr getConstantIndexOrNull(mlir::Value idx) {
  // TODO(cir): should we consider using MLIRs IndexType instead of IntegerAttr?
  if (auto constantOp = dyn_cast<mlir::cir::ConstantOp>(idx.getDefiningOp()))
    return constantOp.value().dyn_cast<mlir::IntegerAttr>();
  return {};
}

static CharUnits getArrayElementAlign(CharUnits arrayAlign, mlir::Value idx,
                                      CharUnits eltSize) {
  // If we have a constant index, we can use the exact offset of the
  // element we're accessing.
  auto constantIdx = getConstantIndexOrNull(idx);
  if (constantIdx) {
    CharUnits offset = constantIdx.getValue().getZExtValue() * eltSize;
    return arrayAlign.alignmentAtOffset(offset);
    // Otherwise, use the worst-case alignment for any element.
  } else {
    return arrayAlign.alignmentOfArrayElement(eltSize);
  }
}

static mlir::Value buildArrayAccessOp(mlir::OpBuilder &builder,
                                      mlir::Location arrayLocBegin,
                                      mlir::Location arrayLocEnd,
                                      mlir::Value arrayPtr, mlir::Type eltTy,
                                      mlir::Value idx) {
  mlir::Value basePtr =
      maybeBuildArrayDecay(builder, arrayLocBegin, arrayPtr, eltTy);
  mlir::Type flatPtrTy = basePtr.getType();

  return builder.create<mlir::cir::PtrStrideOp>(arrayLocEnd, flatPtrTy, basePtr,
                                                idx);
}

static mlir::Value buildArraySubscriptPtr(
    CIRGenFunction &CGF, mlir::Location beginLoc, mlir::Location endLoc,
    mlir::Value ptr, mlir::Type eltTy, ArrayRef<mlir::Value> indices,
    bool inbounds, bool signedIndices, const llvm::Twine &name = "arrayidx") {
  assert(indices.size() == 1 && "cannot handle multiple indices yet");
  auto idx = indices.back();
  auto &CGM = CGF.getCIRGenModule();
  // TODO(cir): LLVM codegen emits in bound gep check here, is there anything
  // that would enhance tracking this later in CIR?
  if (inbounds)
    assert(!UnimplementedFeature::emitCheckedInBoundsGEP() && "NYI");
  return buildArrayAccessOp(CGM.getBuilder(), beginLoc, endLoc, ptr, eltTy,
                            idx);
}

static Address buildArraySubscriptPtr(
    CIRGenFunction &CGF, mlir::Location beginLoc, mlir::Location endLoc,
    Address addr, ArrayRef<mlir::Value> indices, QualType eltType,
    bool inbounds, bool signedIndices, mlir::Location loc,
    QualType *arrayType = nullptr, const Expr *Base = nullptr,
    const llvm::Twine &name = "arrayidx") {
  // Determine the element size of the statically-sized base.  This is
  // the thing that the indices are expressed in terms of.
  if (auto vla = CGF.getContext().getAsVariableArrayType(eltType)) {
    assert(0 && "not implemented");
  }

  // We can use that to compute the best alignment of the element.
  CharUnits eltSize = CGF.getContext().getTypeSizeInChars(eltType);
  CharUnits eltAlign =
      getArrayElementAlign(addr.getAlignment(), indices.back(), eltSize);

  mlir::Value eltPtr;
  auto LastIndex = getConstantIndexOrNull(indices.back());
  if (!LastIndex ||
      (!CGF.IsInPreservedAIRegion && !isPreserveAIArrayBase(CGF, Base))) {
    eltPtr = buildArraySubscriptPtr(CGF, beginLoc, endLoc, addr.getPointer(),
                                    addr.getElementType(), indices, inbounds,
                                    signedIndices, name);
  } else {
    // assert(!UnimplementedFeature::generateDebugInfo() && "NYI");
    // assert(indices.size() == 1 && "cannot handle multiple indices yet");
    // auto idx = indices.back();
    // auto &CGM = CGF.getCIRGenModule();
    // eltPtr = buildArrayAccessOp(CGM.getBuilder(), beginLoc, endLoc,
    //                             addr.getPointer(), addr.getElementType(),
    //                             idx);
    assert(0 && "NYI");
  }

  return Address(eltPtr, CGF.getTypes().convertTypeForMem(eltType), eltAlign);
}

LValue CIRGenFunction::buildArraySubscriptExpr(const ArraySubscriptExpr *E,
                                               bool Accessed) {
  // The index must always be an integer, which is not an aggregate.  Emit it
  // in lexical order (this complexity is, sadly, required by C++17).
  // llvm::Value *IdxPre =
  //     (E->getLHS() == E->getIdx()) ? EmitScalarExpr(E->getIdx()) : nullptr;
  assert(E->getLHS() != E->getIdx() && "not implemented");
  bool SignedIndices = false;
  auto EmitIdxAfterBase = [&](bool Promote) -> mlir::Value {
    mlir::Value Idx;
    if (E->getLHS() != E->getIdx()) {
      assert(E->getRHS() == E->getIdx() && "index was neither LHS nor RHS");
      Idx = buildScalarExpr(E->getIdx());
    }

    QualType IdxTy = E->getIdx()->getType();
    bool IdxSigned = IdxTy->isSignedIntegerOrEnumerationType();
    SignedIndices |= IdxSigned;

    assert(!SanOpts.has(SanitizerKind::ArrayBounds) && "not implemented");

    // TODO: Extend or truncate the index type to 32 or 64-bits.
    // if (Promote && !Idx.getType().isa<::mlir::cir::PointerType>()) {
    //   Idx = Builder.CreateIntCast(Idx, IntPtrTy, IdxSigned, "idxprom");
    // }

    return Idx;
  };

  // If the base is a vector type, then we are forming a vector element
  // with this subscript.
  if (E->getBase()->getType()->isVectorType() &&
      !isa<ExtVectorElementExpr>(E->getBase())) {
    assert(0 && "not implemented");
  }

  // All the other cases basically behave like simple offsetting.

  // Handle the extvector case we ignored above.
  if (isa<ExtVectorElementExpr>(E->getBase())) {
    assert(0 && "not implemented");
  }

  // TODO: TBAAAccessInfo
  LValueBaseInfo EltBaseInfo;
  Address Addr = Address::invalid();
  if (const VariableArrayType *vla =
          getContext().getAsVariableArrayType(E->getType())) {
    assert(0 && "not implemented");
  } else if (const ObjCObjectType *OIT =
                 E->getType()->getAs<ObjCObjectType>()) {
    assert(0 && "not implemented");
  } else if (const Expr *Array = isSimpleArrayDecayOperand(E->getBase())) {
    // If this is A[i] where A is an array, the frontend will have decayed
    // the base to be a ArrayToPointerDecay implicit cast.  While correct, it is
    // inefficient at -O0 to emit a "gep A, 0, 0" when codegen'ing it, then
    // a "gep x, i" here.  Emit one "gep A, 0, i".
    assert(Array->getType()->isArrayType() &&
           "Array to pointer decay must have array source type!");
    LValue ArrayLV;
    // For simple multidimensional array indexing, set the 'accessed' flag
    // for better bounds-checking of the base expression.
    // if (const auto *ASE = dyn_cast<ArraySubscriptExpr>(Array))
    //   ArrayLV = buildArraySubscriptExpr(ASE, /*Accessed*/ true);
    assert(!llvm::isa<ArraySubscriptExpr>(Array) &&
           "multidimensional array indexing not implemented");

    ArrayLV = buildLValue(Array);
    auto Idx = EmitIdxAfterBase(/*Promote=*/true);
    QualType arrayType = Array->getType();

    // Propagate the alignment from the array itself to the result.
    Addr = buildArraySubscriptPtr(
        *this, CGM.getLoc(Array->getBeginLoc()), CGM.getLoc(Array->getEndLoc()),
        ArrayLV.getAddress(), {Idx}, E->getType(),
        !getLangOpts().isSignedOverflowDefined(), SignedIndices,
        CGM.getLoc(E->getExprLoc()), &arrayType, E->getBase());
    EltBaseInfo = ArrayLV.getBaseInfo();
    // TODO: EltTBAAInfo
  } else {
    // The base must be a pointer; emit it with an estimate of its alignment.
    // TODO(cir): EltTBAAInfo
    Addr = buildPointerWithAlignment(E->getBase(), &EltBaseInfo);
    auto Idx = EmitIdxAfterBase(/*Promote*/ true);
    QualType ptrType = E->getBase()->getType();
    Addr = buildArraySubscriptPtr(
        *this, CGM.getLoc(E->getBeginLoc()), CGM.getLoc(E->getEndLoc()), Addr,
        Idx, E->getType(), !getLangOpts().isSignedOverflowDefined(),
        SignedIndices, CGM.getLoc(E->getExprLoc()), &ptrType, E->getBase());
  }

  LValue LV = LValue::makeAddr(Addr, E->getType(), EltBaseInfo);
  if (getLangOpts().ObjC && getLangOpts().getGC() != LangOptions::NonGC) {
    assert(0 && "not implemented");
  }
  return LV;
}

LValue CIRGenFunction::buildStringLiteralLValue(const StringLiteral *E) {
  auto sym = CGM.getAddrOfConstantStringFromLiteral(E);

  auto cstGlobal = mlir::SymbolTable::lookupSymbolIn(CGM.getModule(), sym);
  assert(cstGlobal && "Expected global");

  auto g = dyn_cast<mlir::cir::GlobalOp>(cstGlobal);
  assert(g && "unaware of other symbol providers");

  auto ptrTy =
      mlir::cir::PointerType::get(CGM.getBuilder().getContext(), g.sym_type());
  assert(g.alignment() && "expected alignment for string literal");
  auto align = *g.alignment();
  auto addr = builder.create<mlir::cir::GetGlobalOp>(
      getLoc(E->getSourceRange()), ptrTy, g.sym_name());
  return makeAddrLValue(
      Address(addr, g.sym_type(), CharUnits::fromQuantity(align)), E->getType(),
      AlignmentSource::Decl);
}

/// Emit code to compute a designator that specifies the location
/// of the expression.
/// FIXME: document this function better.
LValue CIRGenFunction::buildLValue(const Expr *E) {
  // FIXME: ApplyDebugLocation DL(*this, E);
  switch (E->getStmtClass()) {
  default: {
    emitError(getLoc(E->getExprLoc()), "l-value not implemented for '")
        << E->getStmtClassName() << "'";
    assert(0 && "not implemented");
  }
  case Expr::ArraySubscriptExprClass:
    return buildArraySubscriptExpr(cast<ArraySubscriptExpr>(E));
  case Expr::BinaryOperatorClass:
    return buildBinaryOperatorLValue(cast<BinaryOperator>(E));
  case Expr::DeclRefExprClass:
    return buildDeclRefLValue(cast<DeclRefExpr>(E));
  case Expr::UnaryOperatorClass:
    return buildUnaryOpLValue(cast<UnaryOperator>(E));
  case Expr::StringLiteralClass:
    return buildStringLiteralLValue(cast<StringLiteral>(E));
  case Expr::ObjCPropertyRefExprClass:
    llvm_unreachable("cannot emit a property reference directly");
  }

  return LValue::makeAddr(Address::invalid(), E->getType());
}

/// Given the address of a temporary variable, produce an r-value of its type.
RValue CIRGenFunction::convertTempToRValue(Address addr, clang::QualType type,
                                           clang::SourceLocation loc) {
  LValue lvalue = makeAddrLValue(addr, type, AlignmentSource::Decl);
  switch (getEvaluationKind(type)) {
  case TEK_Complex:
    llvm_unreachable("NYI");
  case TEK_Aggregate:
    llvm_unreachable("NYI");
  case TEK_Scalar:
    return RValue::get(buildLoadOfScalar(lvalue, loc));
  }
  llvm_unreachable("NYI");
}

/// An LValue is a candidate for having its loads and stores be made atomic if
/// we are operating under /volatile:ms *and* the LValue itself is volatile and
/// performing such an operation can be performed without a libcall.
bool CIRGenFunction::LValueIsSuitableForInlineAtomic(LValue LV) {
  if (!CGM.getCodeGenOpts().MSVolatile)
    return false;

  llvm_unreachable("NYI");
}

/// Emit an if on a boolean condition to the specified blocks.
/// FIXME: Based on the condition, this might try to simplify the codegen of
/// the conditional based on the branch. TrueCount should be the number of
/// times we expect the condition to evaluate to true based on PGO data. We
/// might decide to leave this as a separate pass (see EmitBranchOnBoolExpr
/// for extra ideas).
mlir::LogicalResult CIRGenFunction::buildIfOnBoolExpr(const Expr *cond,
                                                      mlir::Location loc,
                                                      const Stmt *thenS,
                                                      const Stmt *elseS) {
  // TODO(CIR): scoped ApplyDebugLocation DL(*this, Cond);
  // TODO(CIR): __builtin_unpredictable and profile counts?
  cond = cond->IgnoreParens();
  mlir::Value condV = evaluateExprAsBool(cond);
  mlir::LogicalResult resThen = mlir::success(), resElse = mlir::success();

  builder.create<mlir::cir::IfOp>(
      loc, condV, elseS,
      /*thenBuilder=*/
      [&](mlir::OpBuilder &b, mlir::Location loc) {
        // FIXME: abstract all this massive location handling elsewhere.
        SmallVector<mlir::Location, 2> locs;
        if (loc.isa<mlir::FileLineColLoc>()) {
          locs.push_back(loc);
          locs.push_back(loc);
        } else if (loc.isa<mlir::FusedLoc>()) {
          auto fusedLoc = loc.cast<mlir::FusedLoc>();
          locs.push_back(fusedLoc.getLocations()[0]);
          locs.push_back(fusedLoc.getLocations()[1]);
        }
        LexicalScopeContext lexScope{locs[0], locs[1],
                                     builder.getInsertionBlock()};
        LexicalScopeGuard lexThenGuard{*this, &lexScope};
        resThen = buildStmt(thenS, /*useCurrentScope=*/true);
      },
      /*elseBuilder=*/
      [&](mlir::OpBuilder &b, mlir::Location loc) {
        auto fusedLoc = loc.cast<mlir::FusedLoc>();
        auto locBegin = fusedLoc.getLocations()[2];
        auto locEnd = fusedLoc.getLocations()[3];
        LexicalScopeContext lexScope{locBegin, locEnd,
                                     builder.getInsertionBlock()};
        LexicalScopeGuard lexElseGuard{*this, &lexScope};
        resElse = buildStmt(elseS, /*useCurrentScope=*/true);
      });

  return mlir::LogicalResult::success(resThen.succeeded() &&
                                      resElse.succeeded());
}

mlir::Value CIRGenFunction::buildAlloca(StringRef name, InitStyle initStyle,
                                        QualType ty, mlir::Location loc,
                                        CharUnits alignment) {
  auto getAllocaInsertPositionOp =
      [&](mlir::Block **insertBlock) -> mlir::Operation * {
    auto *parentBlock = currLexScope->getEntryBlock();

    auto lastAlloca = std::find_if(
        parentBlock->rbegin(), parentBlock->rend(),
        [](mlir::Operation &op) { return isa<mlir::cir::AllocaOp>(&op); });

    *insertBlock = parentBlock;
    if (lastAlloca == parentBlock->rend())
      return nullptr;
    return &*lastAlloca;
  };

  auto localVarTy = getCIRType(ty);
  auto localVarPtrTy =
      mlir::cir::PointerType::get(builder.getContext(), localVarTy);
  auto alignIntAttr = CGM.getSize(alignment);

  mlir::Value addr;
  {
    mlir::OpBuilder::InsertionGuard guard(builder);
    mlir::Block *insertBlock = nullptr;
    mlir::Operation *insertOp = getAllocaInsertPositionOp(&insertBlock);

    if (insertOp)
      builder.setInsertionPointAfter(insertOp);
    else {
      assert(insertBlock && "expected valid insertion block");
      // No previous alloca found, place this one in the beginning
      // of the block.
      builder.setInsertionPointToStart(insertBlock);
    }

    addr = builder.create<mlir::cir::AllocaOp>(loc, /*addr type*/ localVarPtrTy,
                                               /*var type*/ localVarTy, name,
                                               initStyle, alignIntAttr);
  }
  return addr;
}

mlir::Value CIRGenFunction::buildLoadOfScalar(LValue lvalue,
                                              SourceLocation Loc) {
  return buildLoadOfScalar(lvalue.getAddress(), lvalue.isVolatile(),
                           lvalue.getType(), Loc, lvalue.getBaseInfo(),
                           lvalue.isNontemporal());
}

mlir::Value CIRGenFunction::buildFromMemory(mlir::Value Value, QualType Ty) {
  // Bool has a different representation in memory than in registers.
  if (hasBooleanRepresentation(Ty)) {
    llvm_unreachable("NYI");
  }

  return Value;
}

mlir::Value CIRGenFunction::buildLoadOfScalar(Address Addr, bool Volatile,
                                              QualType Ty, SourceLocation Loc,
                                              LValueBaseInfo BaseInfo,
                                              bool isNontemporal) {
  if (!CGM.getCodeGenOpts().PreserveVec3Type) {
    if (Ty->isVectorType()) {
      llvm_unreachable("NYI");
    }
  }

  // Atomic operations have to be done on integral types
  LValue AtomicLValue = LValue::makeAddr(Addr, Ty, getContext(), BaseInfo);
  if (Ty->isAtomicType() || LValueIsSuitableForInlineAtomic(AtomicLValue)) {
    llvm_unreachable("NYI");
  }

  mlir::cir::LoadOp Load = builder.create<mlir::cir::LoadOp>(
      getLoc(Loc), Addr.getElementType(), Addr.getPointer());

  if (isNontemporal) {
    llvm_unreachable("NYI");
  }

  // TODO: TBAA

  // TODO: buildScalarRangeCheck

  return buildFromMemory(Load, Ty);
}

// Note: this function also emit constructor calls to support a MSVC extensions
// allowing explicit constructor function call.
RValue CIRGenFunction::buildCXXMemberCallExpr(const CXXMemberCallExpr *CE,
                                              ReturnValueSlot ReturnValue) {

  const Expr *callee = CE->getCallee()->IgnoreParens();

  if (isa<BinaryOperator>(callee))
    llvm_unreachable("NYI");

  const auto *ME = cast<MemberExpr>(callee);
  const auto *MD = cast<CXXMethodDecl>(ME->getMemberDecl());

  if (MD->isStatic()) {
    llvm_unreachable("NYI");
  }

  bool HasQualifier = ME->hasQualifier();
  NestedNameSpecifier *Qualifier = HasQualifier ? ME->getQualifier() : nullptr;
  bool IsArrow = ME->isArrow();
  const Expr *Base = ME->getBase();

  return buildCXXMemberOrOperatorMemberCallExpr(
      CE, MD, ReturnValue, HasQualifier, Qualifier, IsArrow, Base);
}

bool CIRGenFunction::isWrappedCXXThis(const Expr *object) {
  const Expr *base = object;
  while (!isa<CXXThisExpr>(base)) {
    // The result of a dynamic_cast can be null.
    if (isa<CXXDynamicCastExpr>(base))
      return false;

    if (const auto *ce = dyn_cast<CastExpr>(base)) {
      (void)ce;
      llvm_unreachable("NYI");
    } else if (const auto *pe = dyn_cast<ParenExpr>(base)) {
      (void)pe;
      llvm_unreachable("NYI");
    } else if (const auto *uo = dyn_cast<UnaryOperator>(base)) {
      (void)uo;
      llvm_unreachable("NYI");
    } else {
      return false;
    }
  }
  return true;
}
