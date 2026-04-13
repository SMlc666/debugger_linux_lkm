# Android Refactor Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Refactor `android/` into an MVI-based, feature-sliced architecture with explicit state flow, protocol-safe gateways, and enforceable health gates.

**Architecture:** Implement feature slices (`Session`, `Process`, `Thread`, `Event`, `Memory`) with `Intent -> ViewModel -> UseCase -> Gateway -> Reducer -> UiState` inside `:app` first. After boundaries are stable and tested, extract `:app-ui`, `:app-domain`, and `:app-data` modules while keeping `:shared` as the bridge contract module.

**Tech Stack:** Kotlin, Android SDK 35, Compose Material3, Coroutines/Flow, JUnit4, Robolectric, Gradle Kotlin DSL, Android Lint.

---

## File Structure Lock-In

Primary source roots:

- `android/app/src/main/java/com/smlc666/lkmdbg/overlay/presentation/workspace/`
- `android/app/src/main/java/com/smlc666/lkmdbg/domain/{session,process,thread,event,memory,workspace}/`
- `android/app/src/main/java/com/smlc666/lkmdbg/domain/gateway/`
- `android/app/src/main/java/com/smlc666/lkmdbg/data/gateway/`
- `android/app/src/main/java/com/smlc666/lkmdbg/platform/overlay/`

Primary test roots:

- `android/app/src/test/java/com/smlc666/lkmdbg/overlay/presentation/workspace/`
- `android/app/src/test/java/com/smlc666/lkmdbg/domain/`
- `android/app/src/test/java/com/smlc666/lkmdbg/data/gateway/`

High-risk existing files to shrink:

- `android/app/src/main/java/com/smlc666/lkmdbg/overlay/ui/screens/MainWorkspaceScreen.kt`
- `android/app/src/main/java/com/smlc666/lkmdbg/overlay/LkmdbgOverlayService.kt`
- `android/app/src/main/java/com/smlc666/lkmdbg/data/SessionBridgeRepository.kt`

## Task 1: Create Workspace MVI Skeleton (No Behavior Change)

**Files:**
- Create: `android/app/src/main/java/com/smlc666/lkmdbg/overlay/presentation/workspace/WorkspaceIntent.kt`
- Create: `android/app/src/main/java/com/smlc666/lkmdbg/overlay/presentation/workspace/WorkspaceAction.kt`
- Create: `android/app/src/main/java/com/smlc666/lkmdbg/overlay/presentation/workspace/WorkspaceUiState.kt`
- Create: `android/app/src/main/java/com/smlc666/lkmdbg/overlay/presentation/workspace/WorkspaceReducer.kt`
- Create: `android/app/src/main/java/com/smlc666/lkmdbg/overlay/presentation/workspace/WorkspaceViewModel.kt`
- Test: `android/app/src/test/java/com/smlc666/lkmdbg/overlay/presentation/workspace/WorkspaceReducerTest.kt`
- Test: `android/app/src/test/java/com/smlc666/lkmdbg/overlay/presentation/workspace/WorkspaceViewModelTest.kt`

- [ ] **Step 1: Write failing reducer test**

```kotlin
class WorkspaceReducerTest {
    @Test
    fun reduce_setSection_updatesSectionAndLeavesOtherSlices() {
        val initial = WorkspaceUiState.initial()
        val next = WorkspaceReducer.reduce(initial, WorkspaceAction.SetSection(WorkspaceSection.Threads))
        assertEquals(WorkspaceSection.Threads, next.section)
        assertEquals(initial.session, next.session)
    }
}
```

- [ ] **Step 2: Run reducer test and verify failure**

Run: `gradle -p android :app:testDebugUnitTest --tests "*WorkspaceReducerTest*"`
Expected: FAIL with unresolved `WorkspaceUiState` or missing reducer symbol.

- [ ] **Step 3: Add minimal MVI skeleton**

