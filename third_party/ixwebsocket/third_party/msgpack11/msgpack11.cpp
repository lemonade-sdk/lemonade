#include "msgpack11.hpp"
#include <cassert>
#include <cmath>
#include <cstdlib>
#include <cstdio>
#include <limits>
#include <array>
#include <tuple>
#include <algorithm>
#include <functional>
#include <stdexcept>

namespace msgpack11 {

static const int max_depth = 200;

using std::string;
using std::vector;
using std::map;
using std::make_shared;
using std::initializer_list;
using std::move;

/* Helper for representing null - just a do-nothing struct, plus comparison
 * operators so the helpers in MsgPackValue work. We can't use nullptr_t because
 * it may not be orderable.
 */
struct NullStruct {
    bool operator==(NullStruct) const { return true; }
    bool operator<(NullStruct) const { return false; }
};

/* * * * * * * * * * * * * * * * * * * *
 * MasPackValue
 */

class MsgPackValue {
public:
    virtual bool equals(const MsgPackValue * other) const = 0;
    virtual bool less(const MsgPackValue * other) const = 0;
    virtual void dump(std::string &out) const = 0;
    virtual MsgPack::Type type() const = 0;
    virtual double number_value() const;
    virtual float float32_value() const;
    virtual double float64_value() const;
    virtual int32_t int_value() const;
    virtual int8_t int8_value() const;
    virtual int16_t int16_value() const;
    virtual int32_t int32_value() const;
    virtual int64_t int64_value() const;
    virtual uint8_t uint8_value() const;
    virtual uint16_t uint16_value() const;
    virtual uint32_t uint32_value() const;
    virtual uint64_t uint64_value() const;
    virtual bool bool_value() const;
    virtual const std::string &string_value() const;
    virtual const MsgPack::array &array_items() const;
    virtual const MsgPack::binary &binary_items() const;
    virtual const MsgPack &operator[](size_t i) const;
    virtual const MsgPack::object &object_items() const;
    virtual const MsgPack &operator[](const std::string &key) const;
    virtual const MsgPack::extension &extension_items() const;
    virtual ~MsgPackValue() {}
};

/* * * * * * * * * * * * * * * * * * * *
 * Serialization
 */

namespace {
static const union {
    uint16_t dummy;
    uint8_t bytes[2];
} endian_check_data { 0x0001 };
static const bool is_big_endian = endian_check_data.bytes[0] == 0x00;

template< typename T >
struct EndianConverter {
    union {
        T packed;
        std::array<uint8_t, sizeof(T)> bytes;
    } value;
};

template< typename T >
void dump_data(T value, std::string &out)
{
    EndianConverter<T> converter;
    converter.value.packed = value;

    auto f = [&](uint8_t byte) {
        out.push_back(byte);
    };

    if(is_big_endian)
    {
        std::for_each(converter.value.bytes.begin(), converter.value.bytes.end(), f);
    }
    else
    {
        std::for_each(converter.value.bytes.rbegin(), converter.value.bytes.rend(), f);
    }
}

/* * * * * * * * * * * * * * * * * * * *
 * MZ HACK: Address additional newer versions of clang complaining when size_t is equivalent to uint32_t
 */
static void verify_length(size_t len)
{
    constexpr bool size_t_greater_than_uint32_t = sizeof(size_t) > sizeof(uint32_t);

    // Lengths must be 32-bit or less, so only check when size_t can represent values greater than 32-bit
    if (size_t_greater_than_uint32_t)
    {
        static_assert(sizeof(size_t) <= sizeof(uint64_t), "Times changed and size_t grew");
        if (static_cast<uint64_t>(len) > 0xffffffff)
            throw std::runtime_error("exceeded maximum data length");
    }
}

static void dump(NullStruct, std::string &out) {
    out.push_back((char) 0xc0);
}

static void dump(float value, std::string &out) {
    out.push_back((char) 0xca);
    dump_data(value, out);
}

static void dump(double value, std::string &out) {
    out.push_back((char) 0xcb);
    dump_data(value, out);
}

static void dump(int8_t value, std::string &out) {
    if( value < -32 )
    {
        out.push_back((char) 0xd0);
    }
    out.push_back(value);
}

static void dump(int16_t value, std::string &out) {
    out.push_back((char) 0xd1);
    dump_data(value, out);
}

static void dump(int32_t value, std::string &out) {
    out.push_back((char) 0xd2);
    dump_data(value, out);
}

static void dump(int64_t value, std::string &out) {
    out.push_back((char) 0xd3);
    dump_data(value, out);
}

static void dump(uint8_t value, std::string &out) {
    if(128 <= value)
    {
        out.push_back((char) 0xcc);
    }
    out.push_back(value);
}

static void dump(uint16_t value, std::string &out) {
    out.push_back((char) 0xcd);
    dump_data(value, out);
}

static void dump(uint32_t value, std::string &out) {
    out.push_back((char) 0xce);
    dump_data(value, out);
}

static void dump(uint64_t value, std::string &out) {
    out.push_back((char) 0xcf);
    dump_data(value, out);
}

static void dump(bool value, std::string &out) {
    const uint8_t msgpack_value = (value) ? 0xc3 : 0xc2;
    out.push_back(msgpack_value);
}

static void dump(const std::string& value, std::string &out) {
    size_t const len = value.size();
    verify_length(len);
    if(len <= 0x1f)
    {
        uint8_t const first_byte = 0xa0 | static_cast<uint8_t>(len);
        out.push_back(first_byte);
    }
    else if(len <= 0xff)
    {
        uint8_t const length = static_cast<uint8_t>(len);
        out.push_back((char) 0xd9);
        out.push_back(length);
    }
    else if(len <= 0xffff)
    {
        uint16_t const length = static_cast<uint16_t>(len);
        out.push_back((char) 0xda);
        dump_data(length, out);
    }
    else
    {
        uint32_t const length = static_cast<uint32_t>(len);
        out.push_back((char) 0xdb);
        dump_data(length, out);
    }

    std::for_each(std::begin(value), std::end(value), [&out](char v){
        dump_data(v, out);
    });
}

static void dump(const MsgPack::array& value, std::string &out) {
    size_t const len = value.size();
    verify_length(len);
    if(len <= 15)
    {
        uint8_t const first_byte = 0x90 | static_cast<uint8_t>(len);
        out.push_back(first_byte);
    }
    else if(len <= 0xffff)
    {
        uint16_t const length = static_cast<uint16_t>(len);
        out.push_back((char) 0xdc);
        dump_data(length, out);
    }
    else
    {
        uint32_t const length = static_cast<uint32_t>(len);
        out.push_back((char) 0xdd);
        dump_data(length, out);
    }

    std::for_each(std::begin(value), std::end(value), [&out](MsgPack::array::value_type const& v){
        v.dump(out);
    });
}

static void dump(const MsgPack::object& value, std::string &out) {
    size_t const len = value.size();
    verify_length(len);
    if(len <= 15)
    {
        uint8_t const first_byte = 0x80 | static_cast<uint8_t>(len);
        out.push_back(first_byte);
    }
    else if(len <= 0xffff)
    {
        uint16_t const length = static_cast<uint16_t>(len);
        out.push_back((char) 0xde);
        dump_data(length, out);
    }
    else
    {
        uint32_t const length = static_cast<uint32_t>(len);
        out.push_back((char) 0xdf);
        dump_data(length, out);
    }

    std::for_each(std::begin(value), std::end(value), [&out](MsgPack::object::value_type const& v){
        v.first.dump(out);
        v.second.dump(out);
    });
}

static void dump(const MsgPack::binary& value, std::string &out) {
    size_t const len = value.size();
    verify_length(len);
    if(len <= 0xff)
    {
        uint8_t const length = static_cast<uint8_t>(len);
        out.push_back((char) 0xc4);
        dump_data(length, out);
    }
    else if(len <= 0xffff)
    {
        uint16_t const length = static_cast<uint16_t>(len);
        out.push_back((char) 0xc5);
        dump_data(length, out);
    }
    else
    {
        uint32_t const length = static_cast<uint32_t>(len);
        out.push_back((char) 0xc6);
        dump_data(length, out);
    }

    std::for_each(std::begin(value), std::end(value), [&out](MsgPack::binary::value_type const& v){
        out.push_back(v);
    });
}

static void dump(const MsgPack::extension& value, std::string &out) {
    const uint8_t type = std::get<0>( value );
    const MsgPack::binary& data = std::get<1>( value );
    const size_t len = data.size();
    verify_length(len);

    if(len == 0x01) {
        out.push_back((char) 0xd4);
    }
    else if(len == 0x02) {
        out.push_back((char) 0xd5);
    }
    else if(len == 0x04) {
        out.push_back((char) 0xd6);
    }
    else if(len == 0x08) {
        out.push_back((char) 0xd7);
    }
    else if(len == 0x10) {
        out.push_back((char) 0xd8);
    }
    else if(len <= 0xff) {
        uint8_t const length = static_cast<uint8_t>(len);
        out.push_back((char) 0xc7);
        out.push_back(length);
    }
    else if(len <= 0xffff) {
        uint16_t const length = static_cast<uint16_t>(len);
        out.push_back((char) 0xc8);
        dump_data(length, out);
    }
    else {
        uint32_t const length = static_cast<uint32_t>(len);
        out.push_back((char) 0xc9);
        dump_data(length, out);
    }

    out.push_back(type);
    std::for_each(std::begin(data), std::end(data), [&out](uint8_t const& v){
        out.push_back(v);
    });
}
}

void MsgPack::dump(std::string &out) const {
    m_ptr->dump(out);
}

/* * * * * * * * * * * * * * * * * * * *
 * Value wrappers
 */

template <MsgPack::Type tag, typename T>
class Value : public MsgPackValue {
protected:

