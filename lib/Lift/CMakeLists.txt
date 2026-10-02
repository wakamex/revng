#
# This file is distributed under the MIT License. See LICENSE.md for details.
#

revng_add_library_internal(
  revngLift
  SHARED
  CodeGenerator.cpp
  CSVOffsets.cpp
  ExternalJumpsHandler.cpp
  InstructionTranslator.cpp
  IRAnnotators.cpp
  LibTcg.cpp
  Lift.cpp
  LinkSupportPipe.cpp
  JumpTargetManager.cpp
  NoReturnAnalysis.cpp
  PostLiftVerifyPass.cpp
  RootAnalyzer.cpp
  SelfReferencingDbgAnnotationWriter.cpp
  UnwindFunctionRanges.cpp
  ValueMaterializerPass.cpp
  VariableManager.cpp)

set(DISASSEMBLER_ARCHITECTURE_SUPPORT "")
foreach(CURRENT_ARCHITECTURE AArch64 ARM Mips SystemZ X86)
  set(DISASSEMBLER_ARCHITECTURE_SUPPORT
      ${DISASSEMBLER_ARCHITECTURE_SUPPORT}
      "${CURRENT_ARCHITECTURE}Disassembler" "${CURRENT_ARCHITECTURE}Info"
      "${CURRENT_ARCHITECTURE}Desc")
endforeach(CURRENT_ARCHITECTURE)

llvm_map_components_to_libnames(
  LIFT_UNWIND_LLVM_LIBRARIES
  DebugInfoDWARF
  MC
  MCDisassembler
  Object
  ${DISASSEMBLER_ARCHITECTURE_SUPPORT})

target_link_libraries(
  revngLift
  PUBLIC dl
         m
         revngABI
         revngBasicAnalyses
         revngModelImporterDebugInfo
         revngFunctionCallIdentification
         revngModel
         revngSupport
         revngTypeShrinking
         revngEarlyFunctionAnalysis
         revngValueMaterializer
         ${LLVM_LIBRARIES}
         ${LIFT_UNWIND_LLVM_LIBRARIES})
