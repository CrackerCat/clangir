//===- CIRDialect.cpp - MLIR CIR ops implementation -----------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file implements the CIR dialect and its operations.
//
//===----------------------------------------------------------------------===//

#include "mlir/Dialect/CIR/IR/CIRDialect.h"
#include "mlir/Dialect/CIR/IR/CIRAttrs.h"
#include "mlir/Dialect/CIR/IR/CIRTypes.h"
#include "mlir/Dialect/LLVMIR/LLVMTypes.h"
#include "mlir/IR/Builders.h"
#include "mlir/IR/DialectImplementation.h"
#include "mlir/IR/OpDefinition.h"
#include "mlir/IR/OpImplementation.h"
#include "mlir/IR/TypeUtilities.h"

using namespace mlir;
using namespace mlir::cir;

#include "mlir/Dialect/CIR/IR/CIROpsEnums.cpp.inc"
#include "mlir/Dialect/CIR/IR/CIROpsStructs.cpp.inc"

#include "mlir/Dialect/CIR/IR/CIROpsDialect.cpp.inc"

//===----------------------------------------------------------------------===//
// CIR Dialect
//===----------------------------------------------------------------------===//
namespace {
struct CIROpAsmDialectInterface : public OpAsmDialectInterface {
  using OpAsmDialectInterface::OpAsmDialectInterface;

  AliasResult getAlias(Type type, raw_ostream &os) const final {
    if (auto structType = type.dyn_cast<StructType>()) {
      os << structType.getTypeName();
      return AliasResult::OverridableAlias;
    }

    return AliasResult::NoAlias;
  }
};
} // namespace

/// Dialect initialization, the instance will be owned by the context. This is
/// the point of registration of types and operations for the dialect.
void cir::CIRDialect::initialize() {
  registerTypes();
  registerAttributes();
  addOperations<
#define GET_OP_LIST
#include "mlir/Dialect/CIR/IR/CIROps.cpp.inc"
      >();
  addInterfaces<CIROpAsmDialectInterface>();
}

//===----------------------------------------------------------------------===//
// ConstantOp
//===----------------------------------------------------------------------===//

static LogicalResult checkConstantTypes(mlir::Operation *op, mlir::Type opType,
                                        mlir::Attribute attrType) {
  if (attrType.isa<BoolAttr>()) {
    if (!opType.isa<mlir::cir::BoolType>())
      return op->emitOpError("result type (")
             << opType << ") must be '!cir.bool' for '" << attrType << "'";
    return success();
  }

  if (attrType.isa<IntegerAttr, FloatAttr>()) {
    if (attrType.getType() != opType) {
      return op->emitOpError("result type (")
             << opType << ") does not match value type (" << attrType.getType()
             << ")";
    }
    return success();
  }

  if (attrType.isa<mlir::cir::CstArrayAttr>()) {
    // CstArrayAttr is already verified to bing with cir.array type.
    return success();
  }

  if (attrType.isa<UnitAttr>()) {
    if (opType.isa<::mlir::cir::PointerType>())
      return success();
    return op->emitOpError("nullptr expects pointer type");
  }

  if (attrType.isa<SymbolRefAttr>()) {
    if (opType.isa<::mlir::cir::PointerType>())
      return success();
    return op->emitOpError("symbolref expects pointer type");
  }

  return op->emitOpError("cannot have value of type ") << attrType.getType();
}

static LogicalResult verify(cir::ConstantOp constOp) {
  // ODS already generates checks to make sure the result type is valid. We just
  // need to additionally check that the value's attribute type is consistent
  // with the result type.
  return checkConstantTypes(constOp.getOperation(), constOp.getType(),
                            constOp.value());
}

static ParseResult parseConstantValue(OpAsmParser &parser,
                                      mlir::Attribute &valueAttr) {
  if (succeeded(parser.parseOptionalKeyword("nullptr"))) {
    valueAttr = UnitAttr::get(parser.getContext());
    return success();
  }

  NamedAttrList attr;
  if (parser.parseAttribute(valueAttr, "value", attr).failed()) {
    return parser.emitError(parser.getCurrentLocation(),
                            "expected constant attribute to match type");
  }

  return success();
}

// FIXME: create a CIRCstAttr and hide this away for both global
// initialization and cir.cst operation.
static void printConstant(OpAsmPrinter &p, bool isNullPtr, Attribute value) {
  if (isNullPtr)
    p << "nullptr";
  else {
    p.printAttribute(value);
  }
}

static void printConstantValue(OpAsmPrinter &p, cir::ConstantOp op,
                               Attribute value) {
  printConstant(p, op.isNullPtr(), value);
}

OpFoldResult ConstantOp::fold(ArrayRef<Attribute> operands) { return value(); }

//===----------------------------------------------------------------------===//
// CastOp
//===----------------------------------------------------------------------===//

