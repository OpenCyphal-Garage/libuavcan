/// @copyright
/// Copyright (C) OpenCyphal Development Team  <opencyphal.org>
/// Copyright Amazon.com Inc. or its affiliates.
/// SPDX-License-Identifier: MIT

#include "application/registry/registry_mock.hpp"
#include "gtest_helpers.hpp"  // NOLINT(misc-include-cleaner)
#include "tracking_memory_resource.hpp"
#include "transport/scattered_buffer_storage_mock.hpp"
#include "transport/svc_sessions_mock.hpp"
#include "transport/transport_gtest_helpers.hpp"
#include "transport/transport_mock.hpp"
#include "verification_utilities.hpp"
#include "virtual_time_scheduler.hpp"

#include <cetl/pf17/cetlpf.hpp>
#include <libcyphal/application/node/registry_provider.hpp>
#include <libcyphal/errors.hpp>
#include <libcyphal/presentation/presentation.hpp>
#include <libcyphal/transport/svc_sessions.hpp>
#include <libcyphal/transport/types.hpp>
#include <libcyphal/types.hpp>

#include <uavcan/_register/Access_1_0.hpp>
#include <uavcan/_register/List_1_0.hpp>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <cstring>
#include <limits>
#include <utility>

namespace
{

using libcyphal::TimePoint;
using namespace libcyphal::application;   // NOLINT This our main concern here in the unit tests.
using namespace libcyphal::presentation;  // NOLINT This our main concern here in the unit tests.
using namespace libcyphal::transport;     // NOLINT This our main concern here in the unit tests.

using testing::_;
using testing::Invoke;
using testing::Return;
using testing::IsEmpty;
using testing::NiceMock;
using testing::StrictMock;
using testing::ElementsAre;
using testing::VariantWith;

// https://github.com/llvm/llvm-project/issues/53444
// NOLINTBEGIN(misc-unused-using-decls, misc-include-cleaner)
using std::literals::chrono_literals::operator""s;
using std::literals::chrono_literals::operator""ms;
// NOLINTEND(misc-unused-using-decls, misc-include-cleaner)

// NOLINTBEGIN(cppcoreguidelines-avoid-magic-numbers, readability-magic-numbers)

class TestRegistryProvider : public testing::Test
{
protected:
    using ListService        = uavcan::_register::List_1_0;
    using AccessService      = uavcan::_register::Access_1_0;
    using UniquePtrReqRxSpec = RequestRxSessionMock::RefWrapper::Spec;
    using UniquePtrResTxSpec = ResponseTxSessionMock::RefWrapper::Spec;

    void SetUp() override
    {
        cetl::pmr::set_default_resource(&mr_default_);

        EXPECT_CALL(transport_mock_, getProtocolParams())
            .WillRepeatedly(Return(ProtocolParams{std::numeric_limits<TransferId>::max(), 0, 0}));
    }

    void TearDown() override
    {
        EXPECT_THAT(mr_.allocations, IsEmpty());
        EXPECT_THAT(mr_.total_allocated_bytes, mr_.total_deallocated_bytes);

        EXPECT_THAT(mr_default_.allocations, IsEmpty());
        EXPECT_THAT(mr_default_.total_allocated_bytes, mr_default_.total_deallocated_bytes);
        EXPECT_THAT(mr_default_.total_allocated_bytes, 0);
    }

    TimePoint now() const
    {
        return scheduler_.now();
    }

    struct ServiceContext
    {
        IRequestRxSession::OnReceiveCallback::Function req_rx_cb_fn;
        StrictMock<RequestRxSessionMock>               req_rx_session_mock;
        StrictMock<ResponseTxSessionMock>              res_tx_session_mock;
    };
    template <typename Service>
    void expectMakeSvcSessions(ServiceContext& svc_context)
    {
        EXPECT_CALL(svc_context.req_rx_session_mock, setOnReceiveCallback(_))  //
            .WillRepeatedly(Invoke([&](auto&& cb_fn) {                         //
                svc_context.req_rx_cb_fn = std::forward<IRequestRxSession::OnReceiveCallback::Function>(cb_fn);
            }));

        constexpr RequestRxParams rx_params{Service::Request::_traits_::ExtentBytes,
                                            Service::Request::_traits_::FixedPortId};
        EXPECT_CALL(transport_mock_, makeRequestRxSession(RequestRxParamsEq(rx_params)))
            .WillOnce(Invoke([&](const auto&) {
                return libcyphal::detail::makeUniquePtr<UniquePtrReqRxSpec>(mr_, svc_context.req_rx_session_mock);
            }));

        constexpr ResponseTxParams tx_params{Service::Response::_traits_::FixedPortId};
        EXPECT_CALL(transport_mock_, makeResponseTxSession(ResponseTxParamsEq(tx_params)))
            .WillOnce(Invoke([&](const auto&) {
                return libcyphal::detail::makeUniquePtr<UniquePtrResTxSpec>(mr_, svc_context.res_tx_session_mock);
            }));

        EXPECT_CALL(svc_context.req_rx_session_mock, deinit()).Times(1);
        EXPECT_CALL(svc_context.res_tx_session_mock, deinit()).Times(1);
    }

    // MARK: Data members:

    // NOLINTBEGIN
    libcyphal::VirtualTimeScheduler        scheduler_{};
    TrackingMemoryResource                 mr_;
    TrackingMemoryResource                 mr_default_;
    cetl::pmr::polymorphic_allocator<void> mr_alloc_{&mr_};
    StrictMock<TransportMock>              transport_mock_;
    // NOLINTEND
};

// MARK: - Tests:

TEST_F(TestRegistryProvider, make_list_req)
{
    Presentation presentation{mr_, scheduler_, transport_mock_};

    registry::IntrospectableRegistryMock registry_mock;

    ServiceContext list_svc_cnxt;
    expectMakeSvcSessions<ListService>(list_svc_cnxt);
    ServiceContext access_svc_cnxt;
    expectMakeSvcSessions<AccessService>(access_svc_cnxt);

    cetl::optional<node::RegistryProvider> registry_provider;

    ListService::Request                 test_request{};
    NiceMock<ScatteredBufferStorageMock> storage_mock;
    EXPECT_CALL(storage_mock, size())
        .WillRepeatedly(Return(ListService::Request::_traits_::SerializationBufferSizeBytes));
    EXPECT_CALL(storage_mock, copy(0, _, _))                           //
        .WillRepeatedly(Invoke([&](auto, auto* const dst, auto len) {  //
            //
            std::array<std::uint8_t, ListService::Request::_traits_::SerializationBufferSizeBytes> buffer{};
            const auto result = serialize(test_request, buffer);
            const auto size   = std::min(result.value(), len);
            (void) std::memmove(dst, buffer.data(), size);
            return size;
        }));
    ScatteredBufferStorageMock::Wrapper storage{&storage_mock};
    ServiceRxTransfer request{{{{123, Priority::Fast}, {}}, NodeId{0x31}}, ScatteredBuffer{std::move(storage)}};

    scheduler_.scheduleAt(1s, [&](const auto&) {
        //
        auto maybe_registry_provider = node::RegistryProvider::make(presentation, registry_mock);
        ASSERT_THAT(maybe_registry_provider, VariantWith<node::RegistryProvider>(_));
        registry_provider.emplace(cetl::get<node::RegistryProvider>(std::move(maybe_registry_provider)));
    });
    scheduler_.scheduleAt(2s, [&](const auto&) {
        //
        EXPECT_CALL(registry_mock, index(0)).WillOnce(Return("abc"));
        EXPECT_CALL(list_svc_cnxt.res_tx_session_mock,
                    send(ServiceTxMetadataEq({{{123, Priority::Fast}, now() + 1s}, NodeId{0x31}}), _))  //
            .WillOnce(Invoke([this](const auto&, const auto fragments) {
                //
                ListService::Response response{mr_alloc_};
                EXPECT_TRUE(libcyphal::verification_utilities::tryDeserialize(response, fragments));
                EXPECT_THAT(response.name.name, ElementsAre('a', 'b', 'c'));
                return cetl::nullopt;
            }));

        request.metadata.rx_meta.timestamp = now();
        list_svc_cnxt.req_rx_cb_fn({request});
    });
    scheduler_.scheduleAt(3s, [&](const auto&) {
        //
        registry_provider->setResponseTimeout(100ms);

        test_request.index = 1;
        EXPECT_CALL(registry_mock, index(1)).WillOnce(Return(nullptr));
        EXPECT_CALL(list_svc_cnxt.res_tx_session_mock,
                    send(ServiceTxMetadataEq({{{124, Priority::Nominal}, now() + 100ms}, NodeId{0x31}}), _))  //
            .WillOnce(Invoke([this](const auto&, const auto fragments) {
                //
                ListService::Response response{mr_alloc_};
                EXPECT_TRUE(libcyphal::verification_utilities::tryDeserialize(response, fragments));
                EXPECT_THAT(response.name.name, IsEmpty());
                return cetl::nullopt;
            }));

        request.metadata.rx_meta.base.transfer_id = 124;
        request.metadata.rx_meta.base.priority    = Priority::Nominal;
        request.metadata.rx_meta.timestamp        = now();
        list_svc_cnxt.req_rx_cb_fn({request});
    });
    scheduler_.scheduleAt(9s, [&](const auto&) {
        //
        registry_provider.reset();
    });
    scheduler_.spinFor(10s);
}

TEST_F(TestRegistryProvider, make_failure)
{
    Presentation presentation{mr_, scheduler_, transport_mock_};

    registry::IntrospectableRegistryMock registry_mock;

    scheduler_.scheduleAt(1s, [&](const auto&) {
        //
        EXPECT_CALL(transport_mock_, makeRequestRxSession(_))  //
            .WillOnce(Return(libcyphal::ArgumentError{}));

        EXPECT_THAT(node::RegistryProvider::make(presentation, registry_mock),
                    VariantWith<Presentation::MakeFailure>(VariantWith<libcyphal::ArgumentError>(_)));
    });
    scheduler_.scheduleAt(2s, [&](const auto&) {
        //
        ServiceContext list_svc_cnxt;
        expectMakeSvcSessions<ListService>(list_svc_cnxt);

        constexpr RequestRxParams rx_params{AccessService::Request::_traits_::ExtentBytes,
                                            AccessService::Request::_traits_::FixedPortId};
        EXPECT_CALL(transport_mock_, makeRequestRxSession(RequestRxParamsEq(rx_params)))  //
            .WillOnce(Return(libcyphal::ArgumentError{}));

        EXPECT_THAT(node::RegistryProvider::make(presentation, registry_mock),
                    VariantWith<Presentation::MakeFailure>(VariantWith<libcyphal::ArgumentError>(_)));
    });
    scheduler_.spinFor(10s);
}

// NOLINTEND(cppcoreguidelines-avoid-magic-numbers, readability-magic-numbers)

}  // namespace
