/// @copyright
/// Copyright (C) OpenCyphal Development Team  <opencyphal.org>
/// Copyright Amazon.com Inc. or its affiliates.
/// SPDX-License-Identifier: MIT

#ifndef LIBCYPHAL_TRANSPORT_UDP_TRANSPORT_IMPL_HPP_INCLUDED
#define LIBCYPHAL_TRANSPORT_UDP_TRANSPORT_IMPL_HPP_INCLUDED

#include "delegate.hpp"
#include "media.hpp"
#include "msg_rx_session.hpp"
#include "msg_tx_session.hpp"
#include "session_tree.hpp"
#include "svc_rx_sessions.hpp"
#include "svc_tx_sessions.hpp"
#include "tx_rx_sockets.hpp"
#include "udp_transport.hpp"

#include "libcyphal/runnable.hpp"
#include "libcyphal/transport/common/tools.hpp"
#include "libcyphal/transport/contiguous_payload.hpp"
#include "libcyphal/transport/errors.hpp"
#include "libcyphal/transport/msg_sessions.hpp"
#include "libcyphal/transport/multiplexer.hpp"
#include "libcyphal/transport/svc_sessions.hpp"
#include "libcyphal/transport/types.hpp"
#include "libcyphal/types.hpp"

#include <cetl/cetl.hpp>
#include <cetl/pf17/cetlpf.hpp>
#include <cetl/pf20/cetlpf.hpp>
#include <cetl/visit_helpers.hpp>
#include <udpard.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <utility>

namespace libcyphal
{
namespace transport
{
namespace udp
{

/// Internal implementation details of the UDP transport.
/// Not supposed to be used directly by the users of the library.
///
namespace detail
{

/// @brief Represents final implementation class of the UDP transport.
///
/// NOSONAR cpp:S4963 for below `class TransportImpl` - we do directly handle resources here;
/// namely: in destructor we have to flush TX queues (otherwise there will be memory leaks).
///
class TransportImpl final : private TransportDelegate, public IUdpTransport  // NOSONAR cpp:S4963
{
    /// @brief Defines private specification for making interface unique ptr.
    ///
    struct Spec : libcyphal::detail::UniquePtrSpec<IUdpTransport, TransportImpl>
    {
        // `explicit` here is in use to disable public construction of derived private `Spec` structs.
        // See https://seanmiddleditch.github.io/enabling-make-unique-with-private-constructors/
        explicit Spec() = default;
    };

    /// @brief Defines private storage of a media index, its interface, TX queue and socket.
    ///
    struct Media final
    {
    public:
        Media(const std::size_t                 index,
              IMedia&                           interface,
              const UdpardNodeID* const         local_node_id,
              const std::size_t                 tx_capacity,
              const struct UdpardMemoryResource udp_mem_res)
            : index_{static_cast<std::uint8_t>(index)}
            , interface_{interface}
            , udpard_tx_{}
        {
            const std::int8_t result = ::udpardTxInit(&udpard_tx_, local_node_id, tx_capacity, udp_mem_res);
            CETL_DEBUG_ASSERT(result == 0, "There should be no path for an error here.");
            (void) result;
        }

        std::uint8_t index() const
        {
            return index_;
        }

        IMedia& interface() const
        {
            return interface_;
        }

        UdpardTx& udpard_tx()
        {
            return udpard_tx_;
        }

        UniquePtr<IRxSocket>& rx_socket_ptr()
        {
            return rx_socket_ptr_;
        }

        UniquePtr<ITxSocket>& tx_socket_ptr()
        {
            return tx_socket_ptr_;
        }

        std::size_t getTxSocketMtu() const noexcept
        {
            return tx_socket_ptr_ ? tx_socket_ptr_->getMtu() : ITxSocket::DefaultMtu;
        }

