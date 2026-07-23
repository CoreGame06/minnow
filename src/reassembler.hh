#pragma once

#include "byte_stream.hh"
#include <map>

class Reassembler
{
public:
  // Construct Reassembler to write into given ByteStream.
  explicit Reassembler( ByteStream&& output ) : output_( std::move( output ) ) {}

  /*
   * Insert a new substring to be reassembled into a ByteStream.
   *   `first_index`: the index of the first byte of the substring
   *   `data`: the substring itself
   *   `is_last_substring`: this substring represents the end of the stream
   *   `output`: a mutable reference to the Writer
   *
   * The Reassembler's job is to reassemble the indexed substrings (possibly out-of-order
   * and possibly overlapping) back into the original ByteStream. As soon as the Reassembler
   * learns the next byte in the stream, it should write it to the output.
   *
   * If the Reassembler learns about bytes that fit within the stream's available capacity
   * but can't yet be written (because earlier bytes remain unknown), it should store them
   * internally until the gaps are filled in.
   *
   * The Reassembler should discard any bytes that lie beyond the stream's available capacity
   * (i.e., bytes that couldn't be written even if earlier gaps get filled in).
   *
   * The Reassembler should close the stream after writing the last byte.
   */
  void insert( uint64_t first_index, std::string data, bool is_last_substring );

  // How many bytes are stored in the Reassembler itself?
  // This function is for testing only; don't add extra state to support it.
  uint64_t count_bytes_pending() const;

  // Access output stream reader
  Reader& reader() { return output_.reader(); }
  const Reader& reader() const { return output_.reader(); }

  // Access output stream writer, but const-only (can't write from outside)
  const Writer& writer() const { return output_.writer(); }

private:
  ByteStream output_; // 目标字节流，目前收到的index符合则直接写入
  std::map<uint64_t, std::string> unassembled_bytes_ {}; // index不符合时，暂存在这里
  uint64_t first_unassembled_index_ { 0 };               // 下一个期待收到的index
  bool has_last_substring_ { false };                    // 记录是否收到了最后一个EOF
  uint64_t last_index_ { 0 };                            // 全局流的总字节数，结束索引
  uint64_t pending_bytes_count_ { 0 };                   // map中的函数

  // 几个辅助函数，模块化
  void crop_substring( uint64_t& first_index, std::string& data );
  void merge_overlaps( uint64_t first_index, std::string data );
  void pop_to_stream();
};
