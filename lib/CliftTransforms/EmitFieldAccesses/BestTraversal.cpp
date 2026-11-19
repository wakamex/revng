//
// This file is distributed under the MIT License. See LICENSE.md for details.
//
#include <algorithm>
#include <compare>
#include <cstdint>
#include <optional>


#include "revng/ADT/RecursiveCoroutine.h"
#include "revng/Clift/Clift.h"
#include "revng/Support/Assert.h"

#include "BestTraversal.h"
#include "PointerArithmetic.h"

using namespace clift;

static Logger Log("best-traversal");

/// Helper function which converts a generic `ArrayPath` to a compatible form
/// used to store the `array` traversal into the `Traversal` class. The
/// re-ordering in descending `Stride` order is provided by the comparison
/// operator of `ArrayShape`
static llvm::SmallVector<ArrayShape>
arrayPathToSortedVector(const ArrayPath &Path) {
  llvm::SmallVector<ArrayShape> Result;
  Result.reserve(Path.size());
  for (const NestedArrayShape &Nested : Path) {
    ArrayShape Shape;
    Shape.NumElements = Nested.NumElements;
    Shape.Stride = Nested.Stride;
    Result.push_back(Shape);
  }
  llvm::sort(Result);
  return Result;
}

// =============================================================================
// `ArrayShape` class methods
// =============================================================================

// =============================================================================
// `Traversal` class methods
// =============================================================================

Traversal::Traversal(mlir::Type TargetType,
                     int64_t StartOffset,
                     int64_t LeftoverOffset,
                     std::vector<uint64_t> TraversedFields,
                     const ArrayPath &Path) :
  TargetType(TargetType),
  StartOffset(StartOffset),
  LeftoverOffset(LeftoverOffset),
  TraversedFields(std::move(TraversedFields)),
  TraversedArrays(arrayPathToSortedVector(Path)) {
}

int64_t Traversal::depth() const {
  return TraversedArrays.size() + TraversedFields.size();
}

int64_t Traversal::begin() const {
  return StartOffset + LeftoverOffset;
}

int64_t Traversal::end() const {
  return begin() + getObjectSizeOrZero(TargetType);
}

void Traversal::dump() const {

  Log << "\nDumping Traversal:\n";

  // We dump the `TargetType` on which the `Traversal` lands on
  Log << "  TargetType: ";
  TargetType.print(*Log.getAsLLVMStream());
  Log << "\n";

  // Dump the `StartOffset` and `LeftoverOffset`
  Log << "  StartOffset: " << StartOffset << "\n";
  Log << "  LeftoverOffset: " << LeftoverOffset << "\n";

  // Dump all the `Traversed Fields` which this `Traversal` describes
  Log << "  Traversed Fields (" << TraversedFields.size() << "): [";
  for (size_t I = 0; I < TraversedFields.size(); ++I) {
    if (I > 0)
      Log << ", ";
    Log << TraversedFields[I];
  }
  Log << "]\n";

  // Dump all the `TraversedArray` which this `Traversal` describes
  Log << "  Traversed Arrays (" << TraversedArrays.size() << "):\n";
  for (const auto &Array : TraversedArrays) {
    Log << "    { NumElements: " << Array.NumElements
        << ", Stride: " << Array.Stride << " }\n";
  }

  Log << "\n";
  Log.flush();
}

mlir::Type deriveBaseType(mlir::Value BasePointer) {

  auto BasePtrType = unwrapped_cast<PointerType>(BasePointer.getType());
  auto PointeeType = BasePtrType.getPointeeType();

  // If the pointee is a struct, union, or array, use it directly — the
  // traversal analyzer can walk its fields and arrays.
  // Wrapping also a `struct`, would mean rewriting a constant offset access
  // into it as `p[0].field` instead of `p->field`.
  if (unwrapped_isa<StructType, UnionType, ArrayType>(PointeeType)) {
    return PointeeType;
  }

  // We don't want to wrap `void` or `function` `Type`s into `array`s.
  if (not unwrapped_isa<ObjectType>(PointeeType)) {
    return PointeeType;
  }

  // In all the other situations, wrap into an implicit array. We use
  // `ImplicitArrayNumElements` a _very large_ array in order to cover any
  // reasonable constant offset.
  static constexpr uint64_t
    ImplicitArrayNumElements = std::numeric_limits<uint64_t>::max();
  return ArrayType::get(PointeeType, ImplicitArrayNumElements);
}