    private:
        const std::uint8_t   index_;
        IMedia&              interface_;
        UdpardTx             udpard_tx_;
        UniquePtr<IRxSocket> rx_socket_ptr_;
        UniquePtr<ITxSocket> tx_socket_ptr_;
    };
    using MediaArray = libcyphal::detail::VarArray<Media>;

public:
    CETL_NODISCARD static Expected<UniquePtr<IUdpTransport>, FactoryFailure> make(
        const MemoryResourcesSpec& mem_res_spec,
        IMultiplexer&              multiplexer,
        const cetl::span<IMedia*>  media,
        const std::size_t          tx_capacity)
    {
        // Verify input arguments:
        // - At least one media interface must be provided, but no more than the maximum allowed (3).
        //
        const auto media_count = static_cast<std::size_t>(
            std::count_if(media.begin(), media.end(), [](const IMedia* const media_ptr) -> bool {
                return media_ptr != nullptr;
            }));
        if ((media_count == 0) || (media_count > UDPARD_NETWORK_INTERFACE_COUNT_MAX))
        {
            return ArgumentError{};
        }

        const MemoryResources memory_resources{mem_res_spec.general,
                                               makeUdpardMemoryResource(mem_res_spec.session, mem_res_spec.general),
                                               makeUdpardMemoryResource(mem_res_spec.fragment, mem_res_spec.general),
                                               makeUdpardMemoryDeleter(mem_res_spec.payload, mem_res_spec.general)};

        const UdpardNodeID unset_node_id = UDPARD_NODE_ID_UNSET;

        // False positive of clang-tidy - we move `media_array` to the `transport` instance, so can't make it const.
        // NOLINTNEXTLINE(misc-const-correctness)
        MediaArray media_array = makeMediaArray(mem_res_spec.general,
                                                media_count,
                                                media,
                                                &unset_node_id,
                                                tx_capacity,
                                                memory_resources.fragment);
        if (media_array.size() != media_count)
        {
            return MemoryError{};
        }

        auto transport = libcyphal::detail::makeUniquePtr<Spec>(memory_resources.general,
                                                                Spec{},
                                                                memory_resources,
                                                                multiplexer,
                                                                std::move(media_array));
        if (transport == nullptr)
        {
            return MemoryError{};
        }

        return transport;
    }

    TransportImpl(const Spec,
                  const MemoryResources& memory_resources,
                  IMultiplexer&          multiplexer,
                  MediaArray&&           media_array)
        : TransportDelegate{memory_resources}
        , media_array_{std::move(media_array)}
        , msg_rx_session_nodes_{memory_resources.general}
        , svc_request_rx_session_nodes_{memory_resources.general}
        , svc_response_rx_session_nodes_{memory_resources.general}
    {
        for (auto& media : media_array_)
        {
            media.udpard_tx().local_node_id = &node_id();
        }

        // TODO: Use it!
        (void) multiplexer;
    }

    TransportImpl(const TransportImpl&)                = delete;
    TransportImpl(TransportImpl&&) noexcept            = delete;
    TransportImpl& operator=(const TransportImpl&)     = delete;
    TransportImpl& operator=(TransportImpl&&) noexcept = delete;

    ~TransportImpl()
    {
        for (Media& media : media_array_)
        {
            flushUdpardTxQueue(media.udpard_tx());
        }
    }

private:
    // MARK: IUdpTransport

    void setTransientErrorHandler(TransientErrorHandler handler) override
    {
        transient_error_handler_ = std::move(handler);
    }

    // MARK: ITransport

    CETL_NODISCARD cetl::optional<NodeId> getLocalNodeId() const noexcept override
    {
        if (node_id() > UDPARD_NODE_ID_MAX)
        {
            return cetl::nullopt;
        }

        return cetl::make_optional(node_id());
    }

    CETL_NODISCARD cetl::optional<ArgumentError> setLocalNodeId(const NodeId new_node_id) noexcept override
    {
        if (new_node_id > UDPARD_NODE_ID_MAX)
        {
            return ArgumentError{};
        }

        // Allow setting the same node ID multiple times, but only once otherwise.
        //
        if (node_id() == new_node_id)
        {
            return cetl::nullopt;
        }
        if (node_id() != UDPARD_NODE_ID_UNSET)
        {
            return ArgumentError{};
        }

        svc_rx_sockets_endpoint_ = setNodeId(new_node_id);

        return cetl::nullopt;
    }