static LogicalResult verify(cir::CastOp castOp) {
  auto resType = castOp.result().getType();
  auto srcType = castOp.src().getType();

  switch (castOp.kind()) {
  case cir::CastKind::int_to_bool: {
    if (!resType.isa<mlir::cir::BoolType>())
      return castOp.emitOpError() << "requires !cir.bool type for result";
    if (!(srcType.isInteger(32) || srcType.isInteger(64)))
      return castOp.emitOpError() << "requires integral type for result";
    return success();
  }
  case cir::CastKind::integral: {
    if (!resType.isa<mlir::IntegerType>())
      return castOp.emitOpError() << "requires !IntegerType for result";
    if (!srcType.isa<mlir::IntegerType>())
      return castOp.emitOpError() << "requires !IntegerType for source";
    return success();
  }
  case cir::CastKind::array_to_ptrdecay: {
    auto arrayPtrTy = srcType.dyn_cast<mlir::cir::PointerType>();
    auto flatPtrTy = resType.dyn_cast<mlir::cir::PointerType>();
    if (!arrayPtrTy || !flatPtrTy)
      return castOp.emitOpError()
             << "requires !cir.ptr type for source and result";

    auto arrayTy = arrayPtrTy.getPointee().dyn_cast<mlir::cir::ArrayType>();
    if (!arrayTy)
      return castOp.emitOpError() << "requires !cir.array pointee";

    if (arrayTy.getEltType() != flatPtrTy.getPointee())
      return castOp.emitOpError()
             << "requires same type for array element and pointee result";
    return success();
  }
  }

  llvm_unreachable("Unknown CastOp kind?");
}

//===----------------------------------------------------------------------===//
// ReturnOp
//===----------------------------------------------------------------------===//

static mlir::LogicalResult checkReturnAndFunction(ReturnOp op,
                                                  FuncOp function) {
  // ReturnOps currently only have a single optional operand.
  if (op.getNumOperands() > 1)
    return op.emitOpError() << "expects at most 1 return operand";

  // The operand number and types must match the function signature.
  const auto &results = function.getType().getResults();
  if (op.getNumOperands() != results.size())
    return op.emitOpError()
           << "does not return the same number of values ("
           << op.getNumOperands() << ") as the enclosing function ("
           << results.size() << ")";

  // If the operation does not have an input, we are done.
  if (!op.hasOperand())
    return mlir::success();

  auto inputType = *op.operand_type_begin();
  auto resultType = results.front();

  // Check that the result type of the function matches the operand type.
  if (inputType == resultType)
    return mlir::success();

  return op.emitError() << "type of return operand (" << inputType
                        << ") doesn't match function result type ("
                        << resultType << ")";
}

static mlir::LogicalResult verify(ReturnOp op) {
  // Returns can be present in multiple different scopes, get the
  // wrapping function and start from there.
  auto *fnOp = op->getParentOp();
  while (!isa<FuncOp>(fnOp))
    fnOp = fnOp->getParentOp();

  // Make sure return types match function return type.
  if (checkReturnAndFunction(op, cast<FuncOp>(fnOp)).failed())
    return failure();

  return success();
}

//===----------------------------------------------------------------------===//
// IfOp
//===----------------------------------------------------------------------===//

static LogicalResult checkBlockTerminator(OpAsmParser &parser,
                                          llvm::SMLoc parserLoc,
                                          llvm::Optional<Location> l, Region *r,
                                          bool ensureTerm = true) {
  mlir::Builder &builder = parser.getBuilder();
  if (r->hasOneBlock()) {
    if (ensureTerm) {
      ::mlir::impl::ensureRegionTerminator(
          *r, builder, *l, [](OpBuilder &builder, Location loc) {
            OperationState state(loc, YieldOp::getOperationName());
            YieldOp::build(builder, state);
            return Operation::create(state);
          });
    } else {
      assert(r && "region must not be empty");
      Block &block = r->back();
      if (block.empty() || !block.back().hasTrait<OpTrait::IsTerminator>()) {
        return parser.emitError(
            parser.getCurrentLocation(),
            "blocks are expected to be explicitly terminated");
      }
    }
    return success();
  }

  // Empty regions don't need any handling.
  auto &blocks = r->getBlocks();
  if (blocks.size() == 0)
    return success();

  // Test that at least one block has a yield/return terminator. We can
  // probably make this a bit more strict.
  for (Block &block : blocks) {
    if (block.empty())
      continue;
    auto &op = block.back();
    if (op.hasTrait<mlir::OpTrait::IsTerminator>() &&
        isa<YieldOp, ReturnOp>(op)) {
      return success();
    }
  }

  parser.emitError(parserLoc,
                   "expected at least one block with cir.yield or cir.return");
  return failure();
}

