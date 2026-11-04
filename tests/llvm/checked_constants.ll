; v2 constant boundary cases, executed against the real PikaFlow runtime.

; CHECK-LABEL: define i64 @add_answer(
; CHECK-NEXT: ret i64 42
define i64 @add_answer() {
  %result = call i64 @pf_checked_add(i64 20, i64 22)
  ret i64 %result
}

; CHECK-LABEL: define i64 @add_high(
; CHECK-NEXT: ret i64 9223372036854775807
define i64 @add_high() {
  %result = call i64 @pf_checked_add(i64 9223372036854775806, i64 1)
  ret i64 %result
}

; CHECK-LABEL: define i64 @add_low(
; CHECK-NEXT: ret i64 -9223372036854775808
define i64 @add_low() {
  %result = call i64 @pf_checked_add(i64 -9223372036854775807, i64 -1)
  ret i64 %result
}

; CHECK-LABEL: define i64 @add_cancel(
; CHECK-NEXT: ret i64 -1
define i64 @add_cancel() {
  %result = call i64 @pf_checked_add(i64 -9223372036854775808, i64 9223372036854775807)
  ret i64 %result
}

; CHECK-LABEL: define i64 @sub_answer(
; CHECK-NEXT: ret i64 42
define i64 @sub_answer() {
  %result = call i64 @pf_checked_sub(i64 64, i64 22)
  ret i64 %result
}

; CHECK-LABEL: define i64 @sub_low(
; CHECK-NEXT: ret i64 -9223372036854775807
define i64 @sub_low() {
  %result = call i64 @pf_checked_sub(i64 -9223372036854775808, i64 -1)
  ret i64 %result
}

; CHECK-LABEL: define i64 @sub_high(
; CHECK-NEXT: ret i64 9223372036854775806
define i64 @sub_high() {
  %result = call i64 @pf_checked_sub(i64 9223372036854775807, i64 1)
  ret i64 %result
}

; CHECK-LABEL: define i64 @mul_answer(
; CHECK-NEXT: ret i64 42
define i64 @mul_answer() {
  %result = call i64 @pf_checked_mul(i64 -6, i64 -7)
  ret i64 %result
}

; CHECK-LABEL: define i64 @mul_signed(
; CHECK-NEXT: ret i64 -42
define i64 @mul_signed() {
  %result = call i64 @pf_checked_mul(i64 7, i64 -6)
  ret i64 %result
}

; CHECK-LABEL: define i64 @mul_square(
; CHECK-NEXT: ret i64 9223372030926249001
define i64 @mul_square() {
  %result = call i64 @pf_checked_mul(i64 3037000499, i64 3037000499)
  ret i64 %result
}

; CHECK-LABEL: define i64 @div_answer(
; CHECK-NEXT: ret i64 42
define i64 @div_answer() {
  %result = call i64 @pf_checked_div(i64 84, i64 2)
  ret i64 %result
}

; CHECK-LABEL: define i64 @div_negative_left(
; CHECK-NEXT: ret i64 -42
define i64 @div_negative_left() {
  %result = call i64 @pf_checked_div(i64 -85, i64 2)
  ret i64 %result
}

; CHECK-LABEL: define i64 @div_negative_right(
; CHECK-NEXT: ret i64 -42
define i64 @div_negative_right() {
  %result = call i64 @pf_checked_div(i64 85, i64 -2)
  ret i64 %result
}

; CHECK-LABEL: define i64 @div_both_negative(
; CHECK-NEXT: ret i64 42
define i64 @div_both_negative() {
  %result = call i64 @pf_checked_div(i64 -85, i64 -2)
  ret i64 %result
}

; CHECK-LABEL: define i64 @div_low(
; CHECK-NEXT: ret i64 -4611686018427387904
define i64 @div_low() {
  %result = call i64 @pf_checked_div(i64 -9223372036854775808, i64 2)
  ret i64 %result
}

; CHECK-LABEL: define i64 @mod_positive(
; CHECK-NEXT: ret i64 1
define i64 @mod_positive() {
  %result = call i64 @pf_checked_mod(i64 85, i64 2)
  ret i64 %result
}

; CHECK-LABEL: define i64 @mod_negative_left(
; CHECK-NEXT: ret i64 -1
define i64 @mod_negative_left() {
  %result = call i64 @pf_checked_mod(i64 -85, i64 2)
  ret i64 %result
}

; CHECK-LABEL: define i64 @mod_negative_right(
; CHECK-NEXT: ret i64 1
define i64 @mod_negative_right() {
  %result = call i64 @pf_checked_mod(i64 85, i64 -2)
  ret i64 %result
}

; CHECK-LABEL: define i64 @mod_both_negative(
; CHECK-NEXT: ret i64 -1
define i64 @mod_both_negative() {
  %result = call i64 @pf_checked_mod(i64 -85, i64 -2)
  ret i64 %result
}

; CHECK-LABEL: define i64 @mod_low(
; CHECK-NEXT: ret i64 -2
define i64 @mod_low() {
  %result = call i64 @pf_checked_mod(i64 -9223372036854775808, i64 3)
  ret i64 %result
}

; CHECK-LABEL: define i64 @add_overflow_high(
; CHECK: call i64 @pf_checked_add(i64 9223372036854775807, i64 1)
define i64 @add_overflow_high() {
  %result = call i64 @pf_checked_add(i64 9223372036854775807, i64 1)
  ret i64 %result
}

; CHECK-LABEL: define i64 @add_overflow_low(
; CHECK: call i64 @pf_checked_add(i64 -9223372036854775808, i64 -1)
define i64 @add_overflow_low() {
  %result = call i64 @pf_checked_add(i64 -9223372036854775808, i64 -1)
  ret i64 %result
}

; CHECK-LABEL: define i64 @sub_overflow_high(
; CHECK: call i64 @pf_checked_sub(i64 9223372036854775807, i64 -1)
define i64 @sub_overflow_high() {
  %result = call i64 @pf_checked_sub(i64 9223372036854775807, i64 -1)
  ret i64 %result
}

; CHECK-LABEL: define i64 @sub_overflow_low(
; CHECK: call i64 @pf_checked_sub(i64 -9223372036854775808, i64 1)
define i64 @sub_overflow_low() {
  %result = call i64 @pf_checked_sub(i64 -9223372036854775808, i64 1)
  ret i64 %result
}

; CHECK-LABEL: define i64 @mul_overflow_high(
; CHECK: call i64 @pf_checked_mul(i64 9223372036854775807, i64 2)
define i64 @mul_overflow_high() {
  %result = call i64 @pf_checked_mul(i64 9223372036854775807, i64 2)
  ret i64 %result
}

; CHECK-LABEL: define i64 @mul_overflow_low(
; CHECK: call i64 @pf_checked_mul(i64 -9223372036854775808, i64 2)
define i64 @mul_overflow_low() {
  %result = call i64 @pf_checked_mul(i64 -9223372036854775808, i64 2)
  ret i64 %result
}

; CHECK-LABEL: define i64 @mul_overflow_minimum(
; CHECK: call i64 @pf_checked_mul(i64 -1, i64 -9223372036854775808)
define i64 @mul_overflow_minimum() {
  %result = call i64 @pf_checked_mul(i64 -1, i64 -9223372036854775808)
  ret i64 %result
}

; CHECK-LABEL: define i64 @mul_overflow_square(
; CHECK: call i64 @pf_checked_mul(i64 3037000500, i64 3037000500)
define i64 @mul_overflow_square() {
  %result = call i64 @pf_checked_mul(i64 3037000500, i64 3037000500)
  ret i64 %result
}

; CHECK-LABEL: define i64 @div_zero(
; CHECK: call i64 @pf_checked_div(i64 1, i64 0)
define i64 @div_zero() {
  %result = call i64 @pf_checked_div(i64 1, i64 0)
  ret i64 %result
}

; CHECK-LABEL: define i64 @div_zero_zero(
; CHECK: call i64 @pf_checked_div(i64 0, i64 0)
define i64 @div_zero_zero() {
  %result = call i64 @pf_checked_div(i64 0, i64 0)
  ret i64 %result
}

; CHECK-LABEL: define i64 @div_overflow(
; CHECK: call i64 @pf_checked_div(i64 -9223372036854775808, i64 -1)
define i64 @div_overflow() {
  %result = call i64 @pf_checked_div(i64 -9223372036854775808, i64 -1)
  ret i64 %result
}

; CHECK-LABEL: define i64 @mod_zero(
; CHECK: call i64 @pf_checked_mod(i64 1, i64 0)
define i64 @mod_zero() {
  %result = call i64 @pf_checked_mod(i64 1, i64 0)
  ret i64 %result
}

; CHECK-LABEL: define i64 @mod_zero_zero(
; CHECK: call i64 @pf_checked_mod(i64 0, i64 0)
define i64 @mod_zero_zero() {
  %result = call i64 @pf_checked_mod(i64 0, i64 0)
  ret i64 %result
}

; CHECK-LABEL: define i64 @mod_overflow(
; CHECK: call i64 @pf_checked_mod(i64 -9223372036854775808, i64 -1)
define i64 @mod_overflow() {
  %result = call i64 @pf_checked_mod(i64 -9223372036854775808, i64 -1)
  ret i64 %result
}

; CHECK-LABEL: define i64 @constant_chain(
; CHECK-NEXT: ret i64 1
define i64 @constant_chain() {
  %a = call i64 @pf_checked_add(i64 20, i64 22)
  %s = call i64 @pf_checked_sub(i64 %a, i64 2)
  %m = call i64 @pf_checked_mul(i64 %s, i64 2)
  %d = call i64 @pf_checked_div(i64 %m, i64 5)
  %r = call i64 @pf_checked_mod(i64 %d, i64 3)
  ret i64 %r
}

; CHECK-LABEL: define i64 @keep_undef_left(
; CHECK: call i64 @pf_checked_mul(i64 undef, i64 0)
define i64 @keep_undef_left() {
  %result = call i64 @pf_checked_mul(i64 undef, i64 0)
  ret i64 %result
}

; CHECK-LABEL: define i64 @keep_undef_right(
; CHECK: call i64 @pf_checked_mul(i64 42, i64 undef)
define i64 @keep_undef_right() {
  %result = call i64 @pf_checked_mul(i64 42, i64 undef)
  ret i64 %result
}

; CHECK-LABEL: define i64 @keep_poison_left(
; CHECK: call i64 @pf_checked_mul(i64 poison, i64 0)
define i64 @keep_poison_left() {
  %result = call i64 @pf_checked_mul(i64 poison, i64 0)
  ret i64 %result
}

; CHECK-LABEL: define i64 @keep_poison_right(
; CHECK: call i64 @pf_checked_mul(i64 42, i64 poison)
define i64 @keep_poison_right() {
  %result = call i64 @pf_checked_mul(i64 42, i64 poison)
  ret i64 %result
}

@where = private constant [18 x i8] c"checked-plugin.ll\00"
declare void @pf_source_location(ptr, i32, i32)

define i32 @main(i32 %argc, ptr %argv) {
entry:
  call void @pf_source_location(ptr @where, i32 77, i32 9)
  switch i32 %argc, label %safe [
    i32 2, label %add_overflow_high
    i32 3, label %add_overflow_low
    i32 4, label %sub_overflow_high
    i32 5, label %sub_overflow_low
    i32 6, label %mul_overflow_high
    i32 7, label %mul_overflow_low
    i32 8, label %mul_overflow_minimum
    i32 9, label %mul_overflow_square
    i32 10, label %div_zero
    i32 11, label %div_zero_zero
    i32 12, label %div_overflow
    i32 13, label %mod_zero
    i32 14, label %mod_zero_zero
    i32 15, label %mod_overflow
  ]
safe:
  %value0 = call i64 @add_answer()
  %ok0 = icmp eq i64 %value0, 42
  %value1 = call i64 @add_high()
  %ok1 = icmp eq i64 %value1, 9223372036854775807
  %value2 = call i64 @add_low()
  %ok2 = icmp eq i64 %value2, -9223372036854775808
  %value3 = call i64 @add_cancel()
  %ok3 = icmp eq i64 %value3, -1
  %value4 = call i64 @sub_answer()
  %ok4 = icmp eq i64 %value4, 42
  %value5 = call i64 @sub_low()
  %ok5 = icmp eq i64 %value5, -9223372036854775807
  %value6 = call i64 @sub_high()
  %ok6 = icmp eq i64 %value6, 9223372036854775806
  %value7 = call i64 @mul_answer()
  %ok7 = icmp eq i64 %value7, 42
  %value8 = call i64 @mul_signed()
  %ok8 = icmp eq i64 %value8, -42
  %value9 = call i64 @mul_square()
  %ok9 = icmp eq i64 %value9, 9223372030926249001
  %value10 = call i64 @div_answer()
  %ok10 = icmp eq i64 %value10, 42
  %value11 = call i64 @div_negative_left()
  %ok11 = icmp eq i64 %value11, -42
  %value12 = call i64 @div_negative_right()
  %ok12 = icmp eq i64 %value12, -42
  %value13 = call i64 @div_both_negative()
  %ok13 = icmp eq i64 %value13, 42
  %value14 = call i64 @div_low()
  %ok14 = icmp eq i64 %value14, -4611686018427387904
  %value15 = call i64 @mod_positive()
  %ok15 = icmp eq i64 %value15, 1
  %value16 = call i64 @mod_negative_left()
  %ok16 = icmp eq i64 %value16, -1
  %value17 = call i64 @mod_negative_right()
  %ok17 = icmp eq i64 %value17, 1
  %value18 = call i64 @mod_both_negative()
  %ok18 = icmp eq i64 %value18, -1
  %value19 = call i64 @mod_low()
  %ok19 = icmp eq i64 %value19, -2
  %chain = call i64 @constant_chain()
  %okchain = icmp eq i64 %chain, 1
  %all0 = and i1 %okchain, %ok0
  %all1 = and i1 %all0, %ok1
  %all2 = and i1 %all1, %ok2
  %all3 = and i1 %all2, %ok3
  %all4 = and i1 %all3, %ok4
  %all5 = and i1 %all4, %ok5
  %all6 = and i1 %all5, %ok6
  %all7 = and i1 %all6, %ok7
  %all8 = and i1 %all7, %ok8
  %all9 = and i1 %all8, %ok9
  %all10 = and i1 %all9, %ok10
  %all11 = and i1 %all10, %ok11
  %all12 = and i1 %all11, %ok12
  %all13 = and i1 %all12, %ok13
  %all14 = and i1 %all13, %ok14
  %all15 = and i1 %all14, %ok15
  %all16 = and i1 %all15, %ok16
  %all17 = and i1 %all16, %ok17
  %all18 = and i1 %all17, %ok18
  %all19 = and i1 %all18, %ok19
  %status = select i1 %all19, i32 0, i32 1
  ret i32 %status
add_overflow_high:
  %fatal_add_overflow_high = call i64 @add_overflow_high()
  ret i32 99
add_overflow_low:
  %fatal_add_overflow_low = call i64 @add_overflow_low()
  ret i32 99
sub_overflow_high:
  %fatal_sub_overflow_high = call i64 @sub_overflow_high()
  ret i32 99
sub_overflow_low:
  %fatal_sub_overflow_low = call i64 @sub_overflow_low()
  ret i32 99
mul_overflow_high:
  %fatal_mul_overflow_high = call i64 @mul_overflow_high()
  ret i32 99
mul_overflow_low:
  %fatal_mul_overflow_low = call i64 @mul_overflow_low()
  ret i32 99
mul_overflow_minimum:
  %fatal_mul_overflow_minimum = call i64 @mul_overflow_minimum()
  ret i32 99
mul_overflow_square:
  %fatal_mul_overflow_square = call i64 @mul_overflow_square()
  ret i32 99
div_zero:
  %fatal_div_zero = call i64 @div_zero()
  ret i32 99
div_zero_zero:
  %fatal_div_zero_zero = call i64 @div_zero_zero()
  ret i32 99
div_overflow:
  %fatal_div_overflow = call i64 @div_overflow()
  ret i32 99
mod_zero:
  %fatal_mod_zero = call i64 @mod_zero()
  ret i32 99
mod_zero_zero:
  %fatal_mod_zero_zero = call i64 @mod_zero_zero()
  ret i32 99
mod_overflow:
  %fatal_mod_overflow = call i64 @mod_overflow()
  ret i32 99
}

declare i64 @pf_checked_add(i64, i64) #0
declare i64 @pf_checked_sub(i64, i64) #0
declare i64 @pf_checked_mul(i64, i64) #0
declare i64 @pf_checked_div(i64, i64) #0
declare i64 @pf_checked_mod(i64, i64) #0
attributes #0 = { "pikaflow.checked-arithmetic"="v2" }