    // Constructors
    explicit Value(const T &value) : m_value(value) {}
    explicit Value(T &&value)      : m_value(move(value)) {}

    // Get type tag
    MsgPack::Type type() const override {
        return tag;
    }

    // Comparisons
    bool equals(const MsgPackValue * other) const override {
        bool const is_same_type = tag == other->type();
        return is_same_type && (m_value == static_cast<const Value<tag, T> *>(other)->m_value);
    }
    bool less(const MsgPackValue * other) const override {
        bool const is_same_type = tag == other->type();
        bool const is_less_type = tag < other->type();
        return is_less_type || (is_same_type && (m_value < static_cast<const Value<tag, T> *>(other)->m_value));
    }

    const T m_value;
    void dump(std::string &out) const override { msgpack11::dump(m_value, out); }
};

bool equal_uint64_int64( uint64_t uint64_value, int64_t int64_value )
{
    bool const is_positive = 0 <= int64_value;
    bool const is_leq_int64_max = uint64_value <= std::numeric_limits<int64_t>::max();
    return is_positive && is_leq_int64_max && ( uint64_value == static_cast<uint64_t>(int64_value));
}

bool less_uint64_int64( uint64_t uint64_value, int64_t int64_value )
{
    bool const is_positive = 0 <= int64_value;
    bool const is_leq_int64_max = uint64_value <= std::numeric_limits<int64_t>::max();
    return is_positive && is_leq_int64_max && ( uint64_value < static_cast<uint64_t>(int64_value));
}

bool less_int64_uint64( int64_t int64_value, uint64_t uint64_value )
{
    bool const is_negative = int64_value < 0;
    bool const is_gt_int64_max = std::numeric_limits<int64_t>::max() < uint64_value;
    return is_negative || is_gt_int64_max || ( static_cast<uint64_t>(int64_value) < uint64_value );
}

template <MsgPack::Type tag, typename T>
class NumberValue : public Value<tag, T>  {
protected:

    // Constructors
    explicit NumberValue(const T &value) : Value<tag, T>(value) {}
    explicit NumberValue(T &&value)      : Value<tag, T>(move(value)) {}

    bool equals(const MsgPackValue * other) const override {
        switch( other->type() )
        {
            case MsgPack::FLOAT32 : // fall through
            case MsgPack::FLOAT64 : // fall through
            case MsgPack::UINT8   : // fall through
            case MsgPack::UINT16  : // fall through
            case MsgPack::UINT32  : // fall through
            case MsgPack::UINT64  : // fall through
            case MsgPack::INT8    : // fall through
            case MsgPack::INT16   : // fall through
            case MsgPack::INT32   : // fall through
            case MsgPack::INT64   : // fall through
            {
                return float64_value() == other->float64_value();
            } break;
            default               :
            {
                return Value<tag,T>::equals( other );
            } break;
        }
    }

    bool less(const MsgPackValue * other) const override {
        switch( other->type() )
        {
            case MsgPack::FLOAT32 : // fall through
            case MsgPack::FLOAT64 : // fall through
            case MsgPack::UINT8   : // fall through
            case MsgPack::UINT16  : // fall through
            case MsgPack::UINT32  : // fall through
            case MsgPack::UINT64  : // fall through
            case MsgPack::INT8    : // fall through
            case MsgPack::INT16   : // fall through
            case MsgPack::INT32   : // fall through
            case MsgPack::INT64   : // fall through
            {
                return float64_value() < other->float64_value();
            } break;
            default               :
            {
                return Value<tag,T>::less( other );
            } break;
        }
    }