    CETL_NODISCARD ProtocolParams getProtocolParams() const noexcept override
    {
        std::size_t min_mtu = std::numeric_limits<std::size_t>::max();
        for (const Media& media : media_array_)
        {
            min_mtu = std::min(min_mtu, media.getTxSocketMtu());
        }

        return ProtocolParams{std::numeric_limits<TransferId>::max(), min_mtu, UDPARD_NODE_ID_MAX + 1};
    }

    CETL_NODISCARD Expected<UniquePtr<IMessageRxSession>, AnyFailure> makeMessageRxSession(
        const MessageRxParams& params) override
    {
        return makeAnyRxSession<IMessageRxSession, MessageRxSession>(params.subject_id, params, msg_rx_session_nodes_);
    }

    CETL_NODISCARD Expected<UniquePtr<IMessageTxSession>, AnyFailure> makeMessageTxSession(
        const MessageTxParams& params) override
    {
        auto failure = ensureMediaTxSockets();
        if (failure.has_value())
        {
            return failure.value();
        }

        return MessageTxSession::make(memoryResources().general, asDelegate(), params);
    }

    CETL_NODISCARD Expected<UniquePtr<IRequestRxSession>, AnyFailure> makeRequestRxSession(
        const RequestRxParams& params) override
    {
        auto failure = ensureMediaRxSockets();
        if (failure.has_value())
        {
            return failure.value();
        }

        return makeAnyRxSession<IRequestRxSession, SvcRequestRxSession>(params.service_id,
                                                                        params,
                                                                        svc_request_rx_session_nodes_);
    }

    CETL_NODISCARD Expected<UniquePtr<IRequestTxSession>, AnyFailure> makeRequestTxSession(
        const RequestTxParams& params) override
    {
        auto failure = ensureMediaTxSockets();
        if (failure.has_value())
        {
            return failure.value();
        }

        return SvcRequestTxSession::make(memoryResources().general, asDelegate(), params);
    }

    CETL_NODISCARD Expected<UniquePtr<IResponseRxSession>, AnyFailure> makeResponseRxSession(
        const ResponseRxParams& params) override
    {
        auto failure = ensureMediaRxSockets();
        if (failure.has_value())
        {
            return failure.value();
        }

        return makeAnyRxSession<IResponseRxSession, SvcResponseRxSession>(params.service_id,
                                                                          params,
                                                                          svc_response_rx_session_nodes_);
    }

    CETL_NODISCARD Expected<UniquePtr<IResponseTxSession>, AnyFailure> makeResponseTxSession(
        const ResponseTxParams& params) override
    {
        auto failure = ensureMediaTxSockets();
        if (failure.has_value())
        {
            return failure.value();
        }

        return SvcResponseTxSession::make(memoryResources().general, asDelegate(), params);
    }

    // MARK: IRunnable

    CETL_NODISCARD IRunnable::MaybeFailure run(const TimePoint now) override
    {
        cetl::optional<AnyFailure> failure{};

        // We deliberately first run TX as much as possible, and only then running RX -
        // transmission will release resources (like TX queue items) and make room for new incoming frames.
        //
        failure = runMediaTransmit(now);
        if (failure.has_value())
        {
            return failure.value();
        }
        //
        failure = runMediaReceive();
        if (failure.has_value())
        {
            return failure.value();
        }

        return {};
    }

    // MARK: TransportDelegate

    CETL_NODISCARD TransportDelegate& asDelegate()
    {
        return *this;
    }

    CETL_NODISCARD cetl::optional<AnyFailure> sendAnyTransfer(const AnyUdpardTxMetadata::Variant& tx_metadata_var,
                                                              const PayloadFragments payload_fragments) override
    {
        // Udpard currently does not support fragmented payloads (at `udpardTx[Publish|Request|Respond]`).
        // so we need to concatenate them when there are more than one non-empty fragment.
        // TODO: Make similar issue but for Udpard repo.
        // See https://github.com/OpenCyphal/libcanard/issues/223
        //
        const ContiguousPayload payload{memoryResources().general, payload_fragments};
        if ((payload.data() == nullptr) && (payload.size() > 0))
        {
            return MemoryError{};
        }

        for (Media& some_media : media_array_)
        {
            cetl::optional<AnyFailure> failure =
                withEnsureMediaTxSocket(some_media,
                                        [this,
                                         &tx_metadata_var,
                                         &payload](auto& media, auto& tx_socket) -> cetl::optional<AnyFailure> {
                                            media.udpard_tx().mtu = tx_socket.getMtu();

                                            const TxTransferHandler transfer_handler{*this, media, payload};
                                            return cetl::visit(transfer_handler, tx_metadata_var);
                                        });
            if (failure.has_value())
            {
                // The handler (if any) just said that it's NOT fine to continue with transferring to
                // other media TX queues, and the error should not be ignored but propagated outside.
                return failure;
            }
        }

        return cetl::nullopt;
    }