namespace {

// =============================================================================
// Static helper functions
// =============================================================================

/// Helper function which counts the length of the common prefix between two
/// sorted `TraversedArrays` vectors. Since both are sorted in descending stride
/// order, this counts how many leading array shapes match exactly.
static uint64_t commonPrefixStrides(const llvm::ArrayRef<ArrayShape> &LHS,
                                    const llvm::ArrayRef<ArrayShape> &RHS) {
  uint64_t Count = 0;
  auto LIt = LHS.begin();
  auto RIt = RHS.begin();

  while (LIt != LHS.end() && RIt != RHS.end() && *LIt == *RIt) {
    ++Count;
    ++LIt;
    ++RIt;
  }

  return Count;
}

/// Checks whether the arrays traversed by `T` can represent every runtime
/// (variable-index) strided term of `Arithmetic`. `Replacement::make` lowers a
/// variable strided term into an array subscript when a traversed array's
/// stride divides the term's stride (the exact-stride match is the divisor
/// being the stride itself; a proper multiple yields a scaled index such as
/// `[2*i]` into a byte array), so a term is representable exactly when some
/// traversed array's stride divides it. A variable term whose stride is a
/// multiple of no traversed array's stride cannot be an integer array index
/// and, carrying a runtime index, would be silently dropped by the leftover
/// emission in `replaceFieldAccess`. A traversal that fails this check is not a
/// faithful lowering of the access, so `getBestTraversal` never selects it as
/// the best; a representable, lower-scored traversal is preferred, and only
/// when none exists is the raw pointer arithmetic kept (as for accesses that
/// fail `PointerArithmetic::verify`). Constant-only leftover terms are fine:
/// they are emitted as explicit integer arithmetic.
static bool canLowerArithmeticOntoTraversal(const PointerArithmetic &Arithmetic,
                                            const Traversal &T) {
  for (const auto &Term : Arithmetic.Offset.LinearCombination) {
    if (not Term.Idx.Variable)
      continue;

    bool Divisible = false;
    for (const ArrayShape &Array : T.TraversedArrays) {
      if (Term.Stride.urem(Array.Stride) == 0) {
        Divisible = true;
        break;
      }
    }

    if (not Divisible)
      return false;
  }

  return true;
}

/// The `typeDistance` function computes the distance between two `Type`s.
/// Returns 0 if the types are exactly equal (after stripping `typedef`s), 1 if
/// they only differ in CV-qualifiers, or infinity otherwise. The return type is
/// kept as `uint64_t` to allow future refinement of the scoring criterion,
/// enabling more fine grained control over this score.
static uint64_t typeDistance(mlir::Type Explicit, mlir::Type Ideal) {
  if (unwrapTypedefs(Explicit) == unwrapTypedefs(Ideal)) {
    return 0;
  }

  if (equivalentUnwrapped(Explicit, Ideal)) {
    return 1;
  }

  return std::numeric_limits<uint64_t>::max();
}

// =============================================================================
// `Score` class definition
// =============================================================================

/// We represent the `SizeRelation` between two `Traversal`s
enum class SizeRelation {
  Same,
  Larger,
  Smaller,
  DontCare
};

/// `Score` is used to represent the score obtained comparing two `Traversal`s.
/// It embeds the criteria we define in order to select the `BestTraversal` that
/// we want to use to rewrite the pointer access.
struct Score {
  bool Valid;
  int64_t StartDistance;
  SizeRelation SizeRelation;
  uint64_t TypeDistance;
  uint64_t CommonStrides;
  int64_t Depth;

  static Score invalid();

