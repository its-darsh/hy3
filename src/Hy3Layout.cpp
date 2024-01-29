#include <regex>
#include <set>

#include <hyprland/src/Compositor.hpp>
#include <hyprland/src/plugins/PluginAPI.hpp>
#include <ranges>

#include "Hy3Layout.hpp"
#include "SelectionHook.hpp"
#include "globals.hpp"

std::unique_ptr<HOOK_CALLBACK_FN> renderHookPtr =
    std::make_unique<HOOK_CALLBACK_FN>(Hy3Layout::renderHook);
std::unique_ptr<HOOK_CALLBACK_FN> windowTitleHookPtr =
    std::make_unique<HOOK_CALLBACK_FN>(Hy3Layout::windowGroupUpdateRecursiveHook);
std::unique_ptr<HOOK_CALLBACK_FN> urgentHookPtr =
    std::make_unique<HOOK_CALLBACK_FN>(Hy3Layout::windowGroupUrgentHook);
std::unique_ptr<HOOK_CALLBACK_FN> tickHookPtr =
    std::make_unique<HOOK_CALLBACK_FN>(Hy3Layout::tickHook);

bool performContainment(Hy3Node& node, bool contained, CWindow* window) {
	if (node.data.type == Hy3NodeType::Group) {
		auto& group = node.data.as_group;
		contained |= group.containment;

		auto iter = node.data.as_group.children.begin();
		while (iter != node.data.as_group.children.end()) {
			switch ((*iter)->data.type) {
			case Hy3NodeType::Group: return performContainment(**iter, contained, window);
			case Hy3NodeType::Window:
				if (contained) {
					auto wpid = (*iter)->data.as_window->getPID();
					auto ppid = getPPIDof(window->getPID());
					while (ppid > 10) { // `> 10` yoinked from HL swallow
						if (ppid == wpid) {
							node.layout->nodes.push_back({
							    .parent = &node,
							    .data = window,
							    .workspace_id = node.workspace_id,
							    .layout = node.layout,
							});

							auto& child_node = node.layout->nodes.back();

							group.children.insert(std::next(iter), &child_node);
							child_node.markFocused();
							node.recalcSizePosRecursive();

							return true;
						}

						ppid = getPPIDof(ppid);
					}
				}
			}

			iter = std::next(iter);
		}
	}

	return false;
}

void Hy3Layout::onWindowCreated(CWindow* window, eDirection direction) {
	for (auto& node: this->nodes) {
		if (node.parent == nullptr && performContainment(node, false, window)) {
			return;
		}
	}

	IHyprLayout::onWindowCreated(window, direction);
}

void Hy3Layout::onWindowCreatedTiling(CWindow* window, eDirection) {
	hy3_log(
	    LOG,
	    "onWindowCreatedTiling called with window {:x} (floating: {}, monitor: {}, workspace: {})",
	    (uintptr_t) window,
	    window->m_bIsFloating,
	    window->m_iMonitorID,
	    window->m_iWorkspaceID
	);

	if (window->m_bIsFloating) return;

	auto* existing = this->getNodeFromWindow(window);
	if (existing != nullptr) {
		hy3_log(
		    ERR,
		    "onWindowCreatedTiling called with a window ({:x}) that is already tiled (node: {:x})",
		    (uintptr_t) window,
		    (uintptr_t) existing
		);
		return;
	}

	this->nodes.push_back({
	    .parent = nullptr,
	    .data = window,
	    .workspace_id = window->m_iWorkspaceID,
	    .layout = this,
	});

	this->insertNode(this->nodes.back());
}

void Hy3Layout::insertNode(Hy3Node& node) {
	if (node.parent != nullptr) {
		hy3_log(
		    ERR,
		    "insertNode called for node {:x} which already has a parent ({:x})",
		    (uintptr_t) &node,
		    (uintptr_t) node.parent
		);
		return;
	}

	auto* workspace = g_pCompositor->getWorkspaceByID(node.workspace_id);

	if (workspace == nullptr) {
		hy3_log(
		    ERR,
		    "insertNode called for node {:x} with invalid workspace id {}",
		    (uintptr_t) &node,
		    node.workspace_id
		);
		return;
	}

	node.reparenting = true;

	auto* monitor = g_pCompositor->getMonitorFromID(workspace->m_iMonitorID);

	Hy3Node* opening_into;
	Hy3Node* opening_after = nullptr;

	auto* root = this->getWorkspaceRootGroup(node.workspace_id);

	if (root != nullptr) {
		opening_after = root->getFocusedNode();

		// opening_after->parent cannot be nullptr
		if (opening_after == root) {
			opening_after =
			    opening_after->intoGroup(Hy3GroupLayout::SplitH, GroupEphemeralityOption::Standard);
		}
	}

	if (opening_after == nullptr) {
		if (g_pCompositor->m_pLastWindow != nullptr
		    && g_pCompositor->m_pLastWindow->m_iWorkspaceID == node.workspace_id
		    && !g_pCompositor->m_pLastWindow->m_bIsFloating
		    && (node.data.type == Hy3NodeType::Window
		        || g_pCompositor->m_pLastWindow != node.data.as_window)
		    && g_pCompositor->m_pLastWindow->m_bIsMapped)
		{
			opening_after = this->getNodeFromWindow(g_pCompositor->m_pLastWindow);
		} else {
			auto* mouse_window =
			    g_pCompositor->vectorToWindowTiled(g_pInputManager->getMouseCoordsInternal());

			if (mouse_window != nullptr && mouse_window->m_iWorkspaceID == node.workspace_id) {
				opening_after = this->getNodeFromWindow(mouse_window);
			}
		}
	}

	if (opening_after != nullptr
	    && ((node.data.type == Hy3NodeType::Group
	         && (opening_after == &node || node.data.as_group.hasChild(opening_after)))
	        || opening_after->reparenting))
	{
		opening_after = nullptr;
	}

	if (opening_after != nullptr) {
		opening_into = opening_after->parent;
	} else {
		if ((opening_into = this->getWorkspaceRootGroup(node.workspace_id)) == nullptr) {
			static const auto* tab_first_window =
			    &HyprlandAPI::getConfigValue(PHANDLE, "plugin:hy3:tab_first_window")->intValue;

			this->nodes.push_back({
			    .data = Hy3GroupLayout::SplitH,
			    .position = monitor->vecPosition + monitor->vecReservedTopLeft,
			    .size = monitor->vecSize - monitor->vecReservedTopLeft - monitor->vecReservedBottomRight,
			    .workspace_id = node.workspace_id,
			    .layout = this,
			});

			if (*tab_first_window) {
				auto& parent = this->nodes.back();

				this->nodes.push_back({
				    .parent = &parent,
				    .data = Hy3GroupLayout::Tabbed,
				    .position = parent.position,
				    .size = parent.size,
				    .workspace_id = node.workspace_id,
				    .layout = this,
				});

				parent.data.as_group.children.push_back(&this->nodes.back());
			}

			opening_into = &this->nodes.back();
		}
	}

	if (opening_into->data.type != Hy3NodeType::Group) {
		hy3_log(ERR, "opening_into node ({:x}) was not a group node", (uintptr_t) opening_into);
		errorNotif();
		return;
	}

	if (opening_into->workspace_id != node.workspace_id) {
		hy3_log(
		    WARN,
		    "opening_into node ({:x}) is on workspace {} which does not match the new window "
		    "(workspace {})",
		    (uintptr_t) opening_into,
		    opening_into->workspace_id,
		    node.workspace_id
		);
	}

	{
		// clang-format off
		static const auto* at_enable = &HyprlandAPI::getConfigValue(PHANDLE, "plugin:hy3:autotile:enable")->intValue;
		static const auto* at_ephemeral = &HyprlandAPI::getConfigValue(PHANDLE, "plugin:hy3:autotile:ephemeral_groups")->intValue;
		static const auto* at_trigger_width = &HyprlandAPI::getConfigValue(PHANDLE, "plugin:hy3:autotile:trigger_width")->intValue;
		static const auto* at_trigger_height = &HyprlandAPI::getConfigValue(PHANDLE, "plugin:hy3:autotile:trigger_height")->intValue;
		// clang-format on

		this->updateAutotileWorkspaces();

		auto& target_group = opening_into->data.as_group;
		if (*at_enable && opening_after != nullptr && target_group.children.size() > 1
		    && target_group.layout != Hy3GroupLayout::Tabbed
		    && this->shouldAutotileWorkspace(opening_into->workspace_id))
		{
			auto is_horizontal = target_group.layout == Hy3GroupLayout::SplitH;
			auto trigger = is_horizontal ? *at_trigger_width : *at_trigger_height;
			auto target_size = is_horizontal ? opening_into->size.x : opening_into->size.y;
			auto size_after_addition = target_size / (target_group.children.size() + 1);

			if (trigger >= 0 && (trigger == 0 || size_after_addition < trigger)) {
				auto opening_after1 = opening_after->intoGroup(
				    is_horizontal ? Hy3GroupLayout::SplitV : Hy3GroupLayout::SplitH,
				    *at_ephemeral ? GroupEphemeralityOption::Ephemeral : GroupEphemeralityOption::Standard
				);
				opening_into = opening_after;
				opening_after = opening_after1;
			}
		}
	}

	node.parent = opening_into;
	node.reparenting = false;

	if (opening_after == nullptr) {
		opening_into->data.as_group.children.push_back(&node);
	} else {
		auto& children = opening_into->data.as_group.children;
		auto iter = std::find(children.begin(), children.end(), opening_after);
		auto iter2 = std::next(iter);
		children.insert(iter2, &node);
	}

	hy3_log(
	    LOG,
	    "tiled node {:x} inserted after node {:x} in node {:x}",
	    (uintptr_t) &node,
	    (uintptr_t) opening_after,
	    (uintptr_t) opening_into
	);

	node.markFocused();
	opening_into->recalcSizePosRecursive();
}