    void onSessionEvent(const SessionEvent::Variant& event_var) override
    {
        cetl::visit(
            [this](const auto& event) {
                cetl::visit(cetl::make_overloaded(
                                [this](const SessionEvent::Message::Destroyed& msg_session_destroyed) {
                                    msg_rx_session_nodes_.removeNodeFor(msg_session_destroyed.subject_id);
                                },
                                [this](const SessionEvent::Request::Destroyed& req_session_destroyed) {
                                    svc_request_rx_session_nodes_.removeNodeFor(req_session_destroyed.service_id);
                                },
                                [this](const SessionEvent::Response::Destroyed& res_session_destroyed) {
                                    svc_response_rx_session_nodes_.removeNodeFor(res_session_destroyed.service_id);
                                }),
                            event);
            },
            event_var);
    }

    // MARK: Privates:

    using Self              = TransportImpl;
    using ContiguousPayload = transport::detail::ContiguousPayload;

    struct TxTransferHandler
    {
        // No Sonar `cpp:S5356` b/c we integrate here with libudpard raw C buffers.
        TxTransferHandler(const Self& self, Media& media, const ContiguousPayload& cont_payload)
            : self_{self}
            , media_{media}
            , payload_{cont_payload.size(), cont_payload.data()}  // NOSONAR cpp:S5356
        {
        }

        cetl::optional<AnyFailure> operator()(const AnyUdpardTxMetadata::Publish& tx_metadata) const
        {
            const std::int32_t result = ::udpardTxPublish(&media_.udpard_tx(),
                                                          tx_metadata.deadline_us,
                                                          tx_metadata.priority,
                                                          tx_metadata.subject_id,
                                                          tx_metadata.transfer_id,
                                                          payload_,
                                                          nullptr);

            return self_.tryHandleTransientUdpardResult<TransientErrorReport::UdpardTxPublish>(media_,
                                                                                               result,
                                                                                               media_.udpard_tx());
        }

        cetl::optional<AnyFailure> operator()(const AnyUdpardTxMetadata::Request& tx_metadata) const
        {
            const std::int32_t result = ::udpardTxRequest(&media_.udpard_tx(),
                                                          tx_metadata.deadline_us,
                                                          tx_metadata.priority,
                                                          tx_metadata.service_id,
                                                          tx_metadata.server_node_id,
                                                          tx_metadata.transfer_id,
                                                          payload_,
                                                          nullptr);

            return self_.tryHandleTransientUdpardResult<TransientErrorReport::UdpardTxRequest>(media_,
                                                                                               result,
                                                                                               media_.udpard_tx());
        }

        cetl::optional<AnyFailure> operator()(const AnyUdpardTxMetadata::Respond& tx_metadata) const
        {
            const std::int32_t result = ::udpardTxRespond(&media_.udpard_tx(),
                                                          tx_metadata.deadline_us,
                                                          tx_metadata.priority,
                                                          tx_metadata.service_id,
                                                          tx_metadata.client_node_id,
                                                          tx_metadata.transfer_id,
                                                          payload_,
                                                          nullptr);

            return self_.tryHandleTransientUdpardResult<TransientErrorReport::UdpardTxRespond>(media_,
                                                                                               result,
                                                                                               media_.udpard_tx());
        }

    private:
        const Self&                self_;
        Media&                     media_;
        const struct UdpardPayload payload_;

    };  // TxTransferHandler

