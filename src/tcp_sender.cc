#include "tcp_sender.hh"
#include "debug.hh"
#include "tcp_config.hh"

using namespace std;

// This function is for testing only; don't add extra state to support it.
uint64_t TCPSender::sequence_numbers_in_flight() const
{
  return next_abs_seqno_ - ack_abs_seqno_;
}

// This function is for testing only; don't add extra state to support it.
uint64_t TCPSender::consecutive_retransmissions() const
{
  return consecutive_retransmissions_;
}

void TCPSender::push( const TransmitFunction& transmit )
{
  // 1.计算窗口大小，窗口大小为0时，当成1
  const uint64_t effective_window = window_size_ == 0 ? 1 : window_size_;

  // 2. 不断发送数据，但是如果现在的curr_in_flight > effective_window，则停止
  while ( true ) {
    const uint64_t curr_in_flight = next_abs_seqno_ - ack_abs_seqno_;
    if ( curr_in_flight >= effective_window )
      break;

    // 计算当前窗口还可以再装多少
    const uint64_t win_free = effective_window - curr_in_flight;

    // 初始化一个发送信息
    TCPSenderMessage msg {};

    if ( reader().has_error() )
      msg.RST = true;
    // 处理SYN标志与最大负荷（网卡和win_free制约）
    if ( next_abs_seqno_ == 0 )
      msg.SYN = true;

    const uint64_t max_payload = min( static_cast<uint64_t>( TCPConfig::MAX_PAYLOAD_SIZE ), win_free - msg.SYN );

    // 从bytestream中获取数据到msg.payload中
    if ( reader().bytes_buffered() > 0 && max_payload > 0 ) {
      string payload {};
      while ( reader().bytes_buffered() > 0 && payload.size() < max_payload ) {
        string_view chunk = reader().peek();
        const uint64_t to_read = min( chunk.size(), max_payload - payload.size() );
        payload.append( chunk.substr( 0, to_read ) );
        input_.reader().pop( to_read ); // 真正弹出字节
      }
      msg.payload = move( payload );
    }
    // FIN标志
    if ( !fin_sent_ && reader().is_finished() && msg.sequence_length() < win_free ) {
      msg.FIN = true;
      fin_sent_ = true;
    }

    if ( msg.sequence_length() == 0 )
      break;

    // 打包序列号
    msg.seqno = Wrap32::wrap( next_abs_seqno_, isn_ );
    // 发送
    transmit( msg );
    // 加入等待队列
    outstanding_segments_.push( msg );
    // 下一个绝对序列号
    next_abs_seqno_ += msg.sequence_length();

    // 如果定时器没开，则按下闹钟
    if ( !timer_running_ ) {
      timer_running_ = true;
      timer_ms_ = 0;
    }

    // 若发送了FIN，则不再发送。push函数只负责第一次发送
    if ( msg.FIN )
      break;
  }
}

TCPSenderMessage TCPSender::make_empty_message() const
{
  // 发送一个空包，确认ack或者告诉对方我们这里出错
  TCPSenderMessage msg {};
  msg.seqno = Wrap32::wrap( next_abs_seqno_, isn_ );
  if ( reader().has_error() )
    msg.RST = true;
  return msg;
}

void TCPSender::receive( const TCPReceiverMessage& msg )
{
  if ( msg.RST ) {
    reader().set_error();
    return;
  }
  // 更新窗口大小
  window_size_ = msg.window_size;
  // 检验ackno
  if ( !msg.ackno.has_value() )
    return;
  // 解包ack
  const uint64_t new_ack = msg.ackno.value().unwrap( isn_, next_abs_seqno_ );
  // 处理非法情况
  if ( new_ack > next_abs_seqno_ )
    return;

  // 收到全新的ack
  if ( new_ack > ack_abs_seqno_ ) {
    ack_abs_seqno_ = new_ack;
    // 弹出已经确认的
    while ( !outstanding_segments_.empty() ) {
      const auto& seg = outstanding_segments_.front();
      const uint64_t seg_abs_seqno = seg.seqno.unwrap( isn_, next_abs_seqno_ );
      if ( seg_abs_seqno + seg.sequence_length() <= ack_abs_seqno_ )
        outstanding_segments_.pop();
      else
        break;
    }

    // 收到新ack，说明网络不拥堵，可以重置这两个参数
    current_RTO_ms_ = initial_RTO_ms_;
    consecutive_retransmissions_ = 0;

    timer_running_ = !outstanding_segments_.empty();
    timer_ms_ = 0;
    
  }
}

void TCPSender::tick( uint64_t ms_since_last_tick, const TransmitFunction& transmit )
{
  if ( !timer_running_ )
    return;
  // 每隔一段时间，调用tick函数，累加时间
  timer_ms_ += ms_since_last_tick;

  if ( timer_ms_ >= current_RTO_ms_ ) {
    if ( !outstanding_segments_.empty() ) {
      transmit( outstanding_segments_.front() );
      if ( window_size_ != 0 ) {
        consecutive_retransmissions_++;
        current_RTO_ms_ *= 2;
      }
    }
    timer_ms_ = 0;
  }
}