void Hy3Layout::onWindowRemovedTiling(CWindow* window) {
	static const auto* node_collapse_policy =
	    &HyprlandAPI::getConfigValue(PHANDLE, "plugin:hy3:node_collapse_policy")->intValue;

	auto* node = this->getNodeFromWindow(window);

	if (node == nullptr) return;

	hy3_log(
	    LOG,
	    "removing window ({:x} as node {:x}) from node {:x}",
	    (uintptr_t) window,
	    (uintptr_t) node,
	    (uintptr_t) node->parent
	);

	window->m_sSpecialRenderData.rounding = true;
	window->m_sSpecialRenderData.border = true;
	window->m_sSpecialRenderData.decorate = true;

	if (window->m_bIsFullscreen) {
		g_pCompositor->setWindowFullscreen(window, false, FULLSCREEN_FULL);
	}

	Hy3Node* expand_actor = nullptr;
	auto* parent = node->removeFromParentRecursive(&expand_actor);
	this->nodes.remove(*node);
	if (expand_actor != nullptr) expand_actor->recalcSizePosRecursive();

	auto& group = parent->data.as_group;

	if (parent != nullptr) {
		parent->recalcSizePosRecursive();

		// returns if a given node is a group that can be collapsed given the current config
		auto node_is_collapsible = [](Hy3Node* node) {
			if (node->data.type != Hy3NodeType::Group) return false;
			if (*node_collapse_policy == 0) return true;
			else if (*node_collapse_policy == 1) return false;
			return node->parent->data.as_group.layout != Hy3GroupLayout::Tabbed;
		};

		if (group.children.size() == 1
		    && (group.ephemeral || node_is_collapsible(group.children.front())))
		{
			auto* target_parent = parent;
			while (target_parent != nullptr && Hy3Node::swallowGroups(target_parent)) {
				target_parent = target_parent->parent;
			}

			if (target_parent != parent && target_parent != nullptr)
				target_parent->recalcSizePosRecursive();
		}
	}
}

void Hy3Layout::onWindowFocusChange(CWindow* window) {
	auto* node = this->getNodeFromWindow(window);
	if (node == nullptr) return;

	hy3_log(
	    TRACE,
	    "changing window focus to window {:x} as node {:x}",
	    (uintptr_t) window,
	    (uintptr_t) node
	);

	node->markFocused();
	while (node->parent != nullptr) node = node->parent;
	node->recalcSizePosRecursive();
}

bool Hy3Layout::isWindowTiled(CWindow* window) {
	return this->getNodeFromWindow(window) != nullptr;
}

void Hy3Layout::recalculateMonitor(const int& monitor_id) {
	hy3_log(LOG, "recalculating monitor {}", monitor_id);
	const auto monitor = g_pCompositor->getMonitorFromID(monitor_id);
	if (monitor == nullptr) return;

	g_pHyprRenderer->damageMonitor(monitor);

	// todo: refactor this

	auto* top_node = this->getWorkspaceRootGroup(monitor->activeWorkspace);
	if (top_node != nullptr) {
		top_node->position = monitor->vecPosition + monitor->vecReservedTopLeft;
		top_node->size =
		    monitor->vecSize - monitor->vecReservedTopLeft - monitor->vecReservedBottomRight;

		top_node->recalcSizePosRecursive();
	}

	top_node = this->getWorkspaceRootGroup(monitor->specialWorkspaceID);

	if (top_node != nullptr) {
		top_node->position = monitor->vecPosition + monitor->vecReservedTopLeft;
		top_node->size =
		    monitor->vecSize - monitor->vecReservedTopLeft - monitor->vecReservedBottomRight;

		top_node->recalcSizePosRecursive();
	}
}

void Hy3Layout::recalculateWindow(CWindow* window) {
	auto* node = this->getNodeFromWindow(window);
	if (node == nullptr) return;
	node->recalcSizePosRecursive();
}

ShiftDirection reverse(ShiftDirection direction) {
	switch (direction)
	{
	case ShiftDirection::Left: return ShiftDirection::Right;
	case ShiftDirection::Right: return ShiftDirection::Left;
	case ShiftDirection::Up: return ShiftDirection::Down;
	case ShiftDirection::Down: return ShiftDirection::Up;
	default: return direction;
	}
}

void Hy3Layout::resizeActiveWindow(const Vector2D& delta, eRectCorner corner, CWindow* pWindow) {
	auto window = pWindow ? pWindow : g_pCompositor->m_pLastWindow;
	if (!g_pCompositor->windowValidMapped(window)) return;

	auto* node = this->getNodeFromWindow(window);

	if(node != nullptr) {
		node = &node->getExpandActor();

		auto monitor = g_pCompositor->getMonitorFromID(window->m_iMonitorID);

		const bool display_left =
			STICKS(node->position.x, monitor->vecPosition.x + monitor->vecReservedTopLeft.x);
		const bool display_right = STICKS(
			node->position.x + node->size.x,
			monitor->vecPosition.x + monitor->vecSize.x - monitor->vecReservedBottomRight.x
		);
		const bool display_top =
			STICKS(node->position.y, monitor->vecPosition.y + monitor->vecReservedTopLeft.y);
		const bool display_bottom = STICKS(
			node->position.y + node->size.y,
			monitor->vecPosition.y + monitor->vecSize.y - monitor->vecReservedBottomRight.y
		);

		Vector2D resize_delta = delta;
		if (display_left && display_right) resize_delta.x = 0;
		if (display_top && display_bottom) resize_delta.y = 0;

		ShiftDirection target_edge_x;
		ShiftDirection target_edge_y;

		// Determine the direction in which we're going to look for the neighbor node
		// that will be resized
		if(corner == CORNER_NONE) {			// It's probably a keyboard event.
			target_edge_x = display_right ? ShiftDirection::Left : ShiftDirection::Right;
			target_edge_y = display_bottom ? ShiftDirection::Up : ShiftDirection::Down;

			// If the anchor is not at the top/left then reverse the delta
			if(target_edge_x == ShiftDirection::Left) resize_delta.x = -resize_delta.x;
			if(target_edge_y == ShiftDirection::Up) resize_delta.y = -resize_delta.y;
		} else {							// It's probably a mouse event
			// Resize against the edges corresponding to the selected corner
			target_edge_x = corner == CORNER_TOPLEFT || corner == CORNER_BOTTOMLEFT
				? ShiftDirection::Left : ShiftDirection::Right;
			target_edge_y = corner == CORNER_TOPLEFT || corner == CORNER_TOPRIGHT
				? ShiftDirection::Up : ShiftDirection::Down;
		}

		// Find the neighboring node in each axis, which will be either above or at the
		// same level as the initiating node in the layout hierarchy.  These are the nodes
		// which must get resized (rather than the initiator) because they are the
		// highest point in the hierarchy
		auto horizontal_neighbor = node->findNeighbor(target_edge_x);
		auto vertical_neighbor = node->findNeighbor(target_edge_y);

		const auto animate =
			&g_pConfigManager->getConfigValuePtr("misc:animate_manual_resizes")->intValue;

		// Note that the resize direction is reversed, because from the neighbor's perspective
		// the edge to be moved is the opposite way round.  However, the delta is still the same.
		if(horizontal_neighbor) {
			horizontal_neighbor->resize(reverse(target_edge_x), resize_delta.x, *animate == 0);
		}

		if(vertical_neighbor) {
			vertical_neighbor->resize(reverse(target_edge_y), resize_delta.y, *animate == 0);
		}
	} else if(window->m_bIsFloating) {
		// No parent node - is this a floating window?  If so, use the same logic as the `main` layout
		const auto required_size = Vector2D(
			std::max((window->m_vRealSize.goalv() + delta).x, 20.0),
			std::max((window->m_vRealSize.goalv() + delta).y, 20.0)
		);
		window->m_vRealSize = required_size;
	}
}