    template <typename SessionInterface, typename Concrete, typename RxParams, typename Tree>
    CETL_NODISCARD auto makeAnyRxSession(const PortId port_id, const RxParams& rx_params, Tree& tree_nodes)  //
        -> Expected<UniquePtr<SessionInterface>, AnyFailure>
    {
        auto node_result = tree_nodes.ensureNewNodeFor(port_id);
        if (auto* const failure = cetl::get_if<AnyFailure>(&node_result))
        {
            return std::move(*failure);
        }

        auto session_result = Concrete::make(memoryResources().general, asDelegate(), rx_params);
        if (nullptr != cetl::get_if<AnyFailure>(&session_result))
        {
            tree_nodes.removeNodeFor(port_id);
        }

        return session_result;
    }

    template <typename Report, typename ErrorVariant, typename Culprit>
    CETL_NODISCARD cetl::optional<AnyFailure> tryHandleTransientMediaError(const Media&   media,
                                                                           ErrorVariant&& error_var,
                                                                           Culprit&&      culprit)
    {
        AnyFailure failure = common::detail::anyFailureFromVariant(std::forward<ErrorVariant>(error_var));
        if (!transient_error_handler_)
        {
            return failure;
        }

        TransientErrorReport::Variant report_var{
            Report{std::move(failure), media.index(), std::forward<Culprit>(culprit)}};

        return transient_error_handler_(report_var);
    }

    template <typename Report, typename Culprit>
    CETL_NODISCARD cetl::optional<AnyFailure> tryHandleTransientUdpardResult(const Media&       media,
                                                                             const std::int32_t result,
                                                                             Culprit&&          culprit) const
    {
        cetl::optional<AnyFailure> failure = optAnyFailureFromUdpard(result);
        if (failure.has_value() && transient_error_handler_)
        {
            TransientErrorReport::Variant report_var{
                Report{std::move(failure.value()), media.index(), std::forward<Culprit>(culprit)}};

            failure = transient_error_handler_(report_var);
        }
        return failure;
    }

    CETL_NODISCARD static MediaArray makeMediaArray(cetl::pmr::memory_resource&       memory,
                                                    const std::size_t                 media_count,
                                                    const cetl::span<IMedia*>         media_interfaces,
                                                    const UdpardNodeID* const         local_node_id_,
                                                    const std::size_t                 tx_capacity,
                                                    const struct UdpardMemoryResource udp_mem_res)
    {
        MediaArray media_array{media_count, &memory};

        // Reserve the space for the whole array (to avoid reallocations).
        // Capacity will be less than requested in case of out of memory.
        media_array.reserve(media_count);
        if (media_array.capacity() >= media_count)
        {
            std::size_t index = 0;
            for (IMedia* const media_interface : media_interfaces)
            {
                if (media_interface != nullptr)
                {
                    IMedia& media = *media_interface;
                    media_array.emplace_back(index, media, local_node_id_, tx_capacity, udp_mem_res);
                    index++;
                }
            }
            CETL_DEBUG_ASSERT(index == media_count, "");
            CETL_DEBUG_ASSERT(media_array.size() == media_count, "");
        }

        return media_array;
    }

    /// @brief Tries to run an action with media and its TX socket (the latter one is made on demand if necessary).
    ///
    template <typename Action>
    CETL_NODISCARD cetl::optional<AnyFailure> withEnsureMediaTxSocket(Media& media, Action&& action)
    {
        if (!media.tx_socket_ptr())
        {
            using ErrorReport = TransientErrorReport::MediaMakeTxSocket;

            auto tx_socket_result = media.interface().makeTxSocket();
            if (auto* const failure = cetl::get_if<IMedia::MakeTxSocketResult::Failure>(&tx_socket_result))
            {
                return tryHandleTransientMediaError<ErrorReport>(media, std::move(*failure), media.interface());
            }

            media.tx_socket_ptr() = cetl::get<IMedia::MakeTxSocketResult::Success>(std::move(tx_socket_result));
            if (!media.tx_socket_ptr())
            {
                return tryHandleTransientMediaError<ErrorReport, cetl::variant<MemoryError>>(media,
                                                                                             MemoryError{},
                                                                                             media.interface());
            }
        }

        return std::forward<Action>(action)(media, *media.tx_socket_ptr());
    }

