//===- Lifetimecheck.cpp - emit diagnostic checks for lifetime violations -===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "mlir/Dialect/CIR/Passes.h"

#include "PassDetail.h"
#include "mlir/Dialect/CIR/IR/CIRDialect.h"

#include "llvm/ADT/SetOperations.h"
#include "llvm/ADT/SmallSet.h"

using namespace mlir;
using namespace cir;

namespace {
struct LifetimeCheckPass : public LifetimeCheckBase<LifetimeCheckPass> {
  LifetimeCheckPass() = default;
  void runOnOperation() override;

  void checkOperation(Operation *op);
  void checkFunc(Operation *op);
  void checkBlock(Block &block);

  void checkRegionWithScope(Region &region);
  void checkRegion(Region &region);

  void checkIf(IfOp op);
  void checkSwitch(SwitchOp op);
  void checkLoop(LoopOp op);
  void checkAlloca(AllocaOp op);
  void checkStore(StoreOp op);
  void checkLoad(LoadOp op);

  struct Options {
    enum : unsigned {
      None = 0,
      // Emit pset remarks only detecting invalid derefs
      RemarkPsetInvalid = 1,
      // Emit pset remarks for all derefs
      RemarkPsetAlways = 1 << 1,
      RemarkAll = 1 << 2,
      HistoryNull = 1 << 3,
      HistoryInvalid = 1 << 4,
      HistoryAll = 1 << 5,
    };
    unsigned val = None;

    void parseOptions(LifetimeCheckPass &pass) {
      for (auto &remark : pass.remarksList) {
        val |= StringSwitch<unsigned>(remark)
                   .Case("pset-invalid", RemarkPsetInvalid)
                   .Case("pset-always", RemarkPsetAlways)
                   .Case("all", RemarkAll)
                   .Default(None);
      }
      for (auto &h : pass.historyList) {
        val |= StringSwitch<unsigned>(h)
                   .Case("invalid", HistoryInvalid)
                   .Case("null", HistoryNull)
                   .Case("all", HistoryAll)
                   .Default(None);
      }
    }

    bool emitRemarkAll() { return val & RemarkAll; }
    bool emitRemarkPsetInvalid() {
      return emitRemarkAll() || val & RemarkPsetInvalid;
    }
    bool emitRemarkPsetAlways() {
      return emitRemarkAll() || val & RemarkPsetAlways;
    }

    bool emitHistoryAll() { return val & HistoryAll; }
    bool emitHistoryNull() { return emitHistoryAll() || val & HistoryNull; }
    bool emitHistoryInvalid() {
      return emitHistoryAll() || val & HistoryInvalid;
    }
  } opts;

  struct State {
    using DataTy = enum {
      Invalid,
      NullPtr,
      Global,
      LocalValue,
      NumKindsMinusOne = LocalValue
    };
    State() { val.setInt(Invalid); }
    State(DataTy d) { val.setInt(d); }
    State(mlir::Value v) { val.setPointerAndInt(v, LocalValue); }

    static constexpr int KindBits = 2;
    static_assert((1 << KindBits) > NumKindsMinusOne,
                  "Not enough room for kind!");
    llvm::PointerIntPair<mlir::Value, KindBits> val;

    /// Provide less/equal than operator for sorting / set ops.
    bool operator<(const State &RHS) const {
      // FIXME: note that this makes the ordering non-deterministic, do
      // we really care?
      if (val.getInt() == LocalValue && RHS.val.getInt() == LocalValue)
        return val.getPointer().getAsOpaquePointer() <
               RHS.val.getPointer().getAsOpaquePointer();
      else
        return val.getInt() < RHS.val.getInt();
    }
    bool operator==(const State &RHS) const {
      if (val.getInt() == LocalValue && RHS.val.getInt() == LocalValue)
        return val.getPointer() == RHS.val.getPointer();
      else
        return val.getInt() == RHS.val.getInt();
    }

    void dump(llvm::raw_ostream &OS = llvm::errs());