    double  number_value()  const override { return static_cast<double>( Value<tag,T>::m_value ); }
    float float32_value()   const override { return static_cast<float>( Value<tag,T>::m_value ); }
    double float64_value()  const override { return static_cast<double>( Value<tag,T>::m_value ); }
    int32_t int_value()     const override { return static_cast<int32_t>( Value<tag,T>::m_value ); }
    int8_t int8_value()     const override { return static_cast<int8_t>( Value<tag,T>::m_value ); }
    int16_t int16_value()   const override { return static_cast<int16_t>( Value<tag,T>::m_value ); }
    int32_t int32_value()   const override { return static_cast<int32_t>( Value<tag,T>::m_value ); }
    int64_t int64_value()   const override { return static_cast<int64_t>( Value<tag,T>::m_value ); }
    uint8_t uint8_value()   const override { return static_cast<uint8_t>( Value<tag,T>::m_value ); }
    uint16_t uint16_value() const override { return static_cast<uint16_t>( Value<tag,T>::m_value ); }
    uint32_t uint32_value() const override { return static_cast<uint32_t>( Value<tag,T>::m_value ); }
    uint64_t uint64_value() const override { return static_cast<uint64_t>( Value<tag,T>::m_value ); }
};

class MsgPackFloat final : public NumberValue<MsgPack::FLOAT32, float> {
public:
    explicit MsgPackFloat(float value) : NumberValue(value) {}
};

class MsgPackDouble final : public NumberValue<MsgPack::FLOAT64, double> {
public:
    explicit MsgPackDouble(double value) : NumberValue(value) {}
};

class MsgPackInt8 final : public NumberValue<MsgPack::INT8, int8_t> {
public:
    explicit MsgPackInt8(int8_t value) : NumberValue(value) {}
};

class MsgPackInt16 final : public NumberValue<MsgPack::INT16, int16_t> {
public:
    explicit MsgPackInt16(int16_t value) : NumberValue(value) {}
};

class MsgPackInt32 final : public NumberValue<MsgPack::INT32, int32_t> {
public:
    explicit MsgPackInt32(int32_t value) : NumberValue(value) {}
};

class MsgPackInt64 final : public NumberValue<MsgPack::INT64, int64_t> {
    bool equals(const MsgPackValue * other) const override
    {
        switch( other->type() )
        {
            case MsgPack::INT64 :
            {
                return int64_value() == other->int64_value();
            } break;
            case MsgPack::UINT64 :
            {
                return equal_uint64_int64( other->uint64_value(), int64_value() );
            } break;
            default :
            {
                return NumberValue<MsgPack::INT64, int64_t>::equals( other );
            }
        }
    }
    bool less(const MsgPackValue * other)   const override
    {
        switch( other->type() )
        {
            case MsgPack::INT64 :
            {
                return int64_value() < other->int64_value();
            } break;
            case MsgPack::UINT64 :
            {
                return less_int64_uint64( int64_value(), other->uint64_value() );
            } break;
            default :
            {
                return NumberValue<MsgPack::INT64, int64_t>::less( other );
            }
        }
    }
public:
    explicit MsgPackInt64(int64_t value) : NumberValue(value) {}
};

class MsgPackUint8 final : public NumberValue<MsgPack::UINT8, uint8_t> {
public:
    explicit MsgPackUint8(uint8_t value) : NumberValue(value) {}
};

class MsgPackUint16 final : public NumberValue<MsgPack::UINT16, uint16_t> {
public:
    explicit MsgPackUint16(uint16_t value) : NumberValue(value) {}
};

class MsgPackUint32 final : public NumberValue<MsgPack::UINT32, uint32_t> {
public:
    explicit MsgPackUint32(uint32_t value) : NumberValue(value) {}
};

class MsgPackUint64 final : public NumberValue<MsgPack::UINT64, uint64_t> {
    bool equals(const MsgPackValue * other) const override
    {
        switch( other->type() )
        {
            case MsgPack::INT64 :
            {
                return equal_uint64_int64( uint64_value(), other->int64_value() );
            } break;
            case MsgPack::UINT64 :
            {
                return uint64_value() == other->uint64_value();
            } break;
            default :
            {
                return NumberValue<MsgPack::UINT64, uint64_t>::equals( other );
            }
        }
    }
    bool less(const MsgPackValue * other)   const override
    {
        switch( other->type() )
        {
            case MsgPack::INT64 :
            {
                return less_uint64_int64( uint64_value(), other->uint64_value() );
            } break;
            case MsgPack::UINT64 :
            {
                return uint64_value() < other->uint64_value();
            } break;
            default :
            {
                return NumberValue<MsgPack::UINT64, uint64_t>::less( other );
            }
        }
    }
public:
    explicit MsgPackUint64(uint64_t value) : NumberValue(value) {}
};

class MsgPackBoolean final : public Value<MsgPack::BOOL, bool> {
    bool bool_value() const override { return m_value; }
public:
    explicit MsgPackBoolean(bool value) : Value(value) {}
};

class MsgPackString final : public Value<MsgPack::STRING, string> {
    const string &string_value() const override { return m_value; }
public:
    explicit MsgPackString(const string &value) : Value(value) {}
    explicit MsgPackString(string &&value)      : Value(move(value)) {}
};

class MsgPackArray final : public Value<MsgPack::ARRAY, MsgPack::array> {
    const MsgPack::array &array_items() const override { return m_value; }
    const MsgPack & operator[](size_t i) const override;
public:
    explicit MsgPackArray(const MsgPack::array &value) : Value(value) {}
    explicit MsgPackArray(MsgPack::array &&value)      : Value(move(value)) {}
};

class MsgPackBinary final : public Value<MsgPack::BINARY, MsgPack::binary> {
    const MsgPack::binary &binary_items() const override { return m_value; }
public:
    explicit MsgPackBinary(const MsgPack::binary &value) : Value(value) {}
    explicit MsgPackBinary(MsgPack::binary &&value)      : Value(move(value)) {}
};

class MsgPackObject final : public Value<MsgPack::OBJECT, MsgPack::object> {
    const MsgPack::object &object_items() const override { return m_value; }
    const MsgPack & operator[](const string &key) const override;
public:
    explicit MsgPackObject(const MsgPack::object &value) : Value(value) {}
    explicit MsgPackObject(MsgPack::object &&value)      : Value(move(value)) {}
};

class MsgPackExtension final : public Value<MsgPack::EXTENSION, MsgPack::extension> {
    const MsgPack::extension &extension_items() const override { return m_value; }
public:
    explicit MsgPackExtension(const MsgPack::extension &value) : Value(value) {}
    explicit MsgPackExtension(MsgPack::extension &&value)      : Value(move(value)) {}
};

class MsgPackNull final : public Value<MsgPack::NUL, NullStruct> {
public:
    MsgPackNull() : Value({}) {}
};

/* * * * * * * * * * * * * * * * * * * *
 * Static globals - static-init-safe
 */
struct Statics {
    const std::shared_ptr<MsgPackValue> null = make_shared<MsgPackNull>();
    const std::shared_ptr<MsgPackValue> t = make_shared<MsgPackBoolean>(true);
    const std::shared_ptr<MsgPackValue> f = make_shared<MsgPackBoolean>(false);
    const string empty_string;
    const vector<MsgPack> empty_vector;
    const map<MsgPack, MsgPack> empty_map;
    const MsgPack::binary empty_binary;
    const MsgPack::extension empty_extension;
    Statics() {}
};

static const Statics & statics() {
    static const Statics s {};
    return s;
}

static const MsgPack & static_null() {
    // This has to be separate, not in Statics, because MsgPack() accesses statics().null.
    static const MsgPack msgpack_null;
    return msgpack_null;
}

/* * * * * * * * * * * * * * * * * * * *
 * Constructors
 */

MsgPack::MsgPack() noexcept                        : m_ptr(statics().null) {}
MsgPack::MsgPack(std::nullptr_t) noexcept          : m_ptr(statics().null) {}
MsgPack::MsgPack(float value)                      : m_ptr(make_shared<MsgPackFloat>(value)) {}
MsgPack::MsgPack(double value)                     : m_ptr(make_shared<MsgPackDouble>(value)) {}
MsgPack::MsgPack(int8_t value)                     : m_ptr(make_shared<MsgPackInt8>(value)) {}
MsgPack::MsgPack(int16_t value)                    : m_ptr(make_shared<MsgPackInt16>(value)) {}
MsgPack::MsgPack(int32_t value)                    : m_ptr(make_shared<MsgPackInt32>(value)) {}
MsgPack::MsgPack(int64_t value)                    : m_ptr(make_shared<MsgPackInt64>(value)) {}
MsgPack::MsgPack(uint8_t value)                    : m_ptr(make_shared<MsgPackUint8>(value)) {}
MsgPack::MsgPack(uint16_t value)                   : m_ptr(make_shared<MsgPackUint16>(value)) {}
MsgPack::MsgPack(uint32_t value)                   : m_ptr(make_shared<MsgPackUint32>(value)) {}
MsgPack::MsgPack(uint64_t value)                   : m_ptr(make_shared<MsgPackUint64>(value)) {}
MsgPack::MsgPack(bool value)                       : m_ptr(value ? statics().t : statics().f) {}
MsgPack::MsgPack(const string &value)              : m_ptr(make_shared<MsgPackString>(value)) {}
MsgPack::MsgPack(string &&value)                   : m_ptr(make_shared<MsgPackString>(move(value))) {}
MsgPack::MsgPack(const char * value)               : m_ptr(make_shared<MsgPackString>(value)) {}
MsgPack::MsgPack(const MsgPack::array &values)     : m_ptr(make_shared<MsgPackArray>(values)) {}
MsgPack::MsgPack(MsgPack::array &&values)          : m_ptr(make_shared<MsgPackArray>(move(values))) {}
MsgPack::MsgPack(const MsgPack::object &values)    : m_ptr(make_shared<MsgPackObject>(values)) {}
MsgPack::MsgPack(MsgPack::object &&values)         : m_ptr(make_shared<MsgPackObject>(move(values))) {}
MsgPack::MsgPack(const MsgPack::binary &values)    : m_ptr(make_shared<MsgPackBinary>(values)) {}
MsgPack::MsgPack(MsgPack::binary &&values)         : m_ptr(make_shared<MsgPackBinary>(move(values))) {}
MsgPack::MsgPack(const MsgPack::extension &values) : m_ptr(make_shared<MsgPackExtension>(values)) {}
MsgPack::MsgPack(MsgPack::extension &&values)      : m_ptr(make_shared<MsgPackExtension>(move(values))) {}

/* * * * * * * * * * * * * * * * * * * *
 * Accessors
 */

MsgPack::Type MsgPack::type()                           const { return m_ptr->type(); }
double MsgPack::number_value()                          const { return m_ptr->float64_value(); }
float MsgPack::float32_value()                          const { return m_ptr->float32_value(); }
double MsgPack::float64_value()                         const { return m_ptr->float64_value(); }
int32_t MsgPack::int_value()                            const { return m_ptr->int32_value(); }
int8_t MsgPack::int8_value()                            const { return m_ptr->int8_value(); }
int16_t MsgPack::int16_value()                          const { return m_ptr->int16_value(); }
int32_t MsgPack::int32_value()                          const { return m_ptr->int32_value(); }
int64_t MsgPack::int64_value()                          const { return m_ptr->int64_value(); }
uint8_t MsgPack::uint8_value()                          const { return m_ptr->uint8_value(); }
uint16_t MsgPack::uint16_value()                        const { return m_ptr->uint16_value(); }
uint32_t MsgPack::uint32_value()                        const { return m_ptr->uint32_value(); }
uint64_t MsgPack::uint64_value()                        const { return m_ptr->uint64_value(); }
bool MsgPack::bool_value()                              const { return m_ptr->bool_value(); }
const string & MsgPack::string_value()                  const { return m_ptr->string_value(); }
const vector<MsgPack>& MsgPack::array_items()           const { return m_ptr->array_items(); }
const MsgPack::binary& MsgPack::binary_items()          const { return m_ptr->binary_items(); }
const MsgPack::extension& MsgPack::extension_items()    const { return m_ptr->extension_items(); }
const map<MsgPack, MsgPack> & MsgPack::object_items()   const { return m_ptr->object_items(); }
const MsgPack & MsgPack::operator[] (size_t i)          const { return (*m_ptr)[i]; }
const MsgPack & MsgPack::operator[] (const string &key) const { return (*m_ptr)[key]; }

double                        MsgPackValue::number_value()              const { return 0.0; }
float                         MsgPackValue::float32_value()             const { return 0.0f; }
double                        MsgPackValue::float64_value()             const { return 0.0; }
int32_t                       MsgPackValue::int_value()                 const { return 0; }
int8_t                        MsgPackValue::int8_value()                const { return 0; }
int16_t                       MsgPackValue::int16_value()               const { return 0; }
int32_t                       MsgPackValue::int32_value()               const { return 0; }
int64_t                       MsgPackValue::int64_value()               const { return 0; }
uint8_t                       MsgPackValue::uint8_value()               const { return 0; }
uint16_t                      MsgPackValue::uint16_value()              const { return 0; }
uint32_t                      MsgPackValue::uint32_value()              const { return 0; }
uint64_t                      MsgPackValue::uint64_value()              const { return 0; }
bool                          MsgPackValue::bool_value()                const { return false; }
const string &                MsgPackValue::string_value()              const { return statics().empty_string; }
const vector<MsgPack> &       MsgPackValue::array_items()               const { return statics().empty_vector; }
const map<MsgPack, MsgPack> & MsgPackValue::object_items()              const { return statics().empty_map; }
const MsgPack::binary & MsgPackValue::binary_items()                    const { return statics().empty_binary; }
const MsgPack::extension & MsgPackValue::extension_items()              const { return statics().empty_extension; }
const MsgPack &               MsgPackValue::operator[] (size_t)         const { return static_null(); }
const MsgPack &               MsgPackValue::operator[] (const string &) const { return static_null(); }

const MsgPack & MsgPackObject::operator[] (const string &key) const {
    auto iter = m_value.find(key);
    return (iter == m_value.end()) ? static_null() : iter->second;
}
const MsgPack & MsgPackArray::operator[] (size_t i) const {
    if (i >= m_value.size()) return static_null();
    else return m_value[i];
}

/* * * * * * * * * * * * * * * * * * * *
 * Comparison
 */

bool MsgPack::operator== (const MsgPack &other) const {
    return m_ptr->equals(other.m_ptr.get());
}

bool MsgPack::operator< (const MsgPack &other) const {
    return m_ptr->less(other.m_ptr.get());
}

namespace {
/* MsgPackParser
 *
 * Object that tracks all state of an in-progress parse.
 */
struct MsgPackParser final {