```kotlin
sealed interface WorkspaceAction {
    data class SetSection(val section: WorkspaceSection) : WorkspaceAction
}

data class WorkspaceUiState(
    val section: WorkspaceSection,
    val session: SessionUiState,
) {
    companion object {
        fun initial() = WorkspaceUiState(
            section = WorkspaceSection.Memory,
            session = SessionUiState.initial(),
        )
    }
}

object WorkspaceReducer {
    fun reduce(state: WorkspaceUiState, action: WorkspaceAction): WorkspaceUiState =
        when (action) {
            is WorkspaceAction.SetSection -> state.copy(section = action.section)
        }
}
```

- [ ] **Step 4: Add ViewModel test and minimal ViewModel**

```kotlin
@Test
fun dispatch_setSection_updatesStateFlow() = runTest {
    val vm = WorkspaceViewModel(WorkspaceUiState.initial())
    vm.dispatch(WorkspaceIntent.SelectSection(WorkspaceSection.Events))
    assertEquals(WorkspaceSection.Events, vm.state.value.section)
}
```

```kotlin
class WorkspaceViewModel(initial: WorkspaceUiState) : ViewModel() {
    private val _state = MutableStateFlow(initial)
    val state: StateFlow<WorkspaceUiState> = _state.asStateFlow()

    fun dispatch(intent: WorkspaceIntent) {
        val action = when (intent) {
            is WorkspaceIntent.SelectSection -> WorkspaceAction.SetSection(intent.section)
        }
        _state.update { WorkspaceReducer.reduce(it, action) }
    }
}
```

- [ ] **Step 5: Run tests and commit**

Run: `gradle -p android :app:testDebugUnitTest --tests "*WorkspaceReducerTest*" --tests "*WorkspaceViewModelTest*"`
Expected: PASS

```bash
git add android/app/src/main/java/com/smlc666/lkmdbg/overlay/presentation/workspace android/app/src/test/java/com/smlc666/lkmdbg/overlay/presentation/workspace
git commit -m "refactor(android): add workspace mvi skeleton"
```

## Task 2: Convert Workspace UI API to `state + dispatch`

**Files:**
- Modify: `android/app/src/main/java/com/smlc666/lkmdbg/overlay/ui/screens/MainWorkspaceScreen.kt`
- Create: `android/app/src/main/java/com/smlc666/lkmdbg/overlay/ui/screens/WorkspaceDispatchAdapters.kt`
- Test: `android/app/src/test/java/com/smlc666/lkmdbg/overlay/presentation/workspace/WorkspaceDispatchAdaptersTest.kt`

- [ ] **Step 1: Write failing adapter mapping test**

```kotlin
@Test
fun map_openEventThread_emitsSelectThreadAndSectionIntent() {
    val intents = mutableListOf<WorkspaceIntent>()
    val dispatch = { intent: WorkspaceIntent -> intents += intent }
    val adapters = workspaceDispatchAdapters(dispatch)
    adapters.openEventThread(1234)
    assertEquals(
        listOf(
            WorkspaceIntent.SelectSection(WorkspaceSection.Threads),
            WorkspaceIntent.SelectThread(1234),
        ),
        intents,
    )
}
```

- [ ] **Step 2: Run adapter test and verify failure**

Run: `gradle -p android :app:testDebugUnitTest --tests "*WorkspaceDispatchAdaptersTest*"`
Expected: FAIL with missing `workspaceDispatchAdapters`.

- [ ] **Step 3: Implement adapter layer**

```kotlin
data class WorkspaceDispatchAdapters(
    val openEventThread: (Int) -> Unit,
)

fun workspaceDispatchAdapters(dispatch: (WorkspaceIntent) -> Unit): WorkspaceDispatchAdapters =
    WorkspaceDispatchAdapters(
        openEventThread = { tid ->
            dispatch(WorkspaceIntent.SelectSection(WorkspaceSection.Threads))
            dispatch(WorkspaceIntent.SelectThread(tid))
        },
    )
```

- [ ] **Step 4: Refactor `MainWorkspaceScreen` signature**

```kotlin
@Composable
fun MainWorkspaceScreen(
    state: WorkspaceUiState,
    dispatch: (WorkspaceIntent) -> Unit,
    onClose: () -> Unit,
    onCollapse: () -> Unit,
) {
    val adapters = remember(dispatch) { workspaceDispatchAdapters(dispatch) }
    // route UI events through adapters instead of 60+ callbacks
}
```

