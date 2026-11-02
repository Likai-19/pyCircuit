// RUN: %pycircuit_opt %s --hwir-verify | %FileCheck %s
// RUN: %pycircuit_opt %s --hwir-verify | %pycircuit_opt --hwir-verify | %FileCheck %s --check-prefix=RELOAD
#w1 = #ac.static_expr<{kind = "literal", value = {kind = "integer", value = #ac.math_int<1>}, origin = {site = {definition = @top, ast_path = []}, expansion = []}, location = {path = "hwir.py", line = 1 : i64, column = 1 : i64, end_line = 1 : i64, end_column = 2 : i64}}>
#w8 = #ac.static_expr<{kind = "literal", value = {kind = "integer", value = #ac.math_int<8>}, origin = {site = {definition = @top, ast_path = []}, expansion = []}, location = {path = "hwir.py", line = 1 : i64, column = 1 : i64, end_line = 1 : i64, end_column = 2 : i64}}>
!b1 = !ac.bits<#w1>
!b8 = !ac.bits<#w8>

module attributes {hwir.contract = 1 : i64} {
  // CHECK: hwir.module @adder(
  // RELOAD: hwir.module @adder(
  hwir.module @adder(!b8, !b8) -> !b8 attributes {input_names = ["lhs", "rhs"], output_names = ["sum"]} {
  ^bb0(%lhs: !b8, %rhs: !b8):
    // CHECK: hwir.rule "add"
    %sum = hwir.rule "add"(%lhs, %rhs) : (!b8, !b8) -> !b8 {
    ^bb0(%x: !b8, %y: !b8):
      %computed = "ac.bits.binary"(%x, %y) {opcode = "add"} : (!b8, !b8) -> !b8
      hwir.yield %computed : !b8
    }
    hwir.yield %sum : !b8
  }

  hwir.module @top(!b8, !b8) -> !b8 {
  ^bb0(%left: !b8, %right: !b8):
    // CHECK: hwir.instance "left" @adder
    // CHECK: hwir.instance "right" @adder
    %first = hwir.instance "left" @adder(%left, %right) : (!b8, !b8) -> !b8
    %second = hwir.instance "right" @adder(%right, %left) : (!b8, !b8) -> !b8
    hwir.yield %first : !b8
  }

  // CHECK: hwir.module @monitored(
  // CHECK: hwir.expect
  // CHECK: hwir.observe
  hwir.module @monitored(!b1, !b1) -> !b1 {
  ^bb0(%condition: !b1, %path: !b1):
    hwir.expect %condition, %path : !b1, !b1 kind "assert"
    hwir.observe %path, %condition : !b1, !b1 kind "trace" attributes {spec = {name = "sample"}}
    hwir.yield %condition : !b1
  }

  // CHECK: hwir.system {callee = @top
  hwir.system {callee = @top, parameters = [], type_arguments = []} domain "default"
}
