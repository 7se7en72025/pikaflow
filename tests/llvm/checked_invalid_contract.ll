; These modules are valid LLVM IR but do not satisfy the checked-runtime contract.
; CHECK-LABEL: define i32 @wrong_return(
; CHECK: call i32 @pf_checked_add(i64 %x, i64 0)
define i32 @wrong_return(i64 %x) {
  %result = call i32 @pf_checked_add(i64 %x, i64 0)
  ret i32 %result
}
; CHECK-LABEL: define i64 @wrong_argument(
; CHECK: call i64 @pf_checked_mul(i1 %x, i64 1)
define i64 @wrong_argument(i1 %x) {
  %result = call i64 @pf_checked_mul(i1 %x, i64 1)
  ret i64 %result
}
; CHECK-LABEL: define i64 @variadic(
; CHECK: call i64 (i64, ...) @pf_checked_sub(i64 %x, i64 0)
define i64 @variadic(i64 %x) {
  %result = call i64 (i64, ...) @pf_checked_sub(i64 %x, i64 0)
  ret i64 %result
}
; CHECK-LABEL: define i64 @wrong_calling_convention(
; CHECK: call fastcc i64 @pf_checked_div(i64 %x, i64 1)
define i64 @wrong_calling_convention(i64 %x) {
  %result = call fastcc i64 @pf_checked_div(i64 %x, i64 1)
  ret i64 %result
}
; CHECK-LABEL: define i64 @wrong_version(
; CHECK: call i64 @pf_checked_mod(i64 %x, i64 1)
define i64 @wrong_version(i64 %x) {
  %result = call i64 @pf_checked_mod(i64 %x, i64 1)
  ret i64 %result
}
; CHECK-LABEL: define i64 @wrong_name(
; CHECK: call i64 @some_other_add(i64 %x, i64 0)
define i64 @wrong_name(i64 %x) {
  %result = call i64 @some_other_add(i64 %x, i64 0)
  ret i64 %result
}
declare i32 @pf_checked_add(i64, i64) #0
declare i64 @pf_checked_mul(i1, i64) #0
declare i64 @pf_checked_sub(i64, ...) #0
declare fastcc i64 @pf_checked_div(i64, i64) #0
declare i64 @pf_checked_mod(i64, i64) #1
declare i64 @some_other_add(i64, i64) #0
attributes #0 = { "pikaflow.checked-arithmetic"="v2" }
attributes #1 = { "pikaflow.checked-arithmetic"="v99" }
