/*
 * Copyright (C) 2015 Pavel Kirienko <pavel.kirienko@gmail.com>
 */

#ifndef UAVCAN_PROTOCOL_DYNAMIC_NODE_ID_SERVER_NODE_ID_SELECTOR_HPP_INCLUDED
#define UAVCAN_PROTOCOL_DYNAMIC_NODE_ID_SERVER_NODE_ID_SELECTOR_HPP_INCLUDED

#include <uavcan/build_config.hpp>
#include <cassert>

namespace uavcan
{
namespace dynamic_node_id_server
{
/**
 * Node ID allocation logic
 */
template <typename Owner>
class NodeIDSelector
{
    typedef bool (Owner::*IsNodeIDTakenMethod)(const NodeID node_id) const;

    const Owner* const owner_;
    const IsNodeIDTakenMethod is_node_id_taken_;

public:
    NodeIDSelector(const Owner* owner, IsNodeIDTakenMethod is_node_id_taken)
        : owner_(owner)
        , is_node_id_taken_(is_node_id_taken)
    {
        UAVCAN_ASSERT(owner_ != NULL);
        UAVCAN_ASSERT(is_node_id_taken_ != NULL);
    }

    /**
     * Reutrns a default-constructed (invalid) node ID if a free one could not be found.
     */
    NodeID findFreeNodeID(const NodeID preferred_node_id) const
    {
        uint8_t candidate = preferred_node_id.isUnicast() ? preferred_node_id.get() : NodeID::Max;

        // Up
        while (candidate <= NodeID::Max)
        {
            if (!(owner_->*is_node_id_taken_)(candidate))
            {
                return candidate;
            }
            candidate++;
        }

        candidate = preferred_node_id.isUnicast() ? preferred_node_id.get() : NodeID::Max;
        candidate--;        // This has been tested already

        // Down
        while (candidate > 0)
        {
            if (!(owner_->*is_node_id_taken_)(candidate))
            {
                return candidate;
            }
            candidate--;
        }

        return NodeID();
    }
};

}
}

#endif // Include guard