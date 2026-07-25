#include "address.hh"
#include "arp_message.hh"
#include "bidirectional_stream_copy.hh"
#include "eventloop.hh"
#include "exception.hh"
#include "helpers.hh"
#include "router.hh"
#include "socket.hh"
#include "tcp_minnow_socket_impl.hh"
#include "tcp_over_ip.hh"

#include <atomic>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <random>
#include <string>
#include <thread>
#include <utility>

using namespace std;

namespace {
EthernetAddress random_host_ethernet_address()
{
  EthernetAddress addr;
  for ( auto& byte : addr ) {
    byte = random_device()();
  }
  addr.at( 0 ) |= 0x02;
  addr.at( 0 ) &= 0xfe;
  return addr;
}

optional<EthernetFrame> maybe_receive_frame( FileDescriptor& fd )
{
  vector<string> strs( 4 );
  strs.at( 0 ).resize( EthernetHeader::LENGTH );
  strs.at( 1 ).resize( IPv4Header::LENGTH );
  strs.at( 2 ).resize( TCPSegment::HEADER_LENGTH );
  fd.read( strs );

  EthernetFrame frame;
  if ( not parse( frame, move( strs ) ) ) {
    return {};
  }
  return frame;
}

inline pair<FileDescriptor, FileDescriptor> make_socket_pair()
{
  array<int, 2> fds {};
  CheckSystemCall( "socketpair", ::socketpair( AF_UNIX, SOCK_DGRAM, 0, fds.data() ) );
  return { FileDescriptor { fds[0] }, FileDescriptor { fds[1] } };
}

class NetworkInterfaceAdapter : public TCPOverIPv4Adapter
{
private:
  struct Sender : public NetworkInterface::OutputPort
  {
    pair<FileDescriptor, FileDescriptor> sockets { make_socket_pair() };

    void transmit( const NetworkInterface&, const EthernetFrame& x ) override
    {
      sockets.first.write( serialize( x ) );
    }
  };

  shared_ptr<Sender> sender_ = make_shared<Sender>();
  NetworkInterface _interface;
  Address _next_hop;

public:
  NetworkInterfaceAdapter( const Address& ip_address, const Address& next_hop )
    : _interface( "tcp_eth_udp adapter", sender_, random_host_ethernet_address(), ip_address )
    , _next_hop( next_hop )
  {}

  optional<TCPMessage> read()
  {
    auto frame_opt = maybe_receive_frame( sender_->sockets.first );
    if ( not frame_opt ) {
      return {};
    }

    _interface.recv_frame( move( frame_opt.value() ) );

    if ( _interface.datagrams_received().empty() ) {
      return {};
    }

    InternetDatagram dgram = move( _interface.datagrams_received().front() );
    _interface.datagrams_received().pop();
    return unwrap_tcp_in_ip( move( dgram ) );
  }

  void write( const TCPMessage& msg ) { _interface.send_datagram( wrap_tcp_in_ip( msg ), _next_hop ); }
  void tick( const size_t ms_since_last_tick ) { _interface.tick( ms_since_last_tick ); }
  NetworkInterface& interface() { return _interface; }

  FileDescriptor& fd() { return sender_->sockets.first; }
  FileDescriptor& frame_fd() { return sender_->sockets.second; }
};

class TCPSocketEthUdp : public TCPMinnowSocket<NetworkInterfaceAdapter>
{
  Address _local_address;

public:
  TCPSocketEthUdp( const Address& ip_address, const Address& next_hop )
    : TCPMinnowSocket<NetworkInterfaceAdapter>( NetworkInterfaceAdapter( ip_address, next_hop ) )
    , _local_address( ip_address )
  {}

  void connect( const Address& address )
  {
    FdAdapterConfig multiplexer_config;
    _local_address = Address { _local_address.ip(), static_cast<uint16_t>( random_device()() ) };
    multiplexer_config.source = _local_address;
    multiplexer_config.destination = address;
    TCPMinnowSocket<NetworkInterfaceAdapter>::connect( {}, multiplexer_config );
  }

  void bind( const Address& address )
  {
    if ( address.ip() != _local_address.ip() ) {
      throw runtime_error( "Cannot bind to " + address.to_string() );
    }
    _local_address = Address { _local_address.ip(), address.port() };
  }

  void listen_and_accept()
  {
    FdAdapterConfig multiplexer_config;
    multiplexer_config.source = _local_address;
    TCPMinnowSocket<NetworkInterfaceAdapter>::listen_and_accept( {}, multiplexer_config );
  }

