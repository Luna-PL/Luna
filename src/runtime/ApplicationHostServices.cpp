#include "ApplicationHostServices.h"

#include <algorithm>
#include <cerrno>
#include <climits>
#include <cstdio>
#include <cstring>
#include <limits>
#include <mutex>
#include <string>
#include <unordered_map>

#ifdef _WIN32
#include <direct.h>
#include <fcntl.h>
#include <io.h>
#include <sys/stat.h>
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace {

class FileRegistry {
public:
    LunaFileHandleV1 insert(int descriptor) {
        std::lock_guard<std::mutex> lock(mMutex);
        LunaFileHandleV1 candidate = mNextHandle++;
        while (candidate == LUNA_INVALID_FILE_HANDLE_V1 ||
               mDescriptors.count(candidate) != 0)
            candidate = mNextHandle++;
        mDescriptors.emplace(candidate, descriptor);
        return candidate;
    }

    template <typename Function>
    int withDescriptor(LunaFileHandleV1 handle, Function&& function) {
        std::lock_guard<std::mutex> lock(mMutex);
        const auto found = mDescriptors.find(handle);
        if (found == mDescriptors.end()) return EBADF;
        return function(found->second);
    }

    bool take(LunaFileHandleV1 handle, int& descriptor) {
        std::lock_guard<std::mutex> lock(mMutex);
        const auto found = mDescriptors.find(handle);
        if (found == mDescriptors.end()) return false;
        descriptor = found->second;
        mDescriptors.erase(found);
        return true;
    }

private:
    std::mutex mMutex;
    std::unordered_map<LunaFileHandleV1, int> mDescriptors;
    LunaFileHandleV1 mNextHandle = 1;
};

FileRegistry files;

uint32_t errorKind(int code) {
    switch (code) {
        case ENOENT: return LUNA_IO_ERROR_NOT_FOUND;
        case EACCES:
#ifdef EPERM
        case EPERM:
#endif
            return LUNA_IO_ERROR_PERMISSION_DENIED;
        case EEXIST: return LUNA_IO_ERROR_ALREADY_EXISTS;
        case EINVAL:
#ifdef ENAMETOOLONG
        case ENAMETOOLONG:
#endif
            return LUNA_IO_ERROR_INVALID_INPUT;
        case EINTR: return LUNA_IO_ERROR_INTERRUPTED;
#ifdef EAGAIN
        case EAGAIN: return LUNA_IO_ERROR_WOULD_BLOCK;
#endif
#if defined(EWOULDBLOCK) && EWOULDBLOCK != EAGAIN
        case EWOULDBLOCK: return LUNA_IO_ERROR_WOULD_BLOCK;
#endif
#ifdef ENOSYS
        case ENOSYS: return LUNA_IO_ERROR_UNSUPPORTED;
#endif
#ifdef ENOTSUP
        case ENOTSUP: return LUNA_IO_ERROR_UNSUPPORTED;
#endif
#if defined(EOPNOTSUPP) && EOPNOTSUPP != ENOTSUP
        case EOPNOTSUPP: return LUNA_IO_ERROR_UNSUPPORTED;
#endif
        default: return LUNA_IO_ERROR_OTHER;
    }
}

int ioFailure(LunaIoErrorV1* error, uint32_t operation, int code) {
    if (error) {
        *error = {LUNA_RUNTIME_ABI_V1, sizeof(LunaIoErrorV1),
                  errorKind(code), operation, code};
    }
    return LUNA_RUNTIME_STATUS_IO_ERROR;
}

bool validUtf8(const char* bytes, size_t size) {
    if (!bytes) return size == 0;
    size_t index = 0;
    while (index < size) {
        const unsigned char first = static_cast<unsigned char>(bytes[index++]);
        if (first <= 0x7f) continue;
        uint32_t scalar = 0;
        size_t remaining = 0;
        if (first >= 0xc2 && first <= 0xdf) {
            scalar = first & 0x1f;
            remaining = 1;
        } else if (first >= 0xe0 && first <= 0xef) {
            scalar = first & 0x0f;
            remaining = 2;
        } else if (first >= 0xf0 && first <= 0xf4) {
            scalar = first & 0x07;
            remaining = 3;
        } else {
            return false;
        }
        if (remaining > size - index) return false;
        for (size_t offset = 0; offset < remaining; ++offset) {
            const unsigned char next =
                static_cast<unsigned char>(bytes[index++]);
            if ((next & 0xc0) != 0x80) return false;
            scalar = (scalar << 6) | (next & 0x3f);
        }
        if ((remaining == 2 && scalar < 0x800) ||
            (remaining == 3 && scalar < 0x10000) ||
            (scalar >= 0xd800 && scalar <= 0xdfff) || scalar > 0x10ffff)
            return false;
    }
    return true;
}

bool validPath(const char* path, size_t pathSize) {
    return validUtf8(path, pathSize) &&
        (!path || std::memchr(path, '\0', pathSize) == nullptr);
}

#ifdef _WIN32
bool nativePath(const char* path, size_t pathSize, std::wstring& result) {
    if (!validPath(path, pathSize) || pathSize > static_cast<size_t>(INT_MAX)) {
        errno = EINVAL;
        return false;
    }
    if (pathSize == 0) {
        result.clear();
        return true;
    }
    const int sourceSize = static_cast<int>(pathSize);
    const int required = MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS, path, sourceSize, nullptr, 0);
    if (required <= 0) {
        errno = GetLastError() == ERROR_NOT_ENOUGH_MEMORY ? ENOMEM : EINVAL;
        return false;
    }
    try {
        result.resize(static_cast<size_t>(required));
    } catch (...) {
        errno = ENOMEM;
        return false;
    }
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path,
                            sourceSize, result.data(), required) != required) {
        errno = GetLastError() == ERROR_NOT_ENOUGH_MEMORY ? ENOMEM : EINVAL;
        return false;
    }
    return true;
}