static ParseResult parseIfOp(OpAsmParser &parser, OperationState &result) {
  // Create the regions for 'then'.
  result.regions.reserve(2);
  Region *thenRegion = result.addRegion();
  Region *elseRegion = result.addRegion();

  auto &builder = parser.getBuilder();
  OpAsmParser::OperandType cond;
  Type boolType = ::mlir::cir::BoolType::get(builder.getContext());

  if (parser.parseOperand(cond) ||
      parser.resolveOperand(cond, boolType, result.operands))
    return failure();

  // Parse the 'then' region.
  auto parseThenLoc = parser.getCurrentLocation();
  if (parser.parseRegion(*thenRegion, /*arguments=*/{},
                         /*argTypes=*/{}))
    return failure();
  if (checkBlockTerminator(parser, parseThenLoc, result.location, thenRegion)
          .failed())
    return failure();

  // If we find an 'else' keyword, parse the 'else' region.
  if (!parser.parseOptionalKeyword("else")) {
    auto parseElseLoc = parser.getCurrentLocation();
    if (parser.parseRegion(*elseRegion, /*arguments=*/{}, /*argTypes=*/{}))
      return failure();
    if (checkBlockTerminator(parser, parseElseLoc, result.location, elseRegion)
            .failed())
      return failure();
  }

  // Parse the optional attribute list.
  if (parser.parseOptionalAttrDict(result.attributes))
    return failure();
  return success();
}

bool shouldPrintTerm(mlir::Region &r) {
  if (!r.hasOneBlock())
    return true;
  auto *entryBlock = &r.front();
  if (entryBlock->empty())
    return false;
  if (isa<ReturnOp>(entryBlock->back()))
    return true;
  YieldOp y = dyn_cast<YieldOp>(entryBlock->back());
  if (y && !y.isPlain())
    return true;
  return false;
}

static void print(OpAsmPrinter &p, IfOp op) {
  p << " " << op.condition();
  auto &thenRegion = op.thenRegion();
  p.printRegion(thenRegion,
                /*printEntryBlockArgs=*/false,
                /*printBlockTerminators=*/shouldPrintTerm(thenRegion));

  // Print the 'else' regions if it exists and has a block.
  auto &elseRegion = op.elseRegion();
  if (!elseRegion.empty()) {
    p << " else";
    p.printRegion(elseRegion,
                  /*printEntryBlockArgs=*/false,
                  /*printBlockTerminators=*/shouldPrintTerm(elseRegion));
  }

  p.printOptionalAttrDict(op->getAttrs());
}

/// Default callback for IfOp builders. Inserts nothing for now.
void mlir::cir::buildTerminatedBody(OpBuilder &builder, Location loc) {}

/// Given the region at `index`, or the parent operation if `index` is None,
/// return the successor regions. These are the regions that may be selected
/// during the flow of control. `operands` is a set of optional attributes that
/// correspond to a constant value for each operand, or null if that operand is
/// not a constant.
void IfOp::getSuccessorRegions(Optional<unsigned> index,
                               ArrayRef<Attribute> operands,
                               SmallVectorImpl<RegionSuccessor> &regions) {
  // The `then` and the `else` region branch back to the parent operation.
  if (index.hasValue()) {
    regions.push_back(RegionSuccessor());
    return;
  }

  // Don't consider the else region if it is empty.
  Region *elseRegion = &this->elseRegion();
  if (elseRegion->empty())
    elseRegion = nullptr;

  // Otherwise, the successor is dependent on the condition.
  // bool condition;
  if (auto condAttr = operands.front().dyn_cast_or_null<IntegerAttr>()) {
    assert(0 && "not implemented");
    // condition = condAttr.getValue().isOneValue();
    // Add the successor regions using the condition.
    // regions.push_back(RegionSuccessor(condition ? &thenRegion() :
    // elseRegion));
    // return;
  }

  // If the condition isn't constant, both regions may be executed.
  regions.push_back(RegionSuccessor(&thenRegion()));
  // If the else region does not exist, it is not a viable successor.
  if (elseRegion)
    regions.push_back(RegionSuccessor(elseRegion));
  return;
}

void IfOp::build(OpBuilder &builder, OperationState &result, Value cond,
                 bool withElseRegion,
                 function_ref<void(OpBuilder &, Location)> thenBuilder,
                 function_ref<void(OpBuilder &, Location)> elseBuilder) {
  assert(thenBuilder && "the builder callback for 'then' must be present");

  result.addOperands(cond);

  OpBuilder::InsertionGuard guard(builder);
  Region *thenRegion = result.addRegion();
  builder.createBlock(thenRegion);
  thenBuilder(builder, result.location);

  Region *elseRegion = result.addRegion();
  if (!withElseRegion)
    return;

  builder.createBlock(elseRegion);
  elseBuilder(builder, result.location);
}

static LogicalResult verify(IfOp op) {
  return RegionBranchOpInterface::verifyTypes(op);
}

//===----------------------------------------------------------------------===//
// ScopeOp
//===----------------------------------------------------------------------===//

static ParseResult parseScopeOp(OpAsmParser &parser, OperationState &result) {
  // Create one region within 'scope'.
  result.regions.reserve(1);
  Region *scopeRegion = result.addRegion();
  auto loc = parser.getCurrentLocation();

  // Parse the scope region.
  if (parser.parseRegion(*scopeRegion, /*arguments=*/{}, /*argTypes=*/{}))
    return failure();

  if (checkBlockTerminator(parser, loc, result.location, scopeRegion).failed())
    return failure();

  // Parse the optional attribute list.
  if (parser.parseOptionalAttrDict(result.attributes))
    return failure();
  return success();
}