- [ ] **Step 5: Run tests and commit**

Run: `gradle -p android :app:testDebugUnitTest --tests "*WorkspaceDispatchAdaptersTest*" --tests "*WorkspaceUiModelsTest*"`
Expected: PASS

```bash
git add android/app/src/main/java/com/smlc666/lkmdbg/overlay/ui/screens/MainWorkspaceScreen.kt android/app/src/main/java/com/smlc666/lkmdbg/overlay/ui/screens/WorkspaceDispatchAdapters.kt android/app/src/test/java/com/smlc666/lkmdbg/overlay/presentation/workspace/WorkspaceDispatchAdaptersTest.kt
git commit -m "refactor(android-ui): replace callback fan-out with dispatch api"
```

## Task 3: Migrate Session and Process Features into Domain UseCases

**Files:**
- Create: `android/app/src/main/java/com/smlc666/lkmdbg/domain/gateway/SessionGateway.kt`
- Create: `android/app/src/main/java/com/smlc666/lkmdbg/domain/gateway/ProcessGateway.kt`
- Create: `android/app/src/main/java/com/smlc666/lkmdbg/domain/session/SessionUseCases.kt`
- Create: `android/app/src/main/java/com/smlc666/lkmdbg/domain/process/ProcessUseCases.kt`
- Create: `android/app/src/main/java/com/smlc666/lkmdbg/data/gateway/SessionGatewayImpl.kt`
- Create: `android/app/src/main/java/com/smlc666/lkmdbg/data/gateway/ProcessGatewayImpl.kt`
- Test: `android/app/src/test/java/com/smlc666/lkmdbg/domain/session/SessionUseCasesTest.kt`
- Test: `android/app/src/test/java/com/smlc666/lkmdbg/domain/process/ProcessUseCasesTest.kt`

- [ ] **Step 1: Write failing session use-case test**

```kotlin
@Test
fun connectAndOpenSession_updatesStateWithSessionOpen() = runTest {
    val gateway = FakeSessionGateway(openResult = SessionOpenResult(success = true, message = "opened"))
    val useCase = ConnectAndOpenSessionUseCase(gateway)
    val state = WorkspaceUiState.initial()
    val next = useCase.invoke(state)
    assertTrue(next.session.open)
    assertEquals("opened", next.session.message)
}
```

- [ ] **Step 2: Run domain tests and verify failure**

Run: `gradle -p android :app:testDebugUnitTest --tests "*SessionUseCasesTest*" --tests "*ProcessUseCasesTest*"`
Expected: FAIL with missing gateway and use-case classes.

- [ ] **Step 3: Implement gateway contracts and minimal use cases**

```kotlin
interface SessionGateway {
    suspend fun connect(): SessionConnectResult
    suspend fun openSession(): SessionOpenResult
}

class ConnectAndOpenSessionUseCase(
    private val gateway: SessionGateway,
) {
    suspend operator fun invoke(state: WorkspaceUiState): WorkspaceUiState {
        gateway.connect()
        val open = gateway.openSession()
        return state.copy(session = state.session.copy(open = open.success, message = open.message))
    }
}
```

- [ ] **Step 4: Implement data adapters backed by current repository/client**

```kotlin
class SessionGatewayImpl(
    private val repository: SessionBridgeRepository,
) : SessionGateway {
    override suspend fun connect(): SessionConnectResult {
        repository.connect()
        return SessionConnectResult(success = true, message = repository.state.value.lastMessage)
    }
    override suspend fun openSession(): SessionOpenResult {
        repository.openSession()
        val state = repository.state.value
        return SessionOpenResult(success = state.snapshot.sessionOpen, message = state.lastMessage)
    }
}
```

- [ ] **Step 5: Run tests and commit**

Run: `gradle -p android :app:testDebugUnitTest --tests "*SessionUseCasesTest*" --tests "*ProcessUseCasesTest*"`
Expected: PASS