int nativeOpen(const std::wstring& path, int flags, int mode) {
    return _wopen(path.c_str(), flags, mode);
}

int nativeRead(int descriptor, void* bytes, size_t capacity) {
    const auto bounded = static_cast<unsigned int>(std::min<size_t>(
        capacity, std::numeric_limits<unsigned int>::max()));
    return _read(descriptor, bytes, bounded);
}

int nativeWrite(int descriptor, const void* bytes, size_t count) {
    const auto bounded = static_cast<unsigned int>(std::min<size_t>(
        count, std::numeric_limits<unsigned int>::max()));
    return _write(descriptor, bytes, bounded);
}

int64_t nativeSeek(int descriptor, int64_t offset, int whence) {
    return _lseeki64(descriptor, offset, whence);
}

int nativeSync(int descriptor) { return _commit(descriptor); }
int nativeClose(int descriptor) { return _close(descriptor); }
#else
bool nativePath(const char* path, size_t pathSize, std::string& result) {
    if (!validPath(path, pathSize)) {
        errno = EINVAL;
        return false;
    }
    try {
        result.assign(path ? path : "", pathSize);
    } catch (...) {
        errno = ENOMEM;
        return false;
    }
    return true;
}

int nativeOpen(const std::string& path, int flags, int mode) {
    return ::open(path.c_str(), flags, static_cast<mode_t>(mode));
}

ssize_t nativeRead(int descriptor, void* bytes, size_t capacity) {
    const size_t bounded = std::min<size_t>(
        capacity, static_cast<size_t>(std::numeric_limits<ssize_t>::max()));
    return ::read(descriptor, bytes, bounded);
}

ssize_t nativeWrite(int descriptor, const void* bytes, size_t count) {
    const size_t bounded = std::min<size_t>(
        count, static_cast<size_t>(std::numeric_limits<ssize_t>::max()));
    return ::write(descriptor, bytes, bounded);
}

