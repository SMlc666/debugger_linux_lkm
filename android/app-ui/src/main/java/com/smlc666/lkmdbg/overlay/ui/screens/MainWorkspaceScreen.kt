package com.smlc666.lkmdbg.overlay.ui.screens

import android.graphics.Bitmap
import android.graphics.Canvas
import androidx.compose.foundation.Image
import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.ColumnScope
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxHeight
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.LazyRow
import androidx.compose.foundation.lazy.items
import androidx.compose.material3.HorizontalDivider
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.Surface
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.asImageBitmap
import androidx.compose.ui.res.painterResource
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import com.smlc666.lkmdbg.appdata.R
import com.smlc666.lkmdbg.data.HwpointPreset
import com.smlc666.lkmdbg.data.MemoryPage
import com.smlc666.lkmdbg.data.MemoryPreviewRow
import com.smlc666.lkmdbg.data.MemoryRegionPreset
import com.smlc666.lkmdbg.data.MemorySearchRefineMode
import com.smlc666.lkmdbg.data.MemorySearchResult
import com.smlc666.lkmdbg.data.MemorySearchValueType
import com.smlc666.lkmdbg.data.ProcessFilter
import com.smlc666.lkmdbg.data.ResolvedProcessRecord
import com.smlc666.lkmdbg.data.SessionBridgeState
import com.smlc666.lkmdbg.data.SessionEventEntry
import com.smlc666.lkmdbg.data.WorkspaceSection
import com.smlc666.lkmdbg.overlay.presentation.workspace.WorkspaceIntent
import com.smlc666.lkmdbg.overlay.presentation.workspace.WorkspaceUiState
import com.smlc666.lkmdbg.overlay.ui.components.OverlayModalContainer
import com.smlc666.lkmdbg.shared.BridgeHwpointRecord
import com.smlc666.lkmdbg.shared.BridgeStopState
import com.smlc666.lkmdbg.shared.BridgeThreadRegistersReply
import com.smlc666.lkmdbg.shared.BridgeThreadRecord

private const val MEMORY_VIEW_MODE_PAGE = 0
private const val MEMORY_VIEW_MODE_RESULTS = 1

private data class StatusBadgeSpec(
    val label: String,
    val containerColor: Color,
    val contentColor: Color,
)

private data class MetricTileSpec(
    val label: String,
    val value: String,
)

@Composable
fun WorkingBar(
    state: SessionBridgeState,
) {
    Surface(
        modifier = Modifier.fillMaxWidth(),
        color = MaterialTheme.colorScheme.surfaceVariant.copy(alpha = 0.8f),
        tonalElevation = 0.dp,
    ) {
        Row(
            modifier = Modifier
                .fillMaxWidth()
                .padding(horizontal = 8.dp, vertical = 4.dp),
            verticalAlignment = Alignment.CenterVertically,
            horizontalArrangement = Arrangement.spacedBy(8.dp),
        ) {
            Icon(
                painter = painterResource(id = R.drawable.ic_lkmdbg_terminal),
                contentDescription = "Memory",
                modifier = Modifier
                    .width(18.dp)
                    .height(18.dp),
                tint = MaterialTheme.colorScheme.onSurfaceVariant,
            )

            val currentProcess = state.processes.find { it.pid == state.snapshot.targetPid }
            val processName = currentProcess?.processName ?: ""
            val emptyTargetLabel = stringResource(R.string.process_select_prompt)
            val memoryPage = state.memoryPage
            val statusText = buildString {
                append("[")
                append(state.snapshot.targetPid)
                append("] ")
                append(processName.ifBlank { emptyTargetLabel })
                if (memoryPage != null) {
                    append(" · ")
                    append(hex64(memoryPage.focusAddress))
                }
            }
            Text(
                text = statusText,
                style = MaterialTheme.typography.bodySmall,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
                modifier = Modifier.weight(1f),
                maxLines = 1,
            )
        }
    }
}

@Composable
fun MainWorkspaceScreen(
    state: SessionBridgeState,
    onSectionSelected: (WorkspaceSection) -> Unit,
    onToggleProcessPicker: () -> Unit,
    onTargetPidInputChanged: (String) -> Unit,
    onTargetTidInputChanged: (String) -> Unit,
    onConnect: () -> Unit,
    onOpenSession: () -> Unit,
    onRefreshSession: () -> Unit,
    onAttachTarget: () -> Unit,
    onRefreshStopState: () -> Unit,
    onFreezeThreads: () -> Unit,
    onThawThreads: () -> Unit,
    onContinueTarget: () -> Unit,
    onSingleStep: () -> Unit,
    onRefreshProcesses: () -> Unit,
    onProcessFilterSelected: (ProcessFilter) -> Unit,
    onSelectProcess: (Int) -> Unit,
    onAttachSelectedProcess: (Int) -> Unit,
    onRefreshThreads: () -> Unit,
    onSelectThread: (Int) -> Unit,
    onRefreshSelectedThreadRegisters: () -> Unit,
    onHwpointAddressChanged: (String) -> Unit,
    onHwpointLengthChanged: (String) -> Unit,
    onCycleHwpointPreset: () -> Unit,
    onUseSelectedPcForHwpoint: () -> Unit,
    onUseMemoryFocusForHwpoint: () -> Unit,
    onAddHwpoint: () -> Unit,
    onSelectHwpoint: (ULong) -> Unit,
    onRemoveSelectedHwpoint: () -> Unit,
    onRefreshHwpoints: () -> Unit,
    onRefreshEvents: () -> Unit,
    onToggleEventsAutoPoll: () -> Unit,
    onClearEvents: () -> Unit,
    onTogglePinnedEvent: (ULong) -> Unit,
    onOpenEventThread: (Int) -> Unit,
    onOpenEventValue: (ULong) -> Unit,
    onStepMemoryPage: (Int) -> Unit,
    onSelectMemoryAddress: (ULong) -> Unit,
    onMemorySearchQueryChanged: (String) -> Unit,
    onMemoryAddressInputChanged: (String) -> Unit,
    onMemorySelectionSizeChanged: (Int) -> Unit,
    onMemoryWriteHexChanged: (String) -> Unit,
    onMemoryWriteAsciiChanged: (String) -> Unit,
    onMemoryWriteAsmChanged: (String) -> Unit,
    onCycleMemorySearchValueType: () -> Unit,
    onCycleMemorySearchRefineMode: () -> Unit,
    onCycleMemoryRegionPreset: () -> Unit,
    onJumpMemoryAddress: () -> Unit,
    onLoadSelectionIntoHexSearch: () -> Unit,
    onLoadSelectionIntoAsciiSearch: () -> Unit,
    onLoadSelectionIntoEditors: () -> Unit,
    onWriteHexAtFocus: () -> Unit,
    onWriteAsciiAtFocus: () -> Unit,
    onAssembleToEditors: () -> Unit,
    onAssembleAndWrite: () -> Unit,
    onRunMemorySearch: () -> Unit,
    onRefineMemorySearch: () -> Unit,
    onRefreshVmas: () -> Unit,
    onRefreshImages: () -> Unit,
    onPreviewSelectedPc: () -> Unit,
    onClose: () -> Unit,
    onCollapse: () -> Unit,
) {
    val dispatchBridge = remember(onSectionSelected, onSelectThread, onTogglePinnedEvent) {
        { intent: WorkspaceIntent ->
            when (intent) {
                is WorkspaceIntent.SelectSection -> onSectionSelected(intent.section)
                is WorkspaceIntent.SelectThread -> onSelectThread(intent.tid)
                is WorkspaceIntent.TogglePinnedEvent -> onTogglePinnedEvent(intent.seq)
            }
        }
    }
    val adapters = remember(dispatchBridge) { workspaceDispatchAdapters(dispatchBridge) }
    var workspaceNavOpen by rememberSaveable { mutableStateOf(false) }

    val closeWorkspaceNavToMemory = {
        workspaceNavOpen = false
        onSectionSelected(WorkspaceSection.Memory)
    }

    Box(
        modifier = Modifier
            .fillMaxSize()
            .background(MaterialTheme.colorScheme.background),
    ) {
        Column(
            modifier = Modifier
                .fillMaxSize()
                .padding(8.dp),
            verticalArrangement = Arrangement.spacedBy(8.dp),
        ) {
            TopBarArea(
                onClose = onClose,
                onCollapse = onCollapse,
                state = state,
                onToggleProcessPicker = onToggleProcessPicker,
                onOpenWorkspaceNav = {
                    workspaceNavOpen = true
                    if (state.workspaceSection == WorkspaceSection.Memory) {
                        onSectionSelected(WorkspaceSection.Session)
                    }
                },
            )

            WorkingBar(state = state)

            MemoryWorkspaceSection(
                bridgeState = state,
                onSearchQueryChanged = onMemorySearchQueryChanged,
                onRunSearch = onRunMemorySearch,
                onRefineSearch = onRefineMemorySearch,
                onCycleValueType = onCycleMemorySearchValueType,
                onCycleRefineMode = onCycleMemorySearchRefineMode,
                onCycleRegionPreset = onCycleMemoryRegionPreset,
                onStepPage = onStepMemoryPage,
                onSelectAddress = onSelectMemoryAddress,
                modifier = Modifier.weight(1f),
            )
        }

        if (workspaceNavOpen) {
            WorkspaceNavDialog(
                state = state,
                onDismiss = closeWorkspaceNavToMemory,
                onSectionSelected = onSectionSelected,
                onTargetPidInputChanged = onTargetPidInputChanged,
                onTargetTidInputChanged = onTargetTidInputChanged,
                onConnect = onConnect,
                onOpenSession = onOpenSession,
                onRefreshSession = onRefreshSession,
                onAttachTarget = {
                    closeWorkspaceNavToMemory()
                    onAttachTarget()
                },
                onRefreshStopState = onRefreshStopState,
                onFreezeThreads = onFreezeThreads,
                onThawThreads = onThawThreads,
                onContinueTarget = onContinueTarget,
                onSingleStep = onSingleStep,
                onRefreshProcesses = onRefreshProcesses,
                onProcessFilterSelected = onProcessFilterSelected,
                onSelectProcess = onSelectProcess,
                onAttachSelectedProcess = { pid ->
                    closeWorkspaceNavToMemory()
                    onAttachSelectedProcess(pid)
                },
                onRefreshThreads = onRefreshThreads,
                onSelectThread = onSelectThread,
                onRefreshSelectedThreadRegisters = onRefreshSelectedThreadRegisters,
                onHwpointAddressChanged = onHwpointAddressChanged,
                onHwpointLengthChanged = onHwpointLengthChanged,
                onCycleHwpointPreset = onCycleHwpointPreset,
                onUseSelectedPcForHwpoint = onUseSelectedPcForHwpoint,
                onUseMemoryFocusForHwpoint = onUseMemoryFocusForHwpoint,
                onAddHwpoint = onAddHwpoint,
                onSelectHwpoint = onSelectHwpoint,
                onRemoveSelectedHwpoint = onRemoveSelectedHwpoint,
                onRefreshHwpoints = onRefreshHwpoints,
                onRefreshEvents = onRefreshEvents,
                onToggleEventsAutoPoll = onToggleEventsAutoPoll,
                onClearEvents = onClearEvents,
                onTogglePinnedEvent = onTogglePinnedEvent,
                onOpenEventThread = adapters.openEventThread,
                onOpenEventValue = { value ->
                    closeWorkspaceNavToMemory()
                    onOpenEventValue(value)
                },
                onPreviewSelectedPc = {
                    closeWorkspaceNavToMemory()
                    onPreviewSelectedPc()
                },
            )
        }
    }
}

