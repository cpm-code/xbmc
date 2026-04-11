#pragma once

#include <cstddef>
#include <type_traits>
#include <utility>

namespace VideoPlayerDualLayer
{
template<typename Packet>
bool IsEnhancementLayerPacket(const Packet& packet)
{
  if constexpr (std::is_pointer_v<Packet>)
    return packet != nullptr && packet->isELPackage;
  else
    return packet.isELPackage;
}

template<typename Packet>
double PacketDts(const Packet& packet)
{
  if constexpr (std::is_pointer_v<Packet>)
    return packet != nullptr ? packet->dts : 0.0;
  else
    return packet.dts;
}

template<typename Queue>
bool HasOppositeLayerFront(const Queue& pendingPackets, bool isELPackage)
{
  return !pendingPackets.empty() &&
         IsEnhancementLayerPacket(pendingPackets.front()) != isELPackage;
}

template<typename Queue>
bool CanPairWithFront(const Queue& pendingPackets, bool isELPackage, double dts)
{
  return HasOppositeLayerFront(pendingPackets, isELPackage) &&
         dts >= PacketDts(pendingPackets.front());
}

template<typename Queue>
bool IncomingPacketPrecedesFront(const Queue& pendingPackets, double dts)
{
  return !pendingPackets.empty() && dts < PacketDts(pendingPackets.front());
}

template<typename Queue, typename Packet>
void QueuePendingPacket(Queue& pendingPackets, Packet&& packet, std::size_t maxPendingPackets)
{
  pendingPackets.emplace_back(std::forward<Packet>(packet));
  if (maxPendingPackets > 0 && pendingPackets.size() > maxPendingPackets)
    pendingPackets.pop_front();
}
} // namespace VideoPlayerDualLayer
