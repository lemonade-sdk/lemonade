/// \file tensor_2d.hpp
/// \brief tensor_2d class
/// \author FastFlowLM Team
/// \date 2025-06-24
/// \version 0.9.24
/// \note This class is a helper class for managing 2D buffers.
#pragma once
#include "typedef.hpp"
#include "buffer.hpp"


/// \note This class is a helper class for managing 2D buffers.
template<typename T>
class tensor_2d{
private:
    uint32_t D;
    buffer<T> *buf;
    uint32_t offset;
    std::vector<buffer<T>> temp;
public:
    /// \brief constructor
    /// \param D the dimension of the tensor
    /// \param offset the offset of the tensor
    tensor_2d(uint32_t D, uint32_t offset = 0) : D(D), offset(offset) {buf = nullptr;}

    /// \brief constructor
    /// \param buf the buffer to assign
    tensor_2d(buffer<T> &buf, uint32_t D, uint32_t offset = 0) : D(D), offset(offset) {
        assign(buf);
    }
    ~tensor_2d(){}

    /// \brief assign the buffer to the tensor_2d
    /// \param buf the buffer to assign
    void assign(buffer<T> &buf){
        this->buf = &buf;
        temp.resize(buf.size() / D);
        for (uint32_t i = 0; i < temp.size(); i++){
            temp[i] = buffer<T>(buf.data() + i * D, D);
        }
    }

    /// \brief clear the tensor_2d
    void clear(){
        buf = nullptr;
    }

    /// \brief get the buffer at the index
    /// \param idx the index
    /// \return the buffer at the index
    buffer<T>& operator[](uint32_t idx){
        assert(buf != nullptr);
        return temp[idx + offset];
    }

    /// \brief assign the buffer to the tensor_2d
    /// \param buf the buffer to assign
    void operator=(buffer<T>& buf){
        assign(buf);
    }

    /// \brief set the offset of the tensor_2d
    /// \param offset the offset to set
    void set_offset(uint32_t offset){
        this->offset = offset;
    }

    /// \brief get the sub buffer
    /// \param start the start index
    /// \param end the end index
    /// \return the sub buffer
    buffer<T>& get_sub_buffer(uint32_t start, uint32_t end){
        assert((end + offset) * D <= buf->size());
        return buffer<T>(buf->data() + (start + offset) * D, (end - start) * D);
    }
};
