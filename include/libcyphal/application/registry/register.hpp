/// @copyright
/// Copyright (C) OpenCyphal Development Team  <opencyphal.org>
/// Copyright Amazon.com Inc. or its affiliates.
/// SPDX-License-Identifier: MIT

#ifndef LIBCYPHAL_APPLICATION_REGISTRY_REGISTER_HPP_INCLUDED
#define LIBCYPHAL_APPLICATION_REGISTRY_REGISTER_HPP_INCLUDED

#include "libcyphal/common/cavl/cavl.hpp"
#include "libcyphal/common/crc.hpp"
#include "libcyphal/types.hpp"
#include "registry_value.hpp"

#include <cetl/rtti.hpp>

#include <cstdint>

namespace libcyphal
{
namespace application
{
namespace registry
{

/// Defines possible errors that can occur when setting a register value.
///
enum class SetError : std::uint8_t
{
    Existence,   /// The register does not exist.
    Mutability,  /// Register is immutable.
    Coercion,    /// Value cannot be coerced to the register type.
    Semantics,   /// Rejected by the register semantics (e.g., out of range, inappropriate value, bad state, etc.).
};

/// Defines interface for a register.
///
/// NOSONAR cpp:S4963 for below `class IRegister` - protected destructor is necessary.
///
class IRegister : public cavl::Node<IRegister>  // NOSONAR cpp:S4963
{
    // 1AD1885B-954B-48CF-BAC4-FA0A251D3FC0
    // clang-format off
    using TypeIdType = cetl::type_id_type<
        // NOLINTNEXTLINE(cppcoreguidelines-avoid-magic-numbers, readability-magic-numbers)
        0x1A, 0xD1, 0x88, 0x5B, 0x95, 0x4B, 0x48, 0xCF, 0xBA, 0xC4, 0xFA, 0x0A, 0x25, 0x1D, 0x3F, 0xC0>;
    // clang-format on

public:
    /// Defines behavior flags of the register value.
    ///
    struct Flags final
    {
        /// True if the register value can be changed.
        bool _mutable{false};

        /// True if the register value is retained across application restarts.
        bool persistent{false};
    };

    /// Defines a pair of the register value and its flags.
    ///
    struct ValueAndFlags final
    {
        Value value;
        Flags flags;
    };

    /// Defines options used when creating a new register.
    ///
    struct Options final
    {
        /// True if the register value is retained across application restarts.
        bool persistent{false};

    };  // Options

    /// Internally, the registers are accessed by key, which is a name hash.
    ///
    /// A perfectly uniform 32-bit hash yields the collision probability of ~0.0001% for 100 registers:
    ///
    ///     >>> n=100
    ///     >>> d=Decimal(2**32)
    ///     >>> 1- ((d-1)/d) ** ((n*(n-1))//2)
    ///     Decimal('0.0000011525110220794863877123')
    ///
    /// A 64-bit hash yields a negligible collision probability even for a much larger set of registers:
    ///
    ///     >>> n=10_000
    ///     >>> d=Decimal(2**64)
    ///     >>> 1 - ((d-1)/d) ** ((n*(n-1))//2)
    ///     Decimal('2.7102343794533273E-12')
    ///
    class Key final
    {
    public:
        explicit Key(const Name name)
            : value_{hash(name)}
        {
        }

        /// Positive if this one is greater than the other.
        ///
        CETL_NODISCARD std::int8_t compare(const Key other) const noexcept
        {
            if (value_ == other.value_)
            {
                return 0;
            }
            return static_cast<std::int8_t>((value_ > other.value_) ? +1 : -1);
        }

    private:
        CETL_NODISCARD static std::uint64_t hash(const Name name) noexcept
        {
            return common::CRC64WE(name.data(), name.data() + name.size()).get();
        }

        // MARK: Data members:

        const std::uint64_t value_;

    };  // Key

    IRegister(const IRegister&)                      = delete;
    IRegister& operator=(const IRegister&)           = delete;
    IRegister& operator=(IRegister&& other) noexcept = delete;

    /// Gets the register current value and its flags.
    ///
    virtual ValueAndFlags get() const = 0;

    /// Sets the register value.
    ///
    /// @return Optional error if the value cannot be set.
    ///
    virtual cetl::optional<SetError> set(const Value& new_value) = 0;

    /// Gets name of the register.
    ///
    virtual Name getName() const = 0;

    /// Gets unique key of the register.
    ///
    Key getKey() const noexcept
    {
        return key_;
    }

    /// Compares the register by key with a given one.
    ///
    CETL_NODISCARD std::int8_t compareBy(const Key other_key) const noexcept
    {
        return key_.compare(other_key);
    }

    /// Checks if the register is linked to a registry.
    ///
    using Node::isLinked;

    // MARK: RTTI

    static constexpr cetl::type_id _get_type_id_() noexcept
    {
        return cetl::type_id_type_value<TypeIdType>();
    }

protected:
    explicit IRegister(const Name name)
        : key_{name}
    {
    }

    IRegister(IRegister&& other) noexcept
        : Node{std::move(static_cast<Node&&>(other))}
        , key_{other.key_}
    {
    }

    ~IRegister()
    {
        if (isLinked())
        {
            remove();
        }
    }

private:
    // MARK: Data members:

    const Key key_;

};  // IRegister

}  // namespace registry
}  // namespace application
}  // namespace libcyphal

#endif  // LIBCYPHAL_APPLICATION_REGISTRY_REGISTER_HPP_INCLUDED