off_t nativeSeek(int descriptor, int64_t offset, int whence) {
    if (offset < std::numeric_limits<off_t>::min() ||
        offset > std::numeric_limits<off_t>::max()) {
        errno = EINVAL;
        return static_cast<off_t>(-1);
    }
    return ::lseek(descriptor, static_cast<off_t>(offset), whence);
}

int nativeSync(int descriptor) { return ::fsync(descriptor); }
int nativeClose(int descriptor) { return ::close(descriptor); }
#endif

int consoleWrite(void*, uint32_t stream, const char* bytes, size_t byteCount) {
    if ((!bytes && byteCount != 0) ||
        (stream != LUNA_CONSOLE_STDOUT && stream != LUNA_CONSOLE_STDERR))
        return LUNA_RUNTIME_STATUS_INVALID_ARGUMENT;
    if (byteCount == 0) return LUNA_RUNTIME_STATUS_OK;
    FILE* output = stream == LUNA_CONSOLE_STDERR ? stderr : stdout;
    return std::fwrite(bytes, 1, byteCount, output) == byteCount
        ? LUNA_RUNTIME_STATUS_OK : LUNA_RUNTIME_STATUS_IO_ERROR;
}

int consoleFlush(void*, uint32_t stream) {
    if (stream != LUNA_CONSOLE_STDOUT && stream != LUNA_CONSOLE_STDERR)
        return LUNA_RUNTIME_STATUS_INVALID_ARGUMENT;
    return std::fflush(stream == LUNA_CONSOLE_STDERR ? stderr : stdout) == 0
        ? LUNA_RUNTIME_STATUS_OK : LUNA_RUNTIME_STATUS_IO_ERROR;
}

int consoleRead(void*, char* bytes, size_t capacity, size_t* bytesRead,
                LunaIoErrorV1* error) {
    if (!bytesRead || !error || (!bytes && capacity != 0))
        return LUNA_RUNTIME_STATUS_INVALID_ARGUMENT;
    *bytesRead = 0;
    if (capacity == 0) return LUNA_RUNTIME_STATUS_OK;
    const size_t count = std::fread(bytes, 1, capacity, stdin);
    *bytesRead = count;
    if (count != 0 || std::feof(stdin)) return LUNA_RUNTIME_STATUS_OK;
    const int code = errno == 0 ? EIO : errno;
    std::clearerr(stdin);
    return ioFailure(error, LUNA_IO_OPERATION_READ, code);
}

