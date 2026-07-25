#include <iostream>

#include "arp_message.hh"
#include "debug.hh"
#include "ethernet_frame.hh"
#include "exception.hh"
#include "helpers.hh"
#include "network_interface.hh"

using namespace std;

//! \param[in] ethernet_address Ethernet (what ARP calls "hardware") address of the interface
//! \param[in] ip_address IP (what ARP calls "protocol") address of the interface
NetworkInterface::NetworkInterface( string_view name,
                                    shared_ptr<OutputPort> port,
                                    const EthernetAddress& ethernet_address,
                                    const Address& ip_address )
  : name_( name )
  , port_( notnull( "OutputPort", move( port ) ) )
  , ethernet_address_( ethernet_address )
  , ip_address_( ip_address )
{
  cerr << "DEBUG: Network interface has Ethernet address " << to_string( ethernet_address_ ) << " and IP address "
       << ip_address.ip() << "\n";
}

//! \param[in] dgram the IPv4 datagram to be sent
//! \param[in] next_hop the IP address of the interface to send it to (typically a router or default gateway, but
//! may also be another host if directly connected to the same network as the destination) Note: the Address type
//! can be converted to a uint32_t (raw 32-bit IP address) by using the Address::ipv4_numeric() method.
void NetworkInterface::send_datagram( const InternetDatagram& dgram, const Address& next_hop )
{
  //  把IP地址转换为32位
  const uint32_t target_ip = next_hop.ipv4_numeric();
  // 查找ARP缓存表
  if(arp_table_.contains(target_ip))
  {
    EthernetFrame frame;
    frame.header.src = ethernet_address_;
    frame.header.dst = arp_table_[target_ip].mac;
    frame.header.type = EthernetHeader::TYPE_IPv4;
    frame.payload = serialize(dgram); //把结构体对象压扁成字节流
    transmit(frame);
    return;
  }
  // 没有找到缓存中的mac地址,先把数据包暂存起来
  waiting_datagrams_.push_back({next_hop,dgram});

  // 检查5s广播规则，如果这个IP5秒内没有发送ARP请求，则发送广播
  if(!arp_requests_lifetime_.contains(target_ip))
  {
    ARPMessage arp_msg;
    arp_msg.opcode = ARPMessage::OPCODE_REQUEST;// 表示这是一共询问包
    arp_msg.sender_ethernet_address = ethernet_address_; //自己的地址
    arp_msg.sender_ip_address = ip_address_.ipv4_numeric();
    arp_msg.target_ethernet_address = {};  //这个还是未知
    arp_msg.target_ip_address = target_ip;

    // 发送广播
    EthernetFrame frame;
    frame.header.src = ethernet_address_;
    frame.header.dst = ETHERNET_BROADCAST;
    frame.header.type = EthernetHeader::TYPE_ARP;
    frame.payload = serialize(arp_msg);

    transmit(frame);
    arp_requests_lifetime_[target_ip] = 0; //记录刚刚发过的ARP请求
  }
}

//! \param[in] frame the incoming Ethernet frame
void NetworkInterface::recv_frame( EthernetFrame frame )
{
  // 过滤，如果mac不是给我的，也不是广播，则丢弃
  if(frame.header.dst != ethernet_address_&&frame.header.dst != ETHERNET_BROADCAST)
    return;

  // 拆包：ipv4
  if(frame.header.type==EthernetHeader::TYPE_IPv4)
  {
    InternetDatagram dgram;
    if(parse(dgram,frame.payload)){ // 反序列化payload，然后填入dgram中
      datagrams_received_.push(dgram);
    }
  }
  // 拆包，收到ARP
  else if(frame.header.type == EthernetHeader::TYPE_ARP)
  {
    ARPMessage arp_msg;
    if(parse(arp_msg,frame.payload))
    {
      const uint32_t sender_ip = arp_msg.sender_ip_address;
      const EthernetAddress sender_mac = arp_msg.sender_ethernet_address;
      // 学习这个映射，记住30s
      arp_table_[sender_ip] = {sender_mac,30000};
      
      // 把之前的暂存包拿出来看看有没有知道mac地址的
      auto it = waiting_datagrams_.begin(); 
      while(it!=waiting_datagrams_.end())
      {
        if(it->first.ipv4_numeric()==sender_ip)
        {
          send_datagram(it->second,it->first);
          it = waiting_datagrams_.erase(it);
        }
        else 
          it++;
      }
      
      // 别人发广播，问我的IP,则回复 ARP Reply
      if(arp_msg.opcode == ARPMessage::OPCODE_REQUEST &&
      arp_msg.target_ip_address == ip_address_.ipv4_numeric())
      {
        ARPMessage reply_msg;
        reply_msg.opcode = ARPMessage::OPCODE_REPLY;
        reply_msg.sender_ethernet_address = ethernet_address_;
        reply_msg.sender_ip_address = ip_address_.ipv4_numeric(); //回复自己的ip
        reply_msg.target_ethernet_address = sender_mac;
        reply_msg.target_ip_address = sender_ip;

        EthernetFrame reply_frame;
        reply_frame.header.src = ethernet_address_;
        reply_frame.header.dst = sender_mac;
        reply_frame.header.type = EthernetHeader::TYPE_ARP;
        reply_frame.payload = serialize(reply_msg);

        transmit(reply_frame);
      }
    }
  }
}

//! \param[in] ms_since_last_tick the number of milliseconds since the last call to this method
void NetworkInterface::tick( const size_t ms_since_last_tick )
{
  // 1. 维护 ARP缓存表倒计时 ，超过30s就删掉
  // 每次调用tick都会减少 ms_since_last_tick
  // 最后一次减少前如果小于 ms_s_l_t，就可以删除了
  for(auto it = arp_table_.begin();it!=arp_table_.end();)
  {
    if(it->second.ttl<=ms_since_last_tick)
      it = arp_table_.erase(it);
    else
    {
      it->second.ttl -= ms_since_last_tick;
      ++it;
    }
  }
  // 2.维护ARP请求5秒广播冷却时间
  for(auto it = arp_requests_lifetime_.begin();it!=arp_requests_lifetime_.end();)
  {
    if(it->second + ms_since_last_tick >= 5000){
      const uint32_t expired_ip = it->first;
      // 丢弃这个所有等待这个过期IP的暂存数据包
      auto w_it = waiting_datagrams_.begin();      
      while(w_it!=waiting_datagrams_.end())
      {
        if(w_it->first.ipv4_numeric()==expired_ip)
          w_it = waiting_datagrams_.erase(w_it);
        else  
          ++w_it;
      }
      it = arp_requests_lifetime_.erase(it);

    }
    else
    {
      it->second+=ms_since_last_tick;
      ++it;
    }
  }
}