@Composable
private fun WorkspaceColumn(
    state: SessionBridgeState,
    memoryViewMode: Int,
    memoryToolsOpen: Boolean,
    onOpenWorkspaceNav: () -> Unit,
    onToggleProcessPicker: () -> Unit,
    onToggleMemoryTools: () -> Unit,
    onTargetPidInputChanged: (String) -> Unit,
    onTargetTidInputChanged: (String) -> Unit,
    onConnect: () -> Unit,
    onOpenSession: () -> Unit,
    onRefreshSession: () -> Unit,
    onAttachTarget: () -> Unit,
    onRefreshStopState: () -> Unit,
    onFreezeThreads: () -> Unit,
    onThawThreads: () -> Unit,
    onContinueTarget: () -> Unit,
    onSingleStep: () -> Unit,
    onRefreshProcesses: () -> Unit,
    onProcessFilterSelected: (ProcessFilter) -> Unit,
    onSelectProcess: (Int) -> Unit,
    onAttachSelectedProcess: (Int) -> Unit,
    onRefreshThreads: () -> Unit,
    onSelectThread: (Int) -> Unit,
    onRefreshSelectedThreadRegisters: () -> Unit,
    onHwpointAddressChanged: (String) -> Unit,
    onHwpointLengthChanged: (String) -> Unit,
    onCycleHwpointPreset: () -> Unit,
    onUseSelectedPcForHwpoint: () -> Unit,
    onUseMemoryFocusForHwpoint: () -> Unit,
    onAddHwpoint: () -> Unit,
    onSelectHwpoint: (ULong) -> Unit,
    onRemoveSelectedHwpoint: () -> Unit,
    onRefreshHwpoints: () -> Unit,
    onRefreshEvents: () -> Unit,
    onToggleEventsAutoPoll: () -> Unit,
    onClearEvents: () -> Unit,
    onTogglePinnedEvent: (ULong) -> Unit,
    onOpenEventThread: (Int) -> Unit,
    onOpenEventValue: (ULong) -> Unit,
    onStepMemoryPage: (Int) -> Unit,
    onSelectMemoryAddress: (ULong) -> Unit,
    onMemorySearchQueryChanged: (String) -> Unit,
    onMemoryAddressInputChanged: (String) -> Unit,
    onMemorySelectionSizeChanged: (Int) -> Unit,
    onMemoryWriteHexChanged: (String) -> Unit,
    onMemoryWriteAsciiChanged: (String) -> Unit,
    onMemoryWriteAsmChanged: (String) -> Unit,
    onCycleMemorySearchValueType: () -> Unit,
    onCycleMemorySearchRefineMode: () -> Unit,
    onCycleMemoryRegionPreset: () -> Unit,
    onJumpMemoryAddress: () -> Unit,
    onLoadSelectionIntoHexSearch: () -> Unit,
    onLoadSelectionIntoAsciiSearch: () -> Unit,
    onLoadSelectionIntoEditors: () -> Unit,
    onWriteHexAtFocus: () -> Unit,
    onWriteAsciiAtFocus: () -> Unit,
    onAssembleToEditors: () -> Unit,
    onAssembleAndWrite: () -> Unit,
    onRunMemorySearch: () -> Unit,
    onRefineMemorySearch: () -> Unit,
    onRefreshVmas: () -> Unit,
    onRefreshImages: () -> Unit,
    onShowMemoryResults: () -> Unit,
    onShowMemoryPage: () -> Unit,
    onPreviewSelectedPc: () -> Unit,
    onClose: () -> Unit,
    onCollapse: () -> Unit,
) {
    Column(
        modifier = Modifier
            .fillMaxSize()
            .padding(8.dp),
    ) {
        TopBarArea(
            onClose = onClose,
            onCollapse = onCollapse,
            state = state,
            onOpenWorkspaceNav = onOpenWorkspaceNav,
            onToggleProcessPicker = onToggleProcessPicker,
        )

        if (state.workspaceSection == WorkspaceSection.Memory) {
            WorkingBar(
                state = state,
                memoryToolsOpen = memoryToolsOpen,
                onToggleMemoryTools = onToggleMemoryTools,
            )
        }

        Box(
            modifier = Modifier
                .weight(1f)
                .fillMaxWidth()
                .padding(top = 8.dp),
        ) {
            MainContentArea(
                state = state,
                memoryViewMode = memoryViewMode,
                memoryToolsOpen = memoryToolsOpen,
                onTargetPidInputChanged = onTargetPidInputChanged,
                onTargetTidInputChanged = onTargetTidInputChanged,
                onConnect = onConnect,
                onOpenSession = onOpenSession,
                onRefreshSession = onRefreshSession,
                onAttachTarget = onAttachTarget,
                onRefreshStopState = onRefreshStopState,
                onFreezeThreads = onFreezeThreads,
                onThawThreads = onThawThreads,
                onContinueTarget = onContinueTarget,
                onSingleStep = onSingleStep,
                onRefreshProcesses = onRefreshProcesses,
                onProcessFilterSelected = onProcessFilterSelected,
                onSelectProcess = onSelectProcess,
                onAttachSelectedProcess = onAttachSelectedProcess,
                onRefreshThreads = onRefreshThreads,
                onSelectThread = onSelectThread,
                onRefreshSelectedThreadRegisters = onRefreshSelectedThreadRegisters,
                onHwpointAddressChanged = onHwpointAddressChanged,
                onHwpointLengthChanged = onHwpointLengthChanged,
                onCycleHwpointPreset = onCycleHwpointPreset,
                onUseSelectedPcForHwpoint = onUseSelectedPcForHwpoint,
                onUseMemoryFocusForHwpoint = onUseMemoryFocusForHwpoint,
                onAddHwpoint = onAddHwpoint,
                onSelectHwpoint = onSelectHwpoint,
                onRemoveSelectedHwpoint = onRemoveSelectedHwpoint,
                onRefreshHwpoints = onRefreshHwpoints,
                onRefreshEvents = onRefreshEvents,
                onToggleEventsAutoPoll = onToggleEventsAutoPoll,
                onClearEvents = onClearEvents,
                onTogglePinnedEvent = onTogglePinnedEvent,
                onOpenEventThread = onOpenEventThread,
                onOpenEventValue = onOpenEventValue,
                onStepMemoryPage = onStepMemoryPage,
                onSelectMemoryAddress = onSelectMemoryAddress,
                onMemorySearchQueryChanged = onMemorySearchQueryChanged,
                onMemoryAddressInputChanged = onMemoryAddressInputChanged,
                onMemorySelectionSizeChanged = onMemorySelectionSizeChanged,
                onMemoryWriteHexChanged = onMemoryWriteHexChanged,
                onMemoryWriteAsciiChanged = onMemoryWriteAsciiChanged,
                onMemoryWriteAsmChanged = onMemoryWriteAsmChanged,
                onCycleMemorySearchValueType = onCycleMemorySearchValueType,
                onCycleMemorySearchRefineMode = onCycleMemorySearchRefineMode,
                onCycleMemoryRegionPreset = onCycleMemoryRegionPreset,
                onJumpMemoryAddress = onJumpMemoryAddress,
                onLoadSelectionIntoHexSearch = onLoadSelectionIntoHexSearch,
                onLoadSelectionIntoAsciiSearch = onLoadSelectionIntoAsciiSearch,
                onLoadSelectionIntoEditors = onLoadSelectionIntoEditors,
                onWriteHexAtFocus = onWriteHexAtFocus,
                onWriteAsciiAtFocus = onWriteAsciiAtFocus,
                onAssembleToEditors = onAssembleToEditors,
                onAssembleAndWrite = onAssembleAndWrite,
                onRunMemorySearch = onRunMemorySearch,
                onRefineMemorySearch = onRefineMemorySearch,
                onRefreshVmas = onRefreshVmas,
                onRefreshImages = onRefreshImages,
                onShowMemoryResults = onShowMemoryResults,
                onShowMemoryPage = onShowMemoryPage,
                onPreviewSelectedPc = onPreviewSelectedPc,
            )
        }
    }
}

@Composable
fun TopBarArea(
    onClose: () -> Unit,
    onCollapse: () -> Unit,
    state: SessionBridgeState,
    onOpenWorkspaceNav: () -> Unit,
    onToggleProcessPicker: () -> Unit,
) {
    Row(
        modifier = Modifier
            .fillMaxWidth()
            .height(48.dp)
            .background(MaterialTheme.colorScheme.surfaceVariant.copy(alpha = 0.8f))
            .padding(horizontal = 8.dp),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        Box(
            modifier = Modifier
                .width(36.dp)
                .height(36.dp),
        ) {
            AppListButton(state = state, onClick = onToggleProcessPicker)
        }
        IconButton(onClick = onOpenWorkspaceNav) {
            Text("...", color = MaterialTheme.colorScheme.onSurfaceVariant)
        }
        Text(
            text = stringResource(workspaceSectionLabelRes(state.workspaceSection)),
            style = MaterialTheme.typography.titleSmall,
            color = MaterialTheme.colorScheme.onSurfaceVariant,
            modifier = Modifier.padding(start = 8.dp),
        )
        Spacer(modifier = Modifier.weight(1f))
        IconButton(onClick = onCollapse) {
            Text("-", color = MaterialTheme.colorScheme.onSurfaceVariant)
        }
        IconButton(onClick = onClose) {
            Text("X", color = MaterialTheme.colorScheme.onSurfaceVariant)
        }
    }
}

@Composable
private fun WorkspaceNavDialog(
    selectedSection: WorkspaceSection,
    onDismiss: () -> Unit,
    onSectionSelected: (WorkspaceSection) -> Unit,
) {
    val sections = remember {
        listOf(
            WorkspaceSection.Memory,
            WorkspaceSection.Session,
            WorkspaceSection.Threads,
            WorkspaceSection.Events,
            WorkspaceSection.Processes,
        )
    }

    OverlayModalContainer(onDismiss = onDismiss) {
        Surface(
            modifier = Modifier.fillMaxWidth(),
            color = MaterialTheme.colorScheme.surface,
            tonalElevation = 4.dp,
        ) {
            Column(
                modifier = Modifier
                    .fillMaxWidth()
                    .padding(12.dp),
                verticalArrangement = Arrangement.spacedBy(8.dp),
            ) {
                Row(
                    modifier = Modifier.fillMaxWidth(),
                    verticalAlignment = Alignment.CenterVertically,
                ) {
                    SectionIntro(
                        title = "Workspace",
                        subtitle = "Memory is primary; use this to open secondary panels.",
                        modifier = Modifier.weight(1f),
                    )
                    OutlinedButton(onClick = onDismiss) {
                        Text(stringResource(R.string.memory_ranges_close))
                    }
                }

                sections.forEach { section ->
                    val isSelected = section == selectedSection
                    OutlinedButton(
                        onClick = { onSectionSelected(section) },
                        enabled = !isSelected,
                        modifier = Modifier.fillMaxWidth(),
                    ) {
                        Text(stringResource(workspaceSectionLabelRes(section)))
                    }
                }
            }
        }
    }
}

@Composable
fun CategoryColumn(
    sections: List<WorkspaceSection>,
    selectedSection: WorkspaceSection,
    onSectionSelected: (WorkspaceSection) -> Unit,
    modifier: Modifier,
) {
    Column(
        modifier = modifier.background(MaterialTheme.colorScheme.surfaceVariant),
    ) {
        sections.forEach { section ->
            val isSelected = selectedSection == section
            Box(
                modifier = Modifier
                    .fillMaxWidth()
                    .weight(1f)
                    .background(
                        if (isSelected) MaterialTheme.colorScheme.primaryContainer else Color.Transparent,
                    )
                    .clickable { onSectionSelected(section) },
                contentAlignment = Alignment.Center,
            ) {
                Icon(
                    painter = painterResource(id = workspaceSectionIconRes(section)),
                    contentDescription = stringResource(id = workspaceSectionLabelRes(section)),
                    tint = if (isSelected) {
                        MaterialTheme.colorScheme.onPrimaryContainer
                    } else {
                        MaterialTheme.colorScheme.onSurfaceVariant
                    },
                )
            }
        }
    }
}

@Composable
fun CategoryRow(
    sections: List<WorkspaceSection>,
    selectedSection: WorkspaceSection,
    onSectionSelected: (WorkspaceSection) -> Unit,
    modifier: Modifier,
) {
    Row(
        modifier = modifier
            .background(MaterialTheme.colorScheme.surfaceVariant)
            .height(48.dp),
    ) {
        sections.forEach { section ->
            val isSelected = selectedSection == section
            Box(
                modifier = Modifier
                    .weight(1f)
                    .fillMaxHeight()
                    .background(
                        if (isSelected) MaterialTheme.colorScheme.primaryContainer else Color.Transparent,
                    )
                    .clickable { onSectionSelected(section) },
                contentAlignment = Alignment.Center,
            ) {
                Icon(
                    painter = painterResource(id = workspaceSectionIconRes(section)),
                    contentDescription = stringResource(id = workspaceSectionLabelRes(section)),
                    tint = if (isSelected) {
                        MaterialTheme.colorScheme.onPrimaryContainer
                    } else {
                        MaterialTheme.colorScheme.onSurfaceVariant
                    },
                )
            }
        }
    }
}

@Composable
fun AppListButton(state: SessionBridgeState, onClick: () -> Unit) {
    IconButton(onClick = onClick) {
        val targetPid = state.snapshot.targetPid
        val currentProcess = state.processes.find { it.pid == targetPid }
        val packageName = currentProcess?.iconPackageName ?: ""

        if (packageName.isNotBlank() && packageName != "None") {
            val context = androidx.compose.ui.platform.LocalContext.current
            val iconLoader = remember(context) { com.smlc666.lkmdbg.shell.AppIconLoader(context) }
            val icon = remember(packageName) { iconLoader.load(packageName) }

            if (icon != null) {
                val imageBitmap = remember(icon) {
                    val bitmap = Bitmap.createBitmap(
                        icon.intrinsicWidth.coerceAtLeast(1),
                        icon.intrinsicHeight.coerceAtLeast(1),
                        Bitmap.Config.ARGB_8888,
                    )
                    val canvas = Canvas(bitmap)
                    icon.setBounds(0, 0, canvas.width, canvas.height)
                    icon.draw(canvas)
                    bitmap.asImageBitmap()
                }
                Image(
                    bitmap = imageBitmap,
                    contentDescription = "App Icon",
                    modifier = Modifier.fillMaxSize(),
                )
            } else {
                Icon(
                    painter = painterResource(id = R.drawable.ic_lkmdbg_terminal),
                    contentDescription = stringResource(R.string.process_select_prompt),
                    tint = MaterialTheme.colorScheme.onSurfaceVariant,
                )
            }
        } else {
            Icon(
                painter = painterResource(id = R.drawable.ic_lkmdbg_terminal),
                contentDescription = stringResource(R.string.process_select_prompt),
                tint = MaterialTheme.colorScheme.onSurfaceVariant,
            )
        }
    }
}

