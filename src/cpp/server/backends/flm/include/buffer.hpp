/// \file buffer.hpp
/// \brief Buffer and bytes class for memory management
/// \author FastFlowLM Team
/// \date 2025-06-24
/// \version 0.9.10
/// \note This class is used to manage the memory.
#pragma once
#include <cstdint>
#include <cstddef>
#include <cassert>
#include <cstring>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>
#include <memory>

#define __XRT__

#ifdef __XRT__
#include "xrt/xrt_bo.h"
#include "xrt/xrt_kernel.h"
#include "xrt/xrt_device.h"
#include "xrt/experimental/xrt_ext.h"
#endif

#include "utils/debug_utils.hpp"

/// \brief bytes class
/// \note This is a buffer wrapper that maps to a bo_buffer or other memory without performing a deep copy.
/// \note A copy (or mapping) does not duplicate the underlying memory; it only maps the pointer.
class bytes {
protected:
    std::unique_ptr<uint8_t[]> owned_data_;
    uint8_t* data_;
    size_t size_;
    bool is_owner_;
#ifdef __XRT__
    bool is_bo_owner_;
    xrt::bo* bo_;
    std::unique_ptr<xrt::bo> owned_bo_;
#endif

public:
    /// \brief constructor
    /// \note This is a buffer wrapper that maps to a bo_buffer or other memory without performing a deep copy.
    /// \note A copy (or mapping) does not duplicate the underlying memory; it only maps the pointer.
    bytes() : data_(nullptr), size_(0), is_owner_(false)
#ifdef __XRT__
        , is_bo_owner_(false), bo_(nullptr), owned_bo_(nullptr)
#endif
    {}

    /// \brief copy constructor
    /// \param other the other bytes
    bytes(const bytes& other) : owned_data_(nullptr), data_(other.data_), size_(other.size_), is_owner_(false)
#ifdef __XRT__
        , is_bo_owner_(false), bo_(other.bo_), owned_bo_(nullptr)
#endif
    {}

    /// \brief move constructor
    /// \param other the other bytes
    bytes(bytes&& other) noexcept
        : owned_data_(std::move(other.owned_data_)), data_(other.data_), size_(other.size_), is_owner_(other.is_owner_)
#ifdef __XRT__
        , is_bo_owner_(other.is_bo_owner_), bo_(other.bo_), owned_bo_(std::move(other.owned_bo_))
#endif
    {
        other.data_ = nullptr;
        other.size_ = 0;
        other.is_owner_ = false;
#ifdef __XRT__
        other.is_bo_owner_ = false;
        other.bo_ = nullptr;
        other.owned_bo_ = nullptr;
#endif
    }

    /// \brief constructor
    /// \param size the size
    bytes(size_t size)
        : size_(size), is_owner_(true)
#ifdef __XRT__
        , is_bo_owner_(false), bo_(nullptr), owned_bo_(nullptr)
#endif
    {
        if (size > 0 && size < 8ull * 1024 * 1024 * 1024){
            try {
                owned_data_ = std::make_unique<uint8_t[]>(size);
            }
            catch (const std::bad_alloc& e) {
                throw std::runtime_error(std::string("Failed to allocate bytes of size ") + std::to_string(size) + ": " + e.what());
            }
            data_ = owned_data_.get();
        }
        else{
            throw std::runtime_error("Invalid size for bytes allocation");
        }
    }

    /// \brief constructor
    /// \param data the data
    /// \param size the size
    bytes(uint8_t* data, size_t size)
        : owned_data_(nullptr), data_(data), size_(size), is_owner_(false)
#ifdef __XRT__
        , is_bo_owner_(false), bo_(nullptr), owned_bo_(nullptr)
#endif
    {}

#ifdef __XRT__
    /// \brief constructor
    /// \param bo the bo
    bytes(xrt::bo& bo)
        : owned_data_(nullptr), data_(bo.map<uint8_t*>()), size_(bo.size()), is_owner_(false), is_bo_owner_(false), bo_(&bo), owned_bo_(nullptr)
    {}

