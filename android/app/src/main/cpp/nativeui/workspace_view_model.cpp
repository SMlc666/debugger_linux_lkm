#include "nativeui/workspace_view_model.h"

#include <string_view>

#include "nativeui/ui_format.h"

namespace lkmdbg::nativeui {

namespace {

using SectionId = WorkspaceSectionId;

std::string BoolText(const WorkspaceLabels &labels, bool value)
{
	return value ? labels.bool_yes : labels.bool_no;
}

std::string_view SectionTitle(const WorkspaceLabels &labels, SectionId section)
{
	switch (section) {
	case SectionId::Session:
		return labels.session;
	case SectionId::Processes:
		return labels.processes;
	case SectionId::Memory:
		return labels.memory;
	case SectionId::Threads:
		return labels.threads;
	case SectionId::Events:
	default:
		return labels.events;
	}
}

std::string_view SectionSubtitle(const WorkspaceLabels &labels, SectionId section)
{
	switch (section) {
	case SectionId::Session:
		return labels.session_subtitle;
	case SectionId::Processes:
		return labels.processes_subtitle;
	case SectionId::Memory:
		return labels.memory_subtitle;
	case SectionId::Threads:
		return labels.threads_subtitle;
	case SectionId::Events:
	default:
		return labels.events_subtitle;
	}
}

controls::StatItem MakeStat(std::string text)
{
	controls::StatItem item;
	item.text = std::move(text);
	return item;
}

WorkspaceCardViewModel MakeCard(std::string title, std::string body, float height)
{
	WorkspaceCardViewModel card;
	card.title = std::move(title);
	card.body = std::move(body);
	card.height = height;
	return card;
}

WorkspaceActionViewModel MakeAction(const WorkspaceActionChip &chip)
{
	return WorkspaceActionViewModel{chip.action_key, chip.label, chip.active};
}

WorkspaceListItemViewModel MakeListItem(const WorkspaceListEntry &entry, float height)
{
	return WorkspaceListItemViewModel{
		entry.action_key,
		entry.title,
		entry.subtitle,
		entry.badge,
		entry.selected,
		height,
	};
}

void AddSharedPanelCards(const WorkspaceLabels &labels, SectionId selected_section,
			 const WorkspaceState &state, float density,
			 WorkspacePanelViewModel &panel)
{
	switch (selected_section) {
	case SectionId::Session:
		panel.cards.push_back(MakeCard(labels.session, state.session_primary,
					       92.0f * density));
		panel.cards.push_back(MakeCard(labels.events, state.session_secondary,
					       92.0f * density));
		break;
	case SectionId::Processes:
		for (const auto &chip : state.process_action_chips)
			panel.primary_actions.push_back(MakeAction(chip));
		for (const auto &entry : state.process_entries)
			panel.list_entries.push_back(MakeListItem(entry, 72.0f * density));
		panel.cards.push_back(MakeCard(labels.processes, state.process_primary,
					       92.0f * density));
		panel.cards.push_back(MakeCard(labels.session, state.process_secondary,
					       92.0f * density));
		break;
	case SectionId::Memory:
		for (const auto &chip : state.memory_action_chips)
			panel.primary_actions.push_back(MakeAction(chip));
		for (const auto &chip : state.memory_page_action_chips)
			panel.secondary_actions.push_back(MakeAction(chip));
		for (const auto &entry : state.memory_result_entries)
			panel.list_entries.push_back(MakeListItem(entry, 74.0f * density));
		for (const auto &entry : state.memory_page_entries)
			panel.detail_entries.push_back(MakeListItem(entry, 66.0f * density));
		panel.detail_lines = state.memory_scalar_entries;
		panel.cards.push_back(MakeCard(labels.memory, state.memory_primary,
					       96.0f * density));
		panel.cards.push_back(MakeCard(labels.processes, state.memory_secondary,
					       92.0f * density));
		break;
	case SectionId::Threads:
		panel.cards.push_back(MakeCard(labels.threads, state.thread_primary,
					       92.0f * density));
		panel.cards.push_back(MakeCard(labels.memory, state.thread_secondary,
					       92.0f * density));
		break;
	case SectionId::Events:
	default:
		panel.cards.push_back(MakeCard(labels.events, state.event_primary,
					       92.0f * density));
		panel.cards.push_back(MakeCard(labels.session, state.event_secondary,
					       92.0f * density));
		break;
	}
	panel.cards.push_back(MakeCard(labels.title, state.footer_message, 84.0f * density));
}

WorkspacePanelViewModel BuildPanel(const WorkspaceLabels &labels,
				   const WorkspaceState &state,
				   SectionId selected_section, float density)
{
	WorkspacePanelViewModel panel{
	};
	panel.title = std::string(SectionTitle(labels, selected_section));
	panel.subtitle = std::string(SectionSubtitle(labels, selected_section));

	switch (selected_section) {
	case SectionId::Session:
		panel.stats.push_back(MakeStat(ui_format::KeyValue(
			labels.connected, BoolText(labels, state.connected))));
		panel.stats.push_back(MakeStat(ui_format::KeyValue(
			labels.session_open, BoolText(labels, state.session_open))));
		panel.stats.push_back(
			MakeStat(ui_format::KeyValue(labels.hook, state.hook_active)));
		panel.stats.push_back(MakeStat(ui_format::KeyValue(labels.process_count,
							 state.process_count)));
		panel.stats.push_back(MakeStat(ui_format::KeyValue("target pid", state.target_pid)));
		break;
	case SectionId::Processes:
		panel.stats.push_back(MakeStat(ui_format::KeyValue(labels.process_count,
							 state.process_count)));
		panel.stats.push_back(MakeStat(ui_format::KeyValue(labels.thread_count,
							 state.thread_count)));
		panel.stats.push_back(MakeStat(ui_format::KeyValue(
			labels.session_open, BoolText(labels, state.session_open))));
		panel.stats.push_back(MakeStat(ui_format::KeyValue("target pid", state.target_pid)));
		break;
	case SectionId::Memory:
		panel.stats.push_back(MakeStat(ui_format::KeyValue(
			labels.session_open, BoolText(labels, state.session_open))));
		panel.stats.push_back(MakeStat(ui_format::KeyValue(labels.process_count,
							 state.process_count)));
		panel.stats.push_back(MakeStat(ui_format::KeyValue(labels.thread_count,
							 state.thread_count)));
		panel.stats.push_back(MakeStat(ui_format::KeyValue("vmas", state.vma_count)));
		panel.stats.push_back(MakeStat(ui_format::KeyValue("images", state.image_count)));
		break;
	case SectionId::Threads:
		panel.stats.push_back(MakeStat(ui_format::KeyValue(labels.thread_count,
							 state.thread_count)));
		panel.stats.push_back(MakeStat(ui_format::KeyValue(labels.event_count,
							 state.event_count)));
		panel.stats.push_back(MakeStat(ui_format::KeyValue(
			labels.session_open, BoolText(labels, state.session_open))));
		panel.stats.push_back(MakeStat(ui_format::KeyValue("target tid", state.target_tid)));
		break;
	case SectionId::Events:
	default:
		panel.stats.push_back(MakeStat(ui_format::KeyValue(labels.event_count,
							 state.event_count)));
		panel.stats.push_back(MakeStat(ui_format::KeyValue(labels.process_count,
							 state.process_count)));
		panel.stats.push_back(MakeStat(ui_format::KeyValue(
			labels.session_open, BoolText(labels, state.session_open))));
		panel.stats.push_back(MakeStat(ui_format::KeyValue("queue", state.event_queue_depth)));
		break;
	}

	AddSharedPanelCards(labels, selected_section, state, density, panel);
	return panel;
}

} // namespace

WorkspaceViewModel BuildWorkspaceViewModel(const WorkspaceLabels &labels,
					   const WorkspaceState &state,
					   WorkspaceSectionId selected_section,
					   const WorkspaceAnimationSnapshot &animation,
					   bool portrait, float density)
{
	WorkspaceViewModel model{
	};
	model.portrait = portrait;
	model.rail_title = labels.title;
	model.workspace_title =
		ui_format::JoinTitle(labels.title, SectionTitle(labels, selected_section));
	model.workspace_subtitle = std::string(SectionSubtitle(labels, selected_section));
	model.panel = BuildPanel(labels, state, selected_section, density);

	const std::array<std::string, kWorkspaceSectionCount> section_labels = {
		labels.session,
		labels.processes,
		labels.memory,
		labels.threads,
		labels.events,
	};

	for (int i = 0; i < kWorkspaceSectionCount; ++i) {
		model.sections[i] = controls::SectionItem{
			section_labels[i],
			i == static_cast<int>(selected_section),
			animation.section_mix[i],
		};
	}

	model.metrics[0] = controls::MetricItem{
		ui_format::KeyValue(labels.connected, BoolText(labels, state.connected)),
		animation.connected_mix,
	};
	model.metrics[1] = controls::MetricItem{
		ui_format::KeyValue(labels.session_open,
				    BoolText(labels, state.session_open)),
		animation.session_mix,
	};
	model.metrics[2] = controls::MetricItem{
		ui_format::KeyValue(labels.hook, state.hook_active),
		animation.hook_mix,
	};
	return model;
}

} // namespace lkmdbg::nativeui