void Hy3Layout::fullscreenRequestForWindow(
    CWindow* window,
    eFullscreenMode fullscreen_mode,
    bool on
) {
	if (!g_pCompositor->windowValidMapped(window)) return;
	if (on == window->m_bIsFullscreen || g_pCompositor->isWorkspaceSpecial(window->m_iWorkspaceID))
		return;

	const auto monitor = g_pCompositor->getMonitorFromID(window->m_iMonitorID);
	const auto workspace = g_pCompositor->getWorkspaceByID(window->m_iWorkspaceID);
	if (workspace->m_bHasFullscreenWindow && on) return;

	window->m_bIsFullscreen = on;
	workspace->m_bHasFullscreenWindow = !workspace->m_bHasFullscreenWindow;

	if (!window->m_bIsFullscreen) {
		auto* node = this->getNodeFromWindow(window);

		if (node) {
			// restore node positioning if tiled
			this->applyNodeDataToWindow(node);
		} else {
			// restore floating position if not
			window->m_vRealPosition = window->m_vLastFloatingPosition;
			window->m_vRealSize = window->m_vLastFloatingSize;

			window->m_sSpecialRenderData.rounding = true;
			window->m_sSpecialRenderData.border = true;
			window->m_sSpecialRenderData.decorate = true;
		}
	} else {
		workspace->m_efFullscreenMode = fullscreen_mode;

		// save position and size if floating
		if (window->m_bIsFloating) {
			window->m_vLastFloatingPosition = window->m_vRealPosition.goalv();
			window->m_vPosition = window->m_vRealPosition.goalv();
			window->m_vLastFloatingSize = window->m_vRealSize.goalv();
			window->m_vSize = window->m_vRealSize.goalv();
		}

		if (fullscreen_mode == FULLSCREEN_FULL) {
			window->m_vRealPosition = monitor->vecPosition;
			window->m_vRealSize = monitor->vecSize;
		} else {
			// Copy of vaxry's massive hack

			// clang-format off
			static const auto* gaps_in = &HyprlandAPI::getConfigValue(PHANDLE, "general:gaps_in")->intValue;
			static const auto* gaps_out = &HyprlandAPI::getConfigValue(PHANDLE, "general:gaps_out")->intValue;
			// clang-format on

			int outer_gaps = -(*gaps_in - *gaps_out);
			auto gap_pos_offset = Vector2D(outer_gaps, outer_gaps);
			auto gap_size_offset = Vector2D(outer_gaps * 2, outer_gaps * 2);

			Hy3Node fakeNode = {
			    .data = window,
			    .position = monitor->vecPosition + monitor->vecReservedTopLeft,
			    .size = monitor->vecSize - monitor->vecReservedTopLeft - monitor->vecReservedBottomRight,
			    .gap_topleft_offset = gap_pos_offset,
			    .gap_bottomright_offset = gap_size_offset,
			    .workspace_id = window->m_iWorkspaceID,
			};

			this->applyNodeDataToWindow(&fakeNode);
		}
	}

	g_pCompositor->updateWindowAnimatedDecorationValues(window);
	g_pXWaylandManager->setWindowSize(window, window->m_vRealSize.goalv());
	g_pCompositor->changeWindowZOrder(window, true);
	this->recalculateMonitor(monitor->ID);
}

std::any Hy3Layout::layoutMessage(SLayoutMessageHeader header, std::string content) {
	if (content == "togglesplit") {
		auto* node = this->getNodeFromWindow(header.pWindow);
		if (node != nullptr && node->parent != nullptr) {
			auto& layout = node->parent->data.as_group.layout;

			switch (layout) {
			case Hy3GroupLayout::SplitH:
				layout = Hy3GroupLayout::SplitV;
				node->parent->recalcSizePosRecursive();
				break;
			case Hy3GroupLayout::SplitV:
				layout = Hy3GroupLayout::SplitH;
				node->parent->recalcSizePosRecursive();
				break;
			case Hy3GroupLayout::Tabbed: break;
			}
		}
	}

	return "";
}

SWindowRenderLayoutHints Hy3Layout::requestRenderHints(CWindow* window) { return {}; }

void Hy3Layout::switchWindows(CWindow* pWindowA, CWindow* pWindowB) {
	// todo
}

void Hy3Layout::moveWindowTo(CWindow* window, const std::string& direction) {
	auto* node = this->getNodeFromWindow(window);
	if (node == nullptr) return;

	ShiftDirection shift;
	if (direction == "l") shift = ShiftDirection::Left;
	else if (direction == "r") shift = ShiftDirection::Right;
	else if (direction == "u") shift = ShiftDirection::Up;
	else if (direction == "d") shift = ShiftDirection::Down;
	else return;

	this->shiftNode(*node, shift, false, false);
}

void Hy3Layout::alterSplitRatio(CWindow* pWindow, float delta, bool exact) {
	// todo
}

std::string Hy3Layout::getLayoutName() { return "hy3"; }

CWindow* Hy3Layout::getNextWindowCandidate(CWindow* window) {
	auto* workspace = g_pCompositor->getWorkspaceByID(window->m_iWorkspaceID);

	if (workspace->m_bHasFullscreenWindow) {
		return g_pCompositor->getFullscreenWindowOnWorkspace(window->m_iWorkspaceID);
	}

	// return the first floating window on the same workspace that has not asked not to be focused
	if (window->m_bIsFloating) {
		for (auto& w: g_pCompositor->m_vWindows | std::views::reverse) {
			if (w->m_bIsMapped && !w->isHidden() && w->m_bIsFloating && w->m_iX11Type != 2
			    && w->m_iWorkspaceID == window->m_iWorkspaceID && !w->m_bX11ShouldntFocus
			    && !w->m_bNoFocus && w.get() != window)
			{
				return w.get();
			}
		}
	}

	auto* node = this->getWorkspaceFocusedNode(window->m_iWorkspaceID, true);
	if (node == nullptr) return nullptr;

	switch (node->data.type) {
	case Hy3NodeType::Window: return node->data.as_window;
	case Hy3NodeType::Group: return nullptr;
	default: return nullptr;
	}
}

void Hy3Layout::replaceWindowDataWith(CWindow* from, CWindow* to) {
	auto* node = this->getNodeFromWindow(from);
	if (node == nullptr) return;

	node->data.as_window = to;
	this->applyNodeDataToWindow(node);
}

bool Hy3Layout::isWindowReachable(CWindow* window) {
	return this->getNodeFromWindow(window) != nullptr || IHyprLayout::isWindowReachable(window);
}

void Hy3Layout::bringWindowToTop(CWindow* window) {
	auto node = this->getNodeFromWindow(window);
	if (node == nullptr) return;
	node->bringToTop();
}