    CETL_NODISCARD cetl::optional<AnyFailure> ensureMediaTxSockets()
    {
        for (Media& media : media_array_)
        {
            cetl::optional<AnyFailure> failure =
                withEnsureMediaTxSocket(media, [](auto&, auto&) -> cetl::nullopt_t { return cetl::nullopt; });
            if (failure.has_value())
            {
                return failure;
            }
        }

        return cetl::nullopt;
    }

    void flushUdpardTxQueue(UdpardTx& udpard_tx) const
    {
        while (const UdpardTxItem* const maybe_item = ::udpardTxPeek(&udpard_tx))
        {
            UdpardTxItem* const item = ::udpardTxPop(&udpard_tx, maybe_item);
            ::udpardTxFree(memoryResources().fragment, item);
        }
    }

    /// @brief Runs transmission loop for each redundant media interface.
    ///
    CETL_NODISCARD cetl::optional<AnyFailure> runMediaTransmit(const TimePoint now)
    {
        for (Media& some_media : media_array_)
        {
            cetl::optional<AnyFailure> failure =
                withEnsureMediaTxSocket(some_media,
                                        [this, now](Media& media, ITxSocket& tx_socket) -> cetl::optional<AnyFailure> {
                                            return runSingleMediaTransmit(media, tx_socket, now);
                                        });
            if (failure.has_value())
            {
                return failure;
            }
        }

        return cetl::nullopt;
    }

    /// @brief Runs transmission loop for a single media interface and its TX socket.
    ///
    /// Transmits as much as possible frames that are ready to be sent by the media TX socket interface.
    ///
    CETL_NODISCARD cetl::optional<AnyFailure> runSingleMediaTransmit(Media&          media,
                                                                     ITxSocket&      tx_socket,
                                                                     const TimePoint now)
    {
        using PayloadFragment = cetl::span<const cetl::byte>;

        while (const UdpardTxItem* const tx_item = ::udpardTxPeek(&media.udpard_tx()))
        {
            // We are dropping any TX item that has expired.
            // Otherwise, we would send it to the media TX socket interface.
            // We use strictly `>=` (instead of `>`) to give this frame a chance (one extra 1us) at the socket.
            //
            const auto deadline = TimePoint{std::chrono::microseconds{tx_item->deadline_usec}};
            if (now >= deadline)
            {
                // Release whole expired transfer b/c possible next frames of the same transfer are also expired.
                popAndFreeUdpardTxItem(&media.udpard_tx(), tx_item, true /*whole transfer*/);

                // No Sonar `cpp:S909` b/c it make sense to use `continue` statement here - the corner case of
                // "early" (by deadline) transfer drop. Using `if` would make the code less readable and more nested.
                continue;  // NOSONAR cpp:S909
            }

            // No Sonar `cpp:S5356` and `cpp:S5357` b/c we integrate here with C libudpard API.
            const auto* const buffer =
                static_cast<const cetl::byte*>(tx_item->datagram_payload.data);  // NOSONAR cpp:S5356 cpp:S5357
            const std::array<PayloadFragment, 1> single_payload_fragment{
                PayloadFragment{buffer, tx_item->datagram_payload.size}};

            ITxSocket::SendResult::Type send_result =
                tx_socket.send(deadline,
                               {tx_item->destination.ip_address, tx_item->destination.udp_port},
                               tx_item->dscp,
                               single_payload_fragment);

            // In case of socket send error we are going to drop this problematic frame
            // (b/c it looks like media TX socket can't handle this frame),
            // but we will continue to process with other frames if transient error handler says so.
            // Note that socket not being ready/able to send a frame just yet (aka temporary)
            // is not reported as an error (see `is_sent` below).
            //
            if (auto* const send_failure = cetl::get_if<ITxSocket::SendResult::Failure>(&send_result))
            {
                // Release whole problematic transfer from the TX queue,
                // so that other transfers in TX queue have their chance.
                // Otherwise, we would be stuck in a run loop trying to send the same frame.
                popAndFreeUdpardTxItem(&media.udpard_tx(), tx_item, true /*whole transfer*/);

                cetl::optional<AnyFailure> failure =
                    tryHandleTransientMediaError<TransientErrorReport::MediaTxSocketSend>(media,
                                                                                          std::move(*send_failure),
                                                                                          tx_socket);
                if (failure.has_value())
                {
                    return failure;
                }

                // The handler just said that it's fine to continue with sending other frames
                // and ignore such a transient media error (and don't propagate it outside).
            }
            else
            {
                const auto sent = cetl::get<ITxSocket::SendResult::Success>(send_result);
                if (!sent.is_accepted)
                {
                    // TX socket interface is busy, so we are done with this media for now,
                    // and will just try again with it later (on next `run`).
                    // Note, we are NOT releasing this item from the queue, so it will be retried on next `run`.
                    break;

                    // TODO: It seems that `Multiplexer` interface would be used here
                    //       but it is not yet implemented, so for now just `break`.
                }

                popAndFreeUdpardTxItem(&media.udpard_tx(), tx_item, false /*single frame*/);
            }

        }  // for each frame

        return cetl::nullopt;
    }