  std::strong_ordering operator<=>(const Score &Other);
};

Score Score::invalid() {
  return Score{ .Valid = false,
                .StartDistance = 0,
                .SizeRelation = SizeRelation::DontCare,
                .TypeDistance = 0,
                .CommonStrides = 0,
                .Depth = 0 };
}

/// We redefine the spaceship operator in order to define the ordering criteria
/// for comparing `Score`s, which drives the selection of the `BestTraversal`
std::strong_ordering Score::operator<=>(const Score &Other) {

  // An `Invalid` field must be considered `greater` than a `Valid` one
  if (not Valid and Other.Valid) {
    return std::strong_ordering::greater;
  }
  if (Valid and not Other.Valid) {
    return std::strong_ordering::less;
  }
  if (not Valid and not Other.Valid) {
    return std::strong_ordering::equal;
  }

  // When both `Score`s are valid, we move on to comparing:
  // 1) `StartDistance`
  auto Cmp = StartDistance <=> Other.StartDistance;
  if (Cmp != 0) {
    return Cmp;
  }

  // 2) `SizeRelation`
  Cmp = SizeRelation <=> Other.SizeRelation;
  if (Cmp != 0) {
    return Cmp;
  }

  // 3) `TypeDistance`
  Cmp = TypeDistance <=> Other.TypeDistance;
  if (Cmp != 0) {
    return Cmp;
  }

  // 4) `CommonStrides`
  Cmp = Other.CommonStrides <=> CommonStrides;
  if (Cmp != 0) {
    return Cmp;
  }

  // 5) `Depth`
  return Depth <=> Other.Depth;
}

/// The `score` function is used in order to obtain a _similarity_ `Score`
/// between the `Explicit` and `Ideal` `Traversal`s. We want to select the
/// `Traversal` with the minimal score as the one that will constitute the
/// pointer access rewrite.
///
/// `AccessArrays` are the array shapes the access actually walks (the shapes of
/// the `ArrayPath` the `Explicit` was built from). They are compared against
/// `Ideal.TraversedArrays` to compute the `CommonStrides` criterion. They are
/// passed separately, rather than read from `Explicit.TraversedArrays`, because
/// `toTraversal` builds the `Explicit` `Traversal` as a copy of the `Ideal`,
/// so its `TraversedArrays` would be identical to `Ideal.TraversedArrays` and
/// `CommonStrides` would degenerate to the `Ideal`'s array count.
static Score score(const Traversal &Explicit,
                   const Traversal &Ideal,
                   llvm::ArrayRef<ArrayShape> AccessArrays) {
  int64_t StartDistance = Explicit.begin() - Ideal.begin();
  int64_t EndDistance = Explicit.end() - Ideal.end();

  uint64_t CommonStrides = commonPrefixStrides(AccessArrays,
                                               Ideal.TraversedArrays);
  uint64_t TypeDistValue = typeDistance(Explicit.TargetType, Ideal.TargetType);

  if (StartDistance < 0) {

    // Explicit comes first - invalid
    return Score::invalid();
  } else if (StartDistance == 0) {

    // They start at same point
    if (EndDistance == 0) {

      // We have a perfect match, in this situation we will rely on the other
      // criteria to elect the `BestTraversal`
      return Score{ .Valid = true,
                    .StartDistance = 0,
                    .SizeRelation = SizeRelation::Same,
                    .TypeDistance = TypeDistValue,
                    .CommonStrides = CommonStrides,
                    .Depth = Ideal.depth() };
    } else if (EndDistance < 0) {

      // Explicit ends before Ideal
      return Score{ .Valid = true,
                    .StartDistance = 0,
                    .SizeRelation = SizeRelation::Larger,
                    .TypeDistance = 0,
                    .CommonStrides = 0,
                    .Depth = Ideal.depth() };
    } else if (EndDistance > 0) {

      // Explicit ends after Ideal
      return Score{ .Valid = true,
                    .StartDistance = 0,
                    .SizeRelation = SizeRelation::Smaller,
                    .TypeDistance = 0,
                    .CommonStrides = 0,
                    .Depth = Ideal.depth() };
    }
  } else if (StartDistance > 0) {

    // Ideal comes first (StartDistance > 0)
    if (EndDistance <= 0) {

      // Explicit ends before or at Ideal
      return Score{ .Valid = true,
                    .StartDistance = StartDistance,
                    .SizeRelation = SizeRelation::DontCare,
                    .TypeDistance = 0,
                    .CommonStrides = 0,
                    .Depth = Ideal.depth() };
    } else {

      // Explicit ends after Ideal - partial overlap, invalid
      return Score::invalid();
    }
  }

  // We do not expect that we can reach this point, all the previous case should
  // cover all the possibilities
  revng_abort();
}

// =============================================================================
// `TypeTraversalAnalyzer` class definition
// =============================================================================

/// `TypeTraversalAnalyzer` answers, for a `BaseType` and a byte offset into it,
/// which `Traversal`s and `ArrayPath`s can describe an access at that offset.
/// It walks only the fields that cover the offset, so its cost depends on how
/// deep the type is, not on how many fields it has: enumerating every
/// `Traversal` of a type instead grows with the size of its expanded field tree,
/// which for a large stack frame or a type with nested unions runs into the
/// thousands or millions. Answers are cached per type and offset for the whole
/// pass run.
class TypeTraversalAnalyzer {
private:
  TraversalCache &Cache;

public:
  TypeTraversalAnalyzer(TraversalCache &Cache) : Cache(Cache) {}

public:
  /// The `ArrayPath`s of the arrays that cover \p Offset, the empty one first,
  /// then in the order a walk of `BaseType` meets them, each sorted by
  /// descending `Stride`
  const std::vector<ArrayPath> &getArrayPaths(mlir::Type BaseType,
                                              int64_t Offset);

  /// The `Traversal`s that cover \p Offset, an offset whose constant array
  /// indices have been made explicit, so that it lies in the first element of
  /// the arrays it goes through. They are sorted by `StartOffset`, then by the
  /// size of their `TargetType`. A `Traversal` must start at the offset an
  /// access starts at, or contain it, to describe the access, so no other
  /// `Traversal` of `BaseType` can.
  const std::vector<Traversal> &getTraversals(mlir::Type BaseType,
                                              int64_t Offset);

private:
  RecursiveCoroutine<void>
  collectArrayPaths(mlir::Type Type,
                    int64_t Offset,
                    int64_t CurrentOffset,
                    const ArrayPath &CurrentArrayPath,
                    std::vector<ArrayPath> &ArrayPaths);