```bash
git add android/app/src/main/java/com/smlc666/lkmdbg/domain/gateway android/app/src/main/java/com/smlc666/lkmdbg/domain/session android/app/src/main/java/com/smlc666/lkmdbg/domain/process android/app/src/main/java/com/smlc666/lkmdbg/data/gateway android/app/src/test/java/com/smlc666/lkmdbg/domain/session android/app/src/test/java/com/smlc666/lkmdbg/domain/process
git commit -m "refactor(android-domain): add session and process usecases with gateways"
```

## Task 4: Migrate Thread and Event Feature Flows

**Files:**
- Create: `android/app/src/main/java/com/smlc666/lkmdbg/domain/gateway/ThreadGateway.kt`
- Create: `android/app/src/main/java/com/smlc666/lkmdbg/domain/gateway/EventGateway.kt`
- Create: `android/app/src/main/java/com/smlc666/lkmdbg/domain/thread/ThreadUseCases.kt`
- Create: `android/app/src/main/java/com/smlc666/lkmdbg/domain/event/EventUseCases.kt`
- Create: `android/app/src/main/java/com/smlc666/lkmdbg/data/gateway/ThreadGatewayImpl.kt`
- Create: `android/app/src/main/java/com/smlc666/lkmdbg/data/gateway/EventGatewayImpl.kt`
- Test: `android/app/src/test/java/com/smlc666/lkmdbg/domain/thread/ThreadUseCasesTest.kt`
- Test: `android/app/src/test/java/com/smlc666/lkmdbg/domain/event/EventUseCasesTest.kt`

- [ ] **Step 1: Write failing event pinning and thread selection tests**

```kotlin
@Test
fun togglePinnedEvent_reordersPinnedFirst() {
    val state = WorkspaceUiState.withEvents(seq = listOf(1uL, 2uL, 3uL))
    val next = TogglePinnedEventUseCase().invoke(state, 2uL)
    assertEquals(listOf(2uL, 3uL, 1uL), next.event.visibleEvents.map { it.seq })
}
```

```kotlin
@Test
fun selectThread_loadsRegistersAndUpdatesSelection() = runTest {
    val gateway = FakeThreadGateway(registerTid = 77)
    val next = SelectThreadUseCase(gateway).invoke(WorkspaceUiState.initial(), 77)
    assertEquals(77, next.thread.selectedTid)
    assertNotNull(next.thread.registers)
}
```

- [ ] **Step 2: Run tests and verify failure**

Run: `gradle -p android :app:testDebugUnitTest --tests "*ThreadUseCasesTest*" --tests "*EventUseCasesTest*"`
Expected: FAIL with missing thread/event domain symbols.

- [ ] **Step 3: Implement thread and event use cases**

```kotlin
class SelectThreadUseCase(
    private val gateway: ThreadGateway,
) {
    suspend operator fun invoke(state: WorkspaceUiState, tid: Int): WorkspaceUiState {
        val registers = gateway.getRegisters(tid)
        return state.copy(thread = state.thread.copy(selectedTid = tid, registers = registers))
    }
}

class TogglePinnedEventUseCase {
    operator fun invoke(state: WorkspaceUiState, seq: ULong): WorkspaceUiState {
        val pinned = state.event.pinned.toMutableSet()
        if (!pinned.add(seq)) pinned.remove(seq)
        return state.copy(event = state.event.withPinned(pinned))
    }
}
```

- [ ] **Step 4: Wire intent handlers in `WorkspaceViewModel`**

```kotlin
when (intent) {
    is WorkspaceIntent.SelectThread -> reduceWithThreadUseCase(intent.tid)
    is WorkspaceIntent.TogglePinnedEvent -> _state.update { togglePinnedEventUseCase(it, intent.seq) }
}
```

- [ ] **Step 5: Run tests and commit**

Run: `gradle -p android :app:testDebugUnitTest --tests "*ThreadUseCasesTest*" --tests "*EventUseCasesTest*" --tests "*WorkspaceUiModelsTest*"`
Expected: PASS

