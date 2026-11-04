@effects = global i64 0

declare { i64, i1 } @llvm.sadd.with.overflow.i64(i64, i64)
declare { i64, i1 } @llvm.ssub.with.overflow.i64(i64, i64)
declare { i64, i1 } @llvm.smul.with.overflow.i64(i64, i64)
declare void @exit(i32) noreturn

define i64 @pf_checked_add(i64 %a, i64 %b) #0 {
  %pair = call { i64, i1 } @llvm.sadd.with.overflow.i64(i64 %a, i64 %b)
  %overflow = extractvalue { i64, i1 } %pair, 1
  br i1 %overflow, label %failed, label %returned
failed:
  call void @exit(i32 77)
  unreachable
returned:
  %value = extractvalue { i64, i1 } %pair, 0
  ret i64 %value
}
define i64 @pf_checked_sub(i64 %a, i64 %b) #0 {
  %pair = call { i64, i1 } @llvm.ssub.with.overflow.i64(i64 %a, i64 %b)
  %overflow = extractvalue { i64, i1 } %pair, 1
  br i1 %overflow, label %failed, label %returned
failed:
  call void @exit(i32 77)
  unreachable
returned:
  %value = extractvalue { i64, i1 } %pair, 0
  ret i64 %value
}
define i64 @pf_checked_mul(i64 %a, i64 %b) #0 {
  %pair = call { i64, i1 } @llvm.smul.with.overflow.i64(i64 %a, i64 %b)
  %overflow = extractvalue { i64, i1 } %pair, 1
  br i1 %overflow, label %failed, label %returned
failed:
  call void @exit(i32 77)
  unreachable
returned:
  %value = extractvalue { i64, i1 } %pair, 0
  ret i64 %value
}
define i64 @pf_checked_div(i64 %a, i64 %b) #0 {
  %zero = icmp eq i64 %b, 0
  %minimum = icmp eq i64 %a, -9223372036854775808
  %negative_one = icmp eq i64 %b, -1
  %overflow = and i1 %minimum, %negative_one
  %invalid = or i1 %zero, %overflow
  br i1 %invalid, label %failed, label %returned
failed:
  call void @exit(i32 77)
  unreachable
returned:
  %value = sdiv i64 %a, %b
  ret i64 %value
}
define i64 @pf_checked_mod(i64 %a, i64 %b) #0 {
  %zero = icmp eq i64 %b, 0
  %minimum = icmp eq i64 %a, -9223372036854775808
  %negative_one = icmp eq i64 %b, -1
  %overflow = and i1 %minimum, %negative_one
  %invalid = or i1 %zero, %overflow
  br i1 %invalid, label %failed, label %returned
failed:
  call void @exit(i32 77)
  unreachable
returned:
  %value = srem i64 %a, %b
  ret i64 %value
}

define i64 @producer() {
  %before = load i64, ptr @effects
  %after = add i64 %before, 1
  store i64 %after, ptr @effects
  ret i64 42
}

define i32 @main() {
  %x = call i64 @producer()
  %a = call i64 @pf_checked_add(i64 %x, i64 0)
  %s = call i64 @pf_checked_sub(i64 %x, i64 0)
  %m = call i64 @pf_checked_mul(i64 %x, i64 1)
  %d = call i64 @pf_checked_div(i64 %x, i64 1)
  %z = call i64 @pf_checked_mul(i64 %x, i64 0)
  %r = call i64 @pf_checked_mod(i64 %x, i64 1)
  %min = call i64 @pf_checked_div(i64 -9223372036854775808, i64 1)
  %max = call i64 @pf_checked_add(i64 9223372036854775807, i64 0)
  %a_ok = icmp eq i64 %a, 42
  %s_ok = icmp eq i64 %s, 42
  %m_ok = icmp eq i64 %m, 42
  %d_ok = icmp eq i64 %d, 42
  %z_ok = icmp eq i64 %z, 0
  %r_ok = icmp eq i64 %r, 0
  %min_ok = icmp eq i64 %min, -9223372036854775808
  %max_ok = icmp eq i64 %max, 9223372036854775807
  %count = load i64, ptr @effects
  %count_ok = icmp eq i64 %count, 1
  %first = and i1 %a_ok, %s_ok
  %second = and i1 %m_ok, %d_ok
  %third = and i1 %z_ok, %r_ok
  %fourth = and i1 %min_ok, %max_ok
  %half = and i1 %first, %second
  %other = and i1 %third, %fourth
  %values_ok = and i1 %half, %other
  %ok = and i1 %values_ok, %count_ok
  %status = select i1 %ok, i32 0, i32 1
  ret i32 %status
}

define i32 @trap_add() {
  %value = call i64 @pf_checked_add(i64 9223372036854775807, i64 1)
  ret i32 1
}
define i32 @trap_sub() {
  %value = call i64 @pf_checked_sub(i64 -9223372036854775808, i64 1)
  ret i32 1
}
define i32 @trap_mul() {
  %value = call i64 @pf_checked_mul(i64 -9223372036854775808, i64 -1)
  ret i32 1
}
define i32 @trap_div_zero() {
  %value = call i64 @pf_checked_div(i64 42, i64 0)
  ret i32 1
}
define i32 @trap_div_overflow() {
  %value = call i64 @pf_checked_div(i64 -9223372036854775808, i64 -1)
  ret i32 1
}
define i32 @trap_mod_zero() {
  %value = call i64 @pf_checked_mod(i64 42, i64 0)
  ret i32 1
}
define i32 @trap_mod_overflow() {
  %value = call i64 @pf_checked_mod(i64 -9223372036854775808, i64 -1)
  ret i32 1
}
define i32 @trap_before_zero() {
  %value = call i64 @pf_checked_add(i64 9223372036854775807, i64 1)
  %zero = call i64 @pf_checked_mul(i64 %value, i64 0)
  ret i32 1
}

attributes #0 = { "pikaflow.checked-arithmetic"="v1" }
