# src/moonir/Container.cpp

Implements the Writer/Reader declared in Container.h: designs and locks down the file layout of the Moon Container at the byte level, handling the magic number, version header, directory table, 8-byte alignment, and SHA-256 digest verification.

## What This File Does

Translates "several in-memory section objects" into a self-describing, tamper-evident binary, or reverses the process. It provides three layers of defense:

1. **Write-side boundary validation** (section count, byte limit, duplicate/unknown ids, all 8 required sections present) — invalid input fails immediately.
2. **Read-side integrity validation** (magic number, format version, header reserved bits, directory alignment, non-overlapping ranges, trailing bytes, padding must be zero, SHA-256 digest matches).
3. **Leaky: never silently fixed** — every illegal structure returns false with an English error message.

This is the file that implements the container-layer "false-positive" protection in "verify before release": a truncated, tampered, or structurally non-conformant container will never be treated as valid input.

- Analogy for C++ readers: a hand-written, minimal ELF/archive parser with a digest for self-validation, emphasizing "spec compliance + injection resistance".

## Structs, Classes, and Constants

| Member | Meaning |
| --- | --- |
| constexpr Magic[8] | Magic number 0x89 4d 4f 4f 4e 0d 0a 1a, for quick identification. |
| HeaderSize = 80 | The header is fixed at 80 bytes. |
| DirectoryEntrySize = 32 | Each directory entry is fixed at 32 bytes. |
| DigestOffset = 48 / DigestSize = 32 | Offset and length of the SHA-256 digest in the file. |
| struct DirectoryEntry | One entry in the directory table: id, flags, offset, length, decodedLength. |

## Key Functions and Methods

| Function | Purpose |
| --- | --- |
| digestFor | Computes the digest over all bytes except the digest region using llvm::SHA256 (the digest region is zeroed first and included in the computation). |
| addWouldOverflow / alignEight | 64-bit addition overflow detection and 8-byte alignment helpers. |
| isKnownRequiredSection / hasAllRequiredSections | Determine whether the id is valid and whether all 8 required sections are present. |
| zeroRange / fail | Verify that a range is all zeroes; uniformly set error to a message and return false. |
| writeU32/writeU64/readU32/readU64 | Little-endian 4/8-byte read and write utilities. |
| ContainerWriter::encode | After validation, sorts, writes the directory and each section's payload, and writes the digest. |
| ContainerWriter::writeFile | Writes the binary file after encode. |
| ContainerReader::parse | Reads the header and directory field by field, performs completeness/alignment/digest checks, then restores the sections. |
| ContainerReader::readFile | Uses the filesystem to get the size, reads the file, then parses. |
| ContainerReader::find | Binary search by id using lower_bound. |

## Relationship to Surrounding Files and Pipeline Stages

- Container.h: the interface declaration for this file; this file is the only implementation.
- ContainerModel.cpp: calls into this file to encode/decode the 8 model sections, treating the packed bytes as the final deliverable.
- Pipeline stage: after Lowering/Sealer/Verifier, persists the validated Module to disk or loads it back.

## Further Reading

- src/moonir/Container.h: the container-layer data structures.
- src/moonir/ContainerModel.cpp: what the 8 sections determined by this layer actually contain.
- src/moonir/Verifier.h: model-level integrity validation, complementary to the byte-level validation here.


---
---
title: Base types and read/write interfaces for the Moon Container binary format
file: src/moonir/Container.h
namespace: moon
stage: MoonIR Serialization / Container payload layer
---

# src/moonir/Container.h

Defines the section data types and the ContainerWriter/ContainerReader read/write interfaces of the Moon Container — a binary container that packs a set of ID-numbered sections into a self-verifying blob with a SHA-256 digest.

## What This File Does

Provides the container-level data structures and interfaces: encodes/decodes several ContainerSections (an id + a payload byte range) into a binary that can be persisted or transmitted, following a defined file layout. The Container is responsible for the **outer envelope**: magic number, version, directory table, 8-byte alignment, SHA-256 digest, and integrity/injection validation such as size, duplicate, and out-of-bounds checks. It does not care about the content semantics inside each section — that belongs to ContainerModel.h/.cpp.

- Analogy for C++ readers: roughly a minimal ELF section table plus digest validation — it only handles how sections are laid out and how integrity is checked, not what they contain.

## Key Structs, Classes, and Enums

| Name | Meaning |
| --- | --- |
| enum ContainerSectionId | Enumerates the 8 required sections: Manifest=1, Type=2, Symbol=3, Contract=4, Code=5, Imports=6, Exports=7, Sysmeta=8. |
| OptionalSectionBit = 0x80000000u | High-bit marker for optional-section ids; an id without this bit that is not a known required section is rejected. |
| struct ContainerSection | One payload segment: id + std::vector<uint8_t> payload. |
| struct ContainerLimits | Boundary limits for parsing/generation: maximum byte count, maximum section count, and maximum string/table-row/nesting depth. |
| class ContainerWriter | Static: encodes a vector<ContainerSection> into a container and (optionally) writes it to a file. |
| class ContainerReader | Parses container bytes into a section list; supports lookup by id and format-version-aware reading. |

## Key Functions and Methods

| Signature | Purpose |
| --- | --- |
| static bool encode(sections, output, error, limits) | After sorting, deduplication, and validation, writes the container bytes (including the digest); returns false with an error for any out-of-bounds or invalid input. |
| static bool writeFile(path, sections, error, limits) | Encodes first, then writes the file in binary mode. |
| bool parse(input, error, limits) | Validates the magic number/version/directory/alignment/digest and restores the section list. |
| bool readFile(path, error, limits) | Reads the file into bytes, then runs parse. |
| const ContainerSection* find(uint32_t id) | Binary-searches for the section with the given id. |
| formatMajor()/formatMinor() | Return the format version carried by the container. |

## Relationship to Surrounding Files and Pipeline Stages

- Container.cpp: the concrete implementation of the interfaces above and all byte-level logic.
- ContainerModel.h/.cpp: the upper layer calls ContainerWriter::encode / ContainerReader::parse to split the model of the whole Module into the 8 sections and merge it back from them.
- Pipeline stage: located before code generation (LLVM lowering) and after frontend lowering; responsible for persisting a validated MoonIR Module into a container or loading it back.

## Further Reading

- ContainerModel.h: the semantic decoding of this layer; defines what each section's bytes mean.
- MoonIR.h: data structures packed by the Container, such as Module, DeclarationRecord, and TypeRecord.
- Verifier.h/.cpp: validates Module integrity before and after encode/decode; this is the executor of verify-before-release.


---
---
title: Implementation of sectioned encode/decode for the Moon container model
file: src/moonir/ContainerModel.cpp
namespace: moon
stage: MoonIR Serialization / Container semantic layer
---