void Hy3Layout::onEnable() {
	for (auto& window: g_pCompositor->m_vWindows) {
		if (window->isHidden() || !window->m_bIsMapped || window->m_bFadingOut || window->m_bIsFloating)
			continue;

		this->onWindowCreatedTiling(window.get());
	}

	HyprlandAPI::registerCallbackStatic(PHANDLE, "render", renderHookPtr.get());
	HyprlandAPI::registerCallbackStatic(PHANDLE, "windowTitle", windowTitleHookPtr.get());
	HyprlandAPI::registerCallbackStatic(PHANDLE, "urgent", urgentHookPtr.get());
	HyprlandAPI::registerCallbackStatic(PHANDLE, "tick", tickHookPtr.get());
	selection_hook::enable();
}

void Hy3Layout::onDisable() {
	HyprlandAPI::unregisterCallback(PHANDLE, renderHookPtr.get());
	HyprlandAPI::unregisterCallback(PHANDLE, windowTitleHookPtr.get());
	HyprlandAPI::unregisterCallback(PHANDLE, urgentHookPtr.get());
	HyprlandAPI::unregisterCallback(PHANDLE, tickHookPtr.get());
	selection_hook::disable();

	for (auto& node: this->nodes) {
		if (node.data.type == Hy3NodeType::Window) {
			node.data.as_window->setHidden(false);
		}
	}

	this->nodes.clear();
}

void Hy3Layout::makeGroupOnWorkspace(
    int workspace,
    Hy3GroupLayout layout,
    GroupEphemeralityOption ephemeral
) {
	auto* node = this->getWorkspaceFocusedNode(workspace);
	this->makeGroupOn(node, layout, ephemeral);
}

void Hy3Layout::makeOppositeGroupOnWorkspace(int workspace, GroupEphemeralityOption ephemeral) {
	auto* node = this->getWorkspaceFocusedNode(workspace);
	this->makeOppositeGroupOn(node, ephemeral);
}

void Hy3Layout::changeGroupOnWorkspace(int workspace, Hy3GroupLayout layout) {
	auto* node = this->getWorkspaceFocusedNode(workspace);
	if (node == nullptr) return;

	this->changeGroupOn(*node, layout);
}

void Hy3Layout::untabGroupOnWorkspace(int workspace) {
	auto* node = this->getWorkspaceFocusedNode(workspace);
	if (node == nullptr) return;

	this->untabGroupOn(*node);
}

void Hy3Layout::toggleTabGroupOnWorkspace(int workspace) {
	auto* node = this->getWorkspaceFocusedNode(workspace);
	if (node == nullptr) return;

	this->toggleTabGroupOn(*node);
}

void Hy3Layout::changeGroupToOppositeOnWorkspace(int workspace) {
	auto* node = this->getWorkspaceFocusedNode(workspace);
	if (node == nullptr) return;

	this->changeGroupToOppositeOn(*node);
}

void Hy3Layout::changeGroupEphemeralityOnWorkspace(int workspace, bool ephemeral) {
	auto* node = this->getWorkspaceFocusedNode(workspace);
	if (node == nullptr) return;

	this->changeGroupEphemeralityOn(*node, ephemeral);
}

void Hy3Layout::makeGroupOn(
    Hy3Node* node,
    Hy3GroupLayout layout,
    GroupEphemeralityOption ephemeral
) {
	if (node == nullptr) return;

	if (node->parent != nullptr) {
		auto& group = node->parent->data.as_group;
		if (group.children.size() == 1) {
			group.setLayout(layout);
			group.setEphemeral(ephemeral);
			node->parent->updateTabBarRecursive();
			node->parent->recalcSizePosRecursive();
			return;
		}
	}

	node->intoGroup(layout, ephemeral);
}

void Hy3Layout::makeOppositeGroupOn(Hy3Node* node, GroupEphemeralityOption ephemeral) {
	if (node == nullptr) return;

	if (node->parent == nullptr) {
		node->intoGroup(Hy3GroupLayout::SplitH, ephemeral);
		return;
	}

	auto& group = node->parent->data.as_group;
	auto layout =
	    group.layout == Hy3GroupLayout::SplitH ? Hy3GroupLayout::SplitV : Hy3GroupLayout::SplitH;

	if (group.children.size() == 1) {
		group.setLayout(layout);
		group.setEphemeral(ephemeral);
		node->parent->recalcSizePosRecursive();
		return;
	}

	node->intoGroup(layout, ephemeral);
}

void Hy3Layout::changeGroupOn(Hy3Node& node, Hy3GroupLayout layout) {
	if (node.parent == nullptr) {
		makeGroupOn(&node, layout, GroupEphemeralityOption::Ephemeral);
		return;
	}

	auto& group = node.parent->data.as_group;
	group.setLayout(layout);
	node.parent->updateTabBarRecursive();
	node.parent->recalcSizePosRecursive();
}

void Hy3Layout::untabGroupOn(Hy3Node& node) {
	if (node.parent == nullptr) return;

	auto& group = node.parent->data.as_group;
	if (group.layout != Hy3GroupLayout::Tabbed) return;

	changeGroupOn(node, group.previous_nontab_layout);
}

void Hy3Layout::toggleTabGroupOn(Hy3Node& node) {
	if (node.parent == nullptr) return;

	auto& group = node.parent->data.as_group;
	if (group.layout != Hy3GroupLayout::Tabbed) changeGroupOn(node, Hy3GroupLayout::Tabbed);
	else changeGroupOn(node, group.previous_nontab_layout);
}

void Hy3Layout::changeGroupToOppositeOn(Hy3Node& node) {
	if (node.parent == nullptr) return;

	auto& group = node.parent->data.as_group;

	if (group.layout == Hy3GroupLayout::Tabbed) {
		group.setLayout(group.previous_nontab_layout);
	} else {
		group.setLayout(
		    group.layout == Hy3GroupLayout::SplitH ? Hy3GroupLayout::SplitV : Hy3GroupLayout::SplitH
		);
	}

	node.parent->recalcSizePosRecursive();
}

void Hy3Layout::changeGroupEphemeralityOn(Hy3Node& node, bool ephemeral) {
	if (node.parent == nullptr) return;

	auto& group = node.parent->data.as_group;
	group.setEphemeral(
	    ephemeral ? GroupEphemeralityOption::ForceEphemeral : GroupEphemeralityOption::Standard
	);
}

void Hy3Layout::shiftNode(Hy3Node& node, ShiftDirection direction, bool once, bool visible) {
	if (once && node.parent != nullptr && node.parent->data.as_group.children.size() == 1) {
		if (node.parent->parent == nullptr) {
			node.parent->data.as_group.setLayout(Hy3GroupLayout::SplitH);
			node.parent->recalcSizePosRecursive();
		} else {
			auto* node2 = node.parent;
			Hy3Node::swapData(node, *node2);
			node2->layout->nodes.remove(node);
			node2->updateTabBarRecursive();
			node2->recalcSizePosRecursive();
		}
	} else {
		this->shiftOrGetFocus(node, direction, true, once, visible);
	}
}

void Hy3Layout::shiftWindow(int workspace, ShiftDirection direction, bool once, bool visible) {
	auto* node = this->getWorkspaceFocusedNode(workspace);
	if (node == nullptr) return;

	this->shiftNode(*node, direction, once, visible);
}

void Hy3Layout::shiftFocus(int workspace, ShiftDirection direction, bool visible) {
	auto* current_window = g_pCompositor->m_pLastWindow;

	if (current_window != nullptr) {
		auto* p_workspace = g_pCompositor->getWorkspaceByID(current_window->m_iWorkspaceID);
		if (p_workspace->m_bHasFullscreenWindow) return;

		if (current_window->m_bIsFloating) {
			auto* next_window = g_pCompositor->getWindowInDirection(
			    current_window,
			    direction == ShiftDirection::Left   ? 'l'
			    : direction == ShiftDirection::Up   ? 'u'
			    : direction == ShiftDirection::Down ? 'd'
			                                        : 'r'
			);

			if (next_window != nullptr) g_pCompositor->focusWindow(next_window);
			return;
		}
	}

	auto* node = this->getWorkspaceFocusedNode(workspace);
	if (node == nullptr) return;

	auto* target = this->shiftOrGetFocus(*node, direction, false, false, visible);

	if (target != nullptr) {
		target->focus();
		while (target->parent != nullptr) target = target->parent;
		target->recalcSizePosRecursive();
	}
}

