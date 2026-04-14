# Android Memory Workspace Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Implement the memory-first Android UX where `Search / Saved / Page` are three sibling memory views sharing one capability base (multi-select, edit, add-to-saved, filter, go-to), while keeping their own independent local state and moving `Session / Threads / Events` into a secondary drawer.

**Architecture:** Introduce a new `MemoryWorkspace` MVI slice in `:app-domain` and a shared `MemoryDataView` scaffold in `:app-ui`. Refactor memory backend code in `:app-data` to execute memory operations without using `SessionBridgeRepository` as a UI-state god object. `Saved` is session-local only and is cleared when the session ends or changes.

**Tech Stack:** Kotlin, Compose Material3, Coroutines/Flow, JUnit4 (JVM), Gradle Kotlin DSL.

---

## File Structure Lock-In

### New/Modified Packages

- `android/app-domain/src/main/java/com/smlc666/lkmdbg/overlay/presentation/memory/`
  - Memory workspace state, intents, reducers, and view model.
- `android/app-data/src/main/java/com/smlc666/lkmdbg/data/memory/`
  - Stateless memory backends (search/page/write helpers) that talk to `PipeAgentClient`.
- `android/app-ui/src/main/java/com/smlc666/lkmdbg/overlay/ui/memory/`
  - Shared memory scaffold and the three memory view composables.
- `android/app-ui/src/main/java/com/smlc666/lkmdbg/overlay/ui/workspace/`
  - New memory-first workspace scaffold + secondary drawer destinations.

### High-Risk Existing Files To Shrink/Replace

- `android/app-ui/src/main/java/com/smlc666/lkmdbg/overlay/ui/screens/MainWorkspaceScreen.kt`
- `android/app/src/main/java/com/smlc666/lkmdbg/overlay/LkmdbgOverlayService.kt`
- `android/app-data/src/main/java/com/smlc666/lkmdbg/data/SessionBridgeRepository.kt`

## Task 0: Create Isolated Worktree + Baseline

**Files:**
- Modify (maybe): `.gitignore` (only if project-local `.worktrees/` is chosen)

- [ ] **Step 1: Pick worktree location**

If using project-local `.worktrees/`, add it to `.gitignore` first:

```gitignore
.worktrees/
```

- [ ] **Step 2: Create worktree branch**

Run:

```bash
git rev-parse --abbrev-ref HEAD
git worktree add ~/.config/superpowers/worktrees/debugger_linux_lkm/memory-workspace -b android/memory-workspace
cd ~/.config/superpowers/worktrees/debugger_linux_lkm/memory-workspace
```

Expected: worktree directory exists and `git status` is clean.

- [ ] **Step 3: Baseline Android tests**

Run:

```bash
gradle -p android :app-domain:test :app-data:test :app-ui:test
```

Expected: PASS. If baseline fails, stop and decide whether to proceed.

## Task 1: Memory Workspace Domain Skeleton (No UI Yet)

**Files:**
- Create: `android/app-domain/src/main/java/com/smlc666/lkmdbg/overlay/presentation/memory/MemoryWorkspaceIntent.kt`
- Create: `android/app-domain/src/main/java/com/smlc666/lkmdbg/overlay/presentation/memory/MemoryWorkspaceState.kt`
- Create: `android/app-domain/src/main/java/com/smlc666/lkmdbg/overlay/presentation/memory/MemoryWorkspaceReducer.kt`
- Create: `android/app-domain/src/main/java/com/smlc666/lkmdbg/overlay/presentation/memory/MemoryWorkspaceViewModel.kt`
- Test: `android/app-domain/src/test/java/com/smlc666/lkmdbg/overlay/presentation/memory/MemoryWorkspaceReducerTest.kt`
- Test: `android/app-domain/src/test/java/com/smlc666/lkmdbg/overlay/presentation/memory/MemoryWorkspaceViewModelTest.kt`

- [ ] **Step 1: Write failing reducer test for independent state**

```kotlin
package com.smlc666.lkmdbg.overlay.presentation.memory

import org.junit.Test
import org.junit.Assert.assertEquals

class MemoryWorkspaceReducerTest {
    @Test
    fun switchTab_doesNotCopySelectionBetweenTabs() {
        val base = MemoryWorkspaceState.initial()
        val initial = base.copy(
            activeTab = MemoryTab.Search,
            search = base.search.copy(selection = setOf(0x1000uL)),
        )

        val switched = MemoryWorkspaceReducer.reduce(
            initial,
            MemoryWorkspaceIntent.SwitchTab(MemoryTab.Saved),
        )

        assertEquals(MemoryTab.Saved, switched.activeTab)
        assertEquals(setOf(0x1000uL), switched.search.selection)
        assertEquals(emptySet(), switched.saved.selection)
    }
}
```