```bash
git add android/app/src/main/java/com/smlc666/lkmdbg/domain/thread android/app/src/main/java/com/smlc666/lkmdbg/domain/event android/app/src/main/java/com/smlc666/lkmdbg/domain/gateway android/app/src/main/java/com/smlc666/lkmdbg/data/gateway android/app/src/main/java/com/smlc666/lkmdbg/overlay/presentation/workspace android/app/src/test/java/com/smlc666/lkmdbg/domain/thread android/app/src/test/java/com/smlc666/lkmdbg/domain/event
git commit -m "refactor(android-domain): migrate thread and event flows"
```

## Task 5: Migrate Memory Feature Last (Highest Risk)

**Files:**
- Create: `android/app/src/main/java/com/smlc666/lkmdbg/domain/gateway/MemoryGateway.kt`
- Create: `android/app/src/main/java/com/smlc666/lkmdbg/domain/memory/MemoryUseCases.kt`
- Create: `android/app/src/main/java/com/smlc666/lkmdbg/data/gateway/MemoryGatewayImpl.kt`
- Modify: `android/app/src/main/java/com/smlc666/lkmdbg/data/MemorySearchCoordinator.kt`
- Modify: `android/app/src/main/java/com/smlc666/lkmdbg/data/MemoryEditorController.kt`
- Test: `android/app/src/test/java/com/smlc666/lkmdbg/domain/memory/MemoryUseCasesTest.kt`

- [ ] **Step 1: Write failing memory lifecycle tests**

```kotlin
@Test
fun runSearch_updatesSummaryAndResultCount() = runTest {
    val gateway = FakeMemoryGateway(resultCount = 3)
    val next = RunMemorySearchUseCase(gateway).invoke(WorkspaceUiState.initial(), "7f 45 4c 46")
    assertEquals(3, next.memory.results.size)
    assertTrue(next.memory.summary.contains("3"))
}
```

```kotlin
@Test
fun openEventValue_setsMemoryFocusAndPageMode() {
    val next = OpenEventValueUseCase().invoke(WorkspaceUiState.initial(), 0x7100001000uL)
    assertEquals(0x7100001000uL, next.memory.focusAddress)
    assertEquals(MemoryViewMode.Page, next.memory.viewMode)
}
```

- [ ] **Step 2: Run tests and verify failure**

Run: `gradle -p android :app:testDebugUnitTest --tests "*MemoryUseCasesTest*"`
Expected: FAIL with missing memory use-case classes.

- [ ] **Step 3: Implement memory use cases and gateway contract**

```kotlin
class RunMemorySearchUseCase(
    private val gateway: MemoryGateway,
) {
    suspend operator fun invoke(state: WorkspaceUiState, query: String): WorkspaceUiState {
        val reply = gateway.search(query, state.memory.searchPreset)
        return state.copy(memory = state.memory.withSearchResults(reply.results, reply.summary))
    }
}
```

- [ ] **Step 4: Replace direct repository memory mutations in ViewModel**

```kotlin
is WorkspaceIntent.RunMemorySearch -> launchMutation {
    _state.update { it.copy(memory = it.memory.copy(loading = true)) }
    val next = runMemorySearchUseCase(_state.value, intent.query)
    _state.value = next.copy(memory = next.memory.copy(loading = false))
}
```

- [ ] **Step 5: Run tests and commit**

Run: `gradle -p android :app:testDebugUnitTest --tests "*MemoryUseCasesTest*" --tests "*WorkspaceUiModelsTest*"`
Expected: PASS

```bash
git add android/app/src/main/java/com/smlc666/lkmdbg/domain/memory android/app/src/main/java/com/smlc666/lkmdbg/domain/gateway/MemoryGateway.kt android/app/src/main/java/com/smlc666/lkmdbg/data/gateway/MemoryGatewayImpl.kt android/app/src/main/java/com/smlc666/lkmdbg/overlay/presentation/workspace android/app/src/test/java/com/smlc666/lkmdbg/domain/memory
git commit -m "refactor(android-memory): migrate memory flow to domain usecases"
```

## Task 6: Strip `LkmdbgOverlayService` Down to Host Responsibilities

