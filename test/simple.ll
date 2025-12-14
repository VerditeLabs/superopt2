; Simple test module for superoptimizer
define i32 @mul_by_2(i32 %x) {
  %r = mul i32 %x, 2
  ret i32 %r
}

define i32 @div_by_4(i32 %x) {
  %r = sdiv i32 %x, 4
  ret i32 %r
}

define i32 @add_zero(i32 %x) {
  %r = add i32 %x, 0
  ret i32 %r
}