@Composable
fun MainContentArea(
    state: SessionBridgeState,
    memoryViewMode: Int,
    memoryToolsOpen: Boolean,
    onTargetPidInputChanged: (String) -> Unit,
    onTargetTidInputChanged: (String) -> Unit,
    onConnect: () -> Unit,
    onOpenSession: () -> Unit,
    onRefreshSession: () -> Unit,
    onAttachTarget: () -> Unit,
    onRefreshStopState: () -> Unit,
    onFreezeThreads: () -> Unit,
    onThawThreads: () -> Unit,
    onContinueTarget: () -> Unit,
    onSingleStep: () -> Unit,
    onRefreshProcesses: () -> Unit,
    onProcessFilterSelected: (ProcessFilter) -> Unit,
    onSelectProcess: (Int) -> Unit,
    onAttachSelectedProcess: (Int) -> Unit,
    onRefreshThreads: () -> Unit,
    onSelectThread: (Int) -> Unit,
    onRefreshSelectedThreadRegisters: () -> Unit,
    onHwpointAddressChanged: (String) -> Unit,
    onHwpointLengthChanged: (String) -> Unit,
    onCycleHwpointPreset: () -> Unit,
    onUseSelectedPcForHwpoint: () -> Unit,
    onUseMemoryFocusForHwpoint: () -> Unit,
    onAddHwpoint: () -> Unit,
    onSelectHwpoint: (ULong) -> Unit,
    onRemoveSelectedHwpoint: () -> Unit,
    onRefreshHwpoints: () -> Unit,
    onRefreshEvents: () -> Unit,
    onToggleEventsAutoPoll: () -> Unit,
    onClearEvents: () -> Unit,
    onTogglePinnedEvent: (ULong) -> Unit,
    onOpenEventThread: (Int) -> Unit,
    onOpenEventValue: (ULong) -> Unit,
    onStepMemoryPage: (Int) -> Unit,
    onSelectMemoryAddress: (ULong) -> Unit,
    onMemorySearchQueryChanged: (String) -> Unit,
    onMemoryAddressInputChanged: (String) -> Unit,
    onMemorySelectionSizeChanged: (Int) -> Unit,
    onMemoryWriteHexChanged: (String) -> Unit,
    onMemoryWriteAsciiChanged: (String) -> Unit,
    onMemoryWriteAsmChanged: (String) -> Unit,
    onCycleMemorySearchValueType: () -> Unit,
    onCycleMemorySearchRefineMode: () -> Unit,
    onCycleMemoryRegionPreset: () -> Unit,
    onJumpMemoryAddress: () -> Unit,
    onLoadSelectionIntoHexSearch: () -> Unit,
    onLoadSelectionIntoAsciiSearch: () -> Unit,
    onLoadSelectionIntoEditors: () -> Unit,
    onWriteHexAtFocus: () -> Unit,
    onWriteAsciiAtFocus: () -> Unit,
    onAssembleToEditors: () -> Unit,
    onAssembleAndWrite: () -> Unit,
    onRunMemorySearch: () -> Unit,
    onRefineMemorySearch: () -> Unit,
    onRefreshVmas: () -> Unit,
    onRefreshImages: () -> Unit,
    onShowMemoryResults: () -> Unit,
    onShowMemoryPage: () -> Unit,
    onPreviewSelectedPc: () -> Unit,
) {
    when (state.workspaceSection) {
        WorkspaceSection.Session -> SessionSectionContent(
            state = state,
            onTargetPidInputChanged = onTargetPidInputChanged,
            onTargetTidInputChanged = onTargetTidInputChanged,
            onConnect = onConnect,
            onOpenSession = onOpenSession,
            onRefreshSession = onRefreshSession,
            onAttachTarget = onAttachTarget,
            onRefreshStopState = onRefreshStopState,
            onFreezeThreads = onFreezeThreads,
            onThawThreads = onThawThreads,
            onContinueTarget = onContinueTarget,
            onSingleStep = onSingleStep,
            onHwpointAddressChanged = onHwpointAddressChanged,
            onHwpointLengthChanged = onHwpointLengthChanged,
            onCycleHwpointPreset = onCycleHwpointPreset,
            onUseSelectedPcForHwpoint = onUseSelectedPcForHwpoint,
            onUseMemoryFocusForHwpoint = onUseMemoryFocusForHwpoint,
            onAddHwpoint = onAddHwpoint,
            onSelectHwpoint = onSelectHwpoint,
            onRemoveSelectedHwpoint = onRemoveSelectedHwpoint,
            onRefreshHwpoints = onRefreshHwpoints,
        )

        WorkspaceSection.Processes -> ProcessesSectionContent(
            state = state,
            onRefreshProcesses = onRefreshProcesses,
            onProcessFilterSelected = onProcessFilterSelected,
            onSelectProcess = onSelectProcess,
            onAttachSelectedProcess = onAttachSelectedProcess,
        )

        WorkspaceSection.Memory -> MemorySectionContent(
            state = state,
            memoryToolsOpen = memoryToolsOpen,
            viewMode = memoryViewMode,
            onStepPage = onStepMemoryPage,
            onSelectAddress = onSelectMemoryAddress,
            onSearchQueryChanged = onMemorySearchQueryChanged,
            onAddressInputChanged = onMemoryAddressInputChanged,
            onSelectionSizeChanged = onMemorySelectionSizeChanged,
            onHexInputChanged = onMemoryWriteHexChanged,
            onAsciiInputChanged = onMemoryWriteAsciiChanged,
            onAsmInputChanged = onMemoryWriteAsmChanged,
            onCycleValueType = onCycleMemorySearchValueType,
            onCycleRefineMode = onCycleMemorySearchRefineMode,
            onCycleRegionPreset = onCycleMemoryRegionPreset,
            onJumpAddress = onJumpMemoryAddress,
            onLoadSelectionIntoHexSearch = onLoadSelectionIntoHexSearch,
            onLoadSelectionIntoAsciiSearch = onLoadSelectionIntoAsciiSearch,
            onLoadSelectionIntoEditors = onLoadSelectionIntoEditors,
            onWriteHexAtFocus = onWriteHexAtFocus,
            onWriteAsciiAtFocus = onWriteAsciiAtFocus,
            onAssembleToEditors = onAssembleToEditors,
            onAssembleAndWrite = onAssembleAndWrite,
            onRunSearch = onRunMemorySearch,
            onRefineSearch = onRefineMemorySearch,
            onRefreshVmas = onRefreshVmas,
            onRefreshImages = onRefreshImages,
            onShowResults = onShowMemoryResults,
            onShowPage = onShowMemoryPage,
            onPreviewSelectedPc = onPreviewSelectedPc,
        )

        WorkspaceSection.Threads -> ThreadsSectionContent(
            state = state,
            onRefreshThreads = onRefreshThreads,
            onSelectThread = onSelectThread,
            onRefreshSelectedThreadRegisters = onRefreshSelectedThreadRegisters,
            onPreviewSelectedPc = onPreviewSelectedPc,
            onSingleStep = onSingleStep,
        )

        WorkspaceSection.Events -> EventsSectionContent(
            state = state,
            onRefreshEvents = onRefreshEvents,
            onToggleEventsAutoPoll = onToggleEventsAutoPoll,
            onClearEvents = onClearEvents,
            onTogglePinnedEvent = onTogglePinnedEvent,
            onOpenEventThread = onOpenEventThread,
            onOpenEventValue = onOpenEventValue,
        )
    }
}

@Composable
private fun SessionSectionContent(
    state: SessionBridgeState,
    onTargetPidInputChanged: (String) -> Unit,
    onTargetTidInputChanged: (String) -> Unit,
    onConnect: () -> Unit,
    onOpenSession: () -> Unit,
    onRefreshSession: () -> Unit,
    onAttachTarget: () -> Unit,
    onRefreshStopState: () -> Unit,
    onFreezeThreads: () -> Unit,
    onThawThreads: () -> Unit,
    onContinueTarget: () -> Unit,
    onSingleStep: () -> Unit,
    onHwpointAddressChanged: (String) -> Unit,
    onHwpointLengthChanged: (String) -> Unit,
    onCycleHwpointPreset: () -> Unit,
    onUseSelectedPcForHwpoint: () -> Unit,
    onUseMemoryFocusForHwpoint: () -> Unit,
    onAddHwpoint: () -> Unit,
    onSelectHwpoint: (ULong) -> Unit,
    onRemoveSelectedHwpoint: () -> Unit,
    onRefreshHwpoints: () -> Unit,
) {
    val controlsBusy = state.busy || state.sessionControlsBusy
    var sessionControlsPanelOpen by rememberSaveable { mutableStateOf(false) }
    var executionControlsPanelOpen by rememberSaveable { mutableStateOf(false) }
    var hwpointControlsPanelOpen by rememberSaveable { mutableStateOf(false) }
    LazyColumn(
        modifier = Modifier.fillMaxSize(),
        contentPadding = PaddingValues(bottom = 16.dp),
        verticalArrangement = Arrangement.spacedBy(12.dp),
    ) {
        item {
            SectionIntro(
                title = stringResource(R.string.session_panel_title),
                subtitle = stringResource(R.string.session_panel_subtitle),
            )
        }

        item {
            SectionCard(title = stringResource(R.string.session_panel_title)) {
                Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                    OutlinedTextField(
                        value = state.targetPidInput,
                        onValueChange = onTargetPidInputChanged,
                        modifier = Modifier.weight(1f),
                        label = { Text(stringResource(R.string.session_target_pid)) },
                        singleLine = true,
                    )
                    OutlinedTextField(
                        value = state.targetTidInput,
                        onValueChange = onTargetTidInputChanged,
                        modifier = Modifier.weight(1f),
                        label = { Text(stringResource(R.string.session_target_tid)) },
                        singleLine = true,
                    )
                }
                Spacer(modifier = Modifier.height(8.dp))
                ActionRow(
                    listOf(
                        stringResource(R.string.session_action_attach_target) to onAttachTarget,
                        stringResource(R.string.session_action_open_controls) to {
                            sessionControlsPanelOpen = true
                        },
                    ),
                    enabled = !controlsBusy,
                )
                Spacer(modifier = Modifier.height(8.dp))
                SessionStatusSummary(state)
            }
        }

        item {
            SectionCard(title = stringResource(R.string.session_execution_title)) {
                ActionRow(
                    listOf(
                        stringResource(R.string.session_action_open_execution_controls) to {
                            executionControlsPanelOpen = true
                        },
                    ),
                    enabled = !controlsBusy,
                )
                Spacer(modifier = Modifier.height(8.dp))
                StopStateSummary(state.stopState)
            }
        }

        item {
            SectionCard(title = stringResource(R.string.hwpoint_title)) {
                Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                    OutlinedTextField(
                        value = state.hwpointAddressInput,
                        onValueChange = onHwpointAddressChanged,
                        modifier = Modifier.weight(1.5f),
                        label = { Text(stringResource(R.string.hwpoint_field_address)) },
                        singleLine = true,
                    )
                    OutlinedTextField(
                        value = state.hwpointLengthInput,
                        onValueChange = onHwpointLengthChanged,
                        modifier = Modifier.weight(1f),
                        label = { Text(stringResource(R.string.hwpoint_field_length)) },
                        singleLine = true,
                    )
                }
                Spacer(modifier = Modifier.height(8.dp))
                ActionRow(
                    actions = listOf(
                        stringResource(R.string.hwpoint_action_open_controls) to {
                            hwpointControlsPanelOpen = true
                        },
                        stringResource(R.string.hwpoint_action_refresh) to onRefreshHwpoints,
                    ),
                    enabled = !controlsBusy,
                )
                Spacer(modifier = Modifier.height(8.dp))
                HwpointList(
                    hwpoints = state.hwpoints,
                    selectedId = state.selectedHwpointId,
                    onSelect = onSelectHwpoint,
                )
            }
        }
    }
    if (sessionControlsPanelOpen) {
        SessionControlsDialog(
            controlsBusy = controlsBusy,
            onDismiss = { sessionControlsPanelOpen = false },
            onConnect = onConnect,
            onOpenSession = onOpenSession,
            onRefreshSession = onRefreshSession,
            onRefreshStopState = onRefreshStopState,
        )
    }
    if (executionControlsPanelOpen) {
        ExecutionControlsDialog(
            controlsBusy = controlsBusy,
            onDismiss = { executionControlsPanelOpen = false },
            onFreezeThreads = onFreezeThreads,
            onThawThreads = onThawThreads,
            onContinueTarget = onContinueTarget,
            onSingleStep = onSingleStep,
        )
    }
    if (hwpointControlsPanelOpen) {
        HwpointControlsDialog(
            state = state,
            controlsBusy = controlsBusy,
            onDismiss = { hwpointControlsPanelOpen = false },
            onCycleHwpointPreset = onCycleHwpointPreset,
            onUseSelectedPcForHwpoint = onUseSelectedPcForHwpoint,
            onUseMemoryFocusForHwpoint = onUseMemoryFocusForHwpoint,
            onAddHwpoint = onAddHwpoint,
            onRemoveSelectedHwpoint = onRemoveSelectedHwpoint,
            onRefreshHwpoints = onRefreshHwpoints,
        )
    }
}

