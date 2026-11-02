// RUN: not %pycircuit_opt %s --hwir-verify 2>&1 | %FileCheck %s
#w1 = #ac.static_expr<{kind = "literal", value = {kind = "integer", value = #ac.math_int<1>}, origin = {site = {definition = @top, ast_path = []}, expansion = []}, location = {path = "hwir.py", line = 1 : i64, column = 1 : i64, end_line = 1 : i64, end_column = 2 : i64}}>
#w8 = #ac.static_expr<{kind = "literal", value = {kind = "integer", value = #ac.math_int<8>}, origin = {site = {definition = @top, ast_path = []}, expansion = []}, location = {path = "hwir.py", line = 1 : i64, column = 1 : i64, end_line = 1 : i64, end_column = 2 : i64}}>
!b1 = !ac.bits<#w1>
!b8 = !ac.bits<#w8>

module {
  hwir.module @bad(!b8) -> !b1 {
  ^bb0(%value: !b8):
    // CHECK: yielded value 0 does not match the declared type
    hwir.yield %value : !b8
  }
}