static void print(OpAsmPrinter &p, ScopeOp op) {
  auto &scopeRegion = op.scopeRegion();
  p.printRegion(scopeRegion,
                /*printEntryBlockArgs=*/false,
                /*printBlockTerminators=*/shouldPrintTerm(scopeRegion));

  p.printOptionalAttrDict(op->getAttrs());
}

/// Given the region at `index`, or the parent operation if `index` is None,
/// return the successor regions. These are the regions that may be selected
/// during the flow of control. `operands` is a set of optional attributes that
/// correspond to a constant value for each operand, or null if that operand is
/// not a constant.
void ScopeOp::getSuccessorRegions(Optional<unsigned> index,
                                  ArrayRef<Attribute> operands,
                                  SmallVectorImpl<RegionSuccessor> &regions) {
  // The only region always branch back to the parent operation.
  if (index.hasValue()) {
    regions.push_back(RegionSuccessor());
    return;
  }

  // If the condition isn't constant, both regions may be executed.
  regions.push_back(RegionSuccessor(&scopeRegion()));
}

void ScopeOp::build(OpBuilder &builder, OperationState &result,
                    TypeRange resultTypes,
                    function_ref<void(OpBuilder &, Location)> scopeBuilder) {
  assert(scopeBuilder && "the builder callback for 'then' must be present");
  result.addTypes(resultTypes);

  OpBuilder::InsertionGuard guard(builder);
  Region *scopeRegion = result.addRegion();
  builder.createBlock(scopeRegion);
  scopeBuilder(builder, result.location);
}

static LogicalResult verify(ScopeOp op) {
  return RegionBranchOpInterface::verifyTypes(op);
}

//===----------------------------------------------------------------------===//
// YieldOp
//===----------------------------------------------------------------------===//

static mlir::LogicalResult verify(YieldOp op) {
  auto isDominatedByLoopOrSwitch = [](Operation *parentOp) {
    while (!llvm::isa<FuncOp>(parentOp)) {
      if (llvm::isa<cir::SwitchOp, cir::LoopOp>(parentOp))
        return true;
      parentOp = parentOp->getParentOp();
    }
    return false;
  };

  auto isDominatedByLoop = [](Operation *parentOp) {
    while (!llvm::isa<FuncOp>(parentOp)) {
      if (llvm::isa<cir::LoopOp>(parentOp))
        return true;
      parentOp = parentOp->getParentOp();
    }
    return false;
  };

  if (op.isBreak()) {
    if (!isDominatedByLoopOrSwitch(op->getParentOp()))
      return op.emitOpError()
             << "shall be dominated by 'cir.loop' or 'cir.switch'";
    return mlir::success();
  }

  if (op.isContinue()) {
    if (!isDominatedByLoop(op->getParentOp()))
      return op.emitOpError() << "shall be dominated by 'cir.loop'";
    return mlir::success();
  }

  if (op.isFallthrough()) {
    if (!llvm::isa<SwitchOp>(op->getParentOp()))
      return op.emitOpError()
             << "fallthrough only expected within 'cir.switch'";
    return mlir::success();
  }

  return mlir::success();
}

//===----------------------------------------------------------------------===//
// BrOp
//===----------------------------------------------------------------------===//

Optional<MutableOperandRange>
BrOp::getMutableSuccessorOperands(unsigned index) {
  assert(index == 0 && "invalid successor index");
  // Current block targets do not have operands.
  return llvm::None;
}

Block *BrOp::getSuccessorForOperands(ArrayRef<Attribute>) { return dest(); }

//===----------------------------------------------------------------------===//
// BrCondOp
//===----------------------------------------------------------------------===//

Optional<MutableOperandRange>
BrCondOp::getMutableSuccessorOperands(unsigned index) {
  assert(index < getNumSuccessors() && "invalid successor index");
  return index == 0 ? destOperandsTrueMutable() : destOperandsFalseMutable();
}

Block *BrCondOp::getSuccessorForOperands(ArrayRef<Attribute> operands) {
  if (IntegerAttr condAttr = operands.front().dyn_cast_or_null<IntegerAttr>())
    return condAttr.getValue().isOneValue() ? destTrue() : destFalse();
  return nullptr;
}

//===----------------------------------------------------------------------===//
// SwitchOp
//===----------------------------------------------------------------------===//

