#pragma once

#define MAGIC_ENUM_RANGE_MIN 0
#define MAGIC_ENUM_RANGE_MAX 255

#include "global.hpp"

#include <array>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include <luxon/common_codes.hpp>
#include <luxon/ser_types.hpp>
#include <magic_enum/magic_enum.hpp>

namespace server {
namespace models {
namespace ser = luxon::ser;

// Concepts
template <typename T, typename Variant> struct IsInVariantImpl : std::false_type {};
template <typename T, typename... Ts> struct IsInVariantImpl<T, std::variant<Ts...>> : std::bool_constant<(std::is_same_v<T, Ts> || ...)> {};
template <typename T, typename Variant>
concept IsVariantMember = IsInVariantImpl<T, Variant>::value;

template <typename E, auto V>
concept ValidEnumerator = std::is_enum_v<E> && magic_enum::enum_contains<E>(V);

// Compile-time string wrapper
template <size_t N> struct StringLiteral {
    constexpr StringLiteral(const char (&str)[N]) {
        for (size_t it = 0; it != N; ++it)
            value[it] = str[it];
    }

    char value[N]{};
    constexpr static size_t length = N - 1;

    constexpr operator std::string_view() const { return {value, length}; }
    constexpr operator const char *() const { return value; }
};

// Default providers
struct NoDefault {
    static constexpr bool provided = false;
};

template <auto V> struct DefaultConst {
    static constexpr bool provided = true;
    static decltype(auto) get() { return V; }
};

// For std::string defaults using a compile-time literal
template <StringLiteral Lit> struct DefaultString {
    static constexpr bool provided = true;
    static std::string get() { return std::string(std::string_view(Lit)); }
};

namespace detail {

// "Wire" type stored inside ser::Value::VariantType for a given ValueT.
// - For enums: underlying integral type.
// - Otherwise: ValueT itself.
template <class T> struct WireType {
    using type = T;
};
template <class E>
    requires std::is_enum_v<E>
struct WireType<E> {
    using type = std::underlying_type_t<E>;
};
template <class T> using wire_type_t = typename WireType<std::remove_cvref_t<T>>::type;

template <class T> constexpr bool is_value_enum_v = std::is_enum_v<std::remove_cvref_t<T>>;

// Convert something to stored value_type (including enum via static_cast).
template <class T, class U> constexpr T to_value(U&& u) {
    if constexpr (std::is_enum_v<T>) {
        return static_cast<T>(u);
    } else {
        return T{std::forward<U>(u)};
    }
}

template <class ValueT, class DefaultProvider>
concept DefaultCompatible =
    !DefaultProvider::provided ||
    (requires { DefaultProvider::get(); } && (is_value_enum_v<ValueT> ? (std::is_constructible_v<wire_type_t<ValueT>, decltype(DefaultProvider::get())> ||
                                                                         std::is_constructible_v<ValueT, decltype(DefaultProvider::get())>)
                                                                      : std::is_constructible_v<ValueT, decltype(DefaultProvider::get())>));

template <typename T> constexpr std::string_view value_type_name() {
    if constexpr (std::is_same_v<T, std::monostate>)
        return "null";
    else if constexpr (std::is_same_v<T, bool>)
        return "bool";
    else if constexpr (std::is_same_v<T, uint8_t>)
        return "byte";
    else if constexpr (std::is_same_v<T, int16_t>)
        return "int16";
    else if constexpr (std::is_same_v<T, int32_t>)
        return "int32";
    else if constexpr (std::is_same_v<T, int64_t>)
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
    else if constexpr (std::is_same_v<T, std::vector<int16_t>>)
        return "int16[]";
    else if constexpr (std::is_same_v<T, std::vector<int32_t>>)
        return "int32[]";
    else if constexpr (std::is_same_v<T, std::vector<int64_t>>)
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

template <class T> inline std::string expected_type_string() {
    if constexpr (std::is_enum_v<T>) {
        using U = std::underlying_type_t<T>;
        return std::format("enum<{}> (encoded as {})", magic_enum::enum_type_name<T>(), value_type_name<U>());
    } else {
        return std::string(value_type_name<T>());
    }
}

inline std::string_view actual_type_name(const ser::Value& v) {
    return std::visit(
        [](const auto& x) -> std::string_view {
            using T = std::decay_t<decltype(x)>;
            return value_type_name<T>();
        },
        v.value);
}

inline ser::OperationResponseMessage make_decode_error(uint8_t op_code, std::string message) {
    ser::OperationResponseMessage resp{};
    resp.operation_code = op_code;
    resp.return_code = static_cast<int16_t>(ErrorCodes::Core::OperationInvalid);
    resp.debug_message = std::move(message);
    resp.parameters = {};
    return resp;
}
} // namespace detail

// Parameter specification
template <class ValueT, typename EnumT, uint8_t key, bool Optional = false, typename DefaultProvider = NoDefault>
    requires ValidEnumerator<EnumT, key> && IsVariantMember<detail::wire_type_t<ValueT>, ser::Value::VariantType> &&
             detail::DefaultCompatible<ValueT, DefaultProvider>
struct Parameter {
    using value_type = ValueT;
    using wire_type = detail::wire_type_t<ValueT>;
    using enum_type = EnumT;
    static constexpr uint8_t param_key = key;

