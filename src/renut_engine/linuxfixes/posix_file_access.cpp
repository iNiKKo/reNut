// Fixes corrupted save data on Linux, caused by a broken POSIX open() mode.
//
// Symptom
// -------
// The game reports its save data as corrupted, repeatedly.
//
// Cause
// -----
// The SDK's POSIX backend (src/core/filesystem_posix.cpp,
// FileHandle::OpenExisting) builds its open() flags like this:
//
//     int open_access = 0;
//     if (desired_access & FileAccess::kGenericRead)  open_access |= O_RDONLY;
//     if (desired_access & FileAccess::kGenericWrite) open_access |= O_WRONLY;
//     if (desired_access & FileAccess::kGenericAll)   open_access |= O_RDWR;
//     ...
//     int handle = open(path.c_str(), open_access);
//
// O_RDONLY/O_WRONLY/O_RDWR are not bit flags. They are mutually exclusive
// values occupying the low two bits (O_ACCMODE):
//
//     O_RDONLY = 0    O_WRONLY = 1    O_RDWR = 2    O_ACCMODE = 3
//
// So OR-ing them silently produces the wrong mode:
//
//     kGenericRead | kGenericWrite  ->  0 | 1  =  1  =  O_WRONLY   (read lost!)
//     kGenericWrite | kGenericAll   ->  1 | 2  =  3  =  O_ACCMODE  (invalid)
//
// A save file is opened for read *and* write, which lands on the first case:
// the descriptor comes back write-only. Every subsequent pread() then fails
// with EBADF, HostPathFile::ReadSync turns that into X_STATUS_END_OF_FILE, and
// the guest sees a zero-length / unreadable save -- i.e. "corrupted". The
// second case is worse: open() rejects mode 3 with EINVAL, so the file cannot
// be opened at all.
//
// This only affects Linux; the Win32 backend passes real GENERIC_* flags to
// CreateFile, which do combine.
//
// Fix
// ---
// The SDK is upstream and cannot be patched, so this file provides its own
// definition of FileHandle::OpenExisting that collapses the requested access
// down to exactly one O_ACCMODE value before OR-ing in the modifier flags.
//
// Unlike the XAudio hooks, every caller of this function lives *inside*
// librexruntimerd.so, so a plain link-time definition here would not be seen by
// them. It works because the symbol is exported with default visibility and the
// library is not linked -Bsymbolic, so its calls go through the PLT and resolve
// against the global scope -- where the executable is searched first. reNut's
// CMakeLists passes -Wl,--export-dynamic-symbol=<mangled name> to put this
// definition into renut's dynamic symbol table so that lookup can find it.
//
// Also fixed here: on a failed read/write the SDK assigns the raw -1 from
// pread/pwrite into *out_bytes_*, which underflows to SIZE_MAX for callers that
// look at the count before the status. This version reports 0.

#include <fcntl.h>
#include <unistd.h>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>

#include <rex/filesystem.h>

namespace {

class LinuxFileHandle final : public rex::filesystem::FileHandle {
 public:
  LinuxFileHandle(const std::filesystem::path& path, int handle)
      : FileHandle(path), handle_(handle) {}

  ~LinuxFileHandle() override {
    if (handle_ != -1) {
      close(handle_);
      handle_ = -1;
    }
  }

  bool Read(size_t file_offset, void* buffer, size_t buffer_length,
            size_t* out_bytes_read) override {
    ssize_t out = pread(handle_, buffer, buffer_length, static_cast<off_t>(file_offset));
    if (out < 0) {
      *out_bytes_read = 0;
      return false;
    }
    *out_bytes_read = static_cast<size_t>(out);
    return true;
  }

  bool Write(size_t file_offset, const void* buffer, size_t buffer_length,
             size_t* out_bytes_written) override {
    ssize_t out = pwrite(handle_, buffer, buffer_length, static_cast<off_t>(file_offset));
    if (out < 0) {
      *out_bytes_written = 0;
      return false;
    }
    *out_bytes_written = static_cast<size_t>(out);
    return true;
  }

  bool SetLength(size_t length) override {
    return ftruncate(handle_, static_cast<off_t>(length)) >= 0;
  }

  void Flush() override { fsync(handle_); }

 private:
  int handle_ = -1;
};

}  // namespace

namespace rex::filesystem {

// Interposes the SDK's definition. Marked used so it survives even though
// nothing inside renut calls it directly.
__attribute__((used)) std::unique_ptr<FileHandle> FileHandle::OpenExisting(
    const std::filesystem::path& path, uint32_t desired_access, bool /*allow_share_delete*/) {
  // POSIX has no share-delete analog; unlinking an open file is always allowed.
  const bool wants_read = (desired_access & (FileAccess::kGenericRead | FileAccess::kGenericExecute |
                                             FileAccess::kGenericAll | FileAccess::kFileReadData)) != 0;
  const bool wants_write =
      (desired_access & (FileAccess::kGenericWrite | FileAccess::kGenericAll |
                         FileAccess::kFileWriteData | FileAccess::kFileAppendData)) != 0;

  // Exactly one access mode, never a bitwise mix.
  int open_flags;
  if (wants_read && wants_write) {
    open_flags = O_RDWR;
  } else if (wants_write) {
    open_flags = O_WRONLY;
  } else {
    open_flags = O_RDONLY;
  }

  // Modifier flags are real bits and may be OR-ed in.
  if (desired_access & FileAccess::kFileAppendData) {
    open_flags |= O_APPEND;
  }

  int handle = open(path.c_str(), open_flags);
  if (handle == -1) {
    return nullptr;
  }
  return std::make_unique<LinuxFileHandle>(path, handle);
}

}  // namespace rex::filesystem
