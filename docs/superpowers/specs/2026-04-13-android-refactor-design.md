# Android Refactor Design

Date: 2026-04-13
Repo: `/root/debugger_linux_lkm`
Scope: `android/`

## Goal

Refactor the Android stack into a maintainable architecture that can absorb future debugger features without recreating oversized UI files, service-level orchestration logic, or stateful god objects.

This design targets:

- single-direction state flow
- feature-bounded logic
- protocol compatibility with the existing root agent and shared bridge frames
- measurable engineering health gates that prevent debt from re-accumulating

The user explicitly allows large structural and UX adjustments as long as the debugger capabilities do not regress and the end state is sustainable.

## Current Problems

- `MainWorkspaceScreen.kt` is a giant Compose surface with an oversized callback API and too much feature knowledge.
- `LkmdbgOverlayService.kt` mixes lifecycle, window hosting, state observation, and feature orchestration.
- `SessionBridgeRepository.kt` owns too many responsibilities: state container, input normalization, business actions, feature coordination, and gateway behavior.
- Testing is narrow. Current JVM coverage mostly validates derived UI-model helpers instead of state transitions and feature behavior.
- Build configuration and release discipline are weaker than the desired target. Toolchain constants are duplicated and signing is still debug-oriented.

## Non-Goals

- No kernel-session protocol redesign.
- No growth of Android-only side channels around the existing session fd model.
- No bootstrap path changes to `/proc/version`.
- No speculative feature additions during the refactor.

## Architecture Summary

The Android stack will move to `MVI + ViewModel + UseCase + Gateway` with feature slices, then split into Gradle modules once the slice boundaries are proven inside the current `:app`.

The architectural flow is:

`UI Intent -> ViewModel -> UseCase -> Gateway -> Reducer -> UiState -> Compose UI`

Rules:

- UI renders `UiState` and dispatches `Intent`.
- ViewModels coordinate feature work and expose immutable state streams.
- UseCases encode feature behavior and combine gateway calls with reducer updates.
- Gateways hide agent, bridge, and platform details.
- Reducers are pure and unit-testable.
- Services host Android system concerns only.

## Target Gradle Modules

The final module shape is:

- `:app`
  - Android app shell and dependency wiring only
- `:app-ui`
  - Compose screens, reusable UI components, ViewModels, navigation between workspace sections
- `:app-domain`
  - feature intents, state, reducers, use cases, domain models, policy logic
- `:app-data`
  - gateway implementations, bridge client adapters, process resolution, memory helpers, installer integration
- `:shared`
  - existing bridge protocol models and codecs shared across Android layers

Dependency direction:

- `:app -> :app-ui`
- `:app -> :app-data`
- `:app-ui -> :app-domain`
- `:app-data -> :app-domain`
- `:app-data -> :shared`
- `:app-domain` must not depend on Android framework classes

`android/agent` remains a separate native project and stays protocol-compatible with `:shared`.

## Transitional Layout Inside `:app`

Before Gradle module extraction, the same boundaries will be created as packages inside `:app`:

- `overlay/ui/`
- `overlay/presentation/`
- `domain/session/`
- `domain/process/`
- `domain/thread/`
- `domain/event/`
- `domain/memory/`
- `data/gateway/`
- `data/bridge/`
- `platform/overlay/`

This keeps migration incremental and reduces simultaneous Gradle and source refactors.

## Feature Slice Model

Each workspace area becomes an explicit feature slice:

- Session
- Process
- Thread
- Event
- Memory

Each feature slice owns:

- `Intent`
- `UiState`
- `Reducer`
- `UseCase`
- feature-specific mapper or formatter helpers when needed

Shared concerns such as top-level workspace state, selected section, and cross-feature focus changes are handled by a workspace-level presentation layer that composes the feature states instead of re-centralizing business logic.

## UI Layer Design

### Compose API

Compose screens should accept:

- a bounded `UiState`
- a small `dispatch(intent)` function or feature-local action interface

They should not accept dozens of independent callbacks.

### Screen Composition

`MainWorkspaceScreen.kt` will be replaced by smaller, purposeful files:

- `WorkspaceScreen`
- `WorkspaceTopBar`
- `SessionPanel`
- `ProcessPanel`
- `ThreadPanel`
- `EventPanel`
- `MemoryPanel`
- shared dialog and chip components

The exact file count can vary, but no single UI file should become the new dumping ground.

### ViewModels

At minimum:

- one workspace-level ViewModel for shell coordination
- one ViewModel per major feature, or one workspace ViewModel with feature reducers if composition stays clean

The selection criterion is clarity of ownership. If a ViewModel starts carrying unrelated feature state, it must be split.

## Service and Platform Layer

`LkmdbgOverlayService` will be reduced to:

- lifecycle binding
- window attach and detach
- Compose host creation
- wiring Android system dependencies into presentation objects

The service must not directly encode feature workflows such as attach-target, open-event-thread, memory navigation, or auto-poll policy. Those become intents and use cases.

Overlay window control, gesture control, and permission handling remain platform concerns and stay outside domain logic.

## Data and Gateway Layer

`SessionBridgeRepository` will be decomposed.

Target shape:

- `SessionGateway`
- `ProcessGateway`
- `ThreadGateway`
- `EventGateway`
- `MemoryGateway`
- bridge-level adapter classes around `PipeAgentClient`

Responsibilities:

- gateways expose stable operations meaningful to the domain
- bridge adapters translate wire/protocol responses into domain results
- input sanitation and UX state mutation move out of gateway code and into reducers or presentation mappers unless they are protocol-safety requirements

`PipeAgentClient` stays narrow and transport-focused. It should not become a presentation-aware repository.

## Protocol Compatibility Constraints

The refactor must preserve wire compatibility between:

- `android/shared`
- `android/agent`
- existing kernel session-fd API expectations

Specifically:

- frame sizes stay unchanged
- field order stays unchanged
- command identifiers stay unchanged
- session fd remains the authoritative command surface

Any protocol-affecting proposal is outside this refactor and requires separate design work.

## State and Error Handling

Reducers will own explicit state transitions for:

- busy and idle transitions
- session connection lifecycle
- process selection and attach
- thread selection and register refresh
- event polling, pinning, and selection
- memory browse, search, refine, preview, and write

Effects should be explicit where one feature drives another. Example:

- opening an event thread emits a selection effect into the thread feature
- opening an event value emits a memory-focus effect into the memory feature

Errors should be normalized into domain-level result types with user-facing messages produced by presentation mappers. This keeps protocol errors, IO failures, and unsupported-operation responses from leaking raw transport details through the UI.

## Build and Release Health

The refactor also standardizes Android build health:

- toolchain versions are defined once and reused by app build logic, custom Gradle tasks, and CI
- `debug` and `release` signing are separated
- release configuration should stop using the fixed debug certificate path
- lint becomes part of CI
- static analysis can begin as non-blocking, then become blocking after the architecture settles

## Testing Strategy

The minimum required automated coverage after refactor:

- reducer tests for every feature slice
- use-case tests for success, unsupported, and failure paths
- gateway tests around bridge-response mapping
- retained UI-model tests where derivation logic is complex

Priority test targets:

- session warm start and refresh behavior
- attach target transitions
- event pinning and filtering
- thread selection and register grouping
- memory search lifecycle and page focus updates
- cross-feature navigation effects

Instrumentation tests are optional for the first pass. JVM tests are the primary enforcement layer.

## Migration Plan

### Phase 1: Create the Architecture Skeleton Inside `:app`

- add feature intents, states, reducers, and use-case contracts
- add gateway interfaces
- introduce ViewModel-owned state flow
- keep existing UI behavior working through adapters

### Phase 2: Migrate Session and Process Features

- move session connection and process attachment into the new flow
- remove service-level orchestration for these features
- replace related callback fan-out in the main workspace UI

### Phase 3: Migrate Thread and Event Features

- move thread selection, register refresh, event polling, filtering, and pinning
- represent cross-feature jumps as effects instead of direct service logic

### Phase 4: Migrate Memory Feature

- move memory browse, jump, search, refine, preview, assemble, and write
- this is the highest-risk phase and happens last
- temporary adapters are allowed only during migration and must be deleted before completion

### Phase 5: Simplify Overlay Host

- strip `LkmdbgOverlayService` down to host responsibilities only
- move all residual business actions to presentation and domain layers

### Phase 6: Extract Gradle Modules

- split the stabilized package boundaries into `:app-ui`, `:app-domain`, and `:app-data`
- keep `:shared` as the bridge contract module
- update CI to build and test the new module graph

## Completion Criteria

The refactor is only considered done when all conditions below are true:

- oversized callback-driven workspace APIs are removed
- `LkmdbgOverlayService` is not a business-orchestration layer
- `SessionBridgeRepository` no longer acts as a god object
- feature slices own their own state transitions
- CI runs app unit tests and lint successfully
- protocol compatibility is preserved
- temporary adapters introduced during migration are deleted

Health gates:

- target file size under 500 lines for normal Kotlin files
- exceptions up to 700 lines require a clear reason and should be rare
- public function parameter lists should stay under 8 whenever practical
- new code should prefer data structures and action objects over callback explosion

## Risks and Mitigations

- Risk: Memory feature migration causes regressions because it spans the most state and commands.
  - Mitigation: migrate memory last and cover reducer and use-case transitions first.
- Risk: Module extraction too early blocks progress with build-system churn.
  - Mitigation: prove boundaries inside `:app` before changing Gradle structure.
- Risk: old and new flows coexist too long and create dual sources of truth.
  - Mitigation: use short-lived adapters and delete them at the end of each phase.
- Risk: UI refactor accidentally changes protocol behavior by coupling presentation and bridge code.
  - Mitigation: keep bridge compatibility under dedicated gateway tests and shared contract ownership.

## Acceptance Standard

This project will not aim for the impossible claim of "zero technical debt forever." The acceptance standard is stricter and more useful:

- no known structural debt backlog remains after the refactor
- new work has clear extension points
- architecture violations are visible in CI and review
- future features can be added inside a feature slice without reopening giant files or service wiring

## Implementation Follow-Up

After this design is approved in-repo, the next step is a concrete implementation plan that sequences the migration by phase, identifies file ownership, and defines the verification commands and CI checks required at each checkpoint.