int openFile(void*, const char* path, size_t pathSize, uint32_t flags,
             LunaFileHandleV1* handle, LunaIoErrorV1* error) {
    if (!handle || !error) return LUNA_RUNTIME_STATUS_INVALID_ARGUMENT;
    *handle = LUNA_INVALID_FILE_HANDLE_V1;
    const uint32_t knownFlags = LUNA_FILE_OPEN_READ | LUNA_FILE_OPEN_WRITE |
        LUNA_FILE_OPEN_APPEND | LUNA_FILE_OPEN_TRUNCATE |
        LUNA_FILE_OPEN_CREATE | LUNA_FILE_OPEN_CREATE_NEW;
    if ((flags & ~knownFlags) != 0 ||
        (flags & (LUNA_FILE_OPEN_READ | LUNA_FILE_OPEN_WRITE)) == 0 ||
        ((flags & (LUNA_FILE_OPEN_APPEND | LUNA_FILE_OPEN_TRUNCATE |
                   LUNA_FILE_OPEN_CREATE | LUNA_FILE_OPEN_CREATE_NEW)) != 0 &&
         (flags & LUNA_FILE_OPEN_WRITE) == 0))
        return LUNA_RUNTIME_STATUS_INVALID_ARGUMENT;

#ifdef _WIN32
    std::wstring converted;
#else
    std::string converted;
#endif
    if (!nativePath(path, pathSize, converted))
        return ioFailure(error, LUNA_IO_OPERATION_OPEN,
                         errno == 0 ? EINVAL : errno);

    int nativeFlags = 0;
    if ((flags & LUNA_FILE_OPEN_READ) && (flags & LUNA_FILE_OPEN_WRITE))
        nativeFlags |=
#ifdef _WIN32
            _O_RDWR;
#else
            O_RDWR;
#endif
    else if (flags & LUNA_FILE_OPEN_WRITE)
        nativeFlags |=
#ifdef _WIN32
            _O_WRONLY;
#else
            O_WRONLY;
#endif
    else
        nativeFlags |=
#ifdef _WIN32
            _O_RDONLY;
#else
            O_RDONLY;
#endif

#ifdef _WIN32
    nativeFlags |= _O_BINARY | _O_NOINHERIT;
    if (flags & LUNA_FILE_OPEN_APPEND) nativeFlags |= _O_APPEND;
    if (flags & LUNA_FILE_OPEN_TRUNCATE) nativeFlags |= _O_TRUNC;
    if (flags & (LUNA_FILE_OPEN_CREATE | LUNA_FILE_OPEN_CREATE_NEW))
        nativeFlags |= _O_CREAT;
    if (flags & LUNA_FILE_OPEN_CREATE_NEW) nativeFlags |= _O_EXCL;
    constexpr int createMode = _S_IREAD | _S_IWRITE;
#else
#ifdef O_CLOEXEC
    nativeFlags |= O_CLOEXEC;
#endif
    if (flags & LUNA_FILE_OPEN_APPEND) nativeFlags |= O_APPEND;
    if (flags & LUNA_FILE_OPEN_TRUNCATE) nativeFlags |= O_TRUNC;
    if (flags & (LUNA_FILE_OPEN_CREATE | LUNA_FILE_OPEN_CREATE_NEW))
        nativeFlags |= O_CREAT;
    if (flags & LUNA_FILE_OPEN_CREATE_NEW) nativeFlags |= O_EXCL;
    constexpr int createMode = 0666;
#endif
    const int descriptor = nativeOpen(converted, nativeFlags, createMode);
    if (descriptor < 0)
        return ioFailure(error, LUNA_IO_OPERATION_OPEN, errno);
    try {
        *handle = files.insert(descriptor);
    } catch (...) {
        nativeClose(descriptor);
        return ioFailure(error, LUNA_IO_OPERATION_OPEN, ENOMEM);
    }
    return LUNA_RUNTIME_STATUS_OK;
}

int readFile(void*, LunaFileHandleV1 handle, void* bytes, size_t capacity,
             size_t* bytesRead, LunaIoErrorV1* error) {
    if (!bytesRead || !error || (!bytes && capacity != 0))
        return LUNA_RUNTIME_STATUS_INVALID_ARGUMENT;
    *bytesRead = 0;
    const int lookup = files.withDescriptor(handle, [&](int descriptor) {
        if (capacity == 0) return 0;
        const auto count = nativeRead(descriptor, bytes, capacity);
        if (count < 0) return errno;
        *bytesRead = static_cast<size_t>(count);
        return 0;
    });
    return lookup == 0 ? LUNA_RUNTIME_STATUS_OK
                       : ioFailure(error, LUNA_IO_OPERATION_READ, lookup);
}

int writeFile(void*, LunaFileHandleV1 handle, const void* bytes, size_t count,
              size_t* bytesWritten, LunaIoErrorV1* error) {
    if (!bytesWritten || !error || (!bytes && count != 0))
        return LUNA_RUNTIME_STATUS_INVALID_ARGUMENT;
    *bytesWritten = 0;
    const int result = files.withDescriptor(handle, [&](int descriptor) {
        if (count == 0) return 0;
        const auto written = nativeWrite(descriptor, bytes, count);
        if (written < 0) return errno;
        *bytesWritten = static_cast<size_t>(written);
        return 0;
    });
    return result == 0 ? LUNA_RUNTIME_STATUS_OK
                       : ioFailure(error, LUNA_IO_OPERATION_WRITE, result);
}