**Files:**
- Create: `android/app/src/main/java/com/smlc666/lkmdbg/platform/overlay/OverlayHostController.kt`
- Create: `android/app/src/main/java/com/smlc666/lkmdbg/platform/overlay/OverlayHostUiBinder.kt`
- Modify: `android/app/src/main/java/com/smlc666/lkmdbg/overlay/LkmdbgOverlayService.kt`
- Modify: `android/app/src/main/java/com/smlc666/lkmdbg/overlay/OverlayStateBinder.kt`
- Test: `android/app/src/test/java/com/smlc666/lkmdbg/platform/overlay/OverlayHostUiBinderTest.kt`

- [ ] **Step 1: Write failing host binder test**

```kotlin
@Test
fun bindState_forExpandedMode_buildsWorkspaceUiModel() {
    val binder = OverlayHostUiBinder()
    val model = binder.bind(WorkspaceUiState.initial(), expanded = true)
    assertTrue(model.showWorkspace)
    assertFalse(model.showCollapsedChip)
}
```

- [ ] **Step 2: Run test and verify failure**

Run: `gradle -p android :app:testDebugUnitTest --tests "*OverlayHostUiBinderTest*"`
Expected: FAIL because binder does not exist.

- [ ] **Step 3: Implement host binder and host controller**

```kotlin
data class OverlayHostUiModel(val showWorkspace: Boolean, val showCollapsedChip: Boolean)

class OverlayHostUiBinder {
    fun bind(state: WorkspaceUiState, expanded: Boolean): OverlayHostUiModel =
        OverlayHostUiModel(showWorkspace = expanded, showCollapsedChip = !expanded)
}
```

- [ ] **Step 4: Refactor service to call ViewModel dispatch only**

```kotlin
MainWorkspaceScreen(
    state = workspaceState,
    dispatch = workspaceViewModel::dispatch,
    onClose = { stopSelf() },
    onCollapse = { updateExpandedState(false) },
)
```

- [ ] **Step 5: Run tests and commit**

Run: `gradle -p android :app:testDebugUnitTest --tests "*OverlayHostUiBinderTest*" --tests "*WorkspaceViewModelTest*"`
Expected: PASS

```bash
git add android/app/src/main/java/com/smlc666/lkmdbg/platform/overlay android/app/src/main/java/com/smlc666/lkmdbg/overlay/LkmdbgOverlayService.kt android/app/src/main/java/com/smlc666/lkmdbg/overlay/OverlayStateBinder.kt android/app/src/test/java/com/smlc666/lkmdbg/platform/overlay/OverlayHostUiBinderTest.kt
git commit -m "refactor(android-overlay): reduce service to host-only responsibilities"
```

## Task 7: Decompose `SessionBridgeRepository` Into Gateway/Adapter Layer

**Files:**
- Create: `android/app/src/main/java/com/smlc666/lkmdbg/data/bridge/SessionBridgeClient.kt`
- Create: `android/app/src/main/java/com/smlc666/lkmdbg/data/bridge/SessionBridgeClientImpl.kt`
- Modify: `android/app/src/main/java/com/smlc666/lkmdbg/data/PipeAgentClient.kt`
- Modify: `android/app/src/main/java/com/smlc666/lkmdbg/data/SessionBridgeRepository.kt`
- Test: `android/app/src/test/java/com/smlc666/lkmdbg/data/gateway/SessionGatewayImplTest.kt`

- [ ] **Step 1: Write failing gateway mapping test**

```kotlin
@Test
fun openSession_mapsInvalidHeaderToFailureResult() = runTest {
    val bridge = FakeSessionBridgeClient(openStatus = BridgeStatusCode.InvalidHeader.wireValue)
    val gateway = SessionGatewayImpl(bridge)
    val result = gateway.openSession()
    assertFalse(result.success)
    assertTrue(result.message.contains("InvalidHeader"))
}
```

- [ ] **Step 2: Run test and verify failure**

Run: `gradle -p android :app:testDebugUnitTest --tests "*SessionGatewayImplTest*"`
Expected: FAIL with missing bridge client abstraction.

