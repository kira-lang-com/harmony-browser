#ifndef HARMONY_NAVIGATION_MODEL_H
#define HARMONY_NAVIGATION_MODEL_H

#include <string>
#include <vector>

// The published navigation model, shared between the WebKit thread that writes
// it and the host's frame thread that reads it.
//
// Two translation units meet here: the one that binds WebKit and produces
// snapshots, and the one that stores them and answers the host. Nothing in
// this header touches WebKit, so a reader never needs the engine loaded to
// answer that it knows nothing yet.
namespace harmony_navigation {

struct HistoryEntry {
    std::string url;
    std::string title;
};

// One tab's navigation model as of the last WebKit callback that changed it.
struct PageSnapshot {
    int tabId { 0 };
    int slot { -1 };

    // What the address bar shows: the load in flight while there is one, and
    // the committed document otherwise.
    std::string url;
    std::string committedURL;
    std::string provisionalURL;
    std::string title;

    double progress { 0.0 };
    bool loading { false };
    bool canGoBack { false };
    bool canGoForward { false };

    int errorCode { 0 };
    std::string errorText;

    // Oldest first, current entry included.
    std::vector<HistoryEntry> history;
    int historyCurrent { -1 };
};

// Replaces what is published for `snapshot.tabId`, appending it when the tab
// is new. Called on the WebKit thread.
void publish(const PageSnapshot& snapshot);

// Updates only a tab's load progress. Progress moves many times per second
// while everything around it stands still, and re-reading a page's title, URLs
// and whole back/forward list to carry one number would put that cost on every
// step of every load.
void publishProgress(int tabId, double progress);

// Records which row of the host's tab strip a tab occupies. Kept apart from
// `publish` because the order of the strip changes without anything about the
// page changing.
void publishSlot(int tabId, int slot);

// Drops a tab from the published model.
void unpublish(int tabId);

// Records which tab the host is showing. Zero when none is.
void publishActiveTab(int tabId);

// Drops every tab.
void publishNothing();

// Records why the model is tracking nothing, for a host that has no page to
// show and needs to say why.
void setDiagnostic(const std::string& message);

} // namespace harmony_navigation

#endif