- [ ] **Step 2: Run test to verify it fails**

Run:

```bash
gradle -p android :app-domain:test --tests "*MemoryWorkspaceReducerTest*"
```

Expected: FAIL due to missing symbols.

- [ ] **Step 3: Add minimal domain types**

```kotlin
package com.smlc666.lkmdbg.overlay.presentation.memory

enum class MemoryTab { Search, Saved, Page }

data class MemorySearchViewState(
    val selection: Set<ULong> = emptySet(),
    val filterText: String = "",
)

data class MemorySavedViewState(
    val selection: Set<ULong> = emptySet(),
    val filterText: String = "",
)

data class MemoryPageViewState(
    val selection: Set<ULong> = emptySet(),
    val filterText: String = "",
)

data class MemoryWorkspaceState(
    val activeTab: MemoryTab,
    val search: MemorySearchViewState,
    val saved: MemorySavedViewState,
    val page: MemoryPageViewState,
) {
    companion object {
        fun initial(): MemoryWorkspaceState =
            MemoryWorkspaceState(
                activeTab = MemoryTab.Search,
                search = MemorySearchViewState(),
                saved = MemorySavedViewState(),
                page = MemoryPageViewState(),
            )
    }
}
```

```kotlin
package com.smlc666.lkmdbg.overlay.presentation.memory

sealed interface MemoryWorkspaceIntent {
    data class SwitchTab(val tab: MemoryTab) : MemoryWorkspaceIntent
}

object MemoryWorkspaceReducer {
    fun reduce(state: MemoryWorkspaceState, intent: MemoryWorkspaceIntent): MemoryWorkspaceState =
        when (intent) {
            is MemoryWorkspaceIntent.SwitchTab -> state.copy(activeTab = intent.tab)
        }
}
```

- [ ] **Step 4: Run tests**

Run:

```bash
gradle -p android :app-domain:test --tests "*MemoryWorkspaceReducerTest*"
```

Expected: PASS.

- [ ] **Step 5: Add a minimal ViewModel wrapper**

```kotlin
package com.smlc666.lkmdbg.overlay.presentation.memory

import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.flow.update

class MemoryWorkspaceViewModel(
    initialState: MemoryWorkspaceState,
    private val scope: CoroutineScope = CoroutineScope(SupervisorJob() + Dispatchers.Default),
) {
    private val mutableState = MutableStateFlow(initialState)

    val state: StateFlow<MemoryWorkspaceState> = mutableState.asStateFlow()

    fun dispatch(intent: MemoryWorkspaceIntent) {
        mutableState.update { current -> MemoryWorkspaceReducer.reduce(current, intent) }
    }
}
```

- [ ] **Step 6: Add a ViewModel test**

```kotlin
package com.smlc666.lkmdbg.overlay.presentation.memory

import kotlinx.coroutines.runBlocking
import org.junit.Test
import org.junit.Assert.assertEquals

class MemoryWorkspaceViewModelTest {
    @Test
    fun dispatch_switchTab_updatesStateFlow() = runBlocking {
        val vm = MemoryWorkspaceViewModel(MemoryWorkspaceState.initial())
        vm.dispatch(MemoryWorkspaceIntent.SwitchTab(MemoryTab.Page))
        assertEquals(MemoryTab.Page, vm.state.value.activeTab)
    }
}
```

- [ ] **Step 7: Run tests**

Run:

```bash
gradle -p android :app-domain:test --tests "*MemoryWorkspaceViewModelTest*"
```

Expected: PASS.

## Task 2: Saved List Model (Session-Local Only)

**Files:**
- Update: `android/app-domain/src/main/java/com/smlc666/lkmdbg/overlay/presentation/memory/MemoryWorkspaceState.kt`
- Update: `android/app-domain/src/main/java/com/smlc666/lkmdbg/overlay/presentation/memory/MemoryWorkspaceIntent.kt`
- Update: `android/app-domain/src/main/java/com/smlc666/lkmdbg/overlay/presentation/memory/MemoryWorkspaceReducer.kt`
- Test: `android/app-domain/src/test/java/com/smlc666/lkmdbg/overlay/presentation/memory/MemoryWorkspaceSavedTest.kt`

- [ ] **Step 1: Write failing test for add-to-saved without sharing selection**