    static constexpr bool optional = Optional;
    using stored_type = std::conditional_t<optional, std::optional<value_type>, value_type>;

    using default_provider = DefaultProvider;
    static constexpr bool has_default = DefaultProvider::provided;
};

template <typename T> struct IsParameterSpecImpl : std::false_type {};
template <typename V, typename E, uint8_t K, bool Opt, typename Def> struct IsParameterSpecImpl<Parameter<V, E, K, Opt, Def>> : std::true_type {};
template <typename T>
concept ParameterSpec = IsParameterSpecImpl<std::remove_cvref_t<T>>::value;

// Storage for a single parameter
template <ParameterSpec P> struct Field {
    using param = P;
    using value_type = typename P::value_type;
    using stored_type = typename P::stored_type;

    stored_type value{};

    void reset_to_default() {
        if constexpr (P::optional) {
            if constexpr (P::has_default)
                value = detail::to_value<value_type>(P::default_provider::get());
            else
                value = std::nullopt;
        } else {
            if constexpr (P::has_default)
                value = detail::to_value<value_type>(P::default_provider::get());
            else
                value = value_type{};
        }
    }

    Field() { reset_to_default(); }
};

// Model: one Field<> base subobject per Parameter<>
template <ParameterSpec... Ps> struct Model : private Field<Ps>... {
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

private:
    static constexpr std::size_t param_count = sizeof...(Ps);
    using SeenArray = std::array<bool, param_count>;
    using ErrorArray = std::array<std::optional<ser::OperationResponseMessage>, param_count>;
    using Handler = void (*)(Model *, const ser::Value&, uint8_t, SeenArray&, ErrorArray&);

    template <ParameterSpec P> static std::string param_name_string() {
        return std::string(magic_enum::enum_name(static_cast<typename P::enum_type>(P::param_key)));
    }

    template <ParameterSpec P> static ser::OperationResponseMessage make_missing_error(uint8_t op_code) {
        const std::string param_name = param_name_string<P>();
        const std::string_view key_name = magic_enum::enum_name(static_cast<typename P::enum_type>(P::param_key));
        const std::string expected = detail::expected_type_string<typename P::value_type>();

        return detail::make_decode_error(op_code, std::format("Missing required parameter '{}' (key={}), expected type {}.", param_name, key_name, expected));
    }

    template <ParameterSpec P> static ser::OperationResponseMessage make_required_null_error(uint8_t op_code) {
        const std::string param_name = param_name_string<P>();
        const std::string_view key_name = magic_enum::enum_name(static_cast<typename P::enum_type>(P::param_key));
        const std::string expected = detail::expected_type_string<typename P::value_type>();

        return detail::make_decode_error(op_code,
                                         std::format("Invalid null for required parameter '{}' (key={}), expected type {}.", param_name, key_name, expected));
    }

    template <ParameterSpec P> static ser::OperationResponseMessage make_type_mismatch_error(uint8_t op_code, const ser::Value& src) {
        const std::string param_name = param_name_string<P>();
        const std::string_view key_name = magic_enum::enum_name(static_cast<typename P::enum_type>(P::param_key));
        const std::string_view expected = detail::value_type_name<typename P::wire_type>();
        const std::string_view got = detail::actual_type_name(src);

        return detail::make_decode_error(op_code,
                                         std::format("Type mismatch for parameter '{}' (key={}): expected {}, got {}.", param_name, key_name, expected, got));
    }

    template <ParameterSpec P> static ser::OperationResponseMessage make_invalid_enum_value_error(uint8_t op_code, typename P::wire_type raw) {
        const std::string param_name = param_name_string<P>();
        const std::string_view key_name = magic_enum::enum_name(static_cast<typename P::enum_type>(P::param_key));

        // We purposely show the raw wire value; that's what the client sent.
        return detail::make_decode_error(op_code, std::format("Invalid enum value for parameter '{}' (key={}): value {} is not a valid {}.", param_name,
                                                              key_name, static_cast<std::uint64_t>(raw), magic_enum::enum_type_name<typename P::value_type>()));
    }

    template <ParameterSpec P, std::size_t I> void decode_value_into(const ser::Value& src, uint8_t op_code, SeenArray& seen, ErrorArray& errs) {
        seen[I] = true;

        auto& dst = get<P>();

        // Explicit null
        if (src.is_null()) {
            if constexpr (P::optional) {
                dst = std::nullopt;
            } else {
                errs[I] = make_required_null_error<P>(op_code);
            }
            return;
        }

        // Type check + assign (decode from wire_type, then cast into value_type)
        if (const auto *p = src.get_ptr<typename P::wire_type>()) {
            if constexpr (std::is_enum_v<typename P::value_type>) {
                const auto enum_val = detail::to_value<typename P::value_type>(*p);
                if (!magic_enum::enum_contains<typename P::value_type>(enum_val)) {
                    errs[I] = make_invalid_enum_value_error<P>(op_code, *p);
                    return;
                }

                if constexpr (P::optional)
                    dst = enum_val;
                else
                    dst = enum_val;

                return;
            } else {
                if constexpr (P::optional)
                    dst = *p;
                else
                    dst = *p;
                return;
            }
        }

        errs[I] = make_type_mismatch_error<P>(op_code, src);
    }

    template <ParameterSpec P, std::size_t I> static void handler(Model *self, const ser::Value& src, uint8_t op_code, SeenArray& seen, ErrorArray& errs) {
        self->template decode_value_into<P, I>(src, op_code, seen, errs);
    }

    static constexpr std::array<Handler, 256> make_handlers() {
        std::array<Handler, 256> h{};
        h.fill(nullptr);

        [&]<std::size_t... Is>(std::index_sequence<Is...>) { ((h[Ps::param_key] = &Model::template handler<Ps, Is>), ...); }(std::index_sequence_for<Ps...>{});

        return h;
    }

    static constexpr std::array<Handler, 256> handlers_ = make_handlers();

public:
    // Decode parameters from an OperationRequestMessage into this model
    std::expected<void, ser::OperationResponseMessage> decode(const ser::OperationRequestMessage& req) {
        // Start from defaults every time
        reset_defaults();

        SeenArray seen{};
        seen.fill(false);

        ErrorArray errs{};
        errs.fill(std::nullopt);

        // Single pass over the input ParameterList; decode known params, ignore unknowns
        for (const auto& [key, val] : req.parameters) {
            if (const auto h = handlers_[key]) {
                h(this, val, req.operation_code, seen, errs);
            }
        }

        // Type/Null/Enum errors for a parameter win over missing-required errors
        std::expected<void, ser::OperationResponseMessage> result{};

        auto check_one = [&]<std::size_t I, ParameterSpec P>() -> bool {
            if (errs[I].has_value()) {
                result = std::unexpected(*errs[I]);
                return false;
            }

            if (!seen[I]) {
                if constexpr (!(P::optional || P::has_default)) {
                    result = std::unexpected(make_missing_error<P>(req.operation_code));
                    return false;
                }
            }

            return true;
        };

        [&]<std::size_t... Is>(std::index_sequence<Is...>) { ((check_one.template operator()<Is, Ps>()) && ...); }(std::index_sequence_for<Ps...>{});

        return result;
    }

    // Encode this model into an OperationResponseMessage
    std::expected<ser::ParameterList, std::string> encode(uint8_t op_code, int16_t return_code = 0,
                                                          std::optional<std::string> debug_message = std::nullopt) const {
        ser::ParameterList parameters{};

        std::optional<std::string> err;

        auto put_one = [&parameters, &err]<ParameterSpec P>(const Model& self) -> void {
            if (err.has_value())
                return;

            auto encode_one_value = [&](const typename P::value_type& v) {
                if constexpr (std::is_enum_v<typename P::value_type>) {
                    if (!magic_enum::enum_contains<typename P::value_type>(v)) {
                        const std::string param_name = param_name_string<P>();
                        const std::string_view key_name = magic_enum::enum_name(static_cast<typename P::enum_type>(P::param_key));
                        const auto raw = static_cast<typename P::wire_type>(v);

                        err = std::format("Cannot encode parameter '{}' (key={}): enum value {} is not valid in {}.", param_name, key_name,
                                          static_cast<std::uint64_t>(raw), magic_enum::enum_type_name<typename P::value_type>());
                        return;
                    }
                    parameters[P::param_key] = ser::Value{static_cast<typename P::wire_type>(v)};
                } else {
                    parameters[P::param_key] = ser::Value{v};
                }
            };

            if constexpr (P::optional) {
                const auto& opt = self.template get<P>();
                if (opt.has_value()) {
                    encode_one_value(*opt);
                }
            } else {
                encode_one_value(self.template get<P>());
            }
        };

        (put_one.template operator()<Ps>(*this), ...);

        if (err.has_value())
            return std::unexpected(*err);

        return parameters;
    }
};

// Model extension
template <typename ModelT, server::models::ParameterSpec... NewPs> struct ExtendModel;
template <server::models::ParameterSpec... OldPs, server::models::ParameterSpec... NewPs> struct ExtendModel<server::models::Model<OldPs...>, NewPs...> {
    using Type = server::models::Model<OldPs..., NewPs...>;
};
template <typename ModelT, server::models::ParameterSpec... NewPs> using ExtendedModel = typename ExtendModel<ModelT, NewPs...>::Type;

// Model merging
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
} // namespace models
} // namespace server
