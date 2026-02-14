#pragma once

// magic_enum configuration (Parameter keys are bytes: 0..255)
#define MAGIC_ENUM_RANGE_MIN 0
#define MAGIC_ENUM_RANGE_MAX 255

#include "global.hpp"

#include <array>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <format>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>
#include <memory>
#include <luxon/common_codes.hpp>
#include <luxon/ser_types.hpp>
#include <magic_enum/magic_enum.hpp>

namespace server::models {
namespace ser = luxon::ser;

// Compile-time helpers / concepts

template <typename T, typename Variant> struct IsInVariantImpl : std::false_type {};
template <typename T, typename... Ts> struct IsInVariantImpl<T, std::variant<Ts...>> : std::bool_constant<(std::is_same_v<T, Ts> || ...)> {};

template <typename T, typename Variant>
concept IsVariantMember = IsInVariantImpl<T, Variant>::value;

template <typename E>
concept ByteKeyEnum = std::is_enum_v<E> && std::is_integral_v<std::underlying_type_t<E>>;

template <typename E, E V>
concept ValidByteKeyEnumerator = ByteKeyEnum<E> && magic_enum::enum_contains(V);

// Compile-time string wrapper (for default std::string values)
template <std::size_t N> struct StringLiteral {
    constexpr StringLiteral(const char (&str)[N]) {
        for (std::size_t i = 0; i != N; ++i)
            value[i] = str[i];
    }

    char value[N]{};
    static constexpr std::size_t length = N - 1;

    constexpr operator std::string_view() const { return {value, length}; }
    constexpr operator const char *() const { return value; }
};

// Default providers

struct NoDefault {
    static constexpr bool provided = false;
};

struct DefaultInit {
    static constexpr bool provided = true;
    template <typename T> static constexpr T get() { return T{}; }
};

template <auto V> struct DefaultConst {
    static constexpr bool provided = true;
    template <typename T> static constexpr T get() { return V; }
};

template <StringLiteral Lit> struct DefaultString {
    static constexpr bool provided = true;
    template <typename T> static T get() { return T(std::string_view(Lit)); }
};

namespace detail {
template <class T> using remove_cvref_t = std::remove_cvref_t<T>;

// Wire type stored inside ser::Value::VariantType.
// - Enums: underlying integral type.
// - Otherwise: T itself.
template <class T> struct WireType {
    using type = T;
};
template <class E>
    requires std::is_enum_v<E>
struct WireType<E> {
    using type = std::underlying_type_t<E>;
};
template <class T> using wire_type_t = typename WireType<remove_cvref_t<T>>::type;

template <class T> inline constexpr bool is_enum_v = std::is_enum_v<remove_cvref_t<T>>;

// Convert from a decoded "wire" scalar into a ValueT (including enum casting).
template <class ValueT, class U> constexpr ValueT to_value(U&& u) {
    if constexpr (std::is_enum_v<ValueT>) {
        return static_cast<ValueT>(u);
    } else {
        return ValueT(std::forward<U>(u));
    }
}

// Default provider compatibility:
// - If no default: ok.
// - Else DefaultProvider::get() must exist and be convertible to ValueT,
//   or (for enums) convertible to ValueT OR its wire type.
template <class ValueT, class DefaultProvider>
concept DefaultCompatible = (!DefaultProvider::provided) || (requires {
                                DefaultProvider::template get<ValueT>();
                            } && (is_enum_v<ValueT> ? (std::is_convertible_v<decltype(DefaultProvider::template get<ValueT>()), ValueT> ||
                                                       std::is_convertible_v<decltype(DefaultProvider::template get<ValueT>()), wire_type_t<ValueT>>)
                                                    : std::is_convertible_v<decltype(DefaultProvider::template get<ValueT>()), ValueT>));

// Heuristic: in views, store references for non-trivially-copyable types
template <class T> inline constexpr bool view_by_ref_v = (!std::is_trivially_copyable_v<remove_cvref_t<T>>) && (!std::is_enum_v<remove_cvref_t<T>>);

// Pretty names for ser::Value payload types
template <typename T> constexpr std::string_view value_type_name() {
    if constexpr (std::is_same_v<T, std::monostate>)
        return "null";
    else if constexpr (std::is_same_v<T, bool>)
        return "bool";
    else if constexpr (std::is_same_v<T, std::uint8_t>)
        return "byte";
    else if constexpr (std::is_same_v<T, std::int16_t>)
        return "int16";
    else if constexpr (std::is_same_v<T, std::int32_t>)
        return "int32";
    else if constexpr (std::is_same_v<T, std::int64_t>)
        return "int64";
    else if constexpr (std::is_same_v<T, float>)
        return "float";
    else if constexpr (std::is_same_v<T, double>)
        return "double";
    else if constexpr (std::is_same_v<T, std::string>)
        return "string";
    else if constexpr (std::is_same_v<T, ser::ByteArray>)
        return "byte[]";
    else if constexpr (std::is_same_v<T, std::vector<bool>>)
        return "bool[]";
    else if constexpr (std::is_same_v<T, std::vector<std::int16_t>>)
        return "int16[]";
    else if constexpr (std::is_same_v<T, std::vector<std::int32_t>>)
        return "int32[]";
    else if constexpr (std::is_same_v<T, std::vector<std::int64_t>>)
        return "int64[]";
    else if constexpr (std::is_same_v<T, std::vector<float>>)
        return "float[]";
    else if constexpr (std::is_same_v<T, std::vector<double>>)
        return "double[]";
    else if constexpr (std::is_same_v<T, std::vector<std::string>>)
        return "string[]";
    else if constexpr (std::is_same_v<T, ser::ObjectArray>)
        return "object[]";
    else if constexpr (std::is_same_v<T, ser::Dictionary>)
        return "dictionary<byte,value>";
    else if constexpr (std::is_same_v<T, ser::HashtablePtr>)
        return "hashtable<value,value>";
    else if constexpr (std::is_same_v<T, ser::RawCustomValue>)
        return "custom";
    else
        return "unknown";
}

inline std::string_view actual_type_name(const ser::Value& v) {
    return std::visit(
        [](const auto& x) -> std::string_view {
            using T = std::decay_t<decltype(x)>;
            return value_type_name<T>();
        },
        v.value);
}

template <class T> inline std::string expected_type_string() {
    if constexpr (std::is_enum_v<T>) {
        using U = std::underlying_type_t<T>;
        return std::format("enum<{}> (encoded as {})", magic_enum::enum_type_name<T>(), value_type_name<U>());
    } else {
        return std::string(value_type_name<T>());
    }
}

inline ser::OperationResponseMessage make_decode_error(std::uint8_t op_code, std::string message) {
    ser::OperationResponseMessage resp{};
    resp.operation_code = op_code;
    resp.return_code = static_cast<std::int16_t>(ErrorCodes::Core::OperationInvalid);
    resp.debug_message = std::move(message);
    resp.parameters = {};
    return resp;
}

// Static storage for defaults used by ModelView (references must outlive the view)
template <class P> inline const typename P::value_type& view_default_object() {
    static const typename P::value_type v = to_value<typename P::value_type>(P::default_provider::template get<typename P::wire_type>());
    return v;
}
template <class P> inline const typename P::value_type& view_empty_object() {
    static const typename P::value_type v{};
    return v;
}

template <class P> inline std::string param_display_name() {
    // Prefer the enum symbolic name (stable, readable)
    const std::string_view n = magic_enum::enum_name(P::key);
    if (!n.empty())
        return std::string(n);
    return std::format("key={}", static_cast<std::uint32_t>(P::wire_key));
}

template <class P> inline ser::OperationResponseMessage make_missing_error(std::uint8_t op_code) {
    const std::string expected = expected_type_string<typename P::value_type>();
    return make_decode_error(op_code, std::format("Missing required parameter '{}' (wire_key={}), expected type {}.", param_display_name<P>(),
                                                  static_cast<std::uint32_t>(P::wire_key), expected));
}

template <class P> inline ser::OperationResponseMessage make_required_null_error(std::uint8_t op_code) {
    const std::string expected = expected_type_string<typename P::value_type>();
    return make_decode_error(op_code, std::format("Invalid null for required parameter '{}' (wire_key={}), expected type {}.", param_display_name<P>(),
                                                  static_cast<std::uint32_t>(P::wire_key), expected));
}

template <class P> inline ser::OperationResponseMessage make_type_mismatch_error(std::uint8_t op_code, const ser::Value& src) {
    const std::string_view expected = value_type_name<typename P::wire_type>();
    const std::string_view got = actual_type_name(src);
    return make_decode_error(op_code, std::format("Type mismatch for parameter '{}' (wire_key={}): expected {}, got {}.", param_display_name<P>(),
                                                  static_cast<std::uint32_t>(P::wire_key), expected, got));
}

template <class P> inline ser::OperationResponseMessage make_invalid_enum_value_error(std::uint8_t op_code, typename P::wire_type raw) {
    return make_decode_error(op_code, std::format("Invalid enum value for parameter '{}' (wire_key={}): value {} is not valid in {}.", param_display_name<P>(),
                                                  static_cast<std::uint32_t>(P::wire_key), static_cast<std::uint64_t>(raw),
                                                  magic_enum::enum_type_name<typename P::value_type>()));
}
} // namespace detail

// Parameter specification

template <class ValueT, auto Key, bool Optional = false, class DefaultProvider = NoDefault>
    requires std::is_enum_v<decltype(Key)> && std::same_as<ValueT, std::remove_cvref_t<ValueT>> &&
             IsVariantMember<detail::wire_type_t<ValueT>, ser::Value::VariantType> && detail::DefaultCompatible<ValueT, DefaultProvider>
struct Parameter {
    using value_type = ValueT;
    using wire_type = detail::wire_type_t<ValueT>;
    using key_enum = decltype(Key);

