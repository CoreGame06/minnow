#include "tcp_receiver.hh"
#include "debug.hh"

using namespace std;

void TCPReceiver::receive( TCPSenderMessage message )
{
  // 先检查包不包含 rst，包含则重置
  if ( message.RST ) {
    reader().set_error();
    return;
  }

  // 捕获 isn
  if ( message.SYN && !isn_.has_value() )
    isn_ = message.seqno;

  // 未收到isn，拒接信息
  if ( !isn_.has_value() )
    return;

  // 计算checkpoint
  // 由于一开始的SYN占用一个字节，所以处理完N个字节后，我们下一个期望收到的字节是N+1
  const uint64_t checkpoint = reassembler_.writer().bytes_pushed() + 1;
  const uint64_t abs_seqno = message.seqno.unwrap( isn_.value(), checkpoint );

  if ( abs_seqno == 0 && !message.SYN )
    return;
  const uint64_t stream_index = abs_seqno + message.SYN - 1;

  reassembler_.insert( stream_index, message.payload, message.FIN );
}

TCPReceiverMessage TCPReceiver::send() const
{
  // 这里我们要告诉对方，我们这里是否发送错误
  // 告诉对方我们下一个期待的seqno
  // 我们的容量
  // 如果我们收到对方的FIN信号，我们也要告知
  TCPReceiverMessage msg {};
  msg.RST = reader().has_error();

  // 收到isn时
  if ( isn_.has_value() ) {
    uint64_t abs_ackno = reassembler_.writer().bytes_pushed() + 1;

    // 如果收到FIN，还要再加1
    if ( reassembler_.writer().is_closed() )
      abs_ackno += 1;

    // 打包
    msg.ackno = Wrap32::wrap( abs_ackno, isn_.value() );
  }
  const uint64_t cap = reassembler_.writer().available_capacity();
  msg.window_size = static_cast<uint16_t>( min( cap, static_cast<uint64_t>( UINT16_MAX ) ) ); // 确保类型一样
  return msg;
}
