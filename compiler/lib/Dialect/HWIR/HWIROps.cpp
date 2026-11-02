#include "pycircuit/Dialect/HWIR/HWIRDialect.h"

#include "pycircuit/Dialect/ACIR/ACIROps.h"

#include "llvm/ADT/APSInt.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringSet.h"

#include <optional>

using namespace mlir;

namespace acir::hwir {
namespace {

bool isHardwareType(Type type) {
  return isa<ac::BitsType, ac::StructType, ac::EnumType, ac::TypeParamType,
             ac::TableType>(type);
}

LogicalResult verifyHardwareTypes(Operation *operation, TypeRange types,
                                  StringRef description) {
  for (Type type : types)
    if (!isHardwareType(type))
      return operation->emitOpError()
             << description << " requires a hardware payload type";
  return success();
}

std::optional<llvm::APSInt> literalInteger(ac::StaticExprAttr expression) {
  if (!expression)
    return std::nullopt;
  DictionaryAttr tree = expression.getTree();
  auto kind = tree ? tree.getAs<StringAttr>("kind") : StringAttr();
  auto value = tree ? tree.getAs<DictionaryAttr>("value") : DictionaryAttr();
  auto valueKind = value ? value.getAs<StringAttr>("kind") : StringAttr();
  auto integer = value ? value.getAs<ac::MathIntAttr>("value") : ac::MathIntAttr();
  if (!kind || kind.getValue() != "literal" || !valueKind ||
      valueKind.getValue() != "integer" || !integer)
    return std::nullopt;
  return llvm::APSInt(integer.getCanonicalValue());
}

bool isOneBit(Type type) {
  auto bits = dyn_cast<ac::BitsType>(type);
  if (!bits)
    return false;
  auto width = literalInteger(bits.getWidth());
  return width && !width->isNegative() && width->getLimitedValue() == 1;
}

void printTypeList(OpAsmPrinter &printer, TypeRange types) {
  llvm::interleaveComma(types, printer,
                        [&](Type type) { printer.printType(type); });
}

void printResultTypes(OpAsmPrinter &printer, TypeRange results) {
  if (results.size() == 1) {
    printer.printType(results.front());
    return;
  }
  printer << "(";
  printTypeList(printer, results);
  printer << ")";
}

ParseResult parseParenTypeList(OpAsmParser &parser,
                               SmallVectorImpl<Type> &types) {
  if (parser.parseLParen())
    return failure();
  if (succeeded(parser.parseOptionalRParen()))
    return success();
  if (parser.parseTypeList(types) || parser.parseRParen())
    return failure();
  return success();
}

ParseResult parseResultTypes(OpAsmParser &parser,
                             SmallVectorImpl<Type> &results) {
  if (succeeded(parser.parseOptionalLParen())) {
    if (succeeded(parser.parseOptionalRParen()))
      return success();
    if (parser.parseTypeList(results) || parser.parseRParen())
      return failure();
    return success();
  }
  Type type;
  if (parser.parseType(type))
    return failure();
  results.push_back(type);
  return success();
}

ParseResult parseOptionalAttributes(OpAsmParser &parser,
                                    OperationState &result) {
  if (succeeded(parser.parseOptionalKeyword("attributes")))
    return parser.parseOptionalAttrDict(result.attributes);
  return success();
}

bool hasPrintableAttributes(Operation *operation, ArrayRef<StringRef> elided) {
  for (NamedAttribute attribute : operation->getAttrs())
    if (!llvm::is_contained(elided, attribute.getName().getValue()))
      return true;
  return false;
}

void printAttributes(OpAsmPrinter &printer, Operation *operation,
                     ArrayRef<StringRef> elided) {
  if (!hasPrintableAttributes(operation, elided))
    return;
  printer << " attributes";
  printer.printOptionalAttrDict(operation->getAttrs(), elided);
}

LogicalResult verifyNameList(Operation *operation, ArrayAttr names,
                             unsigned expected, StringRef description) {
  if (!names)
    return success();
  if (names.size() != expected)
    return operation->emitOpError()
           << description << " must name every signature position";
  llvm::StringSet<> seen;
  for (Attribute raw : names) {
    auto name = dyn_cast<StringAttr>(raw);
    if (!name || name.getValue().empty())
      return operation->emitOpError()
             << description << " requires nonempty string names";
    if (!seen.insert(name.getValue()).second)
      return operation->emitOpError()
             << description << " contains duplicate name '" << name.getValue()
             << "'";
  }
  return success();
}

LogicalResult verifyBlockSignature(Operation *operation, Block &block,
                                   TypeRange expected, StringRef description) {
  if (block.getNumArguments() != expected.size())
    return operation->emitOpError()
           << description << " count does not match its block arguments";
  for (auto [index, type] : llvm::enumerate(expected))
    if (block.getArgument(index).getType() != type)
      return operation->emitOpError()
             << description << " type mismatch at position " << index;
  return success();
}

LogicalResult verifyYieldValues(Operation *operation, YieldOp yield,
                                TypeRange expected) {
  if (yield.getValues().size() != expected.size())
    return operation->emitOpError()
           << "must yield " << expected.size() << " value(s), got "
           << yield.getValues().size();
  for (auto [index, type] : llvm::enumerate(expected))
    if (yield.getValues()[index].getType() != type)
      return operation->emitOpError()
             << "yielded value " << index << " does not match the declared type";
  return success();
}

LogicalResult verifyCallee(Operation *operation, FlatSymbolRefAttr callee,
                           TypeRange inputs, TypeRange outputs,
                           ArrayAttr parameters, ArrayAttr typeArguments) {
  Operation *target = SymbolTable::lookupNearestSymbolFrom(operation, callee);
  if (!target)
    return operation->emitOpError()
           << "callee '" << callee.getValue() << "' does not resolve";

  FunctionType signature;
  if (auto module = dyn_cast<ModuleOp>(target)) {
    auto declared = [](const std::optional<ArrayAttr> &list) {
      return list && list->size() != 0;
    };
    if (declared(module.getParameters()) ||
        declared(module.getTypeParameters()))
      return operation->emitOpError()
             << "parameterized callee instantiation is not part of the first "
                "HWIR contract";
    signature = module.getFunctionType();
  } else if (auto imported = dyn_cast<ac::ModuleImportOp>(target)) {
    if (!imported.getTypeParameters().empty())
      return operation->emitOpError()
             << "type-parameterized import instantiation is not part of the "
                "first HWIR contract";
    signature = imported.getFunctionType();
  } else if (isa<ac::ModuleOp>(target)) {
    return operation->emitOpError()
           << "final ACIR modules are not HWIR callees";
  } else {
    return operation->emitOpError()
           << "callee '" << callee.getValue()
           << "' is not an HWIR module or trusted import";
  }

  if ((parameters && !parameters.empty()) ||
      (typeArguments && !typeArguments.empty()))
    return operation->emitOpError()
           << "static bindings are not part of the first HWIR contract";

  if (inputs.size() != signature.getNumInputs())
    return operation->emitOpError()
           << "input count does not match the callee declaration";
  for (auto [index, type] : llvm::enumerate(signature.getInputs()))
    if (inputs[index] != type)
      return operation->emitOpError()
             << "input " << index << " type does not match the callee";
  if (outputs.size() != signature.getNumResults())
    return operation->emitOpError()
           << "result count does not match the callee declaration";
  for (auto [index, type] : llvm::enumerate(signature.getResults()))
    if (outputs[index] != type)
      return operation->emitOpError()
             << "result " << index << " type does not match the callee";
  return success();
}

} // namespace

// ---------------------------------------------------------------------------
// hwir.module
// ---------------------------------------------------------------------------

ParseResult ModuleOp::parse(OpAsmParser &parser, OperationState &result) {
  StringAttr name;
  if (parser.parseSymbolName(name, "sym_name", result.attributes))
    return failure();
  SmallVector<Type> inputs, outputs;
  if (parseParenTypeList(parser, inputs))
    return failure();
  if (parser.parseArrow())
    return failure();
  if (parseResultTypes(parser, outputs))
    return failure();
  if (parseOptionalAttributes(parser, result))
    return failure();
  result.addAttribute("function_type",
                      TypeAttr::get(FunctionType::get(parser.getContext(),
                                                      inputs, outputs)));
  Region *body = result.addRegion();
  return parser.parseRegion(*body);
}

void ModuleOp::print(OpAsmPrinter &printer) {
  printer << " ";
  printer.printSymbolName(getSymName());
  printer << "(";
  printTypeList(printer, getFunctionType().getInputs());
  printer << ") -> ";
  printResultTypes(printer, getFunctionType().getResults());
  printAttributes(printer, *this, {"sym_name", "function_type"});
  printer << " ";
  printer.printRegion(getBody());
}

LogicalResult ModuleOp::verify() {
  if (getSymName().empty())
    return emitOpError("requires a nonempty symbol name");

  FunctionType signature = getFunctionType();
  if (failed(verifyHardwareTypes(*this, signature.getInputs(),
                                 "module input")))
    return failure();
  if (failed(verifyHardwareTypes(*this, signature.getResults(),
                                 "module output")))
    return failure();
  if (failed(verifyNameList(*this, getInputNamesAttr(),
                            signature.getNumInputs(), "input_names")))
    return failure();
  if (failed(verifyNameList(*this, getOutputNamesAttr(),
                            signature.getNumResults(), "output_names")))
    return failure();

  if (getBody().empty())
    return emitOpError("requires a body region");
  Block &block = getBody().front();
  if (failed(verifyBlockSignature(*this, block, signature.getInputs(),
                                  "module input")))
    return failure();
  auto yield = dyn_cast<YieldOp>(block.getTerminator());
  if (!yield)
    return emitOpError("module body must end with hwir.yield");
  return verifyYieldValues(*this, yield, signature.getResults());
}

// ---------------------------------------------------------------------------
// hwir.rule
// ---------------------------------------------------------------------------

ParseResult RuleOp::parse(OpAsmParser &parser, OperationState &result) {
  StringAttr name;
  if (parser.parseAttribute(name, "name", result.attributes))
    return failure();
  SmallVector<OpAsmParser::UnresolvedOperand> captures;
  SmallVector<Type> captureTypes, resultTypes;
  if (parser.parseLParen() || parser.parseOperandList(captures) ||
      parser.parseRParen())
    return failure();
  if (parser.parseColon() || parseParenTypeList(parser, captureTypes))
    return failure();
  if (parser.resolveOperands(captures, captureTypes,
                             parser.getCurrentLocation(), result.operands))
    return failure();
  if (parser.parseArrow() || parseResultTypes(parser, resultTypes))
    return failure();
  result.addTypes(resultTypes);
  if (parseOptionalAttributes(parser, result))
    return failure();
  Region *body = result.addRegion();
  return parser.parseRegion(*body);
}

void RuleOp::print(OpAsmPrinter &printer) {
  printer << " ";
  printer.printAttributeWithoutType(getNameAttr());
  printer << "(";
  printer.printOperands(getCaptures());
  printer << ") : (";
  printTypeList(printer, getCaptures().getTypes());
  printer << ") -> ";
  printResultTypes(printer, getResults().getTypes());
  printAttributes(printer, *this, {"name"});
  printer << " ";
  printer.printRegion(getBody());
}

LogicalResult RuleOp::verify() {
  if (getName().empty())
    return emitOpError("requires a nonempty rule name");
  if (failed(verifyHardwareTypes(*this, getCaptures().getTypes(), "capture")))
    return failure();
  if (failed(verifyHardwareTypes(*this, getResults().getTypes(), "result")))
    return failure();
  if (getBody().empty())
    return emitOpError("requires a body region");
  Block &block = getBody().front();
  if (failed(verifyBlockSignature(*this, block, getCaptures().getTypes(),
                                  "rule capture")))
    return failure();
  auto yield = dyn_cast<YieldOp>(block.getTerminator());
  if (!yield)
    return emitOpError("rule body must end with hwir.yield");
  return verifyYieldValues(*this, yield, getResults().getTypes());
}

// ---------------------------------------------------------------------------
// hwir.instance / hwir.collection
// ---------------------------------------------------------------------------

ParseResult InstanceOp::parse(OpAsmParser &parser, OperationState &result) {
  StringAttr name;
  FlatSymbolRefAttr callee;
  if (parser.parseAttribute(name, "instance_name", result.attributes) ||
      parser.parseAttribute(callee, "callee", result.attributes))
    return failure();
  SmallVector<OpAsmParser::UnresolvedOperand> inputs;
  SmallVector<Type> inputTypes, outputTypes;
  if (parser.parseLParen() || parser.parseOperandList(inputs) ||
      parser.parseRParen())
    return failure();
  if (parser.parseColon() || parseParenTypeList(parser, inputTypes))
    return failure();
  if (parser.resolveOperands(inputs, inputTypes, parser.getCurrentLocation(),
                             result.operands))
    return failure();
  if (parser.parseArrow() || parseResultTypes(parser, outputTypes))
    return failure();
  result.addTypes(outputTypes);
  return parseOptionalAttributes(parser, result);
}

void InstanceOp::print(OpAsmPrinter &printer) {
  printer << " ";
  printer.printAttributeWithoutType(getInstanceNameAttr());
  printer << " ";
  printer.printAttributeWithoutType(getCalleeAttr());
  printer << "(";
  printer.printOperands(getInputs());
  printer << ") : (";
  printTypeList(printer, getInputs().getTypes());
  printer << ") -> ";
  printResultTypes(printer, getResults().getTypes());
  printAttributes(printer, *this, {"instance_name", "callee"});
}

LogicalResult InstanceOp::verify() {
  if (getInstanceName().empty())
    return emitOpError("requires a nonempty instance name");
  return verifyCallee(*this, getCalleeAttr(), getInputs().getTypes(),
                      getResults().getTypes(), getParametersAttr(),
                      getTypeArgumentsAttr());
}

ParseResult CollectionOp::parse(OpAsmParser &parser, OperationState &result) {
  StringAttr name;
  FlatSymbolRefAttr callee;
  if (parser.parseAttribute(name, "instance_name", result.attributes) ||
      parser.parseAttribute(callee, "callee", result.attributes))
    return failure();
  SmallVector<OpAsmParser::UnresolvedOperand> inputs;
  SmallVector<Type> inputTypes, outputTypes;
  if (parser.parseLParen() || parser.parseOperandList(inputs) ||
      parser.parseRParen())
    return failure();
  if (parser.parseColon() || parseParenTypeList(parser, inputTypes))
    return failure();
  if (parser.resolveOperands(inputs, inputTypes, parser.getCurrentLocation(),
                             result.operands))
    return failure();
  if (parser.parseArrow() || parseResultTypes(parser, outputTypes))
    return failure();
  result.addTypes(outputTypes);
  return parseOptionalAttributes(parser, result);
}

void CollectionOp::print(OpAsmPrinter &printer) {
  printer << " ";
  printer.printAttributeWithoutType(getInstanceNameAttr());
  printer << " ";
  printer.printAttributeWithoutType(getCalleeAttr());
  printer << "(";
  printer.printOperands(getInputs());
  printer << ") : (";
  printTypeList(printer, getInputs().getTypes());
  printer << ") -> ";
  printResultTypes(printer, getResults().getTypes());
  printAttributes(printer, *this, {"instance_name", "callee"});
}

LogicalResult CollectionOp::verify() {
  if (getInstanceName().empty())
    return emitOpError("requires a nonempty instance name");
  if (getShape().size() == 0)
    return emitOpError("requires a nonempty static shape");
  for (Attribute raw : getShape()) {
    auto dimension = dyn_cast<IntegerAttr>(raw);
    if (!dimension || dimension.getValue().isNegative() ||
        dimension.getValue().isZero())
      return emitOpError("requires positive static shape dimensions");
  }
  return verifyCallee(*this, getCalleeAttr(), getInputs().getTypes(),
                      getResults().getTypes(), getParametersAttr(),
                      getTypeArgumentsAttr());
}

// ---------------------------------------------------------------------------
// hwir.yield
// ---------------------------------------------------------------------------

ParseResult YieldOp::parse(OpAsmParser &parser, OperationState &result) {
  SmallVector<OpAsmParser::UnresolvedOperand> values;
  if (parser.parseOperandList(values))
    return failure();
  if (values.size() != 0) {
    SmallVector<Type> types;
    if (parser.parseColonTypeList(types))
      return failure();
    if (parser.resolveOperands(values, types, parser.getCurrentLocation(),
                               result.operands))
      return failure();
  }
  return parseOptionalAttributes(parser, result);
}

void YieldOp::print(OpAsmPrinter &printer) {
  printer << " ";
  printer.printOperands(getValues());
  if (getValues().size() != 0) {
    printer << " : ";
    printTypeList(printer, getValues().getTypes());
  }
  printAttributes(printer, *this, {});
}

LogicalResult YieldOp::verify() {
  Operation *parent = (*this)->getParentOp();
  if (auto module = dyn_cast<ModuleOp>(parent))
    return verifyYieldValues(*this, *this, module.getFunctionType().getResults());
  if (auto rule = dyn_cast<RuleOp>(parent))
    return verifyYieldValues(*this, *this, rule.getResults().getTypes());
  return emitOpError("must terminate an HWIR module or rule");
}

// ---------------------------------------------------------------------------
// hwir.system
// ---------------------------------------------------------------------------

LogicalResult SystemOp::verify() {
  DictionaryAttr entry = getEntry();
  auto callee = entry.getAs<FlatSymbolRefAttr>("callee");
  auto parameters = entry.getAs<ArrayAttr>("parameters");
  auto typeArguments = entry.getAs<ArrayAttr>("type_arguments");
  if (!callee || !parameters || !typeArguments)
    return emitOpError()
           << "entry requires callee, parameters and type_arguments";
  for (NamedAttribute attribute : entry)
    if (attribute.getName() != "callee" &&
        attribute.getName() != "parameters" &&
        attribute.getName() != "type_arguments")
      return emitOpError() << "entry has unsupported key '"
                           << attribute.getName().getValue() << "'";
  if (getDomain().empty())
    return emitOpError("requires a nonempty domain");
  if (parameters.size() != 0 || typeArguments.size() != 0)
    return emitOpError()
           << "static system bindings are not part of the first HWIR contract";
  Operation *target =
      SymbolTable::lookupNearestSymbolFrom(*this, callee);
  if (!isa_and_nonnull<ModuleOp>(target))
    return emitOpError() << "entry '" << callee.getValue()
                         << "' does not resolve to an HWIR module";
  return success();
}

// ---------------------------------------------------------------------------
// hwir.expect / hwir.observe
// ---------------------------------------------------------------------------

ParseResult ExpectOp::parse(OpAsmParser &parser, OperationState &result) {
  SmallVector<OpAsmParser::UnresolvedOperand> operands;
  SmallVector<Type> types;
  if (parser.parseOperandList(operands))
    return failure();
  if (parser.parseColonTypeList(types))
    return failure();
  if (parser.resolveOperands(operands, types, parser.getCurrentLocation(),
                             result.operands))
    return failure();
  StringAttr kind;
  if (parser.parseKeyword("kind") ||
      parser.parseAttribute(kind, "kind", result.attributes))
    return failure();
  return parseOptionalAttributes(parser, result);
}

void ExpectOp::print(OpAsmPrinter &printer) {
  printer << " ";
  printer.printOperands(getOperation()->getOperands());
  printer << " : ";
  printTypeList(printer, getOperation()->getOperandTypes());
  printer << " kind ";
  printer.printAttributeWithoutType(getKindAttr());
  printAttributes(printer, *this, {"kind"});
}

LogicalResult ExpectOp::verify() {
  if (!isOneBit(getCondition().getType()))
    return emitOpError("condition requires a one-bit hardware wire");
  if (!isOneBit(getPath().getType()))
    return emitOpError("path requires a one-bit hardware wire");
  for (Value sampling : getSampling())
    if (!isOneBit(sampling.getType()))
      return emitOpError("sampling requires one-bit hardware wires");

  StringRef kind = getKind();
  if (kind != "assert" && kind != "range" && kind != "guard_known")
    return emitOpError() << "unknown check kind '" << kind << "'";
  if (kind == "guard_known" && getSampling().size() != 2)
    return emitOpError()
           << "guard_known requires clock then reset sampling operands";
  if (kind != "guard_known" && !getSampling().empty())
    return emitOpError() << kind << " does not accept sampling operands";
  return success();
}

ParseResult ObserveOp::parse(OpAsmParser &parser, OperationState &result) {
  SmallVector<OpAsmParser::UnresolvedOperand> operands;
  SmallVector<Type> types;
  if (parser.parseOperandList(operands))
    return failure();
  if (parser.parseColonTypeList(types))
    return failure();
  if (parser.resolveOperands(operands, types, parser.getCurrentLocation(),
                             result.operands))
    return failure();
  StringAttr kind;
  if (parser.parseKeyword("kind") ||
      parser.parseAttribute(kind, "kind", result.attributes))
    return failure();
  return parseOptionalAttributes(parser, result);
}

void ObserveOp::print(OpAsmPrinter &printer) {
  printer << " ";
  printer.printOperands(getOperation()->getOperands());
  printer << " : ";
  printTypeList(printer, getOperation()->getOperandTypes());
  printer << " kind ";
  printer.printAttributeWithoutType(getKindAttr());
  printAttributes(printer, *this, {"kind"});
}

LogicalResult ObserveOp::verify() {
  if (!isOneBit(getPath().getType()))
    return emitOpError("path requires a one-bit hardware wire");
  if (getKind().empty())
    return emitOpError("requires a nonempty observation kind");
  return verifyHardwareTypes(*this, getValues().getTypes(),
                             "observation value");
}

} // namespace acir::hwir