@Composable
private fun SessionControlsDialog(
    controlsBusy: Boolean,
    onDismiss: () -> Unit,
    onConnect: () -> Unit,
    onOpenSession: () -> Unit,
    onRefreshSession: () -> Unit,
    onRefreshStopState: () -> Unit,
) {
    ActionDialog(
        title = stringResource(R.string.session_controls_panel_title),
        subtitle = stringResource(R.string.session_controls_panel_subtitle),
        onDismiss = onDismiss,
    ) {
        ActionRow(
            actions = listOf(
                stringResource(R.string.session_action_connect) to onConnect,
                stringResource(R.string.session_action_open_session) to onOpenSession,
            ),
            enabled = !controlsBusy,
        )
        Spacer(modifier = Modifier.height(8.dp))
        ActionRow(
            actions = listOf(
                stringResource(R.string.session_action_refresh) to onRefreshSession,
                stringResource(R.string.session_action_refresh_stop) to onRefreshStopState,
            ),
            enabled = !controlsBusy,
        )
    }
}

@Composable
private fun ExecutionControlsDialog(
    controlsBusy: Boolean,
    onDismiss: () -> Unit,
    onFreezeThreads: () -> Unit,
    onThawThreads: () -> Unit,
    onContinueTarget: () -> Unit,
    onSingleStep: () -> Unit,
) {
    ActionDialog(
        title = stringResource(R.string.session_execution_panel_title),
        subtitle = stringResource(R.string.session_execution_panel_subtitle),
        onDismiss = onDismiss,
    ) {
        ActionRow(
            actions = listOf(
                stringResource(R.string.session_action_freeze) to onFreezeThreads,
                stringResource(R.string.session_action_thaw) to onThawThreads,
            ),
            enabled = !controlsBusy,
        )
        Spacer(modifier = Modifier.height(8.dp))
        ActionRow(
            actions = listOf(
                stringResource(R.string.session_action_continue) to onContinueTarget,
                stringResource(R.string.session_action_single_step) to onSingleStep,
            ),
            enabled = !controlsBusy,
        )
    }
}

@Composable
private fun HwpointControlsDialog(
    state: SessionBridgeState,
    controlsBusy: Boolean,
    onDismiss: () -> Unit,
    onCycleHwpointPreset: () -> Unit,
    onUseSelectedPcForHwpoint: () -> Unit,
    onUseMemoryFocusForHwpoint: () -> Unit,
    onAddHwpoint: () -> Unit,
    onRemoveSelectedHwpoint: () -> Unit,
    onRefreshHwpoints: () -> Unit,
) {
    ActionDialog(
        title = stringResource(R.string.hwpoint_controls_panel_title),
        subtitle = stringResource(R.string.hwpoint_controls_panel_subtitle),
        onDismiss = onDismiss,
    ) {
        ActionRow(
            actions = listOf(
                stringResource(hwpointPresetLabelRes(state.hwpointPreset)) to onCycleHwpointPreset,
            ),
            enabled = !controlsBusy,
        )
        Spacer(modifier = Modifier.height(8.dp))
        ActionRow(
            actions = listOf(
                stringResource(R.string.hwpoint_action_use_pc) to onUseSelectedPcForHwpoint,
                stringResource(R.string.hwpoint_action_use_memory) to onUseMemoryFocusForHwpoint,
            ),
            enabled = !controlsBusy,
        )
        Spacer(modifier = Modifier.height(8.dp))
        ActionRow(
            actions = listOf(
                stringResource(R.string.hwpoint_action_add) to onAddHwpoint,
                stringResource(R.string.hwpoint_action_remove_selected) to onRemoveSelectedHwpoint,
                stringResource(R.string.hwpoint_action_refresh) to onRefreshHwpoints,
            ),
            enabled = !controlsBusy,
        )
    }
}

@Composable
private fun SessionStatusSummary(state: SessionBridgeState) {
    val scheme = MaterialTheme.colorScheme
    val badges = buildList {
        add(
            StatusBadgeSpec(
                label = stringResource(
                    if (state.snapshot.connected) {
                        R.string.session_badge_bridge_connected
                    } else {
                        R.string.session_badge_bridge_disconnected
                    },
                ),
                containerColor = if (state.snapshot.connected) scheme.primaryContainer else scheme.surfaceVariant,
                contentColor = if (state.snapshot.connected) scheme.onPrimaryContainer else scheme.onSurfaceVariant,
            ),
        )
        add(
            StatusBadgeSpec(
                label = stringResource(
                    if (state.snapshot.sessionOpen) {
                        R.string.session_badge_open
                    } else {
                        R.string.session_badge_closed
                    },
                ),
                containerColor = if (state.snapshot.sessionOpen) scheme.secondaryContainer else scheme.surfaceVariant,
                contentColor = if (state.snapshot.sessionOpen) scheme.onSecondaryContainer else scheme.onSurfaceVariant,
            ),
        )
        add(
            StatusBadgeSpec(
                label = stringResource(
                    if (state.snapshot.targetPid > 0) {
                        R.string.session_badge_target_attached
                    } else {
                        R.string.session_badge_target_idle
                    },
                ),
                containerColor = if (state.snapshot.targetPid > 0) scheme.tertiaryContainer else scheme.surfaceVariant,
                contentColor = if (state.snapshot.targetPid > 0) scheme.onTertiaryContainer else scheme.onSurfaceVariant,
            ),
        )
        add(
            StatusBadgeSpec(
                label = state.stopState?.let { stop ->
                    stringResource(R.string.session_badge_stop_reason, stopReasonLabel(stop.reason))
                } ?: stringResource(R.string.session_badge_running),
                containerColor = if (state.stopState != null) scheme.errorContainer else scheme.surfaceVariant,
                contentColor = if (state.stopState != null) scheme.onErrorContainer else scheme.onSurfaceVariant,
            ),
        )
        add(
            StatusBadgeSpec(
                label = stringResource(R.string.session_badge_hwpoints, state.hwpoints.size),
                containerColor = scheme.secondaryContainer,
                contentColor = scheme.onSecondaryContainer,
            ),
        )
        add(
            StatusBadgeSpec(
                label = stringResource(R.string.session_badge_queue, state.snapshot.eventQueueDepth.toString()),
                containerColor = scheme.surfaceVariant,
                contentColor = scheme.onSurfaceVariant,
            ),
        )
    }
    val metrics = listOf(
        MetricTileSpec(
            label = stringResource(R.string.session_metric_transport),
            value = state.snapshot.transport,
        ),
        MetricTileSpec(
            label = stringResource(R.string.session_metric_session_id),
            value = hex64(state.snapshot.sessionId),
        ),
        MetricTileSpec(
            label = stringResource(R.string.session_metric_target),
            value = "${state.snapshot.targetPid}:${state.snapshot.targetTid}",
        ),
        MetricTileSpec(
            label = stringResource(R.string.session_metric_owner),
            value = state.snapshot.ownerPid.toString(),
        ),
        MetricTileSpec(
            label = stringResource(R.string.session_metric_hook),
            value = state.snapshot.hookActive.toString(),
        ),
        MetricTileSpec(
            label = stringResource(R.string.session_metric_hwpoints),
            value = state.hwpoints.size.toString(),
        ),
    )
    Column(verticalArrangement = Arrangement.spacedBy(8.dp)) {
        CompactBadgeRow(badges)
        MetricTileGrid(metrics = metrics)
        SectionMetricBanner(
            label = stringResource(R.string.session_metric_last_message),
            value = state.lastMessage,
        )
    }
}

@Composable
private fun StopStateSummary(stop: BridgeStopState?) {
    if (stop == null) {
        Text(
            text = stringResource(R.string.session_stop_state_idle),
            color = MaterialTheme.colorScheme.onSurfaceVariant,
        )
        return
    }
    val scheme = MaterialTheme.colorScheme
    val badges = buildList {
        add(
            StatusBadgeSpec(
                label = stringResource(R.string.session_badge_stop_reason, stopReasonLabel(stop.reason)),
                containerColor = scheme.errorContainer,
                contentColor = scheme.onErrorContainer,
            ),
        )
        if ((stop.flags and STOP_FLAG_FROZEN) != 0u) {
            add(
                StatusBadgeSpec(
                    label = stringResource(R.string.session_badge_frozen),
                    containerColor = scheme.tertiaryContainer,
                    contentColor = scheme.onTertiaryContainer,
                ),
            )
        }
        if ((stop.flags and STOP_FLAG_REARM_REQUIRED) != 0u) {
            add(
                StatusBadgeSpec(
                    label = stringResource(R.string.session_badge_rearm),
                    containerColor = scheme.secondaryContainer,
                    contentColor = scheme.onSecondaryContainer,
                ),
            )
        }
        if ((stop.flags and STOP_FLAG_SYSCALL_CONTROL) != 0u) {
            add(
                StatusBadgeSpec(
                    label = stringResource(R.string.session_badge_syscall_control),
                    containerColor = scheme.primaryContainer,
                    contentColor = scheme.onPrimaryContainer,
                ),
            )
        }
        add(
            StatusBadgeSpec(
                label = stringResource(R.string.session_badge_stop_thread, stop.tid),
                containerColor = scheme.surfaceVariant,
                contentColor = scheme.onSurfaceVariant,
            ),
        )
    }
    val metrics = listOf(
        MetricTileSpec(
            label = stringResource(R.string.session_metric_stop_cookie),
            value = hex64(stop.cookie),
        ),
        MetricTileSpec(
            label = stringResource(R.string.session_metric_stop_tid),
            value = "${stop.tgid}:${stop.tid}",
        ),
        MetricTileSpec(
            label = stringResource(R.string.session_metric_stop_flags),
            value = stopFlagsLabel(stop.flags),
        ),
        MetricTileSpec(
            label = stringResource(R.string.session_metric_stop_event_flags),
            value = hex32(stop.eventFlags),
        ),
        MetricTileSpec(
            label = stringResource(R.string.session_metric_stop_pc),
            value = hex64(stop.pc),
        ),
        MetricTileSpec(
            label = stringResource(R.string.session_metric_stop_sp),
            value = hex64(stop.sp),
        ),
        MetricTileSpec(
            label = stringResource(R.string.session_metric_stop_args),
            value = "${hex64(stop.x0)} / ${hex64(stop.x1)}",
        ),
        MetricTileSpec(
            label = stringResource(R.string.session_metric_stop_frame),
            value = "${hex64(stop.x29)} / ${hex64(stop.x30)}",
        ),
        MetricTileSpec(
            label = stringResource(R.string.session_metric_stop_values),
            value = "${hex64(stop.value0)} / ${hex64(stop.value1)}",
        ),
        MetricTileSpec(
            label = stringResource(R.string.session_metric_stop_pstate),
            value = hex64(stop.pstate),
        ),
    )
    Column(verticalArrangement = Arrangement.spacedBy(8.dp)) {
        CompactBadgeRow(badges)
        MetricTileGrid(metrics = metrics)
    }
}

@Composable
private fun ProcessesSectionContent(
    state: SessionBridgeState,
    onRefreshProcesses: () -> Unit,
    onProcessFilterSelected: (ProcessFilter) -> Unit,
    onSelectProcess: (Int) -> Unit,
    onAttachSelectedProcess: (Int) -> Unit,
) {
    val filtered = state.processes.filter { state.processFilter.matches(it) }
    var processFiltersOpen by rememberSaveable { mutableStateOf(false) }
    val processFilterLabels = mapOf(
        ProcessFilter.All to stringResource(R.string.process_filter_all),
        ProcessFilter.AndroidApps to stringResource(R.string.process_filter_android),
        ProcessFilter.CommandLine to stringResource(R.string.process_filter_cmdline),
        ProcessFilter.SystemApps to stringResource(R.string.process_filter_system),
        ProcessFilter.UserApps to stringResource(R.string.process_filter_user),
    )
    Column(modifier = Modifier.fillMaxSize()) {
        SectionIntro(
            title = stringResource(R.string.process_panel_title),
            subtitle = stringResource(R.string.process_panel_subtitle),
        )
        Spacer(modifier = Modifier.height(8.dp))
        ActionRow(
            actions = listOf(
                stringResource(R.string.process_action_refresh) to onRefreshProcesses,
                stringResource(R.string.process_action_open_filters) to { processFiltersOpen = true },
            ),
        )
        Spacer(modifier = Modifier.height(8.dp))
        Text(
            text = stringResource(
                R.string.process_filter_current,
                processFilterLabels.getValue(state.processFilter),
                filtered.size,
                state.processes.size,
            ),
            style = MaterialTheme.typography.bodySmall,
            color = MaterialTheme.colorScheme.onSurfaceVariant,
        )
        Spacer(modifier = Modifier.height(8.dp))
        if (filtered.isEmpty()) {
            EmptyState(stringResource(R.string.process_filter_empty))
            return@Column
        }
        LazyColumn(
            modifier = Modifier.fillMaxSize(),
            verticalArrangement = Arrangement.spacedBy(8.dp),
        ) {
            items(filtered, key = { it.pid }) { process ->
                ProcessRow(
                    process = process,
                    selected = state.selectedProcessPid == process.pid,
                    attached = state.snapshot.targetPid == process.pid,
                    onSelect = { onSelectProcess(process.pid) },
                    onAttach = { onAttachSelectedProcess(process.pid) },
                )
            }
        }
    }
    if (processFiltersOpen) {
        ProcessFiltersDialog(
            selectedFilter = state.processFilter,
            processFilterLabels = processFilterLabels,
            onProcessFilterSelected = onProcessFilterSelected,
            onDismiss = { processFiltersOpen = false },
        )
    }
}

