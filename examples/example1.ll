; Example 1: Simple arithmetic that could be optimized
; x * 2 can be optimized to x + x or x << 1

define i32 @mul_by_2(i32 %x) {
entry:
    %result = mul i32 %x, 2
    ret i32 %result
}

; x / 4 can be optimized to x >> 2 (for unsigned)
define i32 @div_by_4(i32 %x) {
entry:
    %result = udiv i32 %x, 4
    ret i32 %result
}

; x * 15 can be optimized to (x << 4) - x
define i32 @mul_by_15(i32 %x) {
entry:
    %result = mul i32 %x, 15
    ret i32 %result
}

; Redundant computation: (x + y) - y = x
define i32 @redundant_add_sub(i32 %x, i32 %y) {
entry:
    %sum = add i32 %x, %y
    %result = sub i32 %sum, %y
    ret i32 %result
}

; Double negation: --x = x (but expressed differently)
define i32 @double_xor(i32 %x) {
entry:
    %neg1 = xor i32 %x, -1
    %result = xor i32 %neg1, -1
    ret i32 %result
}

; Strength reduction opportunity: x % 8 = x & 7 (for unsigned)
define i32 @mod_by_8(i32 %x) {
entry:
    %result = urem i32 %x, 8
    ret i32 %result
}

; Select optimization: select(c, x, x) = x
define i32 @redundant_select(i1 %cond, i32 %x) {
entry:
    %result = select i1 %cond, i32 %x, i32 %x
    ret i32 %result
}

; Bit manipulation: (x | y) & x = x (when y doesn't matter)
define i32 @or_and_pattern(i32 %x, i32 %y) {
entry:
    %or = or i32 %x, %y
    %result = and i32 %or, %x
    ret i32 %result
}
