/// @copyright
/// Copyright (C) OpenCyphal Development Team  <opencyphal.org>
/// Copyright Amazon.com Inc. or its affiliates.
/// SPDX-License-Identifier: MIT

#include <libcyphal/transport/can/transport.hpp>

#include "media_mock.hpp"
#include "../multiplexer_mock.hpp"
#include "../../gtest_helpers.hpp"
#include "../../test_scheduler.hpp"
#include "../../test_utilities.hpp"
#include "../../memory_resource_mock.hpp"
#include "../../tracking_memory_resource.hpp"

#include <gmock/gmock.h>

namespace
{
using byte = cetl::byte;

using namespace libcyphal;
using namespace libcyphal::transport;
using namespace libcyphal::transport::can;
using namespace libcyphal::test_utilities;

using testing::_;
using testing::Eq;
using testing::Return;
using testing::IsNull;
using testing::IsEmpty;
using testing::NotNull;
using testing::Optional;
using testing::InSequence;
using testing::StrictMock;
using testing::ElementsAre;
using testing::VariantWith;

using namespace std::chrono_literals;

class TestCanSvcRxSessions : public testing::Test
{
protected:
    void SetUp() override
    {
        EXPECT_CALL(media_mock_, getMtu()).WillRepeatedly(Return(CANARD_MTU_CAN_CLASSIC));
    }

    void TearDown() override
    {
        EXPECT_THAT(mr_.allocations, IsEmpty());
        // TODO: Uncomment this when PMR deleter is fixed.
        // EXPECT_EQ(mr_.total_allocated_bytes, mr_.total_deallocated_bytes);
    }

    TimePoint now() const
    {
        return scheduler_.now();
    }

    CETL_NODISCARD UniquePtr<ICanTransport> makeTransport(cetl::pmr::memory_resource& mr, const NodeId local_node_id)
    {
        std::array<IMedia*, 1> media_array{&media_mock_};

        // TODO: `local_node_id` could be just passed to `can::makeTransport` as an argument,
        // but it's not possible due to CETL issue https://github.com/OpenCyphal/CETL/issues/119.
        const auto opt_local_node_id = cetl::optional<NodeId>{local_node_id};

        auto maybe_transport = can::makeTransport(mr, mux_mock_, media_array, 0, opt_local_node_id);
        EXPECT_THAT(maybe_transport, VariantWith<UniquePtr<ICanTransport>>(NotNull()));
        return cetl::get<UniquePtr<ICanTransport>>(std::move(maybe_transport));
    }

    // MARK: Data members:

    TestScheduler               scheduler_{};
    TrackingMemoryResource      mr_;
    StrictMock<MediaMock>       media_mock_{};
    StrictMock<MultiplexerMock> mux_mock_{};
};

// MARK: Tests:

TEST_F(TestCanSvcRxSessions, make_request_setTransferIdTimeout)
{
    auto transport = makeTransport(mr_, 0x31);

    auto maybe_session = transport->makeRequestRxSession({42, 123});
    ASSERT_THAT(maybe_session, VariantWith<UniquePtr<IRequestRxSession>>(NotNull()));
    auto session = cetl::get<UniquePtr<IRequestRxSession>>(std::move(maybe_session));

    EXPECT_THAT(session->getParams().extent_bytes, 42);
    EXPECT_THAT(session->getParams().service_id, 123);

    session->setTransferIdTimeout(0s);
    session->setTransferIdTimeout(500ms);
}

TEST_F(TestCanSvcRxSessions, make_resposnse_no_memory)
{
    StrictMock<MemoryResourceMock> mr_mock{};
    mr_mock.redirectExpectedCallsTo(mr_);

    // Emulate that there is no memory available for the message session.
    EXPECT_CALL(mr_mock, do_allocate(sizeof(can::detail::SvcResponseRxSession), _)).WillOnce(Return(nullptr));

    auto transport = makeTransport(mr_mock, 0x13);

    auto maybe_session = transport->makeResponseRxSession({64, 0x23, 0x45});
    EXPECT_THAT(maybe_session, VariantWith<AnyError>(VariantWith<MemoryError>(_)));
}

TEST_F(TestCanSvcRxSessions, make_request_fails_due_to_argument_error)
{
    auto transport = makeTransport(mr_, 0x31);

    // Try invalid subject id
    auto maybe_session = transport->makeRequestRxSession({64, CANARD_SERVICE_ID_MAX + 1});
    EXPECT_THAT(maybe_session, VariantWith<AnyError>(VariantWith<ArgumentError>(_)));
}

TEST_F(TestCanSvcRxSessions, run_and_receive_requests)
{
    auto transport = makeTransport(mr_, 0x31);

    const std::size_t extent_bytes  = 8;
    auto              maybe_session = transport->makeRequestRxSession({extent_bytes, 0x17B});
    ASSERT_THAT(maybe_session, VariantWith<UniquePtr<IRequestRxSession>>(NotNull()));
    auto session = cetl::get<UniquePtr<IRequestRxSession>>(std::move(maybe_session));

    const auto timeout      = 200ms;
    session->setTransferIdTimeout(timeout);

    {
        SCOPED_TRACE("1-st iteration: one frame available @ 1s");

        scheduler_.setNow(TimePoint{1s});
        const auto rx_timestamp = now();

        EXPECT_CALL(media_mock_, pop(_)).WillOnce([&](auto p) {
            EXPECT_THAT(now(), rx_timestamp + 10ms);
            EXPECT_THAT(p.size(), CANARD_MTU_MAX);
            p[0] = b(42);
            p[1] = b(147);
            p[2] = b(0b111'11101);
            return RxMetadata{rx_timestamp, 0b011'1'1'0'101111011'0110001'0010011, 3};
        });

        scheduler_.runNow(+10ms, [&] { transport->run(now()); });
        scheduler_.runNow(+10ms, [&] { session->run(now()); });

        const auto maybe_rx_transfer = session->receive();
        ASSERT_THAT(maybe_rx_transfer, Optional(_));
        const auto& rx_transfer = maybe_rx_transfer.value();

        EXPECT_THAT(rx_transfer.metadata.timestamp, rx_timestamp);
        EXPECT_THAT(rx_transfer.metadata.transfer_id, 0x1D);
        EXPECT_THAT(rx_transfer.metadata.priority, Priority::High);
        EXPECT_THAT(rx_transfer.metadata.remote_node_id, 0x13);

        std::array<std::uint8_t, 2> buffer{};
        EXPECT_THAT(rx_transfer.payload.size(), 2);
        EXPECT_THAT(rx_transfer.payload.copy(0, buffer.data(), buffer.size()), 2);
        EXPECT_THAT(buffer, ElementsAre(42, 147));
    }
    {
        SCOPED_TRACE("2-nd iteration: no frames available @ 2s");

        scheduler_.setNow(TimePoint{2s});
        const auto rx_timestamp = now();

        EXPECT_CALL(media_mock_, pop(_)).WillOnce([&](auto payload) {
            EXPECT_THAT(now(), rx_timestamp + 10ms);
            EXPECT_THAT(payload.size(), CANARD_MTU_MAX);
            return cetl::nullopt;
        });

        scheduler_.runNow(+10ms, [&] { transport->run(now()); });
        scheduler_.runNow(+10ms, [&] { session->run(now()); });

        const auto maybe_rx_transfer = session->receive();
        EXPECT_THAT(maybe_rx_transfer, Eq(cetl::nullopt));
    }
    {
        SCOPED_TRACE("3-rd iteration: 2 frames available @ 3s");

        scheduler_.setNow(TimePoint{3s});
        const auto rx_timestamp = now();
        {
            InSequence seq;

            EXPECT_CALL(media_mock_, pop(_)).WillOnce([&](auto p) {
                EXPECT_THAT(now(), rx_timestamp + 10ms);
                EXPECT_THAT(p.size(), CANARD_MTU_MAX);
                p[0] = b('0');
                p[1] = b('1');
                p[2] = b('2');
                p[3] = b('3');
                p[4] = b('4');
                p[5] = b('5');
                p[6] = b('6');
                p[7] = b(0b101'11110);
                return RxMetadata{rx_timestamp, 0b000'1'1'0'101111011'0110001'0010011, 8};
            });
            EXPECT_CALL(media_mock_, pop(_)).WillOnce([&](auto p) {
                EXPECT_THAT(now(), rx_timestamp + 30ms);
                EXPECT_THAT(p.size(), CANARD_MTU_MAX);
                p[0] = b('7');
                p[1] = b('8');
                p[2] = b('9');
                p[3] = b(0x7D);
                p[4] = b(0x61);  // expected 16-bit CRC
                p[5] = b(0b010'11110);
                return RxMetadata{rx_timestamp, 0b000'1'1'0'101111011'0110001'0010011, 6};
            });
        }
        scheduler_.runNow(+10ms, [&] { transport->run(now()); });
        scheduler_.runNow(+10ms, [&] { session->run(now()); });
        scheduler_.runNow(+10ms, [&] { transport->run(now()); });
        scheduler_.runNow(+10ms, [&] { session->run(now()); });

        const auto maybe_rx_transfer = session->receive();
        ASSERT_THAT(maybe_rx_transfer, Optional(_));
        const auto& rx_transfer = maybe_rx_transfer.value();

        EXPECT_THAT(rx_transfer.metadata.timestamp, rx_timestamp);
        EXPECT_THAT(rx_transfer.metadata.transfer_id, 0x1E);
        EXPECT_THAT(rx_transfer.metadata.priority, Priority::Exceptional);
        EXPECT_THAT(rx_transfer.metadata.remote_node_id, 0x13);

        std::array<char, extent_bytes> buffer{};
        EXPECT_THAT(rx_transfer.payload.size(), buffer.size());
        EXPECT_THAT(rx_transfer.payload.copy(0, buffer.data(), buffer.size()), buffer.size());
        EXPECT_THAT(buffer, ElementsAre('0', '1', '2', '3', '4', '5', '6', '7'));
    }
}

}  // namespace