  RecursiveCoroutine<void>
  collectTraversals(mlir::Type Type,
                    int64_t Offset,
                    int64_t CurrentOffset,
                    const std::vector<uint64_t> &FieldPath,
                    const ArrayPath &CurrentArrayPath,
                    std::vector<Traversal> &Traversals);
};

/// The `NestedArrayShape` of an array of \p Type entered at \p CurrentOffset,
/// appended to \p CurrentArrayPath
static ArrayPath enterArray(clift::ArrayType Type,
                            int64_t CurrentOffset,
                            const ArrayPath &CurrentArrayPath) {
  NestedArrayShape ArrayInfo;
  ArrayInfo.Stride = getObjectSize(Type.getElementType());
  ArrayInfo.NumElements = Type.getElementsCount();

  // The offset is relative to the parent array, if any
  ArrayInfo.OffsetFromParentArrayElement = CurrentOffset;
  if (not CurrentArrayPath.empty())
    ArrayInfo.OffsetFromParentArrayElement -= CurrentArrayPath.back()
                                                .OffsetFromParentArrayElement;

  ArrayPath NewArrayPath = CurrentArrayPath;
  NewArrayPath.push_back(ArrayInfo);
  return NewArrayPath;
}

const std::vector<ArrayPath> &
TypeTraversalAnalyzer::getArrayPaths(mlir::Type BaseType, int64_t Offset) {
  auto [It, Inserted] = Cache.ArrayPaths.try_emplace({ BaseType, Offset });
  if (not Inserted)
    return It->second;

  std::vector<ArrayPath> ArrayPaths;
  ArrayPaths.push_back(ArrayPath());
  rc_eval(collectArrayPaths(BaseType, Offset, 0, {}, ArrayPaths));

  for (ArrayPath &Path : ArrayPaths) {
    std::sort(Path.begin(),
              Path.end(),
              [](const NestedArrayShape &A, const NestedArrayShape &B) {
                return A.Stride > B.Stride;
              });
  }

  // `collectArrayPaths` may have grown the cache: look the entry up again
  return Cache.ArrayPaths[{ BaseType, Offset }] = std::move(ArrayPaths);
}

const std::vector<Traversal> &
TypeTraversalAnalyzer::getTraversals(mlir::Type BaseType, int64_t Offset) {
  auto [It, Inserted] = Cache.Traversals.try_emplace({ BaseType, Offset });
  if (not Inserted)
    return It->second;

  std::vector<Traversal> Traversals;
  rc_eval(collectTraversals(BaseType, Offset, 0, {}, {}, Traversals));

  std::stable_sort(Traversals.begin(),
                   Traversals.end(),
                   [](const Traversal &A, const Traversal &B) {
                     if (A.StartOffset != B.StartOffset)
                       return A.StartOffset < B.StartOffset;
                     return getObjectSizeOrZero(A.TargetType)
                            < getObjectSizeOrZero(B.TargetType);
                   });

  return Cache.Traversals[{ BaseType, Offset }] = std::move(Traversals);
}

RecursiveCoroutine<void>
TypeTraversalAnalyzer::collectArrayPaths(mlir::Type Type,
                                         int64_t Offset,
                                         int64_t CurrentOffset,
                                         const ArrayPath &CurrentArrayPath,
                                         std::vector<ArrayPath> &ArrayPaths) {
  if (auto Typedef = mlir::dyn_cast<clift::TypedefType>(Type)) {
    rc_recur collectArrayPaths(Typedef.getUnderlyingType(),
                               Offset,
                               CurrentOffset,
                               CurrentArrayPath,
                               ArrayPaths);
  } else if (auto Enum = mlir::dyn_cast<clift::EnumType>(Type)) {
    rc_recur collectArrayPaths(Enum.getUnderlyingType(),
                               Offset,
                               CurrentOffset,
                               CurrentArrayPath,
                               ArrayPaths);
  } else if (auto Array = mlir::dyn_cast<clift::ArrayType>(Type)) {
    // An array covers the offset if the offset lands in one of its elements
    uint64_t Stride = getObjectSize(Array.getElementType());
    uint64_t Relative = Offset - CurrentOffset;
    if (Relative / Stride >= Array.getElementsCount())
      rc_return;

    ArrayPath NewArrayPath = enterArray(Array, CurrentOffset, CurrentArrayPath);
    ArrayPaths.push_back(NewArrayPath);

    // Go on into the element the offset lands in, as if it were the first
    rc_recur collectArrayPaths(Array.getElementType(),
                               CurrentOffset + Relative % Stride,
                               CurrentOffset,
                               NewArrayPath,
                               ArrayPaths);
  } else if (auto Class = mlir::dyn_cast<clift::ClassType>(Type)) {
    for (clift::FieldAttr Field : Class.getFields()) {
      int64_t FieldOffset = CurrentOffset + Field.getOffset();
      uint64_t FieldSize = getObjectSize(Field.getType());
      if (FieldOffset <= Offset
          and static_cast<uint64_t>(Offset - FieldOffset) < FieldSize)
        rc_recur collectArrayPaths(Field.getType(),
                                   Offset,
                                   FieldOffset,
                                   CurrentArrayPath,
                                   ArrayPaths);
    }
  }
}

RecursiveCoroutine<void>
TypeTraversalAnalyzer::collectTraversals(mlir::Type Type,
                                         int64_t Offset,
                                         int64_t CurrentOffset,
                                         const std::vector<uint64_t> &FieldPath,
                                         const ArrayPath &CurrentArrayPath,
                                         std::vector<Traversal> &Traversals) {
  // We should never reach a type with zero size - if we do, it means there is
  // something severely wrong in the types we're working with
  revng_assert(getObjectSizeOrZero(Type) > 0);

  // Pointers are leaves: we do not traverse through them, but a `Traversal`
  // can land on a field whose type is a pointer
  if (mlir::isa<clift::PrimitiveType, clift::PointerType>(Type)) {
    Traversals.emplace_back(Type, CurrentOffset, 0, FieldPath, CurrentArrayPath);
  } else if (auto Typedef = mlir::dyn_cast<clift::TypedefType>(Type)) {
    Traversals.emplace_back(Type, CurrentOffset, 0, FieldPath, CurrentArrayPath);
    rc_recur collectTraversals(Typedef.getUnderlyingType(),
                               Offset,
                               CurrentOffset,
                               FieldPath,
                               CurrentArrayPath,
                               Traversals);
  } else if (auto Enum = mlir::dyn_cast<clift::EnumType>(Type)) {
    Traversals.emplace_back(Type, CurrentOffset, 0, FieldPath, CurrentArrayPath);
    rc_recur collectTraversals(Enum.getUnderlyingType(),
                               Offset,
                               CurrentOffset,
                               FieldPath,
                               CurrentArrayPath,
                               Traversals);
  } else if (auto Array = mlir::dyn_cast<clift::ArrayType>(Type)) {
    Traversals.emplace_back(Type, CurrentOffset, 0, FieldPath, CurrentArrayPath);

    // Only the first element is walked: the access's constant array indices
    // have already been made explicit. An element ending at the offset is still
    // walked, as an access of no size there can land on its last field.
    uint64_t Stride = getObjectSize(Array.getElementType());
    if (static_cast<uint64_t>(Offset - CurrentOffset) <= Stride)
      rc_recur collectTraversals(Array.getElementType(),
                                 Offset,
                                 CurrentOffset,
                                 FieldPath,
                                 enterArray(Array,
                                            CurrentOffset,
                                            CurrentArrayPath),
                                 Traversals);
  } else if (auto Class = mlir::dyn_cast<clift::ClassType>(Type)) {
    Traversals.emplace_back(Type, CurrentOffset, 0, FieldPath, CurrentArrayPath);

    // For `union`s, `Field.getOffset()` always returns 0 by verification
    llvm::ArrayRef<clift::FieldAttr> Fields = Class.getFields();
    for (size_t I = 0; I < Fields.size(); ++I) {
      int64_t FieldOffset = CurrentOffset + Fields[I].getOffset();
      uint64_t FieldSize = getObjectSize(Fields[I].getType());
      if (FieldOffset > Offset
          or static_cast<uint64_t>(Offset - FieldOffset) > FieldSize)
        continue;

      std::vector<uint64_t> NewFieldPath = FieldPath;
      NewFieldPath.push_back(static_cast<uint64_t>(I));
      rc_recur collectTraversals(Fields[I].getType(),
                                 Offset,
                                 FieldOffset,
                                 NewFieldPath,
                                 CurrentArrayPath,
                                 Traversals);
    }
  }
}

// =============================================================================
// `ExplicitPointerArithmetic` struct definition
// =============================================================================

/// An `ExplicitPointerArithmetic` pairs an explicit `PointerArithmetic` (a
/// `PointerArithmetic` re-expressed along a concrete `ArrayPath`, so that every
/// array index is evident in its `LinearCombination`) with the array shapes of
/// that `ArrayPath`, sorted in the same descending-stride order as
/// `Traversal::TraversedArrays`.
///
/// `AccessArrays` records the array structure the access *actually* walks. It
/// is what the `CommonStrides` scoring criterion compares against each
/// candidate `Ideal` traversal's arrays: an access stepping through a stride-2
/// array should prefer an `Ideal` that goes through a stride-2 array. This is
/// kept separate from the `Traversal` produced by `toTraversal`, whose
/// `TraversedArrays` are a copy of the `Ideal`'s (used, unchanged, to drive the
/// emission and the runtime-index representability checks).
struct ExplicitPointerArithmetic {
  PointerArithmetic Arithmetic;
  llvm::SmallVector<ArrayShape> AccessArrays;
};

/// A list of `ExplicitPointerArithmetic`s, one per compatible `ArrayPath`
using ExplicitPointerArithmetics = std::vector<ExplicitPointerArithmetic>;

// =============================================================================
// `BestTraversalChooser` class definition
// =============================================================================

/// `BestTraversalChooser` is used as a compute class for the `BestTraversal`
class BestTraversalChooser {
private:
  /// The `TypeTraversalAnalyzer` is our `Traversal` and `ArrayPath` oracle
  TypeTraversalAnalyzer TraversalAnalyzer;

