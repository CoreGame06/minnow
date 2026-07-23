#include "reassembler.hh"
#include "debug.hh"

using namespace std;

void Reassembler::crop_substring( uint64_t& first_index, std::string& data )
{
  // 记录当前buffer的右边界索引
  uint64_t first_unacceptable_index = writer().bytes_pushed() + writer().available_capacity();

  if ( first_index >= first_unacceptable_index ) {
    data.clear();
    return;
  }

  // 左边界裁切问题
  if ( first_index < first_unassembled_index_ ) {
    uint64_t overflow = first_unassembled_index_ - first_index;
    if ( overflow >= data.size() )
      data.clear();
    else {
      data = data.substr( overflow );
      first_index = first_unassembled_index_;
    }
  }

  // 右边界裁切+防止超过容量
  // 这样写不容易溢出
  if ( data.size() > first_unacceptable_index - first_index ) {
    if ( first_unacceptable_index >= first_index ) {
      uint64_t allowed_len = first_unacceptable_index - first_index;
      data = data.substr( 0, allowed_len );
      has_last_substring_ = false;
    } else
      data.clear();
  }
}

void Reassembler::merge_overlaps( uint64_t first_index, string data )
{
  // 处理map里面的重叠,前向重叠
  uint64_t new_start = first_index;
  uint64_t new_end = new_start + data.size();

  // 找到第一个比new_start大的节点
  // 这里只要处理一次就可以，而后面的重叠要用while处理多次
  auto it = unassembled_bytes_.upper_bound( new_start );
  if ( it != unassembled_bytes_.begin() ) {
    auto prev = std::prev( it );
    uint64_t prev_start = prev->first;
    uint64_t prev_end = prev_start + prev->second.size();

    if ( prev_end >= new_start ) {
      if ( prev_end >= new_end )
        return; // 否则直接跳过
      // 部分重叠
      new_start = prev_start;
      data = prev->second + data.substr( prev_end - first_index );
      pending_bytes_count_ -= prev->second.size();
      unassembled_bytes_.erase( prev );
    }
  }

  // 处理map里面的后向重叠
  while ( it != unassembled_bytes_.end() ) {
    uint64_t next_start = it->first;
    uint64_t next_end = next_start + it->second.size();
    if ( new_end >= next_start ) {
      if ( new_end < next_end ) {
        data += it->second.substr( new_end - next_start );
        new_end = next_end;
      }
      // 前去原来的
      pending_bytes_count_ -= it->second.size();
      it = unassembled_bytes_.erase( it );
    } else
      break;
  }

  // 存入map
  pending_bytes_count_ += data.size();
  unassembled_bytes_[new_start] = move( data );
}

void Reassembler::pop_to_stream()
{
  // 接下来写入buf
  while ( !unassembled_bytes_.empty() && unassembled_bytes_.begin()->first == first_unassembled_index_ ) {
    auto node = unassembled_bytes_.begin();
    string str = node->second;
    output_.writer().push( str );
    first_unassembled_index_ += str.size();
    pending_bytes_count_ -= str.size();
    unassembled_bytes_.erase( node );
  }
}

void Reassembler::insert( uint64_t first_index, string data, bool is_last_substring )
{
  // 如果收到EOF包，记录流的终点位置
  if ( is_last_substring ) {
    has_last_substring_ = true;
    last_index_ = first_index + data.size();
  }
  // 1.剪裁
  crop_substring( first_index, data );
  // 2. 如果剪裁后有数据，则合并并插入 map
  if ( !data.empty() )
    merge_overlaps( first_index, move( data ) );
  // 3.推送到bytestream
  pop_to_stream();
  // 4.检查是否关闭流
  if ( has_last_substring_ && first_unassembled_index_ == last_index_ )
    // has_last_substring 确认终点是否收到
    // 第二个判据的弊端 0==0
    output_.writer().close();
}

// How many bytes are stored in the Reassembler itself?
// This function is for testing only; don't add extra state to support it.
uint64_t Reassembler::count_bytes_pending() const
{
  return pending_bytes_count_;
}