    static State getInvalid() { return {}; }
    static State getNullPtr() { return {NullPtr}; }
    static State getLocalValue(mlir::Value v) { return {v}; }
  };

  using PSetType = llvm::SmallSet<State, 4>;
  // FIXME: this should be a ScopedHashTable for consistency.
  using PMapType = llvm::DenseMap<mlir::Value, PSetType>;

  using PMapInvalidHistType =
      llvm::DenseMap<mlir::Value, std::pair<llvm::Optional<mlir::Location>,
                                            llvm::Optional<mlir::Value>>>;
  PMapInvalidHistType pmapInvalidHist;

  using PMapNullHistType =
      llvm::DenseMap<mlir::Value, llvm::Optional<mlir::Location>>;
  PMapNullHistType pmapNullHist;

  SmallPtrSet<mlir::Value, 8> ptrs;

  // Represents the scope context for IR operations (cir.scope, cir.if,
  // then/else regions, etc). Tracks the declaration of variables in the current
  // local scope.
  struct LexicalScopeContext {
    unsigned Depth = 0;
    LexicalScopeContext() = delete;

    llvm::PointerUnion<mlir::Region *, mlir::Operation *> parent;
    LexicalScopeContext(mlir::Region *R) : parent(R) {}
    LexicalScopeContext(mlir::Operation *Op) : parent(Op) {}
    ~LexicalScopeContext() = default;

    // Track all local values added in this scope
    llvm::SmallVector<mlir::Value, 4> localValues;

    void dumpLocalValues();
  };

  class LexicalScopeGuard {
    LifetimeCheckPass &Pass;
    LexicalScopeContext *OldVal = nullptr;

  public:
    LexicalScopeGuard(LifetimeCheckPass &p, LexicalScopeContext *L) : Pass(p) {
      if (Pass.currScope) {
        OldVal = Pass.currScope;
        L->Depth++;
      }
      Pass.currScope = L;
    }

    LexicalScopeGuard(const LexicalScopeGuard &) = delete;
    LexicalScopeGuard &operator=(const LexicalScopeGuard &) = delete;
    LexicalScopeGuard &operator=(LexicalScopeGuard &&other) = delete;

    void cleanup();
    void restore() { Pass.currScope = OldVal; }
    ~LexicalScopeGuard() {
      cleanup();
      restore();
    }
  };

  class PmapGuard {
    LifetimeCheckPass &Pass;
    PMapType *OldVal = nullptr;

  public:
    PmapGuard(LifetimeCheckPass &lcp, PMapType *L) : Pass(lcp) {
      if (Pass.currPmap) {
        OldVal = Pass.currPmap;
      }
      Pass.currPmap = L;
    }

    PmapGuard(const PmapGuard &) = delete;
    PmapGuard &operator=(const PmapGuard &) = delete;
    PmapGuard &operator=(PmapGuard &&other) = delete;

    void restore() { Pass.currPmap = OldVal; }
    ~PmapGuard() { restore(); }
  };

  LexicalScopeContext *currScope = nullptr;
  PMapType *currPmap = nullptr;
  PMapType &getPmap() { return *currPmap; }

  void joinPmaps(SmallVectorImpl<PMapType> &pmaps);
  void printPset(PSetType &pset, llvm::raw_ostream &OS = llvm::errs());
  void dumpPmap(PMapType &pmap);
};
} // namespace

static StringRef getVarNameFromValue(mlir::Value v) {
  if (auto allocaOp = dyn_cast<AllocaOp>(v.getDefiningOp()))
    return allocaOp.name();
  assert(0 && "how did it get here?");
  return "";
}

static Location getEndLoc(Location loc, int idx = 1) {
  auto fusedLoc = loc.dyn_cast<FusedLoc>();
  if (!fusedLoc)
    return loc;
  return fusedLoc.getLocations()[idx];
}

static Location getEndLocForHist(Operation *Op) {
  return getEndLoc(Op->getLoc());
}