void changeNodeWorkspaceRecursive(Hy3Node& node, CWorkspace* workspace) {
	node.workspace_id = workspace->m_iID;

	if (node.data.type == Hy3NodeType::Window) {
		auto* window = node.data.as_window;
		window->moveToWorkspace(workspace->m_iID);
		window->updateToplevel();
		window->updateDynamicRules();
	} else {
		for (auto* child: node.data.as_group.children) {
			changeNodeWorkspaceRecursive(*child, workspace);
		}
	}
}

void Hy3Layout::moveNodeToWorkspace(int origin, std::string wsname, bool follow) {
	std::string target_name;
	auto target = getWorkspaceIDFromString(wsname, target_name);

	if (target == WORKSPACE_INVALID) {
		hy3_log(ERR, "moveNodeToWorkspace called with invalid workspace {}", wsname);
		return;
	}

	if (origin == target) return;

	auto* node = this->getWorkspaceFocusedNode(origin);
	auto* focused_window = g_pCompositor->m_pLastWindow;
	auto* focused_window_node = this->getNodeFromWindow(focused_window);

	auto* workspace = g_pCompositor->getWorkspaceByID(target);

	auto wsid = node != nullptr           ? node->workspace_id
	          : focused_window != nullptr ? focused_window->m_iWorkspaceID
	                                      : WORKSPACE_INVALID;

	if (wsid == WORKSPACE_INVALID) return;

	auto* origin_ws = g_pCompositor->getWorkspaceByID(wsid);

	if (workspace == nullptr) {
		hy3_log(LOG, "creating target workspace {} for node move", target);

		workspace = g_pCompositor->createNewWorkspace(target, origin_ws->m_iMonitorID, target_name);
	}

	// floating or fullscreen
	if (focused_window != nullptr
	    && (focused_window_node == nullptr || focused_window->m_bIsFullscreen))
	{
		hy3_log(LOG, "{:x}, {:x}", (uintptr_t) focused_window, (uintptr_t) workspace);
		g_pCompositor->moveWindowToWorkspaceSafe(focused_window, workspace);
	} else {
		if (node == nullptr) return;

		hy3_log(
		    LOG,
		    "moving node {:x} from workspace {} to workspace {} (follow: {})",
		    (uintptr_t) node,
		    origin,
		    target,
		    follow
		);

		Hy3Node* expand_actor = nullptr;
		node->removeFromParentRecursive(&expand_actor);
		if (expand_actor != nullptr) expand_actor->recalcSizePosRecursive();

		changeNodeWorkspaceRecursive(*node, workspace);
		this->insertNode(*node);
	}

	if (follow) {
		auto* monitor = g_pCompositor->getMonitorFromID(workspace->m_iMonitorID);

		if (workspace->m_bIsSpecialWorkspace) {
			monitor->setSpecialWorkspace(workspace);
		} else if (origin_ws->m_bIsSpecialWorkspace) {
			g_pCompositor->getMonitorFromID(origin_ws->m_iMonitorID)->setSpecialWorkspace(nullptr);
		}

		monitor->changeWorkspace(workspace);

		static auto* const allow_workspace_cycles =
		    &g_pConfigManager->getConfigValuePtr("binds:allow_workspace_cycles")->intValue;
		if (*allow_workspace_cycles) workspace->rememberPrevWorkspace(origin_ws);
	}
}

void Hy3Layout::changeFocus(int workspace, FocusShift shift) {
	auto* node = this->getWorkspaceFocusedNode(workspace);
	if (node == nullptr) return;

	switch (shift) {
	case FocusShift::Bottom: goto bottom;
	case FocusShift::Top:
		while (node->parent != nullptr) {
			node = node->parent;
		}

		node->focus();
		return;
	case FocusShift::Raise:
		if (node->parent == nullptr) goto bottom;
		else {
			node->parent->focus();
		}
		return;
	case FocusShift::Lower:
		if (node->data.type == Hy3NodeType::Group && node->data.as_group.focused_child != nullptr)
			node->data.as_group.focused_child->focus();
		return;
	case FocusShift::Tab:
		// make sure we go up at least one level
		if (node->parent != nullptr) node = node->parent;
		while (node->parent != nullptr) {
			if (node->data.as_group.layout == Hy3GroupLayout::Tabbed) {
				node->focus();
				return;
			}

			node = node->parent;
		}
		return;
	case FocusShift::TabNode:
		// make sure we go up at least one level
		if (node->parent != nullptr) node = node->parent;
		while (node->parent != nullptr) {
			if (node->parent->data.as_group.layout == Hy3GroupLayout::Tabbed) {
				node->focus();
				return;
			}

			node = node->parent;
		}
		return;
	}

bottom:
	while (node->data.type == Hy3NodeType::Group && node->data.as_group.focused_child != nullptr) {
		node = node->data.as_group.focused_child;
	}

	node->focus();
	return;
}

Hy3Node* findTabBarAt(Hy3Node& node, Vector2D pos, Hy3Node** focused_node) {
	// clang-format off
	static const auto* gaps_in = &HyprlandAPI::getConfigValue(PHANDLE, "general:gaps_in")->intValue;
	static const auto* gaps_out = &HyprlandAPI::getConfigValue(PHANDLE, "general:gaps_out")->intValue;
	static const auto* tab_bar_height = &HyprlandAPI::getConfigValue(PHANDLE, "plugin:hy3:tabs:height")->intValue;
	static const auto* tab_bar_padding = &HyprlandAPI::getConfigValue(PHANDLE, "plugin:hy3:tabs:padding")->intValue;
	// clang-format on

	auto inset = *tab_bar_height + *tab_bar_padding;

	if (node.parent == nullptr) {
		inset += *gaps_out;
	} else {
		inset += *gaps_in;
	}

	if (node.data.type == Hy3NodeType::Group) {
		if (node.hidden) return nullptr;
		// note: tab bar clicks ignore animations
		if (node.position.x > pos.x || node.position.y > pos.y || node.position.x + node.size.x < pos.x
		    || node.position.y + node.size.y < pos.y)
			return nullptr;

		if (node.data.as_group.layout == Hy3GroupLayout::Tabbed
		    && node.data.as_group.tab_bar != nullptr)
		{
			if (pos.y < node.position.y + node.gap_topleft_offset.y + inset) {
				auto& children = node.data.as_group.children;
				auto& tab_bar = *node.data.as_group.tab_bar;

				auto size = tab_bar.size.vec();
				auto x = pos.x - tab_bar.pos.vec().x;
				auto child_iter = children.begin();

				for (auto& tab: tab_bar.bar.entries) {
					if (child_iter == children.end()) break;

					if (x > tab.offset.fl() * size.x && x < (tab.offset.fl() + tab.width.fl()) * size.x) {
						*focused_node = *child_iter;
						return &node;
					}

					child_iter = std::next(child_iter);
				}
			}

			if (node.data.as_group.focused_child != nullptr) {
				return findTabBarAt(*node.data.as_group.focused_child, pos, focused_node);
			}
		} else {
			for (auto child: node.data.as_group.children) {
				if (findTabBarAt(*child, pos, focused_node)) return child;
			}
		}
	}

	return nullptr;
}

