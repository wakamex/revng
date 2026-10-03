//
// This file is distributed under the MIT License. See LICENSE.md for details.
//

#include <cstdint>

extern "C" {
#include "dlfcn.h"
}

#include "revng/Lift/LibTcg.h"
#include "revng/Support/ResourceFinder.h"

static std::string
findLibTcgPath(const model::Architecture::Values Architecture) {
  llvm::StringRef ArchName = model::Architecture::getQEMUName(Architecture);
  const std::string LibTcgName = "/lib/libtcg-" + ArchName.str() + ".so";
  auto OptionalLibTcg = revng::ResourceFinder.findFile(LibTcgName);
  revng_assert(OptionalLibTcg.has_value(), "Cannot find libtinycode");
  return OptionalLibTcg.value();
}

LibTcg LibTcg::get(model::Architecture::Values Architecture) {
  LibTcg Result;

  // Look for the library in the system's paths
  std::string LibraryPath = findLibTcgPath(Architecture);
  Result.LibraryHandle = dlopen(LibraryPath.c_str(), RTLD_LAZY);
  revng_assert(Result.LibraryHandle != nullptr);

  // Obtain the address of the libtcg_load entry point
  using LibTcgLoadFunc = LIBTCG_FUNC_TYPE(libtcg_load);
  void *LibTcgLoadSym = dlsym(Result.LibraryHandle, "libtcg_load");
  auto LibTcgLoad = reinterpret_cast<LibTcgLoadFunc *>(LibTcgLoadSym);
  revng_assert(LibTcgLoad != nullptr);

  // Load the libtcg interface containing relevant function pointers
  Result.Interface = LibTcgLoad();

  Result.Context = Result.Interface.context_create();
  revng_assert(Result.Context != nullptr, "Failed to create libtcg context");

  Result.ArchInfo = Result.Interface.get_arch_info();

  void *PageGetFlagsSym = dlsym(Result.LibraryHandle, "page_get_flags");
  Result.PageGetFlags = reinterpret_cast<PageGetFlagsFunc *>(PageGetFlagsSym);
  revng_assert(Result.PageGetFlags != nullptr);
  Result.MaxGuestAddress = model::Architecture::getPointerSize(Architecture)
                               == 4 ?
                             UINT32_MAX :
                             UINT64_MAX;

  std::set<llvm::StringRef> Names;
  for (int I = 0; I < Result.ArchInfo.num_globals; ++I) {
    if (Result.ArchInfo.globals[I].name == nullptr)
      continue;
    auto Offset = Result.ArchInfo.globals[I].offset
                  + Result.ArchInfo.env_offset;
    llvm::StringRef Name(Result.ArchInfo.globals[I].name);

    revng_assert(not Names.contains(Name));
    revng_assert(not Result.GlobalNames.contains(Offset));
    Result.GlobalNames[Offset] = Name;
  }

  return Result;
}

bool LibTcg::isMappedGuestAddress(uint64_t Address) const {
  // PAGE_VALID in QEMU's include/exec/cpu-all.h
  constexpr int QEMUPageValid = 0x0008;

  if (Address == 0 or Address > MaxGuestAddress)
    return false;

  return (PageGetFlags(Address) & QEMUPageValid) != 0;
}

LibTcg::~LibTcg() {
  Interface.context_destroy(Context);
  dlclose(LibraryHandle);
}