static Location getEndLocForHist(Region *R) {
  auto ifOp = dyn_cast<IfOp>(R->getParentOp());
  assert(ifOp && "what other regions create their own scope?");
  if (&ifOp.thenRegion() == R)
    return getEndLoc(ifOp.getLoc());
  return getEndLoc(ifOp.getLoc(), /*idx=*/3);
}

static Location getEndLocForHist(LifetimeCheckPass::LexicalScopeContext &lsc) {
  assert(!lsc.parent.isNull() && "shouldn't be null");
  if (lsc.parent.is<Region *>())
    return getEndLocForHist(lsc.parent.get<Region *>());
  assert(lsc.parent.is<Operation *>() &&
         "Only support operation beyond this point");
  return getEndLocForHist(lsc.parent.get<Operation *>());
}

void LifetimeCheckPass::LexicalScopeGuard::cleanup() {
  auto *localScope = Pass.currScope;
  auto &pmap = Pass.getPmap();
  // If we are cleaning up at the function level, nothing
  // to do here cause we are past all possible deference points
  if (localScope->Depth == 0)
    return;

  // 2.3 - KILL(x) means to replace all occurrences of x and x' and x'' (etc.)
  // in the pmap with invalid. For example, if pmap is {(p1,{a}), (p2,{a'})},
  // KILL(a') would invalidate only p2, and KILL(a) would invalidate both p1 and
  // p2.
  for (auto pointee : localScope->localValues) {
    for (auto &mapEntry : pmap) {
      auto ptr = mapEntry.first;

      // We are deleting this entry anyways, nothing to do here.
      if (pointee == ptr)
        continue;

      // If the local value is part of this pset, it means
      // we need to invalidate it, otherwise keep searching.
      // FIXME: add support for x', x'', etc...
      auto &pset = mapEntry.second;
      State valState = State::getLocalValue(pointee);
      if (!pset.contains(valState))
        continue;

      // Erase the reference and mark this invalid.
      // FIXME: add a way to just mutate the state.
      pset.erase(valState);
      pset.insert(State::getInvalid());
      Pass.pmapInvalidHist[ptr] =
          std::make_pair(getEndLocForHist(*Pass.currScope), pointee);
    }
    // Delete the local value from pmap, since its gone now.
    pmap.erase(pointee);
  }
}

void LifetimeCheckPass::checkBlock(Block &block) {
  // Block main role is to hold a list of Operations.
  for (Operation &op : block.getOperations())
    checkOperation(&op);
}

void LifetimeCheckPass::checkRegion(Region &region) {
  for (Block &block : region.getBlocks())
    checkBlock(block);
}

void LifetimeCheckPass::checkRegionWithScope(Region &region) {
  // Add a new scope. Note that as part of the scope cleanup process
  // we apply section 2.3 KILL(x) functionality, turning relevant
  // references invalid.
  LexicalScopeContext lexScope{&region};
  LexicalScopeGuard scopeGuard{*this, &lexScope};
  for (Block &block : region.getBlocks())
    checkBlock(block);
}

void LifetimeCheckPass::checkFunc(Operation *op) {
  // FIXME: perhaps this should be a function pass, but for now make
  // sure we reset the state before looking at other functions.
  if (currPmap)
    getPmap().clear();
  pmapNullHist.clear();
  pmapInvalidHist.clear();

  // Add a new scope. Note that as part of the scope cleanup process
  // we apply section 2.3 KILL(x) functionality, turning relevant
  // references invalid.
  LexicalScopeContext lexScope{op};
  LexicalScopeGuard scopeGuard{*this, &lexScope};

  // Create a new pmap for this function.
  PMapType localPmap{};
  PmapGuard pmapGuard{*this, &localPmap};

  for (Region &region : op->getRegions())
    checkRegion(region);

  // FIXME: store the pmap result for this function, we
  // could do some interesting IPA stuff using this info.
}