```kotlin
package com.smlc666.lkmdbg.overlay.presentation.memory

import org.junit.Test
import org.junit.Assert.assertEquals

class MemoryWorkspaceSavedTest {
    @Test
    fun addSelectionToSaved_onlyAffectsSavedEntries_notSavedSelection() {
        val initial = MemoryWorkspaceState.initial().copy(
            activeTab = MemoryTab.Search,
            search = MemorySearchViewState(selection = setOf(0x1000uL, 0x2000uL)),
        )

        val next = MemoryWorkspaceReducer.reduce(
            initial,
            MemoryWorkspaceIntent.AddSelectionToSaved(fromTab = MemoryTab.Search),
        )

        assertEquals(setOf(0x1000uL, 0x2000uL), next.saved.entries.keys)
        assertEquals(emptySet(), next.saved.selection)
        assertEquals(setOf(0x1000uL, 0x2000uL), next.search.selection)
    }
}
```

- [ ] **Step 2: Implement session-local saved entries**

```kotlin
package com.smlc666.lkmdbg.overlay.presentation.memory

data class SavedMemoryEntry(
    val address: ULong,
    val label: String = "",
)

data class MemorySavedViewState(
    val entries: LinkedHashMap<ULong, SavedMemoryEntry> = LinkedHashMap(),
    val selection: Set<ULong> = emptySet(),
    val filterText: String = "",
)
```

```kotlin
sealed interface MemoryWorkspaceIntent {
    data class SwitchTab(val tab: MemoryTab) : MemoryWorkspaceIntent
    data class AddSelectionToSaved(val fromTab: MemoryTab) : MemoryWorkspaceIntent
}
```

```kotlin
object MemoryWorkspaceReducer {
    fun reduce(state: MemoryWorkspaceState, intent: MemoryWorkspaceIntent): MemoryWorkspaceState =
        when (intent) {
            is MemoryWorkspaceIntent.SwitchTab -> state.copy(activeTab = intent.tab)
            is MemoryWorkspaceIntent.AddSelectionToSaved -> {
                val addresses = when (intent.fromTab) {
                    MemoryTab.Search -> state.search.selection
                    MemoryTab.Saved -> state.saved.selection
                    MemoryTab.Page -> state.page.selection
                }
                if (addresses.isEmpty()) return state
                val nextEntries = LinkedHashMap(state.saved.entries)
                addresses.forEach { addr ->
                    nextEntries.putIfAbsent(addr, SavedMemoryEntry(address = addr))
                }
                state.copy(saved = state.saved.copy(entries = nextEntries))
            }
        }
}
```

- [ ] **Step 3: Run tests**

Run:

```bash
gradle -p android :app-domain:test --tests "*MemoryWorkspaceSavedTest*"
```

Expected: PASS.

## Task 3: Shared Memory Actions (Filter + Multi-Select)

**Files:**
- Update: `android/app-domain/src/main/java/com/smlc666/lkmdbg/overlay/presentation/memory/MemoryWorkspaceIntent.kt`
- Update: `android/app-domain/src/main/java/com/smlc666/lkmdbg/overlay/presentation/memory/MemoryWorkspaceReducer.kt`
- Test: `android/app-domain/src/test/java/com/smlc666/lkmdbg/overlay/presentation/memory/MemoryWorkspaceSharedActionsTest.kt`

- [ ] **Step 1: Write failing test for independent filters**

```kotlin
package com.smlc666.lkmdbg.overlay.presentation.memory

import org.junit.Test
import org.junit.Assert.assertEquals

class MemoryWorkspaceSharedActionsTest {
    @Test
    fun filterText_isIndependentPerTab() {
        val initial = MemoryWorkspaceState.initial()
        val a = MemoryWorkspaceReducer.reduce(initial, MemoryWorkspaceIntent.SetFilter(MemoryTab.Search, "hp"))
        val b = MemoryWorkspaceReducer.reduce(a, MemoryWorkspaceIntent.SetFilter(MemoryTab.Saved, "ammo"))
        assertEquals("hp", b.search.filterText)
        assertEquals("ammo", b.saved.filterText)
        assertEquals("", b.page.filterText)
    }
}
```

- [ ] **Step 2: Add shared intent + reducer**

```kotlin
sealed interface MemoryWorkspaceIntent {
    data class SwitchTab(val tab: MemoryTab) : MemoryWorkspaceIntent
    data class AddSelectionToSaved(val fromTab: MemoryTab) : MemoryWorkspaceIntent
    data class SetFilter(val tab: MemoryTab, val text: String) : MemoryWorkspaceIntent
    data class ToggleSelected(val tab: MemoryTab, val address: ULong) : MemoryWorkspaceIntent
    data class ClearSelection(val tab: MemoryTab) : MemoryWorkspaceIntent
}
```

Reducer snippet:

