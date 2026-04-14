# Android Memory Workspace Redesign

Date: 2026-04-14  
Repo: `/root/debugger_linux_lkm`  
Scope: `android/`

## Goal

Reshape the Android layer around a memory-first workflow inspired by GameGuardian-style UX, without leaving a compatibility-shaped half-migration behind.

The new design makes `Memory` the primary workspace and turns `Search`, `Saved`, and `Page` into three sibling views under one shared memory UI base. They share the same memory-data capability layer, but each view keeps its own state and its own core behavior.

## Decision Summary

- `Memory` is the main workspace.
- `Session`, `Threads`, and `Events` move to a secondary drawer / secondary entry area.
- `Search`, `Saved`, and `Page` are first-class memory views.
- The three memory views share a common UI/state base.
- Their selection, filter, focus, scroll, and local inputs are independent.
- `Saved` is session-local only and is cleared when the session ends.
- Common actions such as multi-select, edit, add-to-saved, filter, and go-to-address are implemented once and reused by all three views.

## Current State

The repo already has a partial Android architecture split:

- `android/app-domain/` holds workspace presentation state and intents.
- `android/app-data/` holds `SessionBridgeRepository` and memory helpers.
- `android/app-ui/` holds the current Compose workspace screen.
- `android/app/` hosts overlay lifecycle and wiring.

The current shape is functional, but the memory UI is still too coupled to the broader workspace:

- `MainWorkspaceScreen.kt` is still a large callback surface.
- `LkmdbgOverlayService.kt` still owns too much workspace orchestration.
- `SessionBridgeRepository.kt` still mixes bridge operations with UI-facing memory concerns.
- `Search`, `Saved`, and `Page` are not yet modeled as siblings with a shared base.

## Architecture

### 1. Shared Memory Base

Introduce a shared memory UI/data base that all three views use.

Responsibilities:

- render memory rows and memory item lists
- support multi-select
- support value editing
- support add-to-saved
- support filter entry and clearing
- support go-to-address
- provide a shared long-press / overflow action model

This base is not a catch-all feature screen. It only owns common memory-display behavior.

### 2. Three Specialized Views

`MemorySearchView`

- runs searches
- refines existing results
- shows searchable result rows
- exposes search-specific inputs such as value type, region preset, and refine mode

`MemorySavedView`

- shows session-local saved entries
- supports rename / note editing
- supports freeze / unfreeze
- supports quick edit
- supports remove from saved

`MemoryPageView`

- shows a focused memory page around an address
- supports hex / ASCII / asm browsing and editing
- supports page stepping
- supports in-place selection and follow-up actions

### 3. Secondary Workspace

`Session`, `Threads`, and `Events` remain available, but they no longer compete with the memory workspace as equal primary tabs.

They are treated as secondary workspace entries or drawer destinations, so the user can return to them when needed without polluting the main memory workflow.

## Data Model

### Shared Base State

Define a shared state base for memory views with common fields such as:

- `items`
- `selectedItems`
- `focusedAddress`
- `loading`
- `error`
- `menuTarget`

### View-Specific State

Each view keeps its own state slice:

- `SearchState`: query, value type, refine mode, region preset, results
- `SavedState`: saved entries, notes, frozen flags, local saved ordering
- `PageState`: current focus, visible page bytes, page rows, region summary, disassembly, scalar preview

These slices do not share selection or filter state. Switching views does not carry state across.

### Memory Item Contract

All memory views operate on the same conceptual row model:

- address
- display value
- value type
- region or origin
- selection state
- note or label where applicable
- frozen state where applicable

The UI can ask any row for the same core actions, regardless of which view produced it.

## Actions

### Shared Actions

- multi-select
- clear selection
- edit value
- add to saved
- remove from saved
- filter
- go to address
- copy value / address
- open action menu

### Search-Specific Actions

- run search
- refine search
- change search type
- change search region

### Saved-Specific Actions

- add note
- freeze
- unfreeze
- reorder locally
- drop entry

### Page-Specific Actions

- step page up/down
- jump to address
- write hex / ASCII / asm
- load current selection into an editor input

## Layer Boundaries

### `app-domain`

Owns:

- shared memory state contracts
- memory view intents
- reducers / state transitions
- view-specific use cases
- local saved-list rules

Must not depend on Compose or Android UI classes.

### `app-data`

Owns:

- bridge access
- memory read / write / search helpers
- page building
- search snapshots
- session-local persistence in memory only

Should not encode view layout decisions.

### `app-ui`

Owns:

- shared memory scaffold
- search / saved / page screens
- action menus
- filters
- multi-select UI

Should not know how bridge commands are executed.

### `app`

Owns:

- overlay service lifecycle
- window attachment
- top-level routing
- wiring of view models and controllers

It should not contain memory feature logic.

## Error Handling

- Invalid addresses stay local to the memory view that produced them.
- Bridge or transport failures are normalized into user-facing state messages.
- A failed search or page fetch must not clear unrelated state slices.
- Saved entries survive normal in-session errors but are cleared when the session is reset or closed.

## Testing

The minimum coverage for this redesign:

- reducer tests for shared memory state transitions
- tests proving `Search`, `Saved`, and `Page` keep independent selection/filter/focus state
- tests for shared action mapping
- tests for session-local saved list behavior
- tests for menu routing and `go to address` dispatch
- UI dispatch tests for the shared scaffold

## Migration Strategy

This is a replacement design, not an alias layer.

The migration should:

1. extract the shared memory base
2. split the current memory surface into `Search`, `Saved`, and `Page`
3. move session/thread/event navigation out of the primary workspace
4. shrink `MainWorkspaceScreen.kt` into a thin coordinator
5. shrink `LkmdbgOverlayService.kt` into lifecycle wiring only

No compatibility wrapper should preserve the old mixed memory surface as a public end state.

## Success Criteria

- The main Android workspace centers on memory operations.
- `Search`, `Saved`, and `Page` share one base and one action model.
- The three views keep independent local state.
- `Saved` is session-local only.
- The service and workspace screen are both smaller and easier to reason about.
- No new technical debt is introduced to preserve the old mixed layout.