    /// \brief constructor
    /// \param size the size
    /// \param device the device
    /// \param kernel the kernel
    /// \param group_id the group id
    /// \param flags the flags
    bytes(xrt::device& device, size_t size)
        : owned_data_(nullptr), size_(size), is_owner_(false), is_bo_owner_(true)
    {
        if (size > 3ull * 1024 * 1024 * 1024 || size == 0){
            throw std::runtime_error("Invalid size for bytes allocation");
        }
        size_t alignment = 1024 * 1024;
        int padded_size = (size + alignment - 1) / alignment * alignment; // 4KB alignment, , (xrt::ext::bo::access_mode)(xrt::ext::bo::access_mode::read_write | xrt::ext::bo::access_mode::process)

        try {
            owned_bo_ = std::make_unique<xrt::ext::bo>(device, padded_size);
        }
        catch (const std::exception& e) {
            throw std::runtime_error(std::string("Failed to allocate xrt::ext::bo: ") + e.what());
        }

        // uint64_t bo_address = reinterpret_cast<uintptr_t>(owned_bo_->map<uint8_t*>());
        // while ( ((bo_address & 0xF0000000) == 0x60000000) ||
        //     ((bo_address & 0xF0000000) == 0x70000000) ) {

        //     owned_bo_ = std::make_unique<xrt::ext::bo>(device, padded_size);
        //     //header_print("info", "Re-allocating proj_weights for layer " + std::to_string(i) + " to avoid address in 0x60000000 - 0x7FFFFFFF, new address: " + std::to_string(reinterpret_cast<uintptr_t>(proj_weights[i].data())));
        //     bo_address = reinterpret_cast<uintptr_t>(owned_bo_->map<uint8_t*>());
        // }

        data_ = owned_bo_->map<uint8_t*>();
        bo_ = owned_bo_.get();
    }
#endif

    /// \brief destructor
    virtual ~bytes() {
        if (is_owner_) {
            owned_data_.reset();
        }
        data_ = nullptr;
#ifdef __XRT__
        if (is_bo_owner_) {
            owned_bo_.reset();
        }
        bo_ = nullptr;
#endif
    }

    /// \brief copy assignment operator
    /// \param other the other bytes
    bytes& operator=(const bytes& other) {
        if (this != &other) {
            if (is_owner_){
                owned_data_.reset();
            }
            data_ = other.data_;
            size_ = other.size_;
            is_owner_ = false;
#ifdef __XRT__
            if (is_bo_owner_){
                owned_bo_.reset();
            }
            is_bo_owner_ = false;
            bo_ = other.bo_;
#endif
        }
        return *this;
    }

    /// \brief move assignment operator
    /// \param other the other bytes
    bytes& operator=(bytes&& other) noexcept {
        if (this != &other) {
            if (is_owner_){
                owned_data_.reset();
            }
            owned_data_ = std::move(other.owned_data_);
            data_ = other.data_;
            size_ = other.size_;
            is_owner_ = other.is_owner_;
#ifdef __XRT__
            if (is_bo_owner_){
                owned_bo_.reset();
            }
            is_bo_owner_ = other.is_bo_owner_;
            owned_bo_ = std::move(other.owned_bo_);
            bo_ = other.bo_;
            other.bo_ = nullptr;
            other.is_bo_owner_ = false;
#endif
            other.data_ = nullptr;
            other.size_ = 0;
            other.is_owner_ = false;
        }
        return *this;
    }

    /// \brief operator []
    /// \param index the index
    /// \return the value
    uint8_t& operator[](size_t index) {
        assert(data_ && index < size_);
        return data_[index];
    }

    /// \brief operator []
    /// \param index the index
    /// \return the value
    const uint8_t& operator[](size_t index) const {
        assert(data_ && index < size_);
        return data_[index];
    }

    size_t size() const { return size_; }
    uint8_t* data() const { return data_; }
    uint8_t* bdata() const { return data_; }
    uint8_t* begin() const { return data_; }
    uint8_t* end() const { return data_ + size_; }

    /// \brief copy from
    /// \param src the source
    /// \param size the size
    void copy_from(const uint8_t* src, size_t size) {
        assert(size <= size_);
        std::memcpy(data_, src, size);
    }