ParseResult
parseSwitchOp(OpAsmParser &parser,
              llvm::SmallVectorImpl<std::unique_ptr<::mlir::Region>> &regions,
              ::mlir::ArrayAttr &casesAttr,
              mlir::OpAsmParser::OperandType &cond, mlir::Type &condType) {
  ::mlir::IntegerType intCondType;
  SmallVector<mlir::Attribute, 4> cases;

  auto parseAndCheckRegion = [&]() -> ParseResult {
    // Parse region attached to case
    regions.emplace_back(new Region);
    Region &currRegion = *regions.back().get();
    auto parserLoc = parser.getCurrentLocation();
    if (parser.parseRegion(currRegion, /*arguments=*/{}, /*argTypes=*/{})) {
      regions.clear();
      return failure();
    }

    if (currRegion.empty()) {
      return parser.emitError(parser.getCurrentLocation(),
                              "case region shall not be empty");
    }

    if (checkBlockTerminator(parser, parserLoc, llvm::None, &currRegion,
                             /*ensureTerm=*/false)
            .failed())
      return failure();
    return success();
  };

  auto parseCase = [&]() -> ParseResult {
    auto loc = parser.getCurrentLocation();
    if (parser.parseKeyword("case").failed())
      return parser.emitError(loc, "expected 'case' keyword here");

    if (parser.parseLParen().failed())
      return parser.emitError(parser.getCurrentLocation(), "expected '('");

    ::llvm::StringRef attrStr;
    ::mlir::NamedAttrList attrStorage;

    //   case (equal, 20) {
    //   ...
    // 1. Get the case kind
    // 2. Get the value (next in list)

    // These needs to be in sync with CIROps.td
    if (parser.parseOptionalKeyword(&attrStr, {"default", "equal", "anyof"})) {
      ::mlir::StringAttr attrVal;
      ::mlir::OptionalParseResult parseResult = parser.parseOptionalAttribute(
          attrVal, parser.getBuilder().getNoneType(), "kind", attrStorage);
      if (parseResult.hasValue()) {
        if (failed(*parseResult))
          return ::mlir::failure();
        attrStr = attrVal.getValue();
      }
    }

    if (attrStr.empty()) {
      return parser.emitError(
          loc, "expected string or keyword containing one of the following "
               "enum values for attribute 'kind' [default, equal, anyof]");
    }

    auto attrOptional = ::mlir::cir::symbolizeCaseOpKind(attrStr.str());
    if (!attrOptional)
      return parser.emitError(loc, "invalid ")
             << "kind attribute specification: \"" << attrStr << '"';

    auto kindAttr = ::mlir::cir::CaseOpKindAttr::get(
        parser.getBuilder().getContext(), attrOptional.getValue());

    // `,` value or `,` [values,...]
    SmallVector<mlir::Attribute, 4> caseEltValueListAttr;
    mlir::ArrayAttr caseValueList;

    switch (kindAttr.getValue()) {
    case cir::CaseOpKind::Equal: {
      if (parser.parseComma().failed())
        return mlir::failure();
      int64_t val = 0;
      if (parser.parseInteger(val).failed())
        return ::mlir::failure();
      caseEltValueListAttr.push_back(mlir::IntegerAttr::get(intCondType, val));
      break;
    }
    case cir::CaseOpKind::Anyof: {
      if (parser.parseComma().failed())
        return mlir::failure();
      if (parser.parseLSquare().failed())
        return mlir::failure();
      parser.parseCommaSeparatedList([&]() {
        int64_t val = 0;
        if (parser.parseInteger(val).failed())
          return ::mlir::failure();
        caseEltValueListAttr.push_back(
            mlir::IntegerAttr::get(intCondType, val));
        return ::mlir::success();
      });
      if (parser.parseRSquare().failed())
        return mlir::failure();
      break;
    }
    case cir::CaseOpKind::Default: {
      if (parser.parseRParen().failed())
        return parser.emitError(parser.getCurrentLocation(), "expected ')'");
      cases.push_back(cir::CaseAttr::get(parser.getBuilder().getArrayAttr({}),
                                         kindAttr, parser.getContext()));
      return parseAndCheckRegion();
    }
    }

    caseValueList = parser.getBuilder().getArrayAttr(caseEltValueListAttr);
    cases.push_back(
        cir::CaseAttr::get(caseValueList, kindAttr, parser.getContext()));
    if (succeeded(parser.parseOptionalColon())) {
      Type caseIntTy;
      if (parser.parseType(caseIntTy).failed())
        return parser.emitError(parser.getCurrentLocation(), "expected type");
      if (intCondType != caseIntTy)
        return parser.emitError(parser.getCurrentLocation(),
                                "expected a match with the condition type");
    }
    if (parser.parseRParen().failed())
      return parser.emitError(parser.getCurrentLocation(), "expected ')'");
    return parseAndCheckRegion();
  };

  if (parser.parseLParen())
    return ::mlir::failure();

  if (parser.parseOperand(cond))
    return ::mlir::failure();
  if (parser.parseColon())
    return ::mlir::failure();
  if (parser.parseCustomTypeWithFallback(intCondType))
    return ::mlir::failure();
  condType = intCondType;
  if (parser.parseRParen())
    return ::mlir::failure();

  if (parser
          .parseCommaSeparatedList(OpAsmParser::Delimiter::Square, parseCase,
                                   " in cases list")
          .failed())
    return failure();

  casesAttr = parser.getBuilder().getArrayAttr(cases);
  return ::mlir::success();
}

