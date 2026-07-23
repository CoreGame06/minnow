#include "wrapping_integers.hh"
#include "debug.hh"

using namespace std;

Wrap32 Wrap32::wrap( uint64_t n, Wrap32 zero_point )
{
  // 先64->32(相当于取模)，相加后uint32自动取模(溢出)
  // (a+b) mod M = [(a mol M) + (b mol M)] mol M
  return zero_point + static_cast<uint32_t>( n );
}

uint64_t Wrap32::unwrap( Wrap32 zero_point, uint64_t checkpoint ) const
{
  // L = 2**32，则候选数为 offset+K*L
  uint32_t offset = raw_value_ - zero_point.raw_value_;
  // 我们要找的就是距离上一个 checkpoint 最近的候选数
  uint32_t ck_low = static_cast<uint32_t>( checkpoint );
  // trick
  int diff = static_cast<int32_t>( offset - ck_low );
  int64_t res = diff + static_cast<int64_t>( checkpoint );

  return res < 0 ? static_cast<uint64_t>( res + ( 1ULL << 32 ) ) : static_cast<uint64_t>( res );
}