int seekFile(void*, LunaFileHandleV1 handle, int64_t offset, uint32_t whence,
             uint64_t* position, LunaIoErrorV1* error) {
    if (!position || !error || whence > LUNA_SEEK_FROM_END)
        return LUNA_RUNTIME_STATUS_INVALID_ARGUMENT;
    const int nativeWhence = whence == LUNA_SEEK_FROM_START ? SEEK_SET :
        whence == LUNA_SEEK_FROM_CURRENT ? SEEK_CUR : SEEK_END;
    const int result = files.withDescriptor(handle, [&](int descriptor) {
        const auto value = nativeSeek(descriptor, offset, nativeWhence);
        if (value < 0) return errno;
        *position = static_cast<uint64_t>(value);
        return 0;
    });
    return result == 0 ? LUNA_RUNTIME_STATUS_OK
                       : ioFailure(error, LUNA_IO_OPERATION_SEEK, result);
}

int flushFile(void*, LunaFileHandleV1 handle, LunaIoErrorV1* error) {
    if (!error) return LUNA_RUNTIME_STATUS_INVALID_ARGUMENT;
    const int result = files.withDescriptor(handle, [](int) { return 0; });
    return result == 0 ? LUNA_RUNTIME_STATUS_OK
                       : ioFailure(error, LUNA_IO_OPERATION_FLUSH, result);
}

int syncFile(void*, LunaFileHandleV1 handle, LunaIoErrorV1* error) {
    if (!error) return LUNA_RUNTIME_STATUS_INVALID_ARGUMENT;
    const int result = files.withDescriptor(handle, [](int descriptor) {
        return nativeSync(descriptor) == 0 ? 0 : errno;
    });
    return result == 0 ? LUNA_RUNTIME_STATUS_OK
                       : ioFailure(error, LUNA_IO_OPERATION_SYNC, result);
}

int closeFile(void*, LunaFileHandleV1 handle, LunaIoErrorV1* error) {
    if (!error) return LUNA_RUNTIME_STATUS_INVALID_ARGUMENT;
    int descriptor = -1;
    if (!files.take(handle, descriptor))
        return ioFailure(error, LUNA_IO_OPERATION_CLOSE, EBADF);
    if (nativeClose(descriptor) != 0)
        return ioFailure(error, LUNA_IO_OPERATION_CLOSE, errno);
    return LUNA_RUNTIME_STATUS_OK;
}

uint32_t metadataType(uint64_t mode) {
#ifdef _WIN32
    if ((mode & _S_IFMT) == _S_IFREG) return LUNA_FILE_TYPE_REGULAR;
    if ((mode & _S_IFMT) == _S_IFDIR) return LUNA_FILE_TYPE_DIRECTORY;
#else
    if (S_ISREG(mode)) return LUNA_FILE_TYPE_REGULAR;
    if (S_ISDIR(mode)) return LUNA_FILE_TYPE_DIRECTORY;
    if (S_ISLNK(mode)) return LUNA_FILE_TYPE_SYMLINK;
#endif
    return LUNA_FILE_TYPE_OTHER;
}

#ifdef _WIN32
using NativeStat = struct _stat64;
int descriptorStat(int descriptor, NativeStat* value) {
    return _fstat64(descriptor, value);
}
int pathStat(const std::wstring& path, NativeStat* value) {
    return _wstat64(path.c_str(), value);
}
#else
using NativeStat = struct stat;
int descriptorStat(int descriptor, NativeStat* value) {
    return ::fstat(descriptor, value);
}
int pathStat(const std::string& path, NativeStat* value) {
    return ::lstat(path.c_str(), value);
}
#endif

void writeMetadata(const NativeStat& source, LunaFileMetadataV1* target) {
    *target = {LUNA_RUNTIME_ABI_V1, sizeof(LunaFileMetadataV1),
               metadataType(static_cast<uint64_t>(source.st_mode)), 0,
               source.st_size < 0 ? 0 : static_cast<uint64_t>(source.st_size)};
}

