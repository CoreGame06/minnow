#pragma once

#include "exception.hh"
#include "network_interface.hh"

#include <optional>
#include <vector>
// \brief A router that has multiple network interfaces and
// performs longest-prefix-match routing between them.
class Router
{
public:
  // Add an interface to the router
  // \param[in] interface an already-constructed network interface
  // \returns The index of the interface after it has been added to the router
  size_t add_interface( std::shared_ptr<NetworkInterface> interface )
  {
    interfaces_.push_back( notnull( "add_interface", std::move( interface ) ) );
    return interfaces_.size() - 1;
  }

  // Access an interface by index
  std::shared_ptr<NetworkInterface> interface( const size_t N ) { return interfaces_.at( N ); }

  // Add a route (a forwarding rule)
  void add_route( uint32_t route_prefix,
                  uint8_t prefix_length,
                  std::optional<Address> next_hop,
                  size_t interface_num );

  // Route packets between the interfaces
  void route();

private:
  // The router's collection of network interfaces
  std::vector<std::shared_ptr<NetworkInterface>> interfaces_ {}; // 每一个就是一个插在这个路由器上，由这个路由器管理的网卡

  // 路由表单个的结构体
  struct RouteEntry
  {
    uint32_t route_prefix; //路由前缀IP
    uint8_t prefix_length;  // 前缀匹配长度
    std::optional<Address> next_hop; // 下一跳IP地址，若为空，则目的地就是IP包本身的dst
    size_t interface_num; // 出口网卡接口的索引号
  };

  // 路由表：存储转发规则
  std::vector<RouteEntry> routing_table_{};

  // 辅助函数
  // 找到最匹配的 RouteEntry
  std::optional<Router::RouteEntry> match_route( const uint32_t dst_ip ) const;
  // 处理单个数据包的dgram,处理发送前的准备工作
  void route_datagram( InternetDatagram& dgram );

};