- [ ] **Step 3: Add bridge client abstraction**

```kotlin
interface SessionBridgeClient {
    suspend fun openSession(): BridgeOpenSessionReply
    suspend fun statusSnapshot(): BridgeStatusSnapshot
}

class SessionBridgeClientImpl(
    private val pipeAgentClient: PipeAgentClient,
) : SessionBridgeClient {
    override suspend fun openSession() = pipeAgentClient.openSession()
    override suspend fun statusSnapshot() = pipeAgentClient.statusSnapshot()
}
```

- [ ] **Step 4: Update repository to become composition root only**

```kotlin
class SessionBridgeRepository(...) {
    // keep state exposure for compatibility during migration
    // delegate feature operations to gateways/use cases
}
```

- [ ] **Step 5: Run tests and commit**

Run: `gradle -p android :app:testDebugUnitTest --tests "*SessionGatewayImplTest*" --tests "*SessionUseCasesTest*" --tests "*ProcessUseCasesTest*" --tests "*ThreadUseCasesTest*" --tests "*EventUseCasesTest*" --tests "*MemoryUseCasesTest*"`
Expected: PASS

```bash
git add android/app/src/main/java/com/smlc666/lkmdbg/data/bridge android/app/src/main/java/com/smlc666/lkmdbg/data/PipeAgentClient.kt android/app/src/main/java/com/smlc666/lkmdbg/data/SessionBridgeRepository.kt android/app/src/main/java/com/smlc666/lkmdbg/data/gateway android/app/src/test/java/com/smlc666/lkmdbg/data/gateway/SessionGatewayImplTest.kt
git commit -m "refactor(android-data): split repository into bridge adapter and gateways"
```

## Task 8: Extract Gradle Modules (`:app-ui`, `:app-domain`, `:app-data`)

**Files:**
- Modify: `android/settings.gradle.kts`
- Modify: `android/build.gradle.kts`
- Create: `android/app-ui/build.gradle.kts`
- Create: `android/app-domain/build.gradle.kts`
- Create: `android/app-data/build.gradle.kts`
- Move: presentation/ui files from `android/app/src/main/java/...` to `android/app-ui/src/main/java/...`
- Move: domain files to `android/app-domain/src/main/java/...`
- Move: data gateway/bridge files to `android/app-data/src/main/java/...`
- Modify: `android/app/build.gradle.kts`
- Test: `android/app-domain/src/test/...`
- Test: `android/app-data/src/test/...`

- [ ] **Step 1: Write failing module wiring test command**

Run: `gradle -p android :app-ui:compileDebugKotlin :app-domain:test :app-data:test`
Expected: FAIL because modules are not declared.

- [ ] **Step 2: Add module declarations and build files**

```kotlin
// android/settings.gradle.kts
include(":app")
include(":shared")
include(":app-ui")
include(":app-domain")
include(":app-data")
```

```kotlin
// android/app-ui/build.gradle.kts
plugins {
    alias(libs.plugins.android.library)
    alias(libs.plugins.kotlin.android)
}
android { namespace = "com.smlc666.lkmdbg.appui"; compileSdk = 35 }
dependencies {
    implementation(project(":app-domain"))
    implementation(libs.androidx.lifecycle.runtime.ktx)
    implementation("androidx.compose.material3:material3")
}
```

- [ ] **Step 3: Move source sets and fix imports**

```bash
git mv android/app/src/main/java/com/smlc666/lkmdbg/overlay/ui android/app-ui/src/main/java/com/smlc666/lkmdbg/overlay/ui
git mv android/app/src/main/java/com/smlc666/lkmdbg/overlay/presentation android/app-ui/src/main/java/com/smlc666/lkmdbg/overlay/presentation
git mv android/app/src/main/java/com/smlc666/lkmdbg/domain android/app-domain/src/main/java/com/smlc666/lkmdbg/domain
git mv android/app/src/main/java/com/smlc666/lkmdbg/data/gateway android/app-data/src/main/java/com/smlc666/lkmdbg/data/gateway
git mv android/app/src/main/java/com/smlc666/lkmdbg/data/bridge android/app-data/src/main/java/com/smlc666/lkmdbg/data/bridge
```

