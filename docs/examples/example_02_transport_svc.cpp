/// @file
/// Example of creating a libcyphal node in your project.
///
/// @copyright
/// Copyright (C) OpenCyphal Development Team  <opencyphal.org>
/// Copyright Amazon.com Inc. or its affiliates.
/// SPDX-License-Identifier: MIT
///

#include "platform/posix/udp_media.hpp"

#include "nunavut/support/serialization.hpp"
#include "uavcan/node/Health_1_0.hpp"
#include "uavcan/node/Heartbeat_1_0.hpp"
#include "uavcan/node/Mode_1_0.hpp"

#include <cetl/pf17/cetlpf.hpp>
#include <cetl/pf20/cetlpf.hpp>
#include <libcyphal/executor.hpp>
#include <libcyphal/platform/single_threaded_executor.hpp>
#include <libcyphal/transport/errors.hpp>
#include <libcyphal/transport/msg_sessions.hpp>
#include <libcyphal/transport/transport.hpp>
#include <libcyphal/transport/types.hpp>
#include <libcyphal/transport/udp/media.hpp>
#include <libcyphal/transport/udp/udp_transport.hpp>
#include <libcyphal/transport/udp/udp_transport_impl.hpp>
#include <libcyphal/types.hpp>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <array>
#include <cassert>  // NOLINT for NUNAVUT_ASSERT
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <thread>
#include <utility>

namespace
{

using namespace libcyphal::transport;       // NOLINT This our main concern here in this test.
using namespace libcyphal::transport::udp;  // NOLINT This our main concern here in this test.

using CallbackHandle      = libcyphal::IExecutor::Callback::Handle;
using UdpTransportPtr     = libcyphal::UniquePtr<libcyphal::transport::udp::IUdpTransport>;
using MessageTxSessionPtr = libcyphal::UniquePtr<libcyphal::transport::IMessageTxSession>;

// https://github.com/llvm/llvm-project/issues/53444
// NOLINTBEGIN(misc-unused-using-decls, misc-include-cleaner)
using std::literals::chrono_literals::operator""s;
using std::literals::chrono_literals::operator""ms;
// NOLINTEND(misc-unused-using-decls, misc-include-cleaner)

using testing::_;
using testing::Eq;
using testing::IsEmpty;
using testing::VariantWith;

// NOLINTBEGIN(cppcoreguidelines-avoid-magic-numbers, readability-magic-numbers)

class Example_02_Transport : public testing::Test
{
protected:
    void TearDown() override {}

    template <std::size_t Redundancy>
    UdpTransportPtr makeUdpTransport(std::array<udp::IMedia*, Redundancy>& media_array, const NodeId local_node_id)
    {
        const std::size_t tx_capacity = 16;

        auto maybe_transport = udp::makeTransport({mr_}, executor_, media_array, tx_capacity);
        EXPECT_THAT(maybe_transport, VariantWith<UdpTransportPtr>(_)) << "Failed to create transport.";
        auto udp_transport = cetl::get<UdpTransportPtr>(std::move(maybe_transport));

        udp_transport->setLocalNodeId(local_node_id);

        return udp_transport;
    }

    template <typename T, typename TxSession, typename TxMetadata>
    static cetl::optional<AnyFailure> serializeAndSend(const T&          value,
                                                       TxSession&        tx_session,
                                                       const TxMetadata& metadata)
    {
        using traits = typename T::_traits_;
        std::array<std::uint8_t, traits::SerializationBufferSizeBytes> buffer{};

        const auto data_size = uavcan::node::serialize(value, buffer);

        // NOLINTNEXTLINE
        const cetl::span<const cetl::byte> fragment{reinterpret_cast<cetl::byte*>(buffer.data()), data_size};
        const std::array<const cetl::span<const cetl::byte>, 1> payload{fragment};

        return tx_session.send(metadata, payload);
    }

    void publishHeartbeat(const libcyphal::TimePoint now)
    {
        state_.heartbeat_.transfer_id_ += 1;

        const auto uptime         = now.time_since_epoch();
        const auto uptime_in_secs = std::chrono::duration_cast<std::chrono::seconds>(uptime);

        const uavcan::node::Heartbeat_1_0 heartbeat{static_cast<std::uint32_t>(uptime_in_secs.count()),
                                                    {uavcan::node::Health_1_0::NOMINAL},
                                                    {uavcan::node::Mode_1_0::OPERATIONAL}};

        EXPECT_THAT(serializeAndSend(heartbeat,
                                     *state_.heartbeat_.msg_tx_session_,
                                     TransferMetadata{state_.heartbeat_.transfer_id_, now, Priority::Nominal}),
                    Eq(cetl::nullopt))
            << "Failed to publish heartbeat.";
    }

    // MARK: Data members:
    // NOLINTBEGIN

    struct State
    {
        struct Heartbeat
        {
            TransferId          transfer_id_{0};
            MessageTxSessionPtr msg_tx_session_;
            CallbackHandle      cb_handle_;

            bool makeTxSession(ITransport& transport)
            {
                auto maybe_msg_tx_session =
                    transport.makeMessageTxSession({uavcan::node::Heartbeat_1_0::_traits_::FixedPortId});
                EXPECT_THAT(maybe_msg_tx_session, VariantWith<MessageTxSessionPtr>(_))
                    << "Failed to create Heartbeat tx session.";
                if (auto* const session = cetl::get_if<MessageTxSessionPtr>(&maybe_msg_tx_session))
                {
                    msg_tx_session_ = std::move(*session);
                }
                return nullptr != msg_tx_session_;
            }
        };

        Heartbeat heartbeat_;

    };  // State

    State                                       state_{};
    cetl::pmr::memory_resource&                 mr_{*cetl::pmr::new_delete_resource()};
    libcyphal::platform::SingleThreadedExecutor executor_{mr_};
    // NOLINTEND
};

// MARK: - Tests:

TEST_F(Example_02_Transport, posix_udp)
{
    const libcyphal::transport::NodeId local_node_id{2000};

    // Make UDP transport with a single media.
    //
    example::platform::posix::UdpMedia udp_media{mr_};
    std::array<udp::IMedia*, 1>        media_array{&udp_media};
    auto                               udp_transport = makeUdpTransport(media_array, local_node_id);

    // Publish heartbeat periodically.
    //
    if (state_.heartbeat_.makeTxSession(*udp_transport))
    {
        state_.heartbeat_.msg_tx_session_->setSendTimeout(1000s);  // for stepping in debugger
        state_.heartbeat_.cb_handle_ = executor_.registerCallback([&](const auto now) {
            //
            publishHeartbeat(now);

            constexpr auto period = uavcan::node::Heartbeat_1_0::MAX_PUBLICATION_PERIOD;
            state_.heartbeat_.cb_handle_.scheduleAt(now + std::chrono::seconds{period});
        });
        state_.heartbeat_.cb_handle_.scheduleAt(executor_.now());
    }

    // Main loop.
    //
    const auto deadline = executor_.now() + 10s;
    while (executor_.now() < deadline)
    {
        executor_.spinOnce();

        std::this_thread::sleep_for(1ms);
    }
}

// NOLINTEND(cppcoreguidelines-avoid-magic-numbers, readability-magic-numbers)

}  // namespace
