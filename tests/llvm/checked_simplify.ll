; CHECK-LABEL: define i64 @add_zero(
; CHECK-NEXT: entry:
; CHECK-NEXT: ret i64 %x
define i64 @add_zero(i64 %x) {
entry:
  %result = call i64 @pf_checked_add(i64 %x, i64 0)
  ret i64 %result
}

; CHECK-LABEL: define i64 @sub_zero(
; CHECK-NEXT: entry:
; CHECK-NEXT: ret i64 %x
define i64 @sub_zero(i64 %x) {
entry:
  %result = call i64 @pf_checked_sub(i64 %x, i64 0)
  ret i64 %result
}

; CHECK-LABEL: define i64 @mul_one(
; CHECK-NEXT: entry:
; CHECK-NEXT: ret i64 %x
define i64 @mul_one(i64 %x) {
entry:
  %result = call i64 @pf_checked_mul(i64 %x, i64 1)
  ret i64 %result
}

; CHECK-LABEL: define i64 @div_one(
; CHECK-NEXT: entry:
; CHECK-NEXT: ret i64 %x
define i64 @div_one(i64 %x) {
entry:
  %result = call i64 @pf_checked_div(i64 %x, i64 1)
  ret i64 %result
}

; CHECK-LABEL: define i64 @mul_zero(
; CHECK-NEXT: entry:
; CHECK-NEXT: ret i64 0
define i64 @mul_zero(i64 %x) {
entry:
  %result = call i64 @pf_checked_mul(i64 %x, i64 0)
  ret i64 %result
}

; CHECK-LABEL: define i64 @mod_one(
; CHECK-NEXT: entry:
; CHECK-NEXT: ret i64 0
define i64 @mod_one(i64 %x) {
entry:
  %result = call i64 @pf_checked_mod(i64 %x, i64 1)
  ret i64 %result
}

; CHECK-LABEL: define i64 @keep_add_one(
; CHECK: call i64 @pf_checked_add(i64 %x, i64 1)
define i64 @keep_add_one(i64 %x) {
  %result = call i64 @pf_checked_add(i64 %x, i64 1)
  ret i64 %result
}

; CHECK-LABEL: define i64 @keep_mul_negative_one(
; CHECK: call i64 @pf_checked_mul(i64 %x, i64 -1)
define i64 @keep_mul_negative_one(i64 %x) {
  %result = call i64 @pf_checked_mul(i64 %x, i64 -1)
  ret i64 %result
}

; CHECK-LABEL: define i64 @keep_div_zero(
; CHECK: call i64 @pf_checked_div(i64 %x, i64 0)
define i64 @keep_div_zero(i64 %x) {
  %result = call i64 @pf_checked_div(i64 %x, i64 0)
  ret i64 %result
}

; CHECK-LABEL: define i64 @keep_mod_negative_one(
; CHECK: call i64 @pf_checked_mod(i64 %x, i64 -1)
define i64 @keep_mod_negative_one(i64 %x) {
  %result = call i64 @pf_checked_mod(i64 %x, i64 -1)
  ret i64 %result
}

; CHECK-LABEL: define i64 @keep_nonconstant(
; CHECK: call i64 @pf_checked_add(i64 %x, i64 %right)
define i64 @keep_nonconstant(i64 %x, i64 %right) {
  %result = call i64 @pf_checked_add(i64 %x, i64 %right)
  ret i64 %result
}

; CHECK-LABEL: define i64 @keep_bundled(
; CHECK: call i64 @pf_checked_add(i64 %x, i64 0) [ "deopt"(i32 7) ]
define i64 @keep_bundled(i64 %x) {
  %result = call i64 @pf_checked_add(i64 %x, i64 0) [ "deopt"(i32 7) ]
  ret i64 %result
}

; CHECK-LABEL: define i64 @keep_musttail(
; CHECK: musttail call i64 @pf_checked_add(i64 %x, i64 0)
define i64 @keep_musttail(i64 %x, i64 %unused) {
  %result = musttail call i64 @pf_checked_add(i64 %x, i64 0)
  ret i64 %result
}

; CHECK-LABEL: define i64 @keep_optnone(
; CHECK: call i64 @pf_checked_add(i64 %x, i64 0)
define i64 @keep_optnone(i64 %x) #1 {
  %result = call i64 @pf_checked_add(i64 %x, i64 0)
  ret i64 %result
}

; CHECK-LABEL: define i64 @keep_indirect(
; CHECK: call i64 %callee(i64 %x, i64 0)
define i64 @keep_indirect(ptr %callee, i64 %x) {
  %result = call i64 %callee(i64 %x, i64 0)
  ret i64 %result
}

; Opaque pointers allow a call-site function type different from the declaration.
; CHECK-LABEL: define i32 @keep_callsite_type(
; CHECK: call i32 @pf_checked_add(i64 %x, i64 0)
define i32 @keep_callsite_type(i64 %x) {
  %result = call i32 @pf_checked_add(i64 %x, i64 0)
  ret i32 %result
}

; CHECK-LABEL: define i64 @keep_invoke(
; CHECK: invoke i64 @pf_checked_add(i64 %x, i64 0)
define i64 @keep_invoke(i64 %x) personality ptr @__gxx_personality_v0 {
entry:
  %result = invoke i64 @pf_checked_add(i64 %x, i64 0) to label %returned unwind label %cleanup
returned:
  ret i64 %result
cleanup:
  %exception = landingpad { ptr, i32 } cleanup
  resume { ptr, i32 } %exception
}

declare i32 @__gxx_personality_v0(...)
declare i64 @pf_checked_add(i64, i64) #0
declare i64 @pf_checked_sub(i64, i64) #0
declare i64 @pf_checked_mul(i64, i64) #0
declare i64 @pf_checked_div(i64, i64) #0
declare i64 @pf_checked_mod(i64, i64) #0
attributes #0 = { "pikaflow.checked-arithmetic"="v1" }
attributes #1 = { noinline optnone }