```kotlin
private fun toggle(selection: Set<ULong>, address: ULong): Set<ULong> =
    if (address in selection) selection - address else selection + address
```

- [ ] **Step 3: Run tests**

Run:

```bash
gradle -p android :app-domain:test --tests "*MemoryWorkspaceSharedActionsTest*"
```

Expected: PASS.

## Task 4: Memory Backend (Stateless) in `:app-data`

**Files:**
- Create: `android/app-data/src/main/java/com/smlc666/lkmdbg/data/memory/MemoryPageBackend.kt`
- Create: `android/app-data/src/main/java/com/smlc666/lkmdbg/data/memory/MemorySearchBackend.kt`
- Create: `android/app-data/src/main/java/com/smlc666/lkmdbg/data/memory/MemoryWriteBackend.kt`
- Test: `android/app-data/src/test/java/com/smlc666/lkmdbg/data/memory/MemoryBackendParsingTest.kt`

- [ ] **Step 1: Add a parser test for hex bytes**

```kotlin
package com.smlc666.lkmdbg.data.memory

import org.junit.Test
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull

class MemoryBackendParsingTest {
    @Test
    fun parseHexBytes_acceptsSpaces_andRejectsOddLength() {
        assertEquals(listOf(0x01, 0x02, 0xff), parseHexBytes("01 02 ff")!!.map { it.toInt() and 0xff })
        assertNull(parseHexBytes("0"))
    }
}
```

- [ ] **Step 2: Implement minimal parsing helpers**

```kotlin
package com.smlc666.lkmdbg.data.memory

internal fun parseHexBytes(text: String): ByteArray? {
    val compact = text.replace(" ", "").replace("\n", "").replace("\t", "")
    if (compact.isBlank() || compact.length % 2 != 0) return null
    return runCatching {
        ByteArray(compact.length / 2) { index ->
            compact.substring(index * 2, index * 2 + 2).toInt(16).toByte()
        }
    }.getOrNull()
}
```

- [ ] **Step 3: Run tests**

Run:

```bash
gradle -p android :app-data:test --tests "*MemoryBackendParsingTest*"
```

Expected: PASS.

## Task 5: `MemoryDataView` Shared UI Scaffold

**Files:**
- Create: `android/app-ui/src/main/java/com/smlc666/lkmdbg/overlay/ui/memory/MemoryDataRowModels.kt`
- Create: `android/app-ui/src/main/java/com/smlc666/lkmdbg/overlay/ui/memory/MemoryDataView.kt`
- Create: `android/app-ui/src/main/java/com/smlc666/lkmdbg/overlay/ui/memory/MemoryActionMenu.kt`

- [ ] **Step 1: Add row model**

```kotlin
package com.smlc666.lkmdbg.overlay.ui.memory

data class MemoryDataRow(
    val address: ULong,
    val title: String,
    val subtitle: String = "",
    val value: String = "",
    val selected: Boolean = false,
)
```

- [ ] **Step 2: Implement a list scaffold that supports multi-select and long-press menu**

Minimal signature:

```kotlin
@Composable
fun MemoryDataView(
    rows: List<MemoryDataRow>,
    filterText: String,
    onFilterTextChanged: (String) -> Unit,
    onToggleSelected: (ULong) -> Unit,
    onOpenMenu: (ULong) -> Unit,
    onClearSelection: () -> Unit,
)
```

Behavior:

- filter applies locally to `rows`
- tapping toggles selection when any selection exists
- long-press opens menu for that address

- [ ] **Step 3: Ensure module compiles**

Run:

```bash
gradle -p android :app-ui:compileDebugKotlin
```

Expected: PASS.

## Task 6: Implement `Search` View (Domain + UI Wiring)

**Files:**
- Update: `android/app-domain/src/main/java/com/smlc666/lkmdbg/overlay/presentation/memory/MemoryWorkspaceState.kt`
- Update: `android/app-domain/src/main/java/com/smlc666/lkmdbg/overlay/presentation/memory/MemoryWorkspaceIntent.kt`
- Create: `android/app-ui/src/main/java/com/smlc666/lkmdbg/overlay/ui/memory/MemorySearchView.kt`

- [ ] **Step 1: Add search-local inputs to `MemorySearchViewState`**

```kotlin
data class MemorySearchViewState(
    val query: String = "",
    val selection: Set<ULong> = emptySet(),
    val filterText: String = "",
)
```

- [ ] **Step 2: Render `MemorySearchView` using `MemoryDataView`**

`MemorySearchView` must:

- show query field
- show a `Search` and `Refine` button
- render results as `MemoryDataRow`
- support multi-select + add-to-saved action through the shared menu/action bar

