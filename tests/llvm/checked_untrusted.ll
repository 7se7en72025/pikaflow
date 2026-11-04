@effects = global i64 0

; The spelling alone is not a license to erase this side effect.
define i64 @pf_checked_add(i64 %x, i64 %right) {
  %before = load i64, ptr @effects
  %after = add i64 %before, 1
  store i64 %after, ptr @effects
  %result = add i64 %x, %right
  ret i64 %result
}

; CHECK-LABEL: define i32 @main(
; CHECK: call i64 @pf_checked_add(i64 42, i64 0)
define i32 @main() {
  %result = call i64 @pf_checked_add(i64 42, i64 0)
  %count = load i64, ptr @effects
  %value_ok = icmp eq i64 %result, 42
  %effect_ok = icmp eq i64 %count, 1
  %ok = and i1 %value_ok, %effect_ok
  %status = select i1 %ok, i32 0, i32 1
  ret i32 %status
}