@Composable
private fun ProcessFiltersDialog(
    selectedFilter: ProcessFilter,
    processFilterLabels: Map<ProcessFilter, String>,
    onProcessFilterSelected: (ProcessFilter) -> Unit,
    onDismiss: () -> Unit,
) {
    ActionDialog(
        title = stringResource(R.string.process_filters_panel_title),
        subtitle = stringResource(R.string.process_filters_panel_subtitle),
        onDismiss = onDismiss,
    ) {
        CompactFilterRow(
            filters = ProcessFilter.entries.toList(),
            selectedFilter = selectedFilter,
            labelFor = { processFilterLabels.getValue(it) },
            onSelected = onProcessFilterSelected,
        )
    }
}

@Composable
private fun ThreadsSectionContent(
    state: SessionBridgeState,
    onRefreshThreads: () -> Unit,
    onSelectThread: (Int) -> Unit,
    onRefreshSelectedThreadRegisters: () -> Unit,
    onPreviewSelectedPc: () -> Unit,
    onSingleStep: () -> Unit,
) {
    val configuration = androidx.compose.ui.platform.LocalConfiguration.current
    val isLandscape = configuration.orientation == android.content.res.Configuration.ORIENTATION_LANDSCAPE
    val selectedThread = state.threads.firstOrNull { it.tid == state.selectedThreadTid }
    val parkedCount = state.threads.count { (it.flags and THREAD_FLAG_FREEZE_PARKED) != 0u }
    var threadActionsOpen by rememberSaveable { mutableStateOf(false) }
    Column(modifier = Modifier.fillMaxSize()) {
        SectionIntro(
            title = stringResource(R.string.thread_panel_title),
            subtitle = stringResource(R.string.thread_panel_subtitle),
        )
        Spacer(modifier = Modifier.height(8.dp))
        ActionRow(
            listOf(
                stringResource(R.string.thread_action_refresh) to onRefreshThreads,
                stringResource(R.string.thread_action_open_controls) to { threadActionsOpen = true },
            ),
        )
        Spacer(modifier = Modifier.height(8.dp))
        if (isLandscape) {
            Row(
                modifier = Modifier.fillMaxSize(),
                horizontalArrangement = Arrangement.spacedBy(8.dp),
            ) {
                ThreadListPanel(
                    state = state,
                    selectedThread = selectedThread,
                    parkedCount = parkedCount,
                    onSelectThread = onSelectThread,
                    modifier = Modifier.weight(1f),
                )
                ThreadRegistersPanel(
                    selectedTid = state.selectedThreadTid,
                    registers = state.selectedThreadRegisters,
                    modifier = Modifier.weight(1f),
                )
            }
        } else {
            Column(
                modifier = Modifier.fillMaxSize(),
                verticalArrangement = Arrangement.spacedBy(8.dp),
            ) {
                ThreadListPanel(
                    state = state,
                    selectedThread = selectedThread,
                    parkedCount = parkedCount,
                    onSelectThread = onSelectThread,
                    modifier = Modifier.weight(1f),
                )
                ThreadRegistersPanel(
                    selectedTid = state.selectedThreadTid,
                    registers = state.selectedThreadRegisters,
                    modifier = Modifier.weight(1f),
                )
            }
        }
    }
    if (threadActionsOpen) {
        ActionDialog(
            title = stringResource(R.string.thread_controls_panel_title),
            subtitle = stringResource(R.string.thread_controls_panel_subtitle),
            onDismiss = { threadActionsOpen = false },
        ) {
            ActionRow(
                actions = listOf(
                    stringResource(R.string.thread_action_refresh_registers) to onRefreshSelectedThreadRegisters,
                    stringResource(R.string.memory_action_preview_pc) to onPreviewSelectedPc,
                ),
            )
            Spacer(modifier = Modifier.height(8.dp))
            ActionRow(
                actions = listOf(
                    stringResource(R.string.session_action_single_step) to onSingleStep,
                ),
            )
        }
    }
}

@Composable
private fun EventsSectionContent(
    state: SessionBridgeState,
    onRefreshEvents: () -> Unit,
    onToggleEventsAutoPoll: () -> Unit,
    onClearEvents: () -> Unit,
    onTogglePinnedEvent: (ULong) -> Unit,
    onOpenEventThread: (Int) -> Unit,
    onOpenEventValue: (ULong) -> Unit,
) {
    var filterText by rememberSaveable { mutableStateOf("") }
    var presetOrdinal by rememberSaveable { mutableStateOf(EventFilterPreset.All.ordinal) }
    var eventControlsOpen by rememberSaveable { mutableStateOf(false) }
    val preset = EventFilterPreset.entries[presetOrdinal.coerceIn(0, EventFilterPreset.entries.lastIndex)]
    val presetLabels = mapOf(
        EventFilterPreset.All to stringResource(R.string.event_filter_all),
        EventFilterPreset.Pinned to stringResource(R.string.event_filter_pinned),
        EventFilterPreset.Stops to stringResource(R.string.event_filter_stops),
        EventFilterPreset.Signals to stringResource(R.string.event_filter_signals),
        EventFilterPreset.Syscalls to stringResource(R.string.event_filter_syscalls),
    )
    val filtered = sortEvents(
        events = state.recentEvents.filter { entry ->
            eventMatchesPreset(entry, preset, state.pinnedEventSeqs) &&
                eventMatchesFilter(entry, filterText)
        },
        pinnedEventSeqs = state.pinnedEventSeqs,
    )
    Column(modifier = Modifier.fillMaxSize()) {
        SectionIntro(
            title = stringResource(R.string.event_panel_title),
            subtitle = stringResource(R.string.event_panel_subtitle),
        )
        Spacer(modifier = Modifier.height(8.dp))
        ActionRow(
            listOf(
                stringResource(R.string.event_action_refresh) to onRefreshEvents,
                stringResource(R.string.event_action_open_controls) to { eventControlsOpen = true },
            ),
        )
        Spacer(modifier = Modifier.height(8.dp))
        OutlinedTextField(
            value = filterText,
            onValueChange = { filterText = it },
            modifier = Modifier.fillMaxWidth(),
            label = { Text(stringResource(R.string.event_filter_label)) },
            singleLine = true,
        )
        Spacer(modifier = Modifier.height(8.dp))
        CompactFilterRow(
            filters = EventFilterPreset.entries.toList(),
            selectedFilter = preset,
            labelFor = { presetLabels.getValue(it) },
            onSelected = { selected -> presetOrdinal = selected.ordinal },
        )
        Spacer(modifier = Modifier.height(8.dp))
        Text(
            text = stringResource(
                R.string.event_summary_counts,
                state.snapshot.eventQueueDepth.toInt(),
                filtered.size,
                state.pinnedEventSeqs.size,
            ),
            style = MaterialTheme.typography.bodySmall,
            color = MaterialTheme.colorScheme.onSurfaceVariant,
        )
        Spacer(modifier = Modifier.height(8.dp))
        if (filtered.isEmpty()) {
            EmptyState(stringResource(R.string.event_empty))
            return@Column
        }
        LazyColumn(
            modifier = Modifier.fillMaxSize(),
            verticalArrangement = Arrangement.spacedBy(8.dp),
        ) {
            items(filtered, key = { it.record.seq.toString() }) { event ->
                EventRow(
                    entry = event,
                    pinned = event.record.seq in state.pinnedEventSeqs,
                    onTogglePin = { onTogglePinnedEvent(event.record.seq) },
                    onOpenThread = onOpenEventThread,
                    onOpenValue = onOpenEventValue,
                )
            }
        }
    }
    if (eventControlsOpen) {
        ActionDialog(
            title = stringResource(R.string.event_controls_panel_title),
            subtitle = stringResource(R.string.event_controls_panel_subtitle),
            onDismiss = { eventControlsOpen = false },
        ) {
            ActionRow(
                actions = listOf(
                    stringResource(
                        if (state.eventsAutoPollEnabled) {
                            R.string.event_action_autopoll_stop
                        } else {
                            R.string.event_action_autopoll_start
                        },
                    ) to onToggleEventsAutoPoll,
                    stringResource(R.string.event_action_clear) to onClearEvents,
                ),
            )
        }
    }
}

@Composable
private fun MemorySectionContent(
    state: SessionBridgeState,
    memoryToolsOpen: Boolean,
    viewMode: Int,
    onStepPage: (Int) -> Unit,
    onSelectAddress: (ULong) -> Unit,
    onSearchQueryChanged: (String) -> Unit,
    onAddressInputChanged: (String) -> Unit,
    onSelectionSizeChanged: (Int) -> Unit,
    onHexInputChanged: (String) -> Unit,
    onAsciiInputChanged: (String) -> Unit,
    onAsmInputChanged: (String) -> Unit,
    onCycleValueType: () -> Unit,
    onCycleRefineMode: () -> Unit,
    onCycleRegionPreset: () -> Unit,
    onJumpAddress: () -> Unit,
    onLoadSelectionIntoHexSearch: () -> Unit,
    onLoadSelectionIntoAsciiSearch: () -> Unit,
    onLoadSelectionIntoEditors: () -> Unit,
    onWriteHexAtFocus: () -> Unit,
    onWriteAsciiAtFocus: () -> Unit,
    onAssembleToEditors: () -> Unit,
    onAssembleAndWrite: () -> Unit,
    onRunSearch: () -> Unit,
    onRefineSearch: () -> Unit,
    onRefreshVmas: () -> Unit,
    onRefreshImages: () -> Unit,
    onShowResults: () -> Unit,
    onShowPage: () -> Unit,
    onPreviewSelectedPc: () -> Unit,
) {
    val valueTypeLabel = stringResource(memorySearchValueTypeLabelRes(state.memorySearch.valueType))
    val refineModeLabel = stringResource(memorySearchRefineModeLabelRes(state.memorySearch.refineMode))
    val regionPresetLabel = stringResource(memoryRegionPresetLabelRes(state.memorySearch.regionPreset))
    val toolsLabel = stringResource(R.string.memory_action_tools)
    val selectionSizeLabels = mapOf(
        1 to stringResource(R.string.memory_selection_size, 1),
        2 to stringResource(R.string.memory_selection_size, 2),
        4 to stringResource(R.string.memory_selection_size, 4),
        8 to stringResource(R.string.memory_selection_size, 8),
        16 to stringResource(R.string.memory_selection_size, 16),
    )
    val selectionSummary = state.memoryPage?.let {
        stringResource(
            R.string.memory_selection_summary,
            state.memorySelectionSize,
            hex64(it.focusAddress),
        )
    }
    var searchPanelOpen by rememberSaveable { mutableStateOf(false) }

    Column(modifier = Modifier.fillMaxSize()) {
        Row(
            modifier = Modifier.fillMaxWidth(),
            verticalAlignment = Alignment.CenterVertically,
        ) {
            SectionIntro(
                title = stringResource(R.string.memory_panel_title),
                subtitle = buildString {
                    append(stringResource(R.string.memory_panel_subtitle))
                    append('\n')
                    append(valueTypeLabel)
                    append(" · ")
                    append(refineModeLabel)
                    append(" · ")
                    append(regionPresetLabel)
                    append(" · queue=")
                    append(state.memorySearch.results.size)
                    append(" · vmas=")
                    append(state.vmas.size)
                    append(" · images=")
                    append(state.images.size)
                    if (memoryToolsOpen)
                        append(" · $toolsLabel")
                },
                modifier = Modifier.weight(1f),
            )
            IconButton(
                onClick = { searchPanelOpen = true },
                modifier = Modifier
                    .width(36.dp)
                    .height(36.dp),
            ) {
                Icon(
                    painter = painterResource(id = R.drawable.ic_lkmdbg_search),
                    contentDescription = stringResource(R.string.memory_action_open_search_panel),
                    modifier = Modifier
                        .fillMaxSize()
                        .padding(4.dp),
                    tint = MaterialTheme.colorScheme.primary,
                )
            }
            if (viewMode == MEMORY_VIEW_MODE_RESULTS) {
                OutlinedButton(onClick = onShowPage) {
                    Text(stringResource(R.string.memory_action_show_page))
                }
            } else if (state.memorySearch.results.isNotEmpty()) {
                OutlinedButton(onClick = onShowResults) {
                    Text(
                        stringResource(
                            R.string.memory_action_show_results_count,
                            state.memorySearch.results.size,
                        ),
                    )
                }
            }
        }
        Spacer(modifier = Modifier.height(8.dp))
        if (state.memorySearch.summary.isNotBlank()) {
            Text(
                text = state.memorySearch.summary,
                style = MaterialTheme.typography.bodySmall,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
            )
            Spacer(modifier = Modifier.height(8.dp))
        }
        CompactActionRow(
            actions = listOf(
                stringResource(R.string.memory_action_refresh_ranges) to onRefreshVmas,
                stringResource(R.string.memory_action_refresh_images) to onRefreshImages,
            ),
        )
        if (viewMode == MEMORY_VIEW_MODE_PAGE) {
            Spacer(modifier = Modifier.height(8.dp))
            Row(modifier = Modifier.fillMaxWidth()) {
                OutlinedButton(onClick = { onStepPage(-1) }, modifier = Modifier.weight(1f)) {
                    Text(stringResource(R.string.memory_action_prev_page))
                }
                Spacer(modifier = Modifier.width(8.dp))
                OutlinedButton(onClick = { onStepPage(1) }, modifier = Modifier.weight(1f)) {
                    Text(stringResource(R.string.memory_action_next_page))
                }
            }
        }
        if (memoryToolsOpen) {
            Spacer(modifier = Modifier.height(12.dp))
            SectionCard(title = stringResource(R.string.memory_selection_title)) {
                if (selectionSummary != null) {
                    SummaryLine(selectionSummary)
                    Spacer(modifier = Modifier.height(8.dp))
                }
                FilterRow(
                    filters = listOf(1, 2, 4, 8, 16),
                    selectedFilter = state.memorySelectionSize,
                    labelFor = { size -> selectionSizeLabels.getValue(size) },
                    onSelected = onSelectionSizeChanged,
                )
                Spacer(modifier = Modifier.height(8.dp))
                ActionRow(
                    listOf(
                        stringResource(R.string.memory_action_selection_hex_search) to onLoadSelectionIntoHexSearch,
                        stringResource(R.string.memory_action_selection_ascii_search) to onLoadSelectionIntoAsciiSearch,
                        stringResource(R.string.memory_action_load_selection) to onLoadSelectionIntoEditors,
                    ),
                )
                Spacer(modifier = Modifier.height(8.dp))
                OutlinedTextField(
                    value = state.memoryWriteHexInput,
                    onValueChange = onHexInputChanged,
                    modifier = Modifier.fillMaxWidth(),
                    label = { Text(stringResource(R.string.memory_write_hex_label)) },
                )
                Spacer(modifier = Modifier.height(8.dp))
                ActionRow(
                    listOf(
                        stringResource(R.string.memory_action_write_hex) to onWriteHexAtFocus,
                        stringResource(R.string.memory_action_write_ascii) to onWriteAsciiAtFocus,
                    ),
                )
                Spacer(modifier = Modifier.height(8.dp))
                OutlinedTextField(
                    value = state.memoryWriteAsciiInput,
                    onValueChange = onAsciiInputChanged,
                    modifier = Modifier.fillMaxWidth(),
                    label = { Text(stringResource(R.string.memory_write_ascii_label)) },
                )
                Spacer(modifier = Modifier.height(8.dp))
                OutlinedTextField(
                    value = state.memoryWriteAsmInput,
                    onValueChange = onAsmInputChanged,
                    modifier = Modifier.fillMaxWidth(),
                    label = { Text(stringResource(R.string.memory_write_asm_label)) },
                    minLines = 3,
                    maxLines = 4,
                )
                Spacer(modifier = Modifier.height(8.dp))
                ActionRow(
                    listOf(
                        stringResource(R.string.memory_action_asm_to_editor) to onAssembleToEditors,
                        stringResource(R.string.memory_action_asm_write) to onAssembleAndWrite,
                    ),
                )
            }
        }
        Spacer(modifier = Modifier.height(12.dp))
        Surface(
            modifier = Modifier
                .weight(1f)
                .fillMaxWidth(),
            color = MaterialTheme.colorScheme.surface,
            tonalElevation = 2.dp,
        ) {
            if (viewMode == MEMORY_VIEW_MODE_RESULTS) {
                MemoryResultsList(
                    state = state,
                    onSelectAddress = onSelectAddress,
                )
            } else {
                MemoryPageList(
                    page = state.memoryPage,
                    onSelectAddress = onSelectAddress,
                    showDisassembly = memoryToolsOpen,
                )
            }
        }
    }
    if (searchPanelOpen) {
        MemorySearchDialog(
            state = state,
            valueTypeLabel = valueTypeLabel,
            refineModeLabel = refineModeLabel,
            regionPresetLabel = regionPresetLabel,
            onDismiss = { searchPanelOpen = false },
            onSearchQueryChanged = onSearchQueryChanged,
            onAddressInputChanged = onAddressInputChanged,
            onCycleValueType = onCycleValueType,
            onCycleRefineMode = onCycleRefineMode,
            onCycleRegionPreset = onCycleRegionPreset,
            onJumpAddress = onJumpAddress,
            onRunSearch = onRunSearch,
            onRefineSearch = onRefineSearch,
            onPreviewSelectedPc = onPreviewSelectedPc,
            onShowResults = onShowResults,
            onShowPage = onShowPage,
            viewMode = viewMode,
        )
    }
}

