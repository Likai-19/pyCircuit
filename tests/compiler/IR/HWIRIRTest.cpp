#include "pycircuit/Dialect/HWIR/HWIRDialect.h"
#include "pycircuit/Dialect/HWIR/HWIRVerification.h"

#include "pycircuit/Dialect/ACIR/ACIRDialect.h"

#include "mlir/IR/BuiltinOps.h"
#include "mlir/IR/Diagnostics.h"
#include "mlir/IR/Verifier.h"
#include "mlir/Parser/Parser.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/Support/raw_ostream.h"
#include "gtest/gtest.h"

namespace {

using namespace mlir;

constexpr StringLiteral kWidthAttr = R"IR(
#w1 = #ac.static_expr<{kind = "literal", value = {kind = "integer", value = #ac.math_int<1>}, origin = {site = {definition = @top, ast_path = []}, expansion = []}, location = {path = "hwir_test.py", line = 1 : i64, column = 1 : i64, end_line = 1 : i64, end_column = 2 : i64}}>
#w8 = #ac.static_expr<{kind = "literal", value = {kind = "integer", value = #ac.math_int<8>}, origin = {site = {definition = @top, ast_path = []}, expansion = []}, location = {path = "hwir_test.py", line = 1 : i64, column = 1 : i64, end_line = 1 : i64, end_column = 2 : i64}}>
!b1 = !ac.bits<#w1>
!b8 = !ac.bits<#w8>
)IR";

std::string packageWith(StringRef body) {
  return (kWidthAttr + body).str();
}

TEST(HWIRIRTest, HandwrittenPackageVerifiesAndKeepsConnections) {
  MLIRContext context;
  context.loadDialect<acir::hwir::HWIRDialect>();
  auto source = packageWith(R"IR(
module attributes {hwir.contract = 1 : i64} {
  hwir.module @adder(!b8, !b8) -> !b8 attributes {input_names = ["lhs", "rhs"], output_names = ["sum"]} {
  ^bb0(%lhs: !b8, %rhs: !b8):
    %sum = hwir.rule "add"(%lhs, %rhs) : (!b8, !b8) -> !b8 {
    ^bb0(%x: !b8, %y: !b8):
      %computed = "ac.bits.binary"(%x, %y) {opcode = "add"} : (!b8, !b8) -> !b8
      hwir.yield %computed : !b8
    }
    hwir.yield %sum : !b8
  }
  hwir.module @top(!b8, !b8) -> !b8 {
  ^bb0(%left: !b8, %right: !b8):
    %first = hwir.instance "left" @adder(%left, %right) : (!b8, !b8) -> !b8
    %second = hwir.instance "right" @adder(%right, %left) : (!b8, !b8) -> !b8
    hwir.yield %first : !b8
  }
  hwir.system {callee = @top, parameters = [], type_arguments = []} domain "default"
}
)IR");

  ParserConfig config(&context, /*verifyAfterParse=*/true);
  auto package = parseSourceString<ModuleOp>(source, config);
  ASSERT_TRUE(package);
  ASSERT_TRUE(succeeded(verify(*package)));
  ASSERT_TRUE(succeeded(acir::hwir::verifyHWIRPackage(*package)));

  auto adder =
      dyn_cast<acir::hwir::ModuleOp>(SymbolTable::lookupSymbolIn(*package, "adder"));
  ASSERT_TRUE(adder);
  auto rule = *adder.getBody().front().getOps<acir::hwir::RuleOp>().begin();
  EXPECT_EQ(rule.getName(), "add");
  EXPECT_EQ(rule.getCaptures().size(), 2u);
  EXPECT_EQ(rule.getResults().size(), 1u);

  auto top =
      dyn_cast<acir::hwir::ModuleOp>(SymbolTable::lookupSymbolIn(*package, "top"));
  ASSERT_TRUE(top);
  auto instances =
      llvm::to_vector(top.getBody().front().getOps<acir::hwir::InstanceOp>());
  ASSERT_EQ(instances.size(), 2u);
  EXPECT_EQ(instances[0].getCallee(), "adder");
  EXPECT_EQ(instances[1].getCallee(), "adder");
  EXPECT_NE(instances[0].getOperation(), instances[1].getOperation());
  EXPECT_EQ(instances[0].getInputs()[0], top.getBody().front().getArgument(0));
  EXPECT_EQ(instances[0].getInputs()[1], top.getBody().front().getArgument(1));
  EXPECT_EQ(instances[1].getInputs()[0], top.getBody().front().getArgument(1));
  EXPECT_EQ(instances[0].getResults()[0].getType(),
            top.getBody().front().getArgument(0).getType());
}