void Hy3Layout::focusTab(
    int workspace,
    TabFocus target,
    TabFocusMousePriority mouse,
    bool wrap_scroll,
    int index
) {
	auto* node = this->getWorkspaceRootGroup(workspace);
	if (node == nullptr) return;

	Hy3Node* tab_node = nullptr;
	Hy3Node* tab_focused_node;

	if (target == TabFocus::MouseLocation || mouse != TabFocusMousePriority::Ignore) {
		if (g_pCompositor->windowFloatingFromCursor() == nullptr) {
			auto mouse_pos = g_pInputManager->getMouseCoordsInternal();
			tab_node = findTabBarAt(*node, mouse_pos, &tab_focused_node);
			if (tab_node != nullptr) goto hastab;
		}

		if (target == TabFocus::MouseLocation || mouse == TabFocusMousePriority::Require) return;
	}

	if (tab_node == nullptr) {
		tab_node = this->getWorkspaceFocusedNode(workspace);
		if (tab_node == nullptr) return;

		while (tab_node != nullptr && tab_node->data.as_group.layout != Hy3GroupLayout::Tabbed
		       && tab_node->parent != nullptr)
			tab_node = tab_node->parent;

		if (tab_node == nullptr || tab_node->data.type != Hy3NodeType::Group
		    || tab_node->data.as_group.layout != Hy3GroupLayout::Tabbed)
			return;
	}

hastab:
	if (target != TabFocus::MouseLocation) {
		if (tab_node->data.as_group.focused_child == nullptr
		    || tab_node->data.as_group.children.size() < 2)
			return;

		auto& children = tab_node->data.as_group.children;
		if (target == TabFocus::Index) {
			int i = 1;

			for (auto* node: children) {
				if (i == index) {
					tab_focused_node = node;
					goto cont;
				}

				i++;
			}

			return;
		cont:;
		} else {
			auto node_iter =
			    std::find(children.begin(), children.end(), tab_node->data.as_group.focused_child);
			if (node_iter == children.end()) return;
			if (target == TabFocus::Left) {
				if (node_iter == children.begin()) {
					if (wrap_scroll) node_iter = std::prev(children.end());
					else return;
				} else node_iter = std::prev(node_iter);

				tab_focused_node = *node_iter;
			} else {
				if (node_iter == std::prev(children.end())) {
					if (wrap_scroll) node_iter = children.begin();
					else return;
				} else node_iter = std::next(node_iter);

				tab_focused_node = *node_iter;
			}
		}
	}

	auto* focus = tab_focused_node;
	while (focus->data.type == Hy3NodeType::Group && !focus->data.as_group.group_focused
	       && focus->data.as_group.focused_child != nullptr)
		focus = focus->data.as_group.focused_child;

	focus->focus();
	tab_node->recalcSizePosRecursive();
}

void Hy3Layout::setNodeSwallow(int workspace, SetSwallowOption option) {
	auto* node = this->getWorkspaceFocusedNode(workspace);
	if (node == nullptr || node->parent == nullptr) return;

	auto* containment = &node->parent->data.as_group.containment;
	switch (option) {
	case SetSwallowOption::NoSwallow: *containment = false; break;
	case SetSwallowOption::Swallow: *containment = true; break;
	case SetSwallowOption::Toggle: *containment = !*containment; break;
	}
}

void Hy3Layout::killFocusedNode(int workspace) {
	if (g_pCompositor->m_pLastWindow != nullptr && g_pCompositor->m_pLastWindow->m_bIsFloating) {
		g_pCompositor->closeWindow(g_pCompositor->m_pLastWindow);
	} else {
		auto* node = this->getWorkspaceFocusedNode(workspace);
		if (node == nullptr) return;

		std::vector<CWindow*> windows;
		node->appendAllWindows(windows);

		for (auto* window: windows) {
			window->setHidden(false);
			g_pCompositor->closeWindow(window);
		}
	}
}

void Hy3Layout::expand(int workspace_id, ExpandOption option, ExpandFullscreenOption fs_option) {
	auto* node = this->getWorkspaceFocusedNode(workspace_id, false, true);
	if (node == nullptr) return;

	const auto workspace = g_pCompositor->getWorkspaceByID(workspace_id);
	const auto monitor = g_pCompositor->getMonitorFromID(workspace->m_iMonitorID);

	switch (option) {
	case ExpandOption::Expand: {
		if (node->parent == nullptr) {
			switch (fs_option) {
			case ExpandFullscreenOption::MaximizeAsFullscreen:
			case ExpandFullscreenOption::MaximizeIntermediate: goto fullscreen;
			case ExpandFullscreenOption::MaximizeOnly: return;
			}
		}

		if (node->data.type == Hy3NodeType::Group && !node->data.as_group.group_focused)
			node->data.as_group.expand_focused = ExpandFocusType::Stack;

		auto& group = node->parent->data.as_group;
		group.focused_child = node;
		group.expand_focused = ExpandFocusType::Latch;

		node->parent->recalcSizePosRecursive();

		if (node->parent->parent == nullptr) {
			switch (fs_option) {
			case ExpandFullscreenOption::MaximizeAsFullscreen: goto fullscreen;
			case ExpandFullscreenOption::MaximizeIntermediate:
			case ExpandFullscreenOption::MaximizeOnly: return;
			}
		}
	} break;
	case ExpandOption::Shrink:
		if (node->data.type == Hy3NodeType::Group) {
			auto& group = node->data.as_group;

			group.expand_focused = ExpandFocusType::NotExpanded;
			if (group.focused_child->data.type == Hy3NodeType::Group)
				group.focused_child->data.as_group.expand_focused = ExpandFocusType::Latch;

			node->recalcSizePosRecursive();
		}
		break;
	case ExpandOption::Base: {
		if (node->data.type == Hy3NodeType::Group) {
			node->data.as_group.collapseExpansions();
			node->recalcSizePosRecursive();
		}
		break;
	}
	case ExpandOption::Maximize: break;
	case ExpandOption::Fullscreen: break;
	}

	return;

	CWindow* window;
fullscreen:
	if (node->data.type != Hy3NodeType::Window) return;
	window = node->data.as_window;
	if (!window->m_bIsFullscreen || g_pCompositor->isWorkspaceSpecial(window->m_iWorkspaceID)) return;

	if (workspace->m_bHasFullscreenWindow) return;

	window->m_bIsFullscreen = true;
	workspace->m_bHasFullscreenWindow = true;
	workspace->m_efFullscreenMode = FULLSCREEN_FULL;
	window->m_vRealPosition = monitor->vecPosition;
	window->m_vRealSize = monitor->vecSize;
	goto fsupdate;
// unfullscreen:
// 	if (node->data.type != Hy3NodeType::Window) return;
// 	window = node->data.as_window;
// 	window->m_bIsFullscreen = false;
// 	workspace->m_bHasFullscreenWindow = false;
// 	goto fsupdate;
fsupdate:
	g_pCompositor->updateWindowAnimatedDecorationValues(window);
	g_pXWaylandManager->setWindowSize(window, window->m_vRealSize.goalv());
	g_pCompositor->changeWindowZOrder(window, true);
	this->recalculateMonitor(monitor->ID);
}

bool Hy3Layout::shouldRenderSelected(CWindow* window) {
	if (window == nullptr) return false;
	auto* root = this->getWorkspaceRootGroup(window->m_iWorkspaceID);
	if (root == nullptr || root->data.as_group.focused_child == nullptr) return false;
	auto* focused = root->getFocusedNode();
	if (focused == nullptr
	    || (focused->data.type == Hy3NodeType::Window
	        && focused->data.as_window != g_pCompositor->m_pLastWindow))
		return false;

	switch (focused->data.type) {
	case Hy3NodeType::Window: return focused->data.as_window == window;
	case Hy3NodeType::Group: {
		auto* node = this->getNodeFromWindow(window);
		if (node == nullptr) return false;
		return focused->data.as_group.hasChild(node);
	}
	default: return false;
	}
}

Hy3Node* Hy3Layout::getWorkspaceRootGroup(const int& workspace) {
	for (auto& node: this->nodes) {
		if (node.workspace_id == workspace && node.parent == nullptr
		    && node.data.type == Hy3NodeType::Group && !node.reparenting)
		{
			return &node;
		}
	}

	return nullptr;
}

Hy3Node* Hy3Layout::getWorkspaceFocusedNode(
    const int& workspace,
    bool ignore_group_focus,
    bool stop_at_expanded
) {
	auto* rootNode = this->getWorkspaceRootGroup(workspace);
	if (rootNode == nullptr) return nullptr;
	return rootNode->getFocusedNode(ignore_group_focus, stop_at_expanded);
}