void printSwitchOp(OpAsmPrinter &p, SwitchOp op,
                   mlir::MutableArrayRef<::mlir::Region> regions,
                   mlir::ArrayAttr casesAttr, mlir::Value condition,
                   mlir::Type condType) {
  int idx = 0, lastIdx = regions.size() - 1;

  p << "(";
  p << condition;
  p << " : ";
  p.printStrippedAttrOrType(condType);
  p << ") [";
  // FIXME: ideally we want some extra indentation for "cases" but too
  // cumbersome to pull it out now, since most handling is private. Perhaps
  // better improve overall mechanism.
  p.printNewline();
  for (auto &r : regions) {
    p << "case (";

    auto attr = casesAttr[idx].cast<CaseAttr>();
    auto kind = attr.kind().getValue();
    assert((kind == CaseOpKind::Default || kind == CaseOpKind::Equal ||
            kind == CaseOpKind::Anyof) &&
           "unknown case");

    // Case kind
    p << stringifyCaseOpKind(kind);

    // Case value
    switch (kind) {
    case cir::CaseOpKind::Equal: {
      p << ", ";
      p.printStrippedAttrOrType(attr.value()[0]);
      break;
    }
    case cir::CaseOpKind::Anyof: {
      p << ", [";
      llvm::interleaveComma(attr.value(), p, [&](const Attribute &a) {
        p.printAttributeWithoutType(a);
      });
      p << "] : ";
      p.printType(attr.value()[0].getType());
      break;
    }
    case cir::CaseOpKind::Default:
      break;
    }

    p << ") ";
    p.printRegion(r, /*printEntryBLockArgs=*/false,
                  /*printBlockTerminators=*/true);
    if (idx < lastIdx)
      p << ",";
    p.printNewline();
    idx++;
  }
  p << "]";
}

/// Given the region at `index`, or the parent operation if `index` is None,
/// return the successor regions. These are the regions that may be selected
/// during the flow of control. `operands` is a set of optional attributes
/// that correspond to a constant value for each operand, or null if that
/// operand is not a constant.
void SwitchOp::getSuccessorRegions(Optional<unsigned> index,
                                   ArrayRef<Attribute> operands,
                                   SmallVectorImpl<RegionSuccessor> &regions) {
  // If any index all the underlying regions branch back to the parent
  // operation.
  if (index.hasValue()) {
    regions.push_back(RegionSuccessor());
    return;
  }

  for (auto &r : this->regions()) {
    // If we can figure out the case stmt we are landing, this can be
    // overly simplified.
    // bool condition;
    if (auto condAttr = operands.front().dyn_cast_or_null<IntegerAttr>()) {
      assert(0 && "not implemented");
      (void)r;
      // condition = condAttr.getValue().isOneValue();
      // Add the successor regions using the condition.
      // regions.push_back(RegionSuccessor(condition ? &thenRegion() :
      // elseRegion));
      // return;
    }
  }

  // If the condition isn't constant, all regions may be executed.
  for (auto &r : this->regions())
    regions.push_back(RegionSuccessor(&r));
  return;
}

static LogicalResult verify(SwitchOp op) {
  if (op.regions().empty())
    return success();
  // FIXME: add a verifier that ensures there's only one block per case
  // region where "cir.yield fallthrough" is used.
  return RegionBranchOpInterface::verifyTypes(op);
}

void SwitchOp::build(
    OpBuilder &builder, OperationState &result, Value cond,
    function_ref<void(OpBuilder &, Location, OperationState &)> switchBuilder) {
  assert(switchBuilder && "the builder callback for regions must be present");
  OpBuilder::InsertionGuard guardSwitch(builder);
  result.addOperands({cond});
  switchBuilder(builder, result.location, result);
}

//===----------------------------------------------------------------------===//
// LoopOp
//===----------------------------------------------------------------------===//

void LoopOp::build(OpBuilder &builder, OperationState &result,
                   cir::LoopOpKind kind,
                   function_ref<void(OpBuilder &, Location)> condBuilder,
                   function_ref<void(OpBuilder &, Location)> bodyBuilder,
                   function_ref<void(OpBuilder &, Location)> stepBuilder) {
  OpBuilder::InsertionGuard guard(builder);
  ::mlir::cir::LoopOpKindAttr kindAttr =
      cir::LoopOpKindAttr::get(builder.getContext(), kind);
  result.addAttribute("kind", kindAttr);

  Region *condRegion = result.addRegion();
  builder.createBlock(condRegion);
  condBuilder(builder, result.location);

  Region *bodyRegion = result.addRegion();
  builder.createBlock(bodyRegion);
  bodyBuilder(builder, result.location);

  Region *stepRegion = result.addRegion();
  builder.createBlock(stepRegion);
  stepBuilder(builder, result.location);
}

