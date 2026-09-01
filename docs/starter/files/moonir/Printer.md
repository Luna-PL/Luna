# src/moonir/Printer.cpp

Implementation of Printer.h: prints Module / TypeRecord / declaration / metadata as human-readable text.

## What This File Does

print() emits the output in sections:

- Header: module name and version, active features (runtime, kernel, kernel_runtime_reserved), sourceModule, and packageUses;
- Type section: one line per TypeRecord (id, shape, abi_layout, domain, identity, sysmeta resource contract, abi_size/abi_align);
- metadata schema; declarationTable (decl line + sysmeta sub-lines + metadata attach);
- Declaration list: function / fragment entries each print name(parameters…) -> return type, with kernel / deferred_recipe / generic_recipe / instantiation markers.
printCostReport prints module->costs line by line.

## Key Functions and Methods

| Function | Purpose |
| --- | --- |
| Printer::print | Main text printing (human-readable format, aligned lines). |
| Printer::str | Returns a string via std::ostringstream. |
| Printer::printCostReport | Prints the cost report; shows a notice if there are no costs. |

## Relationship to Surrounding Files and Pipeline Stages

- Reads the record structures in MoonIR.h and calls their *Name() mapping and findType.
- Stage: diagnostics and testing chain; does not affect the IR.

## Further Reading
- Interface: src/moonir/Printer.h.
- Structure: src/moonir/MoonIR.h.


---

---
title: Printer Interface: Textualizing a MoonIR Module
file: src/moonir/Printer.h
namespace: moon
stage: Diagnostics / testing thin layer
---

# src/moonir/Printer.h

Declares Printer: prints a Module as human-oriented text and can produce a cost report string.

## What This File Does

A read-only printing utility: print(Module, ostream), str() (returns std::string), printCostReport(Module, ostream). Used for debugging, test fixtures, and diagnostic output; does not modify the Module.

## Key Structs, Classes, and Enums

| Class | Purpose |
| --- | --- |
| class Printer | Three public methods. |

## Key Functions and Methods

| Function | Purpose |
| --- | --- |
| void print(const Module&, std::ostream&) | Prints the module body (types, declarations, functions, etc.). |
| std::string str(const Module&) | Wraps print with a stringstream and returns the string. |
| void printCostReport(const Module&, std::ostream&) | Prints the module->costs list. |

## Relationship to Surrounding Files and Pipeline Stages

- Reads the Module/TypeRecord/DeclarationRecord structures in MoonIR.h.
- Stage: a thin layer for diagnostics, testing, and tooling; does not affect IR semantics.

## Further Reading
- Implementation: src/moonir/Printer.cpp.
- Structure: src/moonir/MoonIR.h.


---

---
title: Sealer Implementation: Atomically Sealing Function Bodies
file: src/moonir/Sealer.cpp
namespace: moon
stage: MoonIR sealing
---