// The join operation between pmap as described in section 2.3.
//
//  JOIN({pmap1,...,pmapN}) =>
//  { (p, pset1(p) U ... U psetN(p) | (p,*) U pmap1 U ... U pmapN }.
//
void LifetimeCheckPass::joinPmaps(SmallVectorImpl<PMapType> &pmaps) {
  for (auto &mapEntry : getPmap()) {
    auto &val = mapEntry.first;

    PSetType joinPset;
    for (auto &pmapOp : pmaps)
      llvm::set_union(joinPset, pmapOp[val]);

    getPmap()[val] = joinPset;
  }
}

void LifetimeCheckPass::checkLoop(LoopOp loopOp) {
  // 2.4.9. Loops
  //
  // A loop is treated as if it were the first two loop iterations unrolled
  // using an if. For example:
  //
  //  for (/*init*/; /*cond*/; /*incr*/)
  //   { /*body*/ }
  //
  // is treated as:
  //
  //  if (/*init*/; /*cond*/)
  //   { /*body*/; /*incr*/ }
  //  if (/*cond*/)
  //   { /*body*/ }
  //
  // See checkIf for additional explanations.
  SmallVector<PMapType, 4> pmapOps;
  SmallVector<Region *, 4> regionsToCheck;

  auto setupLoopRegionsToCheck = [&](bool isSubsequentTaken = false) {
    regionsToCheck.clear();
    switch (loopOp.kind()) {
    case LoopOpKind::For: {
      regionsToCheck.push_back(&loopOp.cond());
      regionsToCheck.push_back(&loopOp.body());
      if (!isSubsequentTaken)
        regionsToCheck.push_back(&loopOp.step());
      break;
    }
    case LoopOpKind::While: {
      regionsToCheck.push_back(&loopOp.cond());
      regionsToCheck.push_back(&loopOp.body());
      break;
    }
    case LoopOpKind::DoWhile: {
      // Note this is the reverse order from While above.
      regionsToCheck.push_back(&loopOp.body());
      regionsToCheck.push_back(&loopOp.cond());
      break;
    }
    }
  };

  // From 2.4.9 "Note":
  //
  // There are only three paths to analyze:
  // (1) never taken (the loop body was not entered)
  pmapOps.push_back(getPmap());

  // (2) first taken (the first pass through the loop body, which begins
  // with the loop entry pmap)
  PMapType loopExitPmap;
  {
    // Intentional copy from loop entry map
    loopExitPmap = getPmap();
    PmapGuard pmapGuard{*this, &loopExitPmap};
    setupLoopRegionsToCheck();
    for (auto *r : regionsToCheck)
      checkRegion(*r);
    pmapOps.push_back(loopExitPmap);
  }

  // (3) and subsequent taken (second or later iteration, which begins with the
  // loop body exit pmap and so takes into account any invalidations performed
  // in the loop body on any path that could affect the next loop).
  //
  // This ensures that a subsequent loop iteration does not use a Pointer that
  // was invalidated during a previous loop iteration.
  //
  // Because this analysis gives the same answer for each block of code (always
  // converges), all loop iterations after the first get the same answer and
  // so we only need to consider the second iteration, and so the analysis
  // algorithm remains linear, single-pass. As an optimization, if the loop
  // entry pmap is the same as the first loop body exit pmap, there is no need
  // to perform the analysis on the second loop iteration; the answer will be
  // the same.
  if (getPmap() != loopExitPmap) {
    // Intentional copy from first taken loop exit pmap
    PMapType otherTakenPmap = loopExitPmap;
    PmapGuard pmapGuard{*this, &otherTakenPmap};
    setupLoopRegionsToCheck(/*isSubsequentTaken=*/true);
    for (auto *r : regionsToCheck)
      checkRegion(*r);
    pmapOps.push_back(otherTakenPmap);
  }

  joinPmaps(pmapOps);
}

