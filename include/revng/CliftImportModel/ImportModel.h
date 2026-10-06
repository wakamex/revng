#pragma once

//
// This file is distributed under the MIT License. See LICENSE.md for details.
//

#include <optional>

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/StringRef.h"
#include "mlir/IR/BuiltinOps.h"
#include "mlir/IR/MLIRContext.h"
#include "mlir/Support/LogicalResult.h"
#include "mlir/IR/Types.h"

#include "revng/ADT/MutableSet.h"
#include "revng/Clift/Clift.h"
#include "revng/Model/Binary.h"

namespace clift {

/// Convert the specified unqualified model type to a Clift type in the
/// specified context.
///
/// \return The corresponding Clift type, or null on failure.
mlir::Type importType(llvm::function_ref<mlir::InFlightDiagnostic()> EmitError,
                      mlir::MLIRContext *Context,
                      const model::TypeDefinition &ModelType);
mlir::Type importType(mlir::MLIRContext *Context,
                      const model::TypeDefinition &ModelType);

/// Convert the specified qualified model type to a Clift type in the specified
/// context.
///
/// \return The corresponding Clift type, or null on failure.
mlir::Type importType(llvm::function_ref<mlir::InFlightDiagnostic()> EmitError,
                      mlir::MLIRContext *Context,
                      const model::Type &ModelType);
mlir::Type importType(mlir::MLIRContext *Context, const model::Type &ModelType);

/// Convert the specified model function into a clift function declaration.
///
/// \return The corresponding Clift FunctionOp, or null on failure.
FunctionOp
importFunctionDeclaration(mlir::ModuleOp Module,
                          mlir::Location DebugLocation,
                          llvm::StringRef Name,
                          llvm::StringRef Handle,
                          clift::FunctionType Prototype,
                          const MutableSet<model::FunctionAttribute::Values>
                            &Attributes);

/// Convert the specified model segment into a clift variable.
///
/// \return The corresponding Clift GlobalVariableOp, or null on failure.
GlobalVariableOp importSegmentDeclaration(mlir::ModuleOp Module,
                                          mlir::Location DebugLocation,
                                          llvm::StringRef Name,
                                          llvm::StringRef Handle,
                                          mlir::Type Type);

void importAllModelTypes(const model::Binary &Model, mlir::ModuleOp Module);
void importAllModelFunctionDeclarations(const model::Binary &Model,
                                        mlir::ModuleOp Module);
void importAllModelSegmentDeclarations(const model::Binary &Model,
                                       mlir::ModuleOp Module);

void importDescriptiveInfo(const model::Binary &Model, mlir::ModuleOp Module);

// NOTE: this overload can be dropped together with the old pipeline.
/// Name, from \p Model, every type \p Modules reach. All of \p Modules share
/// one context, and the names live in that context's type definitions.
void importModelTypeNames(llvm::ArrayRef<mlir::ModuleOp> Modules,
                          const model::Binary &Model);

void importDescriptiveInfo(const model::Function &Function,
                           const model::Binary &Model,
                           mlir::ModuleOp Module);

/// Build a fresh Clift module holding the declarations needed to parse a piece
/// of decompiled C against the model: all model types plus descriptive info
/// (names and comments) and, when `IncludeGlobals` is set, the function and
/// segment declarations that a function body refers to.
///
/// The returned context owns the module and must outlive it.
std::pair<std::unique_ptr<mlir::MLIRContext>, mlir::OwningOpRef<mlir::ModuleOp>>
makeHeaderModule(const model::Binary &Model, bool IncludeGlobals);

clift::StructType makeOpaqueStruct(mlir::MLIRContext *Context,
                                   uint64_t NumBytes);

/// Whether \p Handle names a class type the model defines: a type definition,
/// or the register set a raw function returns. Modules can omit these
/// definitions from their bytecode (see `OmittedClassDefinitions`), because
/// `importIncompleteModelTypes` restores them from the model.
bool isModelTypeHandle(llvm::StringRef Handle);

/// Define, from \p Model, every class type that \p Modules reach and their
/// context has not defined yet. All of \p Modules share one context. Fails if
/// such a type is not a model type or the model no longer has it.
mlir::LogicalResult
importIncompleteModelTypes(llvm::ArrayRef<mlir::ModuleOp> Modules,
                           const model::Binary &Model);

} // namespace clift