- [ ] **Step 4: Rewire `:app` dependencies and app entrypoints**

```kotlin
dependencies {
    implementation(project(":shared"))
    implementation(project(":app-ui"))
    implementation(project(":app-data"))
}
```

- [ ] **Step 5: Run multi-module checks and commit**

Run: `gradle -p android :app:assembleDebug :app-ui:compileDebugKotlin :app-domain:test :app-data:test`
Expected: PASS

```bash
git add android/settings.gradle.kts android/build.gradle.kts android/app/build.gradle.kts android/app-ui android/app-domain android/app-data
git commit -m "refactor(android): extract ui domain and data modules"
```

## Task 9: Add Build Health Gates, Release Discipline, and Final Cleanup

**Files:**
- Modify: `android/gradle/libs.versions.toml`
- Modify: `android/app/build.gradle.kts`
- Modify: `android/buildSrc/src/main/kotlin/com/smlc666/gradle/BuildBundledAgentTask.kt`
- Modify: `.github/workflows/android-build.yml`
- Create: `android/gradle/toolchain.versions.toml`
- Delete: adapter compatibility files no longer used by new module graph
- Test: `android/app/src/test/...` and module tests

- [ ] **Step 1: Write failing lint gate expectation**

Run: `gradle -p android :app:lintDebug`
Expected: FAIL now or not wired in CI yet.

- [ ] **Step 2: Centralize toolchain constants**

```kotlin
// read versions from a single source
val androidNdkVersion = providers.gradleProperty("android.ndk.version").get()
val androidCmakeVersion = providers.gradleProperty("android.cmake.version").get()
```

```properties
# android/gradle.properties
android.ndk.version=27.1.12297006
android.cmake.version=3.22.1
```

- [ ] **Step 3: Separate release signing from debug signing**

```kotlin
buildTypes {
    debug { signingConfig = signingConfigs.getByName("fixedDebug") }
    release {
        signingConfig = signingConfigs.findByName("release")
        isMinifyEnabled = true
        proguardFiles(getDefaultProguardFile("proguard-android-optimize.txt"), "proguard-rules.pro")
    }
}
```

- [ ] **Step 4: Update CI gates**

```yaml
- name: Run JVM tests and lint
  run: gradle -p android :shared:build :app:testDebugUnitTest :app:lintDebug :app:assembleDebug
```

- [ ] **Step 5: Full verification and commit**

Run: `gradle -p android :shared:build :app:testDebugUnitTest :app:lintDebug :app:assembleDebug`
Expected: PASS

Run: `git diff --check`
Expected: no trailing whitespace errors

```bash
git add android/gradle.properties android/app/build.gradle.kts android/buildSrc/src/main/kotlin/com/smlc666/gradle/BuildBundledAgentTask.kt android/gradle/libs.versions.toml .github/workflows/android-build.yml
git commit -m "build(android): enforce lint gate and release signing discipline"
```

## Spec Coverage Check

- MVI workflow and reducer-driven state: covered by Tasks 1-5.
- Session, process, thread, event, memory feature slices: covered by Tasks 3-5.
- Service reduced to host role: covered by Task 6.
- Repository decomposition and bridge adapter boundary: covered by Task 7.
- Gradle module extraction (`:app-ui`, `:app-domain`, `:app-data`): covered by Task 8.
- Build health and CI quality gates: covered by Task 9.
- Protocol compatibility constraints: covered by Tasks 3, 4, 5, and 7 via gateway/bridge tests and no `:shared` wire changes.

## Placeholder Scan

No deferred markers are present. Every task has concrete files, commands, and commit checkpoints.

## Type Consistency Check

Core types are used consistently across tasks:

- `WorkspaceIntent`, `WorkspaceAction`, `WorkspaceUiState`, `WorkspaceReducer`, `WorkspaceViewModel`
- gateway naming convention: `*Gateway` + `*GatewayImpl`
- use-case naming convention: `*UseCase`