void Hy3Layout::renderHook(void*, SCallbackInfo&, std::any data) {
	static bool rendering_normally = false;
	static std::vector<Hy3TabGroup*> rendered_groups;

	auto render_stage = std::any_cast<eRenderStage>(data);

	switch (render_stage) {
	case RENDER_PRE_WINDOWS:
		rendering_normally = true;
		rendered_groups.clear();
		break;
	case RENDER_POST_WINDOW:
		if (!rendering_normally) break;

		for (auto& entry: g_Hy3Layout->tab_groups) {
			if (!entry.hidden && entry.target_window == g_pHyprOpenGL->m_pCurrentWindow
			    && std::find(rendered_groups.begin(), rendered_groups.end(), &entry)
			           == rendered_groups.end())
			{
				entry.renderTabBar();
				rendered_groups.push_back(&entry);
			}
		}

		break;
	case RENDER_POST_WINDOWS:
		rendering_normally = false;

		for (auto& entry: g_Hy3Layout->tab_groups) {
			if (!entry.hidden
			    && entry.target_window->m_iMonitorID == g_pHyprOpenGL->m_RenderData.pMonitor->ID
			    && std::find(rendered_groups.begin(), rendered_groups.end(), &entry)
			           == rendered_groups.end())
			{
				entry.renderTabBar();
			}
		}

		break;
	default: break;
	}
}

void Hy3Layout::windowGroupUrgentHook(void* p, SCallbackInfo& callback_info, std::any data) {
	CWindow* window = std::any_cast<CWindow*>(data);
	if (window == nullptr) return;
	window->m_bIsUrgent = true;
	Hy3Layout::windowGroupUpdateRecursiveHook(p, callback_info, data);
}

void Hy3Layout::windowGroupUpdateRecursiveHook(void*, SCallbackInfo&, std::any data) {
	CWindow* window = std::any_cast<CWindow*>(data);
	if (window == nullptr) return;
	auto* node = g_Hy3Layout->getNodeFromWindow(window);

	// it is UB for `this` to be null
	if (node == nullptr) return;
	node->updateTabBarRecursive();
}

void Hy3Layout::tickHook(void*, SCallbackInfo&, std::any) {
	auto& tab_groups = g_Hy3Layout->tab_groups;
	auto entry = tab_groups.begin();
	while (entry != tab_groups.end()) {
		entry->tick();
		if (entry->bar.destroy) tab_groups.erase(entry++);
		else entry = std::next(entry);
	}
}

Hy3Node* Hy3Layout::getNodeFromWindow(CWindow* window) {
	for (auto& node: this->nodes) {
		if (node.data.type == Hy3NodeType::Window && node.data.as_window == window) {
			return &node;
		}
	}

	return nullptr;
}

void Hy3Layout::applyNodeDataToWindow(Hy3Node* node, bool no_animation) {
	if (node->data.type != Hy3NodeType::Window) return;
	auto* window = node->data.as_window;
	auto root_node = this->getWorkspaceRootGroup(window->m_iWorkspaceID);
	if (root_node == nullptr) return;

	CMonitor* monitor = nullptr;

	if (g_pCompositor->isWorkspaceSpecial(node->workspace_id)) {
		for (auto& m: g_pCompositor->m_vMonitors) {
			if (m->specialWorkspaceID == node->workspace_id) {
				monitor = m.get();
				break;
			}
		}
	} else {
		monitor = g_pCompositor->getMonitorFromID(
		    g_pCompositor->getWorkspaceByID(node->workspace_id)->m_iMonitorID
		);
	}

	if (monitor == nullptr) {
		hy3_log(
		    ERR,
		    "node {:x}'s workspace has no associated monitor, cannot apply node data",
		    (uintptr_t) node
		);
		errorNotif();
		return;
	}

	// clang-format off
	static const auto* gaps_in = &HyprlandAPI::getConfigValue(PHANDLE, "general:gaps_in")->intValue;
	static const auto* single_window_no_gaps = &HyprlandAPI::getConfigValue(PHANDLE, "plugin:hy3:no_gaps_when_only")->intValue;
	// clang-format on

	if (!g_pCompositor->windowExists(window) || !window->m_bIsMapped) {
		hy3_log(
		    ERR,
		    "node {:x} is an unmapped window ({:x}), cannot apply node data, removing from tiled "
		    "layout",
		    (uintptr_t) node,
		    (uintptr_t) window
		);
		errorNotif();
		this->onWindowRemovedTiling(window);
		return;
	}

	window->m_vSize = node->size;
	window->m_vPosition = node->position;

	auto only_node = root_node->data.as_group.children.size() == 1
	              && root_node->data.as_group.children.front()->data.type == Hy3NodeType::Window;

	if (!g_pCompositor->isWorkspaceSpecial(window->m_iWorkspaceID)
	    && ((*single_window_no_gaps && (only_node || window->m_bIsFullscreen))
	        || (window->m_bIsFullscreen
	            && g_pCompositor->getWorkspaceByID(window->m_iWorkspaceID)->m_efFullscreenMode
	                   == FULLSCREEN_FULL)))
	{

		CBox wb = {window->m_vPosition, window->m_vSize};
		wb.round();

		window->m_vRealPosition = wb.pos();
		window->m_vRealSize = wb.size();

		window->updateWindowDecos();

		window->m_sSpecialRenderData.rounding = false;
		window->m_sSpecialRenderData.border = false;
		window->m_sSpecialRenderData.decorate = false;
	} else {
		window->m_sSpecialRenderData.rounding = true;
		window->m_sSpecialRenderData.border = true;
		window->m_sSpecialRenderData.decorate = true;

		auto calcPos = window->m_vPosition;
		auto calcSize = window->m_vSize;

		auto gaps_offset_topleft = Vector2D(*gaps_in, *gaps_in) + node->gap_topleft_offset;
		auto gaps_offset_bottomright = Vector2D(*gaps_in * 2, *gaps_in * 2)
		                             + node->gap_bottomright_offset + node->gap_topleft_offset;

		calcPos = calcPos + gaps_offset_topleft;
		calcSize = calcSize - gaps_offset_bottomright;

		const auto reserved_area = window->getFullWindowReservedArea();
		calcPos = calcPos + reserved_area.topLeft;
		calcSize = calcSize - (reserved_area.topLeft + reserved_area.bottomRight);

		CBox wb = {calcPos, calcSize};
		wb.round();

		window->m_vRealPosition = wb.pos();
		window->m_vRealSize = wb.size();

		g_pXWaylandManager->setWindowSize(window, calcSize);

		if (no_animation) {
			g_pHyprRenderer->damageWindow(window);

			window->m_vRealPosition.warp();
			window->m_vRealSize.warp();

			g_pHyprRenderer->damageWindow(window);
		}

		window->updateWindowDecos();
	}
}

bool shiftIsForward(ShiftDirection direction) {
	return direction == ShiftDirection::Right || direction == ShiftDirection::Down;
}

bool shiftIsVertical(ShiftDirection direction) {
	return direction == ShiftDirection::Up || direction == ShiftDirection::Down;
}

bool shiftMatchesLayout(Hy3GroupLayout layout, ShiftDirection direction) {
	return (layout == Hy3GroupLayout::SplitV && shiftIsVertical(direction))
	    || (layout != Hy3GroupLayout::SplitV && !shiftIsVertical(direction));
}