    /// \brief resize
    /// \param new_size the new size
    void resize(size_t new_size) {
#ifdef __XRT__
        assert(!is_bo_owner_);
#endif
        if (data_ != nullptr && !is_owner_) {
            throw std::runtime_error("Cannot resize a non-owner buffer");
        }
        if (new_size == 0) {
            throw std::runtime_error("Cannot resize to zero size");
        }
        try {
            owned_data_.reset(new uint8_t[new_size]);
        }
        catch (const std::bad_alloc& e) {
            throw std::runtime_error(std::string("Failed to allocate bytes of size ") + std::to_string(new_size) + ": " + e.what());
        }
        data_ = owned_data_.get();
        size_ = new_size;
        is_owner_ = true;
    }

    /// \brief free, release the memory or the bo
    void free() {
#ifdef __XRT__
        assert(!is_bo_owner_);
#endif
        if (is_owner_){
            owned_data_.reset();
        }
        data_ = nullptr;
        size_ = 0;
        is_owner_ = false;
#ifdef __XRT__
        if (is_bo_owner_){
            owned_bo_.reset();
        }
        is_bo_owner_ = false;
        bo_ = nullptr;
#endif
    }

    /// \brief reserve
    /// \param size the size
    void reserve(size_t size) { resize(size); }

    /// \brief release
    void release() { free(); }

    /// \brief is owner
    /// \return the is owner
    bool is_owner() const { return is_owner_; }
#ifdef __XRT__
    /// \brief is bo owner
    /// \return the is bo owner
    bool is_bo_owner() const { return is_bo_owner_; }

    /// \brief sync to device
    void sync_to_device() { assert(bo_); bo_->sync(XCL_BO_SYNC_BO_TO_DEVICE); }

    /// \brief sync from device
    void sync_from_device() { assert(bo_); bo_->sync(XCL_BO_SYNC_BO_FROM_DEVICE); }

    /// \brief bo
    /// \return the bo
    xrt::bo& bo() { assert(bo_); return *bo_; }
#endif

    /// \brief from file
    /// \param filename the filename
    /// \param offset the offset
    /// \param size the size
    void from_file(const std::string& filename, size_t offset = 0, size_t size = 0) {
        std::ifstream file(filename, std::ios::binary);
        if (!file.is_open()) {
            throw std::runtime_error("Failed to open file: " + filename);
        }
        file.seekg(0, std::ios::end);
        size_t file_size = file.tellg();
        file.seekg(0, std::ios::beg);
        if (size == 0) size = file_size;
        assert(size <= file_size);
        assert(offset + size <= size_);
        file.read(reinterpret_cast<char*>(data_) + offset, size);
        file.close();
    }
};

/// \brief buffer class
/// \note This class wraps a data type T over the underlying byte buffer.
template<typename T>
class buffer : public bytes {
public:
    /// \brief constructor
    buffer() : bytes() {}

    /// \brief constructor
    /// \param count the count
    buffer(size_t count) : bytes(count * sizeof(T)) {}

    /// \brief constructor
    /// \param data the data
    /// \param count the count
    buffer(T* data, size_t count)
        : bytes(reinterpret_cast<uint8_t*>(data), count * sizeof(T)) {}

    /// \brief shallow copy constructor
    /// \param other the other buffer
    buffer(const buffer& other) : bytes(other) {}

#ifdef __XRT__
    /// \brief constructor
    /// \param bo the bo
    buffer(xrt::bo& bo) : bytes(bo) {}

    /// \brief constructor
    /// \param count the count
    /// \param device the device
    /// \param kernel the kernel
    /// \param group_id the group id
    /// \param flags the flags
    buffer(xrt::device& device, size_t count)
        : bytes(device, count * sizeof(T)) {}
#endif

    /// \brief constructor
    /// \param vec the vector
    buffer(const std::vector<T>& vec)
        : bytes(reinterpret_cast<uint8_t*>(const_cast<T*>(vec.data())), vec.size() * sizeof(T))
    {
    }