    /// @brief Tries to run an action with media and its RX socket (the latter one is made on demand if necessary).
    ///
    template <typename Action>
    CETL_NODISCARD cetl::optional<AnyFailure> withEnsureMediaRxSocket(Media& media, Action&& action)
    {
        if (!media.rx_socket_ptr())
        {
            using ErrorReport = TransientErrorReport::MediaMakeRxSocket;

            // Missing RX sockets endpoint means that the local node ID is not set yet,
            // So, node can't be as a destination for any incoming frames - hence nothing to receive.
            //
            if (!svc_rx_sockets_endpoint_.has_value())
            {
                return cetl::nullopt;
            }

            auto rx_socket_result = media.interface().makeRxSocket(svc_rx_sockets_endpoint_.value());
            if (auto* const failure = cetl::get_if<IMedia::MakeRxSocketResult::Failure>(&rx_socket_result))
            {
                return tryHandleTransientMediaError<ErrorReport>(media, std::move(*failure), media.interface());
            }

            media.rx_socket_ptr() = cetl::get<IMedia::MakeRxSocketResult::Success>(std::move(rx_socket_result));
            if (!media.rx_socket_ptr())
            {
                return tryHandleTransientMediaError<ErrorReport, cetl::variant<MemoryError>>(media,
                                                                                             MemoryError{},
                                                                                             media.interface());
            }
        }

        return std::forward<Action>(action)(media, *media.rx_socket_ptr());
    }

    CETL_NODISCARD cetl::optional<AnyFailure> ensureMediaRxSockets()
    {
        for (Media& media : media_array_)
        {
            cetl::optional<AnyFailure> failure =
                withEnsureMediaRxSocket(media, [](auto&, auto&) -> cetl::nullopt_t { return cetl::nullopt; });
            if (failure.has_value())
            {
                return failure;
            }
        }

        return cetl::nullopt;
    }

    cetl::optional<AnyFailure> runMediaReceive()
    {
        for (Media& some_media : media_array_)
        {
            cetl::optional<AnyFailure> failure =
                withEnsureMediaRxSocket(some_media,
                                        [this](const Media& media, IRxSocket& rx_socket) -> cetl::optional<AnyFailure> {
                                            return runSingleMediaReceive(media, rx_socket);
                                        });
            if (failure.has_value())
            {
                return failure;
            }
        }

        return cetl::nullopt;
    }