@Composable
private fun MemorySearchDialog(
    state: SessionBridgeState,
    valueTypeLabel: String,
    refineModeLabel: String,
    regionPresetLabel: String,
    onDismiss: () -> Unit,
    onSearchQueryChanged: (String) -> Unit,
    onAddressInputChanged: (String) -> Unit,
    onCycleValueType: () -> Unit,
    onCycleRefineMode: () -> Unit,
    onCycleRegionPreset: () -> Unit,
    onJumpAddress: () -> Unit,
    onRunSearch: () -> Unit,
    onRefineSearch: () -> Unit,
    onPreviewSelectedPc: () -> Unit,
    onShowResults: () -> Unit,
    onShowPage: () -> Unit,
    viewMode: Int,
) {
    ActionDialog(
        title = stringResource(R.string.memory_search_panel_title),
        subtitle = stringResource(R.string.memory_search_panel_subtitle),
        onDismiss = onDismiss,
    ) {
        if (state.memorySearch.summary.isNotBlank()) {
            Text(
                text = state.memorySearch.summary,
                style = MaterialTheme.typography.bodySmall,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
            )
            Spacer(modifier = Modifier.height(8.dp))
        }
        OutlinedTextField(
            value = state.memorySearch.query,
            onValueChange = onSearchQueryChanged,
            modifier = Modifier.fillMaxWidth(),
            label = { Text(stringResource(R.string.memory_search_query_label)) },
            singleLine = true,
        )
        Spacer(modifier = Modifier.height(8.dp))
        ActionRow(
            actions = listOf(
                valueTypeLabel to onCycleValueType,
                refineModeLabel to onCycleRefineMode,
                regionPresetLabel to onCycleRegionPreset,
            ),
        )
        Spacer(modifier = Modifier.height(8.dp))
        ActionRow(
            actions = listOf(
                stringResource(R.string.memory_action_search) to onRunSearch,
                stringResource(R.string.memory_action_refine) to onRefineSearch,
                stringResource(R.string.memory_action_preview_pc) to onPreviewSelectedPc,
            ),
        )
        Spacer(modifier = Modifier.height(8.dp))
        Row(
            modifier = Modifier.fillMaxWidth(),
            verticalAlignment = Alignment.CenterVertically,
        ) {
            OutlinedTextField(
                value = state.memoryAddressInput,
                onValueChange = onAddressInputChanged,
                modifier = Modifier.weight(1f),
                label = { Text(stringResource(R.string.memory_address_label)) },
                singleLine = true,
            )
            Spacer(modifier = Modifier.width(8.dp))
            OutlinedButton(onClick = onJumpAddress) {
                Text(stringResource(R.string.memory_action_jump))
            }
        }
        if (state.memorySearch.results.isNotEmpty()) {
            Spacer(modifier = Modifier.height(8.dp))
            if (viewMode == MEMORY_VIEW_MODE_RESULTS) {
                ActionRow(
                    actions = listOf(
                        stringResource(R.string.memory_action_show_page) to onShowPage,
                    ),
                )
            } else {
                ActionRow(
                    actions = listOf(
                        stringResource(R.string.memory_action_show_results_count, state.memorySearch.results.size) to onShowResults,
                    ),
                )
            }
        }
    }
}

@Composable
private fun MemoryResultsList(
    state: SessionBridgeState,
    onSelectAddress: (ULong) -> Unit,
) {
    val results = state.memorySearch.results
    if (results.isEmpty()) {
        EmptyState(stringResource(R.string.memory_search_results_empty))
        return
    }
    LazyColumn(modifier = Modifier.fillMaxSize()) {
        items(results, key = { it.address.toString() }) { result ->
            MemoryResultCard(
                result = result,
                selected = state.memoryPage?.focusAddress == result.address,
                onClick = { onSelectAddress(result.address) },
            )
        }
    }
}

@Composable
private fun MemoryResultCard(
    result: MemorySearchResult,
    selected: Boolean,
    onClick: () -> Unit,
) {
    val unnamedRangeLabel = stringResource(R.string.memory_ranges_unnamed)

    Surface(
        modifier = Modifier
            .fillMaxWidth()
            .padding(8.dp)
            .clickable(onClick = onClick),
        color = if (selected) MaterialTheme.colorScheme.primaryContainer else MaterialTheme.colorScheme.surfaceVariant,
        tonalElevation = 1.dp,
    ) {
        Column(modifier = Modifier.padding(12.dp)) {
            Text(
                text = "${hex64(result.address)} · ${result.valueSummary}",
                style = MaterialTheme.typography.bodyMedium,
                color = if (selected) {
                    MaterialTheme.colorScheme.onPrimaryContainer
                } else {
                    MaterialTheme.colorScheme.onSurfaceVariant
                },
            )
            Spacer(modifier = Modifier.height(4.dp))
            Text(
                text = buildString {
                    append(result.regionName.ifBlank { unnamedRangeLabel })
                    append(" · ")
                    append(hex64(result.regionStart))
                    append(" - ")
                    append(hex64(result.regionEnd))
                },
                style = MaterialTheme.typography.bodySmall,
                color = if (selected) {
                    MaterialTheme.colorScheme.onPrimaryContainer
                } else {
                    MaterialTheme.colorScheme.onSurfaceVariant
                },
            )
            if (result.previewHex.isNotBlank()) {
                Spacer(modifier = Modifier.height(4.dp))
                Text(
                    text = result.previewHex.take(32),
                    style = MaterialTheme.typography.labelSmall,
                    color = if (selected) {
                        MaterialTheme.colorScheme.onPrimaryContainer
                    } else {
                        MaterialTheme.colorScheme.onSurfaceVariant
                    },
                )
            }
        }
    }
}

@Composable
private fun MemoryPageList(
    page: MemoryPage?,
    onSelectAddress: (ULong) -> Unit,
    showDisassembly: Boolean,
) {
    if (page == null) {
        EmptyState(stringResource(R.string.memory_error_no_page))
        return
    }
    val regionSummary = page.region?.let { region ->
        "${region.name}  ${hex64(region.startAddr)} - ${hex64(region.endAddr)}  prot=${region.prot.toString(16)}"
    } ?: stringResource(R.string.memory_page_region_none)

    Column(modifier = Modifier.fillMaxSize()) {
        Column(modifier = Modifier.padding(12.dp)) {
            Text(
                text = stringResource(R.string.memory_page_focus, hex64(page.focusAddress)),
                style = MaterialTheme.typography.bodyMedium,
                color = MaterialTheme.colorScheme.onSurface,
            )
            Text(
                text = stringResource(R.string.memory_page_window, page.pageSize.toInt(), page.bytes.size),
                style = MaterialTheme.typography.bodySmall,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
            )
            Text(
                text = regionSummary,
                style = MaterialTheme.typography.bodySmall,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
            )
            if (page.scalars.isNotEmpty()) {
                Spacer(modifier = Modifier.height(8.dp))
                Text(
                    text = stringResource(R.string.memory_page_scalars_title),
                    style = MaterialTheme.typography.labelLarge,
                    color = MaterialTheme.colorScheme.onSurface,
                )
                page.scalars.take(4).forEach { scalar ->
                    Text(
                        text = "${scalar.label}: ${scalar.value}",
                        style = MaterialTheme.typography.bodySmall,
                        color = MaterialTheme.colorScheme.onSurfaceVariant,
                    )
                }
            }
            if (showDisassembly && page.disassembly.isNotEmpty()) {
                Spacer(modifier = Modifier.height(8.dp))
                Text(
                    text = stringResource(R.string.memory_preview_disasm_title),
                    style = MaterialTheme.typography.labelLarge,
                    color = MaterialTheme.colorScheme.onSurface,
                )
                page.disassembly.take(8).forEach { line ->
                    Text(
                        text = line,
                        style = MaterialTheme.typography.bodySmall,
                        color = MaterialTheme.colorScheme.onSurfaceVariant,
                    )
                }
            }
        }
        HorizontalDivider()
        LazyColumn(modifier = Modifier.fillMaxSize()) {
            items(page.rows, key = { it.address.toString() }) { row ->
                MemoryPageRowCard(
                    row = row,
                    selected = rowContainsAddress(row, page.focusAddress),
                    onClick = { onSelectAddress(row.address) },
                )
            }
        }
    }
}

