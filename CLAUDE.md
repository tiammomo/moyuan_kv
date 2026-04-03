# CLAUDE.md

This file provides guidance for Claude Code when working with this codebase.

## Git Commit Rules

### Behavior
When generating git commit messages:
- **Strictly Forbidden**: Never include text indicating the message was AI-generated (e.g., "Written by Claude", "AI-generated").
- **No Footers**: Do not append "Signed-off-by" or "Co-authored-by" lines unless explicitly told to do so for a specific human user.
- **Direct Output**: Output the commit message immediately without introductory text (e.g., skip "Sure, here is the commit...").

### Format Standard
- Use the Conventional Commits format: `<type>(<scope>): <subject>`
- Allowed types: feat, fix, docs, style, refactor, perf, test, build, ci, chore, revert.
- Keep the first line under 72 characters.

## Project Context

MoKV is a distributed KV store based on Raft + LSM Tree, written in C++17.

### Key Technologies
- **Storage Engine**: LSM Tree with SkipList MemTable, SST files, Manifest
- **Distributed**: Raft protocol for consistency
- **Build System**: CMake (migrated from Bazel)
- **RPC**: gRPC + Protocol Buffers
- **Async I/O**: io_uring on Linux

### Documentation
- Primary documentation lives in `docs/`
- Start with `docs/README.md` for the document index
- Project-specific long-term memory for agents lives in `docs/project/agent-memory.md`

### Build & Test
```bash
./build.sh  # Build server and client
cd build && ctest --output-on-failure  # Run tests
```