/// Given the region at `index`, or the parent operation if `index` is None,
/// return the successor regions. These are the regions that may be selected
/// during the flow of control. `operands` is a set of optional attributes
/// that correspond to a constant value for each operand, or null if that
/// operand is not a constant.
void LoopOp::getSuccessorRegions(Optional<unsigned> index,
                                 ArrayRef<Attribute> operands,
                                 SmallVectorImpl<RegionSuccessor> &regions) {
  // TODO: have a enum/getter for regions.
  if (index.hasValue()) {
    switch (*index) {
    case 0: // cond region
      // In a cond region we could either continue the loop (back to the
      // parent) or go to the body region.
      regions.push_back(RegionSuccessor());
      regions.push_back(RegionSuccessor(&this->body()));
      break;
    case 1: // body region
      // Normal body regions could go to step or back to cond.
      if (kind() == LoopOpKind::For)
        regions.push_back(RegionSuccessor(&this->step()));
      regions.push_back(RegionSuccessor(&this->cond()));
      // FIXME: it's also possible to go back to the parent in case a
      // 'cir.yield break' or 'cir.yield return' is used somewhere in
      // the body, but we currently lack such analysis data.
      break;
    case 2: // step region
      // Can only go back to the condition
      regions.push_back(RegionSuccessor(&this->cond()));
      break;
    default:
      assert(0 && "unknown");
    }

    regions.push_back(RegionSuccessor());
    return;
  }

  // FIXME: use operands to shortcut some loops.

  // Parent can transfer control flow to both body and cond, depending
  // on the kind of loop used.
  if (kind() == LoopOpKind::For || kind() == LoopOpKind::While)
    regions.push_back(RegionSuccessor(&this->cond()));
  else if (kind() == LoopOpKind::DoWhile)
    regions.push_back(RegionSuccessor(&this->body()));
}

LogicalResult LoopOp::moveOutOfLoop(ArrayRef<Operation *> ops) {
  assert(0 && "not implemented");
  return success();
}

Region &LoopOp::getLoopBody() { return body(); }

bool LoopOp::isDefinedOutsideOfLoop(Value value) {
  // Loop variables are defined in the surrounding loop cir.scope
  auto *valDefRegion = value.getParentRegion();
  ScopeOp scopeOp =
      dyn_cast_or_null<ScopeOp>(this->getOperation()->getParentOp());
  assert(scopeOp && "expected cir.scope");

  return !scopeOp.scopeRegion().isAncestor(valDefRegion);
}

static LogicalResult verify(LoopOp op) {
  // Cond regions should only terminate with plain 'cir.yield' or
  // 'cir.yield continue'.
  auto terminateError = [&]() {
    return op.emitOpError() << "cond region must be terminated with "
                               "'cir.yield' or 'cir.yield continue'";
  };

  auto &blocks = op.cond().getBlocks();
  for (Block &block : blocks) {
    if (block.empty())
      continue;
    auto &op = block.back();
    if (isa<BrCondOp>(op))
      continue;
    if (!isa<YieldOp>(op))
      terminateError();
    auto y = cast<YieldOp>(op);
    if (!(y.isPlain() || y.isContinue()))
      terminateError();
  }

  return RegionBranchOpInterface::verifyTypes(op);
}

//===----------------------------------------------------------------------===//
// GlobalOp
//===----------------------------------------------------------------------===//

static void printGlobalOpTypeAndInitialValue(OpAsmPrinter &p, GlobalOp op,
                                             TypeAttr type,
                                             Attribute initAttr) {
  auto printType = [&]() { p << ": " << type; };
  if (!op.isDeclaration()) {
    p << "= ";
    // This also prints the type...
    printConstant(p, initAttr.isa<UnitAttr>(), initAttr);
    if (initAttr.isa<SymbolRefAttr>())
      printType();
  } else {
    printType();
  }
}

static ParseResult
parseGlobalOpTypeAndInitialValue(OpAsmParser &parser, TypeAttr &typeAttr,
                                 Attribute &initialValueAttr) {
  if (parser.parseOptionalEqual().failed()) {
    // Absence of equal means a declaration, so we need to parse the type.
    //  cir.global @a : i32
    Type type;
    if (parser.parseColonType(type))
      return failure();
    typeAttr = TypeAttr::get(type);
    return success();
  }

  // Parse constant with initializer, examples:
  //  cir.global @y = 3.400000e+00 : f32
  //  cir.global @rgb = #cir.cst_array<[...] : !cir.array<i8 x 3>>
  if (parseConstantValue(parser, initialValueAttr).failed())
    return failure();

  mlir::Type opTy = initialValueAttr.getType();
  if (initialValueAttr.isa<SymbolRefAttr>()) {
    if (parser.parseColonType(opTy))
      return failure();
  }

  typeAttr = TypeAttr::get(opTy);
  return success();
}

static LogicalResult verify(GlobalOp op) {
  // Verify that the initial value, if present, is either a unit attribute or
  // an attribute CIR supports.
  if (op.initial_value().hasValue())
    return checkConstantTypes(op.getOperation(), op.sym_type(),
                              *op.initial_value());

  if (Optional<uint64_t> alignAttr = op.alignment()) {
    uint64_t alignment = alignAttr.getValue();
    if (!llvm::isPowerOf2_64(alignment))
      return op->emitError() << "alignment attribute value " << alignment
                             << " is not a power of 2";
  }

  // TODO: verify visibility for declarations?
  return success();
}

void GlobalOp::build(OpBuilder &odsBuilder, OperationState &odsState,
                     StringRef sym_name, Type sym_type, bool isConstant) {
  odsState.addAttribute(sym_nameAttrName(odsState.name),
                        odsBuilder.getStringAttr(sym_name));
  odsState.addAttribute(sym_typeAttrName(odsState.name),
                        ::mlir::TypeAttr::get(sym_type));
  if (isConstant)
    odsState.addAttribute("constant", odsBuilder.getUnitAttr());
}

//===----------------------------------------------------------------------===//
// GetGlobalOp
//===----------------------------------------------------------------------===//

