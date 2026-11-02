# HWIR: outgoing hardware IR

HWIR is the hardware boundary that will replace the final ACIR package. The
current implementation establishes the dialect, its operation contracts and an
independent package verifier. It does **not** convert ACIR to HWIR, does not
feed either low-level emitter and does not change any public compile, link or
emit entry point.

## Package shape

A package is a `builtin.module` carrying `hwir.contract = 1 : i64`:

```mlir
module attributes {hwir.contract = 1 : i64} {
  // top-level declarations
  hwir.module @adder(...) -> ... { ... }
  hwir.system {callee = @top, parameters = [], type_arguments = []} domain "default"
}
```

Top-level operations are closed to:

| Operation | Role |
| --- | --- |
| `hwir.module` | reusable definition, signature and hardware body |
| `hwir.system` | selected root and execution domain |
| `ac.struct`, `ac.enum` | nominal payload declarations |
| `ac.module.import` | closed trusted primitive declaration |

Nominal declarations stay at package level because the shared ACIR type
verifiers require that placement (`struct requires package placement`). They
are not nested in `hwir.module`.

## Operations

| Operation | Meaning |
| --- | --- |
| `hwir.module` | inputs are block arguments, outputs are bound by `hwir.yield`; `function_type` owns port types and direction |
| `hwir.instance` | one occurrence with real SSA input/output wires and a resolved callee |
| `hwir.collection` | static family of independent occurrences with a positive shape |
| `hwir.rule` | module-local combinational calculation with explicit captures and results |
| `hwir.yield` | module output or rule result connection, never storage |
| `hwir.system` | root selection |
| `hwir.expect` | retained check with condition, path, optional sampling and check identity |
| `hwir.observe` | retained observation with path, payload and observation spec |

Storage is not redesigned: a register or memory is an ordinary instance of a
trusted import, exactly as in the final hardware contract. Final packages
forbid logical `ac.state`, so HWIR has no state/update operation.

## Shared payload and computation

HWIR reuses `!ac.bits`, `!ac.struct`, `!ac.enum`, `!ac.type_param` and
`!ac.table`. Admitted computation is an explicit whitelist, not "every `ac.*`
operation":

```
ac.bits.constant / constant4 / unary / binary / compare / select /
concat / extract / resize
ac.struct.create / struct.get
ac.enum.create / to_bits / from_bits
```

These operations have no package or stage requirement. Operations whose
verifiers require an `ac::ModuleOp` or `ac::RuleOp` parent (for example
`ac.value.require`) are deliberately excluded; they cannot be reused unchanged
inside `hwir.module` or `hwir.rule`.

## Verification split

Operation verifiers own local structure:

- module signature, block argument types and yield result types;
- rule capture/result and block argument/yield agreement;
- instance and collection callee resolution, input/output arity and payload
  types, positive collection shape;
- system entry keys, domain and root resolution;
- one-bit condition/path/sampling and check-kind constraints.

`verifyHWIRPackage` (exposed as `--hwir-verify`) owns package closure:

- contract version and placement, closed top-level operation set;
- unique definition and occurrence names;
- module body admission (structure, retained checks, whitelisted computation);
- rule bodies admit only `hwir.yield` and whitelisted computation, so retained
  checks and observations cannot hide inside a pure rule;
- exactly one `hwir.system` when the package declares a contract;
- system entry resolves to an `hwir.module` in the same package.

## Current limits

- Parameterized instantiation (`parameters` / `type_arguments`) is rejected by
  the first contract; concrete signatures only.
- Trusted imports are accepted as callees when their signature is concrete.
  Canonical DFF/DFFE/FIFO pin and configuration proof is not yet repeated by
  HWIR; it stays owned by the ACIR import verifier until the import migration.
- `hwir.collection` verifies shape and element signature only; table port
  layout and per-lane wiring are not yet part of the contract.
- No conversion, no backend consumption, no saved-format migration.

## Try it

```bash
cmake --build "$PYC_BUILD_DIR" --target pycircuit-opt HWIRIRTests --parallel 4
ctest --test-dir "$PYC_BUILD_DIR" -R '^HWIRIRTests$' --output-on-failure --no-tests=error
lit -sv --filter 'IR/HWIR/' "$PYC_BUILD_DIR/tests/compiler/lit"
```
