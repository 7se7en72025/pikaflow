source_filename = "remarks.pf"

define i64 @identity(i64 %x) !dbg !10 {
  %value = call i64 @pf_checked_sub(i64 %x, i64 0), !dbg !20
  ret i64 %value, !dbg !20
}

define i64 @constant() !dbg !11 {
  %value = call i64 @pf_checked_add(i64 20, i64 22), !dbg !21
  ret i64 %value, !dbg !21
}

define i32 @overflow() !dbg !12 {
  %value = call i64 @pf_checked_add(i64 9223372036854775807, i64 1), !dbg !22
  ret i32 99, !dbg !22
}

define i32 @zero() !dbg !13 {
  %value = call i64 @pf_checked_div(i64 1, i64 0), !dbg !23
  ret i32 99, !dbg !23
}

define i64 @unknown_contract() {
  %value = call i64 @pf_checked_mod(i64 42, i64 1)
  ret i64 %value
}

define i64 @legacy_nonidentity() {
  %value = call i64 @pf_checked_sub(i64 -9223372036854775808, i64 1)
  ret i64 %value
}

define i64 @unknown_operation() {
  %value = call i64 @some_other_add(i64 20, i64 22)
  ret i64 %value
}

define i64 @disabled() #3 {
  %value = call i64 @pf_checked_add(i64 20, i64 22)
  ret i64 %value
}

define i64 @poison_operand() {
  %value = call i64 @pf_checked_add(i64 poison, i64 0)
  ret i64 %value
}

define i64 @bundled() {
  %value = call i64 @pf_checked_add(i64 20, i64 22) [ "deopt"(i32 1) ]
  ret i64 %value
}

define i32 @invalid_signature() {
  %value = call i32 @pf_checked_mul(i32 20, i32 22)
  ret i32 %value
}

define i32 @main() {
  %first = call i64 @identity(i64 42)
  %second = call i64 @constant()
  %first_ok = icmp eq i64 %first, 42
  %second_ok = icmp eq i64 %second, 42
  %ok = and i1 %first_ok, %second_ok
  %status = select i1 %ok, i32 0, i32 1
  ret i32 %status
}

declare i64 @pf_checked_sub(i64, i64) #0
declare i64 @pf_checked_add(i64, i64) #1
declare i64 @pf_checked_div(i64, i64) #1
declare i64 @pf_checked_mod(i64, i64) #2
declare i32 @pf_checked_mul(i32, i32) #1
define i64 @some_other_add(i64 %a, i64 %b) #1 {
  %value = add i64 %a, %b
  ret i64 %value
}

attributes #0 = { "pikaflow.checked-arithmetic"="v1" }
attributes #1 = { "pikaflow.checked-arithmetic"="v2" }
attributes #2 = { "pikaflow.checked-arithmetic"="v99" }
attributes #3 = { noinline optnone }

!llvm.dbg.cu = !{!0}
!llvm.module.flags = !{!8, !9}
!0 = distinct !DICompileUnit(language: DW_LANG_C, file: !1, producer: "PikaFlow remark test", isOptimized: false, runtimeVersion: 0, emissionKind: FullDebug)
!1 = !DIFile(filename: "remarks.pf", directory: "/pikaflow/tests")
!2 = !DISubroutineType(types: !3)
!3 = !{!4, !4}
!4 = !DIBasicType(name: "i64", size: 64, encoding: DW_ATE_signed)
!5 = !DISubroutineType(types: !6)
!6 = !{!4}
!7 = !DISubroutineType(types: !14)
!14 = !{!15}
!15 = !DIBasicType(name: "i32", size: 32, encoding: DW_ATE_signed)
!8 = !{i32 2, !"Dwarf Version", i32 5}
!9 = !{i32 2, !"Debug Info Version", i32 3}
!10 = distinct !DISubprogram(name: "identity", scope: !1, file: !1, line: 10, type: !2, scopeLine: 10, spFlags: DISPFlagDefinition, unit: !0)
!11 = distinct !DISubprogram(name: "constant", scope: !1, file: !1, line: 20, type: !5, scopeLine: 20, spFlags: DISPFlagDefinition, unit: !0)
!12 = distinct !DISubprogram(name: "overflow", scope: !1, file: !1, line: 30, type: !7, scopeLine: 30, spFlags: DISPFlagDefinition, unit: !0)
!13 = distinct !DISubprogram(name: "zero", scope: !1, file: !1, line: 40, type: !7, scopeLine: 40, spFlags: DISPFlagDefinition, unit: !0)
!20 = !DILocation(line: 11, column: 7, scope: !10)
!21 = !DILocation(line: 21, column: 7, scope: !11)
!22 = !DILocation(line: 31, column: 7, scope: !12)
!23 = !DILocation(line: 41, column: 7, scope: !13)