@Composable
private fun MemoryPageRowCard(
    row: MemoryPreviewRow,
    selected: Boolean,
    onClick: () -> Unit,
) {
    Surface(
        modifier = Modifier
            .fillMaxWidth()
            .padding(horizontal = 8.dp, vertical = 4.dp)
            .clickable(onClick = onClick),
        color = if (selected) MaterialTheme.colorScheme.primaryContainer else MaterialTheme.colorScheme.surfaceVariant,
        tonalElevation = 1.dp,
    ) {
        Column(modifier = Modifier.padding(12.dp)) {
            Text(
                text = "${hex64(row.address)} · ${row.hexBytes}",
                style = MaterialTheme.typography.bodyMedium,
                color = if (selected) {
                    MaterialTheme.colorScheme.onPrimaryContainer
                } else {
                    MaterialTheme.colorScheme.onSurfaceVariant
                },
            )
            Spacer(modifier = Modifier.height(4.dp))
            Text(
                text = row.ascii,
                style = MaterialTheme.typography.bodySmall,
                color = if (selected) {
                    MaterialTheme.colorScheme.onPrimaryContainer
                } else {
                    MaterialTheme.colorScheme.onSurfaceVariant
                },
            )
        }
    }
}

@Composable
private fun ThreadListPanel(
    state: SessionBridgeState,
    selectedThread: BridgeThreadRecord?,
    parkedCount: Int,
    onSelectThread: (Int) -> Unit,
    modifier: Modifier = Modifier,
) {
    Surface(
        modifier = modifier,
        tonalElevation = 2.dp,
        color = MaterialTheme.colorScheme.surface,
    ) {
        Column(
            modifier = Modifier
                .fillMaxSize()
                .padding(8.dp),
        ) {
            SectionCard(title = stringResource(R.string.thread_selection_title)) {
                SummaryLine(
                    stringResource(
                        R.string.thread_summary_counts,
                        state.threads.size,
                        parkedCount,
                        state.selectedThreadTid?.toString() ?: "-",
                    ),
                )
                selectedThread?.let { thread ->
                    val flagLabels = threadFlagText(thread.flags)
                    SummaryLine(
                        stringResource(
                            R.string.thread_selection_summary,
                            thread.comm,
                            thread.tgid,
                            stringResource(threadPrimaryStateLabelRes(thread.flags)),
                        ),
                    )
                    SummaryLine(
                        stringResource(
                            R.string.workspace_thread_summary,
                            thread.tid,
                            hex64(thread.userPc),
                            hex64(thread.userSp),
                        ),
                    )
                    if (flagLabels.isNotBlank()) {
                        SummaryLine(stringResource(R.string.thread_flags_value, flagLabels))
                    }
                }
            }
            Spacer(modifier = Modifier.height(8.dp))
            if (state.threads.isEmpty()) {
                EmptyState(stringResource(R.string.thread_empty))
            } else {
                LazyColumn(
                    modifier = Modifier.fillMaxSize(),
                    verticalArrangement = Arrangement.spacedBy(8.dp),
                ) {
                    items(state.threads, key = { it.tid }) { thread ->
                        ThreadRow(
                            thread = thread,
                            selected = state.selectedThreadTid == thread.tid,
                            onClick = { onSelectThread(thread.tid) },
                        )
                    }
                }
            }
        }
    }
}

@Composable
private fun ThreadRow(
    thread: BridgeThreadRecord,
    selected: Boolean,
    onClick: () -> Unit,
) {
    val stateLabel = stringResource(threadPrimaryStateLabelRes(thread.flags))
    val flagLabels = threadFlagText(thread.flags)
    Surface(
        modifier = Modifier
            .fillMaxWidth()
            .clickable(onClick = onClick),
        color = if (selected) MaterialTheme.colorScheme.primaryContainer else MaterialTheme.colorScheme.surfaceVariant,
    ) {
        Column(modifier = Modifier.padding(12.dp)) {
            Text(
                text = stringResource(
                    R.string.workspace_thread_summary,
                    thread.tid,
                    hex64(thread.userPc),
                    hex64(thread.userSp),
                ),
                color = if (selected) MaterialTheme.colorScheme.onPrimaryContainer else MaterialTheme.colorScheme.onSurfaceVariant,
                fontWeight = FontWeight.SemiBold,
            )
            Text(
                text = stringResource(
                    R.string.thread_selection_summary,
                    thread.comm,
                    thread.tgid,
                    stateLabel,
                ),
                style = MaterialTheme.typography.bodySmall,
                color = if (selected) MaterialTheme.colorScheme.onPrimaryContainer else MaterialTheme.colorScheme.onSurfaceVariant,
            )
            if (flagLabels.isNotEmpty()) {
                Spacer(modifier = Modifier.height(4.dp))
                Text(
                    text = flagLabels,
                    style = MaterialTheme.typography.labelSmall,
                    color = if (selected) MaterialTheme.colorScheme.onPrimaryContainer else MaterialTheme.colorScheme.onSurfaceVariant,
                )
            }
        }
    }
}

@Composable
private fun ThreadRegistersPanel(
    selectedTid: Int?,
    registers: BridgeThreadRegistersReply?,
    modifier: Modifier = Modifier,
) {
    Surface(
        modifier = modifier,
        tonalElevation = 2.dp,
        color = MaterialTheme.colorScheme.surface,
    ) {
        if (selectedTid == null || registers == null) {
            EmptyState(stringResource(R.string.thread_empty))
            return@Surface
        }
        val registerGroups = buildRegisterGroups(registers)
        LazyColumn(
            modifier = Modifier
                .fillMaxSize()
                .padding(8.dp),
            verticalArrangement = Arrangement.spacedBy(8.dp),
        ) {
            item {
                SectionCard(title = stringResource(R.string.thread_regs_title, selectedTid)) {
                    SummaryLine(stringResource(R.string.thread_reg_x0, hex64(registers.regs[0])))
                    SummaryLine(stringResource(R.string.thread_reg_x1, hex64(registers.regs[1])))
                    SummaryLine(stringResource(R.string.thread_reg_x29, hex64(registers.regs[29])))
                    SummaryLine(stringResource(R.string.thread_reg_x30, hex64(registers.regs[30])))
                    SummaryLine(stringResource(R.string.thread_sp_value, hex64(registers.sp)))
                    SummaryLine(stringResource(R.string.thread_pc_value, hex64(registers.pc)))
                    SummaryLine(stringResource(R.string.thread_pstate_value, hex64(registers.pstate)))
                    SummaryLine(stringResource(R.string.thread_register_flags, hex32(registers.flags)))
                    SummaryLine(
                        stringResource(
                            R.string.thread_fp_summary,
                            hex32(registers.features),
                            hex32(registers.fpsr),
                            hex32(registers.fpcr),
                            hex64(registers.v0Lo),
                            hex64(registers.v0Hi),
                        ),
                    )
                }
            }
            items(registerGroups, key = { it.titleRes }) { group ->
                ThreadRegisterGroupCard(group)
            }
        }
    }
}

@Composable
private fun ThreadRegisterGroupCard(group: RegisterGroup) {
    SectionCard(title = stringResource(group.titleRes)) {
        group.fields.chunked(2).forEach { pair ->
            Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                pair.forEach { field ->
                    RegisterFieldChip(
                        field = field,
                        modifier = Modifier.weight(1f),
                    )
                }
                if (pair.size == 1) {
                    Spacer(modifier = Modifier.weight(1f))
                }
            }
        }
    }
}

@Composable
private fun RegisterFieldChip(
    field: RegisterField,
    modifier: Modifier = Modifier,
) {
    Surface(
        modifier = modifier,
        color = MaterialTheme.colorScheme.surfaceVariant,
        tonalElevation = 0.dp,
    ) {
        Column(modifier = Modifier.padding(8.dp)) {
            Text(
                text = field.label,
                style = MaterialTheme.typography.labelSmall,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
            )
            Text(
                text = hex64(field.value),
                style = MaterialTheme.typography.bodySmall,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
            )
        }
    }
}

@Composable
private fun ProcessRow(
    process: ResolvedProcessRecord,
    selected: Boolean,
    attached: Boolean,
    onSelect: () -> Unit,
    onAttach: () -> Unit,
) {
    Surface(
        modifier = Modifier
            .fillMaxWidth()
            .clickable(onClick = onSelect),
        color = if (selected || attached) MaterialTheme.colorScheme.primaryContainer else MaterialTheme.colorScheme.surfaceVariant,
    ) {
        Column(modifier = Modifier.padding(12.dp)) {
            Text(
                text = process.displayName,
                style = MaterialTheme.typography.bodyMedium,
                color = if (selected || attached) MaterialTheme.colorScheme.onPrimaryContainer else MaterialTheme.colorScheme.onSurfaceVariant,
                fontWeight = FontWeight.SemiBold,
            )
            Text(
                text = buildString {
                    append("pid=")
                    append(process.pid)
                    append(" uid=")
                    append(process.uid)
                    append(" · ")
                    append(process.processName)
                },
                style = MaterialTheme.typography.bodySmall,
                color = if (selected || attached) MaterialTheme.colorScheme.onPrimaryContainer else MaterialTheme.colorScheme.onSurfaceVariant,
            )
            val packageName = process.packageName
            if (!packageName.isNullOrBlank()) {
                Text(
                    text = packageName,
                    style = MaterialTheme.typography.bodySmall,
                    color = if (selected || attached) MaterialTheme.colorScheme.onPrimaryContainer else MaterialTheme.colorScheme.onSurfaceVariant,
                )
            }
            Spacer(modifier = Modifier.height(6.dp))
            OutlinedButton(onClick = onAttach, enabled = !attached) {
                Text(
                    stringResource(
                        if (attached) R.string.process_attached else R.string.process_action_attach,
                    ),
                )
            }
        }
    }
}

@Composable
private fun EventRow(
    entry: SessionEventEntry,
    pinned: Boolean,
    onTogglePin: () -> Unit,
    onOpenThread: (Int) -> Unit,
    onOpenValue: (ULong) -> Unit,
) {
    val eventActionLabels = mapOf(
        "thread" to stringResource(R.string.event_action_open_thread),
        "value0" to stringResource(R.string.event_action_open_value0),
        "value1" to stringResource(R.string.event_action_open_value1),
    )
    val containerColor = when {
        pinned -> MaterialTheme.colorScheme.primaryContainer
        eventMatchesPreset(entry, EventFilterPreset.Stops, emptySet()) -> MaterialTheme.colorScheme.errorContainer
        eventMatchesPreset(entry, EventFilterPreset.Syscalls, emptySet()) -> MaterialTheme.colorScheme.secondaryContainer
        eventMatchesPreset(entry, EventFilterPreset.Signals, emptySet()) -> MaterialTheme.colorScheme.tertiaryContainer
        else -> MaterialTheme.colorScheme.surfaceVariant
    }
    val onContainerColor = when {
        pinned -> MaterialTheme.colorScheme.onPrimaryContainer
        eventMatchesPreset(entry, EventFilterPreset.Stops, emptySet()) -> MaterialTheme.colorScheme.onErrorContainer
        eventMatchesPreset(entry, EventFilterPreset.Syscalls, emptySet()) -> MaterialTheme.colorScheme.onSecondaryContainer
        eventMatchesPreset(entry, EventFilterPreset.Signals, emptySet()) -> MaterialTheme.colorScheme.onTertiaryContainer
        else -> MaterialTheme.colorScheme.onSurfaceVariant
    }
    Surface(
        modifier = Modifier.fillMaxWidth(),
        color = containerColor,
    ) {
        Column(modifier = Modifier.padding(12.dp)) {
            Row(
                modifier = Modifier.fillMaxWidth(),
                verticalAlignment = Alignment.CenterVertically,
            ) {
                Column(modifier = Modifier.weight(1f)) {
                    Text(
                        text = "${eventTypeLabel(entry.record.type)} · ${eventCodeLabel(entry.record)}",
                        style = MaterialTheme.typography.bodyMedium,
                        color = onContainerColor,
                        fontWeight = FontWeight.SemiBold,
                    )
                    Text(
                        text = stringResource(
                            R.string.workspace_event_detail_summary,
                            eventCodeLabel(entry.record),
                            entry.record.tid,
                            entry.record.seq.toString(),
                        ),
                        style = MaterialTheme.typography.labelSmall,
                        color = onContainerColor,
                    )
                }
                OutlinedButton(onClick = onTogglePin) {
                    Text(
                        stringResource(
                            if (pinned) R.string.event_action_unpin else R.string.event_action_pin,
                        ),
                    )
                }
            }
            Spacer(modifier = Modifier.height(6.dp))
            Text(
                text = buildString {
                    append("sid=")
                    append(hex64(entry.record.sessionId))
                    append(" · tgid=")
                    append(entry.record.tgid)
                    append(" · flags=")
                    append(hex32(entry.record.flags))
                },
                style = MaterialTheme.typography.bodySmall,
                color = onContainerColor,
            )
            Text(
                text = "value0=${hex64(entry.record.value0)} · value1=${hex64(entry.record.value1)}",
                style = MaterialTheme.typography.bodySmall,
                color = onContainerColor,
            )
            Text(
                text = stringResource(R.string.event_received_template, entry.receivedAtMs.toString()),
                style = MaterialTheme.typography.labelSmall,
                color = onContainerColor,
            )
            val eventActions = buildList<Pair<String, () -> Unit>> {
                if (entry.record.tid > 0) {
                    add(eventActionLabels.getValue("thread") to { onOpenThread(entry.record.tid) })
                }
                if (entry.record.value0 != 0uL) {
                    add(eventActionLabels.getValue("value0") to { onOpenValue(entry.record.value0) })
                }
                if (entry.record.value1 != 0uL && entry.record.value1 != entry.record.value0) {
                    add(eventActionLabels.getValue("value1") to { onOpenValue(entry.record.value1) })
                }
            }
            if (eventActions.isNotEmpty()) {
                Spacer(modifier = Modifier.height(8.dp))
                CompactActionRow(actions = eventActions)
            }
        }
    }
}