    /* State
     */
    const std::string &buffer;
    size_t i;
    string &err;
    bool failed;

    /* fail(msg, err_ret = MsgPack())
     *
     * Mark this parse as failed.
     */
    MsgPack fail(string &&msg) {
        return fail(move(msg), MsgPack());
    }

    template <typename T>
    T fail(string &&msg, const T err_ret) {
        if (!failed)
            err = std::move(msg);
        failed = true;
        return err_ret;
    }

    uint8_t get_first_byte()
    {
        if(buffer.size() <= i)
        {
            err = "end of buffer.";
            failed = true;
            return 0x00;
        }

        const uint8_t first_byte = buffer[i];
        ++i;
        return first_byte;
    }

    std::nullptr_t parse_invalid(uint8_t) {
        err = "invalid first byte.";
        failed = true;
        return nullptr;
    }

    std::nullptr_t parse_nil(uint8_t) {
        return nullptr;
    }

    bool parse_bool(uint8_t first_byte) {
        return (first_byte == 0xc3);
    }

    template< typename T >
    T parse_arith() {
        EndianConverter<T> converter;
        for( size_t j = 0; j < sizeof(T); ++j )
        {
            converter.value.bytes[j] = buffer[i];
            ++i;
        }

        if(!is_big_endian)
        {
            std::reverse(converter.value.bytes.begin(), converter.value.bytes.end());
        }

        return converter.value.packed;
    }