    /// \brief constructor
    /// \param vec the vector
    /// \warning This also creates a shallow mapping.
    /// \warning The caller must ensure that the vector is not used (and remains valid)
    /// \warning after constructing this buffer.
    buffer(std::vector<T>&& vec)
        : bytes(reinterpret_cast<uint8_t*>(vec.data()), vec.size() * sizeof(T))
    {
    }

    /// \brief copy from
    /// \param vec the vector
    void copy_from(const std::vector<T>& vec) {
        if (vec.size() * sizeof(T) != this->size_) {
            throw std::runtime_error("Size mismatch in copy_from(vector)");
        }
        std::memcpy(data_, vec.data(), size_);
    }

    /// \brief cast to another type
    /// \tparam U the type
    /// \return the buffer
    template<typename U>
    buffer<U> cast_to() {
        size_t newCount = size_ / sizeof(U);
        return buffer<U>(reinterpret_cast<U*>(data_), newCount);
    }

    /// \brief as bytes
    /// \return the bytes
    const bytes as_bytes() const {
        return *this;
    }

    /// \brief move assignment operator
    /// \param other the other buffer
    /// \return the buffer
    buffer<T>& operator=(buffer<T>&& other) noexcept {
        bytes::operator=(std::move(other));
        return *this;
    }

    /// \brief copy assignment operator
    /// \param other the other buffer
    /// \return the buffer
    buffer<T>& operator=(const buffer<T>& other) {
        bytes::operator=(other);
        return *this;
    }

    /// \brief operator []
    /// \param index the index
    /// \return the value
    T& operator[](size_t index) {
        assert(data_ != nullptr);
        assert(index < size());
        assert(index >= 0);
        return reinterpret_cast<T*>(data_)[index];
    }

    /// \brief operator []
    /// \param index the index
    /// \return the value
    const T& operator[](size_t index) const {
        assert(data_ != nullptr);
        assert(index < size());
        assert(index >= 0);
        return reinterpret_cast<T*>(data_)[index];
    }

    /// \brief size, number of elements
    /// \return the size
    size_t size() const { return size_ / sizeof(T); }

    /// \brief data
    /// \return the data
    T* data() const { return reinterpret_cast<T*>(data_); }

    /// \brief begin
    /// \return the pointer to the first element
    T* begin() const { return reinterpret_cast<T*>(data_); }

    /// \brief end
    /// \return the pointer to the last element
    T* end() const { return reinterpret_cast<T*>(data_) + size(); }

    /// \brief resize
    /// \param count: the number of elements
    void resize(size_t count) {
        bytes::resize(count * sizeof(T));
    }

    /// \brief reserve
    /// \param count: the number of elements
    void reserve(size_t count) {
        bytes::reserve(count * sizeof(T));
    }

    /// \brief memset
    /// \param value the value
    void memset(T value) {
        T* ptr = data();
        for (size_t i = 0; i < size(); i++) {
            ptr[i] = value;
        }
    }

    /// \brief copy from
    /// \param other the other bytes
    void copy_from(const bytes& other) {
        if (size_ != other.size()) {
            throw std::runtime_error("Size mismatch in copy_from(bytes)");
        }
        memcpy(data_, other.data(), size_); // size_ is already in bytes.
    }

    /// \brief copy from
    /// \param other the other buffer
    void copy_from(const buffer<T>& other) {
        if (size() != other.size()) {
            throw std::runtime_error("Size mismatch in copy_from(buffer)");
        }
        memcpy(data_, other.bdata(), size_); // size_ is already in bytes.
    }

    /// \brief copy from
    /// \param data the data
    /// \param size the number of elements
    void copy_from(T* data, size_t size) {
        if (size > this->size()) {
            throw std::runtime_error("Size mismatch in copy_from(pointer)");
        }
        memcpy(data_, data, size * sizeof(T));
    }

    /// \brief as bytes
    /// \return the bytes
    bytes& as_bytes() {
        return *this;
    }

    /// \brief from file
    /// \param filename the filename
    /// \param offset the offset
    /// \param size the number of elements
    void from_file(const std::string& filename, size_t offset = 0, size_t size = 0) {
        bytes::from_file(filename, offset * sizeof(T), size * sizeof(T));
    }
};
