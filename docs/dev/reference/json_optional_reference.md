# JsonOptional — donor reference implementation

Design reference for the tri-state JSON field type described in
`core-hardening-plan.md` Phase 1 and `core-hardening-standards.md` §1a.
Not compiled into any target; the lemon-json `JsonOptional<T>` is adapted
from this with the deltas recorded in the plan (key omission on unset,
flag/value invariant, bitfield portability). The pqxx section is inert in
this tree and kept for completeness.

```cpp

#include <optional>
#include <type_traits>
#include <nlohmann/json.hpp>

template<typename T>
class JsonOptional {
public:
    using type = T;
    union {
        uint8_t _raw_flags = 0; // Initialize to 0
        struct {
            bool is_set  : 1; // Bit 0 (Directly accessible!)
            bool is_null : 1; // Bit 1 (Directly accessible!)
            uint8_t _pad : 6;
        };
    };
    std::optional<T> value; // The actual value, which can be nullopt

    // --- Constructors ---

    JsonOptional() {
        is_set = 0;
        is_null = 1;
        value = std::nullopt;
    }

    JsonOptional(const JsonOptional&) = default;
    JsonOptional(JsonOptional&&) = default;

    // Allow implicit construction from optional
    JsonOptional(const std::optional<T>& opt) : value(opt) {
        is_set = true;
        is_null = (opt == std::nullopt);
    }

    // Allow implicit construction from value T
    JsonOptional(const T& val) : value(val) {
        is_set = true;
        is_null = false;
    }

    // 1. Assign std::nullopt explicitly
    JsonOptional<T>& operator=(std::nullopt_t) {
        is_set = true;
        is_null = true;
        value = std::nullopt;
        return *this;
    }

    // 2. Templated Assignment: Handles T, const T&, optional<T>, and const char*
    // This uses SFINAE to avoid blocking the default copy/move constructors
    template <typename U,
              typename = std::enable_if_t<
                  !std::is_same_v<std::decay_t<U>, JsonOptional<T>>
              >>
    JsonOptional<T>& operator=(U&& newValue) {
        // Forward the assignment to the internal std::optional
        // This lets std::optional handle the logic of converting const char* to std::string
        value = std::forward<U>(newValue);

        is_set = true;
        is_null = !value.has_value();
        return *this;
    }

    // 3. Keep default copy/move assignments so the class can be copied
    JsonOptional& operator=(const JsonOptional&) = default;
    JsonOptional& operator=(JsonOptional&&) = default;

    // --- Helpers ---

    bool is_unset() const { return !is_set; }
    bool has_value() const { return is_set && !is_null; }

    T value_or(T val) {
        if(!is_set || (is_set && is_null))
            return val;
        else
            return this->get();
    }

    T& get() {
        return value.value();
    }

    const T& get() const {
        return value.value();
    }

    void set(T field) {
        is_set = true;
        is_null = false;
        value = field;
    }

    JsonOptional<T>& set(JsonOptional<T> field) {
        is_set = field.is_set;
        is_null = field.is_null;
        value = field.value;
        return *this;
    }

    // --- Conversion Operators ---

    // Allow implicit conversion to the underlying type T
    operator const T&() const {
        return value.value();
    }

    // Allow implicit conversion to std::optional<T>
    operator const std::optional<T>&() const {
        return value;
    }

    // Allow implicit boolean checks
    explicit operator bool() const {
        return value.has_value();
    }

    // --- Access Operators ---

    const T& operator*() const { return *value; }
    T& operator*() { return *value; }

    const T* operator->() const { return &value.value(); }
    T* operator->() { return &value.value(); }
};

template <typename T> struct is_jsonoptional : std::false_type {};
template <typename U> struct is_jsonoptional<JsonOptional<U>> : std::true_type {};

// is_optional
template <typename T> struct is_optional : std::false_type {};
template <typename U> struct is_optional<std::optional<U>> : std::true_type {};
// underlying_type: Gets the 'T' from JsonOptional<T>, optional<T>, or T
template <typename T>
struct underlying_type { using type = T; };

template <typename U>
struct underlying_type<std::optional<U>> { using type = U; };

template <typename U>
struct underlying_type<JsonOptional<U>> { using type = U; };

// The alias that should be used in the macros
template <typename T>
using underlying_type_t = typename underlying_type<T>::type;

#define SAFE_JSON_TO(field) \
    if (nlohmann_json_t.field.is_set && !nlohmann_json_t.field.is_null && nlohmann_json_t.field.value.has_value()) { \
        nlohmann_json_j[#field] = nlohmann_json_t.field.value.value(); \
    } else { \
        nlohmann_json_j[#field] = nullptr; \
    }

#define NLOHMANN_DEFINE_SAFE_INTRUSIVE(Type, ...) \
    friend void to_json(nlohmann::json& nlohmann_json_j, const Type& nlohmann_json_t) { \
        nlohmann_json_j = nlohmann::json::object(); \
        NLOHMANN_JSON_EXPAND( \
            NLOHMANN_JSON_PASTE( \
                SAFE_JSON_TO, \
                __VA_ARGS__ \
            ) \
        ); \
    } \
    friend void from_json(const nlohmann::json& nlohmann_json_j, Type& nlohmann_json_t) { \
        NLOHMANN_JSON_EXPAND( \
            NLOHMANN_JSON_PASTE( \
                SAFE_JSON_FROM_FIELD_HELPER_PASTE_HELPER, \
                __VA_ARGS__ \
            ) \
        ); \
    }

/* --- You must also adjust your helper macros to match the standard signature --- */

/* Intermediate helper: Re-injects the C++ object variable name (nlohmann_json_t) */
#define SAFE_JSON_FROM_FIELD_HELPER_PASTE_HELPER(field) \
    SAFE_JSON_FROM_FIELD_HELPER(nlohmann_json_t, field)

/* Final helper: Uses nlohmann_json_j (the input JSON) and nlohmann_json_t (the C++ object) */
#define SAFE_JSON_FROM_FIELD_HELPER(obj, field) \
    if (nlohmann_json_j.contains(#field)) { \
        if (!nlohmann_json_j.at(#field).is_null()) { \
            nlohmann_json_j.at(#field).get_to(obj.field); \
        } else { \
            obj.field = std::nullopt; \
        } \
    }

namespace nlohmann {
    template <typename T>
    struct adl_serializer<JsonOptional<T>> {
        // --- from_json: How to convert json TO JsonOptional<T> ---
        static void from_json(const json& j, JsonOptional<T>& settable) {
            // This function is ONLY called by the library if the
            // key *actually exists* in the JSON object.
            // Therefore, we can confidently set this to true.
            settable.is_set = true;
            if (j.is_null()) {
                // CRITICAL: do NOT deserialize a null. Previously the value
                // extraction below ran unconditionally (the if/else only set the
                // flag), so JsonOptional<number> hit adl_serializer<T>::from_json
                // on a null and threw type_error.302 ("must be number, but is
                // null"). That aborted the server whenever a nullable numeric
                // column came back null.
                settable.is_null = 1;
                settable.value = std::nullopt;
            } else {
                settable.is_null = 0;
                // Directly extract the value using the serializer
                T temp;
                nlohmann::adl_serializer<T>::from_json(j, temp);
                settable.value = temp;
            }
        }

        static void to_json(json& j, const JsonOptional<T>& settable) {
            if (settable.is_set && !settable.is_null && settable.value.has_value()) {
                j = settable.value.value();
            } else {
                j = nullptr;
            }
        }
    };
} // namespace nlohmann

// Specialization for nlohmann::json to avoid infinite recursion
namespace nlohmann {
    template <>
    struct adl_serializer<JsonOptional<nlohmann::json>> {
        static void from_json(const json& j, JsonOptional<nlohmann::json>& settable) {
            settable.is_set = true;
            settable.is_null = j.is_null();
            settable.value = j;
        }
        static void to_json(json& j, const JsonOptional<nlohmann::json>& settable) {
            if (settable.is_set && !settable.is_null && settable.value.has_value()) {
                j = settable.value.value();
            } else {
                j = nullptr;
            }
        }
    };
} // namespace nlohmann

#ifdef KANA_JSONOPTIONAL_HAS_PQXX
namespace pqxx {
  template<typename T>
  struct nullness<JsonOptional<T>> {
    static constexpr bool has_null = true;
    static constexpr bool always_null = false;

    static bool is_null(const JsonOptional<T> &obj) noexcept {
      return !obj.has_value();
    }

    static JsonOptional<T> null() {
      JsonOptional<T> j_opt;
      j_opt = std::nullopt;
      return j_opt;
    }
  };

  template<typename T>
  struct string_traits<JsonOptional<T>> {
    static constexpr bool has_null() { return true; }

    [[nodiscard]] static JsonOptional<T> from_string(std::string_view text) {
      auto opt = string_traits<std::optional<T>>::from_string(text);
      return JsonOptional<T>(opt);
    }

    static zview to_string(const JsonOptional<T>& obj) {
      return string_traits<std::optional<T>>::to_string(obj.value);
    }

    static char *into_buf(char *begin, char *end, const JsonOptional<T> &obj) {
      return string_traits<std::optional<T>>::into_buf(begin, end, obj.value);
    }

    static std::size_t size_buffer(const JsonOptional<T> &obj) noexcept {
      return string_traits<std::optional<T>>::size_buffer(obj.value);
    }
  };
}
#endif // KANA_JSONOPTIONAL_HAS_PQXX
```