void LifetimeCheckPass::checkSwitch(SwitchOp switchOp) {
  // 2.4.7. A switch(cond) is treated as if it were an equivalent series of
  // non-nested if statements with single evaluation of cond; for example:
  //
  //    switch (a) {
  //      case 1:/*1*/
  //      case 2:/*2*/ break;
  //      default:/*3*/
  //    }
  //
  // is treated as:
  //
  //    if (auto& a=a; a==1) {/*1*/}
  //    else if (a==1 || a==2) {/*2*/}
  //    else {/*3*/}.
  //
  // See checkIf for additional explanations.
  SmallVector<PMapType, 2> pmapOps;

  // If there are no regions, pmap is the same.
  if (switchOp.regions().empty())
    return;

  auto isCaseFallthroughTerminated = [&](Region &r) {
    assert(r.getBlocks().size() == 1 && "cannot yet handle branches");
    Block &block = r.back();
    assert(!block.empty() && "case regions cannot be empty");

    // FIXME: do something special about return terminated?
    YieldOp y = dyn_cast<YieldOp>(block.back());
    if (!y)
      return false;
    if (y.isFallthrough())
      return true;
    return false;
  };

  auto regions = switchOp.regions();
  for (unsigned regionCurrent = 0, regionPastEnd = regions.size();
       regionCurrent != regionPastEnd; ++regionCurrent) {
    // Intentional pmap copy, basis to start new path.
    PMapType locaCasePmap = getPmap();
    PmapGuard pmapGuard{*this, &locaCasePmap};

    // At any given point, fallbacks (if not empty) will increase the
    // number of control-flow possibilities. For each region ending up
    // with a fallback, keep computing the pmap until we hit a region
    // that has a non-fallback terminator for the region.
    unsigned idx = regionCurrent;
    while (idx < regionPastEnd) {
      // Note that for 'if' regions we use checkRegionWithScope, since
      // there are lexical scopes associated with each region, this is
      // not the case for switch's.
      checkRegion(regions[idx]);
      if (!isCaseFallthroughTerminated(regions[idx]))
        break;
      idx++;
    }
    pmapOps.push_back(locaCasePmap);
  }

  joinPmaps(pmapOps);
}

void LifetimeCheckPass::checkIf(IfOp ifOp) {
  // Both then and else create their own lexical scopes, take that into account
  // while checking then/else.
  //
  // This is also the moment where pmaps are joined because flow forks:
  //    pmap(ifOp) = JOIN( pmap(then), pmap(else) )
  //
  // To that intent the pmap is copied out before checking each region and
  // pmap(ifOp) computed after analysing both paths.
  SmallVector<PMapType, 2> pmapOps;

  {
    PMapType localThenPmap = getPmap();
    PmapGuard pmapGuard{*this, &localThenPmap};
    checkRegionWithScope(ifOp.thenRegion());
    pmapOps.push_back(localThenPmap);
  }

  // In case there's no 'else' branch, the 'else' pmap is the same as
  // prior to the if condition.
  if (!ifOp.elseRegion().empty()) {
    PMapType localElsePmap = getPmap();
    PmapGuard pmapGuard{*this, &localElsePmap};
    checkRegionWithScope(ifOp.elseRegion());
    pmapOps.push_back(localElsePmap);
  } else {
    pmapOps.push_back(getPmap());
  }

  joinPmaps(pmapOps);
}

void LifetimeCheckPass::checkAlloca(AllocaOp allocaOp) {
  auto addr = allocaOp.addr();
  assert(!getPmap().count(addr) && "only one alloca for any given address");

  getPmap()[addr] = {};
  if (!allocaOp.isPointerType()) {
    // 2.4.2 - When a local Value x is declared, add (x, {x}) to pmap.
    getPmap()[addr].insert(State::getLocalValue(addr));
    currScope->localValues.push_back(addr);
    return;
  }

  // 2.4.2 - When a non-parameter non-member Pointer p is declared, add
  // (p, {invalid}) to pmap.
  ptrs.insert(addr);
  getPmap()[addr].insert(State::getInvalid());
  pmapInvalidHist[addr] = std::make_pair(allocaOp.getLoc(), llvm::None);

  // If other styles of initialization gets added, required to add support
  // here.
  assert(
      (allocaOp.initAttr().getValue() == mlir::cir::InitStyle::cinit ||
       allocaOp.initAttr().getValue() == mlir::cir::InitStyle::uninitialized) &&
      "other init styles tbd");
}

