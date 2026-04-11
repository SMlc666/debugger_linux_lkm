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
import androidx.compose.material3.Button
import androidx.compose.material3.HorizontalDivider
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.Surface
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
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
import com.smlc666.lkmdbg.R
import com.smlc666.lkmdbg.data.MemoryPage
import com.smlc666.lkmdbg.data.MemoryPreviewRow
import com.smlc666.lkmdbg.data.MemorySearchResult
import com.smlc666.lkmdbg.data.ProcessFilter
import com.smlc666.lkmdbg.data.ResolvedProcessRecord
import com.smlc666.lkmdbg.data.SessionBridgeState
import com.smlc666.lkmdbg.data.SessionEventEntry
import com.smlc666.lkmdbg.data.WorkspaceSection
import com.smlc666.lkmdbg.shared.BridgeHwpointRecord
import com.smlc666.lkmdbg.shared.BridgeStopState
import com.smlc666.lkmdbg.shared.BridgeThreadRegistersReply
import com.smlc666.lkmdbg.shared.BridgeThreadRecord

private const val MEMORY_VIEW_MODE_PAGE = 0
private const val MEMORY_VIEW_MODE_RESULTS = 1

@Composable
fun WorkingBar(
    state: SessionBridgeState,
    memoryToolsOpen: Boolean,
    onToggleMemoryTools: () -> Unit,
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
            val statusText = buildString {
                append("[")
                append(state.snapshot.targetPid)
                append("] ")
                append(processName.ifBlank { emptyTargetLabel })
                if (state.memoryPage != null) {
                    append(" · ")
                    append(hex64(state.memoryPage.focusAddress))
                }
                if (memoryToolsOpen)
                    append(" · tools")
            }
            Text(
                text = statusText,
                style = MaterialTheme.typography.bodySmall,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
                modifier = Modifier.weight(1f),
                maxLines = 1,
            )

            IconButton(
                onClick = onToggleMemoryTools,
                modifier = Modifier
                    .width(36.dp)
                    .height(36.dp),
            ) {
                Icon(
                    painter = painterResource(id = R.drawable.ic_lkmdbg_radar),
                    contentDescription = stringResource(R.string.memory_action_tools),
                    modifier = Modifier
                        .fillMaxSize()
                        .padding(4.dp),
                    tint = MaterialTheme.colorScheme.primary,
                )
            }
        }
    }
}

@Composable
fun MainWorkspaceScreen(
    state: SessionBridgeState,
    memoryViewMode: Int,
    memoryToolsOpen: Boolean,
    onSectionSelected: (WorkspaceSection) -> Unit,
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
    val configuration = androidx.compose.ui.platform.LocalConfiguration.current
    val isLandscape = configuration.orientation == android.content.res.Configuration.ORIENTATION_LANDSCAPE
    val sections = listOf(
        WorkspaceSection.Session,
        WorkspaceSection.Processes,
        WorkspaceSection.Memory,
        WorkspaceSection.Threads,
        WorkspaceSection.Events,
    )

    Box(
        modifier = Modifier
            .fillMaxSize()
            .background(MaterialTheme.colorScheme.background),
    ) {
        if (isLandscape) {
            Row(modifier = Modifier.fillMaxSize()) {
                CategoryColumn(
                    sections = sections,
                    selectedSection = state.workspaceSection,
                    onSectionSelected = onSectionSelected,
                    modifier = Modifier
                        .width(80.dp)
                        .fillMaxHeight(),
                )

                WorkspaceColumn(
                    state = state,
                    memoryViewMode = memoryViewMode,
                    memoryToolsOpen = memoryToolsOpen,
                    onToggleProcessPicker = onToggleProcessPicker,
                    onToggleMemoryTools = onToggleMemoryTools,
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
                    onClose = onClose,
                    onCollapse = onCollapse,
                )
            }
        } else {
            Column(modifier = Modifier.fillMaxSize()) {
                Row(
                    modifier = Modifier.fillMaxWidth(),
                    verticalAlignment = Alignment.CenterVertically,
                ) {
                    CategoryRow(
                        sections = sections,
                        selectedSection = state.workspaceSection,
                        onSectionSelected = onSectionSelected,
                        modifier = Modifier.weight(1f),
                    )
                    Spacer(modifier = Modifier.width(48.dp))
                }
                WorkspaceColumn(
                    state = state,
                    memoryViewMode = memoryViewMode,
                    memoryToolsOpen = memoryToolsOpen,
                    onToggleProcessPicker = onToggleProcessPicker,
                    onToggleMemoryTools = onToggleMemoryTools,
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
                    onClose = onClose,
                    onCollapse = onCollapse,
                )
            }
        }
    }
}