    template< typename T >
    std::string parse_string_impl(uint8_t, T bytes) {
        std::string res(&(buffer[i]), &(buffer[i+bytes]));
        i += bytes;
        return res;
    }

    template< typename T >
    std::string parse_string(uint8_t first_byte) {
        T const bytes = parse_arith<T>();
        return parse_string_impl<T>(first_byte, bytes);
    }

    template< typename T >
    MsgPack::array parse_array_impl(T bytes) {
        MsgPack::array res;
        for(T j = 0; j < bytes; ++j) {
            res.emplace_back(parse_msgpack(0));
        }

        return res;
    }

    template< typename T >
    MsgPack::array parse_array() {
        T const bytes = parse_arith<T>();
        return parse_array_impl<T>(bytes);
    }

    template< typename T >
    MsgPack::object parse_object_impl(uint8_t, T bytes) {
        MsgPack::object res;
        for(T j = 0; j < bytes; ++j) {
            MsgPack key = parse_msgpack(0);
            MsgPack value = parse_msgpack(0);
            res.insert(std::make_pair(std::move(key), std::move(value)));
        }

        return res;
    }

    template< typename T >
    MsgPack::object parse_object(uint8_t first_byte) {
        T const bytes = parse_arith<T>();
        return parse_object_impl<T>(first_byte, bytes);
    }

    template< typename T >
    MsgPack::binary parse_binary_impl(T bytes) {
        MsgPack::binary res;
        for(T j = 0; j < bytes; ++j) {
            res.push_back(buffer[i]);
            i++;
        }

        return res;
    }

    template< typename T >
    MsgPack::binary parse_binary() {
        T const bytes = parse_arith<T>();
        return parse_binary_impl<T>(bytes);
    }

    template< typename T >
    MsgPack::extension parse_extension() {
        const T bytes = parse_arith<T>();
        const uint8_t type = parse_arith<uint8_t>();
        const MsgPack::binary data =  parse_binary_impl<T>(bytes);
        return std::make_tuple(type, std::move(data));
    }

    uint8_t parse_pos_fixint(uint8_t first_byte) {
        return first_byte;
    }

    MsgPack::object parse_fixobject(uint8_t first_byte) {
        uint8_t const bytes = first_byte & 0x0f;
        return parse_object_impl<uint8_t>(first_byte, bytes);
    }

