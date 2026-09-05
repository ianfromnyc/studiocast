#include "core/maxine/sdk_runtime.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <dlfcn.h>
#include <elf.h>
#include <fstream>
#include <limits>
#include <map>
#include <mutex>
#include <set>
#include <sstream>
#include <vector>

namespace studiocast::maxine {

namespace {
namespace fs = std::filesystem;

bool DirExists(const fs::path &p) {
  std::error_code ec;
  return !p.empty() && fs::exists(p, ec) && fs::is_directory(p, ec);
}

bool FileExists(const fs::path &p) {
  std::error_code ec;
  return !p.empty() && fs::exists(p, ec) && !fs::is_directory(p, ec);
}

// Reads the whole file into memory. The ELF header and the program headers
// always sit near the start, but the dynamic segment they point at, and the
// dynamic string table it names, can sit anywhere in the file. A capped read
// could stop before either one, so the reader takes all of it. SDK libraries
// are large, so this buffer is large as well.
bool ReadFile(const fs::path &p, std::vector<char> *out) {
  std::ifstream in(p, std::ios::binary);
  if (!in)
    return false;
  in.seekg(0, std::ios::end);
  const std::streamoff size = in.tellg();
  if (size <= 0)
    return false;
  // tellg gives a std::streamoff, but read takes a std::streamsize and resize
  // takes a size_t. Either can be narrower, so refuse a file that does not fit
  // in both instead of converting past their range.
  const auto want = static_cast<std::uintmax_t>(size);
  if (want > static_cast<std::uintmax_t>(
                 std::numeric_limits<std::streamsize>::max()) ||
      want > static_cast<std::uintmax_t>(
                 std::numeric_limits<std::size_t>::max())) {
    return false;
  }

  in.seekg(0, std::ios::beg);
  out->resize(static_cast<std::size_t>(size));
  in.read(out->data(), static_cast<std::streamsize>(size));
  return in.good() || in.eof();
}

// Every offset and count below comes out of the file, so none of it can be
// trusted to fit. These two report an overflow instead of wrapping.
bool AddChecked(std::uint64_t a, std::uint64_t b, std::uint64_t *out) {
  if (b > std::numeric_limits<std::uint64_t>::max() - a)
    return false;
  *out = a + b;
  return true;
}

bool MulChecked(std::uint64_t a, std::uint64_t b, std::uint64_t *out) {
  if (a != 0 && b > std::numeric_limits<std::uint64_t>::max() / a)
    return false;
  *out = a * b;
  return true;
}

template <typename T>
bool ReadAt(const std::vector<char> &buf, std::uint64_t offset, T *out) {
  // Both tests subtract; a sum of the two would wrap for a large offset and
  // let the copy below read outside the buffer.
  if (offset > buf.size() || sizeof(T) > buf.size() - offset)
    return false;
  std::memcpy(out, buf.data() + static_cast<size_t>(offset), sizeof(T));
  return true;
}

// Turns a virtual address from the dynamic section into a file offset by
// walking the PT_LOAD segments. Section headers may be stripped; program
// headers never are.
bool VaddrToOffset(const std::vector<Elf64_Phdr> &phdrs, Elf64_Addr vaddr,
                   std::uint64_t *out) {
  for (const auto &ph : phdrs) {
    if (ph.p_type != PT_LOAD)
      continue;
    // `p_vaddr + p_filesz` can wrap. The subtraction below cannot, because the
    // first test proves that `vaddr` is not below `p_vaddr`.
    if (vaddr < ph.p_vaddr || vaddr - ph.p_vaddr >= ph.p_filesz)
      continue;
    std::uint64_t off = 0;
    if (!AddChecked(vaddr - ph.p_vaddr, ph.p_offset, &off))
      continue;
    *out = off;
    return true;
  }
  return false;
}

// Collects the DT_NEEDED entries of an ELF64 shared object.
std::vector<std::string> ReadNeededSonames(const fs::path &library) {
  std::vector<std::string> needed;

  std::vector<char> buf;
  if (!ReadFile(library, &buf))
    return needed;

  Elf64_Ehdr eh{};
  if (!ReadAt(buf, 0, &eh))
    return needed;
  if (std::memcmp(eh.e_ident, ELFMAG, SELFMAG) != 0 ||
      eh.e_ident[EI_CLASS] != ELFCLASS64 ||
      // This reader takes every field as little-endian, which is what every
      // platform StudioCast runs on uses. A big-endian file would give
      // byte-swapped offsets, so refuse it instead of reading nonsense.
      eh.e_ident[EI_DATA] != ELFDATA2LSB || eh.e_phoff == 0 ||
      eh.e_phentsize != sizeof(Elf64_Phdr) || eh.e_phnum == 0 ||
      // PN_XNUM says the real program header count sits in a section header,
      // which a stripped file may not have. No SDK library needs it.
      eh.e_phnum == PN_XNUM) {
    return needed;
  }

  // Prove the whole program header table is in the file before any of it is
  // read, so a wrapped offset cannot look like a short but valid table.
  std::uint64_t ph_bytes = 0;
  std::uint64_t ph_end = 0;
  if (!MulChecked(eh.e_phnum, sizeof(Elf64_Phdr), &ph_bytes) ||
      !AddChecked(eh.e_phoff, ph_bytes, &ph_end) || ph_end > buf.size()) {
    return needed;
  }

  std::vector<Elf64_Phdr> phdrs;
  phdrs.reserve(eh.e_phnum);
  for (unsigned i = 0; i < eh.e_phnum; ++i) {
    Elf64_Phdr ph{};
    // The table fits, so this sum stays inside it and cannot wrap.
    if (!ReadAt(buf, eh.e_phoff + i * sizeof(Elf64_Phdr), &ph))
      return needed;
    phdrs.push_back(ph);
  }

  const Elf64_Phdr *dyn_ph = nullptr;
  for (const auto &ph : phdrs) {
    if (ph.p_type == PT_DYNAMIC) {
      dyn_ph = &ph;
      break;
    }
  }
  if (!dyn_ph)
    return needed;

  // The dynamic segment must be in the file as well.
  std::uint64_t dyn_end = 0;
  if (!AddChecked(dyn_ph->p_offset, dyn_ph->p_filesz, &dyn_end) ||
      dyn_end > buf.size()) {
    return needed;
  }

  // Pass 1: find the dynamic string table.
  Elf64_Addr strtab_vaddr = 0;
  std::vector<Elf64_Xword> needed_offsets;
  const std::uint64_t dyn_count = dyn_ph->p_filesz / sizeof(Elf64_Dyn);
  for (std::uint64_t i = 0; i < dyn_count; ++i) {
    Elf64_Dyn d{};
    // The segment fits, so this sum stays inside it and cannot wrap.
    if (!ReadAt(buf, dyn_ph->p_offset + i * sizeof(Elf64_Dyn), &d))
      return needed;
    if (d.d_tag == DT_NULL)
      break;
    if (d.d_tag == DT_STRTAB)
      strtab_vaddr = d.d_un.d_ptr;
    else if (d.d_tag == DT_NEEDED)
      needed_offsets.push_back(d.d_un.d_val);
  }
  if (strtab_vaddr == 0 || needed_offsets.empty())
    return needed;

  std::uint64_t strtab_off = 0;
  if (!VaddrToOffset(phdrs, strtab_vaddr, &strtab_off))
    return needed;

  for (const auto n : needed_offsets) {
    std::uint64_t at = 0;
    if (!AddChecked(strtab_off, n, &at) || at >= buf.size())
      continue;
    const char *s = buf.data() + static_cast<size_t>(at);
    const size_t max = buf.size() - static_cast<size_t>(at);
    const size_t len = ::strnlen(s, max);
    if (len == 0 || len == max)
      continue;
    needed.emplace_back(s, len);
  }
  return needed;
}

// Libraries the system loader must always own. Loading a second copy of these
// would either fail or split the process runtime in two.
bool IsSystemOwned(const std::string &soname) {
  static const std::set<std::string> kSystem = {
      "libc.so.6",       "libm.so.6",         "libdl.so.2",
      "libpthread.so.0", "librt.so.1",        "libstdc++.so.6",
      "libgcc_s.so.1",   "libatomic.so.1",    "libgomp.so.1",
      "libresolv.so.2",  "libnsl.so.1",       "libutil.so.1",
      "libcrypt.so.2",
      // The NVIDIA driver library belongs to the installed driver.
      "libcuda.so.1",    "libnvidia-ml.so.1",
  };
  if (kSystem.count(soname) != 0)
    return true;
  return soname.rfind("ld-linux", 0) == 0;
}

// The SDK directories that may hold a bundled runtime, in priority order.
std::vector<fs::path> BuildSearchDirs(const fs::path &library,
                                      const fs::path &sdk_root) {
  std::vector<fs::path> dirs;
  auto add = [&](const fs::path &d) {
    if (d.empty() || !DirExists(d))
      return;
    for (const auto &e : dirs) {
      if (e == d)
        return;
    }
    dirs.push_back(d);
  };

  add(library.parent_path());
  if (!sdk_root.empty()) {
    add(sdk_root / "lib");
    add(sdk_root / "external" / "cuda" / "lib");
    add(sdk_root / "external" / "tensorrt" / "lib");
    add(sdk_root / "nvafx" / "lib");
  }
  return dirs;
}

// Walks up from the library to the first ancestor that looks like an SDK root.
fs::path InferSdkRoot(const fs::path &library) {
  fs::path dir = library.parent_path();
  for (int i = 0; i < 4 && !dir.empty() && dir != dir.root_path(); ++i) {
    if (DirExists(dir / "external"))
      return dir;
    dir = dir.parent_path();
  }
  // Fall back to the parent of the directory that holds the library, which is
  // the legacy `<root>/lib/<library>` shape.
  return library.parent_path().parent_path();
}

fs::path ResolveSoname(const std::vector<fs::path> &dirs,
                       const std::string &soname) {
  for (const auto &d : dirs) {
    const auto p = d / soname;
    if (FileExists(p))
      return p;
  }
  return {};
}

// True when this process already has the soname open. Loading a second copy
// from the SDK would be wrong.
bool AlreadyLoaded(const std::string &soname) {
  void *h = dlopen(soname.c_str(), RTLD_NOW | RTLD_LOCAL | RTLD_NOLOAD);
  if (!h)
    return false;
  dlclose(h); // NOLOAD does not add a reference we need to keep.
  return true;
}

// Depth-first pre-load: a library's own dependencies go in before it does.
void PreloadRecursive(const fs::path &library,
                      const std::vector<fs::path> &dirs,
                      std::set<std::string> *visited, SdkRuntimeReport *out) {
  for (const auto &soname : ReadNeededSonames(library)) {
    if (!visited->insert(soname).second)
      continue;
    if (IsSystemOwned(soname))
      continue;

    SdkDependencyLoad entry;
    entry.soname = soname;

    if (AlreadyLoaded(soname)) {
      out->dependencies.push_back(std::move(entry));
      continue;
    }

    entry.resolved = ResolveSoname(dirs, soname);
    if (entry.resolved.empty()) {
      // Not shipped by the SDK: leave it to the system loader.
      out->dependencies.push_back(std::move(entry));
      continue;
    }

    PreloadRecursive(entry.resolved, dirs, visited, out);

    ::dlerror();
    void *h = dlopen(entry.resolved.c_str(), RTLD_NOW | RTLD_GLOBAL);
    if (h) {
      entry.loaded = true;
    } else {
      const char *e = ::dlerror();
      entry.error = e ? e : "dlopen failed";
      out->problems.push_back("Could not pre-load " + soname + " from " +
                              entry.resolved.string() + ": " + entry.error);
    }
    out->dependencies.push_back(std::move(entry));
  }
}

SdkRuntimeReport Compute(const fs::path &library, fs::path sdk_root) {
  SdkRuntimeReport rep;
  rep.library = library;

  if (!FileExists(library)) {
    rep.problems.push_back("SDK library not found: " + library.string());
    return rep;
  }

  if (sdk_root.empty())
    sdk_root = InferSdkRoot(library);
  rep.sdk_root = sdk_root;
  rep.search_dirs = BuildSearchDirs(library, sdk_root);

  std::set<std::string> visited;
  PreloadRecursive(library, rep.search_dirs, &visited, &rep);
  return rep;
}

std::mutex &CacheMutex() {
  static std::mutex m;
  return m;
}

std::map<std::string, SdkRuntimeReport> &Cache() {
  static std::map<std::string, SdkRuntimeReport> c;
  return c;
}

} // namespace

std::string SdkRuntimeReport::Summary() const {
  size_t loaded = 0;
  for (const auto &d : dependencies) {
    if (d.loaded)
      ++loaded;
  }

  std::ostringstream oss;
  oss << "pre-loaded " << loaded << " bundled "
      << (loaded == 1 ? "library" : "libraries");
  if (!sdk_root.empty()) {
    oss << " from " << sdk_root.string();
  }
  if (!problems.empty()) {
    oss << "; " << problems.size()
        << (problems.size() == 1 ? " problem" : " problems");
  }
  return oss.str();
}

const SdkRuntimeReport &PreloadSdkRuntime(const fs::path &library,
                                          const fs::path &sdk_root) {
  // The report depends on the SDK root as well as on the library, so both go
  // into the key. A NUL keeps the two parts apart, because no path holds one.
  const std::string key = library.string() + '\0' + sdk_root.string();

  {
    const std::lock_guard<std::mutex> lock(CacheMutex());
    auto &cache = Cache();
    const auto it = cache.find(key);
    if (it != cache.end())
      return it->second;
  }

  // Compute outside the lock. Compute() reads files and calls dlopen, so it
  // can take a long time, and the lock would stop every other caller, even one
  // that asks for another key.
  SdkRuntimeReport report = Compute(library, sdk_root);

  // The first writer wins. Another thread can have written the same key while
  // this one computed; its report is the result of the same work, and the
  // handles this thread opened stay valid, because dlopen counts references
  // and this code keeps every handle for the life of the process. The map
  // never drops an entry, so the reference stays good.
  const std::lock_guard<std::mutex> lock(CacheMutex());
  return Cache().emplace(key, std::move(report)).first->second;
}

void PreloadSdkRuntimeIfLocal(const fs::path &library) {
  if (!library.has_parent_path() || !FileExists(library))
    return;
  (void)PreloadSdkRuntime(library);
}

} // namespace studiocast::maxine