int handleMetadata(void*, LunaFileHandleV1 handle, LunaFileMetadataV1* metadata,
                   LunaIoErrorV1* error) {
    if (!metadata || !error) return LUNA_RUNTIME_STATUS_INVALID_ARGUMENT;
    const int result = files.withDescriptor(handle, [&](int descriptor) {
        NativeStat value{};
        if (descriptorStat(descriptor, &value) != 0) return errno;
        writeMetadata(value, metadata);
        return 0;
    });
    return result == 0 ? LUNA_RUNTIME_STATUS_OK
                       : ioFailure(error, LUNA_IO_OPERATION_METADATA, result);
}

int pathMetadata(void*, const char* path, size_t pathSize,
                 LunaFileMetadataV1* metadata, LunaIoErrorV1* error) {
    if (!metadata || !error) return LUNA_RUNTIME_STATUS_INVALID_ARGUMENT;
#ifdef _WIN32
    std::wstring converted;
#else
    std::string converted;
#endif
    if (!nativePath(path, pathSize, converted))
        return ioFailure(error, LUNA_IO_OPERATION_METADATA,
                         errno == 0 ? EINVAL : errno);
    NativeStat value{};
    if (pathStat(converted, &value) != 0)
        return ioFailure(error, LUNA_IO_OPERATION_METADATA, errno);
    writeMetadata(value, metadata);
    return LUNA_RUNTIME_STATUS_OK;
}

int removeFile(void*, const char* path, size_t pathSize, LunaIoErrorV1* error) {
    if (!error) return LUNA_RUNTIME_STATUS_INVALID_ARGUMENT;
#ifdef _WIN32
    std::wstring converted;
#else
    std::string converted;
#endif
    if (!nativePath(path, pathSize, converted))
        return ioFailure(error, LUNA_IO_OPERATION_REMOVE_FILE,
                         errno == 0 ? EINVAL : errno);
#ifdef _WIN32
    const int result = _wunlink(converted.c_str());
#else
    const int result = ::unlink(converted.c_str());
#endif
    return result == 0 ? LUNA_RUNTIME_STATUS_OK
                       : ioFailure(error, LUNA_IO_OPERATION_REMOVE_FILE, errno);
}

int createDirectory(void*, const char* path, size_t pathSize,
                    LunaIoErrorV1* error) {
    if (!error) return LUNA_RUNTIME_STATUS_INVALID_ARGUMENT;
#ifdef _WIN32
    std::wstring converted;
#else
    std::string converted;
#endif
    if (!nativePath(path, pathSize, converted))
        return ioFailure(error, LUNA_IO_OPERATION_CREATE_DIRECTORY,
                         errno == 0 ? EINVAL : errno);
#ifdef _WIN32
    const int result = _wmkdir(converted.c_str());
#else
    const int result = ::mkdir(converted.c_str(), 0777);
#endif
    return result == 0 ? LUNA_RUNTIME_STATUS_OK
                       : ioFailure(error, LUNA_IO_OPERATION_CREATE_DIRECTORY,
                                   errno);
}

const LunaConsoleV1 applicationConsole{
    LUNA_RUNTIME_ABI_V1, sizeof(LunaConsoleV1), nullptr,
    consoleWrite, consoleFlush, consoleRead};

const LunaFileSystemV1 applicationFileSystem{
    LUNA_RUNTIME_ABI_V1, sizeof(LunaFileSystemV1), nullptr,
    openFile, readFile, writeFile, seekFile, flushFile, syncFile, closeFile,
    handleMetadata, pathMetadata, removeFile, createDirectory};

} // namespace

const LunaConsoleV1* lunaApplicationConsoleV1() {
    return &applicationConsole;
}

const LunaFileSystemV1* lunaApplicationFileSystemV1() {
    return &applicationFileSystem;
}