    MsgPack::array parse_fixarray(uint8_t first_byte) {
        uint8_t const bytes = first_byte & 0x0f;
        return parse_array_impl<uint8_t>(bytes);
    }

    std::string parse_fixstring(uint8_t first_byte) {
        uint8_t const bytes = first_byte & 0x1f;
        return parse_string_impl<uint8_t>(first_byte, bytes);
    }

    int8_t parse_neg_fixint(uint8_t first_byte) {
        return *reinterpret_cast<int8_t*>(&first_byte);
    }

    MsgPack::extension parse_fixext(uint8_t bytes) {
        const uint8_t type = parse_arith<uint8_t>();
        const MsgPack::binary data =  parse_binary_impl<uint8_t>(bytes);
        return std::make_tuple(type, std::move(data));
    }

    using parser_element_type = std::tuple<uint8_t, uint8_t, std::function< MsgPack(MsgPackParser*, uint8_t) > >;
    static const std::vector< parser_element_type > parsers;

    /* parse_msgpack()
     *
     * Parse a JSON object.
     */
    MsgPack parse_msgpack(int depth) {
        if (depth > max_depth) {
            return fail("exceeded maximum nesting depth");
        }

        uint8_t first_byte = get_first_byte();
        if (failed) {
            return MsgPack();
        }

        for( auto const& parser : parsers ) {
            auto beg = std::get<0>(parser);
            auto end = std::get<1>(parser);
            if((beg <= first_byte) && (first_byte <= end)) {
                auto parser_func = std::get<2>(parser);
                return parser_func(this, first_byte);
            }
        }

        return MsgPack();
    }
};

const std::vector< MsgPackParser::parser_element_type > MsgPackParser::parsers {
    MsgPackParser::parser_element_type{ 0x00u, 0x7fu, [](MsgPackParser* that, uint8_t first_byte){ return MsgPack(that->parse_pos_fixint(first_byte)); }},
    MsgPackParser::parser_element_type{ 0x00u, 0x7fu, [](MsgPackParser* that, uint8_t first_byte) -> MsgPack { return MsgPack(that->parse_pos_fixint(first_byte)); }},
    MsgPackParser::parser_element_type{ 0x80u, 0x8fu, [](MsgPackParser* that, uint8_t first_byte) -> MsgPack { return MsgPack(that->parse_fixobject(first_byte)); }},
    MsgPackParser::parser_element_type{ 0x90u, 0x9fu, [](MsgPackParser* that, uint8_t first_byte) -> MsgPack { return MsgPack(that->parse_fixarray(first_byte)); }},
    MsgPackParser::parser_element_type{ 0xa0u, 0xbfu, [](MsgPackParser* that, uint8_t first_byte) -> MsgPack { return MsgPack(that->parse_fixstring(first_byte)); }},
    MsgPackParser::parser_element_type{ 0xc0u, 0xc0u, [](MsgPackParser* that, uint8_t first_byte) -> MsgPack { return MsgPack(that->parse_nil(first_byte)); }},
    MsgPackParser::parser_element_type{ 0xc1u, 0xc1u, [](MsgPackParser* that, uint8_t first_byte) -> MsgPack { return MsgPack(that->parse_invalid(first_byte)); }},
    MsgPackParser::parser_element_type{ 0xc2u, 0xc3u, [](MsgPackParser* that, uint8_t first_byte) -> MsgPack { return MsgPack(that->parse_bool(first_byte)); }},
    MsgPackParser::parser_element_type{ 0xc4u, 0xc4u, [](MsgPackParser* that, uint8_t) -> MsgPack { return MsgPack(that->parse_binary<uint8_t>()); }},
    MsgPackParser::parser_element_type{ 0xc5u, 0xc5u, [](MsgPackParser* that, uint8_t) -> MsgPack { return MsgPack(that->parse_binary<uint16_t>()); }},
    MsgPackParser::parser_element_type{ 0xc6u, 0xc6u, [](MsgPackParser* that, uint8_t) -> MsgPack { return MsgPack(that->parse_binary<uint32_t>()); }},
    MsgPackParser::parser_element_type{ 0xc7u, 0xc7u, [](MsgPackParser* that, uint8_t) -> MsgPack { return MsgPack(that->parse_extension<uint8_t>()); }},
    MsgPackParser::parser_element_type{ 0xc8u, 0xc8u, [](MsgPackParser* that, uint8_t) -> MsgPack { return MsgPack(that->parse_extension<uint16_t>()); }},
    MsgPackParser::parser_element_type{ 0xc9u, 0xc9u, [](MsgPackParser* that, uint8_t) -> MsgPack { return MsgPack(that->parse_extension<uint32_t>()); }},
    MsgPackParser::parser_element_type{ 0xcau, 0xcau, [](MsgPackParser* that, uint8_t) -> MsgPack { return MsgPack(that->parse_arith<float>()); }},
    MsgPackParser::parser_element_type{ 0xcbu, 0xcbu, [](MsgPackParser* that, uint8_t) -> MsgPack { return MsgPack(that->parse_arith<double>()); }},
    MsgPackParser::parser_element_type{ 0xccu, 0xccu, [](MsgPackParser* that, uint8_t) -> MsgPack { return MsgPack(that->parse_arith<uint8_t>()); }},
    MsgPackParser::parser_element_type{ 0xcdu, 0xcdu, [](MsgPackParser* that, uint8_t) -> MsgPack { return MsgPack(that->parse_arith<uint16_t>()); }},
    MsgPackParser::parser_element_type{ 0xceu, 0xceu, [](MsgPackParser* that, uint8_t) -> MsgPack { return MsgPack(that->parse_arith<uint32_t>()); }},
    MsgPackParser::parser_element_type{ 0xcfu, 0xcfu, [](MsgPackParser* that, uint8_t) -> MsgPack { return MsgPack(that->parse_arith<uint64_t>()); }},
    MsgPackParser::parser_element_type{ 0xd0u, 0xd0u, [](MsgPackParser* that, uint8_t) -> MsgPack { return MsgPack(that->parse_arith<int8_t>()); }},
    MsgPackParser::parser_element_type{ 0xd1u, 0xd1u, [](MsgPackParser* that, uint8_t) -> MsgPack { return MsgPack(that->parse_arith<int16_t>()); }},
    MsgPackParser::parser_element_type{ 0xd2u, 0xd2u, [](MsgPackParser* that, uint8_t) -> MsgPack { return MsgPack(that->parse_arith<int32_t>()); }},
    MsgPackParser::parser_element_type{ 0xd3u, 0xd3u, [](MsgPackParser* that, uint8_t) -> MsgPack { return MsgPack(that->parse_arith<int64_t>()); }},
    MsgPackParser::parser_element_type{ 0xd4u, 0xd4u, [](MsgPackParser* that, uint8_t) -> MsgPack { return MsgPack(that->parse_fixext(1)); }},
    MsgPackParser::parser_element_type{ 0xd5u, 0xd5u, [](MsgPackParser* that, uint8_t) -> MsgPack { return MsgPack(that->parse_fixext(2)); }},
    MsgPackParser::parser_element_type{ 0xd6u, 0xd6u, [](MsgPackParser* that, uint8_t) -> MsgPack { return MsgPack(that->parse_fixext(4)); }},
    MsgPackParser::parser_element_type{ 0xd7u, 0xd7u, [](MsgPackParser* that, uint8_t) -> MsgPack { return MsgPack(that->parse_fixext(8)); }},
    MsgPackParser::parser_element_type{ 0xd8u, 0xd8u, [](MsgPackParser* that, uint8_t) -> MsgPack { return MsgPack(that->parse_fixext(16)); }},
    MsgPackParser::parser_element_type{ 0xd9u, 0xd9u, [](MsgPackParser* that, uint8_t first_byte) -> MsgPack { return MsgPack(that->parse_string<uint8_t>(first_byte)); }},
    MsgPackParser::parser_element_type{ 0xdau, 0xdau, [](MsgPackParser* that, uint8_t first_byte) -> MsgPack { return MsgPack(that->parse_string<uint16_t>(first_byte)); }},
    MsgPackParser::parser_element_type{ 0xdbu, 0xdbu, [](MsgPackParser* that, uint8_t first_byte) -> MsgPack { return MsgPack(that->parse_string<uint32_t>(first_byte)); }},
    MsgPackParser::parser_element_type{ 0xdcu, 0xdcu, [](MsgPackParser* that, uint8_t) -> MsgPack { return MsgPack(that->parse_array<uint16_t>()); }},
    MsgPackParser::parser_element_type{ 0xddu, 0xddu, [](MsgPackParser* that, uint8_t) -> MsgPack { return MsgPack(that->parse_array<uint32_t>()); }},
    MsgPackParser::parser_element_type{ 0xdeu, 0xdeu, [](MsgPackParser* that, uint8_t first_byte) -> MsgPack { return MsgPack(that->parse_object<uint16_t>(first_byte)); }},
    MsgPackParser::parser_element_type{ 0xdfu, 0xdfu, [](MsgPackParser* that, uint8_t first_byte) -> MsgPack { return MsgPack(that->parse_object<uint32_t>(first_byte)); }},
    MsgPackParser::parser_element_type{ 0xe0u, 0xffu, [](MsgPackParser* that, uint8_t first_byte) -> MsgPack { return MsgPack(that->parse_neg_fixint(first_byte)); }}
};

}//namespace {

MsgPack MsgPack::parse(const std::string &in, string &err) {
    MsgPackParser parser { in, 0, err, false };
    MsgPack result = parser.parse_msgpack(0);

    return result;
}

// Documented in msgpack.hpp
vector<MsgPack> MsgPack::parse_multi(const string &in,
                                     std::string::size_type &parser_stop_pos,
                                     string &err) {
    MsgPackParser parser { in, 0, err, false };
    parser_stop_pos = 0;
    vector<MsgPack> msgpack_vec;
    while (parser.i != in.size() && !parser.failed) {
        msgpack_vec.push_back(parser.parse_msgpack(0));
        if (!parser.failed)
            parser_stop_pos = parser.i;
    }
    return msgpack_vec;
}

/* * * * * * * * * * * * * * * * * * * *
 * Shape-checking
 */

bool MsgPack::has_shape(const shape & types, string & err) const {
    if (!is_object()) {
        err = "expected MessagePack object";
        return false;
    }

    for (auto & item : types) {
        if ((*this)[item.first].type() != item.second) {
            err = "bad type for " + item.first;
            return false;
        }
    }

    return true;
}

} // namespace msgpack11