  /// Bit width of the pointer type being rewritten
  unsigned PointerBitWidth = 0;

public:
  /// We need an explicit constructor in order to propagate the
  /// `TraversalCache` which is used as a global cache for storing
  /// `Traversal`s and `ArrayPath`s
  BestTraversalChooser(TraversalCache &Cache) : TraversalAnalyzer(Cache) {}

public:
  /// Public entry point for computing the `BestTraversal` for the
  /// `PointerToReplace` and the pre-computed `Arithmetic`
  std::optional<Traversal>
  computeBestTraversal(ExpressionOpInterface PointerToReplace,
                       const PointerArithmetic &Arithmetic);

private:
  /// Obtain the explicit rewrite of the constant folded portion of
  /// `Arithmetic`, following an array traversal described by `ArrayPath`, so
  /// that it is evident in the `LinearCombination` component of `Arithmetic`
  std::optional<PointerArithmetic>
  getExplicitArithmetic(const PointerArithmetic &Arithmetic,
                        const ArrayPath &ArrayPath);

  /// Obtain all the explicit rewritings of the input `Arithmetic` following all
  /// the `ArrayPath`s for the `BaseType`. Each rewriting is paired with the
  /// array shapes of the `ArrayPath` it followed (see
  /// `ExplicitPointerArithmetic`). We explicitly specify the `BaseType` to
  /// handle the _pointer as array_ situation.
  ExplicitPointerArithmetics
  toExplicitArrayAccesses(const PointerArithmetic &Arithmetic,
                          mlir::Type BaseType);