@Composable
private fun WorkspaceColumn(
    state: SessionBridgeState,
    memoryViewMode: Int,
    memoryToolsOpen: Boolean,
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
        Text(
            text = stringResource(state.workspaceSection.labelRes),
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
                    painter = painterResource(id = section.iconRes),
                    contentDescription = stringResource(id = section.labelRes),
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
                    painter = painterResource(id = section.iconRes),
                    contentDescription = stringResource(id = section.labelRes),
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
                        stringResource(R.string.session_action_connect) to onConnect,
                        stringResource(R.string.session_action_open_session) to onOpenSession,
                        stringResource(R.string.session_action_refresh) to onRefreshSession,
                    ),
                    enabled = !controlsBusy,
                )
                Spacer(modifier = Modifier.height(8.dp))
                ActionRow(
                    listOf(
                        stringResource(R.string.session_action_attach_target) to onAttachTarget,
                        stringResource(R.string.session_action_refresh_stop) to onRefreshStopState,
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
                        stringResource(R.string.session_action_freeze) to onFreezeThreads,
                        stringResource(R.string.session_action_thaw) to onThawThreads,
                    ),
                    enabled = !controlsBusy,
                )
                Spacer(modifier = Modifier.height(8.dp))
                ActionRow(
                    listOf(
                        stringResource(R.string.session_action_continue) to onContinueTarget,
                        stringResource(R.string.session_action_single_step) to onSingleStep,
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
                    listOf(
                        stringResource(state.hwpointPreset.labelRes) to onCycleHwpointPreset,
                        stringResource(R.string.hwpoint_action_use_pc) to onUseSelectedPcForHwpoint,
                        stringResource(R.string.hwpoint_action_use_memory) to onUseMemoryFocusForHwpoint,
                    ),
                    enabled = !controlsBusy,
                )
                Spacer(modifier = Modifier.height(8.dp))
                ActionRow(
                    listOf(
                        stringResource(R.string.hwpoint_action_add) to onAddHwpoint,
                        stringResource(R.string.hwpoint_action_remove_selected) to onRemoveSelectedHwpoint,
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
}

@Composable
private fun SessionStatusSummary(state: SessionBridgeState) {
    Column(verticalArrangement = Arrangement.spacedBy(4.dp)) {
        SummaryLine(stringResource(R.string.session_flag_connected, state.snapshot.connected))
        SummaryLine(stringResource(R.string.session_flag_open, state.snapshot.sessionOpen))
        SummaryLine(stringResource(R.string.session_flag_hook_active, state.snapshot.hookActive))
        SummaryLine(stringResource(R.string.session_flag_owner_pid, state.snapshot.ownerPid))
        SummaryLine(stringResource(R.string.session_flag_event_queue, state.snapshot.eventQueueDepth.toString()))
        SummaryLine(stringResource(R.string.session_summary_transport, state.snapshot.transport))
        SummaryLine(stringResource(R.string.session_summary_session_id, hex64(state.snapshot.sessionId)))
        SummaryLine(stringResource(R.string.session_summary_target, state.snapshot.targetPid, state.snapshot.targetTid))
        SummaryLine(stringResource(R.string.session_last_message, state.lastMessage))
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
    Column(verticalArrangement = Arrangement.spacedBy(4.dp)) {
        SummaryLine(stringResource(R.string.session_stop_reason, stopReasonLabel(stop.reason)))
        SummaryLine(stringResource(R.string.session_stop_cookie, hex64(stop.cookie)))
        SummaryLine(stringResource(R.string.session_stop_tid, stop.tgid, stop.tid))
        SummaryLine(stringResource(R.string.session_stop_flags, stopFlagsLabel(stop.flags)))
        SummaryLine(stringResource(R.string.session_stop_event_flags, hex32(stop.eventFlags)))
        SummaryLine(stringResource(R.string.session_stop_values, hex64(stop.value0), hex64(stop.value1)))
        SummaryLine(stringResource(R.string.workspace_thread_register_summary, hex64(stop.x0), hex64(stop.x1), hex64(stop.pc)))
        SummaryLine(stringResource(R.string.thread_reg_x29, hex64(stop.x29)))
        SummaryLine(stringResource(R.string.thread_reg_x30, hex64(stop.x30)))
        SummaryLine(stringResource(R.string.thread_sp_value, hex64(stop.sp)))
        SummaryLine(stringResource(R.string.thread_pstate_value, hex64(stop.pstate)))
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
    Column(modifier = Modifier.fillMaxSize()) {
        SectionIntro(
            title = stringResource(R.string.process_panel_title),
            subtitle = stringResource(R.string.process_panel_subtitle),
        )
        Spacer(modifier = Modifier.height(8.dp))
        Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
            OutlinedButton(onClick = onRefreshProcesses) {
                Text(stringResource(R.string.process_action_refresh))
            }
            Text(
                text = stringResource(
                    R.string.session_quick_attach_summary,
                    state.processes.size,
                    filtered.size,
                ),
                style = MaterialTheme.typography.bodySmall,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
                modifier = Modifier.align(Alignment.CenterVertically),
            )
        }
        Spacer(modifier = Modifier.height(8.dp))
        FilterRow(
            filters = ProcessFilter.entries.toList(),
            selectedFilter = state.processFilter,
            labelFor = { processFilterLabel(it) },
            onSelected = onProcessFilterSelected,
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
    Column(modifier = Modifier.fillMaxSize()) {
        SectionIntro(
            title = stringResource(R.string.thread_panel_title),
            subtitle = stringResource(R.string.thread_panel_subtitle),
        )
        Spacer(modifier = Modifier.height(8.dp))
        ActionRow(
            listOf(
                stringResource(R.string.thread_action_refresh) to onRefreshThreads,
                stringResource(R.string.thread_action_refresh_registers) to onRefreshSelectedThreadRegisters,
            ),
        )
        Spacer(modifier = Modifier.height(8.dp))
        ActionRow(
            listOf(
                stringResource(R.string.memory_action_preview_pc) to onPreviewSelectedPc,
                stringResource(R.string.session_action_single_step) to onSingleStep,
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
}

@Composable
private fun EventsSectionContent(
    state: SessionBridgeState,
    onRefreshEvents: () -> Unit,
    onToggleEventsAutoPoll: () -> Unit,
    onClearEvents: () -> Unit,
    onTogglePinnedEvent: (ULong) -> Unit,
) {
    var filterText by rememberSaveable { mutableStateOf("") }
    var presetOrdinal by rememberSaveable { mutableStateOf(EventFilterPreset.All.ordinal) }
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
                )
            }
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
    val valueTypeLabel = stringResource(state.memorySearch.valueType.labelRes)
    val refineModeLabel = stringResource(state.memorySearch.refineMode.labelRes)
    val regionPresetLabel = stringResource(state.memorySearch.regionPreset.labelRes)
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
                    append(" · vmas=")
                    append(state.vmas.size)
                    append(" · images=")
                    append(state.images.size)
                    if (memoryToolsOpen)
                        append(" · $toolsLabel")
                },
                modifier = Modifier.weight(1f),
            )
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
        OutlinedTextField(
            value = state.memorySearch.query,
            onValueChange = onSearchQueryChanged,
            modifier = Modifier.fillMaxWidth(),
            label = { Text(stringResource(R.string.memory_search_query_label)) },
            singleLine = true,
        )
        Spacer(modifier = Modifier.height(8.dp))
        Row(modifier = Modifier.fillMaxWidth()) {
            OutlinedButton(onClick = onCycleValueType, modifier = Modifier.weight(1f)) {
                Text(valueTypeLabel)
            }
            Spacer(modifier = Modifier.width(8.dp))
            OutlinedButton(onClick = onCycleRefineMode, modifier = Modifier.weight(1f)) {
                Text(refineModeLabel)
            }
            Spacer(modifier = Modifier.width(8.dp))
            OutlinedButton(onClick = onCycleRegionPreset, modifier = Modifier.weight(1f)) {
                Text(regionPresetLabel)
            }
        }
        Spacer(modifier = Modifier.height(8.dp))
        Row(modifier = Modifier.fillMaxWidth()) {
            Button(onClick = onRunSearch, modifier = Modifier.weight(1f)) {
                Text(stringResource(R.string.memory_action_search))
            }
            Spacer(modifier = Modifier.width(8.dp))
            OutlinedButton(onClick = onRefineSearch, modifier = Modifier.weight(1f)) {
                Text(stringResource(R.string.memory_action_refine))
            }
            Spacer(modifier = Modifier.width(8.dp))
            OutlinedButton(onClick = onPreviewSelectedPc, modifier = Modifier.weight(1f)) {
                Text(stringResource(R.string.memory_action_preview_pc))
            }
        }
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
        Spacer(modifier = Modifier.height(8.dp))
        ActionRow(
            listOf(
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
            if (!process.packageName.isNullOrBlank()) {
                Text(
                    text = process.packageName,
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
) {
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
    labelFor: (T) -> String,
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
    labelFor: (T) -> String,
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

private fun processFilterLabel(filter: ProcessFilter): String = when (filter) {
    ProcessFilter.All -> "All"
    ProcessFilter.AndroidApps -> "Android"
    ProcessFilter.CommandLine -> "CLI"
    ProcessFilter.SystemApps -> "System"
    ProcessFilter.UserApps -> "User"
}

private fun rowContainsAddress(
    row: MemoryPreviewRow,
    address: ULong,
): Boolean {
    val rowEndExclusive = row.address + row.byteValues.size.toUInt().toULong()
    return address >= row.address && address < rowEndExclusive
}

private fun hex64(value: ULong): String = "0x${value.toString(16)}"

private fun hex32(value: UInt): String = "0x${value.toString(16)}"
