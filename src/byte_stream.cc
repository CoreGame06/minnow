#include "byte_stream.hh"
#include <queue>

using namespace std;

ByteStream::ByteStream( uint64_t capacity )
  : buffer_()
  , amount_( 0 )
  , total_pushed_( 0 )
  , total_poped_( 0 )
  , view_offset_( 0 )
  , close_( false )
  , capacity_( capacity )
  , error_( false )
{}

void Writer::push( string data )
{
  uint64_t push_len = min( static_cast<uint64_t>( data.size() ), available_capacity() );
  if ( push_len == 0 )
    return;
  // 截断字符串，只保留可以写入的字节
  if ( push_len < data.size() )
    data.resize( push_len );
  // 放入队列中
  buffer_.emplace( move( data ) );
  total_pushed_ += push_len;
  amount_ += push_len;
}

void Writer::close()
{
  // Your code here.
  close_ = true;
}

bool Writer::is_closed() const
{
  return close_; // Your code here.
}

uint64_t Writer::available_capacity() const
{
  return capacity_ - amount_; // Your code here.
}

uint64_t Writer::bytes_pushed() const
{
  return total_pushed_; // Your code here.
}

string_view Reader::peek() const
{
  if ( amount_ == 0 || buffer_.empty() )
    return {};
  return std::string_view( buffer_.front() ).substr( view_offset_ ); // 跳过被消耗的
}

void Reader::pop( uint64_t len )
{
  uint64_t pop_len = min( len, amount_ );
  amount_ -= pop_len;
  total_poped_ += pop_len;

  while ( pop_len > 0 && ( !buffer_.empty() ) ) {
    uint64_t front_remain = buffer_.front().size() - view_offset_;
    if ( pop_len >= front_remain ) {
      pop_len -= front_remain;
      buffer_.pop();
      view_offset_ = 0;
    } else {
      view_offset_ += pop_len;
      break;
    }
  }
}

bool Reader::is_finished() const
{
  return close_ && amount_ == 0;
}

uint64_t Reader::bytes_buffered() const
{
  return amount_;
}
uint64_t Reader::bytes_popped() const
{
  return total_poped_;
}