  /// Helper which trivially spill a `PointerArithmetic` into a `Traversal`
  Traversal toTraversal(const PointerArithmetic &PA,
                        mlir::Type PointeeType,
                        const Traversal &Ideal);

  /// Obtain the best `Traversal`
  std::optional<Traversal>
  getBestTraversal(mlir::Type BaseType,
                   mlir::Type PointeeType,
                   const PointerArithmetic &Arithmetic,
                   const ExplicitPointerArithmetics &ExplicitAccesses);
};

std::optional<Traversal>
BestTraversalChooser::computeBestTraversal(ExpressionOpInterface
                                             PointerToReplace,
                                           const PointerArithmetic
                                             &Arithmetic) {
  // We only perform the substitution for `PointerType`
  auto PointerToReplaceType = PointerToReplace.getType();
  if (not clift::unwrapped_isa<clift::PointerType>(PointerToReplaceType)) {
    return std::nullopt;
  }

  // Derive the `BaseType` for the traversal analysis, potentially wrapping the
  // `BasePointerType` into an implicit array (`p[i]` rewrite).
  auto BaseType = deriveBaseType(Arithmetic.BasePointer);

  // It may be that the `PointerToReplace` points to a `void 0` type, in that
  // case we cannot provide a `Traversal` for sure
  if (getObjectSizeOrZero(BaseType) == 0) {
    return std::nullopt;
  }

  // Expand to explicit array accesses the input `PointerArithmetic`, so that
  // the constant folded component performed by the compiler is evident in the
  // `LinearCombination` portion of `Arithmetic`
  PointerBitWidth = clift::unwrapped_cast<PointerType>(PointerToReplaceType)
                      .getPointerSize()
                    * 8;

  ExplicitPointerArithmetics
    ExplicitAccesses = toExplicitArrayAccesses(Arithmetic, BaseType);

  mlir::Type
    PointeeType = clift::unwrapped_cast<PointerType>(PointerToReplaceType)
                    .getPointeeType();

  // Obtain the `BestTraversal` for connecting `BaseType` to `PointeeType`,
  // following one of the possible `ExplicitArithmetic`s. Traversals that cannot
  // represent the arithmetic's runtime indices are excluded during selection
  // (see `getBestTraversal`), so if none remains we leave the raw pointer
  // arithmetic in place.
  auto BestTraversal = getBestTraversal(BaseType,
                                        PointeeType,
                                        Arithmetic,
                                        ExplicitAccesses);

  // In case we end up with a `BestTraversal` which does not actually traverse
  // any `struct` field or `array` element, we avoid the rewriting altogether,
  // and we leave the explicit pointer arithmetic access in `clift`
  if (not BestTraversal or BestTraversal->depth() == 0) {
    return std::nullopt;
  }

  return BestTraversal;
}

// Turn the input `Arithmetic` into another `PointerArithmetic` , following the
// array traversal dictaded by the `ArrayPath` `AP`. On success, the output
// `PointerArithmetic` has a `StartOffset` that is lower than or equal than the
// input one, and all the array traversals of `AP` have an equivalent
// `StridedTerm` in the `LinearCombination` of `OffsetExpression` of the
// `Result`.
// For example this can turn P + 12 (with no `LinearCombination`) into
// P + 8 * 1 + 4 (with a single `StridedTerm`, with fixed `Index`) if `AP` is
// e.g. `{.OffsetFromParentArrayElement = 0, .EndOffset = 48, .Stride = 8}`)
std::optional<PointerArithmetic>
BestTraversalChooser::getExplicitArithmetic(const PointerArithmetic &Arithmetic,
                                            const ArrayPath &AP) {

  PointerArithmetic Result{
    .BasePointer = Arithmetic.BasePointer,
    .Offset = PointerArithmetic::OffsetExpression(PointerBitWidth)
  };

  // We do not modify the input `Arithmetic`, but we work on a local copy
  PointerArithmetic WorkingArithmetic = Arithmetic;

  // The variable strided terms are runtime indices that making the array
  // indexing explicit does not change: a term's stride is a multiple of the
  // element stride of the array that indexes it, which already encodes its
  // coefficient. We therefore keep them in the `Result` unchanged and only
  // split the constant part of the offset into an explicit constant index per
  // array level. `WorkingArithmetic` is consumed purely to check that every
  // variable term is representable (divisible by some array in the path).
  auto &LC = Result.Offset.LinearCombination;
  LC = Arithmetic.Offset.LinearCombination;

  for (const NestedArrayShape &NAI : AP) {
    const auto &[OffsetFromParentArrayElement, NumElements, Stride] = NAI;

    // Consume the offset from the parent array element
    Result.Offset.BaseOffset += OffsetFromParentArrayElement;

    revng_assert(WorkingArithmetic.Offset.BaseOffset
                   .uge(OffsetFromParentArrayElement));
    WorkingArithmetic.Offset.BaseOffset -= OffsetFromParentArrayElement;

    // Split the constant offset that reaches into this array into an explicit
    // constant array index. We fold it into the term that already indexes this
    // array, if any (i.e. a variable term of the same stride), or add a new
    // constant-index term.
    if (WorkingArithmetic.Offset.BaseOffset.uge(Stride)) {
      llvm::APInt FixedIndex = WorkingArithmetic.Offset.BaseOffset.udiv(Stride);
      revng_assert(FixedIndex.ult(NumElements));
      WorkingArithmetic.Offset.BaseOffset = WorkingArithmetic.Offset.BaseOffset
                                              .urem(Stride);

      auto SameStride = std::find_if(LC.begin(),
                                     LC.end(),
                                     [&](const PointerArithmetic::StridedTerm
                                           &Term) {
                                       return Term.Stride == Stride;
                                     });
      if (SameStride != LC.end())
        SameStride->Idx.Constant += FixedIndex;
      else
        LC.emplace_back(llvm::APInt(PointerBitWidth, Stride),
                        PointerArithmetic::Index{ mlir::Value(), FixedIndex });
    }

    // Verify this path can represent every runtime index: remove from the
    // working copy every term this array's stride divides (an exact match, or a
    // proper multiple such as a two-byte-stride index into a byte array). A
    // term this array does not divide is left for a later, smaller-stride
    // array; if no array consumes it, the check after the loop discards this
    // path.
    auto &WorkingLC = WorkingArithmetic.Offset.LinearCombination;
    for (auto It = WorkingLC.begin(); It != WorkingLC.end();) {
      if (It->Stride.urem(Stride) == 0)
        It = WorkingLC.erase(It);
      else
        ++It;
    }
  }

  // If some runtime index is a multiple of no array's stride, this path cannot
  // represent it, so we bail out
  if (not WorkingArithmetic.Offset.LinearCombination.empty()) {
    return std::nullopt;
  }

  // If we still have some non-consumed portion of the input `BaseOffset`, it
  // lands inside the target element, so we propagate it in the `Result`
  if (WorkingArithmetic.Offset.BaseOffset.getBoolValue()) {
    Result.Offset.BaseOffset += WorkingArithmetic.Offset.BaseOffset;
  }

  // Restore descending-stride order, which the construction above does not
  // guarantee: the constant array-index terms are appended after the copied
  // (already-ordered) runtime terms, so a large-stride outer array index can
  // land after a smaller-stride runtime term (e.g. `arr[c][i]`, offset
  // `8*c + 4*i`, produces `[{4, i}, {8, c}]`). No current consumer depends on
  // the order (the only reader, the `canLowerArithmeticOntoTraversal` assertion
  // in `getBestTraversal`, is order-independent), but we keep the result a
  // well-formed `PointerArithmetic`, matching the descending, unique-stride
  // invariant that `computePointerArithmetic` and `verify` maintain, so the
  // object stays consistent and safe for any future consumer.
  std::sort(LC.begin(),
            LC.end(),
            [](const PointerArithmetic::StridedTerm &A,
               const PointerArithmetic::StridedTerm &B) {
              return A.Stride.ugt(B.Stride);
            });

  return Result;
}

ExplicitPointerArithmetics
BestTraversalChooser::toExplicitArrayAccesses(const PointerArithmetic
                                                &Arithmetic,
                                              mlir::Type BaseType) {
  ExplicitPointerArithmetics Result;

  // The `ArrayPath`s of `BaseType` whose arrays the access lands in
  int64_t Offset = Arithmetic.Offset.BaseOffset.getZExtValue();
  const std::vector<ArrayPath> &ArrayPaths = TraversalAnalyzer
                                               .getArrayPaths(BaseType, Offset);

  for (const ArrayPath &Path : ArrayPaths) {
    // Try and turn `Arithmetic` into a form where all array indexes, even the
    // constant ones, are explicit. In case of success, we enqueue the
    // `Explicit`, together with the array shapes it walked, in the final
    // `Result`s. The array shapes carry the access's real array structure,
    // which the `CommonStrides` criterion compares against each candidate
    // `Ideal`.
    std::optional<PointerArithmetic>
      Explicit = getExplicitArithmetic(Arithmetic, Path);
    if (Explicit) {
      Result
        .push_back({ .Arithmetic = std::move(*Explicit),
                     .AccessArrays = arrayPathToSortedVector(Path) });
    }
  }

  return Result;
}

Traversal BestTraversalChooser::toTraversal(const PointerArithmetic &PA,
                                            mlir::Type PointeeType,
                                            const Traversal &Ideal) {

  // Ensure that we never end up with a negative `LeftoverOffset`, which would
  // not make any sense
  revng_assert(Ideal.StartOffset >= 0);
  revng_assert(PA.Offset.BaseOffset.uge(Ideal.StartOffset));

  // We want to turn an explicit `PointerArithmetic` `PA` in a `Traversal`,
  // assuming that it does the traversal described in `Ideal`.
  // Because `PA` is explicit (i.e. all array traversals at fixed index have
  // been expanded in the `LinearCombination`), the `Result` `Traversal` will
  // always be the same, except that we have to adjust the leftover offset
  // and fix the `TargetType` to the actual type required by the traversal.
  //
  // Note the `Result`'s `TraversedArrays` deliberately stay those of the
  // `Ideal`: they describe the concrete type structure the emission walks (and
  // whose real strides the runtime-index representability check relies on), not
  // the array structure the access walks. The latter is carried separately in
  // `ExplicitPointerArithmetic::AccessArrays` and only used to score
  // `CommonStrides`.
  auto BaseOffset = PA.Offset.BaseOffset.getZExtValue();
  Traversal Result = Ideal;
  Result.TargetType = PointeeType;
  Result.LeftoverOffset = BaseOffset - Ideal.StartOffset;
  return Result;
}

// To find the `BestTraversal`, we have to compare all the valid `Traversal`s
// from `BaseType` with all the ones that we can build from the
// `ExplicitArithmetic`s we get from `clift`.
std::optional<Traversal>
BestTraversalChooser::getBestTraversal(mlir::Type BaseType,
                                       mlir::Type PointeeType,
                                       const PointerArithmetic &Arithmetic,
                                       const ExplicitPointerArithmetics
                                         &ExplicitAccesses) {

  std::optional<Traversal> BestTraversal = std::nullopt;
  Score BestScore = Score::invalid();

  for (const ExplicitPointerArithmetic &ExplicitAccess : ExplicitAccesses) {
    const PointerArithmetic &Explicit = ExplicitAccess.Arithmetic;

    // Only the `Traversal`s that cover the offset the access starts at can
    // describe it
    int64_t Offset = Explicit.Offset.BaseOffset.getZExtValue();
    for (const Traversal &Ideal :
         TraversalAnalyzer.getTraversals(BaseType, Offset)) {

      // Skip `Ideals` whose `StartOffset` exceeds the `Explicit` `BaseOffset`,
      // the `Explicit` cannot reach that far, and `toTraversal` would
      // underflow.
      auto BaseOffset = Explicit.Offset.BaseOffset.getZExtValue();
      if (Explicit.Offset.BaseOffset.ult(Ideal.StartOffset))
        continue;

      // Convert each `ExplicitArithemtic` into a `Traversal`, so we can compare
      // it with `Ideal`. `ExplicitTraversal` is the `Traversal` that we
      // would obtain traversing the `BaseType` with `Explicit` if we did
      // traverse it as the `Ideal` suggests. Basically what can be
      // different is just the `LeftOverOffset`.
      Traversal ExplicitTraversal = toTraversal(Explicit, PointeeType, Ideal);

      Score CurrentScore = score(ExplicitTraversal,
                                 Ideal,
                                 ExplicitAccess.AccessArrays);

      if (!CurrentScore.Valid)
        continue;

      // A traversal whose array strides cannot represent the arithmetic's
      // runtime indices is not a valid lowering (see
      // `canLowerArithmeticOntoTraversal`), so it must never be considered
      // best: skip it here and let a representable, lower-scored traversal win.
      if (not canLowerArithmeticOntoTraversal(Arithmetic, ExplicitTraversal))
        continue;

      // The explicit arithmetic must be lowerable onto the very same arrays the
      // original arithmetic just was: `getExplicitArithmetic` re-expresses the
      // access along an `ArrayPath`, and its `LinearCombination` must keep
      // every runtime term at a stride the traversed arrays can represent. This
      // is the one live consumer of that `LinearCombination`, and it guards
      // `getExplicitArithmetic` against re-expressing a runtime index at a
      // stride no array divides.
      revng_assert(canLowerArithmeticOntoTraversal(Explicit,
                                                   ExplicitTraversal));

      // We select the `Score` which best suits the criteria defining in the
      // _scoring_ mechanism
      if (CurrentScore < BestScore) {
        BestScore = CurrentScore;
        BestTraversal = ExplicitTraversal;
      }
    }
  }

  // We serialize on the `Log` the selected `BestTraversal`
  if (Log.isEnabled() and BestTraversal) {
    Log << "Elected  BestTraversal:\n";
    BestTraversal->dump();
  }

  // If no `Traversal` was valid, we'll return a `nullopt` here, meaning that we
  // will not replace the `PointerToReplace` with a field access. This basically
  // means that all the possible `Traversal`s that the clift expression could
  // represent are so ugly that we bail out.
  return BestTraversal;
}

} // namespace

std::optional<Traversal>
computeBestTraversal(ExpressionOpInterface PointerToReplace,
                     const PointerArithmetic &Arithmetic,
                     TraversalCache &Cache) {
  auto BestTraversalC = BestTraversalChooser(Cache);
  return BestTraversalC.computeBestTraversal(PointerToReplace, Arithmetic);
}
