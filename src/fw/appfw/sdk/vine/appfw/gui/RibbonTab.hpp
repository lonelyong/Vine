#pragma once

#include "Control.hpp"
#include "Gui.hpp"
#include "RibbonPlacement.hpp"

VN_APPFWGUI_NS_BEGIN

class RibbonGroup;

/**
 * @brief Ribbon tab (wraps SARibbonCategory): a container for a set of panels
 * (RibbonGroup).
 *
 * @note This header includes and exposes no Qt types.
 */
class VN_APPFW_API RibbonTab : public Control {
    VN_OBJECT_META_DECL

  public:
    RibbonTab();
    virtual ~RibbonTab();

  public:
    void   setTitle(const String& t);
    String title() const;

    /**
     * @brief Sets the stable id used for lookups, duplicate checks and placement anchors.
     *
     * @details
     * - Set the id before adding the tab to a RibbonBar; the bar reads it at add time.
     * - Ids are ASCII and are not localized; an empty id means the tab cannot be anchored to
     *   and is excluded from duplicate checks.
     *
     * @param id Unique id within the ribbon bar, or empty for none.
     */
    void setId(const String& id);

    /**
     * @brief Returns the stable id of the tab.
     *
     * @return The id, or an empty string when none was set.
     */
    String id() const;

    /**
     * @brief Sets the semantic role of the tab (a placement/anchor vocabulary, not identity).
     *
     * @details
     * - RibbonTabRole::None means "no semantic role": the tab is reachable by id only.
     * - Custom host roles start at RibbonTabRole::UserRole.
     *
     * @param role Role to assign.
     */
    void setRole(RibbonTabRole role);

    /**
     * @brief Returns the semantic role of the tab.
     *
     * @return The role, or RibbonTabRole::None when none was set.
     */
    RibbonTabRole role() const;

    /**
     * @brief Sets the placement policy of the tab.
     *
     * @details
     * - RibbonTabPolicy::Terminal keeps the tab last; the standard catalog marks Help this way.
     * - Set the policy before adding the tab to a bar.
     *
     * @param policy Policy flags to assign.
     */
    void setPolicy(RibbonTabPolicy policy);

    /**
     * @brief Returns the placement policy of the tab.
     *
     * @return The policy flags.
     */
    RibbonTabPolicy policy() const;

  public:
    void addGroup(RibbonGroup* g);

    /**
     * @brief Adds a group where the placement asks.
     *
     * @details
     * - Group placements anchor by group id only; a role anchor is rejected with InvalidAnchor.
     * - A missing anchor id falls back to the end of the group list (PlacedByFallback).
     * - A duplicate non-empty group id is rejected (DuplicateId); adding the same object twice
     *   is a no-op (AlreadyAdded).
     *
     * @param g         Group to add; must not be null.
     * @param placement Where to place it relative to the groups of this tab.
     * @return What happened and the final logical group index (-1 when not added).
     */
    RibbonPlaceResult addGroup(RibbonGroup* g, const RibbonPlacement& placement);

    /**
     * @brief Finds a group by its stable id.
     *
     * @param id Group id; an empty id never matches.
     * @return The group, or nullptr when no group of this tab has that id.
     */
    RibbonGroup* findGroup(const String& id) const;

    void removeGroup(RibbonGroup* g);
    /// Number of groups.
    int numGroups() const;
    /// Gets the group by index (out of range returns nullptr).
    RibbonGroup* groupAt(int i) const;

  public:
    /// Sets the layout mode for all panels on this tab (three/two/single row).
    void                  setPanelLayoutMode(RibbonPanelLayoutMode m);
    RibbonPanelLayoutMode panelLayoutMode() const;
    /// Shows/hides the titles of all panels on this tab.
    void setPanelTitleVisible(bool on);
    bool panelTitleVisible() const;
    /// Button spacing for all panels on this tab.
    void setPanelSpacing(int n);
    int  panelSpacing() const;

  private:
    struct Impl;
    Impl*       dptr();
    const Impl* dptr() const;
};

VN_APPFWGUI_NS_END