LogicalResult
GetGlobalOp::verifySymbolUses(SymbolTableCollection &symbolTable) {
  // Verify that the result type underlying pointer type matches the type of the
  // referenced cir.global op.
  auto global =
      symbolTable.lookupNearestSymbolFrom<GlobalOp>(*this, nameAttr());
  if (!global)
    return emitOpError("'")
           << name() << "' does not reference a valid cir.global";

  auto resultType = addr().getType().dyn_cast<PointerType>();
  if (!resultType || global.sym_type() != resultType.getPointee())
    return emitOpError("result type pointee type '")
           << resultType.getPointee() << "' does not match type "
           << global.sym_type() << " of the global @" << name();
  return success();
}

//===----------------------------------------------------------------------===//
// CIR defined traits
//===----------------------------------------------------------------------===//

LogicalResult
mlir::OpTrait::impl::verifySameFirstOperandAndResultType(Operation *op) {
  if (failed(verifyAtLeastNOperands(op, 1)) || failed(verifyOneResult(op)))
    return failure();

  auto type = op->getResult(0).getType();
  auto opType = op->getOperand(0).getType();

  if (type != opType)
    return op->emitOpError()
           << "requires the same type for first operand and result";

  return success();
}

//===----------------------------------------------------------------------===//
// CIR attributes
//===----------------------------------------------------------------------===//

LogicalResult mlir::cir::CstArrayAttr::verify(
    ::llvm::function_ref<::mlir::InFlightDiagnostic()> emitError,
    ::mlir::Type type, Attribute attr) {

  mlir::cir::ArrayType at = type.cast<mlir::cir::ArrayType>();
  if (!(attr.isa<mlir::ArrayAttr>() || attr.isa<mlir::StringAttr>()))
    return emitError() << "constant array expects ArrayAttr or StringAttr";

  if (auto strAttr = attr.dyn_cast<mlir::StringAttr>()) {
    auto intTy = at.getEltType().dyn_cast<mlir::IntegerType>();
    // TODO: add CIR type for char.
    if (!intTy || intTy.getWidth() != 8) {
      emitError() << "constant array element for string literals expects i8 "
                     "array element type";
      return failure();
    }
    return success();
  }

  assert(attr.isa<mlir::ArrayAttr>());
  auto arrayAttr = attr.cast<mlir::ArrayAttr>();

  // Make sure both number of elements and subelement types match type.
  if (at.getSize() != arrayAttr.size())
    return emitError() << "constant array size should match type size";
  LogicalResult eltTypeCheck = success();
  arrayAttr.walkSubElements(
      [&](Attribute attr) {
        // Once we find a mismatch, stop there.
        if (eltTypeCheck.failed())
          return;
        if (attr.getType() != at.getEltType()) {
          eltTypeCheck = failure();
          emitError()
              << "constant array element should match array element type";
        }
      },
      [&](Type type) {});
  return eltTypeCheck;
}

::mlir::Attribute CstArrayAttr::parse(::mlir::AsmParser &parser,
                                      ::mlir::Type type) {
  ::mlir::FailureOr<::mlir::Type> resultTy;
  ::mlir::FailureOr<Attribute> resultVal;
  ::llvm::SMLoc loc = parser.getCurrentLocation();
  (void)loc;
  // Parse literal '<'
  if (parser.parseLess())
    return {};

  // Parse variable 'value'
  resultVal = ::mlir::FieldParser<Attribute>::parse(parser);
  if (failed(resultVal)) {
    parser.emitError(parser.getCurrentLocation(),
                     "failed to parse CstArrayAttr parameter 'value' which is "
                     "to be a `Attribute`");
    return {};
  }

  // ArrayAttrs have per-element type, not the type of the array...
  auto attrType = resultVal->getType();
  if (attrType.isa<NoneType>()) {
    // Parse literal ':'
    if (parser.parseColon())
      return {};

    // Parse variable 'type'
    resultTy = ::mlir::FieldParser<::mlir::Type>::parse(parser);
    if (failed(resultTy)) {
      parser.emitError(parser.getCurrentLocation(),
                       "failed to parse CstArrayAttr parameter 'type' which is "
                       "to be a `::mlir::Type`");
      return {};
    }
  } else {
    resultTy = resultVal->getType();
  }

  // Parse literal '>'
  if (parser.parseGreater())
    return {};
  return parser.getChecked<CstArrayAttr>(
      loc, parser.getContext(), resultTy.getValue(), resultVal.getValue());
}

void CstArrayAttr::print(::mlir::AsmPrinter &printer) const {
  printer << "<";
  printer.printStrippedAttrOrType(getValue());
  auto attrType = getValue().getType();
  if (attrType.isa<NoneType>()) {
    printer << ' ' << ":";
    printer << ' ';
    printer.printStrippedAttrOrType(getType());
  }
  printer << ">";
}

//===----------------------------------------------------------------------===//
// TableGen'd op method definitions
//===----------------------------------------------------------------------===//

#define GET_OP_CLASSES
#include "mlir/Dialect/CIR/IR/CIROps.cpp.inc"