@Composable
private fun HwpointList(
    hwpoints: List<BridgeHwpointRecord>,
    selectedId: ULong?,
    onSelect: (ULong) -> Unit,
) {
    if (hwpoints.isEmpty()) {
        Text(
            text = stringResource(R.string.hwpoint_empty),
            color = MaterialTheme.colorScheme.onSurfaceVariant,
        )
        return
    }
    Column(verticalArrangement = Arrangement.spacedBy(8.dp)) {
        hwpoints.forEach { hwpoint ->
            val selected = selectedId == hwpoint.id
            Surface(
                modifier = Modifier
                    .fillMaxWidth()
                    .clickable { onSelect(hwpoint.id) },
                color = if (selected) MaterialTheme.colorScheme.primaryContainer else MaterialTheme.colorScheme.surfaceVariant,
            ) {
                Column(modifier = Modifier.padding(12.dp)) {
                    Text(
                        text = "${hex64(hwpoint.addr)} · ${hwpointTypeLabel(hwpoint)}",
                        color = if (selected) MaterialTheme.colorScheme.onPrimaryContainer else MaterialTheme.colorScheme.onSurfaceVariant,
                        fontWeight = FontWeight.SemiBold,
                    )
                    Text(
                        text = stringResource(
                            R.string.hwpoint_item_detail,
                            hwpoint.id.toString(),
                            hwpoint.tid,
                            hwpoint.len.toInt(),
                            hwpoint.hits.toString(),
                            hex32(hwpoint.state),
                        ),
                        style = MaterialTheme.typography.bodySmall,
                        color = if (selected) MaterialTheme.colorScheme.onPrimaryContainer else MaterialTheme.colorScheme.onSurfaceVariant,
                    )
                }
            }
        }
    }
}

@Composable
private fun ActionRow(
    actions: List<Pair<String, () -> Unit>>,
    enabled: Boolean = true,
) {
    Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
        actions.forEach { (label, action) ->
            OutlinedButton(onClick = action, enabled = enabled, modifier = Modifier.weight(1f)) {
                Text(label)
            }
        }
    }
}

@Composable
private fun <T> FilterRow(
    filters: List<T>,
    selectedFilter: T,
    labelFor: @Composable (T) -> String,
    onSelected: (T) -> Unit,
) {
    Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
        filters.forEach { filter ->
            OutlinedButton(
                onClick = { onSelected(filter) },
                modifier = Modifier.weight(1f),
            ) {
                Text(
                    text = labelFor(filter),
                    color = if (filter == selectedFilter) MaterialTheme.colorScheme.primary else MaterialTheme.colorScheme.onSurfaceVariant,
                )
            }
        }
    }
}

@Composable
private fun <T> CompactFilterRow(
    filters: List<T>,
    selectedFilter: T,
    labelFor: @Composable (T) -> String,
    onSelected: (T) -> Unit,
) {
    LazyRow(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
        items(filters) { filter ->
            OutlinedButton(onClick = { onSelected(filter) }) {
                Text(
                    text = labelFor(filter),
                    color = if (filter == selectedFilter) MaterialTheme.colorScheme.primary else MaterialTheme.colorScheme.onSurfaceVariant,
                )
            }
        }
    }
}

@Composable
private fun CompactActionRow(
    actions: List<Pair<String, () -> Unit>>,
    enabled: Boolean = true,
) {
    LazyRow(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
        items(actions) { (label, action) ->
            OutlinedButton(onClick = action, enabled = enabled) {
                Text(label)
            }
        }
    }
}

@Composable
private fun ActionDialog(
    title: String,
    subtitle: String,
    onDismiss: () -> Unit,
    content: @Composable ColumnScope.() -> Unit,
) {
    OverlayModalContainer(onDismiss = onDismiss) {
        Surface(
            modifier = Modifier.fillMaxWidth(),
            color = MaterialTheme.colorScheme.surface,
            tonalElevation = 4.dp,
        ) {
            Column(
                modifier = Modifier
                    .fillMaxWidth()
                    .padding(12.dp),
                verticalArrangement = Arrangement.spacedBy(8.dp),
            ) {
                Row(
                    modifier = Modifier.fillMaxWidth(),
                    verticalAlignment = Alignment.CenterVertically,
                ) {
                    SectionIntro(
                        title = title,
                        subtitle = subtitle,
                        modifier = Modifier.weight(1f),
                    )
                    OutlinedButton(onClick = onDismiss) {
                        Text(stringResource(R.string.memory_ranges_close))
                    }
                }
                content()
            }
        }
    }
}

@Composable
private fun CompactBadgeRow(
    badges: List<StatusBadgeSpec>,
) {
    LazyRow(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
        items(badges) { badge ->
            Surface(
                color = badge.containerColor,
                tonalElevation = 0.dp,
            ) {
                Text(
                    text = badge.label,
                    color = badge.contentColor,
                    style = MaterialTheme.typography.labelSmall,
                    modifier = Modifier.padding(horizontal = 8.dp, vertical = 4.dp),
                )
            }
        }
    }
}

@Composable
private fun MetricTileGrid(
    metrics: List<MetricTileSpec>,
) {
    Column(verticalArrangement = Arrangement.spacedBy(8.dp)) {
        metrics.chunked(2).forEach { rowMetrics ->
            Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                rowMetrics.forEach { metric ->
                    MetricTile(
                        metric = metric,
                        modifier = Modifier.weight(1f),
                    )
                }
                if (rowMetrics.size == 1) {
                    Spacer(modifier = Modifier.weight(1f))
                }
            }
        }
    }
}

@Composable
private fun MetricTile(
    metric: MetricTileSpec,
    modifier: Modifier = Modifier,
) {
    Surface(
        modifier = modifier,
        color = MaterialTheme.colorScheme.surfaceVariant,
        tonalElevation = 0.dp,
    ) {
        Column(
            modifier = Modifier.padding(8.dp),
            verticalArrangement = Arrangement.spacedBy(2.dp),
        ) {
            Text(
                text = metric.label,
                style = MaterialTheme.typography.labelSmall,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
            )
            Text(
                text = metric.value,
                style = MaterialTheme.typography.bodySmall,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
                fontWeight = FontWeight.SemiBold,
            )
        }
    }
}

@Composable
private fun SectionMetricBanner(
    label: String,
    value: String,
) {
    Surface(
        color = MaterialTheme.colorScheme.surfaceVariant,
        tonalElevation = 0.dp,
    ) {
        Column(
            modifier = Modifier
                .fillMaxWidth()
                .padding(8.dp),
            verticalArrangement = Arrangement.spacedBy(2.dp),
        ) {
            Text(
                text = label,
                style = MaterialTheme.typography.labelSmall,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
            )
            Text(
                text = value,
                style = MaterialTheme.typography.bodySmall,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
            )
        }
    }
}

@Composable
private fun SectionCard(
    title: String,
    content: @Composable ColumnScope.() -> Unit,
) {
    Surface(
        tonalElevation = 2.dp,
        color = MaterialTheme.colorScheme.surface,
    ) {
        Column(
            modifier = Modifier
                .fillMaxWidth()
                .padding(12.dp),
            verticalArrangement = Arrangement.spacedBy(4.dp),
            content = {
                Text(
                    text = title,
                    style = MaterialTheme.typography.titleSmall,
                    color = MaterialTheme.colorScheme.onSurface,
                )
                Spacer(modifier = Modifier.height(4.dp))
                content()
            },
        )
    }
}

@Composable
private fun SectionIntro(
    title: String,
    subtitle: String,
    modifier: Modifier = Modifier,
) {
    Column(
        modifier = modifier,
        verticalArrangement = Arrangement.spacedBy(2.dp),
    ) {
        Text(
            text = title,
            style = MaterialTheme.typography.titleMedium,
            color = MaterialTheme.colorScheme.onBackground,
        )
        Text(
            text = subtitle,
            style = MaterialTheme.typography.bodySmall,
            color = MaterialTheme.colorScheme.onSurfaceVariant,
        )
    }
}

@Composable
private fun SummaryLine(text: String) {
    Text(
        text = text,
        style = MaterialTheme.typography.bodySmall,
        color = MaterialTheme.colorScheme.onSurfaceVariant,
    )
}

@Composable
private fun EmptyState(text: String) {
    Box(
        modifier = Modifier.fillMaxSize(),
        contentAlignment = Alignment.Center,
    ) {
        Text(
            text = text,
            color = MaterialTheme.colorScheme.onSurfaceVariant,
        )
    }
}

@Composable
private fun threadFlagText(flags: UInt): String = listOfNotNull(
    stringResource(R.string.thread_flag_group_leader).takeIf { (flags and THREAD_FLAG_GROUP_LEADER) != 0u },
    stringResource(R.string.thread_flag_session_target).takeIf { (flags and THREAD_FLAG_SESSION_TARGET) != 0u },
    stringResource(R.string.thread_flag_freeze_tracked).takeIf { (flags and THREAD_FLAG_FREEZE_TRACKED) != 0u },
    stringResource(R.string.thread_flag_freeze_settled).takeIf { (flags and THREAD_FLAG_FREEZE_SETTLED) != 0u },
    stringResource(R.string.thread_flag_freeze_parked).takeIf { (flags and THREAD_FLAG_FREEZE_PARKED) != 0u },
    stringResource(R.string.thread_flag_exiting).takeIf { (flags and THREAD_FLAG_EXITING) != 0u },
).joinToString(" · ")

private fun rowContainsAddress(
    row: MemoryPreviewRow,
    address: ULong,
): Boolean {
    val rowEndExclusive = row.address + row.byteValues.size.toUInt().toULong()
    return address >= row.address && address < rowEndExclusive
}

private fun hex64(value: ULong): String = "0x${value.toString(16)}"

private fun hex32(value: UInt): String = "0x${value.toString(16)}"

private fun workspaceSectionLabelRes(section: WorkspaceSection): Int =
    when (section) {
        WorkspaceSection.Session -> R.string.workspace_session
        WorkspaceSection.Processes -> R.string.workspace_processes
        WorkspaceSection.Memory -> R.string.workspace_memory
        WorkspaceSection.Threads -> R.string.workspace_threads
        WorkspaceSection.Events -> R.string.workspace_events
    }

private fun workspaceSectionIconRes(section: WorkspaceSection): Int =
    when (section) {
        WorkspaceSection.Session -> R.drawable.ic_lkmdbg_terminal
        WorkspaceSection.Processes -> R.drawable.ic_lkmdbg_radar
        WorkspaceSection.Memory -> R.drawable.ic_lkmdbg_cpu
        WorkspaceSection.Threads -> R.drawable.ic_lkmdbg_cpu
        WorkspaceSection.Events -> R.drawable.ic_lkmdbg_terminal
    }

private fun hwpointPresetLabelRes(preset: HwpointPreset): Int =
    when (preset) {
        HwpointPreset.ExecHardware -> R.string.hwpoint_preset_exec_hw
        HwpointPreset.ExecMmu -> R.string.hwpoint_preset_exec_mmu
        HwpointPreset.WriteHardware -> R.string.hwpoint_preset_write_hw
        HwpointPreset.WriteMmu -> R.string.hwpoint_preset_write_mmu
    }

private fun memorySearchValueTypeLabelRes(valueType: MemorySearchValueType): Int =
    when (valueType) {
        MemorySearchValueType.Int32 -> R.string.memory_search_type_int32
        MemorySearchValueType.Int64 -> R.string.memory_search_type_int64
        MemorySearchValueType.Float32 -> R.string.memory_search_type_float32
        MemorySearchValueType.Float64 -> R.string.memory_search_type_float64
        MemorySearchValueType.HexBytes -> R.string.memory_search_type_hex
        MemorySearchValueType.Ascii -> R.string.memory_search_type_ascii
    }

private fun memorySearchRefineModeLabelRes(mode: MemorySearchRefineMode): Int =
    when (mode) {
        MemorySearchRefineMode.Exact -> R.string.memory_refine_mode_exact
        MemorySearchRefineMode.Changed -> R.string.memory_refine_mode_changed
        MemorySearchRefineMode.Unchanged -> R.string.memory_refine_mode_unchanged
        MemorySearchRefineMode.Increased -> R.string.memory_refine_mode_increased
        MemorySearchRefineMode.Decreased -> R.string.memory_refine_mode_decreased
    }

private fun memoryRegionPresetLabelRes(preset: MemoryRegionPreset): Int =
    when (preset) {
        MemoryRegionPreset.All -> R.string.memory_region_all
        MemoryRegionPreset.Xa -> R.string.memory_region_xa
        MemoryRegionPreset.Cd -> R.string.memory_region_cd
        MemoryRegionPreset.Ca -> R.string.memory_region_ca
        MemoryRegionPreset.Ch -> R.string.memory_region_ch
        MemoryRegionPreset.Stack -> R.string.memory_region_stack
    }
