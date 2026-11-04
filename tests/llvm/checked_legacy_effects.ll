; v1 promises no effects only for identities: a nonidentity may still log.
@effects = global i64 0

define i64 @pf_checked_add(i64 %a, i64 %b) #0 {
  %identity = icmp eq i64 %b, 0
  br i1 %identity, label %returned, label %logged
logged:
  %before = load i64, ptr @effects
  %after = add i64 %before, 1
  store i64 %after, ptr @effects
  br label %returned
returned:
  %value = add i64 %a, %b
  ret i64 %value
}

; CHECK-LABEL: define i32 @main(
; CHECK: call i64 @pf_checked_add(i64 20, i64 22)
define i32 @main() {
  %result = call i64 @pf_checked_add(i64 20, i64 22)
  %effects = load i64, ptr @effects
  %value_ok = icmp eq i64 %result, 42
  %effects_ok = icmp eq i64 %effects, 1
  %ok = and i1 %value_ok, %effects_ok
  %status = select i1 %ok, i32 0, i32 1
  ret i32 %status
}

attributes #0 = { "pikaflow.checked-arithmetic"="v1" }