    cetl::optional<AnyFailure> runSingleMediaReceive(const Media& media, IRxSocket& rx_socket)
    {
        // 1. Try to receive a frame from the media RX socket.
        //
        IRxSocket::ReceiveResult::Type receive_result = rx_socket.receive();
        if (auto* const failure = cetl::get_if<IRxSocket::ReceiveResult::Failure>(&receive_result))
        {
            using RxSocketReport = TransientErrorReport::MediaRxSocketReceive;
            return tryHandleTransientMediaError<RxSocketReport>(media, std::move(*failure), rx_socket);
        }
        auto rx_success = cetl::get<IRxSocket::ReceiveResult::Success>(std::move(receive_result));
        if (!rx_success.has_value())
        {
            return cetl::nullopt;
        }
        IRxSocket::ReceiveResult::Metadata rx_meta = std::move(rx_success.value());

        // 2. We've got a new frame from the media RX socket, so let's try to pass it into libudpard.

        const auto timestamp_us =
            std::chrono::duration_cast<std::chrono::microseconds>(rx_meta.timestamp.time_since_epoch());

        const auto payload_deleter = rx_meta.payload_ptr.get_deleter();

        // TODO: Currently we expect that user allocates payload memory from the specific PMR (the "payload" one).
        //       Later we will pass users deleter into lizards (along with moved buffer),
        //       so such requirement will be lifted (see https://github.com/OpenCyphal-Garage/libcyphal/issues/352).
        //
        CETL_DEBUG_ASSERT(payload_deleter.resource() == memoryResources().payload.user_reference,
                          "PMR of deleter is expected to be the same as the payload memory resource.");

        UdpardRxRPCTransfer out_transfer{};
        UdpardRxRPCPort*    out_port{nullptr};

        const std::int8_t result =
            ::udpardRxRPCDispatcherReceive(&getUdpardRpcDispatcher(),
                                           static_cast<UdpardMicrosecond>(timestamp_us.count()),
                                           // Udpard takes ownership of the payload buffer,
                                           // regardless of the result of the operation - hence the `.release()`.
                                           {payload_deleter.size(), rx_meta.payload_ptr.release()},
                                           media.index(),
                                           &out_port,
                                           &out_transfer);

        // 3. We might have result TX transfer (built from fragments by libudpard).
        //    If so, we need to pass it to the session delegate for storing.
        //
        using DispatcherReport = TransientErrorReport::UdpardRxSvcReceive;
        cetl::optional<AnyFailure> failure =
            tryHandleTransientUdpardResult<DispatcherReport>(media, result, getUdpardRpcDispatcher());
        if ((!failure.has_value()) && (result > 0))
        {
            CETL_DEBUG_ASSERT(out_port != nullptr, "Expected subscription.");
            CETL_DEBUG_ASSERT(out_port->user_reference != nullptr, "Expected session delegate.");

            // No Sonar `cpp:S5357` b/c the raw `user_reference` is part of libcanard api,
            // and it was set by us at a RX session constructor (see f.e. `MessageRxSession` ctor).
            auto* const delegate = static_cast<IRxSessionDelegate*>(out_port->user_reference);  // NOSONAR cpp:S5357
            delegate->acceptRxTransfer(out_transfer.base);
        }

        return failure;
    }

    // MARK: Data members:

    MediaArray                               media_array_;
    TransientErrorHandler                    transient_error_handler_;
    SessionTree<RxSessionTreeNode::Message>  msg_rx_session_nodes_;
    SessionTree<RxSessionTreeNode::Request>  svc_request_rx_session_nodes_;
    SessionTree<RxSessionTreeNode::Response> svc_response_rx_session_nodes_;
    cetl::optional<IpEndpoint>               svc_rx_sockets_endpoint_;

};  // TransportImpl

}  // namespace detail

/// @brief Makes a new UDP transport instance.
///
/// NB! Lifetime of the transport instance must never outlive memory resources, `media` and `multiplexer` instances.
///
/// @param mem_res_spec Specification of polymorphic memory resources to use for all allocations.
/// @param multiplexer Interface of the multiplexer to use.
/// @param media Collection of redundant media interfaces to use.
/// @param tx_capacity Total number of frames that can be queued for transmission per `IMedia` instance.
/// @return Unique pointer to the new UDP transport instance or a failure.
///
inline Expected<UniquePtr<IUdpTransport>, FactoryFailure> makeTransport(const MemoryResourcesSpec& mem_res_spec,
                                                                        IMultiplexer&              multiplexer,
                                                                        const cetl::span<IMedia*>  media,
                                                                        const std::size_t          tx_capacity)
{
    return detail::TransportImpl::make(mem_res_spec, multiplexer, media, tx_capacity);
}

}  // namespace udp
}  // namespace transport
}  // namespace libcyphal

#endif  // LIBCYPHAL_TRANSPORT_UDP_TRANSPORT_IMPL_HPP_INCLUDED