  NetworkInterfaceAdapter& adapter() { return _datagram_adapter; }
};

void print_usage( const string& argv0 )
{
  cerr << "Usage:\n"
       << "  Server mode: " << argv0 << " server <listen_udp_port> <gateway_virtual_ip> <server_virtual_ip:port>\n"
       << "  Client mode: " << argv0 << " client <listen_udp_port> <target_udp_ip:port> <gateway_virtual_ip> <client_virtual_ip> <server_virtual_ip:port>\n";
}
} // namespace

int main( int argc, char* argv[] )
{
  try {
    if ( argc < 4 ) {
      print_usage( argv[0] );
      return EXIT_FAILURE;
    }

    string mode( argv[1] );
    bool is_client = ( mode == "client" );

    uint16_t listen_udp_port = static_cast<uint16_t>( stoi( argv[2] ) );
    Address target_udp_addr { "0.0.0.0", 0 };
    Address gateway_virt_ip { "0.0.0.0", 0 };
    Address self_virt_addr { "0.0.0.0", 0 };
    Address peer_virt_addr { "0.0.0.0", 0 };

    if ( is_client ) {
      if ( argc < 7 ) {
        print_usage( argv[0] );
        return EXIT_FAILURE;
      }
      string target_udp_str( argv[3] );
      auto pos = target_udp_str.find( ':' );
      if ( pos == string::npos ) {
        throw runtime_error( "Target UDP address format should be ip:port" );
      }
      target_udp_addr = Address( target_udp_str.substr( 0, pos ), static_cast<uint16_t>( stoi( target_udp_str.substr( pos + 1 ) ) ) );
      gateway_virt_ip = Address( argv[4], 0 );
      self_virt_addr = Address( argv[5], 0 );

      string peer_str( argv[6] );
      auto peer_pos = peer_str.find( ':' );
      if ( peer_pos == string::npos ) {
        throw runtime_error( "Server virtual address format should be ip:port" );
      }
      peer_virt_addr = Address( peer_str.substr( 0, peer_pos ), static_cast<uint16_t>( stoi( peer_str.substr( peer_pos + 1 ) ) ) );
    } else {
      if ( argc < 5 ) {
        print_usage( argv[0] );
        return EXIT_FAILURE;
      }
      gateway_virt_ip = Address( argv[3], 0 );
      string self_str( argv[4] );
      auto self_pos = self_str.find( ':' );
      if ( self_pos == string::npos ) {
        throw runtime_error( "Server virtual address format should be ip:port" );
      }
      self_virt_addr = Address( self_str.substr( 0, self_pos ), static_cast<uint16_t>( stoi( self_str.substr( self_pos + 1 ) ) ) );
    }

    UDPSocket udp_sock;
    udp_sock.bind( Address( "0.0.0.0", listen_udp_port ) );

    TCPSocketEthUdp sock { self_virt_addr, gateway_virt_ip };
    atomic<bool> exit_flag { false };

    thread network_thread( [&]() {
      try {
        EventLoop event_loop;

        event_loop.add_rule( "frames from adapter to UDP socket", sock.adapter().frame_fd(), Direction::In, [&] {
          auto frame_opt = maybe_receive_frame( sock.adapter().frame_fd() );
          if ( not frame_opt ) {
            return;
          }
          if ( target_udp_addr.port() != 0 ) {
            string payload;
            for ( const auto& chunk : serialize( frame_opt.value() ) ) {
              payload.append( chunk );
            }
            udp_sock.sendto( target_udp_addr, payload );
          }
        } );

        event_loop.add_rule( "frames from UDP socket to adapter", udp_sock, Direction::In, [&] {
          Address src_addr { "0.0.0.0", 0 };
          string payload;
          udp_sock.recv( src_addr, payload );
          if ( !is_client ) {
            target_udp_addr = src_addr;
          }
          EthernetFrame frame;
          vector<string> strs = { payload };
          if ( parse( frame, move( strs ) ) ) {
            sock.adapter().frame_fd().write( serialize( frame ) );
          }
        } );

        while ( !exit_flag ) {
          if ( EventLoop::Result::Exit == event_loop.wait_next_event( 10 ) ) {
            break;
          }
          sock.adapter().tick( 10 );
        }
      } catch ( const exception& e ) {
        cerr << "Network thread exception: " << e.what() << "\n";
      }
    } );

    if ( is_client ) {
      sock.connect( peer_virt_addr );
    } else {
      sock.bind( self_virt_addr );
      sock.listen_and_accept();
    }

    bidirectional_stream_copy( sock, self_virt_addr.to_string() );
    sock.wait_until_closed();

    exit_flag = true;
    network_thread.join();

  } catch ( const exception& e ) {
    cerr << "Error: " << e.what() << "\n";
    return EXIT_FAILURE;
  }
  return EXIT_SUCCESS;
}