    static constexpr key_enum key = Key;
    static constexpr std::uint8_t wire_key = static_cast<std::uint8_t>(Key);

    static constexpr bool optional = Optional;

    using stored_type = std::conditional_t<optional, std::optional<value_type>, value_type>;

    using default_provider = DefaultProvider;
    static constexpr bool has_default = DefaultProvider::provided;
};

template <typename T> struct IsParameterSpecImpl : std::false_type {};
template <class V, auto K, bool Opt, class Def> struct IsParameterSpecImpl<Parameter<V, K, Opt, Def>> : std::true_type {};

template <typename T>
concept ParameterSpec = IsParameterSpecImpl<std::remove_cvref_t<T>>::value;

namespace detail {
// Select the ParameterSpec in Ps... whose wire_key matches Key (cast to uint8_t).
template <auto Key, ParameterSpec... Ps> struct ParamByKey;

template <auto Key> struct ParamByKey<Key> {
    using type = void;
};

template <auto Key, ParameterSpec P0, ParameterSpec... Rest> struct ParamByKey<Key, P0, Rest...> {
    static constexpr std::uint8_t k = static_cast<std::uint8_t>(Key);
    using type = std::conditional_t<(P0::wire_key == k), P0, typename ParamByKey<Key, Rest...>::type>;
};

template <auto Key, ParameterSpec... Ps> using param_by_key_t = typename ParamByKey<Key, Ps...>::type;

template <auto Key, ParameterSpec... Ps> inline constexpr std::size_t key_match_count_v = ((Ps::wire_key == static_cast<std::uint8_t>(Key)) + ... + 0);

template <auto Key, typename... Ps>
concept UniqueKeyInModel = requires {
    requires(ParameterSpec<Ps> && ...);
    requires(detail::key_match_count_v<Key, Ps...> == 1);
    requires(!std::is_void_v<detail::param_by_key_t<Key, Ps...>>);
};
} // namespace detail

// Forward declaration
template <ParameterSpec... Ps> struct Model;

// ModelView (decoded, read-only)
template <ParameterSpec P> struct ViewField {
    using param = P;
    using value_type = typename P::value_type;

    static constexpr bool by_ref = detail::view_by_ref_v<value_type>;

    using required_storage_t = std::conditional_t<by_ref, std::reference_wrapper<const value_type>, value_type>;

    using optional_storage_t = std::conditional_t<by_ref, std::optional<std::reference_wrapper<const value_type>>, std::optional<value_type>>;

    using stored_type = std::conditional_t<P::optional, optional_storage_t, required_storage_t>;

    stored_type value;

    // Helper to determine the correct initial state
    static stored_type make_default() {
        if constexpr (P::optional) {
            if constexpr (P::has_default) {
                if constexpr (by_ref)
                    return std::cref(detail::view_default_object<P>());
                else
                    return detail::to_value<value_type>(P::default_provider::template get<typename P::wire_type>());
            } else {
                return std::nullopt;
            }
        } else {
            if constexpr (P::has_default) {
                if constexpr (by_ref)
                    return std::cref(detail::view_default_object<P>());
                else
                    return detail::to_value<value_type>(P::default_provider::template get<typename P::wire_type>());
            } else {
                if constexpr (by_ref)
                    return std::cref(detail::view_empty_object<P>());
                else
                    return value_type{};
            }
        }
    }

    // Initialize in the constructor list
    ViewField() : value(make_default()) {}

    void reset_to_default() { value = make_default(); }
};

template <ParameterSpec... Ps> struct ModelView : private ViewField<Ps>... {
    ModelView() = default;

    void reset_defaults() { (ViewField<Ps>::reset_to_default(), ...); }

private:
    template <ParameterSpec P>
        requires((std::is_same_v<P, Ps>) || ...)
    typename ViewField<P>::stored_type& storage() {
        return ViewField<P>::value;
    }

    template <ParameterSpec P>
        requires((std::is_same_v<P, Ps>) || ...)
    const typename ViewField<P>::stored_type& storage() const {
        return ViewField<P>::value;
    }

public:
    // Read-only access:
    // - required: const value_type&
    // - optional by-value: const std::optional<value_type>&
    // - optional by-ref: const value_type*
    template <ParameterSpec P> decltype(auto) get() const {
        if constexpr (P::optional) {
            if constexpr (ViewField<P>::by_ref) {
                return storage<P>() ? std::addressof(storage<P>()->get()) : nullptr;
            } else {
                return storage<P>();
            }
        } else {
            if constexpr (ViewField<P>::by_ref)
                return storage<P>().get();
            else
                return storage<P>();
        }
    }

    template <auto Key>
        requires(detail::UniqueKeyInModel<Key, Ps...>)
    decltype(auto) get() const {
        using P = detail::param_by_key_t<Key, Ps...>;
        return this->template get<P>();
    }

    // Convert a view into an owning model (copies out).
    [[nodiscard]]
    Model<Ps...> materialize() const {
        Model<Ps...> out{};

        auto copy_one = [&]<ParameterSpec P>() {
            if constexpr (P::optional) {
                auto& dst = out.template get<P>(); // std::optional<value_type>&
                if constexpr (ViewField<P>::by_ref) {
                    const auto& opt_ref = get<P>();
                    if (opt_ref)
                        dst = *opt_ref;
                    else
                        dst = std::nullopt;
                } else {
                    dst = get<P>();
                }
            } else {
                out.template get<P>() = get<P>();
            }
        };

        (copy_one.template operator()<Ps>(), ...);
        return out;
    }

private:
    static constexpr std::size_t param_count = sizeof...(Ps);
    using SeenArray = std::array<bool, param_count>;
    using ErrorArray = std::array<std::optional<ser::OperationResponseMessage>, param_count>;

    using Handler = void (*)(ModelView *, const ser::Value&, std::uint8_t op_code, SeenArray&, ErrorArray&);

    template <ParameterSpec P, std::size_t I> void decode_one_into(const ser::Value& src, std::uint8_t op_code, SeenArray& seen, ErrorArray& errs) {
        seen[I] = true;

        // Null handling
        if (src.is_null()) {
            if constexpr (P::optional)
                storage<P>().reset();
            else if constexpr (!P::has_default)
                errs[I] = detail::make_required_null_error<P>(op_code);
            return;
        }

        // Type check
        const auto *p = src.get_ptr<typename P::wire_type>();
        if (!p) {
            errs[I] = detail::make_type_mismatch_error<P>(op_code, src);
            return;
        }

        // Enum validation + assignment
        if constexpr (std::is_enum_v<typename P::value_type>) {
            const auto enum_val = detail::to_value<typename P::value_type>(*p);
            if (!magic_enum::enum_contains<typename P::value_type>(enum_val)) {
                errs[I] = detail::make_invalid_enum_value_error<P>(op_code, *p);
                return;
            }

            storage<P>() = enum_val;

            return;
        } else {
            // Non-enum: by-ref or by-value
            if constexpr (ViewField<P>::by_ref)
                storage<P>() = std::cref(*p);
            else
                storage<P>() = *p;
        }
    }

    template <ParameterSpec P, std::size_t I>
    static void handler(ModelView *self, const ser::Value& src, std::uint8_t op_code, SeenArray& seen, ErrorArray& errs) {
        self->template decode_one_into<P, I>(src, op_code, seen, errs);
    }

    static constexpr std::array<Handler, 256> make_handlers() {
        std::array<Handler, 256> h{};
        h.fill(nullptr);

        [&]<std::size_t... Is>(std::index_sequence<Is...>) {
            ((h[Ps::wire_key] = &ModelView::template handler<Ps, Is>), ...);
        }(std::index_sequence_for<Ps...>{});

        return h;
    }

    static inline constexpr std::array<Handler, 256> handlers_ = make_handlers();

public:
    [[nodiscard]]
    static std::expected<ModelView, ser::OperationResponseMessage> decode(const ser::OperationRequestMessage& req) {
        ModelView view{};
        view.reset_defaults();

        SeenArray seen{};
        seen.fill(false);

        ErrorArray errs{};
        errs.fill(std::nullopt);

        // Single pass: decode known params, ignore unknowns
        for (const auto& [k, v] : req.parameters) {
            if (const auto h = handlers_[k]) {
                h(&view, v, req.operation_code, seen, errs);
            }
        }

        // Per-parameter validation:
        // - decode/type/null/enum errors win
        // - then missing required parameters (no default and not optional)
        std::optional<ser::OperationResponseMessage> fail;

        auto check_one = [&]<std::size_t I, ParameterSpec P>() -> bool {
            if (errs[I].has_value()) {
                fail = *errs[I];
                return false;
            }

            if (!seen[I]) {
                if constexpr (!(P::optional || P::has_default)) {
                    fail = detail::make_missing_error<P>(req.operation_code);
                    return false;
                }
            }

            return true;
        };

        const bool ok = [&]<std::size_t... Is>(std::index_sequence<Is...>) {
            return (check_one.template operator()<Is, Ps>() && ...);
        }(std::index_sequence_for<Ps...>{});

        if (!ok)
            return std::unexpected(*fail);
        return view;
    }
};

// Model (owning)
template <ParameterSpec P> struct Field {
    using value_type = typename P::value_type;
    using stored_type = typename P::stored_type;

    stored_type value{};

    Field() { reset_to_default(); }

    void reset_to_default() {
        if constexpr (P::optional) {
            if constexpr (P::has_default) {
                value = detail::to_value<value_type>(P::default_provider::template get<typename P::wire_type>());
            } else {
                value = std::nullopt;
            }
        } else {
            if constexpr (P::has_default) {
                value = detail::to_value<value_type>(P::default_provider::template get<typename P::wire_type>());
            } else {
                value = value_type{};
            }
        }
    }
};

template <ParameterSpec... Ps> struct Model : private Field<Ps>... {
    using view_type = ModelView<Ps...>;

    Model() = default;

    void reset_defaults() { (Field<Ps>::reset_to_default(), ...); }

    template <ParameterSpec P>
        requires((std::is_same_v<P, Ps>) || ...)
    typename P::stored_type& get() {
        return Field<P>::value;
    }

    template <ParameterSpec P>
        requires((std::is_same_v<P, Ps>) || ...)
    const typename P::stored_type& get() const {
        return Field<P>::value;
    }

    template <auto Key>
        requires(detail::UniqueKeyInModel<Key, Ps...>)
    decltype(auto) get() {
        using P = detail::param_by_key_t<Key, Ps...>;
        return this->template get<P>();
    }

    template <auto Key>
        requires(detail::UniqueKeyInModel<Key, Ps...>)
    decltype(auto) get() const {
        using P = detail::param_by_key_t<Key, Ps...>;
        return this->template get<P>();
    }

    [[nodiscard]]
    static std::expected<view_type, ser::OperationResponseMessage> decode(const ser::OperationRequestMessage& req) {
        return view_type::decode(req);
    }

    [[nodiscard]]
    std::expected<ser::ParameterList, std::string> encode() const {
        ser::ParameterList parameters{};
        std::optional<std::string> err;

        auto put_one = [&]<ParameterSpec P>() {
            if (err)
                return;

            auto encode_one_value = [&](const typename P::value_type& v) {
                if constexpr (std::is_enum_v<typename P::value_type>) {
                    if (!magic_enum::enum_contains<typename P::value_type>(v)) {
                        const auto raw = static_cast<typename P::wire_type>(v);
                        err = std::format("Cannot encode parameter '{}' (wire_key={}): enum value {} is not valid in {}.", detail::param_display_name<P>(),
                                          static_cast<std::uint32_t>(P::wire_key), static_cast<std::uint64_t>(raw),
                                          magic_enum::enum_type_name<typename P::value_type>());
                        return;
                    }
                    parameters[P::wire_key] = ser::Value{static_cast<typename P::wire_type>(v)};
                } else {
                    parameters[P::wire_key] = ser::Value{v};
                }
            };

            if constexpr (P::optional) {
                const auto& opt = this->template get<P>();
                if (opt.has_value())
                    encode_one_value(*opt);
            } else {
                encode_one_value(this->template get<P>());
            }
        };

        (put_one.template operator()<Ps>(), ...);

        if (err)
            return std::unexpected(*err);
        return parameters;
    }
};

// Model composition utilities

template <typename ModelT, ParameterSpec... NewPs> struct ExtendModel;

template <ParameterSpec... OldPs, ParameterSpec... NewPs> struct ExtendModel<Model<OldPs...>, NewPs...> {
    using Type = Model<OldPs..., NewPs...>;
};

template <typename ModelT, ParameterSpec... NewPs> using ExtendedModel = typename ExtendModel<ModelT, NewPs...>::Type;

template <typename ModelA, typename ModelB> struct MergeModels;

template <ParameterSpec... PsA, ParameterSpec... PsB> struct MergeModels<Model<PsA...>, Model<PsB...>> {
    using Type = Model<PsA..., PsB...>;
};

template <typename ModelA, typename ModelB> using MergedModel = typename MergeModels<ModelA, ModelB>::Type;

template <typename... Models> struct MergeMany;

template <typename T> struct MergeMany<T> {
    using Type = T;
};

template <typename Head, typename... Tail> struct MergeMany<Head, Tail...> {
    using Type = MergedModel<Head, typename MergeMany<Tail...>::Type>;
};

template <typename... Models> using MergedModels = typename MergeMany<Models...>::Type;
} // namespace server::models
