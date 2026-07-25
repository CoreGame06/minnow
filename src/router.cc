#include "router.hh"
#include "debug.hh"

#include <iostream>

using namespace std;

// route_prefix: The "up-to-32-bit" IPv4 address prefix to match the datagram's destination address against
// prefix_length: For this route to be applicable, how many high-order (most-significant) bits of
//    the route_prefix will need to match the corresponding bits of the datagram's destination address?
// next_hop: The IP address of the next hop. Will be empty if the network is directly attached to the router (in
//    which case, the next hop address should be the datagram's final destination).
// interface_num: The index of the interface to send the datagram out on.
void Router::add_route( const uint32_t route_prefix,
                        const uint8_t prefix_length,
                        const optional<Address> next_hop,
                        const size_t interface_num )
{
  cerr << "DEBUG: adding route " << Address::from_ipv4_numeric( route_prefix ).ip() << "/"
       << static_cast<int>( prefix_length ) << " => " << ( next_hop.has_value() ? next_hop->ip() : "(direct)" )
       << " on interface " << interface_num << "\n";

  routing_table_.emplace_back(route_prefix,prefix_length,next_hop,interface_num);//原地构造，省区一次拷贝，性能最好
}

std::optional<Router::RouteEntry> Router::match_route(const uint32_t dst_ip) const
{
  const RouteEntry* best_route = nullptr;
  int max_prefix_length = -1;

  for(const auto& route:routing_table_)
  {
    bool is_match = false;

    // 当前prefixlength=0，则任意IP都匹配
    if(route.prefix_length==0)
      is_match = true;
    else
    {
      // 构造掩码
      uint32_t mask = 0xFFFFFFFFu << (32-route.prefix_length);
      is_match = ((dst_ip & mask) == (route.route_prefix & mask));
    }
    // 最长匹配
    if(is_match && static_cast<int>(route.prefix_length)>max_prefix_length)
    {
      max_prefix_length = route.prefix_length;
      best_route = &route;
    }
  }

  return best_route? make_optional(*best_route):nullopt;

}

void Router::route_datagram(InternetDatagram& dgram)
{
  const auto route_opt = match_route(dgram.header.dst);
  if(!route_opt.has_value())
    return;
  
  // 检查TTL
  if(dgram.header.ttl<=1)
    return;

  // TTL减少，重新计算校验码
  dgram.header.ttl--;
  dgram.header.compute_checksum();

  //确定 Next Hop IP
  const auto& route = route_opt.value();
  const Address next_hop_addr = route.next_hop.has_value()? 
                                route.next_hop.value()
                                :Address::from_ipv4_numeric(dgram.header.dst);

  interface(route.interface_num)->send_datagram(dgram,next_hop_addr);
}

// Go through all the interfaces, and route every incoming datagram to its proper outgoing interface.
void Router::route()
{
  // 1.讲网卡积压的数据包拿出来
  // 2.查找手册
  // 3.减少TTL
  // 4.发送
  for(auto& network_interface:interfaces_)
  {
    auto& in_queue = network_interface->datagrams_received();
    while(!in_queue.empty())
    {
      InternetDatagram dgram = move(in_queue.front());
      in_queue.pop();

      // 单包转发
      route_datagram(dgram);
    }
  }
}
