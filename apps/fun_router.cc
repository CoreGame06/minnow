#include "address.hh"
#include "eventloop.hh"
#include "exception.hh"
#include "helpers.hh"
#include "router.hh"
#include "socket.hh"
#include "tcp_segment.hh"

#include <cstdlib>
#include <iostream>
#include <map>
#include <memory>
#include <random>
#include <sstream>
#include <string>
#include <vector>

using namespace std;

namespace {
EthernetAddress random_router_ethernet_address()
{
  EthernetAddress addr;
  for ( auto& byte : addr ) {
    byte = random_device()();
  }
  addr.at( 0 ) = 0x02;
  addr.at( 1 ) = 0;
  addr.at( 2 ) = 0;
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

struct UdpSender : public NetworkInterface::OutputPort
{
  shared_ptr<UDPSocket> socket;
  Address target_address;

  UdpSender( shared_ptr<UDPSocket> sock, const Address& target )
    : socket( move( sock ) ), target_address( target )
  {}

  void transmit( const NetworkInterface&, const EthernetFrame& x ) override
  {
    string payload;
    for ( const auto& chunk : serialize( x ) ) {
      payload.append( chunk );
    }
    socket->sendto( target_address, payload );
  }
};

void print_usage( const string& argv0 )
{
  cerr << "Usage: " << argv0
       << " interface:name:virtual_ip:listen_port:target_ip:target_port ... "
          "route:prefix_ip:prefix_len:interface_name [next_hop_ip] ...\n";
}
} // namespace

int main( int argc, char* argv[] )
{
  try {
    if ( argc < 2 ) {
      print_usage( argv[0] );
      return EXIT_FAILURE;
    }

    Router router;
    map<string, size_t> interface_names {};
    vector<pair<shared_ptr<NetworkInterface>, shared_ptr<UDPSocket>>> interfaces {};
    EventLoop event_loop;

    for ( int i = 1; i < argc; i++ ) {
      string arg( argv[i] );
      if ( arg.rfind( "interface:", 0 ) == 0 ) {
        stringstream ss( arg.substr( 10 ) );
        string name, virt_ip_str, listen_port_str, target_ip_str, target_port_str;

        getline( ss, name, ':' );
        getline( ss, virt_ip_str, ':' );
        getline( ss, listen_port_str, ':' );
        getline( ss, target_ip_str, ':' );
        getline( ss, target_port_str, ':' );

        if ( name.empty() || virt_ip_str.empty() || listen_port_str.empty() || target_ip_str.empty()
             || target_port_str.empty() ) {
          throw runtime_error( "Invalid interface format: " + arg );
        }

        Address virt_ip { virt_ip_str, 0 };
        uint16_t listen_port = static_cast<uint16_t>( stoi( listen_port_str ) );
        Address target_addr { target_ip_str, static_cast<uint16_t>( stoi( target_port_str ) ) };

        auto udp_sock = make_shared<UDPSocket>();
        udp_sock->bind( Address( "0.0.0.0", listen_port ) );

        auto sender = make_shared<UdpSender>( udp_sock, target_addr );
        auto net_if = make_shared<NetworkInterface>( name, sender, random_router_ethernet_address(), virt_ip );

        size_t if_num = router.add_interface( net_if );
        interface_names[name] = if_num;
        interfaces.push_back( { net_if, udp_sock } );

        cerr << "Added interface '" << name << "' (idx " << if_num << ") virt_ip=" << virt_ip.ip()
             << " listening on UDP " << listen_port << " forwarding to " << target_addr.to_string() << "\n";

      } else if ( arg.rfind( "route:", 0 ) == 0 ) {
        stringstream ss( arg.substr( 6 ) );
        string prefix_ip_str, prefix_len_str, if_name, next_hop_str;

        getline( ss, prefix_ip_str, ':' );
        getline( ss, prefix_len_str, ':' );
        getline( ss, if_name, ':' );

        if ( prefix_ip_str.empty() || prefix_len_str.empty() || if_name.empty() ) {
          throw runtime_error( "Invalid route format: " + arg );
        }

        uint32_t prefix_ip = Address( prefix_ip_str, 0 ).ipv4_numeric();
        uint8_t prefix_len = static_cast<uint8_t>( stoi( prefix_len_str ) );

        if ( interface_names.find( if_name ) == interface_names.end() ) {
          throw runtime_error( "Unknown interface in route: " + if_name );
        }
        size_t if_num = interface_names[if_name];

        optional<Address> next_hop {};
        if ( i + 1 < argc && argv[i + 1][0] != '-' && string( argv[i + 1] ).find( ':' ) == string::npos ) {
          next_hop = Address( argv[i + 1], 0 );
          i++;
        }

        router.add_route( prefix_ip, prefix_len, next_hop, if_num );
        cerr << "Added route " << prefix_ip_str << "/" << static_cast<int>( prefix_len ) << " via interface '"
             << if_name << "'" << ( next_hop.has_value() ? " next_hop=" + next_hop->ip() : "" ) << "\n";

      } else {
        throw runtime_error( "Unrecognized argument: " + arg );
      }
    }

    for ( auto& [net_if, sock] : interfaces ) {
      event_loop.add_rule( "incoming frames on " + net_if->name(), *sock, Direction::In, [&, net_if = net_if, sock = sock] {
        auto frame_opt = maybe_receive_frame( *sock );
        if ( not frame_opt ) {
          return;
        }
        net_if->recv_frame( move( frame_opt.value() ) );
        router.route();
      } );
    }

    cerr << "fun_router running. Press Ctrl+C to exit.\n";
    while ( true ) {
      if ( EventLoop::Result::Exit == event_loop.wait_next_event( 10 ) ) {
        break;
      }
      for ( auto& [net_if, sock] : interfaces ) {
        net_if->tick( 10 );
      }
    }
  } catch ( const exception& e ) {
    cerr << "Error: " << e.what() << "\n";
    return EXIT_FAILURE;
  }
  return EXIT_SUCCESS;
}
