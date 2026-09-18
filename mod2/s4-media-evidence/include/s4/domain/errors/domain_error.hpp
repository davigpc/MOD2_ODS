#pragma once

#include <string>
#include <stdexcept>

namespace ods::s4::domain {

class ODSBaseException : public std::runtime_error {
public:
    explicit ODSBaseException(const std::string& message)
        : std::runtime_error(message) {}
};

class DomainError : public ODSBaseException {
public:
    explicit DomainError(const std::string& message)
        : ODSBaseException(message) {}
};

class InvalidTimeWindowError : public DomainError {
public:
    explicit InvalidTimeWindowError(const std::string& message = "start_time must be strictly before end_time")
        : DomainError(message) {}
};

class DiskQuotaExceededError : public DomainError {
public:
    explicit DiskQuotaExceededError(const std::string& message = "NVMe disk quota exceeded with no purgeable media")
        : DomainError(message) {}
};

class ApplicationError : public ODSBaseException {
public:
    explicit ApplicationError(const std::string& message)
        : ODSBaseException(message) {}
};

class MediaFileNotFoundError : public ApplicationError {
public:
    explicit MediaFileNotFoundError(const std::string& message = "Media file not found on storage")
        : ApplicationError(message) {}
};

class MediaBufferEmptyError : public ApplicationError {
public:
    explicit MediaBufferEmptyError(const std::string& message = "No media frames available in the requested time window")
        : ApplicationError(message) {}
};

class HashingError : public ApplicationError {
public:
    explicit HashingError(const std::string& message = "Failed to compute cryptographic hash")
        : ApplicationError(message) {}
};

class SqliteStorageError : public ApplicationError {
public:
    explicit SqliteStorageError(const std::string& message = "SQLite storage error")
        : ApplicationError(message) {}
};

class FileOperationError : public ApplicationError {
public:
    explicit FileOperationError(const std::string& message = "File operation failed")
        : ApplicationError(message) {}
};

} // namespace ods::s4::domain