void LifetimeCheckPass::checkStore(StoreOp storeOp) {
  auto addr = storeOp.addr();

  // We only care about stores that change local pointers, local values
  // are not interesting here (just yet).
  if (!ptrs.count(addr))
    return;

  auto getArrayFromSubscript = [&](PtrStrideOp strideOp) -> mlir::Value {
    auto castOp = dyn_cast<CastOp>(strideOp.base().getDefiningOp());
    if (!castOp)
      return {};
    if (castOp.kind() != cir::CastKind::array_to_ptrdecay)
      return {};
    return castOp.src();
  };

  auto data = storeOp.value();
  // 2.4.2 - If the declaration includes an initialization, the
  // initialization is treated as a separate operation
  if (auto cstOp = dyn_cast<ConstantOp>(data.getDefiningOp())) {
    assert(cstOp.isNullPtr() && "not implemented");
    assert(getPmap().count(addr) && "address should always be valid");
    // 2.4.2 - If the initialization is default initialization or zero
    // initialization, set pset(p) = {null}; for example:
    //
    //  int* p; => pset(p) == {invalid}
    //  int* p{}; or string_view p; => pset(p) == {null}.
    //  int *p = nullptr; => pset(p) == {nullptr} => pset(p) == {null}
    getPmap()[addr].clear();
    getPmap()[addr].insert(State::getNullPtr());
    pmapNullHist[addr] = storeOp.value().getLoc();
    return;
  }

  if (auto allocaOp = dyn_cast<AllocaOp>(data.getDefiningOp())) {
    // p = &x;
    getPmap()[addr].clear();
    getPmap()[addr].insert(State::getLocalValue(data));
    return;
  }

  if (auto ptrStrideOp = dyn_cast<PtrStrideOp>(data.getDefiningOp())) {
    // p = &a[0];
    auto array = getArrayFromSubscript(ptrStrideOp);
    if (array) {
      getPmap()[addr].clear();
      getPmap()[addr].insert(State::getLocalValue(array));
    }
    return;
  }

  // From here on, some uninterestring store (for now?)
}

void LifetimeCheckPass::checkLoad(LoadOp loadOp) {
  auto addr = loadOp.addr();
  // Only interested in checking deference on top of pointer types.
  // Note that usually the use of the invalid address happens at the
  // load or store using the result of this loadOp.
  if (!getPmap().count(addr) || !ptrs.count(addr))
    return;

  if (!loadOp.isDeref())
    return;

  bool hasInvalid = getPmap()[addr].count(State::getInvalid());
  bool hasNullptr = getPmap()[addr].count(State::getNullPtr());

  auto emitPsetRemark = [&] {
    llvm::SmallString<128> psetStr;
    llvm::raw_svector_ostream Out(psetStr);
    printPset(getPmap()[addr], Out);
    emitRemark(loadOp.getLoc()) << "pset => " << Out.str();
  };

  bool psetRemarkEmitted = false;
  if (opts.emitRemarkPsetAlways()) {
    emitPsetRemark();
    psetRemarkEmitted = true;
  }

  // 2.4.2 - On every dereference of a Pointer p, enforce that p is valid.
  if (!hasInvalid && !hasNullptr)
    return;

  // Looks like we found a bad path leading to this deference point,
  // diagnose it.
  StringRef varName = getVarNameFromValue(addr);
  auto D = emitWarning(loadOp.getLoc());
  D << "use of invalid pointer '" << varName << "'";

  if (hasInvalid && opts.emitHistoryInvalid()) {
    assert(pmapInvalidHist.count(addr) && "expected invalid hist");
    auto &info = pmapInvalidHist[addr];
    auto &note = info.first;
    auto &pointee = info.second;

    if (pointee.hasValue()) {
      StringRef pointeeName = getVarNameFromValue(*pointee);
      D.attachNote(note) << "pointee '" << pointeeName
                         << "' invalidated at end of scope";
    } else {
      D.attachNote(note) << "uninitialized here";
    }
  }

  if (hasNullptr && opts.emitHistoryNull()) {
    assert(pmapNullHist.count(addr) && "expected nullptr hist");
    auto &note = pmapNullHist[addr];
    D.attachNote(*note) << "invalidated here";
  }

  if (!psetRemarkEmitted && opts.emitRemarkPsetInvalid())
    emitPsetRemark();
}