Hy3Node* Hy3Layout::shiftOrGetFocus(
    Hy3Node& node,
    ShiftDirection direction,
    bool shift,
    bool once,
    bool visible
) {
	auto* break_origin = &node.getExpandActor();
	auto* break_parent = break_origin->parent;

	auto has_broken_once = false;

	// break parents until we hit a container oriented the same way as the shift
	// direction
	while (true) {
		if (break_parent == nullptr) return nullptr;

		auto& group = break_parent->data.as_group; // must be a group in order to be a parent

		if (shiftMatchesLayout(group.layout, direction)
		    && (!visible || group.layout != Hy3GroupLayout::Tabbed))
		{
			// group has the correct orientation

			if (once && shift && has_broken_once) break;
			if (break_origin != &node) has_broken_once = true;

			// if this movement would break out of the group, continue the break loop
			// (do not enter this if) otherwise break.
			if ((has_broken_once && once && shift)
			    || !(
			        (!shiftIsForward(direction) && group.children.front() == break_origin)
			        || (shiftIsForward(direction) && group.children.back() == break_origin)
			    ))
				break;
		}

		if (break_parent->parent == nullptr) {
			if (!shift) return nullptr;

			// if we haven't gone up any levels and the group is in the same direction
			// there's no reason to wrap the root group.
			if (group.layout != Hy3GroupLayout::Tabbed && shiftMatchesLayout(group.layout, direction))
				break;

			if (group.layout != Hy3GroupLayout::Tabbed && group.children.size() == 2
			    && std::find(group.children.begin(), group.children.end(), &node) != group.children.end())
			{
				group.setLayout(
				    shiftIsVertical(direction) ? Hy3GroupLayout::SplitV : Hy3GroupLayout::SplitH
				);
			} else {
				// wrap the root group in another group
				this->nodes.push_back({
				    .parent = break_parent,
				    .data = shiftIsVertical(direction) ? Hy3GroupLayout::SplitV : Hy3GroupLayout::SplitH,
				    .position = break_parent->position,
				    .size = break_parent->size,
				    .workspace_id = break_parent->workspace_id,
				    .layout = this,
				});

				auto* newChild = &this->nodes.back();
				Hy3Node::swapData(*break_parent, *newChild);
				break_parent->data.as_group.children.push_back(newChild);
				break_parent->data.as_group.group_focused = false;
				break_parent->data.as_group.focused_child = newChild;
				break_origin = newChild;
			}

			break;
		} else {
			break_origin = break_parent;
			break_parent = break_origin->parent;
		}
	}

	auto& parent_group = break_parent->data.as_group;
	Hy3Node* target_group = break_parent;
	std::list<Hy3Node*>::iterator insert;

	if (break_origin == parent_group.children.front() && !shiftIsForward(direction)) {
		if (!shift) return nullptr;
		insert = parent_group.children.begin();
	} else if (break_origin == parent_group.children.back() && shiftIsForward(direction)) {
		if (!shift) return nullptr;
		insert = parent_group.children.end();
	} else {
		auto& group_data = target_group->data.as_group;

		auto iter = std::find(group_data.children.begin(), group_data.children.end(), break_origin);
		if (shiftIsForward(direction)) iter = std::next(iter);
		else iter = std::prev(iter);

		if ((*iter)->data.type == Hy3NodeType::Window
		    || ((*iter)->data.type == Hy3NodeType::Group
		        && (*iter)->data.as_group.expand_focused != ExpandFocusType::NotExpanded)
		    || (shift && once && has_broken_once))
		{
			if (shift) {
				if (target_group == node.parent) {
					if (shiftIsForward(direction)) insert = std::next(iter);
					else insert = iter;
				} else {
					if (shiftIsForward(direction)) insert = iter;
					else insert = std::next(iter);
				}
			} else return (*iter)->getFocusedNode();
		} else {
			// break into neighboring groups until we hit a window
			while (true) {
				target_group = *iter;
				auto& group_data = target_group->data.as_group;

				if (group_data.children.empty()) return nullptr; // in theory this would never happen

				bool shift_after = false;

				if (!shift && group_data.layout == Hy3GroupLayout::Tabbed
				    && group_data.focused_child != nullptr)
				{
					iter = std::find(
					    group_data.children.begin(),
					    group_data.children.end(),
					    group_data.focused_child
					);
				} else if (visible && group_data.layout == Hy3GroupLayout::Tabbed && group_data.focused_child != nullptr)
				{
					// if the group is tabbed and we're going by visible nodes, jump to the current entry
					iter = std::find(
					    group_data.children.begin(),
					    group_data.children.end(),
					    group_data.focused_child
					);
					shift_after = true;
				} else if (shiftMatchesLayout(group_data.layout, direction) || (visible && group_data.layout == Hy3GroupLayout::Tabbed))
				{
					// if the group has the same orientation as movement pick the
					// last/first child based on movement direction
					if (shiftIsForward(direction)) iter = group_data.children.begin();
					else {
						iter = std::prev(group_data.children.end());
						shift_after = true;
					}
				} else {
					if (group_data.focused_child != nullptr) {
						iter = std::find(
						    group_data.children.begin(),
						    group_data.children.end(),
						    group_data.focused_child
						);
						shift_after = true;
					} else {
						iter = group_data.children.begin();
					}
				}

				if (shift && once) {
					if (shift_after) insert = std::next(iter);
					else insert = iter;
					break;
				}

				if ((*iter)->data.type == Hy3NodeType::Window
				    || ((*iter)->data.type == Hy3NodeType::Group
				        && (*iter)->data.as_group.expand_focused != ExpandFocusType::NotExpanded))
				{
					if (shift) {
						if (shift_after) insert = std::next(iter);
						else insert = iter;
						break;
					} else {
						return (*iter)->getFocusedNode();
					}
				}
			}
		}
	}

	auto& group_data = target_group->data.as_group;

	if (target_group == node.parent) {
		// nullptr is used as a signal value instead of removing it first to avoid
		// iterator invalidation.
		auto iter = std::find(group_data.children.begin(), group_data.children.end(), &node);
		*iter = nullptr;
		target_group->data.as_group.children.insert(insert, &node);
		target_group->data.as_group.children.remove(nullptr);
		target_group->recalcSizePosRecursive();
	} else {
		target_group->data.as_group.children.insert(insert, &node);

		// must happen AFTER `insert` is used
		auto* old_parent = node.removeFromParentRecursive(nullptr);
		node.parent = target_group;
		node.size_ratio = 1.0;

		if (old_parent != nullptr) {
			auto& group = old_parent->data.as_group;
			if (old_parent->parent != nullptr && group.ephemeral && group.children.size() == 1
			    && !group.hasChild(&node))
			{
				Hy3Node::swallowGroups(old_parent);
			}

			old_parent->updateTabBarRecursive();
			old_parent->recalcSizePosRecursive();
		}

		target_group->recalcSizePosRecursive();

		auto* target_parent = target_group->parent;
		while (target_parent != nullptr && Hy3Node::swallowGroups(target_parent)) {
			target_parent = target_parent->parent;
		}

		node.updateTabBarRecursive();
		node.focus();

		if (target_parent != target_group && target_parent != nullptr)
			target_parent->recalcSizePosRecursive();
	}

	return nullptr;
}

void Hy3Layout::updateAutotileWorkspaces() {
	static const auto* autotile_raw_workspaces =
	    &HyprlandAPI::getConfigValue(PHANDLE, "plugin:hy3:autotile:workspaces")->strValue;

	if (*autotile_raw_workspaces == this->autotile.raw_workspaces) {
		return;
	}

	this->autotile.raw_workspaces = *autotile_raw_workspaces;
	this->autotile.workspaces.clear();

	if (this->autotile.raw_workspaces == "all") {
		return;
	}

	this->autotile.workspace_blacklist = this->autotile.raw_workspaces.rfind("not:", 0) == 0;

	const auto autotile_raw_workspaces_filtered = (this->autotile.workspace_blacklist)
	                                                ? this->autotile.raw_workspaces.substr(4)
	                                                : this->autotile.raw_workspaces;

	// split on space and comma
	const std::regex regex {R"([\s,]+)"};
	const auto begin = std::sregex_token_iterator(
	    autotile_raw_workspaces_filtered.begin(),
	    autotile_raw_workspaces_filtered.end(),
	    regex,
	    -1
	);
	const auto end = std::sregex_token_iterator();

	for (auto s = begin; s != end; ++s) {
		try {
			this->autotile.workspaces.insert(std::stoi(*s));
		} catch (...) {
			hy3_log(ERR, "autotile:workspaces: invalid workspace id: {}", (std::string) *s);
		}
	}
}

bool Hy3Layout::shouldAutotileWorkspace(int workspace_id) {
	if (this->autotile.workspace_blacklist) {
		return !this->autotile.workspaces.contains(workspace_id);
	} else {
		return this->autotile.workspaces.empty() || this->autotile.workspaces.contains(workspace_id);
	}
}