TEST(HWIRIRTest, PrintedPackageReloadsInFreshContext) {
  MLIRContext context;
  context.loadDialect<acir::hwir::HWIRDialect>();
  auto source = packageWith(R"IR(
module attributes {hwir.contract = 1 : i64} {
  hwir.module @top(!b8) -> !b8 attributes {output_names = ["out"]} {
  ^bb0(%value: !b8):
    hwir.yield %value : !b8
  }
  hwir.system {callee = @top, parameters = [], type_arguments = []} domain "default"
}
)IR");
  ParserConfig config(&context, /*verifyAfterParse=*/true);
  auto package = parseSourceString<ModuleOp>(source, config);
  ASSERT_TRUE(package);

  std::string custom, generic;
  {
    llvm::raw_string_ostream stream(custom);
    package->print(stream);
  }
  {
    llvm::raw_string_ostream stream(generic);
    package->print(stream, OpPrintingFlags().printGenericOpForm());
  }
  for (StringRef text : {StringRef(custom), StringRef(generic)}) {
    MLIRContext fresh;
    fresh.loadDialect<acir::hwir::HWIRDialect>();
    ParserConfig freshConfig(&fresh, /*verifyAfterParse=*/true);
    auto reloaded = parseSourceString<ModuleOp>(text, freshConfig);
    ASSERT_TRUE(reloaded) << text.str();
    EXPECT_TRUE(acir::hwir::verifyHWIRPackage(*reloaded).succeeded());
  }
}

TEST(HWIRIRTest, RejectsYieldTypeMismatch) {
  MLIRContext context;
  context.loadDialect<acir::hwir::HWIRDialect>();
  ScopedDiagnosticHandler handler(
      &context, [](Diagnostic &) { return success(); });
  auto source = packageWith(R"IR(
module {
  hwir.module @bad(!b8) -> !b1 {
  ^bb0(%value: !b8):
    hwir.yield %value : !b8
  }
}
)IR");
  ParserConfig config(&context, /*verifyAfterParse=*/false);
  auto package = parseSourceString<ModuleOp>(source, config);
  ASSERT_TRUE(package);
  EXPECT_TRUE(failed(verify(*package)));
}

TEST(HWIRIRTest, RejectsUnresolvedCallee) {
  MLIRContext context;
  context.loadDialect<acir::hwir::HWIRDialect>();
  ScopedDiagnosticHandler handler(
      &context, [](Diagnostic &) { return success(); });
  auto source = packageWith(R"IR(
module {
  hwir.module @top(!b8) -> !b8 {
  ^bb0(%value: !b8):
    %only = hwir.instance "only" @missing(%value) : (!b8) -> !b8
    hwir.yield %only : !b8
  }
}
)IR");
  ParserConfig config(&context, /*verifyAfterParse=*/false);
  auto package = parseSourceString<ModuleOp>(source, config);
  ASSERT_TRUE(package);
  EXPECT_TRUE(failed(verify(*package)));
}

TEST(HWIRIRTest, PackageRejectsRetainedCheckInsideRule) {
  MLIRContext context;
  context.loadDialect<acir::hwir::HWIRDialect>();
  ScopedDiagnosticHandler handler(
      &context, [](Diagnostic &) { return success(); });
  auto source = packageWith(R"IR(
module attributes {hwir.contract = 1 : i64} {
  hwir.module @checked(!b1, !b1) -> !b1 {
  ^bb0(%condition: !b1, %path: !b1):
    %result = hwir.rule "check"(%condition, %path) : (!b1, !b1) -> !b1 {
    ^bb0(%c: !b1, %p: !b1):
      hwir.expect %c, %p : !b1, !b1 kind "assert"
      hwir.yield %c : !b1
    }
    hwir.yield %result : !b1
  }
  hwir.system {callee = @checked, parameters = [], type_arguments = []} domain "default"
}
)IR");
  ParserConfig config(&context, /*verifyAfterParse=*/false);
  auto package = parseSourceString<ModuleOp>(source, config);
  ASSERT_TRUE(package);
  // Operation verifiers accept the local structure; the package contract owns
  // the placement rule that keeps retained effects out of a pure rule.
  ASSERT_TRUE(verify(*package).succeeded());
  EXPECT_TRUE(failed(acir::hwir::verifyHWIRPackage(*package)));
}

TEST(HWIRIRTest, PackageRequiresExactlyOneSystem) {
  MLIRContext context;
  context.loadDialect<acir::hwir::HWIRDialect>();
  ScopedDiagnosticHandler handler(
      &context, [](Diagnostic &) { return success(); });
  auto source = packageWith(R"IR(
module attributes {hwir.contract = 1 : i64} {
  hwir.module @top(!b8) -> !b8 {
  ^bb0(%value: !b8):
    hwir.yield %value : !b8
  }
}
)IR");
  ParserConfig config(&context, /*verifyAfterParse=*/false);
  auto package = parseSourceString<ModuleOp>(source, config);
  ASSERT_TRUE(package);
  ASSERT_TRUE(verify(*package).succeeded());
  EXPECT_TRUE(failed(acir::hwir::verifyHWIRPackage(*package)));
}

} // namespace