void LifetimeCheckPass::checkOperation(Operation *op) {
  if (isa<::mlir::ModuleOp>(op)) {
    for (Region &region : op->getRegions())
      checkRegion(region);
    return;
  }

  if (isa<ScopeOp>(op)) {
    // Add a new scope. Note that as part of the scope cleanup process
    // we apply section 2.3 KILL(x) functionality, turning relevant
    // references invalid.
    //
    // No need to create a new pmap when entering a new scope since it
    // doesn't cause control flow to diverge (as it does in presence
    // of cir::IfOp or cir::SwitchOp).
    //
    // Also note that for dangling pointers coming from if init stmts
    // should be caught just fine, given that a ScopeOp embraces a IfOp.
    LexicalScopeContext lexScope{op};
    LexicalScopeGuard scopeGuard{*this, &lexScope};
    for (Region &region : op->getRegions())
      checkRegion(region);
    return;
  }

  if (isa<FuncOp>(op))
    return checkFunc(op);
  if (auto ifOp = dyn_cast<IfOp>(op))
    return checkIf(ifOp);
  if (auto switchOp = dyn_cast<SwitchOp>(op))
    return checkSwitch(switchOp);
  if (auto loopOp = dyn_cast<LoopOp>(op))
    return checkLoop(loopOp);
  if (auto allocaOp = dyn_cast<AllocaOp>(op))
    return checkAlloca(allocaOp);
  if (auto storeOp = dyn_cast<StoreOp>(op))
    return checkStore(storeOp);
  if (auto loadOp = dyn_cast<LoadOp>(op))
    return checkLoad(loadOp);
}

void LifetimeCheckPass::runOnOperation() {
  opts.parseOptions(*this);
  Operation *op = getOperation();
  checkOperation(op);
}

std::unique_ptr<Pass> mlir::createLifetimeCheckPass() {
  return std::make_unique<LifetimeCheckPass>();
}

//===----------------------------------------------------------------------===//
// Dump & print helpers
//===----------------------------------------------------------------------===//

void LifetimeCheckPass::LexicalScopeContext::dumpLocalValues() {
  llvm::errs() << "Local values: { ";
  for (auto value : localValues) {
    llvm::errs() << getVarNameFromValue(value);
    llvm::errs() << ", ";
  }
  llvm::errs() << "}\n";
}

void LifetimeCheckPass::State::dump(llvm::raw_ostream &OS) {
  switch (val.getInt()) {
  case Invalid:
    OS << "invalid";
    break;
  case NullPtr:
    OS << "nullptr";
    break;
  case Global:
    OS << "global";
    break;
  case LocalValue:
    OS << getVarNameFromValue(val.getPointer());
    break;
  }
}

void LifetimeCheckPass::printPset(PSetType &pset, llvm::raw_ostream &OS) {
  OS << "{ ";
  auto size = pset.size();
  for (auto s : pset) {
    s.dump(OS);
    size--;
    if (size > 0)
      OS << ", ";
  }
  OS << " }";
}

void LifetimeCheckPass::dumpPmap(PMapType &pmap) {
  llvm::errs() << "pmap {\n";
  int entry = 0;
  for (auto &mapEntry : pmap) {
    llvm::errs() << "  " << entry << ": " << getVarNameFromValue(mapEntry.first)
                 << "  "
                 << "=> ";
    printPset(mapEntry.second);
    llvm::errs() << "\n";
    entry++;
  }
  llvm::errs() << "}\n";
}