- [ ] **Step 3: Compile**

Run:

```bash
gradle -p android :app-ui:compileDebugKotlin
```

Expected: PASS.

## Task 7: Implement `Saved` View (Session-Local)

**Files:**
- Create: `android/app-ui/src/main/java/com/smlc666/lkmdbg/overlay/ui/memory/MemorySavedView.kt`
- Update: `android/app-domain/src/main/java/com/smlc666/lkmdbg/overlay/presentation/memory/MemoryWorkspaceState.kt`

- [ ] **Step 1: Render saved entries as rows**

Saved rows show:

- address
- label (if any)
- last-known value summary (if available)

- [ ] **Step 2: Provide remove-from-saved action**

Add intent:

```kotlin
data class RemoveFromSaved(val address: ULong) : MemoryWorkspaceIntent
```

Reducer removes from `saved.entries` only (does not mutate selection in other tabs).

## Task 8: Implement `Page` View (Browse + Edit)

**Files:**
- Create: `android/app-ui/src/main/java/com/smlc666/lkmdbg/overlay/ui/memory/MemoryPageView.kt`
- Update: `android/app-domain/src/main/java/com/smlc666/lkmdbg/overlay/presentation/memory/MemoryWorkspaceState.kt`

- [ ] **Step 1: Add page-local address input + focused address**

```kotlin
data class MemoryPageViewState(
    val addressInput: String = "",
    val focusedAddress: ULong? = null,
    val selection: Set<ULong> = emptySet(),
    val filterText: String = "",
)
```

- [ ] **Step 2: Render a page preview list**

Display:

- current focused address
- a vertical list of memory rows around it (hex + ASCII)

Editing flow for MVP:

- `Edit` action navigates to `Page` tab, focuses address, and opens a small dialog to accept hex bytes, then writes.

## Task 9: Memory-First Workspace UI + Secondary Drawer

**Files:**
- Create: `android/app-ui/src/main/java/com/smlc666/lkmdbg/overlay/ui/workspace/WorkspaceScaffold.kt`
- Create: `android/app-ui/src/main/java/com/smlc666/lkmdbg/overlay/ui/workspace/WorkspaceDrawer.kt`
- Create: `android/app-ui/src/main/java/com/smlc666/lkmdbg/overlay/ui/workspace/MemoryWorkspaceScreen.kt`
- Modify: `android/app-ui/src/main/java/com/smlc666/lkmdbg/overlay/ui/screens/MainWorkspaceScreen.kt` (replace usage or delete later)

- [ ] **Step 1: Implement a drawer entry icon in top bar**

Top bar keeps:

- close / collapse
- current process badge (existing app icon behavior)
- drawer toggle

- [ ] **Step 2: Body always shows `MemoryWorkspaceScreen`**

`MemoryWorkspaceScreen` contains a 3-tab switch (`Search / Saved / Page`) and shows the active view.

- [ ] **Step 3: Drawer contains `Session / Threads / Events`**

Initial implementation may reuse existing `SessionSectionContent`, `ThreadsSectionContent`, `EventsSectionContent` extracted from the old file.

## Task 10: Service Wiring + Remove Old Memory Mode Flags

**Files:**
- Modify: `android/app/src/main/java/com/smlc666/lkmdbg/overlay/LkmdbgOverlayService.kt`
- Modify: `android/app-data/src/main/java/com/smlc666/lkmdbg/data/SessionBridgeRepository.kt`
- Modify: `android/app-domain/src/main/java/com/smlc666/lkmdbg/data/SessionBridgeStateModels.kt` (remove `memoryViewMode`, `memoryToolsOpen`, memory inputs)

- [ ] **Step 1: Stop passing `memoryViewMode` / `memoryToolsOpen`**

Remove:

- `memoryViewMode`
- `memoryToolsOpen`

from `SessionBridgeState` and all call sites.

- [ ] **Step 2: Replace `MainWorkspaceScreen(...)` call with `WorkspaceScaffold(...)`**

`LkmdbgOverlayService` becomes responsible only for:

- lifecycle
- window mount/unmount
- wiring repository + view models

Feature orchestration (memory UI) moves out of the service.

## Verification

- [ ] **Step 1: Unit tests**

Run:

```bash
gradle -p android :app-domain:test :app-data:test
```

Expected: PASS.

- [ ] **Step 2: Android build**

Run:

```bash
gradle -p android :app:assembleDebug
```

Expected: PASS.

- [ ] **Step 3: Spot-check CI workflow assumptions**

Review:

- `.github/workflows/android-build.yml`

Ensure the build still targets `android/app` and builds the APK.
